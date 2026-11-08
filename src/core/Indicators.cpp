#include "core/Indicators.h"                     // 本模块声明
#include <QtMath>                                // qBound / std::round
#include <limits>                                // std::numeric_limits 取 NaN

namespace Ind {                                  // 指标工具命名空间

static const double NaN = std::numeric_limits<double>::quiet_NaN();  // 表示「该点无有效值」的哨兵

// 首板换手率分档表：按流通市值从小到大排列，matches() 按序命中
// 小盘（≤50 亿）8/10/20/25%，中盘（≤200 亿）5/6/15/20%，大盘（不限）3/3/8/15%
static const QVector<TurnoverBand> kTurnoverBands = [] {
    TurnoverBand smallCap;                       // 小盘档
    smallCap.capUpper = 50.0;                    // 流通市值 ≤ 50 亿
    smallCap.minTurnover = 8.0;                  // 合格线 8%
    smallCap.goodTurnover = 10.0;                // 黄金区间下沿 10%
    smallCap.bestTurnover = 20.0;                // 黄金区间上沿 20%
    smallCap.dangerTurnover = 25.0;              // 危险线 25%

    TurnoverBand midCap;                         // 中盘档
    midCap.capUpper = 200.0;                     // 流通市值 ≤ 200 亿
    midCap.minTurnover = 5.0;                    // 合格线 5%
    midCap.goodTurnover = 6.0;                   // 黄金区间下沿 6%
    midCap.bestTurnover = 15.0;                  // 黄金区间上沿 15%
    midCap.dangerTurnover = 20.0;                // 危险线 20%

    TurnoverBand largeCap;                       // 大盘档
    largeCap.capUpper = 0.0;                     // 不限市值上限
    largeCap.minTurnover = 3.0;                  // 合格线 3%
    largeCap.goodTurnover = 3.0;                 // 黄金区间下沿 3%
    largeCap.bestTurnover = 8.0;                 // 黄金区间上沿 8%
    largeCap.dangerTurnover = 15.0;              // 危险线 15%

    return QVector<TurnoverBand>{ smallCap, midCap, largeCap };
}();

// 双创（20cm）换手天然更高，阈值整体放大系数
static const double kTurnoverScale20cm = 1.3;

// 简单移动平均：滑动窗口求和，窗口未满处填 NaN
QVector<double> sma(const QVector<double>& v, int period)
{
    QVector<double> out(v.size(), NaN);           // 输出与输入等长，先全填 NaN
    if (period <= 0) return out;                  // 非法周期直接返回全 NaN
    double sum = 0.0;                             // 当前窗口累加和
    for (int i = 0; i < v.size(); ++i) {          // 逐点滑动
        sum += v[i];                              // 加入当前点
        if (i >= period) sum -= v[i - period];    // 移出滑出窗口的最老点
        if (i >= period - 1) out[i] = sum / period;  // 窗口已满才写入均值
    }
    return out;                                   // 返回均线序列
}

// 指数移动平均：以首点为初值，逐点按平滑系数递推
QVector<double> ema(const QVector<double>& v, int period)
{
    QVector<double> out(v.size(), NaN);           // 输出等长，默认 NaN
    if (v.isEmpty() || period <= 0) return out;   // 空序列或非法周期直接返回
    const double k = 2.0 / (period + 1.0);        // EMA 平滑系数
    double e = v.first();                         // 用首点作为递推初值
    out[0] = e;                                   // 首点 EMA 即自身
    for (int i = 1; i < v.size(); ++i) {          // 从第二个点开始递推
        e = v[i] * k + e * (1.0 - k);             // 新值 = 现值*k + 前值*(1-k)
        out[i] = e;                               // 写入结果
    }
    return out;                                   // 返回 EMA 序列
}

// 成交量移动平均：与价格均线口径一致，直接复用 sma
QVector<double> volSma(const QVector<double>& v, int period)
{
    return sma(v, period);                        // 语义别名，实现同 sma
}

// 量比 = 当日成交量 / 前 lookback 日均量
double volRatio(const KlineSeries& k, int lookback)
{
    const int n = k.candles.size();               // K 线根数
    if (n < lookback + 1 || lookback <= 0) return 0.0;  // 数据不足或周期非法
    double avg = 0.0;                             // 前 N 日均量累加
    for (int i = n - lookback - 1; i < n - 1; ++i) avg += k.candles[i].volume;  // 跳过当日，累加前 N 日
    avg /= lookback;                              // 求平均
    if (avg <= 0) return 0.0;                     // 均量为 0 无法计算比值
    return k.candles[n - 1].volume / avg;         // 当日量 / 均量
}

// 均线多头强度：MA 多头 +0.25，收盘站上快线 +0.25，基准 0.5
double maBullStrength(const KlineSeries& k, int fast, int slow)
{
    const int n = k.candles.size();               // K 线根数
    if (n < slow + 1) return 0.0;                 // 不足以算出慢线则不给分
    QVector<double> closes;                       // 收盘价序列
    closes.reserve(n);                            // 预留容量避免扩容
    for (const Candle& c : k.candles) closes.append(c.close);  // 逐根提取收盘价

    const QVector<double> f = sma(closes, fast);  // 快线
    const QVector<double> s = sma(closes, slow);  // 慢线
    const double lastF = f[n - 1];                // 快线最新值
    const double lastS = s[n - 1];                // 慢线最新值
    if (std::isnan(lastF) || std::isnan(lastS)) return 0.0;  // 任一无值则无法评分

    double score = 0.5; // 基准                 // 中性起始分
    if (lastF > lastS) score += 0.25;             // 快线在慢线上方＝多头排列
    if (closes[n - 1] > lastF) score += 0.25;     // 收盘站上快线＝强势
    return qBound(0.0, score, 1.0);               // 结果限制在 [0,1]
}

// 涨停价 = 昨收 *（1 + 涨跌幅比例），四舍五入到分
double calcLimitUpPrice(double prevClose, Board board, bool isST)
{
    double ratio = boardLimitRatio(board);        // 取板块基础涨跌幅比例
    if (isST && board == Board::MainBoard) ratio = 0.05;   // 主板 ST 限 5%
    if (isST && (board == Board::ChiNext || board == Board::StarMarket)) ratio = 0.20; // 双创 ST 同样 20%
    double px = prevClose * (1.0 + ratio);        // 未取整的涨停价
    return std::round(px * 100.0) / 100.0;        // 按分四舍五入
}

// 跌停价 = 昨收 *（1 - 涨跌幅比例），四舍五入到分
double calcLimitDownPrice(double prevClose, Board board, bool isST)
{
    double ratio = boardLimitRatio(board);        // 取板块基础涨跌幅比例
    if (isST && board == Board::MainBoard) ratio = 0.05;   // 主板 ST 限 5%
    double px = prevClose * (1.0 - ratio);        // 未取整的跌停价
    return std::round(px * 100.0) / 100.0;        // 按分四舍五入
}

// 是否涨停：与精确涨停价比较，仅留 0.011 元的浮点容差（不到一分钱）
bool isLimitUp(double close, double prevClose, Board board, bool isST)
{
    if (prevClose <= 0) return false;             // 昨收无效无法判定
    const double lp = calcLimitUpPrice(prevClose, board, isST);  // 精确涨停价
    return close >= lp - 0.011;                   // 收盘达涨停价（容忍浮点误差）即涨停
}

// 近 lookback 个交易日内涨停次数（涨停基因）
int limitUpCount(const KlineSeries& k, int lookback, Board board, bool isST)
{
    const QVector<Candle>& v = k.hasRaw() ? k.rawCandles : k.candles;  // 优先用不复权序列判定
    const int n = v.size();                       // 序列长度
    if (n < 2) return 0;                          // 不足两根无法比较
    const int start = qMax(1, n - lookback);      // 回看起点（从 1 开始才有前收）
    int cnt = 0;                                  // 涨停计数
    for (int i = start; i < n; ++i) {             // 逐日比对
        if (isLimitUp(v[i].close, v[i - 1].close, board, isST)) ++cnt;  // 判定并累加
    }
    return cnt;                                   // 返回涨停天数
}

// 第 idx 根的涨幅 %：按需选用复权或不复权口径
double barChangePct(const KlineSeries& k, int idx, bool raw)
{
    const QVector<Candle>& v = (raw && k.hasRaw()) ? k.rawCandles : k.candles;  // 选择口径
    if (idx < 1 || idx >= v.size()) return 0.0;   // 下标越界或没有前一根
    const double prev = v[idx - 1].close;         // 前一根收盘
    if (prev <= 0.0) return 0.0;                  // 前收无效
    return (v[idx].close / prev - 1.0) * 100.0;   // 涨幅百分比
}

// 涨停前一交易日（倒数第二根）的真实涨幅
double prevDayChangePct(const KlineSeries& k)
{
    const QVector<Candle>& v = k.hasRaw() ? k.rawCandles : k.candles;  // 优先不复权
    return barChangePct(k, v.size() - 2, true);   // 取倒数第二根的涨幅
}

// 近 N 日累计涨幅 %
double periodChangePct(const KlineSeries& k, int lookback)
{
    const int n = k.candles.size();               // K 线根数
    if (n < 2 || lookback <= 0) return 0.0;       // 数据不足或周期非法
    int idx = qMax(0, n - 1 - lookback);          // 区间起点下标
    double base = k.candles[idx].close;           // 区间起点收盘价
    if (base <= 0) return 0.0;                    // 基准价无效
    return (k.candles[n - 1].close / base - 1.0) * 100.0;  // 累计涨幅
}

// 近 N 日最高价
double periodHigh(const KlineSeries& k, int lookback)
{
    const int n = k.candles.size();               // K 线根数
    if (n == 0) return 0.0;                       // 空序列
    int start = qMax(0, n - lookback);            // 区间起点
    double h = k.candles[start].high;             // 以起点最高价初始化
    for (int i = start; i < n; ++i) h = qMax(h, k.candles[i].high);  // 逐根取最大
    return h;                                     // 区间最高
}

// 近 N 日最低价
double periodLow(const KlineSeries& k, int lookback)
{
    const int n = k.candles.size();               // K 线根数
    if (n == 0) return 0.0;                       // 空序列
    int start = qMax(0, n - lookback);            // 区间起点
    double l = k.candles[start].low;              // 以起点最低价初始化
    for (int i = start; i < n; ++i) l = qMin(l, k.candles[i].low);  // 逐根取最小
    return l;                                     // 区间最低
}

// 前一交易日收盘价（倒数第二根）
double prevCloseOf(const KlineSeries& k)
{
    const int n = k.candles.size();               // K 线根数
    if (n < 2) return 0.0;                        // 不足两根无前收
    return k.candles[n - 2].close;                // 返回倒数第二根收盘
}

// HHMMSS 整数 → QTime
QTime parseHms(int hms)
{
    if (hms <= 0) return QTime();                 // 非正值视为无效时间
    int hh = hms / 10000;                         // 取小时
    int mm = (hms / 100) % 100;                   // 取分钟
    int ss = hms % 100;                           // 取秒
    if (hh > 23 || mm > 59 || ss > 59) return QTime();  // 越界视为无效
    return QTime(hh, mm, ss);                     // 构造有效时间
}

// 封板时间评分：早于 bestMinute 得 1.0，晚于 worstMinute 得 0，中间线性衰减
double sealTimeScore(QTime t, int bestMinute, int worstMinute)
{
    if (!t.isValid()) return 0.0;                 // 无封板时间不得分
    int m = t.hour() * 60 + t.minute();           // 换算成当日分钟数
    if (m <= bestMinute) return 1.0;              // 足够早，满分
    if (m >= worstMinute) return 0.0;             // 过晚，零分
    return 1.0 - double(m - bestMinute) / double(worstMinute - bestMinute);  // 区间内线性插值
}

// 连板高度评分：4 板最高，随后递减体现高位风险
double heightScore(int boardCount)
{
    if (boardCount <= 1) return 0.35;             // 首板
    if (boardCount == 2) return 0.70;             // 二板
    if (boardCount == 3) return 0.90;             // 三板
    if (boardCount == 4) return 1.00;             // 四板：性价比峰值
    if (boardCount == 5) return 0.75;             // 五板：开始扣分
    if (boardCount == 6) return 0.45;             // 六板：风险显著
    return 0.20;  // 7 板以上：高风险，纯运气博弈
}

// 换手率评分：合格线以下线性衰减，黄金区间满分，超过黄金上限逐步扣到 0
double turnoverScore(double turnover, const TurnoverBand& band)
{
    if (turnover <= 0) return 0.0;                // 无换手视为无效
    if (turnover >= band.dangerTurnover) return 0.0;             // 爆量出货
    if (turnover < band.minTurnover) {            // 缩量区：低于合格线
        if (band.minTurnover <= 0) return 0.0;    // 合格线非法无法归一化
        return qBound(0.0, 0.6 * turnover / band.minTurnover, 0.6);  // 最高只给 0.6
    }
    if (turnover <= band.bestTurnover) {          // 合格线 ~ 黄金上限
        if (turnover >= band.goodTurnover) return 1.0;   // 进入黄金区间满分
        double span = qMax(1e-6, band.goodTurnover - band.minTurnover);  // 过渡区间跨度
        return 0.7 + 0.3 * (turnover - band.minTurnover) / span;         // 0.7 线性升到 1.0
    }
    double span = qMax(1e-6, band.dangerTurnover - band.bestTurnover);   // 危险衰减跨度
    return qBound(0.0, 1.0 - (turnover - band.bestTurnover) / span, 1.0);  // 1.0 线性降到 0
}

// 按流通市值挑选换手率档位，并对 20cm 股票整体放大阈值
TurnoverBand turnoverBandFor(double floatCapYi, Board board)
{
    TurnoverBand out;                             // 未命中时的默认档位
    bool found = false;                           // 是否命中档位
    for (const TurnoverBand& b : kTurnoverBands) {  // 遍历市值档
        if (b.matches(floatCapYi)) { out = b; found = true; break; }  // 命中即复制并跳出
    }
    if (!found) return out;                       // 未命中任何档位则原样返回

    if (board == Board::ChiNext || board == Board::StarMarket) {  // 双创 20cm 需放大阈值
        const double s = qMax(0.1, kTurnoverScale20cm);           // 放大系数，下限 0.1 防空
        out.minTurnover    *= s;                  // 合格线放大
        out.goodTurnover   *= s;                  // 黄金线下沿放大
        out.bestTurnover   *= s;                  // 黄金线上沿放大
        out.dangerTurnover *= s;                  // 危险线放大
    }
    return out;                                   // 返回最终档位
}

// 量能评分：1.5~3 倍为黄金区间，过大或过小都扣分
double volRatioScore(double volRatio, double lo, double hi)
{
    if (volRatio <= 0) return 0.0;                // 无量不得分
    if (volRatio < lo) return qBound(0.0, 0.6 * volRatio / qMax(1e-6, lo), 0.6);  // 缩量最高 0.6
    if (volRatio <= hi) {                         // 黄金区间内
        if (lo >= hi) return 1.0;                 // 区间退化时直接满分
        double mid = (lo + hi) / 2.0;             // 区间中点
        return volRatio <= mid ? 1.0 : qBound(0.8, 1.0 - 0.2 * (volRatio - mid) / qMax(1e-6, hi - mid), 1.0);  // 后半段缓降
    }
    if (volRatio > hi * 2.0) return 0.0;          // 超两倍上限视为异常放量
    return qBound(0.0, 0.8 - 0.8 * (volRatio - hi) / qMax(1e-6, hi), 0.8);  // 上限之外线性降分
}

} // namespace Ind                              // 命名空间结束
