#pragma once

#include "core/FactorTypes.h"
#include <QJsonObject>
#include <QString>
#include <QVector>

// 多因子参数体系：每个维度可独立启停、调权、设过滤门槛、指定参与方式。
// 组合规则、综合分门槛与 TopN 也全部可配置；配置以 JSON 存在 app_setting，
// 实盘 GUI 与无头回测读取同一份配置，保证两套链路口径一致。

enum class FactorMode {
    Score = 0,   // 只叠加评分
    Filter,      // 只做过滤（低于门槛即淘汰，不计入评分）
    Both         // 既叠加评分，也做过滤
};

enum class CombineRule {
    WeightedSum = 0, // 加权求和（默认）
    MinPenalty       // 木桶：75% 加权分 + 25% 最低维度分
};

struct DimensionConfig {
    bool       enabled = true;
    double     weight = 1.0;       // 原始权重，引擎按「启用且可用」的维度归一化
    double     threshold = 0.35;   // 0~1，仅 Filter / Both 生效
    FactorMode mode = FactorMode::Score;
};

struct FactorConfig {
    QVector<DimensionConfig> dims;                 // 长度恒为 kFactorDimensionCount
    CombineRule rule = CombineRule::WeightedSum;
    double      minComposite = 0.0;               // 综合分下限 0~100，0 表示不启用
    bool        requireAllAvailable = false;      // 数据缺失的维度是否直接淘汰该标的
    int         topN = 30;                        // 输出排名前 N

    static FactorConfig defaults();
    static FactorConfig balanced();                       // 「均衡做多」：版本管理移除后唯一保留的策略参数
    static QString strategyName();                        // 策略名（品牌条 / 状态栏展示）
    static FactorConfig fromJson(const QJsonObject& o);
    static FactorConfig fromJsonText(const QString& text);
    // 旧版「多策略版本集合」的一次性迁移：取当时启用版本（「均衡做多」优先）的权重。
    // 入参为空或格式非法时返回 balanced()。
    static FactorConfig fromLegacyVersionsText(const QString& versionsJson);
    QJsonObject toJson() const;

    void    validate();                           // 修正非法值、保证至少一个有效维度
    QString digest() const;                       // 配置指纹（落库 / 复盘用）
    QString summary() const;                      // 人类可读的配置摘要

    // 安全访问：外部可能直接构造未初始化的配置（dims 为空），此时退回默认维度配置
    DimensionConfig dimAt(int i) const {
        return (i >= 0 && i < dims.size()) ? dims.at(i) : DimensionConfig();
    }

    static QString modeName(FactorMode m);
    static QString ruleName(CombineRule r);
};
