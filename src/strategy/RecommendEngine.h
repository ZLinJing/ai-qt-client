#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 Candidate / MarketRegime
#include "core/TradePlan.h"                      // 依赖 TradePlan / RecommendBundle
#include "strategy/FilterPipeline.h"             // 依赖筛选管线的漏斗与淘汰记录
#include "strategy/FactorConfig.h"               // 多因子参数体系
#include "strategy/MultiFactorEngine.h"          // 多因子评分 / 排名 / 贡献度
#include <QDate>                                 // 评分所属交易日（防未来函数）
#include <QHash>                                 // 代码 → 日 K / 快照
#include <QObject>                               // 基类

// RecommendEngine：荐股装配器
// 把「涨停池 + 情绪状态 + 日 K」装配成可直接执行的荐股清单
// 每只都带买入/止盈/止损区间、仓位建议与盈亏比
// 闸门拦截时仍输出清单但标记「仅观察」、仓位为 0 —— 不可执行 ≠ 不显示
//
// 多因子扩展：装配完成后统一调用 MultiFactorEngine::rank()，
// 输出综合评分排名、按综合分重排，并把维度贡献度与「得分由来」写入 TradePlan。
// 回测链路调用同一个 MultiFactorEngine::rank()，保证口径一致。
//
// 候选池口径（数量优先）：
//   plans   —— 通过全部筛选的正式候选（小资金仅前 kConcentratedPositionCount 只可执行）
//   backups —— 分层量价差一点的第二梯队，只作观察备选，仅决策台候选池展示

class RecommendEngine : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit RecommendEngine(QObject* parent = nullptr);

    // 注入多因子配置与数据（GUI 每次 runScreen 前调用；回测自行构造同样的输入）
    void setFactorConfig(const FactorConfig& cfg) { m_factorCfg = cfg; }
    void setFactorData(const QHash<QString, Snapshot>* snapshots,
                       const QHash<QString, KlineSeries>* klines,
                       const QDate& asOf);
    const FactorConfig& factorConfig() const { return m_factorCfg; }
    const FactorReport& lastFactorReport() const { return m_factorReport; }

    // 装配荐股清单
    //   passed   —— 已通过全部筛选的候选（闸门放行时非空）
    //   klines   —— 代码 → 日 K（算 MA5 / 当日最低价等精确锚点）
    //   regime   —— 当日情绪状态
    //   pipeline —— 筛选管线（取其闸门前名单、临近名单、漏斗与淘汰明细）
    RecommendBundle build(const QVector<Candidate>& passed,
                          const QHash<QString, KlineSeries>& klines,
                          const MarketRegime& regime,
                          FilterPipeline* pipeline);

    const RecommendBundle& lastBundle() const { return m_bundle; }

private:
    // 构建单只计划并追加到 out（plans = 推荐名单，backups = 候选池备选）
    void appendPlan(QVector<TradePlan>& out, const Candidate& c,
                    const QHash<QString, KlineSeries>& klines,
                    const MarketRegime& regime,
                    bool actionable, const QString& noteOverride);

    // 多维度评分 → 排名 → 写回计划 → 按综合分重排 → 过滤降级
    void applyFactorRanking(RecommendBundle& bundle, const QVector<Candidate>& eligible);

    RecommendBundle m_bundle;                    // 最近一次装配结果
    FactorConfig m_factorCfg = FactorConfig::defaults();
    const QHash<QString, Snapshot>* m_snapshots = nullptr;
    const QHash<QString, KlineSeries>* m_klines = nullptr;
    QDate m_asOf;
    FactorReport m_factorReport;                 // 最近一次多因子排名报告
};
