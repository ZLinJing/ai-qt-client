#pragma once                                     // 防止头文件被重复包含
#include "core/Types.h"                          // 依赖 Snapshot / KlineSeries / MarketPhase
#include "storage/TradeRecord.h"                 // 依赖 TradeRecord 与费用口径
#include <QDate>                                 // 评估基准日
#include <QString>                               // 文案
#include <QStringList>                           // 条件 / 风险列表
#include <QVector>                               // 归因项与执行步骤

// ============================================================================
// TradeAdvisor：交易日志的「AI 诊断」推理层
//
// 定位：对每一笔成交做**可解释**的盈亏评估，输出三块结论：
//   ① 盈亏原因分析 —— 入场执行 / 兑现位置 / 趋势 / 量能 / 周期 / 费用 / 情绪
//   ② 止盈建议     —— 盈利单适用的策略（分批兑现 / 移动止盈 / 保本止盈 / 时间止盈）
//                     与每一步的价格触发条件、执行依据
//   ③ 回本可行性   —— 亏损单的含费回本价、还需涨幅、修复价位区间、预计交易日、
//                     需要满足的条件与硬约束（跌破计划止损则直接判为「困难」）
//
// 设计约束（与 core/ 其它纯计算模块一致）：
//   · 零 IO、零 GUI、无网络 —— 输入只有成交记录 + 快照 + 日 K + 情绪阶段；
//   · 所有结论都能回溯到具体数字（价格、涨幅、均线、ATR、量比、费用率）；
//   · 数据缺失时降级而不是编造：无 K 线 → 只用计划价与费用口径；无现价 → 明确「待行情」。
//
// 口径说明：
//   · 回本价 = 含双边费用的真实保本价，不是「买入价」本身；
//     breakeven = buyPrice × (1 + 买入佣金率) ÷ (1 − 卖出费率)，费率见 TradeRecord。
//   · 预计天数由 ATR 折算（修复斜率取 0.35×ATR%/日，再按趋势与情绪修正），
//     是**波动率折算**而不是价格预测，输出为区间并标注依据。
// ============================================================================

namespace TradeAI {

// 盈亏状态
enum class Verdict {
    Unknown = 0,   // 无现价 / 记录不完整，暂不评估
    Profit,        // 盈利（含持仓浮盈与已了结盈利）
    Loss,          // 亏损（含持仓浮亏与已了结亏损）
    Flat,          // 持平（扣除费用后接近 0）
};

// 回本可行性档位
enum class Feasibility {
    Unknown = 0,   // 数据不足，无法判断
    NotNeeded,     // 已盈利，无需回本（给保护利润的底线）
    Feasible,      // 回本可行
    Conditional,   // 有条件回本（需要量能 / 趋势 / 情绪配合）
    Hard,          // 回本困难（已破位或跌幅过大）
    Closed,        // 已了结：本笔无法回本，只能由后续交易修复
};

// 一条归因：impact > 0 贡献盈利，< 0 拖累，0 为中性事实
struct ReasonItem {
    QString title;      // 归因维度（入场执行 / 兑现位置 / 趋势结构 …）
    QString detail;     // 具体事实与数字
    int     impact = 0; // -2 … +2
};

// 一条执行步骤（止盈或风控）
struct ExitStep {
    QString action;         // 动作（兑现 50% / 减半 / 清仓 / 继续持有）
    QString trigger;        // 触发条件（价格、均线或时间）
    QString basis;          // 执行依据（锚点与口径）
    bool    defensive = false;  // 是否为防守型（保护利润 / 止损）
};

// 回本或转盈评估
struct RecoveryPlan {
    Feasibility feasibility = Feasibility::Unknown;
    int    score = 0;          // 0~100 的可行性评分（参与档位判定，可展示）
    bool   applicable = true;  // false = 已盈利或已了结，本笔不再讨论「回本」
    double breakeven = 0.0;    // 含双边费用的回本价
    double requiredPct = 0.0;  // 距回本还需的涨幅 %（已了结时为「下一笔需盈利 %」）
    double targetLow = 0.0;    // 修复路径的第一个目标（压力位）
    double targetHigh = 0.0;   // 修复路径的终点（含费回本价）
    double profitTarget = 0.0; // 转盈目标（计划止盈价，未记录时为 0）
    int    daysLow = 0;        // 预计交易日下沿
    int    daysHigh = 0;       // 预计交易日上沿
    QString verdict;           // 一句话判断（直接展示）
    QStringList conditions;    // 需要满足的条件（价格 / 量能 / 情绪 / 时间）
    QStringList blockers;      // 硬约束（跌破计划止损、放量下跌、超期未修复）
};

// 单笔交易的完整诊断结果
struct Insight {
    bool valid = false;        // 记录本身是否可用于分析
    bool priced = false;       // 是否有可用的评估价（现价 / 卖出价）
    bool closed = false;       // 是否已了结
    Verdict verdict = Verdict::Unknown;
    double price = 0.0;        // 评估价
    double pnl = 0.0;          // 评估盈亏（元）
    double pnlPct = 0.0;       // 评估盈亏（%）
    double breakeven = 0.0;    // 含费回本价（任何状态都给，便于展示）

    QString headline;          // 一句话结论
    QString stateTag;          // 表格短标签（浮盈·分批止盈 / 浮亏·可修复 …）

    QVector<ReasonItem> reasons;   // 盈亏原因（按叙事顺序，不按分值排序）
    QString exitStrategy;          // 止盈（或止损）策略名
    QVector<ExitStep> exitSteps;   // 具体执行步骤
    QString exitBasis;             // 执行依据汇总

    RecoveryPlan recovery;         // 回本 / 转盈可行性
    QStringList riskNotes;         // 风险提示（纪律、费用、超期、数据缺失）
    QString trace;                 // 数据口径与推理来源（可核对）
};

// 分析输入：成交记录 + 可选行情 + 可选日 K + 可选情绪阶段
struct Input {
    TradeRecord trade;             // 必填
    Snapshot snapshot;             // 持仓中用于取现价与当日量价（可为空）
    bool hasQuote = false;         // 是否拿到有效现价
    const KlineSeries* kline = nullptr;  // 可选日 K（内部优先用不复权序列）
    QDate asOf;                    // 评估基准日（默认今天）
    MarketPhase phase = MarketPhase::Unknown;  // 当日情绪阶段
    bool phaseKnown = false;       // 情绪阶段是否可信
};

// 单笔分析主入口（纯函数，可重复调用，结果确定性）
Insight analyze(const Input& in);

// 名称映射（UI / CSV / 自检共用，避免多处口径漂移）
QString verdictName(Verdict v);
QString feasibilityName(Feasibility f);

} // namespace TradeAI
