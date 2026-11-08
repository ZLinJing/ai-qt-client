#pragma once                                     // 防止头文件被重复包含

#include <QString>                               // 文本字段
#include <QStringList>                           // 收件人列表
#include <QVector>                               // 服务商预设列表

// 消息推送配置（SMTP）
// 与选股逻辑分离：SMTP 凭据需长期落盘且不能进源码
// 配置文件默认位于 %APPDATA%/DabanQuant/notify_config.json（当前用户私有目录）；
// 兼容读取历史部署中程序目录 / 工作目录里的旧配置，并由 PushService 自动加密迁移。
// 安全约定：授权码经 DPAPI 加密后只写 passwordEnc 字段，不落明文、不打日志、不进邮件正文。

struct MailMessage {
    QString subject;                             // 邮件主题（不含前缀）
    QString html;                                // 邮件正文（HTML）
};

// 常见邮箱服务商的 SMTP 预设：一键填好服务器 / 端口 / 加密方式并给出授权码指引
struct SmtpPreset {
    QString name;                                // 服务商名称（下拉展示）
    QString host;                                // SMTP 服务器
    int     port = 465;                          // 端口
    bool    implicitSsl = true;                  // 是否隐式 TLS（465 直连加密）
    QString hint;                                // 授权码获取方式提示
};

struct NotifyConfig {
    bool    enabled       = false;   // 总开关：关闭时自动推送不触发（手动推送仍可用）
    bool    autoPush      = true;    // 分析完成且有可执行信号时自动推送
    bool    pushWhenNoSignal = false;// 无「可执行」信号时是否也推（默认不推，避免噪音）

    QString host       = QStringLiteral("smtp.qq.com");  // SMTP 服务器
    int     port       = 465;                            // SMTP 端口
    bool    implicitSsl = true;      // true=465 隐式 TLS；false=587 STARTTLS
    QString username;                // 发件邮箱（SMTP 登录名）
    QString password;                // 授权码（仅存内存；落盘由 CredentialStore 加密）
    bool passwordNeedsUpgrade = false; // 读到历史明文配置时置位，由 PushService 立即迁移
    QString fromName   = QStringLiteral("做多量化选股系统");   // 发件人显示名
    QStringList recipients;          // 收件人；为空时回落为 username（自己发给自己）

    int     timeoutMs  = 20000;                          // 单次发送超时（毫秒）
    QString subjectPrefix = QStringLiteral("[做多量化]");  // 主题前缀

    bool ready(QString* why = nullptr) const;            // 是否具备发送条件；why 返回缺失项
    QStringList effectiveRecipients() const;             // 去重后的有效收件人

    static QString canonicalPath();                      // 规范保存路径（当前用户私有目录）
    static QString defaultPath();                        // 实际读取路径（兼容历史部署）
    static NotifyConfig load(const QString& path = QString());  // 读取配置（失败返回默认值）
    bool save(const QString& path = QString()) const;    // 写回配置

    static QVector<SmtpPreset> presets();                // QQ / 163 / 126 / 网易企业 预设
    static QString presetNameForHost(const QString& host);  // 按服务器反查服务商名（未匹配返回"自定义"）
};
