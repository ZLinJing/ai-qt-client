#include "data/DataHub.h"                // 本模块声明
#include "data/StockDetailSource.h"      // 个股详情补充数据源
#include "core/TradingCalendar.h"        // 交易日历（周末 + 学习到的休市日）
#include <QTime>                         // 交易时段判断
#include <QDateTime>                     // 当前时间与缓存时间戳
#include <QDebug>                        // 详情数据诊断日志

// 构造：创建取数器与各数据源，并把它们的信号接到本类的槽/λ
DataHub::DataHub(QObject* parent) : QObject(parent)
{
    m_http = new HttpClient(this);               // 全模块共享一个取数器
    m_http->setMaxConcurrent(10);                // 全局并发 10
    m_http->setRateLimitPerHost(16);             // 每主机每秒 16 次
    m_http->setRetryCount(2);                    // 失败重试 2 次

    m_poolSrc = new LimitPoolSource(m_http, this);       // 当日三池
    m_prevPoolSrc = new LimitPoolSource(m_http, this);   // 昨日涨停池（独立实例）
    m_klineSrc = new KlineSource(m_http, this);          // 筛选用日 K
    m_chartKlineSrc = new KlineSource(m_http, this);     // 图表用长历史日 K
    m_snapSrc = new SnapshotSource(m_http, this);        // 全市场快照
    m_conceptSrc = new ConceptSource(m_http, this);      // 概念题材
    m_detailSrc = new StockDetailSource(m_http, this);   // 个股详情（按需拉取）

    connect(m_detailSrc, &StockDetailSource::ready, this,
            [this](const QString& code, const StockDetailData& data) {
                m_detailInFlight.remove(code);
                // 部分成功也缓存；全部失败时不缓存，用户下次打开会自动重试
                if (data.finance.valid || !data.flows.isEmpty() || data.org.valid) {
                    m_detailCache.insert(code, data);
                    m_detailTime.insert(code, QDateTime::currentDateTime());
                }
                Q_EMIT stockDetailReady(code, data);
            });

    connect(m_conceptSrc, &ConceptSource::themesReady, this,   // 题材就绪
            [this](const QHash<QString, QStringList>& themes,
                   const QHash<QString, double>& change) {
                m_themesOfStock = themes;            // 存代码→题材
                m_themeChangePct = change;           // 存题材→涨幅
                m_themeTime = QDateTime::currentDateTime();  // 记录缓存时间
                Q_EMIT statusMessage(QStringLiteral("概念题材就绪：%1 只标的 / %2 个热门题材")
                                         .arg(themes.size()).arg(change.size()));  // 状态提示
                Q_EMIT themesUpdated(m_themesOfStock, m_themeChangePct);           // 广播更新
            });
    connect(m_conceptSrc, &ConceptSource::failed, this, [this](const QString& msg) {  // 题材失败
        Q_EMIT statusMessage(msg + QStringLiteral("（板块效应退回行业分类）"));         // 说明已自动降级
    });

    connect(m_poolSrc, &LimitPoolSource::poolsReady, this, &DataHub::onPoolReady);    // 三池就绪
    connect(m_poolSrc, &LimitPoolSource::failed, this, &DataHub::onPoolFailed);       // 三池失败
    connect(m_prevPoolSrc, &LimitPoolSource::partialPoolReady, this,                  // 昨日涨停池就绪
            [this](const QDate& date, const QString& kind, const QVector<LimitEntry>& pool) {
                if (kind != QStringLiteral("ZT")) return;   // 只关心涨停池
                m_prevLimitUp = pool;                       // 缓存昨日涨停池
                m_prevDate = date;                          // 记录昨日日期
                Q_EMIT prevLimitPoolUpdated(date, pool);    // 广播更新
            });
    connect(m_snapSrc, &SnapshotSource::snapshotsReady, this, &DataHub::onSnapshotReady);  // 全量快照
    connect(m_snapSrc, &SnapshotSource::partialReady, this, &DataHub::onPartialSnapshot); // 分批快照
    connect(m_snapSrc, &SnapshotSource::failed, this, &DataHub::onPoolFailed);            // 快照失败
    connect(m_klineSrc, &KlineSource::klineReady, this, &DataHub::onKlineReady);          // 筛选 K 线就绪
    connect(m_chartKlineSrc, &KlineSource::klineReady, this,                              // 图表 K 线就绪
            [this](const QString& code, const KlineSeries& series) {
                if (!series.valid()) return;                       // 无效数据不入缓存
                m_chartKlineCache.insert(code, series);            // 只落图表独立缓存
                m_chartKlineTime.insert(code, QDateTime::currentDateTime());  // 记录缓存时间
                Q_EMIT chartKlineReady(code, series);              // 广播给图表
            });
}

