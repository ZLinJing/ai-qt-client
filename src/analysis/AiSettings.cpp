#include "analysis/AiSettings.h"

#include "notify/CredentialStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {

constexpr const char* kFileName = "ai_config.json";

bool printableAscii(const QString& s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u < 0x21 || u > 0x7e) return false;   // 可见 ASCII，排除空白与控制字符
    }
    return true;
}

} // namespace

QString AiSettings::canonicalPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + QLatin1String(kFileName);
}

bool AiSettings::validateKey(const QString& key, QString* why)
{
    auto fail = [why](const QString& text) {
        if (why) *why = text;
        return false;
    };
    const QString k = key.trimmed();
    if (k.isEmpty())        return fail(QStringLiteral("未填写 API Key"));
    if (k.size() < 8)       return fail(QStringLiteral("API Key 长度不足 8 位，请检查是否粘贴完整"));
    if (k.size() > 256)     return fail(QStringLiteral("API Key 超过 256 位，疑似粘贴了多余内容"));
    if (!printableAscii(k)) return fail(QStringLiteral("API Key 含空白或不可见字符，请重新粘贴"));
    if (why) why->clear();
    return true;
}

bool AiSettings::validateEndpoint(const QString& url, QString* why)
{
    auto fail = [why](const QString& text) {
        if (why) *why = text;
        return false;
    };
    const QString u = url.trimmed();
    const QUrl parsed(u);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return fail(QStringLiteral("接口地址不是合法的 URL（例：https://api.openai.com/v1）"));
    if (!parsed.userInfo().isEmpty() || parsed.hasQuery() || parsed.hasFragment())
        return fail(QStringLiteral("接口地址不应包含账号信息、查询串或片段"));
    const QString host = parsed.host().toLower();
    if (parsed.scheme() == QLatin1String("http")) {
        if (host != QLatin1String("localhost") && host != QLatin1String("127.0.0.1"))
            return fail(QStringLiteral("仅本机地址可用 http；外部服务必须使用 https"));
    } else if (parsed.scheme() != QLatin1String("https")) {
        return fail(QStringLiteral("接口地址必须以 https:// 开头"));
    }
    if (why) why->clear();
    return true;
}

bool AiSettings::validateModel(const QString& model, QString* why)
{
    auto fail = [why](const QString& text) {
        if (why) *why = text;
        return false;
    };
    const QString m = model.trimmed();
    if (m.isEmpty())        return fail(QStringLiteral("未填写模型名称"));
    if (m.size() > 128)     return fail(QStringLiteral("模型名称超过 128 位"));
    if (!printableAscii(m)) return fail(QStringLiteral("模型名称含空白或非法字符"));
    if (why) why->clear();
    return true;
}

bool AiSettings::ready(QString* why) const
{
    // 逐项校验，why 返回首个不满足项；未配置时这条文案会直接展示给用户
    return validateKey(apiKey, why)
        && validateEndpoint(baseUrl, why)
        && validateModel(model, why)
        && [this, why]() {
               if (timeoutMs >= 3000 && timeoutMs <= 120000) return true;
               if (why) *why = QStringLiteral("超时设置应在 3000~120000 毫秒之间");
               return false;
           }();
}

QString AiSettings::maskedKey() const
{
    const QString k = apiKey.trimmed();
    if (k.isEmpty()) return QStringLiteral("未配置");
    if (k.size() <= 8) return QStringLiteral("已配置（已隐藏）");
    return QStringLiteral("%1****%2").arg(k.left(3), k.right(4));
}

QString AiSettings::endpoint() const
{
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    return base + QStringLiteral("/chat/completions");
}

AiSettings AiSettings::load(const QString& path)
{
    AiSettings cfg;
    const QString file = path.isEmpty() ? canonicalPath() : path;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return cfg;      // 文件不存在 = 未配置

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return cfg;
    const QJsonObject o = doc.object();

    // 只读密文字段：即使历史文件里出现明文字段也绝不加载
    cfg.apiKey    = CredentialStore::unprotect(o.value(QStringLiteral("apiKeyEnc")).toString());
    cfg.baseUrl   = o.value(QStringLiteral("baseUrl")).toString(cfg.baseUrl);
    cfg.model     = o.value(QStringLiteral("model")).toString(cfg.model);
    cfg.timeoutMs = o.value(QStringLiteral("timeoutMs")).toInt(cfg.timeoutMs);
    return cfg;
}

bool AiSettings::save(const QString& path) const
{
    QString why;
    if (!ready(&why)) return false;                    // 校验不通过一律不落盘

    const QString enc = CredentialStore::protect(apiKey.trimmed());
    if (enc.isEmpty()) return false;                   // 加密失败绝不退回明文

    QJsonObject o;
    o[QStringLiteral("apiKeyEnc")] = enc;             // 只写密文
    o[QStringLiteral("baseUrl")]   = baseUrl.trimmed();
    o[QStringLiteral("model")]     = model.trimmed();
    o[QStringLiteral("timeoutMs")] = timeoutMs;

    const QString file = path.isEmpty() ? canonicalPath() : path;
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool AiSettings::clear(const QString& path)
{
    const QString file = path.isEmpty() ? canonicalPath() : path;
    if (!QFile::exists(file)) return true;
    return QFile::remove(file);
}
