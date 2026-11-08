#pragma once                                     // 防止头文件被重复包含

#include <QString>                               // 条款文本
#include <QStringList>                           // 条款条目
#include <QVector>                               // 分节正文

// ============================================================================
// LegalNotice：免责声明与风险揭示的**唯一事实来源**
//
// 为什么放在 core（零 GUI 依赖）：
//   · 正文既要在启动前置页渲染，也要能被离线门禁逐条断言，两处不能各抄一份；
//   · 合规文本一旦出现两份，改一处漏一处，用户看到的与门禁校验的就不是同一份。
//
// ★ 修改正文后**必须同时提升 version()**：
//   本机同意记录按「版本号」判定，版本不变则已同意用户不会重新确认。
// ============================================================================

namespace LegalNotice {

// 一个分节：标题 + 段落 + 条目（条目统一写成「标签：说明」，渲染时标签加粗）
struct Block {
    QString     heading;
    QStringList paragraphs;
    QStringList items;
};

// 完整声明文档
struct Document {
    QString        product;          // 适用产品
    QString        title;            // 标题
    QString        version;          // 声明版本（同时作为同意记录的失效依据）
    QString        effectiveDate;    // 生效日期 yyyy-MM-dd
    QString        lead;             // 开篇说明（协议属性 / 确认方式）
    QString        summary;          // 一句话风险摘要（顶部醒目提示）
    QVector<Block> blocks;           // 分节正文
    QString        footer;           // 末尾生效与解释权说明
};

// 正文（函数内静态，进程内只构造一次）
const Document& document();

// 扁平化全文：离线门禁逐条检索用，避免断言依赖 HTML 结构
QString plainText();

// 合规必需要点（正文中必须逐项出现，缺失即门禁失败）
QStringList requiredTopics();

// 同意记录的持久化键（QSettings）。放在这里，UI 与门禁共用同一套键名。
QString settingsOrganization();      // DeepInvestQuant
QString settingsApplication();       // Legal
QString acceptedVersionKey();        // disclaimer.acceptedVersion
QString acceptedAtKey();             // disclaimer.acceptedAt

} // namespace LegalNotice
