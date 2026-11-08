#pragma once                                     // 防止头文件被重复包含
#include "core/Types.h"                          // 依赖 Candidate
#include "core/FactorTypes.h"                    // 依赖 ScoreBreakdown（得分由来）
#include <QVector>                               // 分项得分列表

// Scorer：候选标的综合评分（0~100）
// 各因子归一化到 0~1 后按固定权重加权
// 评分只用于「通过筛选之后排序」，不用于放宽筛选条件

class Scorer
{
public:
    Scorer() = default;                             // 无状态：权重与阈值由实现内部固定

    double score(const Candidate& c) const;         // 综合得分 0~100

    QVector<double> breakdown(const Candidate& c) const;  // 分项得分（UI 条形/雷达展示）
    QStringList factorNames() const;                      // 分项名称，与 breakdown 顺序一一对应

    // 「查看得分由来」：公式 + 数据来源 + 8 因子逐项明细 + 修正项（高位/风险/龙头加分）
    // finalScore 传计划中的最终得分（可能已被筛选管线修正），用于自证换算过程
    ScoreBreakdown describe(const Candidate& c, double finalScore) const;
};
