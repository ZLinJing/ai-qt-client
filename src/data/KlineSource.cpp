#include "data/KlineSource.h"            // 本模块声明
#include <QJsonDocument>                 // JSON 解析
#include <QJsonObject>                   // 根对象
#include <QJsonArray>                    // K 线数组
#include <QStringList>                   // 东财 klnes 字符串切分
#include <functional>                    // std::function 备用通道回调
#include <QDebug>                        // 数据异常告警

KlineSource::KlineSource(HttpClient* http, QObject* parent) : QObject(parent), m_http(http) {}  // 记录取数器

// 6 位代码 → 腾讯 symbol：6/9/5 开头为沪市，其余按深市
QString KlineSource::toSymbol(const QString& code)
{
    if (code.size() != 6) return code;           // 非 6 位代码原样返回
    const QChar c = code.at(0);                  // 取首位判断市场
    if (c == QChar('6') || c == QChar('9') || c == QChar('5'))
        return QStringLiteral("sh") + code;      // 沪市前缀
    return QStringLiteral("sz") + code;          // 深市前缀
}

// symbol → 6 位代码：去掉前两位市场前缀
QString KlineSource::toCode(const QString& symbol)
{
    if (symbol.size() >= 8) return symbol.mid(2, 6);  // 形如 sh600519 → 600519
    return symbol;                               // 长度不足则原样返回
}

namespace {

// 解析腾讯的 [日期, 开, 收, 高, 低, 量, 额] 数组
QVector<Candle> parseArr(const QJsonArray& arr)
{
    QVector<Candle> out;                         // 解析结果
    out.reserve(arr.size());                     // 预留容量
    for (const QJsonValue& v : arr) {            // 逐根解析
        const QJsonArray a = v.toArray();        // 单根数组
        if (a.size() < 6) continue;              // 字段不足则跳过
        Candle c;                                // 单根 K 线
        c.date   = QDate::fromString(a.at(0).toString(), QStringLiteral("yyyy-MM-dd"));  // 日期
        c.open   = a.at(1).toDouble();           // 开盘（腾讯第 2 位）
        c.close  = a.at(2).toDouble();           // 收盘（腾讯第 3 位）
        c.high   = a.at(3).toDouble();           // 最高（腾讯第 4 位）
        c.low    = a.at(4).toDouble();           // 最低（腾讯第 5 位）
        c.volume = a.at(5).toDouble();           // 成交量（手）
        if (a.size() >= 7) c.amount = a.at(6).toDouble();  // 成交额（可能缺失）
        // OHLC 自洽校验：字段顺序变更或脏数据不能污染均线与止损锚点
        const bool sane = c.open > 0.0 && c.close > 0.0 && c.high > 0.0 && c.low > 0.0
                       && c.high >= qMax(c.open, c.close) - 1e-6
                       && c.low  <= qMin(c.open, c.close) + 1e-6
                       && c.volume >= 0.0;
        if (c.date.isValid() && sane) out.append(c);
        else if (c.date.isValid())
            qWarning().noquote() << QStringLiteral("[数据源] K 线 OHLC 不自洽，已丢弃：%1")
                                    .arg(c.date.toString(Qt::ISODate));
    }
    return out;                                  // 返回解析结果
}

} // namespace

