#include "review/ReviewEngine.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace {

QString esc(const QString& s) { return s.toHtmlEscaped(); }

QString num(double v, int d = 1) { return QString::number(v, 'f', d); }

const char* kTd = "padding:6px 8px;border-bottom:1px solid #eef1f5;color:#1b2430";
const char* kTh = "text-align:left;padding:6px 8px;background:#f2f5fa;color:#4a5260;"
                  "border-bottom:1px solid #e2e6ed;font-weight:600";

QString head(const QString& title, const QString& sub)
{
    return QStringLiteral("<h2 style=\"margin:0 0 4px 0;font-size:20px\">%1</h2>"
                          "<p style=\"margin:0 0 12px 0;color:#4a5260;font-size:13px\">%2</p>")
        .arg(esc(title), esc(sub));
}

QString kvTable(const QList<QPair<QString, QString>>& rows)
{
    QString h = QStringLiteral("<table style=\"width:100%;border-collapse:collapse;"
                               "font-size:13px;margin:6px 0 12px 0\">");
    for (const auto& r : rows) {
        h += QStringLiteral("<tr><td style=\"%1;width:38%;color:#4a5260\">%2</td>"
                            "<td style=\"%3\">%4</td></tr>")
                 .arg(QLatin1String(kTd), esc(r.first), QLatin1String(kTd), esc(r.second));
    }
    return h + QStringLiteral("</table>");
}

QString sectionTitle(const QString& t)
{
    return QStringLiteral("<h3 style=\"font-size:15px;margin:14px 0 4px 0\">%1</h3>").arg(esc(t));
}

// 从 facts 取数（缺字段一律回落默认值，避免历史数据缺字段时崩）
double factD(const QJsonObject& o, const char* k, double def = 0.0)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toDouble() : def;
}
int factI(const QJsonObject& o, const char* k, int def = 0)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toInt() : def;
}
bool factB(const QJsonObject& o, const char* k, bool def = false)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isBool() ? v.toBool() : def;
}

struct Adj {
    QString id;
    double  factor = 1.0;   // >1 看多（涨停家数上调），<1 看空
    QString note;
};

} // namespace

// ---------------------------------------------------------------- 默认规则
QVector<AssistantRule> ReviewEngine::defaultRules()
{
    auto mk = [](const char* id, const char* name, const char* cond) {
        AssistantRule r;
        r.id = QString::fromLatin1(id);
        r.name = QString::fromUtf8(name);
        r.condition = QString::fromUtf8(cond);
        return r;
    };
    return {
        mk("R_MOMENTUM", "情绪惯性", "复合指数≥58 看延续；<42 看退潮"),
        mk("R_BROKEN",   "封板质量", "炸板率>35% 下调；<20% 上调"),
        mk("R_PREMIUM",  "赚钱效应", "昨日涨停溢价≥+2% 上调；≤-2% 下调"),
        mk("R_BREADTH",  "市场宽度", "上涨占比<35% 下调；>60% 上调"),
        mk("R_TREND",    "情绪趋势", "较5日均值回落≥12点 下调；升≥10点 上调"),
        mk("R_HEIGHT",   "空间高度", "最高≥5板 谨慎；≤2板 修复空间大"),
        mk("R_THEME",    "主线集中", "最强题材≥5家且集中度≥20% 上调"),
        mk("R_ADVANCE",  "晋级接力", "连板晋级率≥50% 上调；<30% 下调"),
    };
}

