#pragma once                                     // 防止头文件被重复包含
#include "core/Types.h"                          // 依赖 LimitEntry（连板数、炸板次数）
#include <QString>                               // basis / note 文案字段

struct AuctionRule {
    bool   hasRule = false;       // 规则是否有效（无规则时不做竞价判定）
    double minOpenPct = 0.0;      // 可接受的最低高开幅度（%）
    double maxOpenPct = 0.0;      // 追高上限（%），超过说明溢价已被吃光
    double abandonPct = 0.0;      // 跌破即放弃（%）
    double minAmountRatio = 0.02; // 竞价成交额 / 昨日成交额 下限
    double strongAmountRatio = 0.05;  // 认定为「量能强」的竞价额占比
    QString basis;                // 规则来源说明（UI 可直接展示）
};

struct AuctionAdvice {
    AuctionRule rule;                            // 被评估的规则副本
    bool hasRule = false;                        // 是否有规则可评估

    bool evaluated = false;       // true = 已拿到真实竞价数据（当日 9:25 之后）
    bool amountKnown = false;     // 竞价成交额是否可单独取得
    double openPct = 0.0;         // 实际高开 %
    double amountRatio = 0.0;     // 竞价成交额 / 昨日成交额

    enum Verdict { Unknown = 0, Relay, Watch, Abandon };  // 待竞价 / 可接力 / 观察 / 放弃
    Verdict verdict = Unknown;                   // 最终裁决
    QString note;                 // 结论文案
};

namespace Auction {                              // 竞价决策命名空间

// 依据连板高度与是否炸板生成竞价规则（弱转强标的放宽下限）
AuctionRule ruleFor(const LimitEntry& e);        // 输入：涨停条目

// 结合真实竞价数据判定；quote 不可用时只返回规则（evaluated=false）
// amountKnown=false 表示竞价成交额无法单独取得，此时只按高开幅度判定
AuctionAdvice evaluate(const AuctionRule& rule, bool hasQuote,   // 规则 + 是否有竞价行情
                       double open, double prevClose,            // 竞价开盘价 + 昨收价
                       double auctionAmount, double prevAmount,  // 竞价成交额 + 昨日成交额
                       bool amountKnown = true);                 // 竞价量能是否可信

QString verdictName(AuctionAdvice::Verdict v);   // 裁决枚举 → 中文标签

} // namespace Auction                           // 命名空间结束
