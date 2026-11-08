#include "data/TextCodec.h"                          // 引入本模块声明
#include <vector>                                    // std::vector：GBK 转换用的宽字符缓冲
#include <QStringDecoder>                            // QStringDecoder：Qt6 UTF-8 解码器

#ifdef _WIN32
#  include <windows.h>                               // MultiByteToWideChar：CP936 转 UTF-16
#endif

namespace TextCodec {                                // 字符集解码工具命名空间

// UTF-8 解码：先用 Qt 解码器检测合法性，出错再退回容错接口
QString fromUtf8(const QByteArray& raw)
{
    if (raw.isEmpty()) return QString();             // 空输入直接返回空串，避免无谓解码
    QStringDecoder dec(QStringDecoder::Utf8);        // 构造 UTF-8 解码器
    QString s = dec.decode(raw);                     // 执行解码并暂存结果
    if (!dec.hasError()) return s;                   // 无非法序列则该结果即最终值
    return QString::fromUtf8(raw);                   // 含非法序列：退回逐字节替换，保证不抛异常
}

// GBK 解码：Windows 走 CP936 系统接口，其它平台退化为此 Latin-1 兜底
QString fromGbk(const QByteArray& raw)
{
    if (raw.isEmpty()) return QString();             // 空输入直接返回空串
#ifdef _WIN32
    const int need = ::MultiByteToWideChar(936, 0, raw.constData(), raw.size(), nullptr, 0); // 先探测所需宽字符数
    if (need > 0) {                                  // 探测成功才分配缓冲
        std::vector<wchar_t> buf(static_cast<size_t>(need));  // 分配足够容纳转换结果的缓冲
        const int got = ::MultiByteToWideChar(936, 0, raw.constData(), raw.size(), buf.data(), need); // 执行 CP936→UTF-16
        if (got > 0) return QString::fromWCharArray(buf.data(), got);  // 转换成功则包装为 QString 返回
    }
#endif
    return QString::fromLatin1(raw);                 // 非 Windows 兜底：按 Latin-1 保留字节（中文乱码但不崩溃）
}

// 判定字节流是否构成合法 UTF-8（空输入视为合法）
bool looksLikeUtf8(const QByteArray& raw)
{
    if (raw.isEmpty()) return true;                  // 空输入无可判断内容，按合法处理
    QStringDecoder dec(QStringDecoder::Utf8);        // 复用同一解码器做语法校验
    dec.decode(raw);                                 // 解码结果不用，只关心是否报错
    return !dec.hasError();                          // 无错误即判定为合法 UTF-8
}

// 按字符集名称分派解码：统一入口，未知名称按 UTF-8 处理
QString decode(const QByteArray& raw, const QString& charset)
{
    if (raw.isEmpty()) return QString();             // 空输入直接返回空串
    const QString cs = charset.trimmed().toLower();  // 归一化名称，容忍大小写与空白
    if (cs.isEmpty() || cs == QStringLiteral("utf-8") || cs == QStringLiteral("utf8"))
        return fromUtf8(raw);                        // 缺省 / utf8 别名走 UTF-8 解码
    if (cs == QStringLiteral("gbk") || cs == QStringLiteral("gb2312") || cs == QStringLiteral("gb18030"))
        return fromGbk(raw);                         // 中文三种 GB 编码统一按 GBK 解码
    if (cs == QStringLiteral("latin1") || cs == QStringLiteral("iso-8859-1"))
        return QString::fromLatin1(raw);              // Latin-1 单字节直接映射
    return fromUtf8(raw);                            // 未知字符集：按 UTF-8 兜底
}

} // namespace TextCodec                             // 命名空间结束
