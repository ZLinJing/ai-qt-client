#pragma once                                     // 防止头文件被重复包含
#include <QString>                               // QString：代码/名称/题材等文本字段
#include <QDate>                                 // QDate：交易日
#include <QTime>                                 // QTime：封板时间点
#include <QDateTime>                             // QDateTime：完整时间戳
#include <QVector>                               // QVector：K 线与候选列表容器
#include <QStringList>                           // QStringList：风险提示、因子名列表
#include <QMetaType>                             // Q_DECLARE_METATYPE：让结构体能跨线程走信号

// 领域模型层：与 UI、数据源解耦的纯值语义结构体，可直接塞进 QVariant / JSON

// 交易标的所属板块
enum class Board {
    Unknown = 0,                                 // 未识别
    MainBoard,     // 沪深主板 60/00  涨跌幅 ±10%
    ChiNext,       // 创业板   30      涨跌幅 ±20%
    StarMarket,    // 科创板   688     涨跌幅 ±20%
    BSE,           // 北交所   8/920   涨跌幅 ±30%
};

// 板块 → 单日涨跌幅限制比例
inline double boardLimitRatio(Board b)
{
    switch (b) {                                 // 按板块返回涨跌停比例
    case Board::MainBoard: return 0.10;          // 主板 ±10%
    case Board::ChiNext:   return 0.20;          // 创业板 ±20%
    case Board::StarMarket:return 0.20;          // 科创板 ±20%
    case Board::BSE:       return 0.30;          // 北交所 ±30%
    default:               return 0.10;          // 未知按主板保守处理
    }
}

// 板块 → 中文名（UI 展示用）
inline QString boardName(Board b)
{
    switch (b) {                                 // 逐个映射到中文
    case Board::MainBoard:  return QStringLiteral("主板");
    case Board::ChiNext:    return QStringLiteral("创业板");
    case Board::StarMarket: return QStringLiteral("科创板");
    case Board::BSE:        return QStringLiteral("北交所");
    default:                return QStringLiteral("未知");
    }
}

// 按 6 位股票代码前缀推断板块
inline Board boardOfCode(const QString& code)
{
    if (code.size() != 6) return Board::Unknown;  // 非 6 位代码无法判定
    if (code.startsWith(QStringLiteral("688")) || code.startsWith(QStringLiteral("689")))
        return Board::StarMarket;                 // 688/689 开头为科创板
    if (code.startsWith(QStringLiteral("30")))
        return Board::ChiNext;                    // 30 开头为创业板
    if (code.startsWith(QStringLiteral("8")) || code.startsWith(QStringLiteral("920")))
        return Board::BSE;                        // 8/920 开头为北交所
    if (code.startsWith(QStringLiteral("6")) || code.startsWith(QStringLiteral("0")))
        return Board::MainBoard;                  // 6/0 开头为沪深主板
    return Board::Unknown;                        // 其余无法归类
}

// 实时快照：来源新浪 Market_Center.getHQNodeData（全市场 5500+ 只）
struct Snapshot {
    QString code;        // 6 位代码
    QString name;                                // 股票名称
    QString symbol;      // sh600519 / sz000001   带市场前缀的行情代码
    double price = 0.0;                          // 最新价（元）
    double prevClose = 0.0;                      // 昨收价（元）
    double open = 0.0;                           // 今开（元）
    double high = 0.0;                           // 最高（元）
    double low = 0.0;                            // 最低（元）
    double changePct = 0.0;      // 涨跌幅 %
    double volume = 0.0;         // 成交量（股）
    double amount = 0.0;         // 成交额（元）
    double turnover = 0.0;       // 换手率 %
    double floatCap = 0.0;       // 流通市值（元）—— 新浪 nmc 单位是万元，已换算
    double totalCap = 0.0;       // 总市值（元）
    double pe = 0.0;                             // 市盈率（负值代表亏损）
    double pb = 0.0;                             // 市净率
    Board board = Board::Unknown;                // 所属板块

    bool isValid() const { return !code.isEmpty() && price > 0.0; }  // 有代码且有有效价格才算可用
    bool isST() const { return name.contains(QStringLiteral("ST")) || name.contains(QStringLiteral("退")); }  // 名称含 ST/退 视为退市风险股
};

// 单根日 K 线：来源腾讯 web.ifzq.gtimg.cn 前复权接口
struct Candle {
    QDate date;                                  // 交易日
    double open = 0.0;                           // 开盘价
    double close = 0.0;                          // 收盘价
    double high = 0.0;                           // 最高价
    double low = 0.0;                            // 最低价
    double volume = 0.0;   // 手                 成交量
    double amount = 0.0;   // 元（部分接口不返回，0 表示缺失）
};

