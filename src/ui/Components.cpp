#include "ui/Components.h"                 // 本模块声明
#include "ui/Theme.h"                       // 令牌
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include <QMenu>
#include <QAction>
#include <QTimer>

namespace Comp {

// ---------------- Badge ----------------
QColor toneColor(BadgeTone tone)
{
    switch (tone) {
        case BadgeTone::Success: return Theme::success();
        case BadgeTone::Warning: return Theme::warning();
        case BadgeTone::Danger:  return Theme::danger();
        case BadgeTone::Accent:  return Theme::accentDeep();
        case BadgeTone::Info:    return Theme::accentDeep();
        default:                 return Theme::textMuted();
    }
}

QColor toneBackground(BadgeTone tone)
{
    switch (tone) {
        case BadgeTone::Success: return Theme::successSoft();
        case BadgeTone::Warning: return Theme::warningSoft();
        case BadgeTone::Danger:  return Theme::dangerSoft();
        case BadgeTone::Accent:  return Theme::accentSoft();
        case BadgeTone::Info:    return Theme::neutralSoft();
        default:                 return Theme::hover();
    }
}

void applyBadge(QLabel* label, BadgeTone tone)
{
    if (!label) return;
    label->setStyleSheet(Theme::chipStyle(toneColor(tone), toneBackground(tone),
                                           Theme::fontXs(), 4, 8, true));
}

QLabel* badge(const QString& text, BadgeTone tone)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("StateBadge"));
    label->setAlignment(Qt::AlignCenter);
    applyBadge(label, tone);
    return label;
}

// ---------------- SectionHeader ----------------
QFrame* sectionHeader(const QString& title, QWidget** outActionsHost)
{
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("Card"));
    auto* lay = new QHBoxLayout(frame);
    lay->setContentsMargins(Theme::Space::Lg, Theme::Space::Md,
                            Theme::Space::Lg, Theme::Space::Md);
    lay->setSpacing(Theme::Space::Md);

    auto* t = new QLabel(title, frame);
    t->setObjectName(QStringLiteral("CardTitle"));
    lay->addWidget(t, 0);
    lay->addStretch(1);

    auto* host = new QWidget(frame);
    auto* hostLay = new QHBoxLayout(host);
    hostLay->setContentsMargins(0, 0, 0, 0);
    hostLay->setSpacing(Theme::Space::Sm);
    lay->addWidget(host, 0);

    if (outActionsHost) *outActionsHost = host;
    return frame;
}

// ---------------- CollapsibleSection ----------------
CollapsibleSection::CollapsibleSection(const QString& title, QWidget* parent)
    : QFrame(parent), m_title(title)
{
    setObjectName(QStringLiteral("Card"));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(Theme::Space::Md, Theme::Space::Sm,
                            Theme::Space::Md, Theme::Space::Md);
    lay->setSpacing(Theme::Space::Sm);

    auto* head = new QWidget(this);
    auto* headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(Theme::Space::Sm);

    m_toggle = new QToolButton(head);
    m_toggle->setObjectName(QStringLiteral("CollapsibleToggle"));
    m_toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setStyleSheet(QStringLiteral(
        "QToolButton{text-align:left;padding:4px 0;border:none;background:transparent;"
        "color:%1;font-size:%2px;font-weight:600;}").arg(Theme::text().name())
        .arg(Theme::fontMd()));
    connect(m_toggle, &QToolButton::clicked, this, [this]() {
        setExpanded(!m_expanded);
        Q_EMIT expandedChanged(m_expanded);
    });
    headLay->addWidget(m_toggle, 0);

    m_count = new QLabel(head);
    m_count->setObjectName(QStringLiteral("StateBadge"));
    headLay->addWidget(m_count, 0);
    headLay->addStretch(1);

    lay->addWidget(head, 0);

    m_body = new QWidget(this);
    m_bodyLay = new QVBoxLayout(m_body);
    m_bodyLay->setContentsMargins(0, 0, 0, 0);
    m_bodyLay->setSpacing(Theme::Space::Sm);
    lay->addWidget(m_body, 0);

    refresh();
}

QWidget* CollapsibleSection::body() const { return m_body; }

bool CollapsibleSection::isExpanded() const { return m_expanded; }

void CollapsibleSection::setExpanded(bool expanded)
{
    if (m_expanded == expanded) return;
    m_expanded = expanded;
    refresh();
}

void CollapsibleSection::setTitle(const QString& title)
{
    m_title = title;
    refresh();
}

void CollapsibleSection::setCount(int count, const QString& suffix)
{
    m_countValue = count;
    m_countSuffix = suffix;
    refresh();
}

