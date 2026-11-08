#include "storage/Database.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QStringList>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include "core/Indicators.h"
#include "review/ReviewEngine.h"
#include <QJsonObject>
#include <QVariant>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <algorithm>

Database::Database(QObject* parent) : QObject(parent) {}

Database::~Database()
{
    close();
}

bool Database::exec(const QString& sql)
{
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool Database::open(const QString& dbPath)
{
    close();

    QStringList candidates;
    if (!dbPath.isEmpty()) {
        candidates << dbPath;
    } else {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        candidates << dir + QStringLiteral("/daban_quant.db");
        // 受限环境（WDAC/AppLocker 等）可能禁止向用户数据目录写库，回退到程序目录
        const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/daban_quant.db");
        if (!candidates.contains(local)) candidates << local;
    }

    m_autoBackupEnabled = dbPath.isEmpty();     // 显式指定路径（自检/冒烟）不做自动备份
    for (const QString& path : candidates) {
        if (tryOpen(path)) {
            m_lastError.clear();                // 回退成功后清掉失败候选留下的错误
            autoBackup();
            return true;
        }
    }
    return false;
}

bool Database::tryOpen(const QString& path)
{
    m_path = path;
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("daban_main"));
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        close();
        return false;
    }
    exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (!ensureSchema()) {
        close();
        return false;
    }
    return true;
}

void Database::close()
{
    const QString name = m_db.connectionName();
    if (m_db.isValid()) {
        if (m_db.isOpen()) m_db.close();
        m_db = QSqlDatabase();              // 先释放本对象引用，再移除命名连接
    }
    // 即便 open() 失败也要清理：否则回退到下一个候选路径时 addDatabase 会报重复连接
    if (!name.isEmpty()) QSqlDatabase::removeDatabase(name);
}

bool Database::isOpen() const
{
    return m_db.isValid() && m_db.isOpen();
}

bool Database::ensureSchema()
{
    // ---- 结构版本：老库升级不再靠 CREATE TABLE IF NOT EXISTS 碰运气 ----
    int version = 0;
    {
        QSqlQuery q(m_db);
        if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
            m_lastError = QStringLiteral("无法读取数据库版本：%1").arg(q.lastError().text());
            return false;
        }
        version = q.value(0).toInt();
    }
    if (version > kCurrentSchemaVersion) {
        m_lastError = QStringLiteral("数据库版本 %1 高于本程序支持的 %2，请升级客户端后再打开")
                          .arg(version).arg(kCurrentSchemaVersion);
        return false;
    }
    if (!migrateSchema(version)) return false;

    // ---- 完整性：损坏的库不应被静默继续使用 ----
    {
        QSqlQuery q(m_db);
        if (!q.exec(QStringLiteral("PRAGMA quick_check")) || !q.next()
            || q.value(0).toString().compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0) {
            m_lastError = QStringLiteral("SQLite 完整性检查失败：%1")
                              .arg(q.value(0).toString());
            return false;
        }
    }

    bool ok = true;
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS daily_kline ("
        " code TEXT NOT NULL, trade_date TEXT NOT NULL, o REAL, c REAL, h REAL, l REAL,"
        " vol REAL, amt REAL, PRIMARY KEY(code, trade_date))"));

    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS recommend_plan ("
        " trade_date TEXT PRIMARY KEY, updated_at TEXT, payload TEXT NOT NULL)"));

    // 交易日志：一次完整交易一行（买入 → 卖出），用于复盘与策略迭代
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS trade_journal ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " buy_date TEXT NOT NULL, code TEXT NOT NULL, name TEXT,"
        " buy_price REAL NOT NULL, qty INTEGER NOT NULL,"
        " sell_date TEXT, sell_price REAL,"
        " plan_buy REAL, plan_stop REAL, plan_take REAL,"
        " note TEXT, created_at TEXT)"));

    // 情绪序列：复合情绪指数按日留档 —— 趋势/加速度/退潮确认的跨日依据。
    // 只新增表，不改动既有 5 张表结构与任何读写接口。
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sentiment_daily ("
        " trade_date TEXT PRIMARY KEY, composite REAL, phase INTEGER, tier INTEGER,"
        " limit_up INTEGER, broken INTEGER, limit_down INTEGER, max_height INTEGER,"
        " advance_rate REAL, premium REAL, broken_rate REAL, cap REAL, note TEXT)"));

    // 键值配置：存放推送去重指纹之类的「跨会话小状态」
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS app_setting ("
        " key TEXT PRIMARY KEY, value TEXT, updated_at TEXT)"));

    // 复盘助手：日报 / 早盘预判 / 偏差评估 / 规则权重 / 有效策略 / 教训
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS review_daily ("
        " trade_date TEXT PRIMARY KEY, created_at TEXT, payload TEXT NOT NULL)"));
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS forecast_daily ("
        " for_date TEXT PRIMARY KEY, created_at TEXT, payload TEXT NOT NULL)"));
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS forecast_eval ("
        " for_date TEXT PRIMARY KEY, created_at TEXT, score INTEGER, hit INTEGER,"
        " payload TEXT NOT NULL)"));
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS assistant_rule ("
        " rule_id TEXT PRIMARY KEY, payload TEXT NOT NULL)"));
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS assistant_strategy ("
        " stance TEXT PRIMARY KEY, hits INTEGER DEFAULT 0, total INTEGER DEFAULT 0,"
        " last_hit TEXT)"));
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS assistant_lesson ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, trade_date TEXT, category TEXT,"
        " text TEXT, created_at TEXT)"));

    // 决策复盘：以「自己的决策」为对象的结构化报告（条件证据 / 事后偏差 / 跳过机会 / 隐性风险）
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS decision_review ("
        " trade_date TEXT PRIMARY KEY, created_at TEXT, payload TEXT NOT NULL)"));

    // 板块轮动：当日概念板块涨幅 + 主力资金流快照；失败原因也留档，界面明确标「数据缺失」
    ok &= exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sector_flow_daily ("
        " trade_date TEXT PRIMARY KEY, fetched_at TEXT, source TEXT, reason TEXT,"
        " payload TEXT NOT NULL)"));

    ok &= exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_journal_date ON trade_journal(buy_date)"));
    ok &= exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_kline_code_date ON daily_kline(code, trade_date)"));
    if (ok && version < kCurrentSchemaVersion)
        ok &= exec(QStringLiteral("PRAGMA user_version=%1").arg(kCurrentSchemaVersion));
    return ok;
}

