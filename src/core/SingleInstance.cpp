#include "core/SingleInstance.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <memory>

namespace {
std::unique_ptr<QLocalServer> g_server;
}

bool SingleInstance::tryActivateExisting(const QString& key)
{
    QLocalSocket sock;
    sock.connectToServer(key);
    if (!sock.waitForConnected(300)) return false;
    sock.write("activate");
    sock.flush();
    sock.waitForBytesWritten(300);
    sock.disconnectFromServer();
    return true;
}

bool SingleInstance::listen(const QString& key, QObject* context,
                            const std::function<void()>& onActivate)
{
    if (!context || g_server) return g_server != nullptr;

    auto server = std::make_unique<QLocalServer>();
    if (!server->listen(key)) {
        QLocalServer::removeServer(key);         // 清理上次异常退出留下的命名管道
        if (!server->listen(key)) return false;
    }

    QLocalServer* raw = server.get();
    g_server = std::move(server);
    QObject::connect(raw, &QLocalServer::newConnection, context, [raw, onActivate]() {
        while (raw->hasPendingConnections()) {
            QLocalSocket* c = raw->nextPendingConnection();
            QObject::connect(c, &QLocalSocket::readyRead, c, [c, onActivate]() {
                c->readAll();
                onActivate();
            });
            QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
        }
    });
    return true;
}

void SingleInstance::release()
{
    if (g_server) {
        g_server->close();
        g_server.reset();
    }
}