// 一只股票的日线序列（同时保存复权与不复权两份）
struct KlineSeries {
    QString code;                                // 对应股票代码
    QVector<Candle> candles;      // 前复权 —— 只用于均线 / 量能等「连续价格」计算
    QVector<Candle> rawCandles;   // 不复权 —— 涨停判定 / 止损锚点 / 真实成交价必须用它

    bool valid() const { return !candles.isEmpty(); }   // 前复权序列是否可用
    bool hasRaw() const { return !rawCandles.isEmpty(); }  // 不复权序列是否可用

    static int lastIndexAt(const QVector<Candle>& v, const QDate& d) {   // 找 <= d 的最后一根 K 线下标
        if (!d.isValid()) return v.isEmpty() ? -1 : v.size() - 1;        // 日期无效则退化为最末一根
        for (int i = v.size() - 1; i >= 0; --i)                          // 从最新往前扫
            if (v.at(i).date.isValid() && v.at(i).date <= d) return i;   // 首个满足条件的即目标下标
        return -1;                                                       // 全部晚于 d，则无匹配
    }

    int indexAt(const QDate& d) const { return lastIndexAt(candles, d); }  // 对前复权序列定位下标

    // 截断到某个交易日（含）：指标一律先截断再算，避免「用最新一根算历史交易日」
    KlineSeries sliceTo(const QDate& d) const {
        KlineSeries out;                         // 承载截断结果
        out.code = code;                         // 继承代码
        const int i = lastIndexAt(candles, d);   // 前复权序列的截断点
        if (i >= 0) out.candles = candles.mid(0, i + 1);   // 取 [0, i] 区间的 K 线
        const int j = lastIndexAt(rawCandles, d);          // 不复权序列的截断点
        if (j >= 0) out.rawCandles = rawCandles.mid(0, j + 1);  // 同样截断不复权序列
        return out;                              // 返回截断后的新序列
    }

    // 前复权 / 不复权的价格换算系数（除权除息后两者不再相等）
    double adjustFactor() const {
        if (rawCandles.isEmpty() || candles.isEmpty()) return 1.0;  // 缺任一份数据则视为无复权差
        const Candle& q = candles.back();        // 前复权最后一根
        const int j = lastIndexAt(rawCandles, q.date);  // 在原始序列中定位同一交易日
        if (j < 0) return 1.0;                   // 定位失败视为无复权差
        const double rc = rawCandles.at(j).close;   // 该日真实收盘价
        if (rc <= 0.0 || q.close <= 0.0) return 1.0;  // 价格异常时不做换算
        return rc / q.close;                     // 真实价 / 复权价 即换算系数
    }
};

// 涨停池条目：来源东财 push2ex 的涨停/炸板/跌停池接口
struct LimitEntry {
    QString code;                                // 股票代码
    QString name;                                // 股票名称
    Board board = Board::Unknown;                // 所属板块
    double price = 0.0;        // 最新价（元）
    double limitPrice = 0.0;   // 涨停价（元）
    double changePct = 0.0;    // 涨跌幅 %
    double amount = 0.0;       // 成交额（元）
    double floatCap = 0.0;     // 流通市值（元）
    double totalCap = 0.0;     // 总市值（元）
    double turnover = 0.0;     // 换手率 %
    double sealOrder = 0.0;    // 封单额（元）
    int  boardCount = 1;       // 连板数 lbc
    int  breakCount = 0;       // 炸板次数 zbc
    QTime firstSealTime;       // 首次封板时间 fbt
    QTime lastSealTime;        // 最后封板时间 lbt
    QDate tradeDate;           // 涨停所属交易日（用于在K线上定位涨停点）
    QString sector;            // 行业板块 hybk（东财自带，仅作兜底）
    QString theme;             // 最强题材（概念板块，由 ConceptSource 注入）
    int   themeLimitCount = 0; // 该题材当日涨停家数
    double themeChangePct = 0.0; // 该题材板块涨幅 %
    int   themeRank = 0;       // 同题材内身位（1 = 龙头）
    bool  themeLeader = false; // 是否同题材最高身位
    bool isOneWordBoard = false;   // 真·一字板（open=high=low=涨停价）
    bool isSecondSeal = false;     // 秒板 / T 字板（集合竞价附近封死，非一字）
    bool broken = false;       // 是否来自炸板池
    bool hasTodayPct = false;  // changePct 是否为「今日涨跌幅」（东财昨日涨停池接口自带，可直接算溢价）

    double open = 0.0;                           // 当日开盘价（由快照回填，0 表示未知）
    double high = 0.0;                           // 当日最高价（同上）
    double low = 0.0;                            // 当日最低价（同上）

    double sealToAmountRatio() const { return amount > 0 ? sealOrder / amount : 0.0; }  // 封单额占成交额比
    double sealToFloatCapRatio() const { return floatCap > 0 ? sealOrder / floatCap : 0.0; }  // 封单额占流通市值比
};

