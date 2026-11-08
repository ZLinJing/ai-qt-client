#include "data/LimitPoolSource.h"        // 本模块声明
#include "data/TextCodec.h"              // 编码工具
#include "core/Indicators.h"             // Ind::parseHms 解析封板时间
#include <QJsonDocument>                 // JSON 文档解析
#include <QJsonObject>                   // 根对象
#include <QJsonArray>                    // pool 数组
#include <QDateTime>                     // 日期格式化
#include <QDebug>                        // 数据异常告警

namespace {
const char* kUt = "7eea3edcaed734bea9cbfc24409ed989";  // 东财接口固定 ut 令牌
}

LimitPoolSource::LimitPoolSource(HttpClient* http, QObject* parent)
    : QObject(parent), m_http(http)       // 记录外部注入的取数器
{
}

// 解析一个池的 JSON：字段名与东财接口一一对应
// yesterday=true 时解析「昨日涨停池」（getYesterdayZTPool）：
//   连板数字段为 ylbc、首封时间为 yfbt、无 lbt/fund/zbc，
//   且 zdp 是「今日涨跌幅」（可直接算昨日涨停溢价），条目带 hasTodayPct 标记
static QVector<LimitEntry> parsePool(const QByteArray& raw, bool broken, const QDate& date,
                                     bool yesterday = false)
{
    QVector<LimitEntry> out;                     // 解析结果
    QJsonParseError err;                         // 解析错误信息
    QJsonDocument doc = QJsonDocument::fromJson(raw, &err);  // 解析 JSON
    if (err.error != QJsonParseError::NoError) return out;   // 非法 JSON 直接返回空
    QJsonObject root = doc.object();             // 根对象
    QJsonObject data = root.value("data").toObject();  // data 子对象
    QJsonArray pool = data.value("pool").toArray();    // 池数组

    for (const QJsonValue& v : pool) {           // 逐只解析
        QJsonObject o = v.toObject();            // 单只对象
        LimitEntry e;                            // 单只结果
        e.code         = o.value("c").toString();          // 代码
        e.name         = o.value("n").toString();          // 名称
        e.changePct    = o.value("zdp").toDouble();        // 涨跌幅（昨日池=今日涨跌幅）
        e.amount       = o.value("amount").toDouble();     // 成交额
        e.floatCap     = o.value("ltsz").toDouble();       // 流通市值
        e.totalCap     = o.value("tshare").toDouble();     // 总市值
        e.turnover     = o.value("hs").toDouble();         // 换手率
        e.boardCount   = o.value(yesterday ? QStringLiteral("ylbc")
                                           : QStringLiteral("lbc")).toInt(1);  // 连板数（缺省 1）
        e.breakCount   = o.value("zbc").toInt(0);          // 炸板次数（缺省 0）
        e.sealOrder    = o.value("fund").toDouble();       // 封单额
        e.sector       = o.value("hybk").toString();       // 行业板块
        e.broken       = broken;                           // 是否来自炸板池
        e.hasTodayPct  = yesterday;                        // 昨日池的 zdp 即今日涨跌幅

        const double p = o.value("p").toDouble();          // 价格字段原始值（单位：厘）
        e.price = p > 0 ? p / 1000.0 : 0.0;                // 厘 → 元
        const double ztp = o.value("ztp").toDouble();      // 涨停价原始值（单位：厘）
        e.limitPrice = ztp > 0 ? ztp / 1000.0 : 0.0;       // 厘 → 元

        e.board = boardOfCode(e.code);                     // 由代码推断板块

        e.firstSealTime = Ind::parseHms(o.value(yesterday ? QStringLiteral("yfbt")
                                                          : QStringLiteral("fbt")).toInt());  // 首次封板时间
        e.lastSealTime  = yesterday ? QTime()                              // 昨日池无最后封板时间
                                    : Ind::parseHms(o.value("lbt").toInt());             // 最后封板时间
        e.tradeDate     = date;                            // 归属交易日

        e.isOneWordBoard = e.firstSealTime.isValid()       // 一字板：首封与最后封同一时刻
                        && e.lastSealTime.isValid()
                        && e.firstSealTime == e.lastSealTime
                        && e.turnover < 3.0;               // 且换手极低

        // 数据质量断言：字段/单位变更时不能静默算出错误的买入与止损价位
        const bool priceSane = e.price >= 0.5 && e.price <= 2000.0;
        const bool limitSane = e.limitPrice <= 0.0
                            || (e.limitPrice >= e.price * 0.85 && e.limitPrice <= e.price * 1.35);
        if (!priceSane || !limitSane || e.floatCap < 0.0 || e.turnover < 0.0) {
            qWarning().noquote()
                << QStringLiteral("[数据源] 涨停池条目价格/量纲异常，已丢弃：%1 %2 p=%3 ztp=%4")
                       .arg(e.code, e.name).arg(e.price).arg(e.limitPrice);
            continue;
        }
        if (!e.code.isEmpty()) out.append(e);              // 代码非空才收录
    }
    if (!pool.isEmpty() && out.isEmpty()) {
        qWarning().noquote()
            << QStringLiteral("[数据源] 涨停池原始 %1 条全部未通过解析，疑似接口字段结构变更")
                   .arg(pool.size());
    }
    return out;                                        // 返回解析结果
}

