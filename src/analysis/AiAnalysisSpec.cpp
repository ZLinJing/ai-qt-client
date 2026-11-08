#include "analysis/AiAnalysisSpec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

namespace AiSpec {
namespace {

const QStringList& keys()
{
    static const QStringList k = {
        QStringLiteral("tradeDate"),          QStringLiteral("code"),
        QStringLiteral("name"),               QStringLiteral("buyLow"),
        QStringLiteral("buyHigh"),            QStringLiteral("takeProfitLow"),
        QStringLiteral("takeProfitHigh"),     QStringLiteral("stopLossLow"),
        QStringLiteral("stopLossHigh"),       QStringLiteral("riskReward"),
        QStringLiteral("positionPct"),        QStringLiteral("score"),
        QStringLiteral("actionStatus"),       QStringLiteral("riskFlags"),
        QStringLiteral("sentimentComposite"), QStringLiteral("positionCapPct"),
        QStringLiteral("gateAllowed"),        QStringLiteral("localDiagnosis")
    };
    return k;
}

const QSet<QString>& numericKeys()
{
    static const QSet<QString> k = {
        QStringLiteral("buyLow"), QStringLiteral("buyHigh"),
        QStringLiteral("takeProfitLow"), QStringLiteral("takeProfitHigh"),
        QStringLiteral("stopLossLow"), QStringLiteral("stopLossHigh"),
        QStringLiteral("riskReward"), QStringLiteral("positionPct"),
        QStringLiteral("score"), QStringLiteral("sentimentComposite"),
        QStringLiteral("positionCapPct")
    };
    return k;
}

const QSet<QString>& textKeys()
{
    static const QSet<QString> k = {
        QStringLiteral("tradeDate"), QStringLiteral("code"), QStringLiteral("name"),
        QStringLiteral("actionStatus"), QStringLiteral("localDiagnosis")
    };
    return k;
}

QString clip(const QString& s, int max)
{
    QString out = s.trimmed();
    if (out.size() > max) out = out.left(max);
    return out;
}

QString esc(const QString& s) { return s.toHtmlEscaped(); }

bool exactKeys(const QJsonObject& o, const QStringList& expected, QString* error)
{
    const QStringList actual = o.keys();
    for (const QString& k : actual) {
        if (!expected.contains(k)) {
            if (error) *error = QStringLiteral("出现约定之外的字段「%1」").arg(k);
            return false;
        }
    }
    for (const QString& k : expected) {
        if (!o.contains(k)) {
            if (error) *error = QStringLiteral("缺少字段「%1」").arg(k);
            return false;
        }
    }
    return true;
}

bool stringListField(const QJsonObject& o, const QString& key, int maxCount,
                     int maxLen, QStringList* out, QString* error)
{
    const QJsonValue v = o.value(key);
    if (!v.isArray()) {
        if (error) *error = QStringLiteral("字段「%1」应为数组").arg(key);
        return false;
    }
    const QJsonArray arr = v.toArray();
    if (arr.size() > maxCount) {
        if (error) *error = QStringLiteral("字段「%1」最多 %2 项").arg(key).arg(maxCount);
        return false;
    }
    for (const QJsonValue& item : arr) {
        if (!item.isString() || item.toString().isEmpty()
            || item.toString().length() > maxLen) {
            if (error) *error = QStringLiteral("字段「%1」的每项应为 1~%2 字文本").arg(key).arg(maxLen);
            return false;
        }
        out->append(item.toString().trimmed());
    }
    return true;
}

} // namespace

QString goalText()
{
    return QStringLiteral(
        "只解读本次提供的单个结构化计划快照：核对区间数值是否自洽、复述计划已写明的纪律、"
        "指出快照中可见的风险。不预测价格与涨跌，不生成新的买入/卖出/仓位指令，"
        "不选股，不替代情绪闸门与仓位计算。");
}

QString inputScopeText()
{
    return QStringLiteral(
        "只能使用 user 消息中给定的 JSON 字段，且字段必须来自白名单：%1。\n"
        "禁止使用外部知识、实时行情、网络检索、账户/资金/个人信息、API Key、"
        "原始 K 线序列、快照全量数据，以及白名单之外的任何文本（备注、理由、依据长文）；"
        "字段值为 null 表示本地未提供，一律按「数据不足」处理，不得推断或补齐。")
        .arg(keys().join(QStringLiteral("、")));
}

QString outputSchemaText()
{
    return QStringLiteral(
        "只输出一个 JSON 对象，不要 Markdown、不要代码围栏、不要额外解释文字；全中文。\n"
        "键固定为 conclusion / summary / evidence / risks / discipline / uncertainty，"
        "不得增加或缺少键。\n"
        "conclusion 只能是「一致」「存疑」「数据不足」之一；\n"
        "evidence 为数组（≤5），每项形如 {\"field\":\"白名单字段名\",\"observation\":\"≤60字\"}，"
        "field 必须取自本次给定字段；\n"
        "risks（≤5）、discipline（≤3）为字符串数组，每项 ≤60 字；\n"
        "summary ≤120 字，uncertainty ≤80 字。");
}

QString principlesText()
{
    return QStringLiteral(
        "1. 每条结论都必须能在 evidence 中找到对应输入字段，只引用给定数字与标记，不得编造数据。\n"
        "2. 只复述计划已有的区间与纪律，不给目标价、不给胜率、不给买卖时点，不构成投资建议。\n"
        "3. 输入字段互相矛盾（例如止损下沿 ≥ 买入上沿）→ conclusion = 存疑；关键字段缺失 → 数据不足。\n"
        "4. 与本地规则诊断不一致时以本地计算结果为准，只标注差异，不改写任何数值。\n"
        "5. 每次请求独立、不保留历史上下文、温度固定为 0；不回显密钥与请求头，不产生额外请求。");
}

QString systemPrompt()
{
    return QStringLiteral("你是量化选股客户端的 AI 分析助手，只做「结构化计划快照的一致性解读」。\n\n")
         + QStringLiteral("【分析目标】\n") + goalText()
         + QStringLiteral("\n\n【输入信息范围】\n") + inputScopeText()
         + QStringLiteral("\n\n【输出内容结构】\n") + outputSchemaText()
         + QStringLiteral("\n\n【判断原则】\n") + principlesText();
}

QString scopeHtml()
{
    const QString block = QStringLiteral(
        "<div style='margin:0 0 8px 0'><b>%1</b><br><span style='color:#4a5260'>%2</span></div>");
    return block.arg(QStringLiteral("分析目标"), esc(goalText()).replace(QStringLiteral("\n"), QStringLiteral("<br>")))
         + block.arg(QStringLiteral("输入信息范围"), esc(inputScopeText()).replace(QStringLiteral("\n"), QStringLiteral("<br>")))
         + block.arg(QStringLiteral("输出内容结构"), esc(outputSchemaText()).replace(QStringLiteral("\n"), QStringLiteral("<br>")))
         + block.arg(QStringLiteral("判断原则"), esc(principlesText()).replace(QStringLiteral("\n"), QStringLiteral("<br>")));
}

QStringList allowedInputKeys() { return keys(); }

QString inputCatalogText()
{
    return QStringLiteral(
        "tradeDate 交易日 / code 代码 / name 名称 / buyLow~buyHigh 买入区间 / "
        "takeProfitLow~takeProfitHigh 止盈区间 / stopLossLow~stopLossHigh 止损区间 / "
        "riskReward 盈亏比 / positionPct 建议仓位 / score 综合评分 / actionStatus 执行状态 / "
        "riskFlags 风险标记 / sentimentComposite 复合情绪 / positionCapPct 有效总仓上限 / "
        "gateAllowed 情绪闸门 / localDiagnosis 本地规则诊断摘要");
}

QJsonObject sanitizeInput(const QJsonObject& raw)
{
    QJsonObject out;
    for (const QString& key : keys()) {
        const QJsonValue v = raw.value(key);
        if (numericKeys().contains(key)) {
            out.insert(key, v.isDouble() ? QJsonValue(v.toDouble()) : QJsonValue());
        } else if (textKeys().contains(key)) {
            const int limit = (key == QLatin1String("localDiagnosis")) ? 120 : 60;
            out.insert(key, v.isString() ? QJsonValue(clip(v.toString(), limit)) : QJsonValue());
        } else if (key == QLatin1String("gateAllowed")) {
            out.insert(key, v.isBool() ? QJsonValue(v.toBool()) : QJsonValue());
        } else {  // riskFlags
            QJsonArray arr;
            if (v.isArray()) {
                const QJsonArray in = v.toArray();
                for (int i = 0; i < in.size() && i < 8; ++i) {
                    if (in.at(i).isString()) arr.append(clip(in.at(i).toString(), 40));
                }
            }
            out.insert(key, arr);
        }
    }
    return out;                                     // 白名单外的字段已全部丢弃
}

QJsonObject buildRequestBody(const QString& model, const QJsonObject& sanitizedInput)
{
    const QJsonObject safe = sanitizeInput(sanitizedInput);   // 再过滤一次，防止调用方绕过
    QJsonObject systemMsg, userMsg;
    systemMsg.insert(QStringLiteral("role"), QStringLiteral("system"));
    systemMsg.insert(QStringLiteral("content"), systemPrompt());
    userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMsg.insert(QStringLiteral("content"),
                   QStringLiteral("结构化输入（JSON）：\n%1\n请严格按 system 约定输出。")
                       .arg(QString::fromUtf8(QJsonDocument(safe).toJson(QJsonDocument::Compact))));
    QJsonArray messages;
    messages.append(systemMsg);
    messages.append(userMsg);
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("temperature"), 0);
    body.insert(QStringLiteral("max_tokens"), 900);
    body.insert(QStringLiteral("messages"), messages);
    return body;
}

