#include "analysis/AiAnalysisClient.h"

#include "analysis/AiAnalysisSpec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

AiAnalysisClient::AiAnalysisClient(QObject* parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_timedOut = true;
        if (m_reply) m_reply->abort();            // abort 会触发 finished，由 onReply 统一收尾
    });
}

void AiAnalysisClient::cancel()
{
    m_timer->stop();
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    m_timedOut = false;
}

void AiAnalysisClient::analyze(const AiSettings& settings, const QJsonObject& rawInput)
{
    // 输入白名单在 AiSpec::buildRequestBody 内还会再过滤一次
    post(settings, AiSpec::buildRequestBody(settings.model.trimmed(),
                                            AiSpec::sanitizeInput(rawInput)), false);
}

void AiAnalysisClient::verify(const AiSettings& settings)
{
    post(settings, AiSpec::buildValidationBody(settings.model.trimmed()), true);
}

void AiAnalysisClient::post(const AiSettings& settings, const QJsonObject& body, bool verifyMode)
{
    cancel();
    QString why;
    if (!settings.ready(&why)) {                  // 未配置 / 配置非法：不发任何网络请求
        if (verifyMode) Q_EMIT verified(false, why);
        else            Q_EMIT finished(false, QString(), why);
        return;
    }

    QNetworkRequest req(QUrl(settings.endpoint()));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    req.setRawHeader("Accept", QByteArray("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + settings.apiKey.trimmed().toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    m_timer->start(settings.timeoutMs);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, verifyMode]() { onReply(reply, verifyMode, m_timedOut); });
}

void AiAnalysisClient::onReply(QNetworkReply* reply, bool verifyMode, bool timedOut)
{
    m_timer->stop();
    const QByteArray payload = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = reply->error();
    m_timedOut = false;
    m_reply.clear();
    reply->deleteLater();

    auto fail = [this, verifyMode](const QString& message) {
        if (verifyMode) Q_EMIT verified(false, message);
        else            Q_EMIT finished(false, QString(), message);
    };

    if (timedOut || netErr == QNetworkReply::OperationCanceledError)
        return fail(QStringLiteral("请求超时，请在「API Key…」设置中调整超时或更换接口地址"));
    if (netErr != QNetworkReply::NoError || status < 200 || status >= 300) {
        if (status == 401 || status == 403)
            return fail(QStringLiteral("API Key 无效或无权访问该模型（HTTP %1），请重新填写密钥").arg(status));
        if (status == 429)
            return fail(QStringLiteral("接口限流（HTTP 429），请稍后重试"));
        if (status == 404)
            return fail(QStringLiteral("接口地址或模型不存在（HTTP 404），请检查 baseUrl 与模型名"));
        if (status >= 500)
            return fail(QStringLiteral("服务端错误（HTTP %1），请稍后重试").arg(status));
        return fail(QStringLiteral("请求失败（HTTP %1）：%2")
                        .arg(status)
                        .arg(netErr == QNetworkReply::NoError
                                 ? QStringLiteral("接口返回非成功状态")
                                 : reply->errorString()));
    }

    if (verifyMode) {
        Q_EMIT verified(true, QStringLiteral("API Key 校验通过：端点与模型均可访问"));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return fail(QStringLiteral("接口未返回分析内容，本次结果已丢弃"));
    const QString content = choices.at(0).toObject()
                                .value(QStringLiteral("message")).toObject()
                                .value(QStringLiteral("content")).toString();
    if (content.trimmed().isEmpty())
        return fail(QStringLiteral("接口返回空内容，本次结果已丢弃"));

    AiSpec::Result result;
    QString error;
    if (!AiSpec::parseResult(content.toUtf8(), &result, &error))
        return fail(QStringLiteral("输出不符合约定结构，已整份丢弃：%1").arg(error));
    Q_EMIT finished(true, AiSpec::renderResult(result), QString());
}