// 拉取三类池：重置状态并并发发起三个请求
void LimitPoolSource::fetch(const QDate& date)
{
    m_limitUp.clear(); m_broken.clear(); m_limitDown.clear();  // 清空上一轮缓存
    m_broadcast = false;                         // 复位广播标记
    m_singleMode = false;                        // 三池模式
    m_date = date;                               // 记录交易日
    m_pending = { QStringLiteral("ZT"), QStringLiteral("ZB"), QStringLiteral("DT") };  // 三个池都待 settle
    requestPool(date, QStringLiteral("ZT"));     // 请求涨停池
    requestPool(date, QStringLiteral("ZB"));     // 请求炸板池
    requestPool(date, QStringLiteral("DT"));     // 请求跌停池
}

// 单池拉取（用于补历史交易日）
void LimitPoolSource::fetchOnly(const QDate& date, const QString& kind)
{
    m_singleMode = true;                         // 走 partialPoolReady 分支
    m_broadcast = false;                         // 复位广播标记
    m_date = date;                               // 记录交易日
    m_pending = { kind };                        // 只有一个池待 settle
    requestPool(date, kind);                     // 发起请求
}

// 拉取「昨日涨停池」：getYesterdayZTPool 自带每只昨日涨停股的今日涨跌幅（zdp），
// 溢价不再依赖新浪快照拼装；为空或失败时回退 getTopicZTPool 历史模式
void LimitPoolSource::fetchYesterdayPool(const QDate& prevDate)
{
    m_singleMode = true;                         // 成功时同样走 partialPoolReady
    m_broadcast = false;                         // 复位广播标记
    m_date = prevDate;                           // 池所属交易日 = 昨日
    m_pending = { QStringLiteral("ZT") };        // 单池待 settle
    m_yesterdayMode = true;                      // 标记昨日池模式（供失败回调区分）

    const QString d = prevDate.addDays(1).toString(QStringLiteral("yyyyMMdd"));  // 接口 date 参数 = 目标交易日
    const QString url = QStringLiteral("https://push2ex.eastmoney.com/getYesterdayZTPool"
                                       "?ut=%1&dpt=wz.ztzt&Pageindex=0&pagesize=5000"
                                       "&sort=zs%3Adesc&date=%2")
                            .arg(QString::fromLatin1(kUt), d);   // 昨日池按涨速排序（官方页面口径）

    m_http->get(url, {{"Referer", "https://data.eastmoney.com/"}}, QStringLiteral("utf-8"),
                [this, prevDate](const QByteArray& raw) {       // 成功回调
                    const QVector<LimitEntry> pool =            // 解析昨日池（自带今日涨跌幅）
                        parsePool(raw, false, prevDate, /*yesterday=*/true);
                    m_yesterdayMode = false;                    // 无论是否回退都先复位标记
                    if (!pool.isEmpty()) {                      // 新接口有数据
                        m_limitUp = pool;                       // 写入涨停池缓存
                        m_pending.remove(QStringLiteral("ZT")); // 标记 settle
                        tryBroadcast(prevDate);                 // 广播昨日池
                        return;                                 // 结束
                    }
                    fetchOnly(prevDate, QStringLiteral("ZT"));  // 为空（节假日/变更）回退历史模式
                },
                [this, prevDate](const QString&) {              // 失败回调
                    m_yesterdayMode = false;                    // 复位标记
                    fetchOnly(prevDate, QStringLiteral("ZT"));  // 回退历史模式（溢价改由快照补）
                });
}

