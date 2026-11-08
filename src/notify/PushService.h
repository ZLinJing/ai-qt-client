#pragma once                                     // 防止头文件被重复包含

#include "notify/NotifyConfig.h"                 // 依赖 NotifyConfig / MailMessage
#include "core/TradePlan.h"                      // 依赖 RecommendBundle
#include "sentiment/SentimentTypes.h"            // 依赖 SentimentState
#include <QObject>                               // 基类（信号槽）
#include <QString>                               // 文本

class Database;                                  // 前向声明：去重指纹落库
class SmtpClient;                                // 前向声明：实际发信

// PushService：把「一次荐股结果」整理成一封邮件并投递
// 只做「读结果 → 排版 → 交给 SmtpClient」，不做选股、不改计划、不碰 UI
// 自动推送三重闸门：总开关 / 有可执行信号 / 同日同内容去重（盘中每 60 秒重算，无去重会打爆邮箱）
// 凭据落 notify_config.json，去重指纹落 SQLite

class PushService : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit PushService(QObject* parent = nullptr);  // 构造：建 SMTP 客户端并连线

    void setDatabase(Database* db) { m_db = db; }     // 注入数据库（用于去重）

    NotifyConfig& config() { return m_cfg; }          // 可写配置引用（设置页编辑用）
    const NotifyConfig& config() const { return m_cfg; }  // 只读配置引用

    bool loadConfig();                                // 从磁盘加载配置
    bool saveConfig();                                // 保存配置到磁盘
    bool busy() const { return m_busy; }              // 是否正在发送

    // manual=true：用户点「推送」，忽略总开关与「无信号不推」，但仍做配置校验与内容去重以外的一切检查
    void pushBundle(const RecommendBundle& bundle, const SentimentState& state, bool manual);
    void sendTest();                                  // 发送一封测试邮件

    void sendCustom(const QString& subject, const QString& html);  // 发任意 HTML（早盘简报/复盘），不参与去重

    // 纯排版（不发信）：供预览 / 自检 / 导出复用，保证「预览所见 = 实际所发」
    static MailMessage compose(const NotifyConfig& cfg, const RecommendBundle& bundle,
                               const SentimentState& state);

Q_SIGNALS:
    void log(const QString& line);                    // 过程日志
    void finished(bool ok, const QString& detail);    // 发送结果
    void busyChanged(bool busy);                      // 忙闲状态变化

private:
    void dispatch(const QString& subject, const QString& html, const QString& digest);  // 实际发信
    static QString buildSubject(const NotifyConfig& cfg, const RecommendBundle& bundle,  // 生成主题
                                const SentimentState& state);
    static QString buildHtml(const RecommendBundle& bundle, const SentimentState& state);  // 生成 HTML 正文
    static QString digestOf(const RecommendBundle& bundle);                             // 内容指纹

    static bool retryable(const QString& detail);  // 判断失败是否值得自动重试

    NotifyConfig m_cfg;                          // 当前配置
    Database* m_db = nullptr;                    // 数据库（可为空）
    SmtpClient* m_smtp = nullptr;                // SMTP 客户端
    QString m_pendingDigest;   // 发送成功后才写入去重表
    MailMessage m_lastMsg;                       // 最近一次待发内容（重试用）
    int m_retryLeft = 0;                         // 剩余自动重试次数
    bool m_busy = false;                         // 是否正在发送
};
