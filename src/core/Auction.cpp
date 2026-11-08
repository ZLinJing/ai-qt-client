#include "core/Auction.h" // 竞价评估模块声明（原文件此行缺少 # 号，已修复）

namespace Auction {                              // 竞价决策命名空间

namespace {                                      // 竞价阈值（固定口径）

constexpr double kAbandonPct        = -3.5;  // 低开跌破即放弃（%）
constexpr double kMinAmountRatio    = 0.02;  // 竞价额 / 昨日成交额 下限
constexpr double kStrongAmountRatio = 0.05;  // 认定为「量能强」的竞价比

constexpr double kFirstMinOpenPct   = -2.0;  // 首板高开下限
constexpr double kFirstMaxOpenPct   =  5.0;  // 首板高开上限
constexpr double kSecondMinOpenPct  =  0.0;  // 二板平开以下不接
constexpr double kSecondMaxOpenPct  =  7.0;  // 二板高开上限
constexpr double kHighMinOpenPct    =  2.0;  // 三板以上高开下限
constexpr double kHighMaxOpenPct    =  8.0;  // 三板以上高开上限

} // namespace

// 将竞价裁决枚举转换为可读中文标签
QString verdictName(AuctionAdvice::Verdict v)
{
    switch (v) {                                 // 逐一映射裁决枚举
    case AuctionAdvice::Relay:   return QStringLiteral("可接力");   // 符合接力条件，可执行买入
    case AuctionAdvice::Watch:   return QStringLiteral("观察");     // 条件未完全满足，需盘中确认
    case AuctionAdvice::Abandon: return QStringLiteral("放弃");     // 明确不符合，禁止介入
    default:                     return QStringLiteral("待竞价");   // 尚未获取竞价数据
    }
}

// 根据连板高度生成当日竞价规则
AuctionRule ruleFor(const LimitEntry& e)
{
    AuctionRule r;                               // 待填充的规则
    r.hasRule = true;                            // 有连板数据即可生成规则
    r.abandonPct = kAbandonPct;                  // 低开放弃阈值（%）
    r.minAmountRatio = kMinAmountRatio;          // 竞价额/昨成交额最低比例
    r.strongAmountRatio = kStrongAmountRatio;    // 强势竞价量能比例

    if (e.boardCount <= 1) {                     // 首板：标准接力区间
        r.minOpenPct = kFirstMinOpenPct;         // 首板高开下限
        r.maxOpenPct = kFirstMaxOpenPct;         // 首板高开上限
        r.basis = QStringLiteral("首板进二板：高开 %1%~%2% 接力；竞价额≥昨日成交额 %3%")
                      .arg(r.minOpenPct, 0, 'f', 1)   // 填入下限
                      .arg(r.maxOpenPct, 0, 'f', 1)   // 填入上限
                      .arg(r.minAmountRatio * 100, 0, 'f', 1);  // 比例转百分比
    } else if (e.boardCount == 2) {              // 二板：分歧消化要求更高
        r.minOpenPct = kSecondMinOpenPct;        // 二板平开以下不接
        r.maxOpenPct = kSecondMaxOpenPct;        // 二板高开上限
        r.basis = QStringLiteral("二板进三板：高开 %1%~%2%（低于区间说明分歧未消化完）；"
                                 "竞价额≥%3%")
                      .arg(r.minOpenPct, 0, 'f', 1)   // 填入下限
                      .arg(r.maxOpenPct, 0, 'f', 1)   // 填入上限
                      .arg(r.minAmountRatio * 100, 0, 'f', 1);  // 比例转百分比
    } else {                                     // 三板及以上：高位，上限即兑现线
        r.minOpenPct = kHighMinOpenPct;          // 高位高开下限
        r.maxOpenPct = kHighMaxOpenPct;          // 高位高开上限
        r.basis = QStringLiteral("三板以上：高开 %1%~%2%，超过上沿按兑现处理（不做接力）")
                      .arg(r.minOpenPct, 0, 'f', 1)   // 填入下限
                      .arg(r.maxOpenPct, 0, 'f', 1);  // 填入上限
    }

    if (e.broken || e.breakCount >= 2) {         // 封板质量差需修正区间
        if (e.boardCount <= 2) {                 // 低位炸板后高开＝弱转强
            r.minOpenPct -= 2.0;                 // 放宽下限 2 个百分点
            r.basis += QStringLiteral("；炸板 %1 次按弱转强放宽下限至 %2%")
                           .arg(qMax(1, e.breakCount))    // 炸板次数至少记 1 次
                           .arg(r.minOpenPct, 0, 'f', 1); // 放宽后的下限
        } else {                                 // 高位多次开板＝分歧巨大
            r.maxOpenPct -= 1.5;                 // 收紧追高上限 1.5 个百分点
            r.basis += QStringLiteral("；高位开板 %1 次，追高上限收紧至 %2%")
                           .arg(e.breakCount)              // 开板次数
                           .arg(r.maxOpenPct, 0, 'f', 1);  // 收紧后的上限
        }
    }
    return r;                                    // 返回生成的规则
}

// 基于竞价实盘数据评估是否满足接力条件
AuctionAdvice evaluate(const AuctionRule& rule, bool hasQuote,   // 规则 + 是否有竞价行情
                       const double open, const double prevClose,  // 开盘价 + 昨收价
                       const double auctionAmount, const double prevAmount,  // 竞价额 + 昨日额
                       const bool amountKnown)                 // 竞价量能是否可信
{
    AuctionAdvice a;                             // 待填充的建议
    a.rule = rule;                               // 记录规则副本
    a.hasRule = rule.hasRule;                    // 传递规则有效性
    if (!rule.hasRule) return a;                 // 无规则直接返回空建议

    if (!hasQuote || open <= 0.0 || prevClose <= 0.0) {   // 竞价数据缺失或价格异常
        a.verdict = AuctionAdvice::Unknown;      // 标记为待竞价
        a.note = QStringLiteral("待次日 9:25 竞价确认：%1").arg(rule.basis);  // 提示等待
        return a;                                // 不给出买卖结论
    }

    a.evaluated = true;                          // 已有真实竞价数据
    a.openPct = (open / prevClose - 1.0) * 100.0; // 计算开盘涨幅%
    a.amountKnown = amountKnown;                 // 记录量能是否可信
    if (amountKnown && prevAmount > 0.0)         // 量能可信且基数有效才计算占比
        a.amountRatio = auctionAmount / prevAmount; // 竞价额占比

    if (a.openPct < rule.abandonPct) {           // ① 低开破位：严禁接飞刀
        a.verdict = AuctionAdvice::Abandon;      // 直接放弃
        a.note = QStringLiteral("竞价低开 %1%（< %2%），接力盘已崩，放弃")
                     .arg(a.openPct, 0, 'f', 2)       // 实际高开幅度
                     .arg(rule.abandonPct, 0, 'f', 1); // 放弃阈值
        return a;                                // 提前返回
    }
    if (a.openPct > rule.maxOpenPct) {           // ② 高开过头：溢价已被透支
        a.verdict = AuctionAdvice::Abandon;      // 不接兑现盘
        a.note = QStringLiteral("竞价高开 %1%（> %2%），溢价已被吃光，不接")
                     .arg(a.openPct, 0, 'f', 2)       // 实际高开幅度
                     .arg(rule.maxOpenPct, 0, 'f', 1); // 追高上限
        return a;                                // 提前返回
    }
    if (a.openPct < rule.minOpenPct) {           // ③ 高开不足：分歧未消化
        a.verdict = AuctionAdvice::Watch;        // 需盘中确认
        a.note = QStringLiteral("竞价高开 %1%，低于接力区间下沿 %2%，需分时放量上攻再确认")
                     .arg(a.openPct, 0, 'f', 2)       // 实际高开幅度
                     .arg(rule.minOpenPct, 0, 'f', 1); // 区间下沿
        return a;                                // 提前返回
    }
    if (amountKnown && a.amountRatio < rule.minAmountRatio) {  // ④ 区间内但承接不足
        a.verdict = AuctionAdvice::Watch;        // 继续观察
        a.note = QStringLiteral("竞价额仅占昨日成交额 %1%（< %2%），承接不足，观察")
                     .arg(a.amountRatio * 100, 0, 'f', 2)    // 实际占比
                     .arg(rule.minAmountRatio * 100, 0, 'f', 1);  // 占比下限
        return a;                                // 提前返回
    }

    if (!amountKnown) {                          // ⑤ 无量能数据：只确认价格区间
        a.verdict = AuctionAdvice::Relay;        // 给出可接力结论
        a.note = QStringLiteral("竞价高开 %1%，落在接力区间；竞价成交额未单独采集，请自行核对量能")
                     .arg(a.openPct, 0, 'f', 2); // 填入实际高开幅度
        return a;                                // 提前返回
    }

    a.verdict = AuctionAdvice::Relay;            // 全部检查通过
    a.note = QStringLiteral("竞价高开 %1%，竞价额占昨日成交额 %2%，符合接力条件")
                 .arg(a.openPct, 0, 'f', 2)      // 实际高开幅度
                 .arg(a.amountRatio * 100, 0, 'f', 2);  // 实际量能占比
    if (a.amountRatio >= rule.strongAmountRatio) // 量能强弱决定仓位
        a.note += QStringLiteral("（竞价量能强，可正常仓位）");      // 量能充足
    else
        a.note += QStringLiteral("（竞价量能一般，建议半仓试错）");  // 量能一般降仓
    return a;                                    // 返回最终建议
}

} // namespace Auction                           // 命名空间结束
