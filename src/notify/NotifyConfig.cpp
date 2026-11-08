#include "notify/NotifyConfig.h"         // 本模块声明

#include <QCoreApplication>              // 程序目录
#include <QDir>                          // 当前工作目录
#include <QFile>                         // 存在性检测
#include <QJsonArray>                    // 收件人数组
#include <QJsonDocument>                 // JSON 读写
#include <QJsonObject>                   // 配置对象
#include <QSaveFile>                     // 原子写入，避免写坏配置
#include <QStandardPaths>                // 用户私有数据目录
#include "notify/CredentialStore.h"      // 授权码加密

namespace {

QString configFileName()                 // 配置文件名（固定）
{
    return QStringLiteral("notify_config.json");
}

QString appDir()                         // 程序所在目录（无 QCoreApplication 时退回工作目录）
{
    return QCoreApplication::instance() ? QCoreApplication::applicationDirPath()
                                        : QDir::currentPath();
}

} // namespace

// 规范保存路径：当前用户私有目录（不落在 Program Files / 共享目录）
QString NotifyConfig::canonicalPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + configFileName();
}

// 读取路径：优先用户私有目录；兼容历史部署中的程序目录 / 工作目录旧文件
QString NotifyConfig::defaultPath()
{
    const QString canonical = canonicalPath();
    if (QFile::exists(canonical)) return canonical;

    const QStringList legacy{ appDir() + QLatin1Char('/') + configFileName(),
                              QDir::currentPath() + QLatin1Char('/') + configFileName() };
    for (const QString& p : legacy) {
        if (QFile::exists(p)) return p;
    }
    return canonical;                        // 都不存在时保存到用户私有目录
}

// 是否具备发送条件：逐项校验并在 why 中写入首个缺失项
bool NotifyConfig::ready(QString* why) const
{
    auto fail = [why](const QString& text) {  // 失败辅助：写入原因并返回 false
        if (why) *why = text;
        return false;
    };
    if (host.trimmed().isEmpty()) return fail(QStringLiteral("未填写 SMTP 服务器"));  // 服务器必填
    if (port <= 0 || port > 65535) return fail(QStringLiteral("SMTP 端口非法"));       // 端口合法区间
    if (username.trimmed().isEmpty())                                                 // 发件邮箱必填
        return fail(QStringLiteral("未填写发件邮箱（例如 123456@qq.com）"));
    if (!username.contains(QLatin1Char('@')))                                         // 邮箱需含 @
        return fail(QStringLiteral("发件邮箱格式不正确"));
    if (password.isEmpty())                                                           // 授权码必填
        return fail(QStringLiteral("未填写授权码（QQ 邮箱：设置 → 账户 → 开启 SMTP 后获取）"));
    const QStringList to = effectiveRecipients();                                     // 有效收件人
    if (to.isEmpty()) return fail(QStringLiteral("未填写收件人"));                     // 至少一个收件人
    for (const QString& addr : to) {                                                  // 逐个校验格式
        if (!addr.contains(QLatin1Char('@')))
            return fail(QStringLiteral("收件人格式不正确：%1").arg(addr));
    }
    if (why) why->clear();                   // 全部通过则清空原因
    return true;                             // 具备发送条件
}

// 有效收件人：去空白 + 去重；为空时回落为发件邮箱
QStringList NotifyConfig::effectiveRecipients() const
{
    QStringList out;                         // 结果
    for (const QString& raw : recipients) {  // 遍历配置的收件人
        const QString addr = raw.trimmed();  // 去空白
        if (!addr.isEmpty() && !out.contains(addr, Qt::CaseInsensitive)) out << addr;  // 非空且未重复则收录
    }
    if (out.isEmpty() && !username.trimmed().isEmpty()) out << username.trimmed();  // 无收件人则发给自己
    return out;                              // 返回结果
}

// 读取配置：文件不存在或格式错误时返回默认值
NotifyConfig NotifyConfig::load(const QString& path)
{
    NotifyConfig cfg;                        // 默认配置
    const QString file = path.isEmpty() ? defaultPath() : path;  // 解析目标路径
    QFile f(file);                           // 打开配置文件
    if (!f.open(QIODevice::ReadOnly)) return cfg;  // 打不开则直接返回默认值

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());  // 解析 JSON
    if (!doc.isObject()) return cfg;         // 非对象则返回默认值
    const QJsonObject o = doc.object();      // 配置对象

    auto str = [&o](const char* key, const QString& def) {   // 读字符串字段（类型不符用默认值）
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isString() ? v.toString() : def;
    };
    auto num = [&o](const char* key, int def) {              // 读整型字段
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isDouble() ? v.toInt() : def;
    };
    auto flag = [&o](const char* key, bool def) {            // 读布尔字段
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isBool() ? v.toBool() : def;
    };

    cfg.enabled          = flag("enabled", cfg.enabled);          // 总开关
    cfg.autoPush         = flag("autoPush", cfg.autoPush);        // 自动推送
    cfg.pushWhenNoSignal = flag("pushWhenNoSignal", cfg.pushWhenNoSignal);  // 无信号也推
    cfg.host             = str("host", cfg.host);                 // 服务器
    cfg.port             = num("port", cfg.port);                 // 端口
    cfg.implicitSsl      = flag("implicitSsl", cfg.implicitSsl);  // 隐式 TLS
    cfg.username         = str("username", cfg.username);         // 发件邮箱
    const QString protectedPwd = str("passwordEnc", QString());
    if (!protectedPwd.isEmpty()) {
        cfg.password = CredentialStore::unprotect(protectedPwd);  // 新格式：DPAPI 密文
    } else {
        const QString legacy = str("password", QString());
        if (!legacy.isEmpty()) {                                  // 历史明文：读入后立即迁移
            cfg.password = legacy;
            cfg.passwordNeedsUpgrade = true;
        }
    }
    cfg.fromName         = str("fromName", cfg.fromName);         // 发件人名称
    cfg.timeoutMs        = num("timeoutMs", cfg.timeoutMs);       // 超时
    cfg.subjectPrefix    = str("subjectPrefix", cfg.subjectPrefix);  // 主题前缀

    cfg.recipients.clear();                  // 收件人先清空再重建
    for (const QJsonValue& v : o.value(QStringLiteral("recipients")).toArray()) {  // 遍历数组
        const QString addr = v.toString().trimmed();   // 去空白
        if (!addr.isEmpty()) cfg.recipients << addr;   // 非空才收录
    }
    return cfg;                              // 返回读到的配置
}