// ---------------------------------------------------------------- 复盘
ReviewReport ReviewEngine::buildReview(const ReviewInput& in)
{
    ReviewReport r;
    r.date = in.date;
    r.valid = true;

    const SentimentState& s = in.sentiment;
    const SentimentFact& sf = s.fact;

    int act = 0, obs = 0;
    for (const TradePlan& p : in.bundle.plans) {
        if (p.actionable) ++act; else ++obs;
    }

    // 题材热度（用候选池统计，避免依赖历史题材接口）
    QHash<QString, int> themeCnt;
    for (const Candidate& c : in.candidates) {
        if (!c.entry.theme.isEmpty()) themeCnt[c.entry.theme] += 1;
    }
    QVector<QPair<QString, int>> themes;
    for (auto it = themeCnt.constBegin(); it != themeCnt.constEnd(); ++it)
        themes.append(qMakePair(it.key(), it.value()));
    std::sort(themes.begin(), themes.end(),
              [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
                  return a.second > b.second;
              });
    QStringList topThemes;
    for (int i = 0; i < qMin(3, themes.size()); ++i) topThemes << themes[i].first;

    r.headline = QStringLiteral("%1 复合情绪 %2 · 涨停 %3 / 炸板 %4 · 可执行 %5")
                     .arg(in.date.toString(QStringLiteral("MM-dd")))
                     .arg(num(s.composite))
                     .arg(sf.limitUpCount).arg(sf.brokenCount).arg(act);

    // ---- facts ----
    QJsonObject f;
    f["composite"] = s.composite;
    f["phase"] = marketPhaseName(s.phase);
    f["cap"] = s.positionCapPct;
    f["tradeAllowed"] = s.tradeAllowed;
    f["degraded"] = s.degraded;
    f["trend"] = s.trend;
    f["trendKnown"] = s.trendKnown;
    f["limitUp"] = sf.limitUpCount;
    f["broken"] = sf.brokenCount;
    f["limitDown"] = sf.limitDownCount;
    f["maxBoard"] = sf.maxBoardHeight;
    f["brokenRate"] = sf.brokenRate;
    f["advance"] = sf.advanceRate;
    f["advanceKnown"] = sf.prevLimitUpCount > 0;
    f["premium"] = sf.premiumPct;
    f["premiumKnown"] = sf.premiumKnown;
    f["upRatio"] = sf.upRatio;
    f["breadthKnown"] = sf.breadthKnown;
    f["topThemeCount"] = sf.topThemeCount;
    f["themeFocus"] = sf.themeFocus;
    f["themeKnown"] = sf.themeKnown;
    f["actionable"] = act;
    f["watch"] = obs;
    QJsonArray th;
    for (const QString& t : topThemes) th.append(t);
    f["themes"] = th;
    QJsonArray veto;
    for (const QString& v : s.vetoReasons) veto.append(v);
    f["veto"] = veto;
    QJsonArray plans;
    for (const TradePlan& p : in.bundle.plans) {
        QJsonObject o;
        o["code"] = p.code;
        o["name"] = p.name;
        o["actionable"] = p.actionable;
        o["position"] = p.positionPct;
        o["buyLow"] = p.buyLow;
        o["buyHigh"] = p.buyHigh;
        o["stop"] = p.stopLossHigh;
        o["take"] = p.takeProfitLow;
        o["fill"] = p.fillProbability;
        plans.append(o);
    }
    f["plans"] = plans;
    r.facts = f;

    // ---- HTML ----
    QString h;
    h += QStringLiteral("<div style=\"font-family:'Microsoft YaHei',Arial,sans-serif;"
                        "color:#1b2430;max-width:780px\">");
    h += head(QStringLiteral("收盘复盘 · %1").arg(in.date.toString(QStringLiteral("yyyy-MM-dd"))),
              r.headline);

    h += sectionTitle(QStringLiteral("市场温度"));
    h += kvTable({
        { QStringLiteral("涨停 / 炸板 / 跌停"),
          QStringLiteral("%1 / %2 / %3 家").arg(sf.limitUpCount).arg(sf.brokenCount).arg(sf.limitDownCount) },
        { QStringLiteral("炸板率"), QStringLiteral("%1%").arg(num(sf.brokenRate * 100)) },
        { QStringLiteral("最高连板"), QStringLiteral("%1 板").arg(sf.maxBoardHeight) },
        { QStringLiteral("连板晋级率"),
          sf.prevLimitUpCount > 0 ? QStringLiteral("%1%（昨日 %2 家）")
                                        .arg(num(sf.advanceRate * 100)).arg(sf.prevLimitUpCount)
                                  : QStringLiteral("样本不足") },
        { QStringLiteral("昨日涨停溢价"),
          sf.premiumKnown ? QStringLiteral("%1%（采样 %2）")
                                .arg(num(sf.premiumPct, 2)).arg(sf.premiumSamples)
                          : QStringLiteral("样本不足") },
        { QStringLiteral("市场宽度"),
          sf.breadthKnown ? QStringLiteral("上涨 %1% · 中位数 %2%")
                                .arg(num(sf.upRatio * 100)).arg(num(sf.medianChangePct, 2))
                          : QStringLiteral("快照缺失") },
        { QStringLiteral("情绪指数"), QStringLiteral("%1（有效总仓 ≤%2%）")
                                          .arg(num(s.composite), num(s.positionCapPct, 0)) },
        { QStringLiteral("有效仓位上限"), QStringLiteral("%1%").arg(num(s.positionCapPct, 0)) },
    });

    h += sectionTitle(QStringLiteral("情绪六维"));
    h += QStringLiteral("<table style=\"width:100%;border-collapse:collapse;font-size:13px;"
                        "margin:6px 0 12px 0\"><tr>"
                        "<th style=\"%1\">维度</th><th style=\"%1\">得分</th>"
                        "<th style=\"%1\">权重</th><th style=\"%1\">明细</th></tr>")
             .arg(QLatin1String(kTh));
    for (const SentimentSubScore& sub : s.subScores) {
        h += QStringLiteral("<tr><td style=\"%1\">%2 %3</td><td style=\"%1\">%4</td>"
                            "<td style=\"%1\">%5</td><td style=\"%1;color:#4a5260\">%6</td></tr>")
                 .arg(QLatin1String(kTd), esc(sub.key), esc(sub.name))
                 .arg(sub.available ? num(sub.score) : QStringLiteral("—"))
                 .arg(num(sub.weight, 2))
                 .arg(esc(sub.detail));
    }
    h += QStringLiteral("</table>");

    h += sectionTitle(QStringLiteral("选股与计划"));
    h += kvTable({
        { QStringLiteral("筛选漏斗"),
          QStringLiteral("输入 %1 → 硬排除 %2 → 板块 %3 → 量价 %4 → 输出 %5")
              .arg(in.funnelInput).arg(in.funnelHard)
              .arg(in.funnelSector).arg(in.funnelQuant).arg(in.funnelOut) },
        { QStringLiteral("可执行 / 观察"), QStringLiteral("%1 / %2").arg(act).arg(obs) },
        { QStringLiteral("主线题材"),
          topThemes.isEmpty() ? QStringLiteral("题材数据缺失") : topThemes.join(QStringLiteral("、")) },
    });

    if (!in.bundle.plans.isEmpty()) {
        h += QStringLiteral("<table style=\"width:100%;border-collapse:collapse;font-size:13px;"
                            "margin:6px 0 12px 0\"><tr>"
                            "<th style=\"%1\">标的</th><th style=\"%1\">状态</th>"
                            "<th style=\"%1\">买入</th><th style=\"%1\">止损</th>"
                            "<th style=\"%1\">仓位</th><th style=\"%1\">成交概率</th></tr>")
                 .arg(QLatin1String(kTh));
        for (const TradePlan& p : in.bundle.plans) {
            h += QStringLiteral("<tr><td style=\"%1\"><b>%2</b> %3</td>"
                                "<td style=\"%1\">%4</td><td style=\"%1\">%5~%6</td>"
                                "<td style=\"%1\">%7</td><td style=\"%1\">%8%</td>"
                                "<td style=\"%1\">%9%</td></tr>")
                     .arg(QLatin1String(kTd), esc(p.code), esc(p.name))
                     .arg(p.actionable ? QStringLiteral("可执行") : QStringLiteral("观察"))
                     .arg(num(p.buyLow, 2), num(p.buyHigh, 2), num(p.stopLossHigh, 2))
                     .arg(num(p.positionPct))
                     .arg(num(p.fillProbability * 100, 0));
        }
        h += QStringLiteral("</table>");
    }

    if (!s.vetoReasons.isEmpty() || s.degraded) {
        h += sectionTitle(QStringLiteral("风险与数据质量"));
        h += QStringLiteral("<ul style=\"margin:4px 0 10px 18px;padding:0;font-size:13px;color:#8a3b12\">");
        for (const QString& v : s.vetoReasons)
            h += QStringLiteral("<li>%1</li>").arg(esc(v));
        for (const QString& w : s.dataWarnings)
            h += QStringLiteral("<li>%1</li>").arg(esc(w));
        if (s.degraded) h += QStringLiteral("<li>存在分项数据缺失，仓位已按降级系数折减</li>");
        h += QStringLiteral("</ul>");
    }

    h += QStringLiteral("<p style=\"margin-top:16px;color:#8a93a1;font-size:12px\">"
                        "生成时间：%1 ｜ 数据源：东财 / 腾讯 / 新浪<br/>"
                        "本报告由做多量化选股系统自动生成，仅用于策略研究，不构成投资建议。</p></div>")
             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    r.html = h;
    r.text = r.headline;
    return r;
}

