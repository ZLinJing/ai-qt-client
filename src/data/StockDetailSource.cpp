#include "data/StockDetailSource.h"
#include "data/HttpClient.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QByteArray>
#include <QMap>
#include <QDate>
#include <QDebug>

namespace {

// secid：沪市 1.xxxxxx，深市/北交所 0.xxxxxx
QString secId(const QString& code)
{
    return (code.startsWith(QLatin1Char('6')) ? QStringLiteral("1.") : QStringLiteral("0.")) + code;
}

// SECUCODE：机构持仓接口按 "600000.SH" 过滤
QString secuCode(const QString& code)
{
    if (code.startsWith(QLatin1Char('6'))) return code + QStringLiteral(".SH");
    if (code.startsWith(QLatin1Char('8')) || code.startsWith(QLatin1Char('4'))
        || code.startsWith(QStringLiteral("920")))
        return code + QStringLiteral(".BJ");
    return code + QStringLiteral(".SZ");
}

QJsonObject parseObject(const QByteArray& raw)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return QJsonObject();
    return doc.object();
}

// datacenter 接口统一取 result.data[0]
QJsonObject firstResultRow(const QByteArray& raw)
{
    const QJsonObject root = parseObject(raw);
    const QJsonArray arr = root.value(QStringLiteral("result")).toObject()
                               .value(QStringLiteral("data")).toArray();
    if (arr.isEmpty() || !arr.at(0).isObject()) return QJsonObject();
    return arr.at(0).toObject();
}

double num(const QJsonObject& o, const QString& key)
{
    const QJsonValue v = o.value(key);
    return v.isDouble() ? v.toDouble() : 0.0;
}

QString str(const QJsonObject& o, const QString& key)
{
    return o.value(key).toString();
}

QMap<QByteArray, QByteArray> jsonHeaders(const QString& referer)
{
    QMap<QByteArray, QByteArray> h;
    h.insert("Referer", referer.toUtf8());
    h.insert("Accept", "application/json, text/plain, */*");
    return h;
}

} // namespace

StockDetailSource::StockDetailSource(HttpClient* http, QObject* parent)
    : QObject(parent), m_http(http)
{
}

void StockDetailSource::fetch(const QString& code)
{
    if (!m_http || code.isEmpty()) return;
    auto job = QSharedPointer<Job>::create();
    job->data.code = code;
    fetchFinance(code, job);
    fetchFlow(code, job);
    fetchOrg(code, job);
}

void StockDetailSource::finish(const QPointer<StockDetailSource>& guard,
                               const QString& code, const QSharedPointer<Job>& job)
{
    if (--job->pending > 0) return;
    job->data.error = job->errors.join(QStringLiteral("；"));
    qInfo() << "[detail] ready" << code
            << "finance" << job->data.finance.valid
            << "flows" << job->data.flows.size()
            << "org" << job->data.org.valid
            << "err" << job->data.error;
    if (guard) Q_EMIT guard->ready(code, job->data);
}

void StockDetailSource::fetchFinance(const QString& code, const QSharedPointer<Job>& job)
{
    const QString url = QStringLiteral(
        "https://datacenter-web.eastmoney.com/api/data/v1/get"
        "?reportName=RPT_LICO_FN_CPD&columns=ALL"
        "&filter=(SECURITY_CODE%3D%22%1%22)&pageNumber=1&pageSize=1"
        "&sortColumns=REPORTDATE&sortTypes=-1").arg(code);

    QPointer<StockDetailSource> guard(this);
    auto done = [guard, code, job]() {
        if (guard) guard->finish(guard, code, job);
    };

    m_http->get(url, jsonHeaders(QStringLiteral("https://data.eastmoney.com/")),
                QStringLiteral("utf-8"),
        [job, done](const QByteArray& raw) {
            const QJsonObject o = firstResultRow(raw);
            if (!o.isEmpty()) {
                FinanceReport& f = job->data.finance;
                f.period = str(o, QStringLiteral("DATATYPE"));
                f.reportDate = str(o, QStringLiteral("REPORTDATE")).left(10);
                f.revenue = num(o, QStringLiteral("TOTAL_OPERATE_INCOME"));
                f.revenueYoy = num(o, QStringLiteral("YSTZ"));
                f.netProfit = num(o, QStringLiteral("PARENT_NETPROFIT"));
                f.netProfitYoy = num(o, QStringLiteral("SJLTZ"));
                f.roe = num(o, QStringLiteral("WEIGHTAVG_ROE"));
                f.valid = !f.reportDate.isEmpty();
            }
            job->data.financeLoaded = true;
            done();
        },
        [job, done](const QString& err) {
            qWarning() << "[detail] finance failed:" << err;
            job->errors << QStringLiteral("财报：%1").arg(err);
            job->data.financeLoaded = true;
            done();
        });
}