bool Database::migrateSchema(int fromVersion)
{
    switch (fromVersion) {
    case 0:
    case 1:                                  // v1 → v2 只新增表，不改动任何既有表结构与读写口径
    case 2:
        return true;
    default:
        m_lastError = QStringLiteral("不支持的数据库版本：%1").arg(fromVersion);
        return false;
    }
}

int Database::schemaVersion() const
{
    if (!isOpen()) return 0;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) return 0;
    return q.value(0).toInt();
}

bool Database::backup(const QString& destPath)
{
    if (!isOpen() || destPath.isEmpty()) return false;
    QFile::remove(destPath);                    // VACUUM INTO 要求目标文件不存在
    QString p = destPath;
    p.replace(QLatin1Char('\''), QStringLiteral("''"));
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("VACUUM INTO '%1'").arg(p))) {
        m_lastError = q.lastError().text();
        QFile::remove(destPath);                 // 清理失败残留，避免下次被误判成「今天已备份」
        return false;
    }
    return true;
}

void Database::autoBackup()
{
    if (!m_autoBackupEnabled || !isOpen()) return;
    const QString name = QStringLiteral("daban_quant_%1.db")
                             .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
    const QString preferred = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                            + QStringLiteral("/backups/") + name;
    const QString fallback  = QFileInfo(m_path).absolutePath()
                            + QStringLiteral("/backups/") + name;

    QString dest = preferred;
    QDir().mkpath(QFileInfo(preferred).absolutePath());
    const auto hasValidBackup = [](const QString& p) {
        const QFileInfo fi(p);
        return fi.exists() && fi.size() > 0;
    };
    if (hasValidBackup(dest)) return;           // 每天最多一份（空文件不算）

    if (!backup(dest)) {
        // 受限环境可能禁止在用户目录新建 .db：退回数据库所在目录，确保备份仍然发生
        QDir().mkpath(QFileInfo(fallback).absolutePath());
        if (hasValidBackup(fallback)) return;
        dest = fallback;
        if (!backup(dest)) {
            qWarning().noquote() << QStringLiteral("[做多量化] 自动备份失败：%1").arg(m_lastError);
            return;
        }
    }
    m_lastError.clear();
    qInfo().noquote() << QStringLiteral("[做多量化] 已自动备份：%1").arg(dest);

    const QString dir = QFileInfo(dest).absolutePath();
    const QStringList files = QDir(dir).entryList({ QStringLiteral("daban_quant_*.db") },
                                                  QDir::Files, QDir::Name);
    for (int i = 0; i < files.size() - 7; ++i)
        QFile::remove(dir + QLatin1Char('/') + files.at(i));
}

