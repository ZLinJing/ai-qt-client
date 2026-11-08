#include "notify/SmtpClient.h"           // 本模块声明

#include <QDateTime>                     // 报文 Date 头
#include <QHostInfo>                     // 取本机名用于 EHLO
#include <QLocale>                       // 英文日期格式
#include <QSslSocket>                    // TLS 套接字
#include <QSslError>                     // 证书错误详情
#include <QList>                         // 证书错误列表
#include <QTimer>                        // 超时定时器
#include <QUuid>                         // 生成 Message-ID

namespace {

// EHLO 用的本机名：必须是 ASCII 域名，否则 QQ/网易 会直接回 501 拒绝整个会话
QString localHostName()
{
    QString name = QHostInfo::localHostName().trimmed().toLower();  // 取本机名并归一化
    for (int i = 0; i < name.size(); ++i) {                          // 逐字符过滤
        const QChar c = name.at(i);
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                     || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                     || c == QLatin1Char('-') || c == QLatin1Char('.');
        if (!ok) { name.clear(); break; }                            // 含中文/下划线等非法字符则整体弃用
    }
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('.')))
        return QStringLiteral("dabanquant.local");                   // 兜底为固定合法域名
    return name;                                                     // 返回可用的 ASCII 域名
}

} // namespace

SmtpClient::SmtpClient(QObject* parent) : QObject(parent)
{
    m_timer = new QTimer(this);                       // 超时定时器
    m_timer->setSingleShot(true);                     // 只触发一次
    connect(m_timer, &QTimer::timeout, this, &SmtpClient::onTimeout);  // 到点判超时
}

SmtpClient::~SmtpClient()
{
    if (m_sock) {                                     // 有会话则清理
        m_sock->disconnect(this);                     // 断开信号，避免析构期间回调
        m_sock->abort();                              // 立即中止连接
    }
}

// 发信入口
void SmtpClient::send(const NotifyConfig& cfg, const MailMessage& mail)
{
    if (m_busy) {                                     // 已有会话在跑
        Q_EMIT finished(false, QStringLiteral("上一条邮件仍在发送中，请稍候"));
        return;                                       // 直接拒绝
    }
    m_cfg = cfg;                                      // 记录配置
    m_mail = mail;                                    // 记录邮件
    m_mode = Mode::Send;                              // 发信模式
    start();                                          // 启动状态机
}

// 探活入口：只连接 + EHLO
void SmtpClient::probe(const NotifyConfig& cfg)
{
    if (m_busy) {                                     // 已有会话在跑
        Q_EMIT finished(false, QStringLiteral("正在发送中，请稍候"));
        return;                                       // 拒绝
    }
    m_cfg = cfg;                                      // 记录配置
    m_mail = MailMessage();                           // 探活无正文
    m_mode = Mode::Probe;                             // 探活模式
    start();                                          // 启动状态机
}

// 初始化会话：清状态、建套接字、连信号、起超时、发起连接
void SmtpClient::start()
{
    m_busy = true;                                    // 标记忙
    m_done = false;                                   // 复位收尾标记
    m_state = State::Greeting;                        // 首态：等问候
    m_buf.clear();                                    // 清空缓冲
    m_replyText.clear();                              // 清空多行应答
    m_replyCode = 0;                                  // 复位应答码
    m_rcptIndex = 0;                                  // 复位收件人下标
    m_startTlsDone = false;                           // 复位 STARTTLS 标记

    m_sock = new QSslSocket(this);                    // 新套接字
    connect(m_sock, &QSslSocket::readyRead, this, &SmtpClient::onReadyRead);  // 数据到达
    connect(m_sock, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onSocketError();                              // 错误统一收口
    });
    connect(m_sock, &QSslSocket::encrypted, this, [this]() {  // TLS 握手成功
        if (m_state == State::StartTls) {             // 仅在 STARTTLS 之后有效
            m_startTlsDone = true;                    // 标记已完成
            sendLine(QStringLiteral("EHLO ") + localHostName());  // 加密后重新 EHLO
            m_state = State::Ehlo;                    // 回到 EHLO 状态
        }
    });
    // 显式拒绝证书异常：不连接此信号时 Qt 会静默放行，凭据可能被发给中间人
    connect(m_sock, &QSslSocket::sslErrors, this,
            [this](const QList<QSslError>& errors) {
                const QString detail = errors.isEmpty() ? QStringLiteral("未知证书错误")
                                                        : errors.first().errorString();
                finish(false, QStringLiteral("TLS 证书校验失败：%1").arg(detail));
            });

    Q_EMIT transcript(QStringLiteral("→ 连接 %1:%2（%3）")
                          .arg(m_cfg.host)                        // 服务器
                          .arg(m_cfg.port)                        // 端口
                          .arg(m_cfg.implicitSsl ? QStringLiteral("SSL/TLS")
                                                 : QStringLiteral("STARTTLS")));  // 加密方式
    m_timer->start(qMax(5000, m_cfg.timeoutMs));      // 启动超时（至少 5 秒）

    if (m_cfg.implicitSsl) m_sock->connectToHostEncrypted(m_cfg.host, quint16(m_cfg.port));  // 465 隐式 TLS
    else                   m_sock->connectToHost(m_cfg.host, quint16(m_cfg.port));           // 587 明文后升级
}