// 批量拉取：每只票发两次请求（前复权 + 不复权）
void KlineSource::fetch(const QStringList& codes, int days)
{
    m_total = codes.size();                      // 记录本批总数
    m_ok = 0;                                    // 重置成功计数
    m_fail = 0;                                  // 重置失败计数
    m_pending.clear();                           // 清空上一批状态
    if (codes.isEmpty()) {                       // 空批次
        Q_EMIT allDone(0, 0);                    // 立即通知结束
        return;                                  // 结束
    }

    for (const QString& code : codes) {          // 逐只票发起请求
        m_pending.insert(code, Pending{ 2, KlineSeries() });  // 预期两次回调

        const QString sym = toSymbol(code);      // 转成腾讯 symbol

        // 备用通道：东财 push2his，腾讯接口 501 时保底
        const std::function<void(bool)> fallback = [this, code](bool adjusted) {
            const QChar c0 = code.isEmpty() ? QChar('0') : code.at(0);  // 取首位判市场
            const QString secid = (c0 == QChar('6') || c0 == QChar('9') || c0 == QChar('5'))
                                      ? QStringLiteral("1.") + code     // 沪市 secid 前缀 1.
                                      : QStringLiteral("0.") + code;    // 深市 secid 前缀 0.
            const QString url =
                QStringLiteral("https://push2his.eastmoney.com/api/qt/stock/kline/get"
                               "?secid=%1&klt=101&fqt=%2&lmt=180&end=20500101"
                               "&fields1=f1,f2,f3,f4,f5,f6&fields2=f51,f52,f53,f54,f55,f56,f57")
                    .arg(secid).arg(adjusted ? 1 : 0);  // fqt=1 前复权 / 0 不复权
            m_http->get(url, {{"Referer", "https://quote.eastmoney.com/"}}, QStringLiteral("utf-8"),
                [this, code, adjusted](const QByteArray& raw) {           // 备用通道成功
                    const QJsonObject data =
                        QJsonDocument::fromJson(raw).object().value("data").toObject();  // data 节点
                    QVector<Candle> arr;                              // 解析结果
                    for (const QJsonValue& v : data.value("klines").toArray()) {  // 逐根字符串
                        const QStringList p = v.toString().split(QLatin1Char(','));  // 逗号分隔
                        if (p.size() < 7) continue;                   // 字段不足跳过
                        Candle c;                                     // 单根 K 线
                        c.date = QDate::fromString(p.at(0), QStringLiteral("yyyy-MM-dd"));  // 日期
                        c.open = p.at(1).toDouble();                  // 开盘
                        c.close = p.at(2).toDouble();                 // 收盘
                        c.high = p.at(3).toDouble();                  // 最高
                        c.low = p.at(4).toDouble();                   // 最低
                        c.volume = p.at(5).toDouble();                // 成交量
                        c.amount = p.at(6).toDouble();                // 成交额
                        if (c.date.isValid()) arr.append(c);          // 合法才收录
                    }
                    auto it = m_pending.find(code);                   // 找到该票状态
                    if (it == m_pending.end()) return;                // 已被清理则放弃
                    if (adjusted) {                                   // 前复权分支
                        it->series.code = code;                       // 记录代码
                        it->series.candles = arr;                     // 写入前复权序列
                    } else {
                        it->series.rawCandles = arr;                  // 写入不复权序列
                    }
                    settle(code);                                     // 结算一次回调
                },
                [this, code](const QString&) { settle(code); });       // 备用通道失败也结算
        };

        const auto issue = [this, code, sym, days, fallback](bool adjusted) {  // 发主通道请求
            const QString url = adjusted
                ? QStringLiteral("https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=%1,day,,,%2,qfq")
                      .arg(sym).arg(days)            // 前复权参数带 qfq
                : QStringLiteral("https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=%1,day,,,%2")
                      .arg(sym).arg(days);           // 不复权参数不带 qfq
            m_http->get(url, {{"Referer", "https://gu.qq.com/"}}, QStringLiteral("utf-8"),
                [this, code, adjusted, fallback](const QByteArray& raw) {  // 主通道成功
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);  // 解析 JSON
                    const QJsonObject node =
                        doc.object().value("data").toObject().value(toSymbol(code)).toObject();  // 该票节点
                    QVector<Candle> arr = parseArr(node.value("qfqday").toArray());  // 优先前复权数组
                    if (arr.isEmpty()) arr = parseArr(node.value("day").toArray());  // 回退不复权数组
                    if (arr.isEmpty()) { fallback(adjusted); return; }   // 空响应也走备用通道

                    auto it = m_pending.find(code);          // 找到该票状态
                    if (it == m_pending.end()) return;       // 已清理则放弃
                    if (adjusted) {                          // 前复权分支
                        it->series.code = code;              // 记录代码
                        it->series.candles = arr;            // 写入前复权序列
                    } else {
                        it->series.rawCandles = arr;         // 写入不复权序列
                    }
                    settle(code);                            // 结算一次回调
                },
                [fallback, adjusted](const QString&) { fallback(adjusted); });  // 主通道失败转备用
        };
        issue(true);                             // 请求前复权
        issue(false);                            // 请求不复权
    }
}

// 结算：两次回调都到齐后判断成败并广播
void KlineSource::settle(const QString& code)
{
    auto it = m_pending.find(code);              // 找到该票状态
    if (it == m_pending.end()) return;           // 状态不存在则忽略重复回调
    if (--it->remaining > 0) return;             // 还有回调未到，继续等待

    KlineSeries series = it->series;             // 取出累积结果
    m_pending.erase(it);                         // 移出待回填表，防止重复结算

    if (series.candles.isEmpty() && !series.rawCandles.isEmpty())
        series.candles = series.rawCandles;      // 前复权缺失时用不复权顶替（至少能算均线）

    if (!series.valid()) {                       // 无有效数据
        ++m_fail;                                // 记失败
    } else {
        ++m_ok;                                  // 记成功
        Q_EMIT klineReady(code, series);         // 广播单只结果
    }
    if (m_ok + m_fail >= m_total) Q_EMIT allDone(m_ok, m_fail);  // 整批结束才发一次
}
