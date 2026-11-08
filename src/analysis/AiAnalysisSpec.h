#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// AI 分析的「思路与边界」唯一定义处。
//
// 需求约定：分析目标 / 输入信息范围 / 输出内容结构 / 判断原则全部集中在本模块，
// prompt 拼装、输入过滤、输出解析与渲染只走这里，避免分析范围不清或输出不一致。
// 本模块零 GUI、零网络、零 IO 依赖，可被离线单元门禁直接断言。
namespace AiSpec {

// ---- 四段边界文本（界面展示与 system prompt 共用，改一处即全链路生效）----
QString goalText();
QString inputScopeText();
QString outputSchemaText();
QString principlesText();
QString scopeHtml();                 // 界面上的「分析边界」说明（固定四段）

QString systemPrompt();              // 固定 system prompt

// ---- 输入白名单：只有这些字段会进入请求，其余一律丢弃 ----
QStringList allowedInputKeys();
QString inputCatalogText();          // 人可读的字段清单（含含义）
QJsonObject sanitizeInput(const QJsonObject& raw);   // 白名单 + 类型收敛 + 缺失置 null

// ---- 请求体：OpenAI 兼容 chat/completions ----
QJsonObject buildRequestBody(const QString& model, const QJsonObject& sanitizedInput);
QJsonObject buildValidationBody(const QString& model);  // 密钥连通性校验用的最小请求

// ---- 输出结构：严格 schema ----
struct Result {
    QString     conclusion;   // 一致 / 存疑 / 数据不足
    QString     summary;
    QStringList evidence;     // "字段：观察"
    QStringList risks;
    QStringList discipline;
    QString     uncertainty;
};

bool    parseResult(const QByteArray& raw, Result* out, QString* error); // 不符合即整份丢弃
QString renderResult(const Result& r);                                   // 固定结构 HTML

} // namespace AiSpec