// 市场情绪阶段：决定今天「能不能出手」，比选股更前置
enum class MarketPhase {
    Unknown = 0,                                 // 未判定
    IcePoint,      // 冰点：空仓，仅记录
    Recovery,      // 回暖：小仓试错
    MainUp,        // 主升：主攻
    Climax,        // 高潮：减仓
    Retreat,       // 退潮：空仓
};

QString marketPhaseName(MarketPhase p);          // 阶段中文名（实现见 strategy/MarketRegime.cpp）
QString marketPhaseAdvice(MarketPhase p);        // 阶段操作建议文案

// 单个交易日的全市场情绪指标
struct MarketRegime {
    QDate date;                                  // 统计所属交易日
    int limitUpCount = 0;      // 涨停家数
    int brokenCount = 0;       // 炸板家数
    int limitDownCount = 0;    // 跌停家数
    int firstBoardCount = 0;   // 首板家数
    int multiBoardCount = 0;   // 连板家数
    int maxBoardHeight = 0;    // 最高连板高度
    double brokenRate = 0.0;   // 炸板率 = 炸板 / (涨停 + 炸板)
    double advanceRate = 0.0;  // 连板晋级率（昨日连板今日继续涨停的比例）
    double firstBoardAdvanceRate = 0.0; // 首板晋级率（昨日首板今日涨停比例）
    double prevLimitUpAvgPct = 0.0;  // 昨日涨停股今日平均涨幅（溢价率）
    int prevLimitUpCount = 0;  // 昨日涨停家数（对照用）
    int premiumSampleCount = 0; // 溢价率实际采样数（快照缺失时不能假装知道）
    bool premiumKnown = false;  // 溢价率是否可信（采样覆盖率 ≥50%）
    bool yesterdayKnown = false; // 昨日数据是否已就绪
    MarketPhase phase = MarketPhase::Unknown;    // 判定的情绪阶段
    bool tradeAllowed = false; // 闸门是否放行
    QString note;              // 闸门说明（UI 直接展示）

    bool valid() const { return date.isValid(); }  // 有日期即视为有效样本
};

// 单只票的因子命中明细：UI 表格直接渲染
struct FactorHit {
    QString name;      // 因子中文名
    QString display;   // 实际值
    bool passed = false;                         // 是否通过该因子
    QString hint;      // 未通过原因 / 阈值说明
};

// 选股结果：涨停条目 + 可选的快照/K线 + 派生指标
struct Candidate {
    LimitEntry entry;                            // 涨停池原始条目
    Snapshot snapshot;      // 可选（来自全市场快照）
    KlineSeries kline;      // 可选（用于量比 / 涨停基因）

    double volRatio5 = 0.0;      // 当日量 / 前 5 日均量
    double limitCount20 = 0.0;     // 近 20 日涨停次数（涨停基因）
    double maBull = 0.0;         // 均线多头排列强度 0~1
    double prevDayChangePct = 0.0; // 涨停前一交易日涨幅（真实 T-1，不是当日）
    int listDays = 0;            // K 线可见交易日数（次新股判定用）
    int sectorLimitCount = 0;    // 同板块当日涨停家数
    double score = 0.0;          // 综合评分 0~100
    QString grade;               // 评级 A/B/C

    QVector<FactorHit> factors;  // 全部因子明细（含未通过项）
    QStringList riskFlags;       // 风险提示（亏损 / 微盘 / 高位监管 / 次新…）
    QString rejectReason;        // 若被淘汰，记录首要原因
};

// 筛选漏斗单层数据：让 UI 能摊开「每层过了几只」自证没坏
struct FunnelStage {
    QString label;        // "涨停池" / "硬排除" / "题材效应" / "分层量价" / "输出"
    int    count = 0;     // 该层**通过**的数量（不是被淘汰的数量）
    bool   isOutput = false;   // 末端（最终清单）
};

// 淘汰明细：记录某只票卡在哪一层哪一条
struct RejectRow {
    QString code;                                // 被淘汰的代码
    QString name;                                // 被淘汰的名称
    QString stage;      // L1 硬排除 / L2 题材效应 / L3 分层量价 / 风险硬排除
    QString reason;     // 具体卡在哪一条阈值
};

// 跨线程经信号传递需注册为元类型，必须放在所有类型定义之后
Q_DECLARE_METATYPE(Snapshot)                     // 注册快照
Q_DECLARE_METATYPE(LimitEntry)                   // 注册涨停条目
Q_DECLARE_METATYPE(Candidate)                    // 注册候选
Q_DECLARE_METATYPE(MarketRegime)                 // 注册情绪指标
Q_DECLARE_METATYPE(QVector<Snapshot>)            // 注册快照列表
Q_DECLARE_METATYPE(QVector<LimitEntry>)          // 注册涨停条目列表
Q_DECLARE_METATYPE(QVector<Candidate>)           // 注册候选列表