// ---------------------------------------------------------------- 次日预判
Forecast ReviewEngine::buildForecast(const ReviewReport& yesterday,
                                     const QVector<AssistantRule>& rules,
                                     double biasLogRatio)
{
    Forecast f;
    f.valid = false;
    if (!yesterday.valid) return f;

    const QJsonObject& facts = yesterday.facts;
    const int    lastUp    = factI(facts, "limitUp");
    const int    lastBoard = factI(facts, "maxBoard", 1);
    const double comp      = factD(facts, "composite");
    const double brokRate  = factD(facts, "brokenRate");
    const double prem      = factD(facts, "premium");
    const bool   premKnown = factB(facts, "premiumKnown");
    const double upRatio   = factD(facts, "upRatio");
    const bool   brKnown   = factB(facts, "breadthKnown");
    const double trend     = factD(facts, "trend");
    const bool   trKnown   = factB(facts, "trendKnown");
    const double adv       = factD(facts, "advance");
    const bool   advKnown  = factB(facts, "advanceKnown");
    const double cap       = factD(facts, "cap");
    const double topTheme  = factI(facts, "topThemeCount");
    const double focus     = factD(facts, "themeFocus");
    const bool   thKnown   = factB(facts, "themeKnown");

    f.forDate = yesterday.date.addDays(1);
    while (f.forDate.dayOfWeek() > Qt::Friday) f.forDate = f.forDate.addDays(1);
    f.madeAt = QDate::currentDate();
    f.valid = true;

    QVector<Adj> adjs;
    if (comp >= 58.0)      adjs.append({ "R_MOMENTUM", 1.06, QStringLiteral("情绪处于高位（%1），惯性延续").arg(num(comp)) });
    else if (comp < 42.0)  adjs.append({ "R_MOMENTUM", 0.80, QStringLiteral("情绪偏冷（%1），次日大概率缩量").arg(num(comp)) });
    else                   adjs.append({ "R_MOMENTUM", 0.97, QStringLiteral("情绪中性（%1）").arg(num(comp)) });

    if (brokRate > 0.35)      adjs.append({ "R_BROKEN", 0.86, QStringLiteral("炸板率 %1% 偏高").arg(num(brokRate * 100)) });
    else if (brokRate < 0.20) adjs.append({ "R_BROKEN", 1.05, QStringLiteral("炸板率 %1% 偏低，封板质量好").arg(num(brokRate * 100)) });
    else                      adjs.append({ "R_BROKEN", 1.00, QStringLiteral("炸板率 %1% 正常").arg(num(brokRate * 100)) });

    if (premKnown) {
        if (prem >= 2.0)      adjs.append({ "R_PREMIUM", 1.06, QStringLiteral("昨日涨停溢价 %1%，接力有钱赚").arg(num(prem, 2)) });
        else if (prem <= -2.0) adjs.append({ "R_PREMIUM", 0.82, QStringLiteral("昨日涨停溢价 %1%，打板即亏").arg(num(prem, 2)) });
        else                   adjs.append({ "R_PREMIUM", 1.00, QStringLiteral("溢价 %1%，中性").arg(num(prem, 2)) });
    }

    if (brKnown) {
        if (upRatio < 0.35)      adjs.append({ "R_BREADTH", 0.88, QStringLiteral("全市场仅 %1% 上涨，宽度差").arg(num(upRatio * 100)) });
        else if (upRatio > 0.60) adjs.append({ "R_BREADTH", 1.04, QStringLiteral("上涨占比 %1%，普涨格局").arg(num(upRatio * 100)) });
        else                     adjs.append({ "R_BREADTH", 1.00, QStringLiteral("上涨占比 %1%").arg(num(upRatio * 100)) });
    }

    if (trKnown) {
        if (trend <= -12.0)    adjs.append({ "R_TREND", 0.82, QStringLiteral("情绪较 5 日均值回落 %1 点").arg(num(-trend)) });
        else if (trend >= 10.0) adjs.append({ "R_TREND", 1.05, QStringLiteral("情绪较 5 日均值抬升 %1 点").arg(num(trend)) });
        else                    adjs.append({ "R_TREND", 1.00, QStringLiteral("情绪趋势平稳") });
    }

    if (lastBoard >= 5)      adjs.append({ "R_HEIGHT", 0.94, QStringLiteral("空间已到 %1 板，高位分歧概率上升").arg(lastBoard) });
    else if (lastBoard <= 2) adjs.append({ "R_HEIGHT", 1.04, QStringLiteral("空间仅 %1 板，向上修复空间大").arg(lastBoard) });
    else                     adjs.append({ "R_HEIGHT", 1.00, QStringLiteral("空间 %1 板，中性").arg(lastBoard) });

    if (thKnown) {
        if (topTheme >= 5 && focus >= 0.20)
            adjs.append({ "R_THEME", 1.04, QStringLiteral("最强题材 %1 家、集中度 %2%，主线明确")
                                             .arg(int(topTheme)).arg(num(focus * 100, 0)) });
        else
            adjs.append({ "R_THEME", 0.99, QStringLiteral("主线不集中（最强题材 %1 家）").arg(int(topTheme)) });
    }

    if (advKnown) {
        if (adv >= 0.50)      adjs.append({ "R_ADVANCE", 1.05, QStringLiteral("连板晋级率 %1%").arg(num(adv * 100)) });
        else if (adv < 0.30)  adjs.append({ "R_ADVANCE", 0.87, QStringLiteral("连板晋级率仅 %1%").arg(num(adv * 100)) });
        else                  adjs.append({ "R_ADVANCE", 1.00, QStringLiteral("晋级率 %1%").arg(num(adv * 100)) });
    }

    // 权重加权（对数空间），再叠加历史幅度偏差校正
    double wsum = 0.0, lsum = 0.0;
    QJsonArray trace;
    for (const Adj& a : adjs) {
        double w = 1.0;
        bool active = true;
        for (const AssistantRule& r : rules) {
            if (r.id == a.id) { w = r.weight; active = r.active; break; }
        }
        QJsonObject t;
        t["id"] = a.id;
        t["factor"] = a.factor;
        t["weight"] = w;
        t["note"] = a.note;
        t["active"] = active;
        trace.append(t);
        if (!active) continue;
        wsum += w;
        lsum += w * std::log(a.factor);
    }
    const double adj = wsum > 0.0 ? std::exp(lsum / wsum) : 1.0;
    const double bias = qBound(-0.45, biasLogRatio, 0.45);
    const double centerRaw = lastUp * adj * std::exp(bias);

    f.limitUpCenter = qMax(0, int(std::lround(centerRaw)));
    f.limitUpLow = qMax(0, int(std::lround(centerRaw * 0.82 - 2.0)));
    f.limitUpHigh = qMax(f.limitUpLow + 2, int(std::lround(centerRaw * 1.18 + 2.0)));

    // 空间高度：按晋级率 / 炸板率做修正
    int board = lastBoard;
    if (advKnown && adv < 0.30) board -= 2;
    else if (advKnown && adv >= 0.50) board += 1;
    if (brokRate > 0.35) board -= 1;
    board = qBound(1, board, 8);
    f.maxBoardLow = qMax(1, board - 1);
    f.maxBoardHigh = qMin(8, board + 1);

    // 晋级率：以昨日为基准做区间（没有更可靠的信息源）
    const double advCenter = advKnown ? adv : 0.35;
    f.advanceLow = qBound(0.0, advCenter - 0.15, 1.0);
    f.advanceHigh = qBound(0.0, advCenter + 0.15, 1.0);

    // 立场
    if (!factB(facts, "tradeAllowed")) {
        f.stance = QStringLiteral("退潮风险（空仓观察）");
    } else if (comp >= 58.0 && (!advKnown || adv >= 0.50)) {
        f.stance = QStringLiteral("强势延续（可做接力）");
    } else if (comp >= 58.0) {
        f.stance = QStringLiteral("高位分歧（只做低位首板）");
    } else if (comp >= 42.0) {
        f.stance = QStringLiteral("结构分化（只做主线龙头）");
    } else {
        f.stance = QStringLiteral("弱修复（小仓试错）");
    }

    f.capPct = cap * 0.9;   // 预判层面给 10% 安全垫，实际额度以当日情绪为准
    f.themes.clear();
    for (const QJsonValue& v : facts.value(QStringLiteral("themes")).toArray())
        f.themes << v.toString();

    // 剧本
    f.playbook << QStringLiteral("开盘 30 分钟涨停家数 ≥ %1 家 → 按计划执行；不足则当日只观察")
                      .arg(qMax(1, int(f.limitUpLow * 0.6)));
    f.playbook << QStringLiteral("只做 ≤%1 板，优先题材龙头；超过高度一律不追").arg(f.maxBoardHigh);
    f.playbook << QStringLiteral("若盘中炸板率升破 35%，停止加仓并降低持仓");
    f.playbook << QStringLiteral("有效仓位控制在 %1% 以内，单票不超过 %2%")
                      .arg(num(f.capPct, 0)).arg(num(20.0, 0));

    // 风险
    if (comp >= 58.0)
        f.risks << QStringLiteral("情绪高位，一旦晋级率跌破 30% 容易一日退潮");
    if (brokRate > 0.30)
        f.risks << QStringLiteral("炸板率偏高（%1%），封板不牢的票次日容易低开").arg(num(brokRate * 100));
    if (premKnown && prem < 0.0)
        f.risks << QStringLiteral("昨日涨停今日转负（%1%），接力意愿弱").arg(num(prem, 2));
    if (!brKnown)
        f.risks << QStringLiteral("缺全市场宽度数据，情绪判断置信度下降");

    // 置信度：命中规则的历史准确率（无历史时给 0.5）
    double accSum = 0.0, accW = 0.0;
    for (const AssistantRule& r : rules) {
        if (!r.active || r.total <= 0) continue;
        bool fired = false;
        for (const Adj& a : adjs) if (a.id == r.id) { fired = true; break; }
        if (!fired) continue;
        accSum += r.accuracy() * r.weight;
        accW += r.weight;
    }
    f.confidence = accW > 0.0 ? accSum / accW : 0.5;
    if (factB(facts, "degraded")) f.confidence = qMax(0.15, f.confidence - 0.15);

    trace.append(QJsonObject{ { "adj", adj }, { "bias", bias }, { "lastUp", lastUp } });
    f.trace = QJsonObject{ { "rules", trace }, { "adj", adj }, { "bias", bias },
                           { "lastUp", lastUp }, { "lastBoard", lastBoard },
                           { "confidence", f.confidence } };

    // ---- HTML 简报 ----
    QString h;
    h += QStringLiteral("<div style=\"font-family:'Microsoft YaHei',Arial,sans-serif;"
                        "color:#1b2430;max-width:780px\">");
    h += head(QStringLiteral("早盘投资简报 · %1").arg(f.forDate.toString(QStringLiteral("yyyy-MM-dd"))),
              QStringLiteral("基于 %1 收盘复盘 · 置信度 %2%")
                  .arg(yesterday.date.toString(QStringLiteral("MM-dd")))
                  .arg(num(f.confidence * 100, 0)));

    h += QStringLiteral("<div style=\"background:#f6f8fb;border-left:3px solid #2f6fed;"
                        "padding:10px 12px;font-size:14px;margin-bottom:12px\">"
                        "今日预判：<b>%1</b></div>").arg(esc(f.stance));

    const QString dirWord = centerRaw > lastUp * 1.03 ? QStringLiteral("放大")
                          : (centerRaw < lastUp * 0.97 ? QStringLiteral("收缩") : QStringLiteral("持平"));
    h += kvTable({
        { QStringLiteral("涨停家数"),
          QStringLiteral("%1 ~ %2 家（昨日 %3，预计%4）")
              .arg(f.limitUpLow).arg(f.limitUpHigh).arg(lastUp).arg(dirWord) },
        { QStringLiteral("空间高度"), QStringLiteral("%1 ~ %2 板（昨日 %3 板）")
                                          .arg(f.maxBoardLow).arg(f.maxBoardHigh).arg(lastBoard) },
        { QStringLiteral("连板晋级率"), QStringLiteral("%1% ~ %2%")
                                          .arg(num(f.advanceLow * 100, 0), num(f.advanceHigh * 100, 0)) },
        { QStringLiteral("建议有效仓位"), QStringLiteral("≤%1%").arg(num(f.capPct, 0)) },
        { QStringLiteral("主线预期"),
          f.themes.isEmpty() ? QStringLiteral("无明确主线") : f.themes.join(QStringLiteral("、")) },
    });

    h += sectionTitle(QStringLiteral("今日剧本"));
    h += QStringLiteral("<ul style=\"margin:4px 0 10px 18px;padding:0;font-size:13px\">");
    for (const QString& p : f.playbook) h += QStringLiteral("<li>%1</li>").arg(esc(p));
    h += QStringLiteral("</ul>");

    if (!f.risks.isEmpty()) {
        h += sectionTitle(QStringLiteral("风险提示"));
        h += QStringLiteral("<ul style=\"margin:4px 0 10px 18px;padding:0;font-size:13px;color:#8a3b12\">");
        for (const QString& p : f.risks) h += QStringLiteral("<li>%1</li>").arg(esc(p));
        h += QStringLiteral("</ul>");
    }

    h += QStringLiteral("<p style=\"margin-top:16px;color:#8a93a1;font-size:12px\">"
                        "生成时间：%1 ｜ 本简报由规则引擎生成，仅用于策略研究，不构成投资建议。</p></div>")
             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    f.html = h;
    return f;
}

