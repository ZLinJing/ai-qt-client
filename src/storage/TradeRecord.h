#pragma once

#include <QDate>
#include <QString>
#include <QVector>

// ============================================================================
// TradeRecord：交易日志的一条记录（一次完整交易闭环）
//
// 打板是「T 日买、T+1 卖」的单笔闭环，所以一条记录 = 一次完整交易。
// 没有这张结构，跑一年也无法回答「我的 A 级票实际胜率多少、平均溢价多少」。
//
// 本文件只放**与存储后端无关**的数据结构与统计口径：
//   · SQLite 后端（storage/Database）与 MySQL 后端（storage/MySqlStore）共用；
//   · 费用 / 盈亏口径只有一份，已实现与浮动盈亏同源，横向可比。
// ============================================================================

// ---------------------------------------------------------------- 交易日志
struct TradeRecord {
    int     id = 0;
    QDate   buyDate;
    QDate   sellDate;
    QString code;
    QString name;
    double  buyPrice = 0.0;
    int     qty = 0;
    double  sellPrice = 0.0;
    double  planBuy = 0.0;    // 当时计划买入中枢（用于事后核对执行力）
    double  planStop = 0.0;   // 当时计划止损
    double  planTake = 0.0;   // 当时计划止盈
    QString note;

    bool closed() const { return sellDate.isValid() && sellPrice > 0.0 && qty > 0 && buyPrice > 0.0; }
    double buyAmount() const { return buyPrice * qty; }
    double sellAmount() const { return sellPrice * qty; }

    // ---------------- 费用口径（已实现 / 浮动 共用同一套） ----------------
    // 买入佣金 万 2.5；卖出佣金 万 2.5 + 印花税 千 1（合计 0.00125）。
    // 不含过户费，也不套用券商「最低 5 元」门槛 —— 客户端按比例估算，
    // 目的是让不同本金规模、不同状态的记录可以横向比较。
    static double buyFeeRate() { return 0.00025; }
    static double sellFeeRate() { return 0.00125; }

    // 以 price 作为「卖出价」时的双边费用（持仓中就用当前价预估卖出）
    double feeAt(double price) const {
        if (qty <= 0 || buyPrice <= 0.0) return 0.0;
        return buyAmount() * buyFeeRate() + qMax(0.0, price) * qty * sellFeeRate();
    }
    // 以 price 结算的盈亏：与已了结记录 pnl() 完全同源（同一费用口径）
    double pnlAt(double price) const {
        if (qty <= 0 || buyPrice <= 0.0 || price <= 0.0) return 0.0;
        return price * qty - buyAmount() - feeAt(price);
    }
    double pnlPctAt(double price) const {
        return (buyAmount() > 0.0) ? pnlAt(price) / buyAmount() * 100.0 : 0.0;
    }

    double fee() const {
        return buyAmount() * buyFeeRate() + (closed() ? sellAmount() * sellFeeRate() : 0.0);
    }
    double pnl() const { return closed() ? pnlAt(sellPrice) : 0.0; }
    double pnlPct() const { return closed() ? pnlPctAt(sellPrice) : 0.0; }
    int holdDays() const { return (closed() && buyDate.isValid()) ? buyDate.daysTo(sellDate) : 0; }
    // 持仓中：持有至今的自然日数（了结后返回 holdDays）
    int holdingDays(const QDate& ref) const {
        if (closed()) return holdDays();
        return (buyDate.isValid() && ref.isValid()) ? buyDate.daysTo(ref) : 0;
    }
    bool executedAsPlanned() const {
        // 买入价落在计划买入区间内视为「按计划执行」
        return planBuy > 0.0 && buyPrice > 0.0
            && qAbs(buyPrice - planBuy) / planBuy <= 0.03;
    }
};

struct TradeStats {
    int total = 0;
    int closedCount = 0;
    int winCount = 0;
    int planFollowed = 0;
    double winRate = 0.0;
    double avgPnlPct = 0.0;
    double avgWinPct = 0.0;
    double avgLossPct = 0.0;
    double expectancy = 0.0;     // 每笔期望收益 %
    double totalPnl = 0.0;
    double maxWin = 0.0;
    double maxLoss = 0.0;
    double maxDrawdown = 0.0;    // 累计盈亏曲线的最大回撤（%）
    double avgHoldDays = 0.0;
    double stopLossHitRate = 0.0; // 触及计划止损的比例
};

TradeStats computeTradeStats(const QVector<TradeRecord>& list);