DataHub::~DataHub() = default;                   // 子对象由父子关系自动释放

// 初始化：记录当前交易日
void DataHub::init()
{
    m_tradeDate = currentTradeDate();            // 取当前交易日
}

// 当前交易日：盘中/盘后都归当日，节假日由空池自然识别
QDate DataHub::currentTradeDate() const
{
    return QDate::currentDate();                 // 直接用系统日期
}

// 是否处于交易时段：交易日历判定为交易日，且落在 9:15~11:30 或 13:00~15:05
bool DataHub::isTradingTime() const
{
    if (!TradingCalendar::instance().isTradingDay(QDate::currentDate())) return false;
    const QTime t = QTime::currentTime();
    return (t >= QTime(9, 15) && t <= QTime(11, 30))     // 早盘含集合竞价
        || (t >= QTime(13, 0) && t <= QTime(15, 5));      // 午盘含收盘后 5 分钟
}

// 行情陈旧：超过 maxMinutes 没成功取到涨停池，或从未成功过
bool DataHub::isDataStale(int maxMinutes) const
{
    if (!m_lastSuccessAt.isValid()) return true;
    return m_lastSuccessAt.secsTo(QDateTime::currentDateTime()) > maxMinutes * 60;
}

// 完整刷新：三池 + 全市场快照
void DataHub::refreshAll(const QDate& date)
{
    m_tradeDate = date;                          // 记录交易日
    Q_EMIT statusMessage(QStringLiteral("正在拉取 %1 涨停/炸板/跌停池…").arg(date.toString("yyyy-MM-dd")));  // 状态提示
    refreshLimitPool(date);                      // 拉三池
    refreshSnapshot();                           // 拉快照
}

// 刷新三池
void DataHub::refreshLimitPool(const QDate& date)
{
    m_tradeDate = date;                          // 记录交易日
    m_poolSrc->fetch(date);                      // 交给数据源并发拉取
}

// 刷新全市场快照
void DataHub::refreshSnapshot()
{
    Q_EMIT statusMessage(QStringLiteral("正在拉取全市场快照（沪深 A，约 55 页）…"));  // 状态提示
    m_snapSrc->fetchAll(QStringLiteral("hs_a")); // 只拉沪深 A
}

// 补历史涨停池（只拉涨停池）
// 优先用 getYesterdayZTPool（自带今日涨跌幅，溢价不依赖快照）；为空/失败自动回退 getTopicZTPool 历史模式
void DataHub::refreshPrevLimitPool(const QDate& date)
{
    if (!date.isValid()) return;                 // 日期无效直接返回
    m_prevPoolSrc->fetchYesterdayPool(date);     // 昨日涨停池（含溢价数据）
}

// 刷新概念题材：15 分钟内不重复拉取
void DataHub::refreshThemes(bool force)
{
    if (!force && m_themeTime.isValid()                 // 有缓存时间
        && m_themeTime.secsTo(QDateTime::currentDateTime()) < 15 * 60) {  // 且未超过 15 分钟
        return;   // 15 分钟内不重复拉取
    }
    Q_EMIT statusMessage(QStringLiteral("正在拉取概念题材板块（涨幅前 30）…"));  // 状态提示
    m_conceptSrc->fetchThemes(30);               // 拉涨幅前 30 个题材
}

// 刷新筛选用日 K：只拉缓存缺失或超过 1 小时的代码
void DataHub::refreshKlines(const QStringList& codes, int days)
{
    if (codes.isEmpty()) return;                 // 无代码直接返回
    QStringList need;                            // 需要拉取的代码
    const QDateTime now = QDateTime::currentDateTime();  // 当前时间
    for (const QString& c : codes) {             // 逐个检查缓存
        const QDateTime t = m_klineTime.value(c);        // 该代码的缓存时间
        if (!t.isValid() || t.secsTo(now) > 3600) need.append(c);  // 缺失或超 1 小时才拉
    }
    if (need.isEmpty()) return;                  // 全部命中缓存则不发请求
    Q_EMIT statusMessage(QStringLiteral("正在拉取 %1 只个股日 K（%2 日）…").arg(need.size()).arg(days));  // 状态提示
    m_klineSrc->fetch(need, days);               // 批量拉取
}