QJsonObject buildValidationBody(const QString& model)
{
    QJsonObject systemMsg, userMsg;
    systemMsg.insert(QStringLiteral("role"), QStringLiteral("system"));
    systemMsg.insert(QStringLiteral("content"),
                     QStringLiteral("只输出 {\"ok\":true}，不要其它文字。"));
    userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMsg.insert(QStringLiteral("content"), QStringLiteral("连通性校验"));
    QJsonArray messages;
    messages.append(systemMsg);
    messages.append(userMsg);
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("temperature"), 0);
    body.insert(QStringLiteral("max_tokens"), 16);
    body.insert(QStringLiteral("messages"), messages);
    return body;
}

bool parseResult(const QByteArray& raw, Result* out, QString* error)
{
    auto fail = [error](const QString& text) {
        if (error) *error = text;
        return false;
    };
    QByteArray text = raw.trimmed();
    // 兼容少数模型仍会包一层代码围栏；其余情况绝不宽松放行
    if (text.startsWith("```")) {
        const int firstNl = text.indexOf('\n');
        if (firstNl < 0) return fail(QStringLiteral("输出不是有效 JSON"));
        text = text.mid(firstNl + 1).trimmed();
        if (text.endsWith("```")) text.chop(3);
        text = text.trimmed();
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(text, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("输出不是有效 JSON 对象"));
    const QJsonObject o = doc.object();
    if (!exactKeys(o, { QStringLiteral("conclusion"), QStringLiteral("summary"),
                        QStringLiteral("evidence"), QStringLiteral("risks"),
                        QStringLiteral("discipline"), QStringLiteral("uncertainty") }, error))
        return false;

    const QString conclusion = o.value(QStringLiteral("conclusion")).toString();
    static const QStringList kConclusions = { QStringLiteral("一致"), QStringLiteral("存疑"),
                                              QStringLiteral("数据不足") };
    if (!kConclusions.contains(conclusion))
        return fail(QStringLiteral("conclusion 只能是「一致 / 存疑 / 数据不足」"));
    if (!o.value(QStringLiteral("summary")).isString()
        || !o.value(QStringLiteral("uncertainty")).isString())
        return fail(QStringLiteral("summary / uncertainty 应为文本"));
    const QString summary = o.value(QStringLiteral("summary")).toString().trimmed();
    const QString uncertainty = o.value(QStringLiteral("uncertainty")).toString().trimmed();
    if (summary.isEmpty() || summary.size() > 120)
        return fail(QStringLiteral("summary 应为 1~120 字"));
    if (uncertainty.isEmpty() || uncertainty.size() > 80)
        return fail(QStringLiteral("uncertainty 应为 1~80 字"));

    const QJsonArray evidence = o.value(QStringLiteral("evidence")).toArray();
    if (!o.value(QStringLiteral("evidence")).isArray() || evidence.size() > 5)
        return fail(QStringLiteral("evidence 应为不超过 5 项的数组"));
    QStringList evidenceLines;
    for (const QJsonValue& item : evidence) {
        if (!item.isObject()) return fail(QStringLiteral("evidence 每项应为对象"));
        const QJsonObject e = item.toObject();
        if (!exactKeys(e, { QStringLiteral("field"), QStringLiteral("observation") }, error))
            return false;
        const QString field = e.value(QStringLiteral("field")).toString();
        const QString observation = e.value(QStringLiteral("observation")).toString().trimmed();
        if (!keys().contains(field))
            return fail(QStringLiteral("evidence 引用了白名单外的字段「%1」").arg(field));
        if (observation.isEmpty() || observation.size() > 60)
            return fail(QStringLiteral("evidence.observation 应为 1~60 字"));
        evidenceLines << QStringLiteral("%1：%2").arg(field, observation);
    }

    QStringList risks, discipline;
    if (!stringListField(o, QStringLiteral("risks"), 5, 60, &risks, error)) return false;
    if (!stringListField(o, QStringLiteral("discipline"), 3, 60, &discipline, error)) return false;

    if (!out) return fail(QStringLiteral("内部错误：结果容器为空"));
    *out = Result{ conclusion, summary, evidenceLines, risks, discipline, uncertainty };
    if (error) error->clear();
    return true;
}

QString renderResult(const Result& r)
{
    const QString head = QStringLiteral(
        "<div style='font-size:15px;font-weight:700;color:#1b2430;margin-bottom:8px'>"
        "AI 分析结论：%1</div>").arg(esc(r.conclusion));
    auto list = [](const QStringList& items) {
        QString s = QStringLiteral("<ul style='margin:2px 0 8px 18px;padding:0'>");
        for (const QString& x : items) s += QStringLiteral("<li>%1</li>").arg(esc(x));
        return s + QStringLiteral("</ul>");
    };
    auto section = [](const QString& title, const QString& body) {
        return QStringLiteral("<div style='margin-bottom:6px'><b>%1</b><br>%2</div>")
            .arg(esc(title), body);
    };
    return head
         + section(QStringLiteral("① 结论摘要"), QStringLiteral("<div>%1</div>").arg(esc(r.summary)))
         + section(QStringLiteral("② 依据（仅引用给定字段）"),
                   r.evidence.isEmpty() ? QStringLiteral("<div>无</div>") : list(r.evidence))
         + section(QStringLiteral("③ 可见风险"),
                   r.risks.isEmpty() ? QStringLiteral("<div>无</div>") : list(r.risks))
         + section(QStringLiteral("④ 计划纪律（复述，不新增指令）"),
                   r.discipline.isEmpty() ? QStringLiteral("<div>无</div>") : list(r.discipline))
         + section(QStringLiteral("⑤ 不确定性"), QStringLiteral("<div>%1</div>").arg(esc(r.uncertainty)))
         + QStringLiteral("<div style='color:#8a93a1;font-size:11px;margin-top:8px'>"
                          "本结果只解读已给出的结构化快照，不构成投资建议；数值以本地计算为准。</div>");
}

} // namespace AiSpec
