#include "data/HttpClient.h"             // 本模块声明
#include <QNetworkRequest>               // 构造请求对象
#include <QEventLoop>                    // （保留：部分同步场景需要）
#include <QThread>                       // （保留：线程相关辅助）
#include <QtEndian>                       // 字节序工具
#include <QNetworkProxy>                 // 显式代理设置
#include <QHostAddress>                  // 主机地址

#include <QSslConfiguration>             // TLS 配置
#include <QSslSocket>                    // TLS 支持检测
#include "data/TextCodec.h"              // 编码探测与解码

// 构造：建 NAM、按需装代理、设置 UA/Referer、启动限流定时器
HttpClient::HttpClient(QObject* parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);   // 复用同一 NAM 以获得连接复用

    m_forceProxy = (qgetenv("DABAN_FORCE_PROXY") == "1");   // 默认不用环境代理，避免 TLS 初始化失败
    if (m_forceProxy) {                        // 仅在显式开启时才装配代理
        const QByteArray envHttps = qgetenv("https_proxy");   // 优先取 https_proxy
        const QByteArray envHttp  = qgetenv("http_proxy");    // 回退 http_proxy
        const QByteArray envProxy = !envHttps.isEmpty() ? envHttps : envHttp;  // 选出可用的那个
        const QUrl pu(QString::fromLatin1(envProxy.trimmed()));  // 解析代理 URL
        if (pu.isValid() && !pu.host().isEmpty()) {  // 地址有效才装配
            QNetworkProxy proxy(QNetworkProxy::HttpProxy);  // 建 HTTP 代理对象
            proxy.setHostName(pu.host());        // 代理主机
            proxy.setPort(pu.port(80));          // 代理端口（默认 80）
            if (!pu.userName().isEmpty()) {      // 有用户名则设置认证
                proxy.setUser(pu.userName());    // 代理用户名
                proxy.setPassword(pu.password()); // 代理密码
            }
            m_nam->setProxy(proxy);              // 应用到 NAM
        }
    }

    m_userAgent = QStringLiteral(                // 伪装成桌面 Chrome，部分接口按 UA 过滤
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    m_defaultReferer = QStringLiteral("https://data.eastmoney.com/");  // 东财接口需要 Referer

    m_drainTimer = new QTimer(this);             // 定时器作为限流窗口的兜底驱动器
    m_drainTimer->setInterval(100);              // 每 100ms 尝试派发一次
    connect(m_drainTimer, &QTimer::timeout, this, &HttpClient::dispatch);  // 到点即派发
    m_drainTimer->start();                       // 启动定时器
}

HttpClient::~HttpClient()
{
    abortAll();
}