// 刷新图表长历史 K 线：30 分钟内命中缓存则直接回调
void DataHub::refreshChartKline(const QString& code, int days, bool force)
{
    if (code.isEmpty()) return;                  // 无代码直接返回
    if (!force) {                                // 非强制时先查缓存
        const QDateTime last = m_chartKlineTime.value(code);  // 图表缓存时间
        if (last.isValid() && last.secsTo(QDateTime::currentDateTime()) < 1800) {  // 30 分钟内
            const auto it = m_chartKlineCache.constFind(code);  // 查缓存
            if (it != m_chartKlineCache.constEnd()) {           // 命中
                Q_EMIT chartKlineReady(code, it.value());       // 直接回调，避免反复打接口
                return;                                         // 结束
            }
        }
    }
    Q_EMIT statusMessage(QStringLiteral("正在拉取 %1 长周期日 K（%2 日）…").arg(code).arg(days));  // 状态提示
    m_chartKlineSrc->fetch(QStringList{ code }, days);  // 走图表专用源
}

// 取图表缓存
const KlineSeries* DataHub::chartKline(const QString& code) const
{
    const auto it = m_chartKlineCache.constFind(code);  // 查缓存
    if (it == m_chartKlineCache.constEnd()) return nullptr;  // 未命中返回空
    return &it.value();                          // 返回只读指针
}

// 详情补充数据：命中 10 分钟缓存时同步回调，避免每次打开弹窗都打三个接口
void DataHub::refreshStockDetail(const QString& code, bool force)
{
    if (code.isEmpty()) return;
    if (!force) {
        const QDateTime last = m_detailTime.value(code);
        if (last.isValid() && last.secsTo(QDateTime::currentDateTime()) < 600) {
            const auto it = m_detailCache.constFind(code);
            if (it != m_detailCache.constEnd()) {
                Q_EMIT stockDetailReady(code, it.value());
                return;
            }
        }
    }
    if (m_detailInFlight.contains(code)) return;   // 同一只票不并发重复拉取
    m_detailInFlight.insert(code);
    m_detailSrc->fetch(code);
}

const StockDetailData* DataHub::stockDetail(const QString& code) const
{
    const auto it = m_detailCache.constFind(code);
    if (it == m_detailCache.constEnd()) return nullptr;
    return &it.value();
}

// 三池就绪：更新缓存并广播
void DataHub::onPoolReady(const QDate& date, const QVector<LimitEntry>& up,
                          const QVector<LimitEntry>& broken, const QVector<LimitEntry>& down)
{
    m_limitUp = up;                              // 缓存涨停池
    m_broken = broken;                           // 缓存炸板池
    m_limitDown = down;                          // 缓存跌停池
    if (!up.isEmpty()) m_lastSuccessAt = QDateTime::currentDateTime();  // 标记数据新鲜
    Q_EMIT statusMessage(QStringLiteral("%1：涨停 %2 家 / 炸板 %3 家 / 跌停 %4 家")
        .arg(date.toString("yyyy-MM-dd")).arg(up.size()).arg(broken.size()).arg(down.size()));  // 状态提示
    Q_EMIT limitPoolUpdated(date, up, broken, down);  // 广播更新
}

// 全量快照就绪：写索引并广播
void DataHub::onSnapshotReady(const QVector<Snapshot>& list)
{
    for (const Snapshot& s : list) m_snapshotIndex.insert(s.code, s);  // 逐只写入索引
    m_snapshotTime = QDateTime::currentDateTime();                     // 记录缓存时间
    Q_EMIT statusMessage(QStringLiteral("全市场快照就绪：%1 只").arg(list.size()));  // 状态提示
    Q_EMIT snapshotUpdated(list);                // 广播更新
}

// 分批快照就绪：只写索引与广播，不改缓存时间
void DataHub::onPartialSnapshot(const QVector<Snapshot>& list)
{
    for (const Snapshot& s : list) m_snapshotIndex.insert(s.code, s);  // 逐只写入索引
    Q_EMIT snapshotUpdated(list);                // 广播本批
}

// 单只 K 线就绪：入缓存并广播
void DataHub::onKlineReady(const QString& code, const KlineSeries& series)
{
    if (!series.valid()) return;                 // 无效数据不入缓存
    m_klineCache.insert(code, series);           // 写入筛选用缓存
    m_klineTime.insert(code, QDateTime::currentDateTime());  // 记录缓存时间
    Q_EMIT klineUpdated(code, series);           // 广播给筛选链路
}

// 数据源失败：转发为统一错误信号
void DataHub::onPoolFailed(const QString& msg)
{
    Q_EMIT errorOccurred(msg);                   // 只做转发
}

// 取筛选用 K 线
const KlineSeries* DataHub::kline(const QString& code) const
{
    auto it = m_klineCache.constFind(code);      // 查缓存
    if (it == m_klineCache.constEnd()) return nullptr;  // 未命中返回空
    return &it.value();                          // 返回只读指针
}
