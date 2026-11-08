#pragma once                                     // 防止头文件被重复包含
#include "core/Types.h"                          // 依赖 KlineSeries / Candle / Board
#include <QVector>                               // 指标序列使用 QVector<double>

// 纯计算指标层：不做 IO，不依赖 Qt GUI，可单元测试

namespace Ind {                                  // 指标工具命名空间

// 首板换手率分档（按流通市值分层的合格线 / 黄金区间 / 危险线）
struct TurnoverBand {
    double capUpper = 0.0;      // 流通市值上限（亿元）；<=0 表示不限
    double minTurnover = 0.0;   // 合格线 %
    double goodTurnover = 0.0;  // 黄金区间下限 %
    double bestTurnover = 0.0;  // 黄金区间上限 %
    double dangerTurnover = 0.0;// 危险线 %（超过即淘汰）

    bool matches(double floatCapYi) const {   // 判断该市值是否落入本档
        return capUpper <= 0.0 || floatCapYi <= capUpper;  // 不限档或未超上限即命中
    }
};

// 简单移动平均；返回与输入等长，前 period-1 个为 NaN
QVector<double> sma(const QVector<double>& v, int period);   // 输入序列 + 周期

// 指数移动平均
QVector<double> ema(const QVector<double>& v, int period);   // 输入序列 + 周期

// 成交量移动平均
QVector<double> volSma(const QVector<double>& v, int period);  // 本质是 sma 的语义封装

// 计算「当日成交量 / 前 N 日均量」（量比 / 放量倍数）；数据不足返回 0
double volRatio(const KlineSeries& k, int lookback = 5);      // 默认取前 5 日均量

// 均线多头排列强度 0~1：完全多头(MA5>MA10 且收盘>MA5)=1.0，完全空头=0.0
double maBullStrength(const KlineSeries& k, int fast = 5, int slow = 20);  // 快慢均线周期

// 涨停基因：统计最近 N 个交易日内收盘达涨停阈值的天数
// 优先用不复权序列，避免除权日被误判成涨停/非涨停
int limitUpCount(const KlineSeries& k, int lookback = 20, Board board = Board::MainBoard,  // 回看天数 + 板块
                 bool isST = false);         // ST 股涨跌幅规则不同

// 第 idx 根 K 线相对前一根的涨幅 %（默认用不复权口径）
double barChangePct(const KlineSeries& k, int idx, bool raw = true);  // 下标 + 是否用不复权

// 「涨停前一交易日」的涨幅 %（T-1 真实涨幅，非涨停当日）
double prevDayChangePct(const KlineSeries& k);   // 只传序列，内部取倒数第二根

// 计算涨停价：昨收 * (1 + ratio)，四舍五入到分
double calcLimitUpPrice(double prevClose, Board board, bool isST = false);  // 昨收 + 板块 + 是否 ST

// 计算跌停价
double calcLimitDownPrice(double prevClose, Board board, bool isST = false);  // 同上

// 是否为有效涨停（按 0.01 最小变动单位判定，不用百分比容差）
bool isLimitUp(double close, double prevClose, Board board, bool isST = false);  // 收盘价 + 昨收

// 近 N 日累计涨幅 %
double periodChangePct(const KlineSeries& k, int lookback);   // 回看天数

// 近 N 日最高价 / 最低价
double periodHigh(const KlineSeries& k, int lookback);        // 区间最高
double periodLow(const KlineSeries& k, int lookback);         // 区间最低

// 前一交易日收盘价（序列中倒数第二个）
double prevCloseOf(const KlineSeries& k);                     // 数据不足返回 0

// 把 HHMMSS 整数（如 92500）转为 QTime
QTime parseHms(int hms);                                      // 非法值返回无效 QTime

// 封板时间评分：越早越高（0~1）。10:30 前满分，之后线性衰减到 13:00 归零
double sealTimeScore(QTime t, int bestMinute = 570, int worstMinute = 690);  // 570=9:30 后基准，690=11:30

// 连板高度评分：2 板起递增，6 板以上扣分（高位风险）
double heightScore(int boardCount);                            // 输入连板数

// 换手率评分：黄金区间得高分，过低或过高都扣分
double turnoverScore(double turnover, const TurnoverBand& band);  // 实际换手 + 所在档位区间

// 按流通市值选档，并对 20cm（创业板/科创板）放大换手阈值
TurnoverBand turnoverBandFor(double floatCapYi, Board board);  // 流通市值(亿) + 板块

// 量能评分：1.5~3 倍为黄金
double volRatioScore(double volRatio, double lo = 1.5, double hi = 3.0);  // 量比 + 黄金区间

} // namespace Ind                              // 命名空间结束