// 拼装 URL 并发起请求：不同池的排序参数不同
void LimitPoolSource::requestPool(const QDate& date, const QString& kind)
{
    const QString d = date.toString(QStringLiteral("yyyyMMdd"));  // 日期格式化成接口要求的 8 位
    QString url;                                 // 目标地址
    if (kind == QStringLiteral("ZT")) {          // 涨停池
        url = QStringLiteral("https://push2ex.eastmoney.com/getTopicZTPool"
                             "?ut=%1&dpt=wz.ztzt&Pageindex=0&pagesize=2000&sort=fbt%3Aasc&date=%2")
              .arg(QString::fromLatin1(kUt), d);  // 必带 sort=fbt:asc，否则 pool 为空
    } else if (kind == QStringLiteral("ZB")) {   // 炸板池
        url = QStringLiteral("https://push2ex.eastmoney.com/getTopicZBPool"
                             "?ut=%1&dpt=wz.ztzt&Pageindex=0&pagesize=2000&sort=zbc%3Adesc&date=%2")
              .arg(QString::fromLatin1(kUt), d);  // 必带 sort=zbc:desc，否则 pool 为空
    } else {                                      // 跌停池
        // 必须带 sort=fund:asc，否则 pool 恒为空（实测 2026-11-04：不带 0 家，带后 10 家）
        url = QStringLiteral("https://push2ex.eastmoney.com/getTopicDTPool"
                             "?ut=%1&dpt=wz.ztzt&Pageindex=0&pagesize=2000&sort=fund%3Aasc&date=%2")
              .arg(QString::fromLatin1(kUt), d);
    }

    m_http->get(url, {{"Referer", "https://data.eastmoney.com/"}}, QStringLiteral("utf-8"),  // 带 Referer 取 UTF-8
                [this, date, kind](const QByteArray& raw) {   // 成功回调
                    onPoolResult(date, kind, raw);            // 交给解析
                },
                [this, date, kind](const QString& err) {      // 失败回调
                    onPoolError(date, kind, err);             // 记为 settle 后继续等其余池
                });
}

// 单池失败：先广播错误，再把该池标记为已 settle
void LimitPoolSource::onPoolError(const QDate& date, const QString& kind, const QString& err)
{
    Q_EMIT failed(QStringLiteral("[涨停池%1] 拉取失败: %2").arg(kind, err));  // 通知上层
    m_pending.remove(kind);                      // 失败也算 settle，否则永远等不到广播
    tryBroadcast(date);                          // 尝试触发广播
}

// 尝试广播：全部 settle 且未广播过才发信号
void LimitPoolSource::tryBroadcast(const QDate& date)
{
    if (m_broadcast) return;                     // 已广播过则幂等返回
    if (!m_pending.isEmpty()) return;            // 还有池子没回来
    m_broadcast = true;                          // 标记已广播
    if (m_singleMode) {                          // 单池模式
        Q_EMIT partialPoolReady(date, QStringLiteral("ZT"), m_limitUp);  // 绝不能广播 poolsReady 覆盖当日池
        return;                                  // 结束
    }
    Q_EMIT poolsReady(date, m_limitUp, m_broken, m_limitDown);  // 三池就绪
}

// 单池成功：按类型写入对应缓存
void LimitPoolSource::onPoolResult(const QDate& date, const QString& kind, const QByteArray& raw)
{
    if (kind == QStringLiteral("ZT"))      m_limitUp  = parsePool(raw, false, date);  // 涨停池
    else if (kind == QStringLiteral("ZB")) m_broken   = parsePool(raw, true, date);   // 炸板池（broken=true）
    else                                   m_limitDown = parsePool(raw, false, date); // 跌停池

    m_pending.remove(kind);                      // 标记该池已 settle
    tryBroadcast(date);                          // 尝试触发广播
}