// ---------------------------------------------------------------- 评估
ForecastEval ReviewEngine::evaluate(const Forecast& f, const SentimentState& actual,
                                    const ReviewInput& today)
{
    ForecastEval e;
    if (!f.valid || !actual.fact.valid()) return e;
    e.forDate = f.forDate;
    e.valid = true;

    const SentimentFact& a = actual.fact;
    e.actualLimitUp = a.limitUpCount;
    e.actualMaxBoard = a.maxBoardHeight;
    e.actualAdvance = a.advanceRate;

    const bool upHit   = a.limitUpCount >= f.limitUpLow && a.limitUpCount <= f.limitUpHigh;
    const bool boardHit = a.maxBoardHeight >= f.maxBoardLow && a.maxBoardHeight <= f.maxBoardHigh;
    const bool advKnown = a.prevLimitUpCount > 0;
    const bool advHit = !advKnown || (a.advanceRate >= f.advanceLow && a.advanceRate <= f.advanceHigh);

    const int lastUp = f.trace.value(QStringLiteral("lastUp")).toInt(f.limitUpCenter);
    const double predDir = f.limitUpCenter > lastUp * 1.03 ? 1.0
                         : (f.limitUpCenter < lastUp * 0.97 ? -1.0 : 0.0);
    const double actDir = a.limitUpCount > lastUp * 1.03 ? 1.0
                        : (a.limitUpCount < lastUp * 0.97 ? -1.0 : 0.0);
    const bool dirHit = qAbs(predDir - actDir) < 0.5;
    e.directionHit = dirHit ? 1 : 0;

    int score = 0;
    score += upHit ? 40 : 0;
    score += boardHit ? 25 : 0;
    score += advHit ? 20 : 0;
    score += dirHit ? 15 : 0;
    e.score = score;
    e.hit = score >= 65;

    // ---- 偏差归因 ----
    const QString range = QStringLiteral("预判 %1~%2，实际 %3")
                              .arg(f.limitUpLow).arg(f.limitUpHigh).arg(a.limitUpCount);
    if (!upHit) {
        QString cause;
        if (a.limitUpCount < f.limitUpLow) {
            if (a.premiumKnown && a.premiumPct < 0.0)
                cause = QStringLiteral("昨日涨停今日转负（%1%），接力资金撤退").arg(num(a.premiumPct, 2));
            else if (a.breadthKnown && a.upRatio < 0.35)
                cause = QStringLiteral("全市场仅 %1% 上涨，宽度拖累赚钱效应").arg(num(a.upRatio * 100));
            else
                cause = QStringLiteral("封板质量恶化（炸板率 %1%）").arg(num(a.brokenRate * 100));
            e.deviations << QStringLiteral("涨停家数低于下限（%1）：%2").arg(range, cause);
        } else {
            QString cause = (a.premiumKnown && a.premiumPct > 2.0)
                                ? QStringLiteral("溢价走高带动接力，修复力度超预期")
                                : QStringLiteral("低位补涨扩散，低估了修复弹性");
            e.deviations << QStringLiteral("涨停家数高于上限（%1）：%2").arg(range, cause);
        }
    }
    if (!boardHit) {
        e.deviations << QStringLiteral("空间高度不符（预判 %1~%2 板，实际 %3 板）%4")
                            .arg(f.maxBoardLow).arg(f.maxBoardHigh).arg(a.maxBoardHeight)
                            .arg(a.maxBoardHeight > f.maxBoardHigh
                                     ? QStringLiteral("：接力超预期，需上调晋级权重")
                                     : QStringLiteral("：高位断板，需下调高度预期"));
    }
    if (!advHit) {
        e.deviations << QStringLiteral("连板晋级率偏离（预判 %1%~%2%，实际 %3%）")
                            .arg(num(f.advanceLow * 100, 0), num(f.advanceHigh * 100, 0),
                                 num(a.advanceRate * 100));
    }
    if (!dirHit) {
        e.deviations << QStringLiteral("方向判断错误：预判「%1」为%2，实际%3")
                            .arg(f.stance,
                                 predDir > 0 ? QStringLiteral("扩张") : (predDir < 0 ? QStringLiteral("收缩") : QStringLiteral("持平")),
                                 actDir > 0 ? QStringLiteral("扩张") : (actDir < 0 ? QStringLiteral("收缩") : QStringLiteral("持平")));
    }

    // ---- 反思 ----
    if (e.hit) {
        e.lessons << QStringLiteral("预判命中（%1 分），保留该判断思路并上调命中规则权重").arg(score);
    } else if (dirHit) {
        e.lessons << QStringLiteral("方向正确、幅度偏差：保留思路，仅校正幅度阈值（偏差校正项已更新）");
    } else {
        e.lessons << QStringLiteral("方向性错误：降低本组规则权重，下一轮以更保守的基准区间出预判");
    }
    if (!boardHit && a.maxBoardHeight <= 2) {
        e.lessons << QStringLiteral("高位股集体断板，空间高度应更多依赖晋级率而非昨日高度");
    }
    if (advKnown && a.advanceRate < 0.30 && f.limitUpCenter > a.limitUpCount) {
        e.lessons << QStringLiteral("晋级率跌破 30% 时，涨停家数不宜线性外推");
    }

    QString h;
    h += QStringLiteral("<div style=\"font-family:'Microsoft YaHei',Arial,sans-serif;"
                        "color:#1b2430;max-width:780px\">");
    h += head(QStringLiteral("预判复核 · %1").arg(f.forDate.toString(QStringLiteral("yyyy-MM-dd"))),
              QStringLiteral("得分 %1 / 100 · %2")
                  .arg(score)
                  .arg(e.hit ? QStringLiteral("命中") : QStringLiteral("偏差")));
    h += kvTable({
        { QStringLiteral("预判"), QStringLiteral("%1 ｜ 涨停 %2~%3 ｜ 高度 %4~%5 板")
                                      .arg(f.stance).arg(f.limitUpLow).arg(f.limitUpHigh)
                                      .arg(f.maxBoardLow).arg(f.maxBoardHigh) },
        { QStringLiteral("实际"), QStringLiteral("涨停 %1 ｜ 高度 %2 板 ｜ 晋级率 %3%")
                                      .arg(a.limitUpCount).arg(a.maxBoardHeight)
                                      .arg(num(a.advanceRate * 100, 0)) },
    });
    if (!e.deviations.isEmpty()) {
        h += sectionTitle(QStringLiteral("偏差与归因"));
        h += QStringLiteral("<ul style=\"margin:4px 0 10px 18px;padding:0;font-size:13px;color:#8a3b12\">");
        for (const QString& d : e.deviations) h += QStringLiteral("<li>%1</li>").arg(esc(d));
        h += QStringLiteral("</ul>");
    }
    if (!e.lessons.isEmpty()) {
        h += sectionTitle(QStringLiteral("反思与迭代"));
        h += QStringLiteral("<ul style=\"margin:4px 0 10px 18px;padding:0;font-size:13px\">");
        for (const QString& d : e.lessons) h += QStringLiteral("<li>%1</li>").arg(esc(d));
        h += QStringLiteral("</ul>");
    }
    h += QStringLiteral("</div>");
    e.html = h;

    Q_UNUSED(today);
    return e;
}