void StockDetailSource::fetchFlow(const QString& code, const QSharedPointer<Job>& job)
{
    const QString url = QStringLiteral(
        "https://push2his.eastmoney.com/api/qt/stock/fflow/daykline/get"
        "?lmt=0&klt=101&secid=%1&fields1=f1,f2,f3,f7"
        "&fields2=f51,f52,f53,f54,f55,f56,f57,f58,f59,f60,f61,f62,f63,f64,f65")
        .arg(secId(code));

    QPointer<StockDetailSource> guard(this);
    auto done = [guard, code, job]() {
        if (guard) guard->finish(guard, code, job);
    };

    m_http->get(url, jsonHeaders(QStringLiteral("https://quote.eastmoney.com/")),
                QStringLiteral("utf-8"),
        [job, done](const QByteArray& raw) {
            const QJsonObject root = parseObject(raw);
            const QJsonArray kl = root.value(QStringLiteral("data")).toObject()
                                      .value(QStringLiteral("klines")).toArray();
            for (const QJsonValue& v : kl) {
                const QStringList f = v.toString().split(QLatin1Char(','));
                if (f.size() < 11) continue;
                MoneyFlowDay d;
                d.date = QDate::fromString(f.at(0), QStringLiteral("yyyy-MM-dd"));
                if (!d.date.isValid()) continue;
                d.mainNet     = f.at(1).toDouble();
                d.smallNet    = f.at(2).toDouble();
                d.midNet      = f.at(3).toDouble();
                d.bigNet      = f.at(4).toDouble();
                d.superBigNet = f.at(5).toDouble();
                d.mainPct     = f.at(6).toDouble();
                d.bigPct      = f.at(9).toDouble();
                d.superBigPct = f.at(10).toDouble();
                job->data.flows.append(d);
            }
            const int extra = job->data.flows.size() - 20;
            if (extra > 0) job->data.flows.remove(0, extra);
            job->data.flowLoaded = true;
            done();
        },
        [job, done](const QString& err) {
            qWarning() << "[detail] flow failed:" << err;
            job->errors << QStringLiteral("资金流：%1").arg(err);
            job->data.flowLoaded = true;
            done();
        });
}

void StockDetailSource::fetchOrg(const QString& code, const QSharedPointer<Job>& job)
{
    const QString url = QStringLiteral(
        "https://datacenter-web.eastmoney.com/api/data/v1/get"
        "?reportName=RPT_F10_MAIN_ORGHOLD&columns=ALL"
        "&filter=(SECUCODE%3D%22%1%22)&pageNumber=1&pageSize=2"
        "&sortColumns=REPORT_DATE&sortTypes=-1").arg(secuCode(code));

    QPointer<StockDetailSource> guard(this);
    auto done = [guard, code, job]() {
        if (guard) guard->finish(guard, code, job);
    };

    m_http->get(url, jsonHeaders(QStringLiteral("https://data.eastmoney.com/")),
                QStringLiteral("utf-8"),
        [job, done](const QByteArray& raw) {
            const QJsonObject o = firstResultRow(raw);
            if (!o.isEmpty()) {
                OrgHolding& h = job->data.org;
                h.reportDate = str(o, QStringLiteral("REPORT_DATE")).left(10);
                h.orgNum = int(num(o, QStringLiteral("TOTAL_ORG_NUM")));
                h.orgNumChange = int(num(o, QStringLiteral("TOTAL_ORG_NUM_CHANGE")));
                h.sharesRatio = num(o, QStringLiteral("TOTAL_SHARES_RATIO"));
                h.sharesChange = num(o, QStringLiteral("FREE_SHARES_CHANGE"));
                h.marketCap = num(o, QStringLiteral("TOTAL_MARKET_CAP"));
                h.valid = !h.reportDate.isEmpty();
            }
            job->data.orgLoaded = true;
            done();
        },
        [job, done](const QString& err) {
            qWarning() << "[detail] org failed:" << err;
            job->errors << QStringLiteral("机构持仓：%1").arg(err);
            job->data.orgLoaded = true;
            done();
        });
}