// 统一中止：退出、切换交易日或重置数据源时调用，避免挂起连接占住并发槽
void HttpClient::abortAll()
{
    m_queue.clear();
    for (QNetworkReply* r : m_active) {
        if (!r) continue;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    m_active.clear();
    m_pending = 0;
}

void HttpClient::setMaxConcurrent(int n) { m_maxConcurrent = qMax(1, n); }        // 并发至少为 1
void HttpClient::setRetryCount(int n) { m_retry = qMax(0, n); }                   // 重试次数不为负
void HttpClient::setUserAgent(const QString& ua) { m_userAgent = ua; }            // 覆盖 UA
void HttpClient::setDefaultReferer(const QString& ref) { m_defaultReferer = ref; }// 覆盖默认 Referer
void HttpClient::setRateLimitPerHost(int perSecond) { m_ratePerHost = qMax(1, perSecond); }  // 每主机限流至少 1

// 入队并立即尝试派发
void HttpClient::get(const QString& url,
                     const QMap<QByteArray, QByteArray>& headers,
                     const QString& encoding,
                     SuccessFn onSuccess,
                     ErrorFn onError)
{
    if (m_queue.size() >= kMaxQueue) {           // 队列上限：失败重试用尽内存前先拒绝新任务
        const QString err = QStringLiteral("请求队列已满（%1），丢弃：%2").arg(kMaxQueue).arg(url);
        m_lastError = err;
        if (onError) onError(err);
        Q_EMIT requestFailed(url, err);
        return;
    }

    Task t;                                      // 组装任务
    t.url = url;                                 // 地址
    t.headers = headers;                         // 请求头
    t.encoding = encoding;                       // 期望编码
    t.onSuccess = std::move(onSuccess);          // 移动语义接管成功回调
    t.onError = std::move(onError);              // 移动语义接管失败回调
    m_queue.append(t);                           // 入队
    m_idleEmitted = false;                       // 有新任务：允许下一轮「忙→闲」跳变发 allIdle
    dispatch();     // 立即派发；定时器仅作为限流补偿的兜底
}

// 编码探测：响应头 charset > HTML meta > UTF-8 合法性 > 默认 GBK
QByteArray HttpClient::detectEncoding(const QByteArray& raw, const QByteArray& ctype)
{
    const int idx = ctype.indexOf("charset=");   // 先看响应头是否声明了 charset
    if (idx >= 0) {                              // 声明了就直接采用
        QByteArray cs = ctype.mid(idx + 8).trimmed().toLower();  // 取出并归一化
        const int semi = cs.indexOf(';');        // 截掉后续参数
        if (semi >= 0) cs = cs.left(semi);       // 只保留分号前部分
        cs = cs.trimmed();                       // 再去空白
        if (!cs.isEmpty()) return cs;            // 有效则返回
    }
    const int m = raw.left(1024).indexOf("charset=");  // 前 1KB 内找 HTML meta 声明
    if (m >= 0) {                                // 找到了
        QByteArray cs = raw.mid(m + 8, 32).toLower();  // 取其后 32 字节并转小写
        int stop = 0;                            // 有效字符终止位置
        while (stop < cs.size() && cs[stop] != '"' && cs[stop] != '\'' && cs[stop] != '>'
               && cs[stop] != ' ' && cs[stop] != ';')
            ++stop;                              // 遇到引号/尖括号/空白/分号即停
        cs = cs.left(stop).trimmed();            // 截取有效编码名
        if (!cs.isEmpty()) return cs;            // 有效则返回
    }
    if (TextCodec::looksLikeUtf8(raw)) return "utf-8";  // 字节流合法 UTF-8 则按 UTF-8
    return "gbk";                                // 否则按国内接口常见的 GBK
}

// 按指定编码解码（空编码按 UTF-8 处理）
QString HttpClient::decode(const QByteArray& raw, const QString& encoding)
{
    if (raw.isEmpty()) return QString();         // 空响应直接返回空串
    return TextCodec::decode(raw, encoding.isEmpty() ? QStringLiteral("utf-8") : encoding);  // 交给编码模块
}

// 派发：按并发上限与每主机令牌桶挑选可发任务
void HttpClient::dispatch()
{
    if (m_queue.isEmpty()) {                     // 队列空
        if (m_pending == 0 && !m_idleEmitted) { // 且无在途请求：只在跳变时广播一次
            m_idleEmitted = true;
            Q_EMIT allIdle();
        }
        return;                                  // 无事可做
    }
    if (m_pending >= m_maxConcurrent) return;    // 已达到并发上限，等下一次触发

    const QDateTime now = QDateTime::currentDateTime();  // 当前时间

    QVector<int> ready;                          // 本轮可发送任务的下标（先收集再取出，避免遍历时错位）
    for (int i = 0; i < m_queue.size(); ++i) {   // 逐个检查排队任务
        const QString host = QUrl(m_queue[i].url).host();  // 取目标主机
        QDateTime& win = m_hostWindow[host];     // 该主机的限流窗口
        if (!win.isValid() || win.msecsTo(now) >= 1000) {  // 窗口不存在或已过期
            win = now;                           // 重置窗口起点
            m_hostQueue[host] = 0;               // 令牌桶清零
        }
        if (m_hostQueue[host] >= m_ratePerHost) continue;  // 该主机已到每秒上限则跳过
        if (m_pending + ready.size() >= m_maxConcurrent) break;  // 并发额度已满则停止挑选
        ready.append(i);                         // 记入可发送列表
        ++m_hostQueue[host];   // 先占位，避免同一主机超发
    }

    for (int k = ready.size() - 1; k >= 0; --k) {   // 倒序取出：takeAt 会让后续索引前移
        const int idx = ready[k];                // 目标下标
        if (idx < 0 || idx >= m_queue.size()) continue;  // 下标失效则跳过
        Task task = m_queue.takeAt(idx);         // 从队列移除并取出任务
        ++m_pending;                             // 在途计数加一

        QNetworkRequest req{QUrl(task.url)};     // 构造请求
        req.setTransferTimeout(kTransferTimeoutMs);  // 关键：挂起连接不得永久占用并发槽
        req.setRawHeader("User-Agent", m_userAgent.toUtf8());              // 伪装 UA
        req.setRawHeader("Accept", "application/json, text/plain, */*");   // 接受类型
        req.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9");             // 语言偏好
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);        // 允许同等安全的跳转
        if (!task.headers.isEmpty()) {           // 调用方自定义了请求头
            for (auto it = task.headers.constBegin(); it != task.headers.constEnd(); ++it)
                req.setRawHeader(it.key(), it.value());   // 逐条写入
        } else if (!m_defaultReferer.isEmpty()) {  // 未自定义则补默认 Referer
            req.setRawHeader("Referer", m_defaultReferer.toUtf8());
        }

        QNetworkReply* reply = m_nam->get(req);  // 发起异步 GET
        m_active.insert(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, task]() {  // 应答完成回调
            onReplyFinished(reply, task);        // 交给统一处理函数
        });
        connect(reply, &QNetworkReply::destroyed, this, [this, reply]() {
            m_active.remove(reply);              // 兜底：异常销毁时也不留悬空指针
        });
    }
}