// ---------------------------------------------------------------- 规则迭代
QVector<RuleUpdate> ReviewEngine::updateRules(QVector<AssistantRule>& rules,
                                              const Forecast& f, const ForecastEval& e)
{
    QVector<RuleUpdate> updates;
    if (!f.valid || !e.valid) return updates;

    const int lastUp = f.trace.value(QStringLiteral("lastUp")).toInt(f.limitUpCenter);
    const int actDir = e.actualLimitUp > lastUp * 1.03 ? 1
                     : (e.actualLimitUp < lastUp * 0.97 ? -1 : 0);

    for (const QJsonValue& v : f.trace.value(QStringLiteral("rules")).toArray()) {
        const QJsonObject t = v.toObject();
        const QString id = t.value(QStringLiteral("id")).toString();
        const double factor = t.value(QStringLiteral("factor")).toDouble(1.0);
        if (factor <= 0.0 || qAbs(factor - 1.0) < 1e-9) continue;   // 中性判断不计数

        const int ruleDir = factor > 1.0 ? 1 : -1;
        const bool matched = (ruleDir == actDir) || (actDir == 0 && factor > 1.0 && e.actualLimitUp >= lastUp);

        for (AssistantRule& r : rules) {
            if (r.id != id) continue;
            RuleUpdate u;
            u.id = r.id;
            u.name = r.name;
            u.totalBefore = r.total;
            u.hitsBefore = r.hits;
            u.weightBefore = r.weight;

            ++r.total;
            if (matched) ++r.hits;
            // Laplace 平滑：小样本不会因为一次命中就权重爆表
            r.weight = qBound(0.20, (r.hits + 2.0) / (r.total + 4.0) * 1.6, 1.40);
            if (r.total >= 8 && r.accuracy() < 0.45 && r.active) {
                r.active = false;
                u.deactivated = true;
            }
            u.hitsAfter = r.hits;
            u.totalAfter = r.total;
            u.weightAfter = r.weight;
            u.reason = matched
                ? QStringLiteral("方向命中（%1）").arg(e.actualLimitUp >= lastUp ? QStringLiteral("扩张") : QStringLiteral("收缩"))
                : QStringLiteral("方向相反（实际%1）").arg(e.actualLimitUp >= lastUp ? QStringLiteral("扩张") : QStringLiteral("收缩"));
            updates.append(u);
            break;
        }
    }
    return updates;
}