// ---------------------------------------------------------------- 荐股计划
namespace {

// 「查看得分由来」明细：断网看缓存时同样要能解释分数怎么来的
QJsonObject breakdownToJson(const ScoreBreakdown& sb)
{
    QJsonObject o;
    o["title"] = sb.title;
    o["value"] = sb.value;
    o["formula"] = sb.formula;
    o["source"] = sb.source;
    if (!sb.notes.isEmpty()) o["notes"] = QJsonArray::fromStringList(sb.notes);
    if (!sb.items.isEmpty()) {
        QJsonArray items;
        for (const ScoreItem& it : sb.items) {
            QJsonObject io;
            io["name"] = it.name;
            io["value"] = it.value;
            io["weight"] = it.weight;
            io["contribution"] = it.contribution;
            io["available"] = it.available;
            io["detail"] = it.detail;
            items.append(io);
        }
        o["items"] = items;
    }
    return o;
}

ScoreBreakdown breakdownFromJson(const QJsonObject& o)
{
    ScoreBreakdown sb;
    sb.title = o.value("title").toString();
    sb.value = o.value("value").toDouble();
    sb.formula = o.value("formula").toString();
    sb.source = o.value("source").toString();
    for (const QJsonValue& v : o.value("notes").toArray()) sb.notes << v.toString();
    for (const QJsonValue& v : o.value("items").toArray()) {
        const QJsonObject io = v.toObject();
        ScoreItem it;
        it.name = io.value("name").toString();
        it.value = io.value("value").toDouble();
        it.weight = io.value("weight").toDouble();
        it.contribution = io.value("contribution").toDouble();
        it.available = io.value("available").toBool(true);
        it.detail = io.value("detail").toString();
        sb.items.append(it);
    }
    return sb;
}

QJsonObject planToJson(const TradePlan& p)
{
    QJsonObject o;
    o["code"] = p.code;
    o["name"] = p.name;
    o["currentPrice"] = p.currentPrice;             // 卡片「当前价格」：缓存回看也要有
    o["tag"] = p.tag;
    o["sector"] = p.sector;
    o["theme"] = p.theme;
    o["themeRank"] = p.themeRank;
    o["themeLeader"] = p.themeLeader;
    o["buyLow"] = p.buyLow;
    o["buyHigh"] = p.buyHigh;
    o["takeProfitLow"] = p.takeProfitLow;
    o["takeProfitHigh"] = p.takeProfitHigh;
    o["stopLossLow"] = p.stopLossLow;
    o["stopLossHigh"] = p.stopLossHigh;
    o["positionPct"] = p.positionPct;
    o["riskReward"] = p.riskReward;
    o["riskRewardMax"] = p.riskRewardMax;
    o["riskPct"] = p.riskPct;
    o["riskTooHigh"] = p.riskTooHigh;
    o["score"] = p.score;
    o["grade"] = p.grade;
    o["actionable"] = p.actionable;
    o["note"] = p.note;
    o["buyBasis"] = p.buyBasis;
    o["tpBasis"] = p.tpBasis;
    o["slBasis"] = p.slBasis;
    o["riskNote"] = p.riskNote;
    o["signalLevel"] = p.signalLevel;
    o["riskBudgetPct"] = p.riskBudgetPct;
    o["sentimentNote"] = p.sentimentNote;
    o["fillProbability"] = p.fillProbability;
    o["fillNote"] = p.fillNote;
    // 风险标记：业绩亏损 / 高位监管 / 微盘 / 次新 ……
    // 之前没有落库，「断网看缓存」时这一层安全信息整体消失。
    o["riskFlags"] = QJsonArray::fromStringList(p.riskFlags);
    // 筹码分布与量能结论：不落库则缓存模式下「定价第二套锚点」整体消失
    o["chipSupportLow"] = p.chipSupportLow;
    o["chipSupportHigh"] = p.chipSupportHigh;
    o["chipPressureLow"] = p.chipPressureLow;
    o["chipPressureHigh"] = p.chipPressureHigh;
    o["chipProfitRatio"] = p.chipProfitRatio;
    o["volRatio"] = p.volRatio;
    o["supportQuality"] = p.supportQuality;
    o["divergenceNote"] = p.divergenceNote;
    // 次日竞价方案：不落库则缓存模式下「接 0.00~0.00」，等于没有。
    o["auctionEvaluated"] = p.auctionEvaluated;
    o["auctionVerdict"] = p.auctionVerdict;
    o["auctionOpenPct"] = p.auctionOpenPct;
    o["auctionAmountRatio"] = p.auctionAmountRatio;
    o["auctionBuyLow"] = p.auctionBuyLow;
    o["auctionBuyHigh"] = p.auctionBuyHigh;
    o["auctionAbandonPrice"] = p.auctionAbandonPrice;
    o["auctionBasis"] = p.auctionBasis;
    o["auctionNote"] = p.auctionNote;
    // 多因子评分结果：综合分 / 排名 / 维度贡献度（断网看缓存时同样可解释）
    QJsonObject factor;
    factor["compositeScore"] = p.compositeScore;
    factor["rank"] = p.factorRank;
    factor["reject"] = p.factorReject;
    factor["summary"] = p.factorSummary;
    QJsonArray carr;
    for (const FactorContribution& fc : p.contributions) {
        QJsonObject co;
        co["dim"] = fc.dimension;
        co["name"] = fc.name;
        co["norm"] = fc.normalized;
        co["weight"] = fc.weight;
        co["contribution"] = fc.contribution;
        co["share"] = fc.share;
        co["coverage"] = fc.coverage;
        co["available"] = fc.available;
        co["detail"] = fc.detail;
        carr.append(co);
    }
    factor["contributions"] = carr;
    o["factor"] = factor;
    // 得分由来：打板 8 因子与综合维度明细，缓存模式下也能点开对照
    if (p.dabanBreakdown.valid())
        o["dabanBreakdown"] = breakdownToJson(p.dabanBreakdown);
    if (p.compositeBreakdown.valid())
        o["compositeBreakdown"] = breakdownToJson(p.compositeBreakdown);
    return o;
}

TradePlan planFromJson(const QJsonObject& o)
{
    TradePlan p;
    p.code = o.value("code").toString();
    p.name = o.value("name").toString();
    // 兼容旧缓存：没有 currentPrice 字段时为 0，卡片显示「—」
    p.currentPrice = o.contains("currentPrice") ? o.value("currentPrice").toDouble() : 0.0;
    p.tag = o.value("tag").toString();
    p.sector = o.value("sector").toString();
    p.theme = o.value("theme").toString();
    p.themeRank = o.value("themeRank").toInt();
    p.themeLeader = o.value("themeLeader").toBool();
    p.buyLow = o.value("buyLow").toDouble();
    p.buyHigh = o.value("buyHigh").toDouble();
    p.takeProfitLow = o.value("takeProfitLow").toDouble();
    p.takeProfitHigh = o.value("takeProfitHigh").toDouble();
    p.stopLossLow = o.value("stopLossLow").toDouble();
    p.stopLossHigh = o.value("stopLossHigh").toDouble();
    p.positionPct = o.value("positionPct").toDouble();
    p.riskReward = o.value("riskReward").toDouble();
    p.riskRewardMax = o.value("riskRewardMax").toDouble();
    p.riskPct = o.value("riskPct").toDouble();
    p.riskTooHigh = o.value("riskTooHigh").toBool();
    p.score = o.value("score").toDouble();
    p.grade = o.value("grade").toString();
    p.actionable = o.value("actionable").toBool(true);
    p.note = o.value("note").toString();
    p.buyBasis = o.value("buyBasis").toString();
    p.tpBasis = o.value("tpBasis").toString();
    p.slBasis = o.value("slBasis").toString();
    p.riskNote = o.value("riskNote").toString();
    p.signalLevel = o.value("signalLevel").toInt();
    p.riskBudgetPct = o.value("riskBudgetPct").toDouble();
    p.sentimentNote = o.value("sentimentNote").toString();
    p.fillProbability = o.contains("fillProbability") ? o.value("fillProbability").toDouble() : 1.0;
    p.fillNote = o.value("fillNote").toString();
    // 兼容旧记录：没有 riskFlags 字段时留空，不报错
    if (o.contains("riskFlags")) {
        for (const QJsonValue& v : o.value("riskFlags").toArray()) {
            p.riskFlags << v.toString();
        }
    }
    // 筹码分布与量能结论（兼容旧记录：缺失即为空 / 0）
    p.chipSupportLow  = o.value("chipSupportLow").toDouble();
    p.chipSupportHigh = o.value("chipSupportHigh").toDouble();
    p.chipPressureLow  = o.value("chipPressureLow").toDouble();
    p.chipPressureHigh = o.value("chipPressureHigh").toDouble();
    p.chipProfitRatio = o.value("chipProfitRatio").toDouble();
    p.volRatio        = o.value("volRatio").toDouble();
    p.supportQuality  = o.value("supportQuality").toString();
    p.divergenceNote  = o.value("divergenceNote").toString();
    p.auctionEvaluated = o.value("auctionEvaluated").toBool();
    p.auctionVerdict = o.value("auctionVerdict").toInt();
    p.auctionOpenPct = o.value("auctionOpenPct").toDouble();
    p.auctionAmountRatio = o.value("auctionAmountRatio").toDouble();
    p.auctionBuyLow = o.value("auctionBuyLow").toDouble();
    p.auctionBuyHigh = o.value("auctionBuyHigh").toDouble();
    p.auctionAbandonPrice = o.value("auctionAbandonPrice").toDouble();
    p.auctionBasis = o.value("auctionBasis").toString();
    p.auctionNote = o.value("auctionNote").toString();
    // 兼容旧记录：没有 factor 字段时综合分维持 0，不影响原有展示
    if (o.contains("factor")) {
        const QJsonObject f = o.value("factor").toObject();
        p.compositeScore = f.value("compositeScore").toDouble();
        p.factorRank = f.value("rank").toInt();
        p.factorReject = f.value("reject").toString();
        p.factorSummary = f.value("summary").toString();
        for (const QJsonValue& v : f.value("contributions").toArray()) {
            const QJsonObject co = v.toObject();
            FactorContribution fc;
            fc.dimension = co.value("dim").toInt();
            fc.name = co.value("name").toString();
            fc.normalized = co.value("norm").toDouble();
            fc.weight = co.value("weight").toDouble();
            fc.contribution = co.value("contribution").toDouble();
            fc.share = co.value("share").toDouble();
            fc.coverage = co.value("coverage").toDouble();
            fc.available = co.value("available").toBool();
            fc.detail = co.value("detail").toString();
            p.contributions.append(fc);
        }
    }
    // 兼容旧记录：没有得分由来字段时留空，弹窗给出「重新计算后可见」提示
    if (o.contains("dabanBreakdown"))
        p.dabanBreakdown = breakdownFromJson(o.value("dabanBreakdown").toObject());
    if (o.contains("compositeBreakdown"))
        p.compositeBreakdown = breakdownFromJson(o.value("compositeBreakdown").toObject());
    return p;
}

QJsonObject regimeToJson(const MarketRegime& r)
{
    QJsonObject o;
    o["date"] = r.date.toString(Qt::ISODate);
    o["limitUpCount"] = r.limitUpCount;
    o["brokenCount"] = r.brokenCount;
    o["limitDownCount"] = r.limitDownCount;
    o["firstBoardCount"] = r.firstBoardCount;
    o["multiBoardCount"] = r.multiBoardCount;
    o["maxBoardHeight"] = r.maxBoardHeight;
    o["brokenRate"] = r.brokenRate;
    o["advanceRate"] = r.advanceRate;
    o["prevLimitUpAvgPct"] = r.prevLimitUpAvgPct;
    o["phase"] = int(r.phase);
    o["tradeAllowed"] = r.tradeAllowed;
    o["note"] = r.note;
    return o;
}

MarketRegime regimeFromJson(const QJsonObject& o)
{
    MarketRegime r;
    r.date = QDate::fromString(o.value("date").toString(), Qt::ISODate);
    r.limitUpCount = o.value("limitUpCount").toInt();
    r.brokenCount = o.value("brokenCount").toInt();
    r.limitDownCount = o.value("limitDownCount").toInt();
    r.firstBoardCount = o.value("firstBoardCount").toInt();
    r.multiBoardCount = o.value("multiBoardCount").toInt();
    r.maxBoardHeight = o.value("maxBoardHeight").toInt();
    r.brokenRate = o.value("brokenRate").toDouble();
    r.advanceRate = o.value("advanceRate").toDouble();
    r.prevLimitUpAvgPct = o.value("prevLimitUpAvgPct").toDouble();
    r.phase = MarketPhase(o.value("phase").toInt());
    r.tradeAllowed = o.value("tradeAllowed").toBool();
    r.note = o.value("note").toString();
    return r;
}

} // namespace