void CollapsibleSection::refresh()
{
    if (!m_toggle) return;
    // 有可折叠内容才显示整个区块；没有就整块隐藏，不留空卡片
    const bool hasContent = (m_countValue >= 0);
    setVisible(hasContent);
    if (!hasContent) {
        m_toggle->setText(m_title);
        m_count->clear();
        m_body->setVisible(false);
        return;
    }

    m_toggle->setText(QStringLiteral("%1 %2")
                      .arg(m_expanded ? QStringLiteral("\u25be") : QStringLiteral("\u25b8"), m_title));
    m_body->setVisible(m_expanded);

    if (m_countValue >= 0) {
        m_count->setText(QStringLiteral("%1%2").arg(m_countValue).arg(m_countSuffix));
        applyBadge(m_count, m_countValue > 0 ? BadgeTone::Accent : BadgeTone::Neutral);
        m_count->setVisible(true);
    } else {
        m_count->setVisible(false);
    }
}

// ---------------- EmptyState ----------------
QWidget* emptyState(const QString& title, const QString& hint, const QString& actionText)
{
    auto* box = new QWidget;
    box->setObjectName(QStringLiteral("EmptyState"));
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(Theme::Space::Xl, Theme::Space::Xl,
                            Theme::Space::Xl, Theme::Space::Xl);
    lay->setSpacing(Theme::Space::Sm);
    lay->addStretch(1);

    auto* t = new QLabel(title, box);
    t->setObjectName(QStringLiteral("EmptyTitle"));
    t->setAlignment(Qt::AlignCenter);
    t->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontMd(), true));
    lay->addWidget(t, 0);

    if (!hint.isEmpty()) {
        auto* h = new QLabel(hint, box);
        h->setObjectName(QStringLiteral("EmptyHint"));
        h->setAlignment(Qt::AlignCenter);
        h->setWordWrap(true);
        h->setStyleSheet(Theme::textStyle(Theme::textFaint(), Theme::fontSm(), false));
        lay->addWidget(h, 0);
    }

    if (!actionText.isEmpty()) {
        auto* btn = new QPushButton(actionText, box);
        btn->setObjectName(QStringLiteral("EmptyAction"));
        btn->setCursor(Qt::PointingHandCursor);
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(btn);
        row->addStretch(1);
        lay->addLayout(row);
    }

    lay->addStretch(1);
    return box;
}

void showTableEmptyHint(QTableWidget* table, const QString& title, const QString& hint)
{
    if (!table) return;
    // 复用既有的视口居中提示控件，文案交给统一组件给
    QLabel* hintLabel = table->findChild<QLabel*>(QStringLiteral("TableEmptyHintLabel"));
    if (!hintLabel) return;
    hintLabel->setText(hint.isEmpty() ? title
                                      : QStringLiteral("%1\n%2").arg(title, hint));
    hintLabel->setStyleSheet(Theme::textStyle(Theme::textFaint(), Theme::fontMd(), false)
                             + QStringLiteral("background:transparent;"));
}

// ---------------- KpiTile ----------------
KpiTile::KpiTile(const QString& label, Level level, QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("KpiTile"));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);

    auto* name = new QLabel(label, this);
    name->setObjectName(QStringLiteral("KpiLabel"));
    name->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));
    lay->addWidget(name, 0);

    m_value = new QLabel(QStringLiteral("—"), this);
    m_value->setObjectName(level == Level::Primary ? QStringLiteral("Value")
                                                   : QStringLiteral("ValueSm"));
    m_value->setStyleSheet(level == Level::Primary
        ? Theme::textStyle(Theme::text(), Theme::fontKpi(), true)
        : Theme::textStyle(Theme::text(), Theme::fontLg(), true));
    lay->addWidget(m_value, 0);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("KpiHint"));
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(Theme::textStyle(Theme::textFaint(), Theme::fontXs(), false));
    m_hint->hide();
    lay->addWidget(m_hint, 0);
}

void KpiTile::setValue(const QString& text)
{
    if (m_value) m_value->setText(text);
}

void KpiTile::setValueColor(const QColor& c)
{
    if (!m_value) return;
    const bool primary = (m_value->objectName() == QStringLiteral("Value"));
    m_value->setStyleSheet(Theme::textStyle(c, primary ? Theme::fontKpi() : Theme::fontLg(), true));
}

void KpiTile::setHint(const QString& hint)
{
    if (!m_hint) return;
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
}