double ReviewEngine::updateBias(double bias, const Forecast& f, const ForecastEval& e)
{
    if (!f.valid || !e.valid || f.limitUpCenter <= 0) return bias;
    const double ratio = double(qMax(1, e.actualLimitUp)) / double(f.limitUpCenter);
    const double err = qBound(-0.4, std::log(ratio), 0.4);
    return qBound(-0.45, 0.7 * bias + 0.3 * err, 0.45);
}

void ReviewEngine::appendEvalSection(ReviewReport& report, const ForecastEval& e,
                                     const QVector<RuleUpdate>& updates)
{
    if (!e.valid) return;
    QString h = QStringLiteral("<hr style=\"border:none;border-top:1px solid #e2e6ed;margin:16px 0\"/>");
    h += sectionTitle(QStringLiteral("昨日预判复核"));
    h += QStringLiteral("<p style=\"margin:0 0 8px 0;font-size:13px\">得分 <b>%1</b> / 100 ｜ %2</p>")
             .arg(e.score)
             .arg(e.hit ? QStringLiteral("判断有效，思路保留") : QStringLiteral("存在偏差，已调整规则权重"));
    if (!e.deviations.isEmpty()) {
        h += QStringLiteral("<ul style=\"margin:4px 0 10px 18px;padding:0;font-size:13px;color:#8a3b12\">");
        for (const QString& d : e.deviations) h += QStringLiteral("<li>%1</li>").arg(esc(d));
        h += QStringLiteral("</ul>");
    }
    if (!updates.isEmpty()) {
        h += QStringLiteral("<p style=\"margin:8px 0 4px 0;font-size:13px;color:#4a5260\">规则权重调整：</p>");
        h += QStringLiteral("<ul style=\"margin:4px 0 10px 18px;padding:0;font-size:13px\">");
        for (const RuleUpdate& u : updates) {
            h += QStringLiteral("<li>%1：权重 %2 → %3（累计命中 %4/%5）%6</li>")
                     .arg(esc(u.name))
                     .arg(num(u.weightBefore, 2), num(u.weightAfter, 2))
                     .arg(u.hitsAfter).arg(u.totalAfter)
                     .arg(u.deactivated ? QStringLiteral("，已停用") : QString());
        }
        h += QStringLiteral("</ul>");
    }
    const int pos = report.html.lastIndexOf(QStringLiteral("</div>"));
    if (pos >= 0) report.html.insert(pos, h);
}

