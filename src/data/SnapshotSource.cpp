#include "data/SnapshotSource.h"         // 本模块声明
#include "data/TextCodec.h"              // UTF-8 / GBK 解码
#include <QJsonDocument>                 // JSON 解析
#include <QJsonArray>                    // 数组容器
#include <QJsonObject>                   // 单只对象
#include <QRegularExpression>            // 修复无引号 key
#include <QDebug>                        // 数据异常告警
#include <initializer_list>              // 多候选字段名

SnapshotSource::SnapshotSource(HttpClient* http, QObject* parent) : QObject(parent), m_http(http) {}  // 记录取数器

// 数字/字符串兼容取值：新浪同字段类型不稳定，逐候选尝试
static double num(const QJsonObject& o, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {                 // 按候选字段名依次尝试
        const QJsonValue v = o.value(QLatin1String(k));  // 取字段值
        if (v.isDouble()) return v.toDouble();   // 数字直接返回
        if (v.isString()) {                      // 字符串则尝试转数字
            bool ok = false;                     // 转换是否成功
            const double d = v.toString().toDouble(&ok);  // 字符串转 double
            if (ok) return d;                    // 成功则返回
        }
    }
    return 0.0;                                  // 全部失败返回 0
}

// 解析新浪 getHQNodeData 返回的数组
static QVector<Snapshot> parseSinaArray(const QByteArray& raw)
{
    QVector<Snapshot> out;                       // 解析结果
    QString text = TextCodec::fromUtf8(raw).trimmed();  // 按 UTF-8 解码并去空白
    if (text.isEmpty()) return out;              // 空响应直接返回

    static const QRegularExpression keyFix(R"(\{([{,])(\w+)\s*:)");  // 匹配无引号 key
    QString fixed;                               // 修复后的文本
    int pos = 0;                                 // 扫描位置
    while (pos < text.size()) {                  // 逐段替换
        const QRegularExpressionMatch m = keyFix.match(text, pos);  // 找下一个无引号 key
        if (!m.hasMatch()) { fixed.append(text.mid(pos)); break; }  // 没有了则拷贝剩余
        fixed.append(text.mid(pos, m.capturedStart(0) - pos));      // 拷贝匹配前的内容
        fixed.append(m.captured(1));             // 保留 { 或 ,
        fixed.append(QStringLiteral("\"") + m.captured(2) + QStringLiteral("\":"));  // 给 key 补引号
        pos = m.capturedEnd(0);                  // 继续向后扫
    }
    if (!fixed.startsWith('[')) fixed = QStringLiteral("[") + fixed;  // 补数组起始符
    if (!fixed.endsWith(']')) fixed += QStringLiteral("]");           // 补数组结束符

    QJsonParseError err;                         // 解析错误
    QJsonDocument doc = QJsonDocument::fromJson(fixed.toUtf8(), &err);  // 解析修复后的 JSON
    if (err.error != QJsonParseError::NoError) return out;  // 仍失败则放弃本页

    QJsonArray arr = doc.array();                // 取数组
    int dropped = 0;                            // 异常记录计数（汇总告警，避免刷屏）
    for (const QJsonValue& v : arr) {            // 逐只解析
        QJsonObject o = v.toObject();            // 单只对象
        Snapshot s;                              // 单只快照
        s.symbol = o.value("symbol").toString(); // 带市场前缀的代码
        s.code   = o.value("code").toString();   // 6 位代码
        s.name   = o.value("name").toString();   // 名称
        s.price  = num(o, { "trade", "price", "now" });              // 最新价（多字段名兼容）
        s.open   = num(o, { "open" });                                // 今开
        s.high   = num(o, { "high" });                                // 最高
        s.low    = num(o, { "low" });                                 // 最低
        s.prevClose = num(o, { "settlement", "prevclose", "yestclose" });  // 昨收
        s.changePct = num(o, { "changepercent", "change" });          // 涨跌幅
        s.volume = num(o, { "volume" });                              // 成交量
        s.amount = num(o, { "amount" });                              // 成交额
        s.turnover = num(o, { "turnoverratio", "turnover" });         // 换手率
        s.pe = num(o, { "per", "pe" });                               // 市盈率
        s.pb = num(o, { "pb" });                                      // 市净率
        s.totalCap = num(o, { "mktcap" }) * 10000.0;                  // 总市值：万元 → 元
        s.floatCap = num(o, { "nmc" }) * 10000.0;                     // 流通市值：万元 → 元

        if (s.price <= 0.0 && s.prevClose > 0.0)                      // 缺现价则用昨收+涨跌幅反推
            s.price = s.prevClose * (1.0 + s.changePct / 100.0);
        if (s.prevClose <= 0.0 && s.price > 0.0 && s.changePct > -100.0)  // 缺昨收则反向推算
            s.prevClose = s.price / (1.0 + s.changePct / 100.0);

        s.board = boardOfCode(s.code);           // 由代码推断板块

        // 数据质量断言：现价/昨收/涨跌幅异常会直接污染溢价率与持仓浮动盈亏
        const bool sane = !s.code.isEmpty()
                       && s.price > 0.0 && s.price < 10000.0
                       && s.prevClose > 0.0
                       && s.changePct > -35.0 && s.changePct < 35.0
                       && s.floatCap >= 0.0;
        if (sane) out.append(s);
        else ++dropped;
    }
    if (dropped > 0) {
        qWarning().noquote()
            << QStringLiteral("[数据源] 全市场快照丢弃 %1 条异常记录（本页 %2 条）")
                   .arg(dropped).arg(arr.size());
    }
    return out;                                  // 返回解析结果
}

