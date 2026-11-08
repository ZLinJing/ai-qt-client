#include "data/SectorFlowSource.h"

#include "data/ConceptSource.h"          // 复用伪题材黑名单
#include "data/HttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QtGlobal>

namespace {

// 实测：https 的 push2 在本机被断连，82.push2 的 http 镜像可用（与 ConceptSource 一致）
const char* kApiHost = "http://82.push2.eastmoney.com";

// f62 是「主力净流入净额」，单位为元；单个板块超过 5 万亿视为量纲异常，
// 宁可标数据缺失也不把错误单位写进复盘报告。
constexpr double kMaxPlausibleMainNet = 5.0e12;

double numberField(const QJsonObject& o, const char* key, bool* ok)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) { *ok = true; return v.toDouble(); }
    if (v.isString()) {
        bool parsed = false;
        const double d = v.toString().toDouble(&parsed);
        *ok = parsed;
        return d;
    }
    *ok = false;
    return 0.0;
}

} // namespace

SectorFlowSource::SectorFlowSource(QObject* parent)
    : QObject(parent)
{
    m_http = new HttpClient(this);      // 独立取数器：一次请求，不占用选股链路的并发额度
}

QString SectorFlowSource::sourceText()
{
    return QStringLiteral(
        "东方财富 82.push2 /api/qt/clist/get · fs=m:90+t:3（概念板块）· "
        "f3=板块涨跌幅(%) / f62=主力净流入净额(元，展示时 ÷1e8 转亿元) / f184=主力净额占成交额(%)；"
        "主力 = 大单 + 超大单；仅当日快照，字段缺失或接口失败即标「数据缺失」");
}

void SectorFlowSource::fetch()
{
    // fid=f3 按涨幅降序，po=1；pz=600 覆盖全部概念板块，一次请求即可拿到领涨 / 领跌两端
    const QString url = QStringLiteral(
        "%1/api/qt/clist/get"
        "?pn=1&pz=600&po=1&np=1&fltt=2&invt=2&fid=f3&fs=m:90+t:3"
        "&fields=f12,f14,f3,f62,f184").arg(QString::fromLatin1(kApiHost));

    m_http->get(url, {{"Referer", "http://quote.eastmoney.com/"}}, QStringLiteral("utf-8"),
        [this](const QByteArray& raw) { onReply(raw); },
        [this](const QString& err) {
            Q_EMIT failed(QStringLiteral("板块资金流接口请求失败：%1").arg(err));
        });
}

void SectorFlowSource::onReply(const QByteArray& raw)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError) {
        Q_EMIT failed(QStringLiteral("板块资金流 JSON 解析失败（不以缓存涨幅代替资金流）"));
        return;
    }
    const QJsonArray diff = doc.object().value(QStringLiteral("data"))
                               .toObject().value(QStringLiteral("diff")).toArray();
    if (diff.isEmpty()) {
        Q_EMIT failed(QStringLiteral("板块资金流返回为空（接口可能已变更或当日未开盘）"));
        return;
    }

    QVector<DecisionReview::SectorFlowRow> rows;
    int flowOk = 0;
    for (const QJsonValue& v : diff) {
        const QJsonObject o = v.toObject();
        DecisionReview::SectorFlowRow r;
        r.code = o.value(QStringLiteral("f12")).toString();
        r.name = o.value(QStringLiteral("f14")).toString();
        if (!r.code.startsWith(QStringLiteral("BK")) || r.name.isEmpty()
            || isPseudoTheme(r.name)) continue;      // 与概念题材同一份伪题材口径

        bool pctOk = false, netOk = false, mainPctOk = false;
        const double change = numberField(o, "f3", &pctOk);
        const double net = numberField(o, "f62", &netOk);
        const double mainPct = numberField(o, "f184", &mainPctOk);
        if (!pctOk) continue;                       // 涨幅都解析不出，该行不参与领涨 / 领跌排序
        r.changePct = change;
        if (netOk && mainPctOk && qAbs(net) <= kMaxPlausibleMainNet) {
            r.mainNetYi = net / 1.0e8;              // 元 → 亿元
            r.mainPct = mainPct;
            r.flowKnown = true;
            ++flowOk;
        } else {
            r.flowKnown = false;                    // 保留涨幅，但资金流明确标缺失
        }
        rows.append(r);
    }

    if (rows.isEmpty()) {
        Q_EMIT failed(QStringLiteral("过滤伪题材后没有可用板块"));
        return;
    }
    if (flowOk == 0) {
        Q_EMIT failed(QStringLiteral("板块资金流字段 f62 / f184 全部缺失或量纲异常：只展示客观涨跌幅，不推断资金流"));
        return;
    }
    Q_EMIT ready(rows, QDateTime::currentDateTime());
}