// 套接字错误收口
void SmtpClient::onSocketError()
{
    if (m_done) return;                               // 已收尾则忽略
    const QString err = m_sock ? m_sock->errorString() : QStringLiteral("网络错误");  // 取错误描述
    finish(false, QStringLiteral("连接失败：%1").arg(err));  // 统一失败
}

// 超时收口
void SmtpClient::onTimeout()
{
    if (m_done) return;                               // 已收尾则忽略
    finish(false, QStringLiteral("SMTP 超时（%1 ms 未完成），请检查网络 / 端口 / 授权码")
                     .arg(m_cfg.timeoutMs));
}

// 外部中止
void SmtpClient::abort(const QString& reason)
{
    finish(false, reason.isEmpty() ? QStringLiteral("已取消") : reason);  // 以失败结束
}

// 发送一条命令行
void SmtpClient::sendLine(const QString& line, bool redact)
{
    if (!m_sock) return;                              // 无连接则忽略
    Q_EMIT transcript(redact ? QStringLiteral("C: <已隐藏>") : QStringLiteral("C: %1").arg(line));  // 打日志（敏感行打码）
    m_sock->write((line + QStringLiteral("\r\n")).toUtf8());  // 写入并按协议补 CRLF
    m_sock->flush();                                  // 立即冲刷
}

// 解析服务器应答：按行切分，处理多行应答
void SmtpClient::onReadyRead()
{
    if (!m_sock || m_done) return;                    // 已收尾则忽略
    m_buf += m_sock->readAll();                       // 追加到缓冲区

    int idx = -1;                                     // 行结束位置
    while ((idx = m_buf.indexOf("\r\n")) >= 0) {      // 逐行处理
        const QString line = QString::fromUtf8(m_buf.left(idx));  // 取出一行
        m_buf.remove(0, idx + 2);                     // 从缓冲移除
        if (line.isEmpty()) continue;                 // 空行忽略

        if (line.size() >= 4 && line.at(3) == QLatin1Char('-')) {   // 多行应答（如 EHLO 能力列表）
            if (m_replyCode == 0) m_replyCode = line.left(3).toInt();  // 记住起始码
            m_replyText += line.mid(4) + QLatin1Char('\n');            // 累积后续文本
            continue;                                 // 等下一行
        }

        const int code = line.left(3).toInt();        // 末行的应答码
        m_replyText += line.mid(qMin(4, line.size()));  // 拼接末行文本
        const QString text = m_replyText.trimmed();   // 完整应答文本
        m_replyText.clear();                          // 清空累积
        m_replyCode = 0;                              // 复位起始码
        onReply(code, text);                          // 交给状态机处理
        if (m_done || !m_sock) return;                // 会话可能已结束
    }
}