// ---------------------------------------------------------------- 序列化
QJsonObject ReviewEngine::toJson(const ReviewReport& r)
{
    QJsonObject o;
    o["date"] = r.date.toString(Qt::ISODate);
    o["headline"] = r.headline;
    o["html"] = r.html;
    o["text"] = r.text;
    o["facts"] = r.facts;
    return o;
}

ReviewReport ReviewEngine::reviewFromJson(const QJsonObject& o)
{
    ReviewReport r;
    r.date = QDate::fromString(o.value(QStringLiteral("date")).toString(), Qt::ISODate);
    r.headline = o.value(QStringLiteral("headline")).toString();
    r.html = o.value(QStringLiteral("html")).toString();
    r.text = o.value(QStringLiteral("text")).toString();
    r.facts = o.value(QStringLiteral("facts")).toObject();
    r.valid = r.date.isValid() && !r.facts.isEmpty();
    return r;
}

QJsonObject ReviewEngine::toJson(const Forecast& f)
{
    QJsonObject o;
    o["forDate"] = f.forDate.toString(Qt::ISODate);
    o["madeAt"] = f.madeAt.toString(Qt::ISODate);
    o["stance"] = f.stance;
    o["limitUpLow"] = f.limitUpLow;
    o["limitUpHigh"] = f.limitUpHigh;
    o["limitUpCenter"] = f.limitUpCenter;
    o["maxBoardLow"] = f.maxBoardLow;
    o["maxBoardHigh"] = f.maxBoardHigh;
    o["advanceLow"] = f.advanceLow;
    o["advanceHigh"] = f.advanceHigh;
    o["capPct"] = f.capPct;
    o["themes"] = QJsonArray::fromStringList(f.themes);
    o["playbook"] = QJsonArray::fromStringList(f.playbook);
    o["risks"] = QJsonArray::fromStringList(f.risks);
    o["html"] = f.html;
    o["confidence"] = f.confidence;
    o["trace"] = f.trace;
    return o;
}

