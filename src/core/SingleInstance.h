#pragma once

#include <QString>
#include <functional>

class QObject;

// 单实例：避免多开导致重复自动推送邮件、并发写同一 SQLite、重复轮询行情接口
namespace SingleInstance {
// 已有实例在跑：通知它激活窗口并返回 true（本进程应退出）
bool tryActivateExisting(const QString& key);
// 首个实例：监听本地 socket，收到激活消息时回调
bool listen(const QString& key, QObject* context, const std::function<void()>& onActivate);
void release();
}