// 拉取全市场：按页并发（每页 100 只，60 页覆盖 6000 只）
void SnapshotSource::fetchAll(const QString& node)
{
    m_accum.clear();                             // 清空上轮累积
    m_codeIndex.clear();                         // 清空索引
    m_received = 0;                              // 重置回包计数
    m_pages = 60;                                // 计划页数
    m_lastPages = m_pages;                       // 对外记录页数
    for (int p = 1; p <= m_pages; ++p) requestPage(node, p, m_pages);  // 逐页发起请求
}

// 批量拉取指定代码：走新浪单只快照接口
void SnapshotSource::fetchCodes(const QStringList& codes)
{
    if (codes.isEmpty()) return;                 // 无代码直接返回
    const int batch = 40;                        // 分批大小（接口限制约 60 只/次）
    for (int i = 0; i < codes.size(); i += batch) {  // 逐批处理
        QStringList batchCodes = codes.mid(i, batch); // 当前批代码
        QStringList symbols;                      // 带市场前缀的代码
        for (const QString& c : batchCodes) {     // 逐个加前缀
            const QChar ch = c.at(0);             // 首位判市场
            symbols << ((ch == QChar('6') || ch == QChar('9') || ch == QChar('5'))
                            ? QStringLiteral("sh") + c : QStringLiteral("sz") + c);
        }
        const QString url = QStringLiteral(
            "https://hq.sinajs.cn/list=%1").arg(symbols.join(','));  // 拼接请求地址
        m_http->get(url, {{"Referer", "https://finance.sina.com.cn/"}}, QStringLiteral("gbk"),  // 该接口返回 GBK
            [this](const QByteArray& raw) {       // 成功回调
                const QString text = TextCodec::fromGbk(raw);  // 按 GBK 解码
                QVector<Snapshot> out;            // 本批结果
                for (const QString& line : text.split('\n')) {  // 逐行解析（一行一只票）
                    const int q1 = line.indexOf('=');          // 等号位置
                    const int q2 = line.lastIndexOf('"');      // 最后一个引号位置
                    if (q1 < 0 || q2 <= q1) continue;          // 结构不符则跳过
                    const QString sym = line.mid(q1 + 1, q2 - q1 - 1);  // 引号内的 symbol
                    const QStringList f = line.mid(q2 + 1).split(',');  // 引号后的字段序列
                    if (f.size() < 10) continue;               // 字段不足跳过
                    Snapshot s;                    // 单只快照
                    s.symbol = sym;                             // 带前缀代码
                    s.code = sym.size() >= 8 ? sym.mid(2, 6) : sym;  // 去掉前缀
                    s.name = f[0];                              // 名称
                    s.open = f[1].toDouble();                   // 今开
                    s.prevClose = f[2].toDouble();              // 昨收
                    s.price = f[3].toDouble();                  // 现价
                    s.high = f[4].toDouble();                   // 最高
                    s.low = f[5].toDouble();                    // 最低
                    s.volume = f[8].toDouble();                 // 成交量
                    s.amount = f[9].toDouble();                 // 成交额
                    if (s.prevClose > 0)                        // 有昨收才计算涨跌幅
                        s.changePct = (s.price / s.prevClose - 1.0) * 100.0;
                    s.board = boardOfCode(s.code);              // 推断板块
                    out.append(s);                              // 收录
                }
                if (!out.isEmpty()) Q_EMIT partialReady(out);   // 本批就绪即广播
            },
            [](const QString&) {});               // 本批失败静默（后续批次仍可成功）
    }
}

// 拉取单页并累积；所有页回齐后广播
void SnapshotSource::requestPage(const QString& node, int page, int totalPages)
{
    const QString url = QStringLiteral(
        "https://vip.stock.finance.sina.com.cn/quotes_service/api/json_v2.php/"
        "Market_Center.getHQNodeData?page=%1&num=100&sort=symbol&asc=1&node=%2")
        .arg(page).arg(node);                    // 页码 + 节点

    m_http->get(url, {{"Referer", "https://finance.sina.com.cn/"}}, QStringLiteral("utf-8"),
        [this, page, totalPages](const QByteArray& raw) {   // 成功回调
            const QVector<Snapshot> part = parseSinaArray(raw);  // 解析本页
            m_accum.append(part);                             // 累加到总表
            for (const Snapshot& s : part) m_codeIndex.insert(s.code, s);  // 同步更新索引
            ++m_received;                                     // 回包计数
            if (m_received >= totalPages) {                   // 全部页回齐
                Q_EMIT snapshotsReady(m_accum);               // 广播全量结果
            }
        },
        [this, page, totalPages](const QString&) {            // 失败回调
            ++m_received;                                     // 失败也计数，避免永远等不到广播
            if (m_received >= totalPages) {                   // 全部页都有结果
                if (!m_accum.isEmpty())                       // 至少部分成功
                    Q_EMIT snapshotsReady(m_accum);           // 广播已拿到的部分
                else
                    Q_EMIT failed(QStringLiteral("全市场快照全部请求失败"));  // 全失败才报错
            }
        });
}
