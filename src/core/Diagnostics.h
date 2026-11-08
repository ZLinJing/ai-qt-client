#pragma once

#include <QString>

// 运行时可观测性：文件日志（按天轮转）+ Windows 崩溃转储
// GUI 与自检入口都调用 install()；日志不记录授权码、邮件正文等敏感内容。
namespace Diagnostics {
void    install(bool enableCrashDump = true);
QString logDir();
QString crashDumpDir();
QString lastLogPath();
}
