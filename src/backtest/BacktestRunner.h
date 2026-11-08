#pragma once                    // 头文件包含保护

#include "core/Types.h"          // 基础类型定义
#include "core/TradePlan.h"      // 交易计划结构体
#include "core/FactorTypes.h"    // 维度枚举
#include "strategy/FactorConfig.h" // 与实盘共用的多因子配置
#include <QDate>                 // 日期类型
#include <QHash>                 // 哈希容器
#include <QObject>               // Qt对象基类（信号槽支持）
#include <QStringList>           // 字符串列表
#include <QVector>               // 动态数组容器

class HttpClient;                // HTTP客户端前向声明
class LimitPoolSource;           // 涨跌停池数据源前向声明
class KlineSource;               // K线数据源前向声明

struct BacktestConfig {
    int    days = 20;          // 向前回放的交易日数
    QDate  endDate;            // 回测结束日期，默认今天
    int    klineDays = 120;    // K线拉取跨度，需覆盖回放区间+2天
    QString csvPath;           // CSV导出路径，非空则输出逐笔明细
    FactorConfig factorConfig; // 与实盘 GUI 读取同一份 app_setting 配置
};

struct BacktestTrade {
    QDate   planDate;          // 计划生成日（T日）
    QString code;              // 股票代码
    QString name;              // 股票名称
    int     board = 0;         // 连板高度
    QString tier;              // 信号层级：试仓/建仓/主攻
    double  buyLow = 0.0;      // 买入区间下限
    double  buyHigh = 0.0;     // 买入区间上限
    double  stop = 0.0;        // 止损价
    double  take = 0.0;        // 止盈价
    double  fillProbability = 0.0; // 预估成交概率
    double  positionPct = 0.0; // 仓位百分比

    bool    filled = false;    // 是否实际成交
    double  entry = 0.0;       // 实际成交价
    double  exit = 0.0;        // 实际卖出价
    double  pnlPct = 0.0;      // 净盈亏百分比（扣成本后）
    QString outcome;           // 结果标签：止盈/止损/跳空止损/到期/未成交原因

    // 多因子：与实盘同一 MultiFactorEngine::rank() 产出
    double  compositeScore = 0.0;  // 综合评分 0~100
    int     factorRank = 0;        // 综合分排名
    QVector<double> dimensionShares; // 各维度贡献占比 %（按维度索引）
};

struct BacktestResult {
    QVector<BacktestTrade> trades; // 全部交易记录
    int    days = 0;               // 实际回测交易日数
    int    klineMissingDays = 0;   // K线缺失天数，>0表示样本不完整
    int    signalCount = 0;        // 总信号数（避免命名signals与Qt宏冲突）
    int    filled = 0;             // 成交笔数
    int    wins = 0;               // 盈利笔数
    double winRate = 0.0;          // 胜率（%）
    double avgPnl = 0.0;           // 每笔成交平均收益%
    double expectancy = 0.0;       // 每信号期望收益%（未成交计0）
    double maxDrawdown = 0.0;      // 累计收益曲线最大回撤（百分点）
    double totalPnlPts = 0.0;      // 等权累计收益（百分点）
    QHash<QString, QVector<double>> byTier; // 按层级分组的收益序列
    QHash<QString, int> blockerStats;       // 各拦截门命中次数统计
    QStringList notes;                      // 备注与警告信息

    // 维度贡献度聚合：评估各维度对最终选股结果的影响
    QVector<double> avgDimensionContribution; // 平均贡献分
    QVector<double> avgDimensionShare;        // 平均贡献占比 %
    QString factorDigest;                     // 配置指纹（与实盘比对）
    QString factorNotes;                      // 数据可得性说明
};

class BacktestRunner : public QObject
{
    Q_OBJECT                     // 启用Qt元对象系统（信号槽）
public:
    explicit BacktestRunner(QObject* parent = nullptr); // 构造函数

    void run(const BacktestConfig& cfg); // 执行回测主流程

    // 单笔结算模拟（公开以便单元测试验证T+1买入/T+2卖出逻辑）
    static BacktestTrade simulate(const TradePlan& plan, const QString& tier,
                                  const KlineSeries& series, const QDate& planDate,
                                  double fillProb);

Q_SIGNALS:
    void progress(const QString& text);          // 进度更新信号
    void finished(const BacktestResult& result); // 回测完成信号

private:
    struct Pools {
        QVector<LimitEntry> up, broken, down; // 涨停/炸板/跌停列表
        bool ok = false;                       // 数据获取成功标志
    };

    bool fetchPools(const QDate& date, Pools& out); // 同步获取指定日期涨跌停池
    bool ensureKlines(const QStringList& codes);    // 确保K线缓存完整

    HttpClient* m_http = nullptr;        // HTTP客户端实例
    LimitPoolSource* m_poolSrc = nullptr; // 涨跌停池数据源
    KlineSource* m_klineSrc = nullptr;    // K线数据源
    QHash<QString, KlineSeries> m_klines; // K线缓存（含未来数据，使用时按需截断）
};
