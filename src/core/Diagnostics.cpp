#include "core/Diagnostics.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QtGlobal>
#include <cstdio>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <DbgHelp.h>
#endif

namespace {

QMutex g_mutex;
QString g_lastLogPath;

QString baseDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}

QString currentLogPath()
{
    return Diagnostics::logDir() + QLatin1Char('/')
        + QDate::currentDate().toString(QStringLiteral("yyyyMMdd")) + QStringLiteral(".log");
}

QString levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return QStringLiteral("DEBUG");
    case QtInfoMsg:     return QStringLiteral("INFO");
    case QtWarningMsg:  return QStringLiteral("WARN");
    case QtCriticalMsg: return QStringLiteral("ERROR");
    case QtFatalMsg:    return QStringLiteral("FATAL");
    }
    return QStringLiteral("LOG");
}

void cleanupOldLogs()
{
    QDir d(Diagnostics::logDir());
    const QDate cutoff = QDate::currentDate().addDays(-14);
    for (const QFileInfo& fi : d.entryInfoList({ QStringLiteral("*.log") }, QDir::Files)) {
        const QDate day = QDate::fromString(fi.completeBaseName(), QStringLiteral("yyyyMMdd"));
        if (day.isValid() && day < cutoff) QFile::remove(fi.absoluteFilePath());
    }
}

void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    Q_UNUSED(ctx);
    QMutexLocker locker(&g_mutex);
    const QString line = QStringLiteral("[%1] [%2] %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             levelName(type), msg);

    std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
    std::fflush(stderr);

    g_lastLogPath = currentLogPath();
    QFile f(g_lastLogPath);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << line << '\n';
    }
}

#ifdef Q_OS_WIN
LONG WINAPI crashFilter(EXCEPTION_POINTERS* info)
{
    const QString dir = Diagnostics::crashDumpDir();
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/')
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))
        + QStringLiteral(".dmp");

    HANDLE hFile = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                          MiniDumpWithDataSegs, &mei, nullptr, nullptr);
        CloseHandle(hFile);
        std::fprintf(stderr, "crash dump written: %s\n", path.toLocal8Bit().constData());
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace

QString Diagnostics::logDir()
{
    return baseDir() + QStringLiteral("/logs");
}

QString Diagnostics::crashDumpDir()
{
    return baseDir() + QStringLiteral("/crash");
}

QString Diagnostics::lastLogPath()
{
    QMutexLocker locker(&g_mutex);
    return g_lastLogPath;
}

void Diagnostics::install(bool enableCrashDump)
{
    QDir().mkpath(logDir());
    cleanupOldLogs();
    qInstallMessageHandler(messageHandler);
#ifdef Q_OS_WIN
    if (enableCrashDump) {
        QDir().mkpath(crashDumpDir());
        SetUnhandledExceptionFilter(crashFilter);
    }
#else
    Q_UNUSED(enableCrashDump);
#endif
    qInfo().noquote() << QStringLiteral("做多量化选股系统启动，日志目录：%1").arg(logDir());
}