bool Database::saveRecommend(const RecommendBundle& bundle)
{
    if (!isOpen()) return false;
    if (!bundle.tradeDate.isValid()) return false;

    QJsonObject root;
    root["updatedAt"] = bundle.updatedAt;
    root["notice"] = bundle.notice;
    root["positionCapPct"] = bundle.positionCapPct;
    root["factorDigest"] = bundle.factorDigest;
    root["factorRule"] = bundle.factorRule;
    root["regime"] = regimeToJson(bundle.regime);
    QJsonArray arr;
    for (const TradePlan& p : bundle.plans) arr.append(planToJson(p));
    root["plans"] = arr;
    // 候选池备选：断网回看时决策台的候选池仍保持「数量优先」的池容量
    if (!bundle.backups.isEmpty()) {
        QJsonArray barr;
        for (const TradePlan& p : bundle.backups) barr.append(planToJson(p));
        root["backups"] = barr;
    }
    // 筛选漏斗：断网看缓存时，「涨停 47 只去哪了」仍要有答案
    if (!bundle.funnel.isEmpty()) {
        QJsonArray farr;
        for (const FunnelStage& s : bundle.funnel) {
            QJsonObject fo;
            fo["label"] = s.label;
            fo["count"] = s.count;
            fo["isOutput"] = s.isOutput;
            farr.append(fo);
        }
        root["funnel"] = farr;
    }
    if (!bundle.rejects.isEmpty()) {
        QJsonArray rarr;
        for (const RejectRow& r : bundle.rejects) {
            QJsonObject ro;
            ro["code"] = r.code;
            ro["name"] = r.name;
            ro["stage"] = r.stage;
            ro["reason"] = r.reason;
            rarr.append(ro);
        }
        root["rejects"] = rarr;
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO recommend_plan(trade_date, updated_at, payload) VALUES(?,?,?)"
        " ON CONFLICT(trade_date) DO UPDATE SET updated_at=excluded.updated_at,"
        " payload=excluded.payload"));
    q.addBindValue(bundle.tradeDate.toString(Qt::ISODate));
    q.addBindValue(bundle.updatedAt);
    q.addBindValue(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    return q.exec();
}

