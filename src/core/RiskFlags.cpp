#include "core/RiskFlags.h"                      // 本模块声明
#include "core/Indicators.h"                     // 可能用到的指标工具

namespace Risk {                                 // 风险标记命名空间

// 逐条比对可量化风险字段，命中的追加到返回列表
QStringList evaluate(const LimitEntry& e, const Snapshot& snap,   // 输入：涨停条目、快照
                     const KlineSeries* kline)                    // 可选 K 线（可为空）
{
    QStringList out;                             // 累积命中的风险条目

    if (e.name.contains(QStringLiteral("ST")) || e.name.contains(QStringLiteral("退")))
        out << QStringLiteral("ST / 退市风险：涨跌幅 5% 且随时可能停牌核查");  // ST/退市股提示

    if (snap.pe < 0.0)                           // PE 为负代表亏损
        out << QStringLiteral("业绩亏损（PE %1）：注意预告下修 / 问询函").arg(snap.pe, 0, 'f', 1);

    if (snap.pe > 150.0)                         // PE 过高提示估值风险
        out << QStringLiteral("估值过高（PE %1）").arg(snap.pe, 0, 'f', 0);

    if (e.boardCount >= 5)                       // 5 连板以上进入监管高发区
        out << QStringLiteral("%1 连板：异动监管 / 停牌核查高发区").arg(e.boardCount);

    const double capYi = e.floatCap / 1e8;       // 流通市值换算成「亿元」
    if (capYi > 0.0 && capYi < 20.0)             // 20 亿以下属微盘，进出困难
        out << QStringLiteral("流通盘仅 %1 亿：易被资金操纵，进出困难").arg(capYi, 0, 'f', 1);

    if (e.price > 0.0 && e.price < 3.0)          // 3 元以下触及退市新规敏感区
        out << QStringLiteral("低价股（%1 元）：退市新规敏感").arg(e.price, 0, 'f', 2);

    if (kline && kline->valid() && kline->candles.size() < 60)   // 上市不足 60 个交易日视为次新
        out << QStringLiteral("次新股（仅 %1 根日 K）：无套牢盘结构参考")
                   .arg(kline->candles.size());

    if (e.isOneWordBoard)                        // 一字板无换手
        out << QStringLiteral("一字板：无换手，开板抛压集中");

    if (kline) {                                 // 有 K 线才做日内振幅判断
        const KlineSeries k = kline->sliceTo(e.tradeDate);            // 截到涨停当日
        const QVector<Candle>& raw = k.hasRaw() ? k.rawCandles : k.candles;  // 优先用不复权真实价
        if (raw.size() >= 2) {                   // 至少要有前一日才能算振幅
            const double prev = raw.at(raw.size() - 2).close;         // 前一日收盘价
            const double low = raw.back().low;                        // 当日最低价
            if (prev > 0.0 && low > 0.0) {       // 价格有效才计算
                const double amp = (1.0 - low / prev) * 100.0;        // 相对前收的最大回撤百分比
                if (amp > 5.0)                   // 回撤超 5% 说明止损空间被拉大
                    out << QStringLiteral("日内曾跌 %1%：结构止损空间被拉大").arg(amp, 0, 'f', 1);
            }
        }
    }

    if (e.breakCount >= 2)                       // 当日炸板 2 次以上说明封板不坚决
        out << QStringLiteral("当日炸板 %1 次：封板决心不足").arg(e.breakCount);

    return out;                                  // 返回全部命中条目（空表示未见风险）
}

} // namespace Risk                              // 命名空间结束
