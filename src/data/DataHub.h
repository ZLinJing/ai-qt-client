#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 领域模型
#include "data/HttpClient.h"                     // 统一取数器
#include "data/LimitPoolSource.h"                // 涨停池数据源
#include "data/KlineSource.h"                    // 日 K 数据源
#include "data/SnapshotSource.h"                 // 全市场快照源
#include "data/ConceptSource.h"                  // 概念题材源
#include "data/StockDetailTypes.h"               // 个股详情补充数据（财报/资金流/机构持仓）
#include <QObject>                               // 基类（信号槽）
#include <QTimer>                                // （预留）定时刷新
#include <QHash>                                 // 内存缓存索引
#include <QSet>                                  // 详情请求防重
#include <QDateTime>                             // 缓存时间戳

// DataHub：数据层统一门面
// 向下聚合 Http/LimitPool/Kline/Snapshot/Concept 各源；向上以信号广播
// 内存缓存：K 线按代码缓存（TTL 1 小时）、快照 60 秒、题材 15 分钟、图表 K 线 30 分钟

class StockDetailSource;

class DataHub : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit DataHub(QObject* parent = nullptr); // 构造：建各数据源并连线
    ~DataHub() override;                         // 析构：默认实现

    void init();                                 // 记录当前交易日

    void refreshAll(const QDate& date);                          // 手动触发一次完整刷新
    void refreshLimitPool(const QDate& date);                    // 刷新涨停/炸板/跌停池
    void refreshSnapshot();                                      // 刷新全市场快照
    void refreshKlines(const QStringList& codes, int days = 120); // 刷新筛选用日 K（带 1 小时缓存）
    // 图表专用长历史 K 线：独立源 + 独立缓存，不写入 m_klineCache / 不发 klineUpdated
    void refreshChartKline(const QString& code, int days = 480, bool force = false);
    const KlineSeries* chartKline(const QString& code) const;    // 取图表缓存

    // 个股详情补充数据：按需拉取（财报 / 资金流 / 机构持仓），10 分钟缓存
    void refreshStockDetail(const QString& code, bool force = false);
    const StockDetailData* stockDetail(const QString& code) const;

    void refreshPrevLimitPool(const QDate& date);                // 补历史交易日涨停池（算晋级率/溢价）

    void refreshThemes(bool force = false);                      // 刷新概念题材（TTL 15 分钟）
    const QHash<QString, QStringList>& themesOfStock() const { return m_themesOfStock; }      // 代码→题材
    const QHash<QString, double>& themeChangePct() const { return m_themeChangePct; }         // 题材→涨幅
    bool themesReady() const { return !m_themesOfStock.isEmpty(); }                           // 题材是否可用

    const QVector<LimitEntry>& limitUp() const { return m_limitUp; }              // 涨停池
    const QVector<LimitEntry>& brokenPool() const { return m_broken; }            // 炸板池
    const QVector<LimitEntry>& limitDownPool() const { return m_limitDown; }      // 跌停池
    const QVector<LimitEntry>& prevLimitUp() const { return m_prevLimitUp; }      // 昨日涨停池
    QDate prevTradeDate() const { return m_prevDate; }                            // 昨日日期
    const QHash<QString, Snapshot>& snapshots() const { return m_snapshotIndex; } // 快照索引
    const KlineSeries* kline(const QString& code) const;                          // 取筛选用 K 线

    bool isTradingTime() const;                  // 当前是否处于交易时段（含交易日历）
    QDate currentTradeDate() const;              // 当前交易日
    QDateTime lastSuccessAt() const { return m_lastSuccessAt; }  // 最近一次成功取到涨停池的时间
    bool isDataStale(int maxMinutes = 10) const; // 行情是否陈旧（据此禁止给出可执行结论）

Q_SIGNALS:
    void limitPoolUpdated(const QDate& date, const QVector<LimitEntry>& limitUp,   // 三池更新
                          const QVector<LimitEntry>& broken, const QVector<LimitEntry>& limitDown);
    void snapshotUpdated(const QVector<Snapshot>& list);            // 快照更新
    void klineUpdated(const QString& code, const KlineSeries& series);  // 筛选用 K 线更新
    void chartKlineReady(const QString& code, const KlineSeries& series);  // 图表用长历史 K 线就绪
    void stockDetailReady(const QString& code, const StockDetailData& data); // 详情补充数据就绪
    void prevLimitPoolUpdated(const QDate& date, const QVector<LimitEntry>& limitUp);  // 昨日涨停池就绪
    void themesUpdated(const QHash<QString, QStringList>& themesOfStock,   // 题材映射更新
                       const QHash<QString, double>& themeChangePct);
    void statusMessage(const QString& text);                        // 状态文案（UI 直接展示）
    void errorOccurred(const QString& message);                      // 错误通知

private slots:
    void onPoolReady(const QDate& date, const QVector<LimitEntry>& up,      // 三池就绪处理
                     const QVector<LimitEntry>& broken, const QVector<LimitEntry>& down);
    void onSnapshotReady(const QVector<Snapshot>& list);                    // 全量快照就绪
    void onPartialSnapshot(const QVector<Snapshot>& list);                  // 分批快照就绪
    void onKlineReady(const QString& code, const KlineSeries& series);      // 单只 K 线就绪
    void onPoolFailed(const QString& msg);                                  // 数据源失败

private:
    HttpClient* m_http = nullptr;                // 共享取数器
    LimitPoolSource* m_poolSrc = nullptr;        // 当日池数据源
    LimitPoolSource* m_prevPoolSrc = nullptr;   // 历史涨停池（独立实例，避免覆盖当日状态）
    KlineSource* m_klineSrc = nullptr;           // 筛选用日 K 源
    KlineSource* m_chartKlineSrc = nullptr;   // 图表长历史（独立实例，避免与筛选日 K 互相覆盖）
    SnapshotSource* m_snapSrc = nullptr;         // 全市场快照源
    ConceptSource* m_conceptSrc = nullptr;       // 概念题材源
    StockDetailSource* m_detailSrc = nullptr;    // 个股详情补充数据源（按需）

    QVector<LimitEntry> m_limitUp;               // 涨停池缓存
    QVector<LimitEntry> m_broken;                // 炸板池缓存
    QVector<LimitEntry> m_limitDown;             // 跌停池缓存
    QVector<LimitEntry> m_prevLimitUp;     // 昨日涨停池
    QDate m_prevDate;                            // 昨日交易日
    QHash<QString, Snapshot> m_snapshotIndex;    // 代码 → 快照
    QHash<QString, KlineSeries> m_klineCache;    // 代码 → 筛选用 K 线
    QHash<QString, KlineSeries> m_chartKlineCache;  // 代码 → 图表用长历史 K 线
    QHash<QString, QDateTime> m_chartKlineTime;     // 代码 → 图表缓存时间
    QHash<QString, StockDetailData> m_detailCache;  // 代码 → 详情补充数据
    QHash<QString, QDateTime> m_detailTime;         // 代码 → 详情缓存时间
    QSet<QString> m_detailInFlight;                 // 正在拉取的详情请求
    QHash<QString, QStringList> m_themesOfStock; // 代码 → 题材列表
    QHash<QString, double> m_themeChangePct;     // 题材 → 涨幅
    QDateTime m_themeTime;                       // 题材缓存时间
    QHash<QString, QDateTime> m_klineTime;       // 代码 → 筛选 K 线缓存时间
    QDateTime m_snapshotTime;                    // 快照缓存时间
    QDate m_tradeDate;                           // 当前交易日
    QDateTime m_lastSuccessAt;                   // 最近一次成功取到涨停池的时间
};
