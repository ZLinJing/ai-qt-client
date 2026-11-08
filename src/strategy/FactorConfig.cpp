#include "strategy/FactorConfig.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QtGlobal>

namespace {

// 默认权重：打板核心占主导，新增五维合计 65%
constexpr double kDefaultWeights[] = { 0.35, 0.15, 0.15, 0.15, 0.10, 0.10 };

DimensionConfig defaultDim(int i)
{
    DimensionConfig d;
    d.weight = kDefaultWeights[qBound(0, i, kFactorDimensionCount - 1)];
    return d;
}

} // namespace

QString FactorConfig::modeName(FactorMode m)
{
    switch (m) {
    case FactorMode::Score:  return QStringLiteral("叠加");
    case FactorMode::Filter: return QStringLiteral("过滤");
    case FactorMode::Both:   return QStringLiteral("叠加+过滤");
    }
    return QStringLiteral("叠加");
}

QString FactorConfig::ruleName(CombineRule r)
{
    return r == CombineRule::MinPenalty
        ? QStringLiteral("木桶(75%加权+25%短板)")
        : QStringLiteral("加权求和");
}

FactorConfig FactorConfig::balanced()
{
    // 「均衡做多」：打板核心主导（35%），趋势 / 动量 / 资金流向 / 基本面 / 波动率
    // 五维合计 65% —— 即原「均衡做多」版本的参数体系，作为单一启用配置保留下来。
    FactorConfig c;
    c.dims.clear();
    for (int i = 0; i < kFactorDimensionCount; ++i) c.dims.append(defaultDim(i));
    c.validate();
    return c;
}

QString FactorConfig::strategyName()
{
    return QStringLiteral("均衡做多");
}

FactorConfig FactorConfig::defaults()
{
    return balanced();
}

void FactorConfig::validate()
{
    if (dims.size() != kFactorDimensionCount) {
        QVector<DimensionConfig> fixed;
        for (int i = 0; i < kFactorDimensionCount; ++i)
            fixed.append(i < dims.size() ? dims.at(i) : defaultDim(i));
        dims = fixed;
    }
    double total = 0.0;
    int enabledCount = 0;
    for (DimensionConfig& d : dims) {
        d.weight = qBound(0.0, d.weight, 10.0);
        d.threshold = qBound(0.0, d.threshold, 1.0);
        if (d.mode < FactorMode::Score || d.mode > FactorMode::Both)
            d.mode = FactorMode::Score;
        if (d.enabled && d.weight > 0.0) { total += d.weight; ++enabledCount; }
    }
    // 全部关闭或权重归零：强制恢复打板核心，避免输出无意义的 0 分榜单
    if (enabledCount == 0 || total <= 0.0) {
        dims[int(FactorDimension::Daban)] = defaultDim(int(FactorDimension::Daban));
    }
    if (rule < CombineRule::WeightedSum || rule > CombineRule::MinPenalty)
        rule = CombineRule::WeightedSum;
    minComposite = qBound(0.0, minComposite, 100.0);
    topN = qBound(1, topN, 200);
}

QJsonObject FactorConfig::toJson() const
{
    QJsonObject o;
    o["version"] = 1;
    o["rule"] = int(rule);
    o["minComposite"] = minComposite;
    o["requireAllAvailable"] = requireAllAvailable;
    o["topN"] = topN;
    QJsonArray arr;
    for (int i = 0; i < kFactorDimensionCount; ++i) {
        const DimensionConfig d = i < dims.size() ? dims.at(i) : defaultDim(i);
        QJsonObject item;
        item["enabled"] = d.enabled;
        item["weight"] = d.weight;
        item["threshold"] = d.threshold;
        item["mode"] = int(d.mode);
        arr.append(item);
    }
    o["dims"] = arr;
    return o;
}

FactorConfig FactorConfig::fromJson(const QJsonObject& o)
{
    FactorConfig c = defaults();
    c.rule = static_cast<CombineRule>(qBound(0, o.value("rule").toInt(0), 1));
    c.minComposite = o.value("minComposite").toDouble(0.0);
    c.requireAllAvailable = o.value("requireAllAvailable").toBool(false);
    c.topN = o.value("topN").toInt(30);

    const QJsonArray arr = o.value("dims").toArray();
    for (int i = 0; i < kFactorDimensionCount && i < arr.size(); ++i) {
        const QJsonObject item = arr.at(i).toObject();
        DimensionConfig d = c.dims[i];
        d.enabled = item.value("enabled").toBool(true);
        d.weight = item.value("weight").toDouble(d.weight);
        d.threshold = item.value("threshold").toDouble(d.threshold);
        d.mode = static_cast<FactorMode>(qBound(0, item.value("mode").toInt(0), 2));
        c.dims[i] = d;
    }
    c.validate();
    return c;
}

FactorConfig FactorConfig::fromJsonText(const QString& text)
{
    if (text.trimmed().isEmpty()) return defaults();
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return defaults();
    return fromJson(doc.object());
}

FactorConfig FactorConfig::fromLegacyVersionsText(const QString& versionsJson)
{
    // 旧版把多套权重存成 {"activeId":"...","versions":[{"id":"...","config":{...}}]}，
    // 这里只取一套：优先启用版本，其次「均衡做多」，最后第一套。
    if (versionsJson.trimmed().isEmpty()) return balanced();
    const QJsonDocument doc = QJsonDocument::fromJson(versionsJson.toUtf8());
    if (!doc.isObject()) return balanced();

    const QJsonObject root = doc.object();
    const QString activeId = root.value("activeId").toString();
    const QJsonArray versions = root.value("versions").toArray();

    QJsonObject picked;
    bool found = false;
    for (const QJsonValue& v : versions) {
        const QJsonObject vo = v.toObject();
        if (!activeId.isEmpty() && vo.value("id").toString() == activeId) {
            picked = vo;
            found = true;
            break;
        }
    }
    if (!found) {
        for (const QJsonValue& v : versions) {
            const QJsonObject vo = v.toObject();
            if (vo.value("id").toString() == QStringLiteral("balanced")) {
                picked = vo;
                found = true;
                break;
            }
        }
    }
    if (!found && !versions.isEmpty()) {
        picked = versions.first().toObject();
        found = true;
    }
    if (!found) return balanced();

    return fromJson(picked.value("config").toObject());
}

QString FactorConfig::digest() const
{
    const QByteArray raw = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(raw, QCryptographicHash::Sha1).toHex().left(16));
}

QString FactorConfig::summary() const
{
    QStringList parts;
    double total = 0.0;
    for (int i = 0; i < kFactorDimensionCount; ++i) {
        const DimensionConfig d = i < dims.size() ? dims.at(i) : defaultDim(i);
        if (d.enabled && d.weight > 0.0) total += d.weight;
    }
    for (int i = 0; i < kFactorDimensionCount; ++i) {
        const DimensionConfig d = i < dims.size() ? dims.at(i) : defaultDim(i);
        if (!d.enabled || d.weight <= 0.0) continue;
        parts << QStringLiteral("%1 %2%").arg(factorDimensionName(i))
                                        .arg(d.weight / qMax(1e-9, total) * 100.0, 0, 'f', 0);
    }
    return QStringLiteral("%1 ｜ %2 ｜ 综合分门槛 %3 ｜ Top%4")
        .arg(ruleName(rule), parts.join(QStringLiteral(" · ")))
        .arg(minComposite, 0, 'f', 0).arg(topN);
}
