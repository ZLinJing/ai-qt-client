#include "storage/MySqlStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <mysql.h>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// MySqlStore 实现说明
//
// · 客户端库在运行时动态加载（LoadLibrary + GetProcAddress），
//   静态导入 libmysql.dll 会被本机设备保护策略在启动时拦截；
// · 连接字符集统一 utf8mb4（保证中文股票名 / 备注不乱码）；
// · 所有来自用户的字符串都经过 mysql_real_escape_string 转义后再拼进 SQL，
//   SQL 模板本身是常量，不含任何外部输入；
// · 数值用 QString::number 输出（与系统区域设置无关），不会出现逗号小数点；
// · 任何一步失败都会把服务端原因写进 m_lastError，界面可直接展示。
// ============================================================================

namespace {

// 表名：与 SQLite 后端的 trade_journal 保持同名同义
constexpr const char* kTable = "trade_journal";

// Connector/C 的函数指针表（运行时解析）
struct Api {
    MYSQL*     (*init)(MYSQL*) = nullptr;
    int        (*options)(MYSQL*, enum mysql_option, const void*) = nullptr;
    MYSQL*     (*realConnect)(MYSQL*, const char*, const char*, const char*,
                              const char*, unsigned int, const char*, unsigned long) = nullptr;
    void       (*close)(MYSQL*) = nullptr;
    const char*(*error)(MYSQL*) = nullptr;
    int        (*setCharacterSet)(MYSQL*, const char*) = nullptr;
    int        (*selectDb)(MYSQL*, const char*) = nullptr;
    MYSQL_RES* (*storeResult)(MYSQL*) = nullptr;
    void       (*freeResult)(MYSQL_RES*) = nullptr;
    MYSQL_ROW  (*fetchRow)(MYSQL_RES*) = nullptr;
    unsigned long* (*fetchLengths)(MYSQL_RES*) = nullptr;
    unsigned long (*realEscapeString)(MYSQL*, char*, const char*, unsigned long) = nullptr;
    int        (*realQuery)(MYSQL*, const char*, unsigned long) = nullptr;
    uint64_t   (*insertId)(MYSQL*) = nullptr;
};

// 查找 libmysql.dll：只在**程序所在目录**加载。
//
// 为什么不用 MySQL 安装目录 / PATH：
//   受限终端的设备保护（WDAC/AppLocker）策略允许从程序目录加载配套 DLL，
//   但从 C:\Program Files\MySQL\... 直接加载同一个 DLL 会被判定违规，
//   直接**终止进程**（没有异常、没有回退机会）。因此这里坚持同目录加载，
//   找不到就优雅回退本地 SQLite —— 部署时把 libmysql.dll 放在 exe 旁边即可。
QString clientLibPath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/libmysql.dll");
}

} // namespace

MySqlStore::MySqlStore(const Config& cfg, QObject* parent)
    : QObject(parent), m_cfg(cfg)
{
    m_api = new Api();
}

MySqlStore::~MySqlStore()
{
    close();
    delete static_cast<Api*>(m_api);
    m_api = nullptr;
}

// ---------------------------------------------------------------- 客户端库
// 运行时加载 libmysql.dll 并解析所需符号。
// 不做静态链接的原因：本机设备保护策略会拦截「静态导入 libmysql.dll」的程序启动。
bool MySqlStore::loadClient()
{
    if (m_module) return true;
#ifdef _WIN32
    const QString path = clientLibPath();
    HMODULE mod = LoadLibraryA(path.toLocal8Bit().constData());
    if (!mod) {
        m_lastError = QStringLiteral("未找到 MySQL 客户端库 libmysql.dll（应与程序放在同一目录：%1）")
                          .arg(path);
        return false;
    }
    m_module = mod;

    Api* api = static_cast<Api*>(m_api);
    auto sym = [this](const char* name) -> void* {
        return reinterpret_cast<void*>(
            GetProcAddress(static_cast<HMODULE>(m_module), name));
    };
    api->init             = reinterpret_cast<decltype(api->init)>(sym("mysql_init"));
    api->options          = reinterpret_cast<decltype(api->options)>(sym("mysql_options"));
    api->realConnect      = reinterpret_cast<decltype(api->realConnect)>(sym("mysql_real_connect"));
    api->close            = reinterpret_cast<decltype(api->close)>(sym("mysql_close"));
    api->error            = reinterpret_cast<decltype(api->error)>(sym("mysql_error"));
    api->setCharacterSet  = reinterpret_cast<decltype(api->setCharacterSet)>(sym("mysql_set_character_set"));
    api->selectDb         = reinterpret_cast<decltype(api->selectDb)>(sym("mysql_select_db"));
    api->storeResult      = reinterpret_cast<decltype(api->storeResult)>(sym("mysql_store_result"));
    api->freeResult       = reinterpret_cast<decltype(api->freeResult)>(sym("mysql_free_result"));
    api->fetchRow         = reinterpret_cast<decltype(api->fetchRow)>(sym("mysql_fetch_row"));
    api->fetchLengths     = reinterpret_cast<decltype(api->fetchLengths)>(sym("mysql_fetch_lengths"));
    api->realEscapeString = reinterpret_cast<decltype(api->realEscapeString)>(sym("mysql_real_escape_string"));
    api->realQuery        = reinterpret_cast<decltype(api->realQuery)>(sym("mysql_real_query"));
    api->insertId         = reinterpret_cast<decltype(api->insertId)>(sym("mysql_insert_id"));

    if (!api->init || !api->realConnect || !api->close || !api->error
        || !api->setCharacterSet || !api->selectDb || !api->storeResult
        || !api->freeResult || !api->fetchRow || !api->fetchLengths
        || !api->realEscapeString || !api->realQuery || !api->insertId) {
        m_lastError = QStringLiteral("libmysql.dll 缺少必要导出函数（版本不兼容）");
        unloadClient();
        return false;
    }
    return true;
#else
    m_lastError = QStringLiteral("当前平台暂不支持 MySQL 后端");
    return false;
#endif
}

