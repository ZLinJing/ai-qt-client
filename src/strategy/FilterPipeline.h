#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 LimitEntry / Candidate / MarketRegime
#include <QObject>                               // 基类（信号：进度）
#include <QHash>                                 // K 线/快照/统计索引

struct SentimentState;   // 前向声明：情绪状态由 sentiment/SentimentEngine 产出

// FilterPipeline：打板选股分层筛选引擎
// 分层结构（避免「三层标准混着用」）：
//   L0 情绪闸门 → L1 通用硬排除 → L2 板块效应 → L3 分层量价 → L4 评分排序
// 每一层淘汰的标的都记录原因，UI 可展开查看「为什么被淘汰」

struct FilterContext {
    QHash<QString, KlineSeries> klines;        // 代码 -> K线
    QHash<QString, Snapshot>     snapshots;    // 代码 -> 快照
    MarketRegime                 regime;       // 当日情绪状态
    QHash<QString, int>          sectorCount;  // 行业 -> 当日涨停家数（兜底）

    QHash<QString, QStringList>  themesOfStock;  // 代码 -> 所属题材
    QHash<QString, double>       themeChangePct; // 题材 -> 板块涨幅 %
    QHash<QString, int>          themeCount;     // 题材 -> 当日涨停家数（run() 内计算）

    // 情绪状态（可空）：非空时收窄「阶段允许的连板高度窗口」
    const SentimentState* sentiment = nullptr;
};

class FilterPipeline : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit FilterPipeline(QObject* parent = nullptr);  // 构造

    // 主入口：返回通过全部筛选的候选（已按评分降序）
    QVector<Candidate> run(const QVector<LimitEntry>& limitUpPool,  // 当日涨停池
                           const FilterContext& ctx);               // 上下文

    static QHash<QString, int> buildSectorCounts(const QVector<LimitEntry>& pool);  // 按行业统计涨停家数

    static QHash<QString, int> buildThemeCounts(const QVector<LimitEntry>& pool,    // 按题材统计涨停家数（主口径）
                                                const QHash<QString, QStringList>& themesOfStock);

    Candidate evaluate(const LimitEntry& entry, const FilterContext& ctx) const;  // 单只标的的完整因子评估

    struct Funnel {                              // 各层通过统计（供 UI 展示漏斗）
        int input = 0;                           // 输入总数（涨停池）
        int afterHardFilter = 0;                 // L1 通过数
        int afterSector = 0;                     // L2 通过数
        int afterQuant = 0;                      // L3 通过数
        int output = 0;                          // 最终输出数
    };
    Funnel lastFunnel() const { return m_funnel; }  // 上一轮漏斗

    struct RejectRecord {                        // 逐只淘汰记录
        QString code;                            // 代码
        QString name;                            // 名称
        QString stage;      // L1 硬排除 / L2 题材效应 / L3 分层量价 / 风险硬排除
        QString reason;                          // 具体淘汰原因
    };
    const QVector<RejectRecord>& lastRejects() const { return m_rejects; }  // 上一轮淘汰明细

    // 闸门拦截时「因子已达标但环境不允许」的清单，用于生成观察名单
    const QVector<Candidate>& lastPassedPreGate() const { return m_passedPreGate; }

    // 分层量价差一点被淘汰的标的（按评分降序，保留前 maxNearMiss 只）
    const QVector<Candidate>& lastNearMiss() const { return m_nearMiss; }
    void setMaxNearMiss(int n) { m_maxNearMiss = n; }  // 调整第二梯队容量

Q_SIGNALS:
    void progress(const QString& text);          // 阶段性进度文案

private:
    bool passHardFilter(const LimitEntry& e,                            // L1 硬性排除
                        const FilterContext& ctx, Candidate& c, QString& reason) const;
    bool passSector(const LimitEntry& e,                                // L2 板块效应
                    const FilterContext& ctx, Candidate& c, QString& reason) const;
    bool passQuant(const LimitEntry& e,                                 // L3 分层量价
                   const FilterContext& ctx, Candidate& c, QString& reason) const;

    void attachFactors(Candidate& c, const FilterContext& ctx) const;   // 生成因子明细

    Funnel m_funnel;                             // 最近一次漏斗统计
    QVector<Candidate> m_passedPreGate;   // 闸门拦截前的完整通过名单
    QVector<Candidate> m_nearMiss;        // 分层量价差一点被淘汰的标的
    QVector<RejectRecord> m_rejects;      // 逐只淘汰记录（阶段 + 原因）
    int m_maxNearMiss = 6;                       // 第二梯队容量上限
};
