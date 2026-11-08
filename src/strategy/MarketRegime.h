#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 MarketRegime / MarketPhase / LimitEntry
#include <QHash>                                 // 代码 → 快照索引
#include <QObject>                               // 基类（便于后续扩展为带定时器的引擎）

// MarketRegime：市场情绪周期判定（情绪闸门）
// 情绪闸门比选股更前置 —— 退潮期最好的板也不碰
// 输入：当日涨停/炸板/跌停池 + 昨日连板晋级情况 + 昨日涨停股今日表现
// 输出：阶段判定 + 是否允许出手 + 建议仓位

class RegimeEngine : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit RegimeEngine(QObject* parent = nullptr);  // 构造：无额外初始化

    // 基础情绪指标（由当日三池直接统计）
    // tradeDate —— 涨停池所属交易日；非交易日回退时必须传入实际交易日，否则缓存会存错键
    static void computeBase(MarketRegime& reg,                  // 就地填充结果
                             const QVector<LimitEntry>& limitUp,   // 涨停池
                             const QVector<LimitEntry>& broken,    // 炸板池
                             const QVector<LimitEntry>& limitDown, // 跌停池
                             const QDate& tradeDate = QDate());    // 实际交易日

    // 叠加昨日数据：连板晋级率、首板晋级率、昨日涨停股今日平均涨幅（溢价率）
    // 这三个是打板情绪周期最灵敏的先行指标，涨停家数只是滞后指标
    static void applyYesterday(MarketRegime& reg,                     // 就地填充结果
                               const QVector<LimitEntry>& prevLimitUp,   // 昨日涨停池
                               const QVector<LimitEntry>& todayLimitUp,  // 今日涨停池
                               const QHash<QString, Snapshot>& quotes);  // 今日快照（算溢价）

    static void evaluateOn(MarketRegime& reg);       // 对外部持有的结构就地判定

    void evaluate();                                 // 对内部成员判定（等价于 evaluateOn(m_regime)）

    const MarketRegime& regime() const { return m_regime; }  // 只读访问判定结果

    double suggestedPosition() const;                // 建议总仓上限（0~1）

    static QString positionAdvice(MarketPhase p);    // 阶段 → 仓位映射说明

private:
    MarketRegime m_regime;                           // 内部持有的情绪快照
};
