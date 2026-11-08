#pragma once

#include <QDate>
#include <QSet>
#include <QString>

class Database;

// 交易日历：周末规则 + 由数据源实测反馈「学习」到的非交易日。
// 免费行情接口不提供交易日历，硬编码节假日每年都要维护且容易写错；
// 这里改为：某工作日在正常时段确认无行情（且已越过开盘时间）时记为非交易日，
// 持久化到 app_setting，下次启动直接生效，避免节假日空跑与数据错标。
class TradingCalendar
{
public:
    static TradingCalendar& instance();

    void setDatabase(Database* db) { m_db = db; }
    void load();                                 // 从数据库恢复已学习的非交易日

    bool isTradingDay(const QDate& d) const;
    void markNonTrading(const QDate& d);         // 数据源确认当日无行情
    int  learnedCount() const;

private:
    TradingCalendar() = default;
    void persist();

    QSet<QString> m_learned;                     // ISO 日期字符串
    Database* m_db = nullptr;
};
