#pragma once                                     // 防止头文件被重复包含

#include "notify/NotifyConfig.h"                 // 依赖 NotifyConfig / MailMessage
#include <QObject>                               // 基类（信号槽）
#include <QString>                               // 文本

class QSslSocket;                                // 前向声明：避免头文件引入网络模块
class QTimer;                                    // 前向声明：超时定时器

// SmtpClient：最小可用 SMTP 客户端（QSslSocket，异步状态机）
// 只发纯 HTML 邮件，AUTH LOGIN + DATA 足够；不额外引入依赖；全流程可日志化
// 支持：465 隐式 TLS / 587 STARTTLS / 多收件人 / UTF-8 主题正文（encoded-word + base64）
// 安全：密码只以 base64 出现在报文里，日志中一律打码

class SmtpClient : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit SmtpClient(QObject* parent = nullptr);  // 构造：建超时定时器
    ~SmtpClient() override;                      // 析构：断开并中止套接字

    void send(const NotifyConfig& cfg, const MailMessage& mail);  // 异步发信，结果走 finished
    void probe(const NotifyConfig& cfg);         // 只做连接 + EHLO 探活（配置页自检）
    void abort(const QString& reason = QString());  // 中止当前会话

    bool busy() const { return m_busy; }         // 是否正在处理

Q_SIGNALS:
    void transcript(const QString& line);        // 协议日志（密码已打码）
    void finished(bool ok, const QString& detail);  // 最终结果

private:
    enum class Mode { Send, Probe };             // 发信 / 探活
    enum class State { Idle, Greeting, Ehlo, StartTls,     // 状态机各阶段
                       AuthLogin, AuthUser, AuthPass,      // AUTH LOGIN 两问两答
                       MailFrom, RcptTo, Data, Body, Quit };

    void start();                                // 建立连接并启动状态机
    void onReadyRead();                          // 解析服务器应答
    void onSocketError();                        // 套接字错误收口
    void onTimeout();                            // 超时收口
    void onReply(int code, const QString& text); // 按当前状态处理应答
    void sendLine(const QString& line, bool redact = false);  // 发送命令行（可打码）
    void finish(bool ok, const QString& detail); // 统一收尾（清资源 + 发信号）
    QString buildMime() const;                   // 组装 MIME 报文
    static QString base64Wrapped(const QByteArray& raw);  // base64 按 76 列折行

    Mode m_mode = Mode::Send;                    // 当前模式
    State m_state = State::Idle;                 // 当前状态
    NotifyConfig m_cfg;                          // 本次使用的配置
    MailMessage m_mail;                          // 本次发送的邮件
    QSslSocket* m_sock = nullptr;                // 会话套接字
    QTimer* m_timer = nullptr;                   // 超时定时器
    QByteArray m_buf;                            // 应答缓冲区（按 CRLF 切行）
    QString m_replyText;                         // 多行应答累积文本
    int m_replyCode = 0;                         // 多行应答的起始码
    int m_rcptIndex = 0;                         // 当前收件人下标
    bool m_startTlsDone = false;                 // STARTTLS 是否已完成
    bool m_busy = false;                         // 是否有会话在跑
    bool m_done = false;                         // 是否已收尾（防重复回调）
};