// ---------------- ColumnPicker ----------------
ColumnPicker::ColumnPicker(QTableWidget* table, QPushButton* button,
                           const QVector<bool>& defaults, QObject* parent)
    : QObject(parent), m_table(table), m_button(button), m_visible(defaults)
{
    if (!m_table || !m_button) return;

    auto* menu = new QMenu(m_button);
    for (int c = 0; c < m_table->columnCount(); ++c) {
        const QString name = m_table->horizontalHeaderItem(c)
            ? m_table->horizontalHeaderItem(c)->text() : QStringLiteral("列 %1").arg(c + 1);
        auto* act = menu->addAction(name);
        act->setCheckable(true);
        act->setChecked(c < m_visible.size() ? m_visible.at(c) : true);
        connect(act, &QAction::toggled, this, [this, c](bool on) {
            if (c >= m_visible.size()) return;
            m_visible[c] = on;
            if (m_table) m_table->setColumnHidden(c, !on);
            Q_EMIT changed(encode());
        });
    }
    m_button->setMenu(menu);
    m_button->setCursor(Qt::PointingHandCursor);
    m_button->setToolTip(QStringLiteral("选择要显示的列（选择结果跨会话保留）"));

    for (int c = 0; c < m_table->columnCount(); ++c) {
        const bool vis = (c < m_visible.size()) ? m_visible.at(c) : true;
        m_table->setColumnHidden(c, !vis);
    }
}

QString ColumnPicker::encode() const
{
    QStringList ids;
    for (int c = 0; c < m_visible.size(); ++c)
        if (m_visible.at(c)) ids << QString::number(c);
    return ids.join(QLatin1Char(','));
}

bool ColumnPicker::restore(const QString& encoded)
{
    if (encoded.isEmpty() || !m_table) return false;
    const QStringList ids = encoded.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (ids.isEmpty()) return false;

    QVector<bool> next(m_table->columnCount(), false);
    for (const QString& id : ids) {
        bool ok = false;
        const int c = id.toInt(&ok);
        if (ok && c >= 0 && c < next.size()) next[c] = true;
    }
    // 一列都不显示等于把表藏了 —— 至少保留代码列
    if (!next.isEmpty()) next[0] = true;

    m_visible = next;
    for (int c = 0; c < m_table->columnCount(); ++c)
        m_table->setColumnHidden(c, !next.at(c));
    refreshActions();
    return true;
}

void ColumnPicker::refreshActions()
{
    if (!m_button || !m_button->menu()) return;
    const QList<QAction*> acts = m_button->menu()->actions();
    for (int c = 0; c < acts.size(); ++c) {
        const bool on = (c < m_visible.size()) ? m_visible.at(c) : true;
        if (acts.at(c)->isChecked() != on) acts.at(c)->setChecked(on);
    }
}

// ---------------- FeedbackCenter ----------------
FeedbackCenter::FeedbackCenter(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("FeedbackBar"));
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(Theme::Space::Xl, Theme::Space::Xs,
                            Theme::Space::Xl, Theme::Space::Xs);
    lay->setSpacing(Theme::Space::Sm);

    m_icon = new QLabel(this);
    m_icon->setObjectName(QStringLiteral("FeedbackIcon"));
    m_icon->setMinimumWidth(14);
    lay->addWidget(m_icon, 0);

    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("FeedbackText"));
    m_text->setWordWrap(true);
    lay->addWidget(m_text, 1);

    // 进度类消息 4 秒后自动淡出（错误态不自动消失，必须等用户处理）
    m_expire = new QTimer(this);
    m_expire->setSingleShot(true);
    m_expire->setInterval(4000);
    connect(m_expire, &QTimer::timeout, this, [this]() {
        if (m_current != Kind::Progress) return;
        setVisible(false);
    });

    setVisible(false);
}

void FeedbackCenter::push(Kind kind, const QString& text)
{
    if (text.isEmpty()) return;
    m_entries.append({ kind, text });
    while (m_entries.size() > 20) m_entries.removeFirst();

    // 错误态保留到被新消息取代；其余类型只显示最新一条
    m_current = kind;
    m_currentError = (kind == Kind::Error);
    refresh();

    if (m_expire) {
        if (kind == Kind::Progress) m_expire->start();
        else m_expire->stop();
    }
}

void FeedbackCenter::clearAll()
{
    m_entries.clear();
    m_currentError = false;
    refresh();
}

void FeedbackCenter::refresh()
{
    if (m_entries.isEmpty()) {
        setVisible(false);
        return;
    }
    const Entry& e = m_entries.last();
    QString icon;
    QColor color;
    switch (e.kind) {
        case Kind::Error:   icon = QStringLiteral("✕"); color = Theme::danger();  break;
        case Kind::Warn:    icon = QStringLiteral("!"); color = Theme::warning(); break;
        case Kind::Progress:icon = QStringLiteral("…"); color = Theme::accent();   break;
        default:            icon = QStringLiteral("i"); color = Theme::textMuted(); break;
    }
    m_icon->setText(icon);
    m_icon->setStyleSheet(Theme::textStyle(color, Theme::fontMd(), true));
    m_text->setText(e.text);
    m_text->setStyleSheet(Theme::textStyle(color, Theme::fontSm(), false));
    setVisible(true);
}

} // namespace Comp