// 状态机：按应答码推进协议
void SmtpClient::onReply(int code, const QString& text)
{
    Q_EMIT transcript(QStringLiteral("S: %1 %2").arg(code).arg(text.left(160)));  // 日志截断 160 字符

    if (code >= 400) {                                // 4xx/5xx 一律视为失败
        finish(false, QStringLiteral("服务器拒绝（%1）：%2").arg(code).arg(text));
        return;                                       // 结束
    }

    const QStringList to = m_cfg.effectiveRecipients();  // 有效收件人

    switch (m_state) {                                // 按状态分发
    case State::Greeting:                             // 等待 220 问候
        if (code != 220) {                            // 非 220 视为异常
            finish(false, QStringLiteral("服务器问候异常（%1）：%2").arg(code).arg(text));
            return;                                   // 结束
        }
        sendLine(QStringLiteral("EHLO ") + localHostName());  // 发送 EHLO
        m_state = State::Ehlo;                        // 进入 EHLO 状态
        break;

    case State::Ehlo:                                 // EHLO 应答
        if (!m_cfg.implicitSsl && !m_startTlsDone) {  // 明文连接需先升级 TLS
            if (!text.contains(QStringLiteral("STARTTLS"), Qt::CaseInsensitive)) {  // 服务器不支持
                finish(false, QStringLiteral("服务器不支持 STARTTLS，请改用 465 端口"));
                return;                               // 结束
            }
            sendLine(QStringLiteral("STARTTLS"));     // 请求升级
            m_state = State::StartTls;                // 进入升级状态
        } else if (m_mode == Mode::Probe) {           // 探活模式到此为止
            sendLine(QStringLiteral("QUIT"));         // 直接退出
            m_state = State::Quit;                    // 进入退出状态
        } else {                                      // 发信模式开始认证
            sendLine(QStringLiteral("AUTH LOGIN"));   // 请求登录
            m_state = State::AuthLogin;               // 进入认证状态
        }
        break;

    case State::StartTls:                             // STARTTLS 应答
        if (code != 220) {                            // 非 220 视为拒绝
            finish(false, QStringLiteral("STARTTLS 被拒绝（%1）").arg(code));
            return;                                   // 结束
        }
        Q_EMIT transcript(QStringLiteral("→ 开始 TLS 握手"));
        m_sock->startClientEncryption();              // 异步握手：成功走 encrypted()，失败走 errorOccurred()
        break;

    case State::AuthLogin:      // 服务器提示「用户名:」
        if (code != 334) {                            // 非 334 说明不接受 AUTH LOGIN
            finish(false, QStringLiteral("AUTH LOGIN 不被接受（%1）").arg(code));
            return;                                   // 结束
        }
        sendLine(QString::fromLatin1(m_cfg.username.toUtf8().toBase64()), true);  // 发送 base64 用户名（打码）
        m_state = State::AuthUser;                    // 进入用户名已发状态
        break;

    case State::AuthUser:       // 服务器提示「密码:」
        if (code != 334) {                            // 未索取密码则流程异常
            finish(false, QStringLiteral("认证流程异常（%1）：服务器未索取密码").arg(code));
            return;                                   // 结束
        }
        sendLine(QString::fromLatin1(m_cfg.password.toUtf8().toBase64()), true);  // 发送 base64 授权码（打码）
        m_state = State::AuthPass;                    // 进入认证结果状态
        break;

    case State::AuthPass:       // 期望 235
        if (code != 235) {                            // 非 235 即认证失败
            finish(false, QStringLiteral("认证失败（%1）：授权码或发件邮箱不正确。"
                                         "QQ 邮箱需使用「授权码」，不是登录密码")
                             .arg(code));
            return;                                   // 结束
        }
        sendLine(QStringLiteral("MAIL FROM:<%1>").arg(m_cfg.username));  // 声明发件人
        m_state = State::MailFrom;                    // 进入发件人状态
        break;

    case State::MailFrom:                             // MAIL FROM 应答
        if (to.isEmpty()) {                           // 无收件人无法继续
            finish(false, QStringLiteral("收件人为空"));
            return;                                   // 结束
        }
        m_rcptIndex = 0;                              // 从第一个收件人开始
        sendLine(QStringLiteral("RCPT TO:<%1>").arg(to.at(0)));  // 声明首个收件人
        m_state = State::RcptTo;                      // 进入收件人状态
        break;

    case State::RcptTo:                               // 逐个声明收件人
        ++m_rcptIndex;                                // 指向下一个
        if (m_rcptIndex < to.size()) {                // 还有收件人
            sendLine(QStringLiteral("RCPT TO:<%1>").arg(to.at(m_rcptIndex)));  // 继续声明
        } else {                                      // 全部声明完毕
            sendLine(QStringLiteral("DATA"));         // 请求发送正文
            m_state = State::Data;                    // 进入正文状态
        }
        break;

    case State::Data:                                 // 期望 354
        if (code != 354) {                            // 非 354 说明被拒绝
            finish(false, QStringLiteral("DATA 被拒绝（%1）：%2").arg(code).arg(text));
            return;                                   // 结束
        }
        m_sock->write((buildMime() + QStringLiteral("\r\n.\r\n")).toUtf8());  // 写入报文并以 . 结束
        m_sock->flush();                              // 立即冲刷
        Q_EMIT transcript(QStringLiteral("C: <邮件正文 %1 字节>")
                              .arg(m_mail.html.toUtf8().size()));  // 日志只记大小
        m_state = State::Body;                        // 进入投递结果状态
        break;

    case State::Body:                                 // 期望 250
        if (code != 250) {                            // 非 250 即投递失败
            finish(false, QStringLiteral("正文投递失败（%1）：%2").arg(code).arg(text));
            return;                                   // 结束
        }
        sendLine(QStringLiteral("QUIT"));             // 正常退出
        m_state = State::Quit;                        // 进入退出状态
        break;

    case State::Quit:                                 // 退出应答
        finish(true, m_mode == Mode::Probe           // 按模式给成功文案
                         ? QStringLiteral("SMTP 可达，握手与 EHLO 正常")
                         : QStringLiteral("已投递：%1").arg(to.join(QStringLiteral(", "))));
        break;

    case State::Idle:                                 // 空闲态不应有应答
        break;
    }
}