RecommendBundle Database::loadRecommend(const QDate& date) const
{
    RecommendBundle b;
    if (!isOpen() || !date.isValid()) return b;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT payload FROM recommend_plan WHERE trade_date=?"));
    q.addBindValue(date.toString(Qt::ISODate));
    if (!q.exec() || !q.next()) return b;

    const QJsonDocument doc = QJsonDocument::fromJson(q.value(0).toString().toUtf8());
    if (!doc.isObject()) return b;
    const QJsonObject o = doc.object();

    b.updatedAt = o.value("updatedAt").toString();
    b.notice = o.value("notice").toString();
    b.positionCapPct = o.contains("positionCapPct") ? o.value("positionCapPct").toDouble() : -1.0;
    b.factorDigest = o.value("factorDigest").toString();
    b.factorRule = o.value("factorRule").toString();
    b.regime = regimeFromJson(o.value("regime").toObject());
    b.tradeDate = date;
    for (const QJsonValue& v : o.value("plans").toArray())
        b.plans.append(planFromJson(v.toObject()));
    for (const QJsonValue& v : o.value("backups").toArray())
        b.backups.append(planFromJson(v.toObject()));
    // 筛选漏斗：旧记录没有这两个字段时留空（首页据此隐藏漏斗卡，不报错）
    for (const QJsonValue& v : o.value("funnel").toArray()) {
        const QJsonObject fo = v.toObject();
        FunnelStage s;
        s.label = fo.value("label").toString();
        s.count = fo.value("count").toInt();
        s.isOutput = fo.value("isOutput").toBool();
        b.funnel.append(s);
    }
    for (const QJsonValue& v : o.value("rejects").toArray()) {
        const QJsonObject ro = v.toObject();
        RejectRow r;
        r.code = ro.value("code").toString();
        r.name = ro.value("name").toString();
        r.stage = ro.value("stage").toString();
        r.reason = ro.value("reason").toString();
        b.rejects.append(r);
    }
    return b;
}

QVector<QDate> Database::recommendDates() const
{
    QVector<QDate> out;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT trade_date FROM recommend_plan ORDER BY trade_date DESC")))
        while (q.next()) out.append(QDate::fromString(q.value(0).toString(), Qt::ISODate));
    return out;
}

QDate Database::latestRecommendDate() const
{
    const QVector<QDate> d = recommendDates();
    return d.isEmpty() ? QDate() : d.first();
}

// ---------------------------------------------------------------- 情绪序列
bool Database::saveSentiment(const SentimentState& state)
{
    if (!isOpen() || !state.valid()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO sentiment_daily(trade_date, composite, phase, tier, limit_up, broken,"
        " limit_down, max_height, advance_rate, premium, broken_rate, cap, note)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(trade_date) DO UPDATE SET composite=excluded.composite,"
        " phase=excluded.phase, tier=excluded.tier, limit_up=excluded.limit_up,"
        " broken=excluded.broken, limit_down=excluded.limit_down,"
        " max_height=excluded.max_height, advance_rate=excluded.advance_rate,"
        " premium=excluded.premium, broken_rate=excluded.broken_rate,"
        " cap=excluded.cap, note=excluded.note"));
    q.addBindValue(state.date.toString(Qt::ISODate));
    q.addBindValue(state.composite);
    q.addBindValue(static_cast<int>(state.phase));
    q.addBindValue(static_cast<int>(state.tier));
    q.addBindValue(state.fact.limitUpCount);
    q.addBindValue(state.fact.brokenCount);
    q.addBindValue(state.fact.limitDownCount);
    q.addBindValue(state.fact.maxBoardHeight);
    q.addBindValue(state.fact.advanceRate);
    q.addBindValue(state.fact.premiumPct);
    q.addBindValue(state.fact.brokenRate);
    q.addBindValue(state.positionCapPct);
    q.addBindValue(state.note);
    return q.exec();
}

QVector<QPair<QDate, double>> Database::sentimentHistory(const QDate& until, int days) const
{
    QVector<QPair<QDate, double>> out;
    if (!isOpen()) return out;
    const int limit = qBound(1, days, 250);

    QSqlQuery q(m_db);
    if (until.isValid()) {
        q.prepare(QStringLiteral(
            "SELECT trade_date, composite FROM sentiment_daily"
            " WHERE trade_date <= ? ORDER BY trade_date DESC LIMIT ?"));
        q.addBindValue(until.toString(Qt::ISODate));
    } else {
        q.prepare(QStringLiteral(
            "SELECT trade_date, composite FROM sentiment_daily"
            " ORDER BY trade_date DESC LIMIT ?"));
    }
    q.addBindValue(limit);
    if (!q.exec()) return out;

    // 倒序取出 → 反转成升序，便于计算趋势（最新一天在末尾）
    while (q.next()) {
        const QDate d = QDate::fromString(q.value(0).toString(), Qt::ISODate);
        if (d.isValid()) out.append(qMakePair(d, q.value(1).toDouble()));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

SentimentState Database::loadSentiment(const QDate& date) const
{
    SentimentState s;
    if (!isOpen() || !date.isValid()) return s;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT composite, phase, tier, limit_up, broken, limit_down, max_height,"
        " advance_rate, premium, broken_rate, cap, note"
        " FROM sentiment_daily WHERE trade_date = ?"));
    q.addBindValue(date.toString(Qt::ISODate));
    if (!q.exec() || !q.next()) return s;

    s.date = date;
    s.composite = q.value(0).toDouble();
    s.phase = static_cast<MarketPhase>(q.value(1).toInt());
    s.tier = static_cast<SentimentTier>(q.value(2).toInt());
    s.fact.date = date;
    s.fact.limitUpCount = q.value(3).toInt();
    s.fact.brokenCount = q.value(4).toInt();
    s.fact.limitDownCount = q.value(5).toInt();
    s.fact.maxBoardHeight = q.value(6).toInt();
    s.fact.advanceRate = q.value(7).toDouble();
    s.fact.premiumPct = q.value(8).toDouble();
    s.fact.brokenRate = q.value(9).toDouble();
    s.positionCapPct = q.value(10).toDouble();
    s.note = q.value(11).toString();
    s.tradeAllowed = s.tier != SentimentTier::NoTrade;
    s.regime.date = date;
    s.regime.phase = s.phase;
    s.regime.tradeAllowed = s.tradeAllowed;
    s.regime.note = s.note;
    return s;
}

