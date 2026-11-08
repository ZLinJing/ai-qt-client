#pragma once                                     // 防止头文件被重复包含
#include "core/Types.h"                          // 依赖 Candle（OHLCV）
#include <QString>                               // 文案字段
#include <QVector>                               // 价格网格与峰列表

// ============================================================================
// 筹码分布（成本分布）纯计算模块
//  作用：由日 K 重建「每个价位沉淀了多少筹码」，从而定位
//        ① 当前价下方最近的**支撑密集峰**（买入承接带）
//        ② 当前价上方最近的**压力密集峰**（卖出兑现区）
//  口径：必须用**不复权真实价**序列，与涨停价 / 成交价同口径，否则除权日会整体错位。
//  约束：纯计算、零 IO、零 GUI，可单元测试。
// ============================================================================

namespace Chip {                                 // 筹码分布命名空间

struct Peak {                                    // 一个筹码密集峰
    double low = 0.0;                            // 峰区下沿（价）
    double high = 0.0;                           // 峰区上沿（价）
    double center = 0.0;                         // 峰顶价（筹码最密集的价位）
    double volume = 0.0;                         // 峰内筹码量
    double share = 0.0;                          // 占总筹码比例（0~1）
};

struct Distribution {                            // 一次筹码分布的完整结果
    bool valid = false;                          // 数据不足时为 false，调用方需回退到均线口径
    QVector<double> prices;                      // 价格网格（升序）
    QVector<double> volumes;                     // 各价位的筹码量
    double totalVolume = 0.0;                    // 总筹码量
    double avgCost = 0.0;                        // 平均成本（筹码量加权）
    double profitRatio = 0.0;                    // 获利盘比例（当前价以下筹码占比，0~1）
    QVector<Peak> peaks;                         // 密集峰（按筹码量降序）

    // 当前价下方最近的密集峰（支撑）；无合格峰返回 nullptr
    const Peak* supportBelow(double price) const;
    // 当前价上方最近的密集峰（压力）；无合格峰返回 nullptr
    const Peak* pressureAbove(double price) const;
};

// 由不复权日 K 构建筹码分布。
//   raw     —— 不复权真实价日 K（应已按决策日截断，杜绝未来函数）
//   price   —— 当前价（用于切分获利盘、定位上下峰）
//   decay   —— 每日筹码衰减系数（越新的筹码权重越高；0.985 ≈ 20 个交易日换手 40%）
//   bins    —— 价格网格分辨率
Distribution build(const QVector<Candle>& raw, double price,
                   double decay = 0.985, int bins = 140);

// 峰的有效性门槛：筹码占比低于此值视为噪声峰，不参与支撑 / 压力定位
constexpr double kMinPeakShare = 0.02;           // 2%

} // namespace Chip                              // 命名空间结束
