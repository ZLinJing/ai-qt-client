#include "ui/QcpAdapter.h"
#include "ui/Theme.h"

namespace QcpAdapter {

// ---------------------------------------------------------------------
// 自定义刻度生成器：2.x 的 QCPAxisTicker 通过 generate() 产出刻度与标签，
// 要显示自定义文本（如「000678 襄阳轴承」或「10-22」）必须重写该虚函数。
// ---------------------------------------------------------------------
namespace {

class CategoryTicker : public QCPAxisTicker
{
public:
    CategoryTicker(const QList<double>& positions, const QList<QString>& labels)
        : m_positions(positions), m_labels(labels) {}

    void generate(const QCPRange&, const QLocale&, QChar, int,
                  QVector<double>& ticks, QVector<double>* subTicks,
                  QVector<QString>* tickLabels) override
    {
        ticks = m_positions;
        if (subTicks) subTicks->clear();
        if (tickLabels) *tickLabels = m_labels;
    }

private:
    QList<double>  m_positions;
    QList<QString> m_labels;
};

} // namespace

AxisSet installAxes(QCPAxisRect* rect, bool withRight, bool withTop)
{
    AxisSet set;
    set.bottom = rect->addAxis(QCPAxis::atBottom);
    set.left   = rect->addAxis(QCPAxis::atLeft);
    if (withRight) set.right = rect->addAxis(QCPAxis::atRight);
    if (withTop)   set.top   = rect->addAxis(QCPAxis::atTop);

    styleAxis(set.bottom);
    styleAxis(set.left);
    if (set.right) styleAxis(set.right);
    if (set.top)   styleAxis(set.top);
    return set;
}

void styleAxis(QCPAxis* axis, bool gridOn)
{
    axis->setBasePen(QPen(Theme::border()));
    axis->setTickPen(QPen(Theme::border()));
    axis->setSubTickPen(QPen(Theme::border()));
    axis->setTickLabelColor(Theme::textMuted());
    axis->setLabelColor(Theme::textMuted());
    axis->grid()->setPen(QPen(Theme::grid(), 1, Qt::DotLine));
    axis->grid()->setSubGridVisible(false);
    axis->grid()->setZeroLinePen(Qt::NoPen);
    if (!gridOn) axis->grid()->setVisible(false);
}

void setCategoryLabels(QCPAxis* axis, const QList<double>& positions,
                       const QList<QString>& labels)
{
    axis->setTicker(QSharedPointer<QCPAxisTicker>(
        new CategoryTicker(positions, labels)));
    axis->setTickLabelColor(Theme::textMuted());
    axis->setBasePen(QPen(Theme::border()));
    axis->setTickPen(QPen(Theme::border()));
    axis->setSubTickPen(QPen(Theme::border()));
}

} // namespace QcpAdapter
