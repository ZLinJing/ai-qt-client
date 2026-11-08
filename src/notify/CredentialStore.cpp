#include "notify/CredentialStore.h"

#include <QByteArray>
#include <QtGlobal>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <wincrypt.h>
#endif

namespace {

const QString kDpapiPrefix = QStringLiteral("dpapi:");
const QString kObfPrefix   = QStringLiteral("obf:");

// 非 Windows 回退：仅防「打开文件一眼看到明文」，不是加密
QByteArray xorWithKey(const QByteArray& in)
{
    static const char key[] = "DabanQuant.CredentialStore";
    QByteArray out = in;
    const int n = int(sizeof(key)) - 1;
    for (int i = 0; i < out.size(); ++i)
        out[i] = char(out.at(i) ^ key[i % n]);
    return out;
}

} // namespace

bool CredentialStore::isProtected(const QString& stored)
{
    return stored.startsWith(kDpapiPrefix) || stored.startsWith(kObfPrefix);
}

QString CredentialStore::protect(const QString& plain)
{
    if (plain.isEmpty()) return QString();
    const QByteArray raw = plain.toUtf8();

#ifdef Q_OS_WIN
    DATA_BLOB in {};
    in.cbData = DWORD(raw.size());
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(raw.constData()));
    DATA_BLOB out {};
    if (!CryptProtectData(&in, L"DabanQuant.SMTP", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return QString();                       // 加密失败：宁可不保存，也不落明文
    const QByteArray blob(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return kDpapiPrefix + QString::fromLatin1(blob.toBase64());
#else
    return kObfPrefix + QString::fromLatin1(xorWithKey(raw).toBase64());
#endif
}

QString CredentialStore::unprotect(const QString& stored)
{
    if (stored.isEmpty()) return QString();

#ifdef Q_OS_WIN
    if (stored.startsWith(kDpapiPrefix)) {
        const QByteArray blob = QByteArray::fromBase64(stored.mid(kDpapiPrefix.size()).toLatin1());
        DATA_BLOB in {};
        in.cbData = DWORD(blob.size());
        in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.constData()));
        DATA_BLOB out {};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &out))
            return QString();                   // 换用户/换机器：要求重新输入授权码
        const QString plain = QString::fromUtf8(reinterpret_cast<const char*>(out.pbData),
                                                int(out.cbData));
        LocalFree(out.pbData);
        return plain;
    }
#endif

    if (stored.startsWith(kObfPrefix)) {
        const QByteArray blob = QByteArray::fromBase64(stored.mid(kObfPrefix.size()).toLatin1());
        return QString::fromUtf8(xorWithKey(blob));
    }
    return stored;                               // 历史明文配置：由调用方负责迁移
}
