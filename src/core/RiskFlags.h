#pragma once                                     // 防止头文件被重复包含
#include "core/Types.h"                          // 依赖 LimitEntry / Snapshot / KlineSeries
#include <QStringList>                           // 返回值为 QStringList 风险条目

// RiskFlags：问题股 / 风险事件标记
// 打板亏大钱常是「形态完美 + 突发利空」——用 PE、市值、连板高度、上市天数、
// 开高低这些可量化字段先把风险标出来，再决定打不打

namespace Risk {                                // 风险标记命名空间

// 返回命中的风险条目（人类可读）。空 = 未见明显风险
QStringList evaluate(const LimitEntry& entry, const Snapshot& snap,   // 输入：涨停条目 + 全市场快照
                     const KlineSeries* kline);                       // 可选 K 线（可为空）

} // namespace Risk                              // 命名空间结束
