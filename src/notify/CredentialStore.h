#pragma once

#include <QString>

// SMTP 授权码保护。
// Windows：DPAPI 绑定到当前 Windows 用户账户加密，密文以 base64 写入配置文件；
//           换用户 / 换机器解不开，明文不会落盘、不会进版本库、不会进日志。
// 其它平台：仅做可逆混淆，不具备密码学强度（本项目部署目标为 Windows）。
namespace CredentialStore {
QString protect(const QString& plain);              // 明文 → 可落盘的受保护串
QString unprotect(const QString& stored);           // 受保护串 → 明文（兼容历史明文）
bool    isProtected(const QString& stored);         // 是否已加密/混淆
}
