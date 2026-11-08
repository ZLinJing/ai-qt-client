#include "ui/TableEmptyHint.h"
#include "ui/Theme.h"
#include <QTableWidget>
#include <QLabel>
#include <QEvent>

TableEmptyHint::TableEmptyHint(QTableWidget* table, const QString& text, QObject* parent)
    : QObject(parent ? parent : table), m_table(table)
{
    m_label = new QLabel(text, table->viewport());
    m_label->setObjectName(QStringLiteral("TableEmptyHintLabel"));   // 统一空态组件靠它定位
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setWordWrap(true);
    m_label->setStyleSheet(Theme::textStyle(Theme::textFaint(), Theme::fontMd(), false)
                           + QStringLiteral("background:transparent;"));
    m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_label->setGeometry(table->viewport()->rect());
    m_label->hide();

    table->viewport()->installEventFilter(this);
}

void TableEmptyHint::setText(const QString& text)
{
    if (m_label) m_label->setText(text);
}

void TableEmptyHint::setVisible(bool visible)
{
    if (m_label) m_label->setVisible(visible);
}

bool TableEmptyHint::eventFilter(QObject* watched, QEvent* event)
{
    if (m_table && watched == m_table->viewport() && event->type() == QEvent::Resize)
        m_label->setGeometry(m_table->viewport()->rect());
    return QObject::eventFilter(watched, event);
}
