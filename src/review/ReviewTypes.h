#pragma once                                     // 防止头文件被重复包含

#include "core/TradePlan.h"                      // 依赖 Candidate
#include "sentiment/SentimentTypes.h"            // 依赖 SentimentState
#include <QDate>                                 // 交易日
#include <QJsonObject>                           // 结构化事实
#include <QString>                               // 文本
#include <QStringList>                           // 剧本/风险条目
#include <QVector>                               // 规则与更新列表

// 复盘助手数据模型（闭环）：ReviewReport（收盘复盘）→ Forecast（次日预判）→ ForecastEval（偏差评估）
// 评估结果回写 AssistantRule 权重与 Lesson，下一轮预判即用新权重
// 底线：助手只影响「预判与简报」，不修改任何交易阈值（自动调参是危险动作）

// 一条可解释的判断规则（代码实现条件，库里只存统计量）
struct AssistantRule {
    QString id;          // R_MOMENTUM / R_BROKEN ...  规则编号
    QString name;        // 中文名
    QString condition;   // 人类可读条件
    int     hits = 0;    // 方向判断命中次数
    int     total = 0;   // 触发次数
    double  weight = 1.0;// 当前权重（越大越被采纳）
    bool    active = true;                           // 是否启用

    double accuracy() const { return total > 0 ? double(hits) / total : 0.0; }  // 命中率
};

// 收盘复盘
struct ReviewReport {
    QDate   date;                                    // 复盘所属交易日
    bool    valid = false;                           // 是否有效
    QString headline;      // 一句话结论
    QString html;          // 邮件/面板用
    QString text;          // 纯文本
    QJsonObject facts;     // 结构化事实（供次日预判与评估）
};

// 次日早盘预判
struct Forecast {
    QDate   forDate;       // 预测的目标交易日
    QDate   madeAt;        // 生成日
    bool    valid = false;                           // 是否有效

    QString stance;        // 强势延续 / 高位分歧 / 结构分化 / 退潮风险
    int     limitUpLow = 0, limitUpHigh = 0, limitUpCenter = 0;  // 涨停家数区间与中枢
    int     maxBoardLow = 0, maxBoardHigh = 0;                   // 最高板区间
    double  advanceLow = 0.0, advanceHigh = 0.0;                 // 晋级率区间
    double  capPct = 0.0;  // 建议有效仓位（沿用昨日复盘，不越权改阈值）

    QStringList themes;    // 主线预判
    QStringList playbook;  // if-then 剧本
    QStringList risks;     // 风险场景

    QString html;          // 简报正文
    double  confidence = 0.5;                        // 置信度 0~1
    QJsonObject trace;     // 命中的规则与调整明细（可审计）
};

// 预判 vs 实际
struct ForecastEval {
    QDate   forDate;                                 // 被评估的交易日
    bool    valid = false;                           // 是否有效
    int     score = 0;     // 0~100                 偏差得分
    bool    hit = false;   // score >= 65            是否算命中
    int     actualLimitUp = 0;                       // 实际涨停家数
    int     actualMaxBoard = 0;                      // 实际最高板
    double  actualAdvance = 0.0;                     // 实际晋级率
    int     directionHit = 0;   // 0 未中 / 1 命中    方向是否判断正确
    QStringList deviations;     // 偏差描述
    QStringList lessons;        // 反思结论
    QString html;                                    // 评估 HTML
};

// 规则更新记录（用于在复盘报告里展示「这轮学到了什么」）
struct RuleUpdate {
    QString id;                                      // 规则编号
    QString name;                                    // 规则名称
    int     totalBefore = 0;                         // 更新前触发次数
    int     hitsBefore = 0;                          // 更新前命中次数
    int     totalAfter = 0;                          // 更新后触发次数
    int     hitsAfter = 0;                           // 更新后命中次数
    double  weightBefore = 1.0;                      // 更新前权重
    double  weightAfter = 1.0;                       // 更新后权重
    bool    deactivated = false;                     // 是否被停用
    QString reason;                                  // 调整原因
};
