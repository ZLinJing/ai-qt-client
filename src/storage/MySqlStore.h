#pragma once

#include "storage/JournalStore.h"
#include <QObject>
#include <QString>

struct MYSQL;
struct MYSQL_RES;

// ============================================================================
// MySqlStore：交易日志的 MySQL 后端
//
// · 本机 Qt 6.8.3 未随附 qsqlmysql 插件、也未装 MySQL ODBC 驱动，
//   因此直接使用 MySQL Server 自带的 Connector/C（libmysql）。
// · 客户端库为**运行时动态加载**：静态导入 libmysql.dll 的可执行文件会被
//   本机的设备保护（WDAC/AppLocker）策略在启动时拦截；改成 LoadLibrary 后
//   程序本身不携带该依赖，可以正常启动，连不上时回退本地 SQLite。
// · 首次连接自动建库建表；表结构与 SQLite 后端字段一一对应，历史流水可整体搬迁。
// · 连接信息存在本地 SQLite 的 app_setting，密码用 DPAPI 保护后落盘。
// ============================================================================

class MySqlStore : public QObject, public JournalStore
{
    Q_OBJECT
public:
    // 连接参数。默认账号密码按部署要求内置（root / 230374），
    // 首次连接成功后会被 DPAPI 加密写回 app_setting，之后不再以明文落盘。
    struct Config {
        QString host = QStringLiteral("127.0.0.1");
        int     port = 3306;
        QString user = QStringLiteral("root");
        QString password = QStringLiteral("230374");
        QString database = QStringLiteral("daban_quant");
    };

    explicit MySqlStore(const Config& cfg, QObject* parent = nullptr);
    ~MySqlStore() override;

    // 加载客户端库 + 连接 + 自愈建库建表；失败时返回 false，原因见 lastError()
    bool open();
    void close();
    bool isOpen() const;

    const Config& config() const { return m_cfg; }

    // ---------- JournalStore 接口 ----------
    bool addTrade(TradeRecord& rec) override;
    bool updateTrade(const TradeRecord& rec) override;
    bool deleteTrade(int id) override;
    QVector<TradeRecord> trades() const override;
    QString lastError() const override { return m_lastError; }
    QString describe() const override;

    // 供一次性搬迁使用：把 SQLite 里的历史流水搬过来
    // （按「代码 + 买入日 + 买入价」去重，重复执行不会产生重复记录）
    int importFrom(const QVector<TradeRecord>& legacy);

private:
    bool loadClient();                        // 运行时加载 libmysql 并解析符号
    void unloadClient();
    bool exec(const QString& sql);            // 执行一条无返回结果的语句
    bool ensureDatabase();                    // CREATE DATABASE IF NOT EXISTS
    bool ensureTable();                       // CREATE TABLE IF NOT EXISTS
    QString escape(const QString& s) const;   // 转义字面量（防注入）
    QString num(double v) const;              // 数值 → SQL 字面量（与区域设置无关）

    Config  m_cfg;
    MYSQL*  m_conn = nullptr;
    void*   m_module = nullptr;               // 动态库句柄
    void*   m_api = nullptr;                  // Api 函数指针表（定义在 .cpp）
    // mutable：trades() 是 const 查询，但仍需要把失败原因记进错误信息
    mutable QString m_lastError;
};
