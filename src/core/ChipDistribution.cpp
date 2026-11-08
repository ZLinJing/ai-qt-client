#include "core/ChipDistribution.h"                // 筹码分布声明
#include <QtMath>                                 // qBound / qMin / qMax
#include <algorithm>                              // std::sort
#include <cmath>                                  // std::pow / std::lround

namespace Chip {                                  // 筹码分布命名空间

namespace {                                      // 内部常量

constexpr double kPeakHalfRatio  = 0.5;          // 半高定义峰区宽度（峰密度跌到一半即出峰）
constexpr double kMinPeakHeight  = 1.2;          // 峰高 / 均值 的最低倍数（过滤噪声小峰）
constexpr int    kMaxPeakSpanPct = 40;           // 单峰最大跨度（占网格比例），防止平台期无限扩张
constexpr int    kSmoothRadius   = 1;            // 峰检测前的平滑半径（格）

} // namespace

// 三角形密度函数在 [a,b] 上的积分（支撑 [L,H]，峰顶 M）
// 单日成交量在 [最低价, 最高价] 之间按三角形分布，峰顶取当日均价
//   左半段 f(x) = span/(M-L)^2 * (x-L)，右半段 f(x) = span/(H-M)^2 * (H-x)
// 注意：跨过峰顶 M 的格子要同时累加左右两段，不能只取一段。
double triIntegral(double a, double b, double L, double M, double H)
{
    if (b <= a) return 0.0;                      // 区间非法
    if (H <= L) return 0.0;                      // 无振幅
    const double span = H - L;
    double sum = 0.0;
    // 左半段：把 [a,b] 截到 [L,M] 上积分
    if (M > L) {
        const double u = qBound(L, a, M);        // 截到 [L,M]
        const double v = qBound(L, b, M);
        if (v > u) {
            const double k = span / ((M - L) * (M - L));
            sum += k * (v - u) * (v + u - 2.0 * L) * 0.5;
        }
    }
    // 右半段：把 [a,b] 截到 [M,H] 上积分
    if (H > M) {
        const double u = qBound(M, a, H);        // 截到 [M,H]
        const double v = qBound(M, b, H);
        if (v > u) {
            const double k = span / ((H - M) * (H - M));
            sum += k * (2.0 * H - v - u) * (v - u) * 0.5;
        }
    }
    return sum;
}

// 当前价下方最近的密集峰（支撑）：峰顶必须低于现价，取峰顶最高者
const Peak* Distribution::supportBelow(double price) const
{
    const Peak* best = nullptr;
    for (const Peak& p : peaks) {
        if (p.center >= price) continue;          // 峰顶不低于现价 → 不是下方支撑
        if (p.share < kMinPeakShare) continue;    // 筹码占比过低 → 噪声峰
        if (!best || p.center > best->center) best = &p;
    }
    return best;
}

// 当前价上方最近的密集峰（压力）：峰顶必须高于现价，取峰顶最低者
const Peak* Distribution::pressureAbove(double price) const
{
    const Peak* best = nullptr;
    for (const Peak& p : peaks) {
        if (p.center <= price) continue;          // 峰顶不高于现价 → 不是上方压力
        if (p.share < kMinPeakShare) continue;    // 筹码占比过低 → 噪声峰
        if (!best || p.center < best->center) best = &p;
    }
    return best;
}

// 由不复权日 K 构建筹码分布
Distribution build(const QVector<Candle>& raw, double price, double decay, int bins)
{
    Distribution d;                               // 待填充的结果

    // ① 过滤有效 K 线（必须有价格、振幅与成交量）
    QVector<Candle> ks;
    ks.reserve(raw.size());
    for (const Candle& c : raw) {
        if (c.low <= 0.0 || c.high < c.low || c.close <= 0.0 || c.volume <= 0.0) continue;
        ks.append(c);
    }
    if (ks.size() < 10 || bins < 20) return d;    // 数据太少，筹码分布不可信 → 调用方回退均线口径

    // ② 价格区间（留 2% 边界，避免边界 bin 被截断）
    double lo = ks.first().low;
    double hi = ks.first().high;
    for (const Candle& c : ks) {
        lo = qMin(lo, c.low);
        hi = qMax(hi, c.high);
    }
    if (hi <= lo) return d;                       // 价格无振幅
    const double pad = (hi - lo) * 0.02;
    lo -= pad;
    hi += pad;

    // ③ 价格网格
    const double step = (hi - lo) / (bins - 1);
    d.prices.resize(bins);
    d.volumes.assign(bins, 0.0);
    for (int i = 0; i < bins; ++i) d.prices[i] = lo + i * step;

    // ④ 逐日投放筹码：单日成交量按三角形分布摊到 [最低价, 最高价]
    const int n = ks.size();
    for (int k = 0; k < n; ++k) {
        const Candle& c = ks.at(k);
        const int age = n - 1 - k;                // 距今天数（0 = 最新一根）
        const double w = std::pow(decay, age) * c.volume;   // 越久远的筹码权重越低
        if (w <= 0.0) continue;
        const double L = c.low;
        const double H = c.high;
        if (H <= L) {                             // 一字板：无振幅，筹码全落在该价位
            const int idx = qBound(0, int(std::lround((c.close - lo) / step)), bins - 1);
            d.volumes[idx] += w;
            continue;
        }
        // 峰顶取当日均价，并夹在区间内避免退化
        double M = (c.open + c.high + c.low + c.close) * 0.25;
        const double eps = (H - L) * 0.05;
        M = qBound(L + eps, M, H - eps);
        const double total = (H - L) * 0.5;       // 三角形密度积分总量
        for (int i = 0; i < bins - 1; ++i) {
            const double a = d.prices[i];
            const double b = d.prices[i + 1];
            const double part = triIntegral(a, b, L, M, H);
            if (part > 0.0) d.volumes[i] += w * part / total;   // 按密度占比摊分
        }
    }

    // ⑤ 汇总统计
    for (int i = 0; i < bins; ++i) d.totalVolume += d.volumes[i];
    if (d.totalVolume <= 0.0) return d;           // 无有效筹码
    double cost = 0.0;
    double profit = 0.0;
    for (int i = 0; i < bins; ++i) {
        cost += d.prices[i] * d.volumes[i];
        if (price > 0.0 && d.prices[i] < price) profit += d.volumes[i];
    }
    d.avgCost = cost / d.totalVolume;
    d.profitRatio = profit / d.totalVolume;

    // ⑥ 平滑后检测局部极大（抑制单日尖峰造成的伪密集峰）
    QVector<double> sm(bins, 0.0);
    for (int i = 0; i < bins; ++i) {
        double sum = 0.0;
        int cnt = 0;
        for (int j = qMax(0, i - kSmoothRadius); j <= qMin(bins - 1, i + kSmoothRadius); ++j) {
            sum += d.volumes[j];
            ++cnt;
        }
        sm[i] = cnt > 0 ? sum / cnt : 0.0;
    }
    const double mean = d.totalVolume / bins;
    const int maxSpan = qMax(3, bins * kMaxPeakSpanPct / 100);

    // ⑦ 逐个峰做半高扩展，得到峰区上下沿
    for (int i = 1; i < bins - 1; ++i) {
        if (sm[i] <= sm[i - 1] || sm[i] < sm[i + 1]) continue;  // 非局部极大
        if (sm[i] < mean * kMinPeakHeight) continue;             // 峰太矮，忽略
        const double half = sm[i] * kPeakHalfRatio;
        int l = i;
        int r = i;
        while (l > 0 && sm[l] > half && i - l < maxSpan) --l;
        while (r < bins - 1 && sm[r] > half && r - i < maxSpan) ++r;
        Peak pk;
        pk.low    = d.prices[l];
        pk.high   = d.prices[r];
        pk.center = d.prices[i];
        double vol = 0.0;
        for (int j = l; j <= r; ++j) vol += d.volumes[j];
        pk.volume = vol;
        pk.share  = vol / d.totalVolume;
        d.peaks.append(pk);
    }
    if (d.peaks.isEmpty()) return d;              // 无峰 → 视为筹码分布不可用

    // ⑧ 合并重叠峰（保留筹码量更大者），再按筹码量降序输出
    std::sort(d.peaks.begin(), d.peaks.end(),
              [](const Peak& a, const Peak& b) { return a.center < b.center; });
    QVector<Peak> merged;
    for (const Peak& p : d.peaks) {
        if (!merged.isEmpty() && p.low <= merged.last().high) {
            Peak& m = merged.last();
            if (p.volume > m.volume) {            // 新峰更强，整体替换
                m.low = qMin(m.low, p.low);
                m.high = qMax(m.high, p.high);
                m.center = p.center;
                m.volume = p.volume;
            } else {                              // 旧峰更强，只吸收区间
                m.high = qMax(m.high, p.high);
                m.volume = qMax(m.volume, p.volume);
            }
            m.share = m.volume / d.totalVolume;
            continue;
        }
        merged.append(p);
    }
    d.peaks = merged;
    std::sort(d.peaks.begin(), d.peaks.end(),
              [](const Peak& a, const Peak& b) { return a.volume > b.volume; });

    d.valid = true;                               // 标记可用
    return d;
}

} // namespace Chip                              // 命名空间结束
