#pragma once                                     // 防止头文件被重复包含

#include <QObject>                               // 基类 QObject（支持信号槽）
#include <QNetworkAccessManager>                 // 复用的网络管理器
#include <QNetworkProxy>                         // 可选的显式代理
#include <QNetworkReply>                         // 应答对象
#include <QByteArray>                            // 原始响应体
#include <QString>                               // URL 与错误文本
#include <QUrl>                                  // 解析主机名
#include <QVector>                               // 待发队列
#include <QHash>                                 // （历史依赖，保留以免影响包含顺序）
#include <QMap>                                  // 头字段表与令牌桶
#include <QSet>                                  // 在途应答集合
#include <QDateTime>                             // 限流时间窗
#include <QTimer>                                // 队列兜底派发定时器
#include <functional>                            // std::function 回调

// HttpClient：统一的异步 HTTP 取数器
// 复用 QNetworkAccessManager；伪装浏览器 UA/Referer；自动重试 + 指数退避；
// 全局并发闸门与每主机限流（免密钥接口无 SLA 需克制）；UTF-8/GBK 编码自适应
// 单请求传输超时 + HTTP 状态码校验 + 队列上限 + 可统一中止在途请求

class HttpClient : public QObject
{
    Q_OBJECT                                     // 启用 moc（信号槽所需）
public:
    explicit HttpClient(QObject* parent = nullptr);  // 构造：初始化 NAM、UA、限流定时器
    ~HttpClient() override;                          // 析构：中止所有在途请求

    using SuccessFn = std::function<void(const QByteArray&)>;  // 成功回调：拿原始字节
    using ErrorFn   = std::function<void(const QString&)>;     // 失败回调：拿错误文本

    // 发起 GET；encoding 为空表示自动判断
    void get(const QString& url,                     // 目标地址
             const QMap<QByteArray, QByteArray>& headers = {},  // 额外请求头（为空则用默认 Referer）
             const QString& encoding = QString(),    // 期望编码，空=自动探测
             SuccessFn onSuccess = nullptr,          // 成功回调
             ErrorFn onError = nullptr);             // 失败回调

    void setMaxConcurrent(int n);                    // 设置全局最大并发数
    void setRetryCount(int n);                       // 设置失败重试次数
    void setUserAgent(const QString& ua);            // 设置 UA
    void setDefaultReferer(const QString& ref);      // 设置默认 Referer
    void setRateLimitPerHost(int perSecond);         // 设置每主机每秒请求上限
    void abortAll();                                 // 中止在途请求并清空队列（退出 / 切换交易日）

    bool busy() const { return m_pending > 0; }      // 是否还有在途请求
    int  pending() const { return m_pending; }       // 在途请求数
    QString lastError() const { return m_lastError; }  // 最近一次错误描述

Q_SIGNALS:
    void requestFailed(const QString& url, const QString& error);  // 单请求最终失败
    void allIdle();                                  // 队列清空且无在途请求（仅在忙→闲跳变时发一次）

private:
    struct Task {                                    // 单个待发请求
        QString url;                                 // 目标地址
        QMap<QByteArray, QByteArray> headers;        // 自定义请求头
        QString encoding;                            // 期望编码
        SuccessFn onSuccess;                         // 成功回调
        ErrorFn onError;                             // 失败回调
        int attempt = 0;                             // 已重试次数
    };

    void dispatch();                                 // 派发队列中可发出的请求
    void onReplyFinished(QNetworkReply* reply, Task task);  // 单个应答结束的处理
    static QByteArray detectEncoding(const QByteArray& raw, const QByteArray& ctype);  // 编码探测
    static QString decode(const QByteArray& raw, const QString& encoding);             // 按编码解码

    static constexpr int kMaxQueue = 512;            // 队列上限，防止失败重试无限堆积
    static constexpr int kTransferTimeoutMs = 15000; // 单请求传输超时（毫秒）

    QNetworkAccessManager* m_nam = nullptr;          // 网络管理器（连接复用）
    QMap<QString, int> m_hostQueue;      // 每主机令牌桶：当前 1 秒窗口内已发数
    QMap<QString, QDateTime> m_hostWindow;  // 每主机令牌桶：窗口起始时间
    QVector<Task> m_queue;                           // 待发队列
    int m_pending = 0;                               // 在途请求数
    int m_maxConcurrent = 8;                         // 全局并发上限
    int m_retry = 2;                                 // 默认重试次数
    int m_ratePerHost = 12;                          // 每主机每秒上限
    QString m_userAgent;                             // 当前 UA
    QString m_defaultReferer;                        // 默认 Referer
    QString m_lastError;                             // 最近错误
    QNetworkProxy m_envProxy;                        // 环境代理副本（备用）
    bool m_forceProxy = false;                       // 是否强制使用环境代理
    QTimer* m_drainTimer = nullptr;                  // 队列兜底派发定时器
    QSet<QNetworkReply*> m_active;                   // 在途应答（用于统一中止）
    bool m_idleEmitted = true;                       // allIdle 边沿触发标记
};