Forecast ReviewEngine::forecastFromJson(const QJsonObject& o)
{
    Forecast f;
    f.forDate = QDate::fromString(o.value(QStringLiteral("forDate")).toString(), Qt::ISODate);
    f.madeAt = QDate::fromString(o.value(QStringLiteral("madeAt")).toString(), Qt::ISODate);
    f.stance = o.value(QStringLiteral("stance")).toString();
    f.limitUpLow = o.value(QStringLiteral("limitUpLow")).toInt();
    f.limitUpHigh = o.value(QStringLiteral("limitUpHigh")).toInt();
    f.limitUpCenter = o.value(QStringLiteral("limitUpCenter")).toInt();
    f.maxBoardLow = o.value(QStringLiteral("maxBoardLow")).toInt();
    f.maxBoardHigh = o.value(QStringLiteral("maxBoardHigh")).toInt();
    f.advanceLow = o.value(QStringLiteral("advanceLow")).toDouble();
    f.advanceHigh = o.value(QStringLiteral("advanceHigh")).toDouble();
    f.capPct = o.value(QStringLiteral("capPct")).toDouble();
    for (const QJsonValue& v : o.value(QStringLiteral("themes")).toArray()) f.themes << v.toString();
    for (const QJsonValue& v : o.value(QStringLiteral("playbook")).toArray()) f.playbook << v.toString();
    for (const QJsonValue& v : o.value(QStringLiteral("risks")).toArray()) f.risks << v.toString();
    f.html = o.value(QStringLiteral("html")).toString();
    f.confidence = o.value(QStringLiteral("confidence")).toDouble(0.5);
    f.trace = o.value(QStringLiteral("trace")).toObject();
    f.valid = f.forDate.isValid() && !f.stance.isEmpty();
    return f;
}

QJsonObject ReviewEngine::toJson(const ForecastEval& e)
{
    QJsonObject o;
    o["forDate"] = e.forDate.toString(Qt::ISODate);
    o["score"] = e.score;
    o["hit"] = e.hit;
    o["actualLimitUp"] = e.actualLimitUp;
    o["actualMaxBoard"] = e.actualMaxBoard;
    o["actualAdvance"] = e.actualAdvance;
    o["directionHit"] = e.directionHit;
    o["deviations"] = QJsonArray::fromStringList(e.deviations);
    o["lessons"] = QJsonArray::fromStringList(e.lessons);
    o["html"] = e.html;
    return o;
}

ForecastEval ReviewEngine::evalFromJson(const QJsonObject& o)
{
    ForecastEval e;
    e.forDate = QDate::fromString(o.value(QStringLiteral("forDate")).toString(), Qt::ISODate);
    e.score = o.value(QStringLiteral("score")).toInt();
    e.hit = o.value(QStringLiteral("hit")).toBool();
    e.actualLimitUp = o.value(QStringLiteral("actualLimitUp")).toInt();
    e.actualMaxBoard = o.value(QStringLiteral("actualMaxBoard")).toInt();
    e.actualAdvance = o.value(QStringLiteral("actualAdvance")).toDouble();
    e.directionHit = o.value(QStringLiteral("directionHit")).toInt();
    for (const QJsonValue& v : o.value(QStringLiteral("deviations")).toArray()) e.deviations << v.toString();
    for (const QJsonValue& v : o.value(QStringLiteral("lessons")).toArray()) e.lessons << v.toString();
    e.html = o.value(QStringLiteral("html")).toString();
    e.valid = e.forDate.isValid();
    return e;
}