// ---------------------------------------------------------------- K线
bool Database::saveKline(const KlineSeries& k)
{
    if (!isOpen() || !k.valid()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO daily_kline(code, trade_date, o, c, h, l, vol, amt) VALUES(?,?,?,?,?,?,?,?)"
        " ON CONFLICT(code, trade_date) DO UPDATE SET o=excluded.o, c=excluded.c,"
        " h=excluded.h, l=excluded.l, vol=excluded.vol, amt=excluded.amt"));
    for (const Candle& c : k.candles) {
        q.addBindValue(k.code);
        q.addBindValue(c.date.toString(Qt::ISODate));
        q.addBindValue(c.open);
        q.addBindValue(c.close);
        q.addBindValue(c.high);
        q.addBindValue(c.low);
        q.addBindValue(c.volume);
        q.addBindValue(c.amount);
    }
    return q.exec();
}

KlineSeries Database::loadKline(const QString& code) const
{
    KlineSeries out;
    out.code = code;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT trade_date, o, c, h, l, vol, amt FROM daily_kline WHERE code=? ORDER BY trade_date"));
    q.addBindValue(code);
    if (!q.exec()) return out;
    while (q.next()) {
        Candle c;
        c.date = QDate::fromString(q.value(0).toString(), Qt::ISODate);
        c.open = q.value(1).toDouble();
        c.close = q.value(2).toDouble();
        c.high = q.value(3).toDouble();
        c.low = q.value(4).toDouble();
        c.volume = q.value(5).toDouble();
        c.amount = q.value(6).toDouble();
        if (c.date.isValid()) out.candles.append(c);
    }
    return out;
}

// ---------------------------------------------------------------- 交易日志
bool Database::addTrade(TradeRecord& rec)
{
    if (!isOpen()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO trade_journal(buy_date, code, name, buy_price, qty, sell_date, sell_price,"
        " plan_buy, plan_stop, plan_take, note, created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(rec.buyDate.toString(Qt::ISODate));
    q.addBindValue(rec.code);
    q.addBindValue(rec.name);
    q.addBindValue(rec.buyPrice);
    q.addBindValue(rec.qty);
    q.addBindValue(rec.sellDate.isValid() ? rec.sellDate.toString(Qt::ISODate) : QVariant());
    q.addBindValue(rec.sellPrice > 0.0 ? rec.sellPrice : QVariant());
    q.addBindValue(rec.planBuy);
    q.addBindValue(rec.planStop);
    q.addBindValue(rec.planTake);
    q.addBindValue(rec.note);
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!q.exec()) return false;
    rec.id = q.lastInsertId().toInt();
    return true;
}

bool Database::updateTrade(const TradeRecord& rec)
{
    if (!isOpen() || rec.id <= 0) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE trade_journal SET buy_date=?, code=?, name=?, buy_price=?, qty=?,"
        " sell_date=?, sell_price=?, plan_buy=?, plan_stop=?, plan_take=?, note=?"
        " WHERE id=?"));
    q.addBindValue(rec.buyDate.toString(Qt::ISODate));
    q.addBindValue(rec.code);
    q.addBindValue(rec.name);
    q.addBindValue(rec.buyPrice);
    q.addBindValue(rec.qty);
    q.addBindValue(rec.sellDate.isValid() ? rec.sellDate.toString(Qt::ISODate) : QVariant());
    q.addBindValue(rec.sellPrice > 0.0 ? rec.sellPrice : QVariant());
    q.addBindValue(rec.planBuy);
    q.addBindValue(rec.planStop);
    q.addBindValue(rec.planTake);
    q.addBindValue(rec.note);
    q.addBindValue(rec.id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool Database::deleteTrade(int id)
{
    if (!isOpen() || id <= 0) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM trade_journal WHERE id=?"));
    q.addBindValue(id);
    return q.exec();
}

QString Database::describe() const
{
    return QStringLiteral("本地 SQLite（%1）").arg(m_path);
}

QVector<TradeRecord> Database::trades() const
{
    QVector<TradeRecord> out;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT id, buy_date, code, name, buy_price, qty, sell_date, sell_price,"
            " plan_buy, plan_stop, plan_take, note FROM trade_journal ORDER BY buy_date DESC, id DESC")))
        return out;

    while (q.next()) {
        TradeRecord r;
        r.id        = q.value(0).toInt();
        r.buyDate   = QDate::fromString(q.value(1).toString(), Qt::ISODate);
        r.code      = q.value(2).toString();
        r.name      = q.value(3).toString();
        r.buyPrice  = q.value(4).toDouble();
        r.qty       = q.value(5).toInt();
        r.sellDate  = QDate::fromString(q.value(6).toString(), Qt::ISODate);
        r.sellPrice = q.value(7).toDouble();
        r.planBuy   = q.value(8).toDouble();
        r.planStop  = q.value(9).toDouble();
        r.planTake  = q.value(10).toDouble();
        r.note      = q.value(11).toString();
        out.append(r);
    }
    return out;
}

// ---------------------------------------------------------------- 键值配置
bool Database::setSetting(const QString& key, const QString& value)
{
    if (!isOpen() || key.isEmpty()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO app_setting(key, value, updated_at) VALUES(?,?,?)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    return q.exec();
}

QString Database::getSetting(const QString& key, const QString& def) const
{
    if (!isOpen() || key.isEmpty()) return def;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM app_setting WHERE key=?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next()) return def;
    const QString v = q.value(0).toString();
    return v.isEmpty() ? def : v;
}

