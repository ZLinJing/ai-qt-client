#include "data/ConceptSource.h"          // 本模块声明

#include <QDateTime>                     // 记录拉取时间
#include <QJsonArray>                    // diff 数组
#include <QJsonDocument>                 // JSON 解析
#include <QJsonObject>                   // 对象节点
#include <algorithm>                     // std::sort

// 实测：push2.eastmoney.com 的 https 本机被断连，82.push2 的 http 可用；突发并发会被限流
static const char* kApiHost = "http://82.push2.eastmoney.com";

// 东财「概念板块」混着统计类/资金属性类板块（如「昨日首板」），会顶掉真正的题材主线，必须剔除
// 声明在 ConceptSource.h：板块资金流排行榜复用同一份口径
bool isPseudoTheme(const QString& name)
{
    static const QStringList kBlacklist = {      // 伪题材关键词黑名单
        QStringLiteral("昨日"), QStringLiteral("融资融券"), QStringLiteral("转融券"),   // 统计类
        QStringLiteral("沪股通"), QStringLiteral("深股通"), QStringLiteral("MSCI"),     // 通道/指数类
        QStringLiteral("富时"), QStringLiteral("标普"), QStringLiteral("道琼斯"),        // 海外指数
        QStringLiteral("机构重仓"), QStringLiteral("基金重仓"), QStringLiteral("社保重仓"),  // 持仓属性
        QStringLiteral("QFII"), QStringLiteral("预亏"), QStringLiteral("预盈"),          // 资金/业绩属性
        QStringLiteral("破净"), QStringLiteral("高送转"), QStringLiteral("送转"),        // 财务属性
        QStringLiteral("次新股"), QStringLiteral("新股"), QStringLiteral("低价股"),      // 股本属性
        QStringLiteral("高价股"), QStringLiteral("百元股"), QStringLiteral("举牌"),      // 价格/事件属性
        QStringLiteral("股权转让"), QStringLiteral("转债标的"), QStringLiteral("ST板块"), // 事件属性
        QStringLiteral("大盘"), QStringLiteral("中盘"), QStringLiteral("小盘"),          // 市值分类
        QStringLiteral("B股"), QStringLiteral("AH股"), QStringLiteral("GDR"),           // 交易品种
        QStringLiteral("同花顺"), QStringLiteral("东方财富"), QStringLiteral("融资"),     // 平台/资金类
        QStringLiteral("重仓"), QStringLiteral("成份"), QStringLiteral("成分"),          // 指数成分
        QStringLiteral("指数"), QStringLiteral("概念股"), QStringLiteral("两市"),        // 泛称
    };
    for (const QString& k : kBlacklist)          // 逐个关键词匹配
        if (name.contains(k)) return true;       // 命中即判为伪题材
    return false;                                // 未命中则为真题材
}

ConceptSource::ConceptSource(HttpClient* http, QObject* parent)
    : QObject(parent), m_http(http)              // 记录外部注入的取数器
{
    m_pump = new QTimer(this);                   // 匀速泵定时器
    m_pump->setInterval(140);                    // 每 140ms 发一个成分股请求
    connect(m_pump, &QTimer::timeout, this, &ConceptSource::pumpMembers);  // 到点泵一个
}

// 拉取题材：先清状态，再请求概念板块列表
void ConceptSource::fetchThemes(int topN)
{
    m_themesOfStock.clear();                     // 清空代码→题材映射
    m_themeChange.clear();                       // 清空题材→涨幅
    m_boards.clear();                            // 清空板块列表
    m_pending = 0;                               // 重置未回包计数
    m_ready = false;                             // 标记未就绪

    const QString url = QStringLiteral(          // pz=600 覆盖全部概念板块，按涨幅降序
        "%1/api/qt/clist/get"
        "?pn=1&pz=600&po=1&np=1&fltt=2&invt=2&fid=f3&fs=m:90+t:3"
        "&fields=f12,f14,f3").arg(QString::fromLatin1(kApiHost));
    m_http->get(url, {{"Referer", "http://quote.eastmoney.com/"}}, QStringLiteral("utf-8"),
        [this, topN](const QByteArray& raw) { onBoardList(raw, topN); },   // 成功则解析板块
        [this](const QString& err) {                                       // 失败则上报
            Q_EMIT failed(QStringLiteral("[概念板块] 拉取失败：%1").arg(err));
        });
}