// base64 按 76 列折行（RFC 2045 要求）
QString SmtpClient::base64Wrapped(const QByteArray& raw)
{
    const QByteArray b64 = raw.toBase64();            // 先整体编码
    QString out;                                      // 结果
    out.reserve(b64.size() + b64.size() / 76 + 2);    // 预留（含折行符）
    for (int i = 0; i < b64.size(); i += 76) {        // 每 76 字符一段
        out += QString::fromLatin1(b64.mid(i, 76));   // 取一段
        out += QStringLiteral("\r\n");                // 补 CRLF
    }
    return out;                                       // 返回折行后的文本
}

// 组装 MIME 报文
QString SmtpClient::buildMime() const
{
    const QDateTime now = QDateTime::currentDateTime();  // 当前时间
    const QString date = QLocale::c().toString(now, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"))
                       + QStringLiteral(" +0800");       // RFC 2822 日期（带东八区偏移）
    const QString msgId = QStringLiteral("<%1@dabanquant>")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));  // 唯一 Message-ID

    const QString fromName = QStringLiteral("=?UTF-8?B?%1?=")
                                 .arg(QString::fromLatin1(m_cfg.fromName.toUtf8().toBase64()));  // 名称用 encoded-word
    const QString subject = QStringLiteral("=?UTF-8?B?%1?=")
                                .arg(QString::fromLatin1(m_mail.subject.toUtf8().toBase64()));  // 主题用 encoded-word

    QString head;                                     // 头部
    head += QStringLiteral("From: %1 <%2>\r\n").arg(fromName, m_cfg.username);  // 发件人
    head += QStringLiteral("To: %1\r\n").arg(m_cfg.effectiveRecipients().join(QStringLiteral(", ")));  // 收件人
    head += QStringLiteral("Subject: %1\r\n").arg(subject);       // 主题
    head += QStringLiteral("Date: %1\r\n").arg(date);             // 日期
    head += QStringLiteral("Message-ID: %1\r\n").arg(msgId);      // 唯一标识
    head += QStringLiteral("MIME-Version: 1.0\r\n");              // MIME 版本
    head += QStringLiteral("Content-Type: text/html; charset=UTF-8\r\n");  // HTML 正文
    head += QStringLiteral("Content-Transfer-Encoding: base64\r\n");       // base64 传输编码
    head += QStringLiteral("X-Mailer: ZuoDuoQuant/1.0\r\n");      // 客户端标识（ASCII 技术标识）
    head += QStringLiteral("\r\n");                               // 头部与正文分隔

    return head + base64Wrapped(m_mail.html.toUtf8());  // 头 + base64 正文
}

// 统一收尾：清资源、发日志与结果信号
void SmtpClient::finish(bool ok, const QString& detail)
{
    if (m_done) return;                               // 幂等：只收尾一次
    m_done = true;                                    // 标记已收尾
    m_busy = false;                                   // 解除忙标记
    m_state = State::Idle;                            // 回到空闲
    if (m_timer) m_timer->stop();                     // 停超时
    if (m_sock) {                                     // 清理套接字
        QSslSocket* sock = m_sock;                    // 暂存指针
        m_sock = nullptr;                             // 先置空防重入
        sock->disconnect(this);                       // 断开信号
        sock->abort();                                // 中止连接
        sock->deleteLater();                          // 交给事件循环回收
    }
    Q_EMIT transcript(ok ? QStringLiteral("✓ %1").arg(detail)
                         : QStringLiteral("✗ %1").arg(detail));  // 结果日志
    Q_EMIT finished(ok, detail);                      // 通知调用方
}