// ---------------------------------------------------------------- 复盘助手
namespace {

QString payloadOf(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace

bool Database::saveReview(const ReviewReport& r)
{
    if (!isOpen() || !r.date.isValid()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO review_daily(trade_date, created_at, payload) VALUES(?,?,?)"
        " ON CONFLICT(trade_date) DO UPDATE SET created_at=excluded.created_at,"
        " payload=excluded.payload"));
    q.addBindValue(r.date.toString(Qt::ISODate));
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(payloadOf(ReviewEngine::toJson(r)));
    return q.exec();
}

ReviewReport Database::loadReview(const QDate& date) const
{
    ReviewReport out;
    if (!isOpen() || !date.isValid()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT payload FROM review_daily WHERE trade_date=?"));
    q.addBindValue(date.toString(Qt::ISODate));
    if (!q.exec() || !q.next()) return out;
    return ReviewEngine::reviewFromJson(
        QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object());
}

QDate Database::latestReviewDate() const
{
    if (!isOpen()) return QDate();
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT trade_date FROM review_daily ORDER BY trade_date DESC LIMIT 1")))
        return QDate();
    if (!q.next()) return QDate();
    return QDate::fromString(q.value(0).toString(), Qt::ISODate);
}

bool Database::saveForecast(const Forecast& f)
{
    if (!isOpen() || !f.forDate.isValid()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO forecast_daily(for_date, created_at, payload) VALUES(?,?,?)"
        " ON CONFLICT(for_date) DO UPDATE SET created_at=excluded.created_at,"
        " payload=excluded.payload"));
    q.addBindValue(f.forDate.toString(Qt::ISODate));
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(payloadOf(ReviewEngine::toJson(f)));
    return q.exec();
}

Forecast Database::loadForecast(const QDate& forDate) const
{
    Forecast out;
    if (!isOpen() || !forDate.isValid()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT payload FROM forecast_daily WHERE for_date=?"));
    q.addBindValue(forDate.toString(Qt::ISODate));
    if (!q.exec() || !q.next()) return out;
    return ReviewEngine::forecastFromJson(
        QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object());
}

bool Database::saveForecastEval(const ForecastEval& e)
{
    if (!isOpen() || !e.forDate.isValid()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO forecast_eval(for_date, created_at, score, hit, payload) VALUES(?,?,?,?,?)"
        " ON CONFLICT(for_date) DO UPDATE SET created_at=excluded.created_at,"
        " score=excluded.score, hit=excluded.hit, payload=excluded.payload"));
    q.addBindValue(e.forDate.toString(Qt::ISODate));
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(e.score);
    q.addBindValue(e.hit ? 1 : 0);
    q.addBindValue(payloadOf(ReviewEngine::toJson(e)));
    return q.exec();
}

QVector<ForecastEval> Database::recentEvals(int limit) const
{
    QVector<ForecastEval> out;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT payload FROM forecast_eval ORDER BY for_date DESC LIMIT ?"));
    q.addBindValue(qMax(1, limit));
    if (!q.exec()) return out;
    while (q.next()) {
        out.append(ReviewEngine::evalFromJson(
            QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object()));
    }
    return out;
}

bool Database::saveRules(const QVector<AssistantRule>& rules)
{
    if (!isOpen()) return false;
    bool ok = true;
    for (const AssistantRule& r : rules) {
        QJsonObject o;
        o["id"] = r.id;
        o["name"] = r.name;
        o["condition"] = r.condition;
        o["hits"] = r.hits;
        o["total"] = r.total;
        o["weight"] = r.weight;
        o["active"] = r.active;
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "INSERT INTO assistant_rule(rule_id, payload) VALUES(?,?)"
            " ON CONFLICT(rule_id) DO UPDATE SET payload=excluded.payload"));
        q.addBindValue(r.id);
        q.addBindValue(payloadOf(o));
        ok = q.exec() && ok;
    }
    return ok;
}

QVector<AssistantRule> Database::loadRules() const
{
    QVector<AssistantRule> out;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT payload FROM assistant_rule"))) return out;
    while (q.next()) {
        const QJsonObject o = QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object();
        AssistantRule r;
        r.id = o.value("id").toString();
        r.name = o.value("name").toString();
        r.condition = o.value("condition").toString();
        r.hits = o.value("hits").toInt();
        r.total = o.value("total").toInt();
        r.weight = o.value("weight").toDouble(1.0);
        r.active = o.value("active").toBool(true);
        if (!r.id.isEmpty()) out.append(r);
    }
    return out;
}

bool Database::addLesson(const QDate& date, const QString& category, const QString& text)
{
    if (!isOpen()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO assistant_lesson(trade_date, category, text, created_at) VALUES(?,?,?,?)"));
    q.addBindValue(date.toString(Qt::ISODate));
    q.addBindValue(category);
    q.addBindValue(text);
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    return q.exec();
}

QStringList Database::recentLessons(int limit) const
{
    QStringList out;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT trade_date, category, text FROM assistant_lesson ORDER BY id DESC LIMIT ?"));
    q.addBindValue(qMax(1, limit));
    if (!q.exec()) return out;
    while (q.next()) {
        out << QStringLiteral("%1 · %2 · %3")
                   .arg(q.value(0).toString(), q.value(1).toString(), q.value(2).toString());
    }
    return out;
}

bool Database::touchStrategy(const QString& stance, bool hit)
{
    if (!isOpen() || stance.isEmpty()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO assistant_strategy(stance, hits, total, last_hit) VALUES(?,?,?,?)"
        " ON CONFLICT(stance) DO UPDATE SET hits=hits+excluded.hits, total=total+1,"
        " last_hit=CASE WHEN excluded.hits>0 THEN excluded.last_hit ELSE last_hit END"));
    q.addBindValue(stance);
    q.addBindValue(hit ? 1 : 0);
    q.addBindValue(1);
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    return q.exec();
}

QVector<QPair<QString, QPair<int, int>>> Database::strategies() const
{
    QVector<QPair<QString, QPair<int, int>>> out;
    if (!isOpen()) return out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT stance, hits, total FROM assistant_strategy ORDER BY hits DESC, total DESC")))
        return out;
    while (q.next()) {
        out.append(qMakePair(q.value(0).toString(),
                             qMakePair(q.value(1).toInt(), q.value(2).toInt())));
    }
    return out;
}

// ---------------------------------------------------------------- 决策复盘
bool Database::saveDecisionReview(const DecisionReview::Report& r)
{
    if (!isOpen() || !r.date.isValid()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO decision_review(trade_date, created_at, payload) VALUES(?,?,?)"
        " ON CONFLICT(trade_date) DO UPDATE SET created_at=excluded.created_at,"
        " payload=excluded.payload"));
    q.addBindValue(r.date.toString(Qt::ISODate));
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(payloadOf(r.toJson()));
    return q.exec();
}