// 解析板块列表 → 过滤伪题材 → 按涨幅排序取前 topN → 启动匀速泵
void ConceptSource::onBoardList(const QByteArray& raw, int topN)
{
    QJsonParseError err;                         // 解析错误
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);  // 解析 JSON
    if (err.error != QJsonParseError::NoError) {  // 解析失败
        Q_EMIT failed(QStringLiteral("[概念板块] JSON 解析失败"));
        return;                                  // 结束
    }
    const QJsonArray diff = doc.object().value("data").toObject().value("diff").toArray();  // 板块数组
    if (diff.isEmpty()) {                        // 空数组说明接口可能变更
        Q_EMIT failed(QStringLiteral("[概念板块] 返回为空（接口可能已变更）"));
        return;                                  // 结束
    }

    for (const QJsonValue& v : diff) {           // 逐板块解析
        const QJsonObject o = v.toObject();      // 单板块对象
        Board b;                                 // 单板块结构
        b.code = o.value("f12").toString();      // 板块代码
        b.name = o.value("f14").toString();      // 板块名称
        b.changePct = o.value("f3").toDouble();  // 板块涨幅
        if (b.code.startsWith(QStringLiteral("BK")) && !b.name.isEmpty()   // 代码以 BK 开头且有名
            && !isPseudoTheme(b.name))           // 且不是统计类伪题材
            m_boards.append(b);                  // 收录
    }
    std::sort(m_boards.begin(), m_boards.end(), [](const Board& a, const Board& b) {
        return a.changePct > b.changePct;        // 按涨幅从高到低排序
    });
    if (m_boards.size() > topN) m_boards.resize(topN);  // 只保留涨幅前 topN

    if (m_boards.isEmpty()) {                    // 过滤后为空
        Q_EMIT failed(QStringLiteral("[概念板块] 未解析到有效板块"));
        return;                                  // 结束
    }

    m_pending = m_boards.size();                 // 待回包的成分股请求数
    m_nextIndex = 0;                             // 从第一个板块开始
    m_pump->start();     // 匀速发出，避免被东财直接断连
}

// 泵：按节奏逐个发出成分股请求，发完即停
void ConceptSource::pumpMembers()
{
    if (m_nextIndex >= m_boards.size()) {        // 全部已发出
        m_pump->stop();                          // 停泵
        return;                                  // 结束
    }
    requestMembers(m_nextIndex++);               // 发出下一个请求
}

// 请求单个板块的成分股，并把「代码→题材」写回映射
void ConceptSource::requestMembers(int index)
{
    if (index < 0 || index >= m_boards.size()) return;  // 下标非法则忽略
    const Board b = m_boards.at(index);          // 目标板块
    const QString url = QStringLiteral(
        "%1/api/qt/clist/get"
        "?pn=1&pz=500&po=1&np=1&fltt=2&invt=2&fid=f3&fs=b:%2&fields=f12,f14")
        .arg(QString::fromLatin1(kApiHost), b.code);  // 以板块代码为过滤条件

    m_http->get(url, {{"Referer", "http://quote.eastmoney.com/"}}, QStringLiteral("utf-8"),
        [this, b](const QByteArray& raw) {       // 成功回调
            const QJsonDocument doc = QJsonDocument::fromJson(raw);  // 解析 JSON
            const QJsonArray diff = doc.object().value("data").toObject().value("diff").toArray();  // 成分股
            for (const QJsonValue& v : diff) {   // 逐只成分股
                const QString code = v.toObject().value("f12").toString();  // 股票代码
                if (code.size() != 6) continue;  // 非 6 位代码跳过
                m_themesOfStock[code].append(b.name);  // 记录该股票所属题材
            }
            m_themeChange[b.name] = b.changePct; // 记录题材涨幅
            if (--m_pending <= 0) {              // 全部板块回齐
                m_ready = true;                  // 标记就绪
                m_fetchedAt = QDateTime::currentDateTime();  // 记录完成时间
                Q_EMIT themesReady(m_themesOfStock, m_themeChange);  // 广播结果
            }
        },
        [this](const QString&) {                 // 失败回调：同样要递减计数
            if (--m_pending <= 0) {              // 全部板块都有了结果
                m_ready = !m_themesOfStock.isEmpty();  // 有数据才算就绪
                m_fetchedAt = QDateTime::currentDateTime();  // 记录时间
                if (m_ready) Q_EMIT themesReady(m_themesOfStock, m_themeChange);  // 部分成功可用
                else Q_EMIT failed(QStringLiteral("[概念板块] 成分股全部拉取失败"));  // 全失败才报错
            }
        });
}
