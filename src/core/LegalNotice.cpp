#include "core/LegalNotice.h"

#include <QStringList>

namespace LegalNotice {

namespace {

Document buildDocument()
{
    Document d;
    d.product       = QStringLiteral("做多量化选股系统（DabanQuant）");
    d.title         = QStringLiteral("免责声明与风险揭示");
    // ★ 正文有任何实质改动（含新增 / 删除条款、改变责任划分），都必须提升版本号，
    //   否则已同意旧版的用户不会重新确认。
    d.version       = QStringLiteral("1.4.0");
    d.effectiveDate = QStringLiteral("2026-11-08");

    d.lead = QStringLiteral(
        "本声明是您与软件开发者（以下简称“开发者”）之间关于下载、安装、访问或使用"
        "本软件（含其后续更新版本）的法律协议。请您在继续操作前逐条完整阅读。"
        "您勾选同意选项并点击“同意并继续”，即表示您已阅读、理解并自愿接受本声明的"
        "全部内容；如您不同意本声明的任何内容，请点击“不同意并退出”并停止使用本软件。");

    d.summary = QStringLiteral(
        "股票交易属于高风险投资活动，可能导致本金全部损失。"
        "本软件输出仅为基于公开数据的量化研究信息，不构成任何投资建议，"
        "不承诺任何收益，亦不代您作出交易决策。");

    // ---------------------------------------------------------------- 一、风险揭示
    {
        Block b;
        b.heading = QStringLiteral("一、投资风险揭示");
        b.paragraphs << QStringLiteral(
            "本软件面向 A 股短线交易研究，所涉策略具有高波动、强时效、低胜率容忍度等特征。"
            "请您在充分理解下列风险后再决定是否使用，并确认自身具备相应的风险识别能力与"
            "风险承受能力：");
        b.items
            << QStringLiteral(
                   "本金损失风险：证券价格可能因市场、行业、个股基本面或流动性变化而大幅下跌；"
                   "在连续跌停、停牌或极端行情下，您可能无法及时卖出，极端情况下可能损失全部"
                   "投资本金。")
            << QStringLiteral(
                   "市场波动风险：A 股实行涨跌幅限制与 T+1 交收，价格可能因政策调整、板块轮动、"
                   "资金博弈、突发事件、境外市场联动等因素剧烈波动；历史走势与回测结果不预示"
                   "未来表现。")
            << QStringLiteral(
                   "策略与模型风险：量化评分、交易计划、次日竞价判定与复盘结论均建立在历史数据、"
                   "统计假设和固定阈值之上，可能存在样本偏差、参数失效、过拟合或信号滞后，"
                   "任何模型均不能保证持续有效。")
            << QStringLiteral(
                   "执行与操作风险：实际成交价格可能因滑点、盘口变化、涨跌停封单、网络延迟而与"
                   "计划价格不同；佣金、印花税、过户费等交易成本会侵蚀收益；误操作、设备故障、"
                   "断电断网亦可能造成损失。")
            << QStringLiteral(
                   "集中持仓与杠杆风险：本软件按小资金集中持仓口径最多推荐 2 只标的，个股波动"
                   "对组合的影响更为集中；若您使用融资融券或其他杠杆工具，亏损可能被放大并"
                   "超出初始本金。")
            << QStringLiteral(
                   "流动性与退出风险：小市值、低换手或连续涨停的标的在情绪反转时可能出现流动性"
                   "骤降，买入后无法按计划价格卖出，止损指令也可能因跌停而无法成交。");
        d.blocks.append(b);
    }

    // ---------------------------------------------------------------- 二、信息性质
    {
        Block b;
        b.heading = QStringLiteral("二、信息性质：不构成投资建议");
        b.paragraphs << QStringLiteral(
            "本软件是量化研究与决策辅助工具，不是投资顾问，也不提供证券投资咨询服务。"
            "请您准确理解本软件全部输出的性质：");
        b.items
            << QStringLiteral(
                   "研究信息：选股结果、评分、买入 / 止盈 / 止损区间、仓位建议、竞价判定、"
                   "复盘结论及 AI 解读，均为基于公开数据的量化研究信息，仅供学习、研究与"
                   "自我复盘参考。")
            << QStringLiteral(
                   "不构成投资建议：上述信息不构成任何证券的买卖要约、要约邀请、收益承诺或"
                   "推荐，不构成投资顾问服务，不能替代您的独立判断，也不能替代持牌证券投资"
                   "咨询机构的专业意见。")
            << QStringLiteral(
                   "不代客操作：本软件不接入券商交易接口，不代客下单、不代客理财、不管理或"
                   "托管任何账户资金；所有交易均应由您本人通过合法渠道独立完成。")
            << QStringLiteral(
                   "无收益承诺：本软件及其开发者不对任何收益、胜率、回撤或交易结果作出明示或"
                   "默示的保证；界面中的“建议仓位”“目标价”等表述均为研究口径，不构成对实际"
                   "成交或收益的承诺。")
            << QStringLiteral(
                   "单一来源局限：本软件不掌握您的财务状况、投资目标、风险偏好与持仓结构，"
                   "无法进行投资者适当性匹配；您应自行判断相关功能是否适合自身情况。");
        d.blocks.append(b);
    }

    // ---------------------------------------------------------------- 三、数据来源
    {
        Block b;
        b.heading = QStringLiteral("三、数据来源与准确性");
        b.paragraphs << QStringLiteral(
            "本软件的数据来自第三方公开接口。受接口能力、网络状况与披露口径限制，"
            "数据与计算结果可能存在偏差：");
        b.items
            << QStringLiteral(
                   "数据来源：行情快照、涨停 / 炸板 / 跌停池、日 K 线、概念板块及资金流等数据"
                   "来自东方财富、腾讯、新浪等第三方公开接口；本软件不运营、不控制上述数据源。")
            << QStringLiteral(
                   "可能的偏差：第三方数据可能存在延迟、缺失、重复、错误、复权口径差异、"
                   "字段单位差异或接口变更，不同数据源之间也可能互相矛盾。")
            << QStringLiteral(
                   "按现状提供：本软件按“现状”和“可得”提供数据、指标与分析结果，不对其"
                   "准确性、完整性、及时性、适用性或不侵权作出任何明示或默示保证。")
            << QStringLiteral(
                   "缺失不推算：程序对缺失数据标注“数据缺失”，不补 0、不推测；但该标注本身"
                   "不代表其余数据必然正确，仍请您以券商或交易所的官方行情与成交回报为准。")
            << QStringLiteral(
                   "使用后果：因数据错误、延迟、中断、口径差异或第三方服务变更导致的任何"
                   "决策偏差与损失，由您自行承担。");
        d.blocks.append(b);
    }

    // ---------------------------------------------------------------- 四、责任划分
    {
        Block b;
        b.heading = QStringLiteral("四、用户自主决策与责任划分");
        b.paragraphs << QStringLiteral(
            "本软件仅提供信息与计算工具，不参与您的交易决策。请您确认并遵守以下责任划分：");
        b.items
            << QStringLiteral(
                   "独立决策：是否交易、交易何种证券、买卖数量、价格与时点，均由您独立判断并"
                   "自行决定；本软件不参与、不干预、不代您作出任何交易决策。")
            << QStringLiteral(
                   "自行核对：下单前请自行核对证券代码、买卖方向、价格、数量、涨跌停与停牌"
                   "状态、可用资金及持仓，并自行设置止损、仓位上限与风险预算。")
            << QStringLiteral(
                   "合规使用：您应遵守《中华人民共和国证券法》《证券、期货投资咨询管理暂行"
                   "办法》及沪深北交易所业务规则（含程序化交易管理相关规定），不得利用本软件"
                   "从事市场操纵、内幕交易、虚假申报或其他违法违规活动。")
            << QStringLiteral(
                   "账户与密钥：账户、密码、API Key 及设备安全由您自行保管；因保管不当、"
                   "设备被入侵、第三方服务中断或您向他人泄露信息造成的损失，由您自行承担。")
            << QStringLiteral(
                   "责任归属：您违反法律法规、交易所规则或本声明使用本软件所产生的任何后果"
                   "与责任，均由您自行承担；本软件开发者不承担由此产生的任何责任。");
        d.blocks.append(b);
    }

    // ---------------------------------------------------------------- 五、法律与责任限制
    {
        Block b;
        b.heading = QStringLiteral("五、适用法律与责任限制");
        b.paragraphs << QStringLiteral(
            "本条款在适用法律允许的范围内适用。如您所在地区对本声明部分内容有强制性规定，"
            "以该等强制性规定为准：");
        b.items
            << QStringLiteral(
                   "适用法律：本声明的订立、效力、解释、履行与争议解决均适用中华人民共和国"
                   "法律（为本声明之目的，不含香港特别行政区、澳门特别行政区及台湾地区法律）。")
            << QStringLiteral(
                   "争议解决：因本声明或使用本软件产生的争议，双方应首先友好协商；协商不成"
                   "的，任何一方可向开发者住所地有管辖权的人民法院提起诉讼。")
            << QStringLiteral(
                   "责任限制：在法律允许的最大范围内，开发者不对任何间接损失、附带损失、"
                   "特殊损失、惩罚性损失、利润损失、商誉损失、数据丢失或交易损失承担责任。")
            << QStringLiteral(
                   "责任上限：若上述限制被有管辖权的机关认定不可执行，开发者就本软件承担的"
                   "累计责任总额，以您为使用本软件实际支付的费用为限；本软件免费提供的，"
                   "该上限为零。")
            << QStringLiteral(
                   "强制性规定：本条不排除或限制依照法律规定不得排除或限制的责任（包括因"
                   "故意或重大过失造成的责任）；本声明任何条款被认定无效或不可执行的，"
                   "不影响其余条款的效力。");
        d.blocks.append(b);
    }

    // ---------------------------------------------------------------- 六、更新与确认
    {
        Block b;
        b.heading = QStringLiteral("六、声明更新与确认");
        b.paragraphs << QStringLiteral(
            "为反映功能调整、数据源变化或监管要求，本声明可能更新。请您关注软件内的版本提示：");
        b.items
            << QStringLiteral(
                   "版本更新：本声明更新后，软件将重新提示确认；您需再次确认后方可继续使用。")
            << QStringLiteral(
                   "确认方式：您勾选同意选项并点击“同意并继续”，即构成您对本声明的明确同意；"
                   "该电子确认记录可作为您已阅读并同意的证明。")
            << QStringLiteral(
                   "记录保存：同意记录（声明版本号与确认时间）仅保存在您本机的配置中，"
                   "不上传至任何服务器；您可随时通过软件内的“免责声明”入口查阅本声明全文。")
            << QStringLiteral(
                   "不同意：如您不同意本声明的任何内容，请点击“不同意并退出”，并停止下载、"
                   "安装或使用本软件。");
        d.blocks.append(b);
    }

    d.footer = QStringLiteral(
        "本声明自 %1 起生效，版本 %2。"
        "开发者保留在法律允许范围内对本声明进行解释与更新的权利。")
        .arg(d.effectiveDate, d.version);
    return d;
}

} // namespace

const Document& document()
{
    static const Document doc = buildDocument();
    return doc;
}

QString plainText()
{
    const Document& d = document();
    QStringList lines;
    lines << d.product << d.title
          << QStringLiteral("版本 %1 ｜ 生效日期 %2").arg(d.version, d.effectiveDate)
          << d.summary << d.lead;
    for (const Block& b : d.blocks) {
        lines << b.heading;
        lines << b.paragraphs;
        lines << b.items;
    }
    lines << d.footer;
    return lines.join(QLatin1Char('\n'));
}

QStringList requiredTopics()
{
    // 与需求逐条对应：本金损失 / 市场波动 / 非投资建议 / 数据来源与准确性 /
    // 用户自行决策 / 适用法律与责任限制。任一项在正文中缺失，离线门禁即失败。
    return {
        QStringLiteral("本金损失风险"),
        QStringLiteral("市场波动风险"),
        QStringLiteral("不构成投资建议"),
        QStringLiteral("数据来源"),
        QStringLiteral("准确性"),
        QStringLiteral("独立决策"),
        QStringLiteral("适用中华人民共和国法律"),
        QStringLiteral("责任限制"),
    };
}

QString settingsOrganization() { return QStringLiteral("DeepInvestQuant"); }
QString settingsApplication()  { return QStringLiteral("Legal"); }
QString acceptedVersionKey()   { return QStringLiteral("disclaimer.acceptedVersion"); }
QString acceptedAtKey()        { return QStringLiteral("disclaimer.acceptedAt"); }

} // namespace LegalNotice