DecisionReview::Report Database::loadDecisionReview(const QDate& date) const
{
    DecisionReview::Report out;
    if (!isOpen() || !date.isValid()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT payload FROM decision_review WHERE trade_date=?"));
    q.addBindValue(date.toString(Qt::ISODate));
    if (!q.exec() || !q.next()) return out;
    return DecisionReview::Report::fromJson(
        QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object());
}

bool Database::saveSectorFlows(const QDate& date, const QDateTime& fetchedAt,
                               const QVector<DecisionReview::SectorFlowRow>& rows,
                               const QString& source)
{
    if (!isOpen() || !date.isValid()) return false;
    QJsonArray a;
    for (const DecisionReview::SectorFlowRow& r : rows) a.append(r.toJson());
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO sector_flow_daily(trade_date, fetched_at, source, reason, payload)"
        " VALUES(?,?,?,?,?)"
        " ON CONFLICT(trade_date) DO UPDATE SET fetched_at=excluded.fetched_at,"
        " source=excluded.source, reason=excluded.reason, payload=excluded.payload"));
    q.addBindValue(date.toString(Qt::ISODate));
    q.addBindValue(fetchedAt.toString(Qt::ISODate));
    q.addBindValue(source);
    q.addBindValue(QString());
    q.addBindValue(QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
    return q.exec();
}

bool Database::saveSectorFlowFailure(const QDate& date, const QString& reason)
{
    if (!isOpen() || !date.isValid()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO sector_flow_daily(trade_date, fetched_at, source, reason, payload)"
        " VALUES(?,?,?,?,?)"
        " ON CONFLICT(trade_date) DO UPDATE SET fetched_at=excluded.fetched_at,"
        " source=excluded.source, reason=excluded.reason, payload=excluded.payload"));
    q.addBindValue(date.toString(Qt::ISODate));
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(QStringLiteral(
        "东方财富板块资金流 clist：f3 / f62 / f184（本次请求失败，字段口径见复盘页说明）"));
    q.addBindValue(reason);
    q.addBindValue(QStringLiteral("[]"));
    return q.exec();
}

QVector<DecisionReview::SectorFlowRow> Database::loadSectorFlows(
    const QDate& date, QDateTime* fetchedAt, QString* source, QString* reason) const
{
    QVector<DecisionReview::SectorFlowRow> out;
    if (!isOpen() || !date.isValid()) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT fetched_at, source, reason, payload FROM sector_flow_daily WHERE trade_date=?"));
    q.addBindValue(date.toString(Qt::ISODate));
    if (!q.exec() || !q.next()) {
        if (reason) *reason = QStringLiteral("当日未拉取过板块资金流（无留档）");
        return out;
    }
    if (fetchedAt) *fetchedAt = QDateTime::fromString(q.value(0).toString(), Qt::ISODate);
    if (source) *source = q.value(1).toString();
    if (reason) *reason = q.value(2).toString();
    const QJsonArray a = QJsonDocument::fromJson(q.value(3).toString().toUtf8()).array();
    for (const QJsonValue& v : a)
        out.append(DecisionReview::SectorFlowRow::fromJson(v.toObject()));
    return out;
}

// ---------------------------------------------------------------- 统计
TradeStats computeTradeStats(const QVector<TradeRecord>& list)
{
    TradeStats s;
    s.total = list.size();

    double pnlSum = 0.0;
    double pctSum = 0.0;
    double winSum = 0.0;
    int winN = 0;
    double lossSum = 0.0;
    int lossN = 0;
    double holdSum = 0.0;
    int holdN = 0;

    // 按买入日排序后计算累计盈亏曲线的回撤
    QVector<TradeRecord> closed;
    for (const TradeRecord& r : list) if (r.closed()) closed.append(r);
    std::sort(closed.begin(), closed.end(), [](const TradeRecord& a, const TradeRecord& b) {
        return a.sellDate < b.sellDate;
    });

    double cum = 0.0;
    double peak = 0.0;
    int stopHit = 0;
    for (const TradeRecord& r : closed) {
        ++s.closedCount;
        const double pnl = r.pnl();
        const double pct = r.pnlPct();
        s.totalPnl += pnl;
        pnlSum += pnl;
        pctSum += pct;

        if (pnl > 0.0) {
            ++s.winCount;
            winSum += pct;
            ++winN;
            s.maxWin = qMax(s.maxWin, pct);
        } else {
            lossSum += pct;
            ++lossN;
            s.maxLoss = qMin(s.maxLoss, pct);
        }
        if (r.executedAsPlanned()) ++s.planFollowed;
        if (r.planStop > 0.0 && r.pnlPct() <=
            (r.planStop / qMax(1e-9, r.buyPrice) - 1.0) * 100.0 + 0.3) {
            // 收盘价已跌破计划止损（容错 0.3%）
            ++stopHit;
        }
        if (r.holdDays() >= 0) { holdSum += r.holdDays(); ++holdN; }

        cum += pct;                     // 用百分比串联，避免仓位不同导致金额不可比
        peak = qMax(peak, cum);
        s.maxDrawdown = qMax(s.maxDrawdown, peak - cum);
    }

    if (s.closedCount > 0) {
        s.winRate = double(s.winCount) / double(s.closedCount);
        s.avgPnlPct = pctSum / double(s.closedCount);
        // 期望 = 胜率×平均盈利 + 败率×平均亏损
        const double avgWin = winN > 0 ? winSum / winN : 0.0;
        const double avgLoss = lossN > 0 ? lossSum / lossN : 0.0;
        s.avgWinPct = avgWin;
        s.avgLossPct = avgLoss;
        s.expectancy = s.winRate * avgWin + (1.0 - s.winRate) * avgLoss;
        s.avgHoldDays = holdN > 0 ? holdSum / holdN : 0.0;
        s.stopLossHitRate = double(stopHit) / double(s.closedCount);
    }
    Q_UNUSED(pnlSum);
    return s;
}
