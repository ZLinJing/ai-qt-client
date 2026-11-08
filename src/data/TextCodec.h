#pragma once

#include <QByteArray>
#include <QString>

// ============================================================================
// TextCodec：轻量字符集解码工具
// 免密钥接口中，腾讯 qt.gtimg.cn 返回 GBK，新浪/东财返回 UTF-8。
// 本模块不依赖 Qt5Compat（Core5Compat 不随 qtbase 附带），直接用
// Win32 MultiByteToWideChar(CP936) 完成 GBK→UTF-16 转换，零外部依赖。
// ============================================================================

namespace TextCodec {

// UTF-8 解码（非法序列以 U+FFFD 替换，不抛异常）
QString fromUtf8(const QByteArray& raw);

// GBK 解码。Windows 下走 CP936；其它平台退化为 Latin-1 兜底。
QString fromGbk(const QByteArray& raw);

// 按名称解码：支持 "utf-8" / "gbk" / "gb2312" / "gb18030" / "latin1"
QString decode(const QByteArray& raw, const QString& charset);

// 判断字节流是否可能是合法 UTF-8
bool looksLikeUtf8(const QByteArray& raw);

} // namespace TextCodec
