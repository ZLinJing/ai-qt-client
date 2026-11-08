#pragma once

#include "analysis/AiSettings.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// AI 分析的唯一网络出口。
// 硬门禁：settings.ready() 为 false 时绝不发请求，只回传可直接展示的原因文案。
// 安全约定：密钥只出现在 Authorization 头，绝不进入请求体、错误信息或日志。
class AiAnalysisClient : public QObject
{
    Q_OBJECT
public:
    explicit AiAnalysisClient(QObject* parent = nullptr);

    void analyze(const AiSettings& settings, const QJsonObject& rawInput);
    void verify(const AiSettings& settings);      // 设置页「测试连接」

Q_SIGNALS:
    void finished(bool ok, const QString& resultHtml, const QString& error);
    void verified(bool ok, const QString& message);

private:
    void post(const AiSettings& settings, const QJsonObject& body, bool verifyMode);
    void onReply(QNetworkReply* reply, bool verifyMode, bool timedOut);
    void cancel();

    QNetworkAccessManager* m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QTimer* m_timer = nullptr;
    bool m_timedOut = false;
};