// 常见邮箱服务商预设（QQ / 网易全系均使用 465 隐式 TLS + 授权码）
QVector<SmtpPreset> NotifyConfig::presets()
{
    QVector<SmtpPreset> out;                     // 预设列表
    // QQ 邮箱：需先在「设置 → 账户」开启 SMTP 服务，并生成 16 位授权码
    out.append({ QStringLiteral("QQ 邮箱"),
                 QStringLiteral("smtp.qq.com"), 465, true,
                 QStringLiteral("QQ 邮箱：设置 → 账户 → 开启「SMTP 服务」→ 生成授权码（16 位），"
                                "授权码即密码，登录密码无效") });
    // 网易 163 邮箱：需在「设置 → POP3/SMTP/IMAP」开启服务并新增授权码
    out.append({ QStringLiteral("网易 163 邮箱"),
                 QStringLiteral("smtp.163.com"), 465, true,
                 QStringLiteral("网易 163：设置 → POP3/SMTP/IMAP → 开启 SMTP → 新增授权码，"
                                "授权码即密码；若提示 554，请把发件人加入白名单") });
    // 网易 126 邮箱：与 163 同源，服务器域名不同
    out.append({ QStringLiteral("网易 126 邮箱"),
                 QStringLiteral("smtp.126.com"), 465, true,
                 QStringLiteral("网易 126：设置 → POP3/SMTP/IMAP → 开启 SMTP → 新增授权码，"
                                "授权码即密码") });
    // 网易企业邮箱：同样走 465，也可用 994（隐式 TLS）
    out.append({ QStringLiteral("网易企业邮箱"),
                 QStringLiteral("smtp.qiye.163.com"), 465, true,
                 QStringLiteral("网易企业邮：管理后台 → 客户端设置 → 开启 SMTP，"
                                "使用「客户端授权码」登录（端口 465 或 994）") });
    return out;                                  // 返回预设
}

// 按服务器地址反查服务商名称，用于回显下拉框
QString NotifyConfig::presetNameForHost(const QString& host)
{
    const QString h = host.trimmed().toLower();  // 归一化比较
    for (const SmtpPreset& p : presets()) {      // 逐个匹配
        if (p.host.toLower() == h) return p.name;
    }
    return QStringLiteral("自定义");             // 未匹配即自定义
}

// 保存配置：字段名与 load 严格对应
bool NotifyConfig::save(const QString& path) const
{
    QJsonObject o;                           // 待写入对象
    o["enabled"] = enabled;                  // 总开关
    o["autoPush"] = autoPush;                // 自动推送
    o["pushWhenNoSignal"] = pushWhenNoSignal;// 无信号也推
    o["host"] = host;                        // 服务器
    o["port"] = port;                        // 端口
    o["implicitSsl"] = implicitSsl;          // 隐式 TLS
    o["username"] = username;                // 发件邮箱
    // 授权码只以 DPAPI 密文落盘；加密失败则拒绝保存，绝不退回明文
    if (password.isEmpty()) {
        o["passwordEnc"] = QString();        // 清空凭据，旧授权码立即失效
    } else {
        const QString enc = CredentialStore::protect(password);
        if (enc.isEmpty()) return false;
        o["passwordEnc"] = enc;
    }
    o["fromName"] = fromName;                // 发件人名称
    o["timeoutMs"] = timeoutMs;              // 超时
    o["subjectPrefix"] = subjectPrefix;      // 主题前缀
    QJsonArray arr;                          // 收件人数组
    for (const QString& r : recipients) arr.append(r);  // 逐个写入
    o["recipients"] = arr;                   // 挂到对象上

    const QString file = path.isEmpty() ? canonicalPath() : path;  // 默认写到用户私有目录
    QSaveFile f(file);                       // 原子写（先写临时文件再替换）
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;  // 打不开则失败
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));          // 缩进格式便于人工核对
    return f.commit();                       // 提交成功才算保存成功
}
