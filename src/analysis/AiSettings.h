#pragma once

#include <QString>

// AI 分析（大模型解读）的用户自设凭据。
//
// 需求约定（BYOK，Bring Your Own Key）：
//  · 程序不内置任何 API Key，不读环境变量、不读硬编码常量；
//    唯一来源是用户在本机「API Key…」设置里填写并保存的配置；
//  · 明文只在内存存在，落盘只写 DPAPI 保护的 apiKeyEnc，绝不写明文、绝不打日志；
//  · 未配置时 ready() 返回 false 并给出可直接展示给用户的原因文案。
//
// 配置落点：QStandardPaths::AppDataLocation/ai_config.json
// （GUI 下为 %APPDATA%/DeepInvestQuant/DeepInvestQuant/ai_config.json；与 SMTP 配置同目录但互不共用）。
struct AiSettings {
    QString apiKey;                                        // 仅内存；空 = 未配置
    QString baseUrl   = QStringLiteral("https://api.openai.com/v1"); // OpenAI 兼容端点
    QString model     = QStringLiteral("gpt-4o-mini");
    int     timeoutMs = 30000;                            // 单次请求超时

    bool    ready(QString* why = nullptr) const;          // 是否允许发起 AI 分析
    QString maskedKey() const;                            // 展示用掩码（不泄露完整密钥）
    QString endpoint() const;                             // baseUrl + /chat/completions

    static QString canonicalPath();                       // 规范保存路径
    static AiSettings load(const QString& path = QString());
    bool save(const QString& path = QString()) const;     // 原子写入，失败返回 false
    static bool clear(const QString& path = QString());   // 清除本机密钥

    // 保存前校验：格式错误不允许落盘
    static bool validateKey(const QString& key, QString* why = nullptr);
    static bool validateEndpoint(const QString& url, QString* why = nullptr);
    static bool validateModel(const QString& model, QString* why = nullptr);
};