// 应答结束：统计、重试或回调，然后继续派发
void HttpClient::onReplyFinished(QNetworkReply* reply, Task task)
{
    --m_pending;                                 // 在途计数减一
    m_active.remove(reply);
    const QByteArray raw = reply->readAll();     // 读取完整响应体
    const QNetworkReply::NetworkError err = reply->error();  // 网络错误码
    const QString errStr = reply->errorString();  // 错误描述
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray ctype = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();  // 响应类型（含 charset）
    reply->deleteLater();                        // 交给事件循环回收

    // HTTP 4xx/5xx 在 Qt 里仍算 NoError，必须显式判状态码，否则会把错误页当正常数据解析
    const bool httpFailed = (httpStatus >= 400);
    const bool retryableStatus = (httpStatus == 429 || httpStatus >= 500);
    if (err != QNetworkReply::NoError || httpFailed) {
        const QString reason = httpFailed
            ? QStringLiteral("HTTP %1：%2").arg(httpStatus).arg(errStr)
            : errStr;
        const bool canRetry = (err == QNetworkReply::NoError) ? retryableStatus : true;
        if (canRetry && task.attempt < m_retry) {            // 还有重试额度
            Task retry = task;                   // 复制一份用于重试
            ++retry.attempt;                     // 累加重试次数
            int delayMs[] = {500, 1200, 2000};   // 指数退避阶梯
            int d = delayMs[qMin(retry.attempt - 1, 2)];  // 取对应档位延迟并夹紧下标
            QTimer::singleShot(d, this, [this, retry]() {  // 延迟后重新入队
                m_queue.append(retry);           // 回到队尾
                dispatch();                      // 触发派发
            });
            return;                              // 本轮不再回调
        }
        m_lastError = QStringLiteral("%1 -> %2").arg(task.url, reason);  // 记录最后一次错误
        if (task.onError) task.onError(reason);  // 通知调用方
        Q_EMIT requestFailed(task.url, reason);  // 广播失败信号
        dispatch();                              // 继续派发后续任务
        return;                                  // 结束
    }

    const QString enc = task.encoding.isEmpty() ? detectEncoding(raw, ctype) : task.encoding;  // 计算编码（供上层按需使用）
    if (task.onSuccess) task.onSuccess(raw);     // 回调原始字节，由调用方解码
    Q_UNUSED(enc);                               // 编码值当前仅做探测，故显式忽略
    dispatch();                                  // 继续派发
}
