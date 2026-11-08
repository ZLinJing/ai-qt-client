#pragma once                                     // 防止头文件被重复包含

#include "core/TradePlan.h"                      // 依赖 Candidate / RecommendBundle
#include "review/DecisionReview.h"               // 依赖板块资金流行 / 决策复盘数据结构
#include "review/ReviewTypes.h"                  // 依赖 ReviewReport / Forecast / ForecastEval
#include "sentiment/SentimentTypes.h"            // 依赖 SentimentState
#include "storage/TradeRecord.h"                 // 依赖交易日志
#include <QDate>                                 // 交易日
#include <QDateTime>                             // 板块数据抓取时间
#include <QHash>                                 // 代码 → 日 K / 资金流
#include <QJsonObject>                           // 序列化
#include <QString>                               // 文本
#include <QVector>                               // 列表

// 复盘输入：把「当日已算完的结果」打包，避免引擎反向依赖 UI/管线
struct ReviewInput {
    QDate date;                                  // 交易日
    SentimentState sentiment;                    // 当日情绪状态
    int funnelInput = 0;                         // 漏斗：输入
    int funnelHard = 0;                          // 漏斗：硬排除后
    int funnelSector = 0;                        // 漏斗：题材后
    int funnelQuant = 0;                         // 漏斗：量价后
    int funnelOut = 0;                           // 漏斗：输出
    QVector<Candidate> candidates;               // 候选
    RecommendBundle bundle;                      // 荐股结果

    // ---- 决策复盘所需的事实（只打包「当日已算完的结果」，不反向依赖 UI / 管线）----
    // 历史交易日回溯由 ReviewPanel 从本地库补齐；这里只留当日实时可得的部分。
    QHash<QString, KlineSeries> klines;          // 代码 → 日 K（含不复权）
    QVector<TradeRecord> trades;                 // 交易日志（判定执行 / 跳过 / 持仓）
    QHash<QString, double> mainNetYi;            // 代码 → 个股主力净流入（亿元）
    QHash<QString, QString> mainNetSource;       // 代码 → 资金流来源与窗口
    QHash<QString, double> sealToAmount;         // 代码 → 封单额/成交额（代理口径）

    bool valid() const { return date.isValid() && sentiment.fact.valid(); }  // 有日期且有情绪事实才有效
};

// ReviewEngine：纯计算，无 IO、无 GUI
// buildReview 收盘复盘 / buildForecast 次日预判 / evaluate 偏差评估 /
// updateRules 规则权重调整（Laplace 平滑）/ updateBias 幅度偏差在线校正（EMA）

class ReviewEngine
{
public:
    static QVector<AssistantRule> defaultRules();  // 内置规则表

    static ReviewReport buildReview(const ReviewInput& in);  // 生成收盘复盘

    static Forecast buildForecast(const ReviewReport& yesterday,   // 昨日复盘
                                  const QVector<AssistantRule>& rules,  // 规则表
                                  double biasLogRatio);                // 幅度偏差校正项

    static ForecastEval evaluate(const Forecast& f, const SentimentState& actual,  // 预判 vs 实际
                                 const ReviewInput& today);

    static QVector<RuleUpdate> updateRules(QVector<AssistantRule>& rules,  // 按命中情况调整权重
                                           const Forecast& f, const ForecastEval& e);

    static double updateBias(double bias, const Forecast& f, const ForecastEval& e);  // 偏差 EMA 更新

    // 把「昨日预判 vs 今日实际」追加到复盘正文（报告的最后一节）
    static void appendEvalSection(ReviewReport& report, const ForecastEval& e,
                                  const QVector<RuleUpdate>& updates);

    static QJsonObject toJson(const ReviewReport& r);      // 序列化复盘
    static QJsonObject toJson(const Forecast& f);          // 序列化预判
    static QJsonObject toJson(const ForecastEval& e);      // 序列化评估
    static ReviewReport reviewFromJson(const QJsonObject& o);    // 反序列化复盘
    static Forecast forecastFromJson(const QJsonObject& o);      // 反序列化预判
    static ForecastEval evalFromJson(const QJsonObject& o);      // 反序列化评估
};
