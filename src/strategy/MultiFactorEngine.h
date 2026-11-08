#pragma once

#include "core/Types.h"
#include "core/FactorTypes.h"
#include "strategy/FactorConfig.h"
#include <QDate>
#include <QHash>
#include <QString>
#include <QVector>

// 多维度做多因子引擎：纯计算、零 IO、零 GUI。
// 实盘（RecommendEngine）与回测（BacktestRunner）共用本文件的 rank()/evaluate()，
// 输入统一由 makeInputs() 构造并 sliceTo(asOf) 截断，保证两条链路口径完全一致。
//
// 保留原打板策略：FilterPipeline 的 L0-L3 硬筛选与 Scorer 8 因子不动，
// 其评分作为「打板核心」维度参与多因子组合；新增维度只负责叠加、过滤与排序。

struct FactorInput {
    Candidate   candidate;                 // 已通过打板硬筛选的候选（含 c.score）
    Snapshot    snapshot;                  // 可为空：回测历史快照缺失时 isValid()==false
    KlineSeries kline;                     // 调用方保证已 sliceTo(asOf)
    double      dabanScore = 0.0;          // 0~100，来自 Scorer::score
};

struct FactorEvaluation {
    QString code;
    QString name;
    double  composite = 0.0;               // 综合评分 0~100
    double  dabanScore = 0.0;              // 原打板分 0~100
    int     rank = 0;                      // 综合分排名（1 起）
    bool    passed = true;                 // 是否被多因子过滤规则淘汰
    QString rejectReason;
    QVector<FactorContribution> contributions;  // 维度贡献度分解
    QString summary;                       // 主导维度摘要
};

struct FactorReport {
    QVector<FactorEvaluation> ranking;     // 通过过滤者，按综合分降序，截断至 TopN
    QVector<FactorEvaluation> rejected;    // 被过滤者（含原因），便于解释「为什么没入选」
    QVector<double> avgContribution;       // 各维度平均贡献分（按维度索引）
    QVector<double> avgShare;              // 各维度平均贡献占比 %
    QVector<int>    availableCount;        // 各维度实际参与评分的标的数
    QString configDigest;                  // 配置指纹
    QString notes;                         // 口径与数据可得性说明
};

class MultiFactorEngine
{
public:
    // 单只标的：维度计算 → 过滤 → 加权组合 → 贡献度分解
    static FactorEvaluation evaluate(const FactorInput& in, const FactorConfig& cfg);

    // 批量：评分、过滤、排名、TopN、维度贡献度聚合
    static FactorReport rank(const QVector<FactorInput>& inputs, const FactorConfig& cfg);

    // 统一的输入构造：实盘与回测都走这里，K 线截断口径唯一
    static QVector<FactorInput> makeInputs(const QVector<Candidate>& candidates,
                                           const QHash<QString, Snapshot>& snapshots,
                                           const QHash<QString, KlineSeries>& klines,
                                           const QDate& asOf);
};
