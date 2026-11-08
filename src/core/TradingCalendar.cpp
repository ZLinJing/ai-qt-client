#include "core/TradingCalendar.h"

#include "storage/Database.h"

#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

namespace {
const QString kSettingKey = QStringLiteral("calendar.nonTrading");
QMutex g_mutex;
constexpr int kMaxLearned = 400;                 // 约覆盖一年半的休市日
}

TradingCalendar& TradingCalendar::instance()
{
    static TradingCalendar inst;
    return inst;
}

void TradingCalendar::load()
{
    if (!m_db || !m_db->isOpen()) return;
    QMutexLocker locker(&g_mutex);
    m_learned.clear();
    const QStringList items = m_db->getSetting(kSettingKey)
                                  .split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& s : items) {
        const QDate d = QDate::fromString(s.trimmed(), Qt::ISODate);
        if (d.isValid()) m_learned.insert(d.toString(Qt::ISODate));
    }
}

bool TradingCalendar::isTradingDay(const QDate& d) const
{
    if (!d.isValid()) return false;
    if (d.dayOfWeek() > Qt::Friday) return false;          // 周六 / 周日
    QMutexLocker locker(&g_mutex);
    return !m_learned.contains(d.toString(Qt::ISODate));
}

void TradingCalendar::markNonTrading(const QDate& d)
{
    if (!d.isValid() || d.dayOfWeek() > Qt::Friday) return; // 只学习工作日休市

    {
        QMutexLocker locker(&g_mutex);
        if (m_learned.contains(d.toString(Qt::ISODate))) return;

        if (m_learned.size() >= kMaxLearned) {             // 容量控制：丢弃最早一半
            QStringList all;
            for (const QString& s : m_learned) all << s;
            all.sort();
            m_learned.clear();
            for (int i = all.size() / 2; i < all.size(); ++i) m_learned.insert(all.at(i));
        }
        m_learned.insert(d.toString(Qt::ISODate));
    }
    persist();
}

int TradingCalendar::learnedCount() const
{
    QMutexLocker locker(&g_mutex);
    return m_learned.size();
}

void TradingCalendar::persist()
{
    if (!m_db || !m_db->isOpen()) return;
    QStringList all;
    {
        QMutexLocker locker(&g_mutex);
        for (const QString& s : m_learned) all << s;
    }
    all.sort();
    m_db->setSetting(kSettingKey, all.join(QLatin1Char(',')));
}