void MySqlStore::unloadClient()
{
    if (m_conn) {
        static_cast<Api*>(m_api)->close(m_conn);
        m_conn = nullptr;
    }
#ifdef _WIN32
    if (m_module) {
        FreeLibrary(static_cast<HMODULE>(m_module));
        m_module = nullptr;
    }
#endif
}

// ---------------------------------------------------------------- 连接管理
bool MySqlStore::open()
{
    close();
    m_lastError.clear();

    if (!loadClient()) return false;
    Api* api = static_cast<Api*>(m_api);

    m_conn = api->init(nullptr);
    if (!m_conn) {
        m_lastError = QStringLiteral("MySQL 客户端初始化失败");
        return false;
    }
    // 超时：连接阶段最多等 5 秒，避免启动被拖死
    unsigned int timeout = 5;
    api->options(m_conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    api->options(m_conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
    api->options(m_conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    // 注：Connector/C 8+ 默认不自动重连，连接断开即报错，由上层决定回退策略

    // 第一次连接不指定库：先把库建出来
    if (!api->realConnect(m_conn,
                          m_cfg.host.toUtf8().constData(),
                          m_cfg.user.toUtf8().constData(),
                          m_cfg.password.toUtf8().constData(),
                          nullptr,
                          static_cast<unsigned int>(m_cfg.port),
                          nullptr,
                          CLIENT_MULTI_STATEMENTS)) {
        m_lastError = QStringLiteral("连接 MySQL 失败：%1")
                          .arg(QString::fromUtf8(api->error(m_conn)));
        api->close(m_conn);
        m_conn = nullptr;
        return false;
    }
    if (api->setCharacterSet(m_conn, "utf8mb4") != 0) {
        m_lastError = QStringLiteral("设置字符集 utf8mb4 失败：%1")
                          .arg(QString::fromUtf8(api->error(m_conn)));
        api->close(m_conn);
        m_conn = nullptr;
        return false;
    }
    if (!ensureDatabase()) { close(); return false; }
    if (!ensureTable())    { close(); return false; }
    return true;
}

void MySqlStore::close()
{
    if (!m_conn) return;
    static_cast<Api*>(m_api)->close(m_conn);
    m_conn = nullptr;
}

bool MySqlStore::isOpen() const
{
    return m_conn != nullptr;
}

bool MySqlStore::ensureDatabase()
{
    // 库名来自配置，先做一次白名单校验再拼进 DDL（防注入，也避免非法标识符）
    const QString db = m_cfg.database;
    static const QString kIdent = QStringLiteral("abcdefghijklmnopqrstuvwxyz"
                                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
    if (db.isEmpty()) {
        m_lastError = QStringLiteral("MySQL 库名不能为空");
        return false;
    }
    for (const QChar& c : db) {
        if (!kIdent.contains(c)) {
            m_lastError = QStringLiteral("MySQL 库名含非法字符：%1").arg(db);
            return false;
        }
    }
    const QString sql = QStringLiteral(
        "CREATE DATABASE IF NOT EXISTS `%1` DEFAULT CHARACTER SET utf8mb4 "
        "COLLATE utf8mb4_unicode_ci").arg(db);
    if (!exec(sql)) return false;
    Api* api = static_cast<Api*>(m_api);
    if (api->selectDb(m_conn, db.toUtf8().constData()) != 0) {
        m_lastError = QStringLiteral("选择数据库 %1 失败：%2")
                          .arg(db, QString::fromUtf8(api->error(m_conn)));
        return false;
    }
    return true;
}

bool MySqlStore::ensureTable()
{
    // 字段与 SQLite 后端一一对应；id 用 AUTO_INCREMENT 以便回填
    const QString sql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS `%1` ("
        "  `id`         INT NOT NULL AUTO_INCREMENT,"
        "  `buy_date`   DATE NOT NULL,"
        "  `code`       VARCHAR(16) NOT NULL,"
        "  `name`       VARCHAR(64) DEFAULT NULL,"
        "  `buy_price`  DOUBLE NOT NULL,"
        "  `qty`        INT NOT NULL,"
        "  `sell_date`  DATE DEFAULT NULL,"
        "  `sell_price` DOUBLE DEFAULT NULL,"
        "  `plan_buy`   DOUBLE DEFAULT NULL,"
        "  `plan_stop`  DOUBLE DEFAULT NULL,"
        "  `plan_take`  DOUBLE DEFAULT NULL,"
        "  `note`       TEXT,"
        "  `created_at` DATETIME DEFAULT NULL,"
        "  PRIMARY KEY (`id`),"
        "  KEY `idx_journal_date` (`buy_date`),"
        "  KEY `idx_journal_code` (`code`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").arg(QString::fromLatin1(kTable));
    return exec(sql);
}

// ---------------------------------------------------------------- 工具函数
QString MySqlStore::escape(const QString& s) const
{
    if (!m_conn) return QStringLiteral("''");
    if (s.isEmpty()) return QStringLiteral("''");
    const QByteArray raw = s.toUtf8();
    QByteArray out(raw.size() * 2 + 1, Qt::Uninitialized);   // 转义后最长翻倍
    const unsigned long n = static_cast<Api*>(m_api)->realEscapeString(
        m_conn, out.data(), raw.constData(), static_cast<unsigned long>(raw.size()));
    out.truncate(static_cast<int>(n));
    return QStringLiteral("'%1'").arg(QString::fromUtf8(out));
}

QString MySqlStore::num(double v) const
{
    // QString::number 与系统区域设置无关，不会出现千分位逗号
    return QString::number(v, 'f', 6);
}

bool MySqlStore::exec(const QString& sql)
{
    if (!m_conn) {
        m_lastError = QStringLiteral("MySQL 连接未建立");
        return false;
    }
    Api* api = static_cast<Api*>(m_api);
    const QByteArray raw = sql.toUtf8();
    if (api->realQuery(m_conn, raw.constData(), static_cast<unsigned long>(raw.size())) != 0) {
        m_lastError = QString::fromUtf8(api->error(m_conn));
        return false;
    }
    // 有结果集的话必须取走，否则下一次查询会报 "commands out of sync"
    MYSQL_RES* res = api->storeResult(m_conn);
    if (res) api->freeResult(res);
    return true;
}

// ---------------------------------------------------------------- 增删改查
bool MySqlStore::addTrade(TradeRecord& rec)
{
    if (!isOpen()) { m_lastError = QStringLiteral("MySQL 连接未建立"); return false; }
    const QString sql = QStringLiteral(
        "INSERT INTO `%1` (buy_date, code, name, buy_price, qty, sell_date, sell_price,"
        " plan_buy, plan_stop, plan_take, note, created_at) VALUES (%2, %3, %4, %5, %6,"
        " %7, %8, %9, %10, %11, %12, %13)")
        .arg(QString::fromLatin1(kTable),
             escape(rec.buyDate.toString(Qt::ISODate)),
             escape(rec.code),
             escape(rec.name),
             num(rec.buyPrice),
             QString::number(rec.qty),
             rec.sellDate.isValid() ? escape(rec.sellDate.toString(Qt::ISODate))
                                    : QStringLiteral("NULL"),
             rec.sellPrice > 0.0 ? num(rec.sellPrice) : QStringLiteral("NULL"),
             num(rec.planBuy),
             num(rec.planStop),
             num(rec.planTake),
             escape(rec.note),
             escape(QDateTime::currentDateTime().toString(Qt::ISODate)));
    if (!exec(sql)) return false;
    rec.id = static_cast<int>(static_cast<Api*>(m_api)->insertId(m_conn));
    return true;
}

bool MySqlStore::updateTrade(const TradeRecord& rec)
{
    if (!isOpen()) { m_lastError = QStringLiteral("MySQL 连接未建立"); return false; }
    if (rec.id <= 0) { m_lastError = QStringLiteral("记录 id 无效，无法更新"); return false; }
    const QString sql = QStringLiteral(
        "UPDATE `%1` SET buy_date=%2, code=%3, name=%4, buy_price=%5, qty=%6,"
        " sell_date=%7, sell_price=%8, plan_buy=%9, plan_stop=%10, plan_take=%11, note=%12"
        " WHERE id=%13")
        .arg(QString::fromLatin1(kTable),
             escape(rec.buyDate.toString(Qt::ISODate)),
             escape(rec.code),
             escape(rec.name),
             num(rec.buyPrice),
             QString::number(rec.qty),
             rec.sellDate.isValid() ? escape(rec.sellDate.toString(Qt::ISODate))
                                    : QStringLiteral("NULL"),
             rec.sellPrice > 0.0 ? num(rec.sellPrice) : QStringLiteral("NULL"),
             num(rec.planBuy),
             num(rec.planStop),
             num(rec.planTake),
             escape(rec.note),
             QString::number(rec.id));
    return exec(sql);
}

bool MySqlStore::deleteTrade(int id)
{
    if (!isOpen()) { m_lastError = QStringLiteral("MySQL 连接未建立"); return false; }
    if (id <= 0) { m_lastError = QStringLiteral("记录 id 无效，无法删除"); return false; }
    return exec(QStringLiteral("DELETE FROM `%1` WHERE id=%2")
                     .arg(QString::fromLatin1(kTable), QString::number(id)));
}

QVector<TradeRecord> MySqlStore::trades() const
{
    QVector<TradeRecord> out;
    if (!m_conn) return out;
    const QByteArray raw = QStringLiteral(
        "SELECT id, buy_date, code, name, buy_price, qty, sell_date, sell_price,"
        " plan_buy, plan_stop, plan_take, note FROM `%1` ORDER BY buy_date DESC, id DESC")
                             .arg(QString::fromLatin1(kTable)).toUtf8();
    Api* api = static_cast<Api*>(m_api);
    if (api->realQuery(m_conn, raw.constData(), static_cast<unsigned long>(raw.size())) != 0) {
        m_lastError = QString::fromUtf8(api->error(m_conn));
        return out;
    }
    MYSQL_RES* res = api->storeResult(m_conn);
    if (!res) return out;

    while (MYSQL_ROW r = api->fetchRow(res)) {
        const unsigned long* lens = api->fetchLengths(res);
        auto text = [&](int i) -> QString {
            if (!r[i]) return QString();
            return QString::fromUtf8(reinterpret_cast<const char*>(r[i]),
                                     static_cast<int>(lens ? lens[i] : 0));
        };
        auto dbl = [&](int i) -> double {
            if (!r[i]) return 0.0;
            return text(i).toDouble();
        };
        TradeRecord rec;
        rec.id       = text(0).toInt();
        rec.buyDate  = QDate::fromString(text(1), Qt::ISODate);
        rec.code     = text(2);
        rec.name     = text(3);
        rec.buyPrice = dbl(4);
        rec.qty      = text(5).toInt();
        rec.sellDate = QDate::fromString(text(6), Qt::ISODate);
        rec.sellPrice = dbl(7);
        rec.planBuy  = dbl(8);
        rec.planStop = dbl(9);
        rec.planTake = dbl(10);
        rec.note     = text(11);
        out.append(rec);
    }
    api->freeResult(res);
    return out;
}

int MySqlStore::importFrom(const QVector<TradeRecord>& legacy)
{
    if (!isOpen() || legacy.isEmpty()) return 0;
    // 已存在的记录按「代码 + 买入日 + 买入价 + 数量」判定，避免重复搬运
    QVector<TradeRecord> current = trades();
    QSet<QString> exist;
    for (const TradeRecord& r : current) {
        exist.insert(QStringLiteral("%1|%2|%3|%4")
                         .arg(r.code)
                         .arg(r.buyDate.toString(Qt::ISODate))
                         .arg(QString::number(r.buyPrice, 'f', 4))
                         .arg(r.qty));
    }
    int moved = 0;
    for (const TradeRecord& r : legacy) {
        const QString key = QStringLiteral("%1|%2|%3|%4")
                                .arg(r.code)
                                .arg(r.buyDate.toString(Qt::ISODate))
                                .arg(QString::number(r.buyPrice, 'f', 4))
                                .arg(r.qty);
        if (exist.contains(key)) continue;
        TradeRecord copy = r;
        copy.id = 0;                       // 让服务端自增
        if (!addTrade(copy)) break;        // 失败即停，保留 lastError
        exist.insert(key);
        ++moved;
    }
    return moved;
}

QString MySqlStore::describe() const
{
    return QStringLiteral("MySQL %1@%2:%3/%4")
        .arg(m_cfg.user, m_cfg.host)
        .arg(m_cfg.port)
        .arg(m_cfg.database);
}
