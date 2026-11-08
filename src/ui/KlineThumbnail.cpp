#include "ui/KlineThumbnail.h"
#include "ui/Theme.h"
#include "core/Indicators.h"
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QFont>
#include <QFontMetrics>
#include <QPolygonF>
#include <QColor>
#include <cmath>

namespace {

constexpr int kMaxBars = 60;      // 实际绘制的根数
constexpr int kMaWarmup = 60;     // MA60 的预热数据，否则图上只剩一个 MA60 点

bool finite(double v)
{
    return std::isfinite(v) && v > 0.0;
}

} // namespace

KlineThumbnail::KlineThumbnail(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("KlineThumbnail"));
    setMinimumHeight(150);
    setAttribute(Qt::WA_StyledBackground, true);
}

void KlineThumbnail::setData(const KlineSeries& series, double buyLow, double buyHigh,
                             double currentPrice)
{
    m_bars.clear();
    m_ma5.clear();
    m_ma20.clear();
    m_ma60.clear();
    m_buyLow = buyLow;
    m_buyHigh = buyHigh;
    m_price = currentPrice;

    const QVector<Candle>& src = series.hasRaw() ? series.rawCandles : series.candles;
    // 多取 60 根做 MA60 预热，只绘制最后 60 根，三条均线在图上才都完整
    const int from = qMax(0, src.size() - (kMaxBars + kMaWarmup));
    const QVector<Candle> calc = src.mid(from);
    m_bars = calc.mid(qMax(0, calc.size() - kMaxBars));
    const int offset = calc.size() - m_bars.size();

    if (!calc.isEmpty()) {
        QVector<double> closes;
        closes.reserve(calc.size());
        for (const Candle& c : calc) closes.append(c.close);
        m_ma5 = Ind::sma(closes, 5).mid(offset);
        m_ma20 = Ind::sma(closes, 20).mid(offset);
        m_ma60 = Ind::sma(closes, 60).mid(offset);
    }
    update();
}

void KlineThumbnail::paintEvent(QPaintEvent* e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Theme::card());
    p.setPen(QPen(Theme::border(), 1));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

    const QRectF area = QRectF(rect()).adjusted(10, 10, -10, -10);
    if (m_bars.isEmpty() || area.width() < 40.0) {
        p.setPen(Theme::textMuted());
        QFont f = p.font();
        f.setPointSizeF(Theme::fontSm());
        p.setFont(f);
        p.drawText(area, Qt::AlignCenter, QStringLiteral("暂无 K 线数据（等待行情缓存就绪）"));
        return;
    }

    const double priceH = area.height() * 0.72;
    const QRectF priceR(area.left(), area.top(), area.width(), priceH);
    const QRectF volR(area.left(), priceR.bottom() + 6.0,
                      area.width(), area.height() - priceH - 6.0);

    double lo = 1e18, hi = -1e18;
    auto consider = [&](double v) {
        if (!finite(v)) return;
        lo = qMin(lo, v);
        hi = qMax(hi, v);
    };
    for (const Candle& c : m_bars) { consider(c.low); consider(c.high); }
    for (const QVector<double>* ma : { &m_ma5, &m_ma20, &m_ma60 })
        for (double v : *ma) consider(v);
    consider(m_buyLow);
    consider(m_buyHigh);
    consider(m_price);
    if (hi <= lo) { lo = 0.0; hi = 1.0; }
    const double pad = (hi - lo) * 0.06;
    lo -= pad;
    hi += pad;

    const int n = m_bars.size();
    const double step = priceR.width() / n;
    auto xAt = [&](int i) { return priceR.left() + (i + 0.5) * step; };
    auto yAt = [&](double v) {
        return priceR.bottom() - (v - lo) / (hi - lo) * priceR.height();
    };

    // 买入价格区间：浅色底带 + 上下沿，直观看到现价相对买点的位置
    if (finite(m_buyLow) && finite(m_buyHigh) && m_buyHigh >= m_buyLow) {
        const double yTop = yAt(m_buyHigh);
        const double yBot = yAt(m_buyLow);
        QColor band = Theme::accentSoft();
        band.setAlpha(170);
        p.fillRect(QRectF(priceR.left(), yTop, priceR.width(), yBot - yTop), band);
        p.setPen(QPen(Theme::accentMuted(), 1, Qt::DashLine));
        p.drawLine(QPointF(priceR.left(), yTop), QPointF(priceR.right(), yTop));
        p.drawLine(QPointF(priceR.left(), yBot), QPointF(priceR.right(), yBot));
    }

    // 当前价水平虚线
    if (finite(m_price)) {
        const double y = yAt(m_price);
        p.setPen(QPen(Theme::accent(), 1.5, Qt::DashLine));
        p.drawLine(QPointF(priceR.left(), y), QPointF(priceR.right(), y));
    }

    // 成交量柱
    double volMax = 0.0;
    for (const Candle& c : m_bars) volMax = qMax(volMax, c.volume);
    if (volMax > 0.0) {
        const double bw = qMax(1.0, step * 0.55);
        for (int i = 0; i < n; ++i) {
            const Candle& c = m_bars.at(i);
            const double h = c.volume / volMax * volR.height();
            const bool up = c.close >= c.open;
            QColor vc = up ? Theme::up() : Theme::down();
            vc.setAlpha(140);
            p.fillRect(QRectF(xAt(i) - bw / 2.0, volR.bottom() - h, bw, h), vc);
        }
    }

    // K 线蜡烛（红涨绿跌）
    const double bodyW = qMax(1.5, step * 0.6);
    for (int i = 0; i < n; ++i) {
        const Candle& c = m_bars.at(i);
        const QColor col = c.close >= c.open ? Theme::up() : Theme::down();
        const double x = xAt(i);
        p.setPen(QPen(col, 1));
        p.drawLine(QPointF(x, yAt(c.high)), QPointF(x, yAt(c.low)));
        const double yo = yAt(c.open), yc = yAt(c.close);
        const double top = qMin(yo, yc);
        const double h = qMax(1.5, qAbs(yc - yo));
        p.fillRect(QRectF(x - bodyW / 2.0, top, bodyW, h), col);
    }

    // 三条均线
    auto drawMa = [&](const QVector<double>& ma, const QColor& col) {
        p.setPen(QPen(col, 1.4));
        QPolygonF poly;
        for (int i = 0; i < n && i < ma.size(); ++i) {
            if (!finite(ma.at(i))) continue;
            poly << QPointF(xAt(i), yAt(ma.at(i)));
        }
        if (poly.size() > 1) p.drawPolyline(poly);
    };
    drawMa(m_ma5, Theme::accentDeep());
    drawMa(m_ma20, Theme::warning());
    drawMa(m_ma60, Theme::success());

    // 图例
    QFont f = p.font();
    f.setPixelSize(Theme::fontXs());
    p.setFont(f);
    const QStringList legend = {
        QStringLiteral("MA5"), QStringLiteral("MA20"), QStringLiteral("MA60"),
        QStringLiteral("买入区间"), QStringLiteral("现价")
    };
    const QList<QColor> colors = {
        Theme::accentDeep(), Theme::warning(), Theme::success(),
        Theme::accentMuted(), Theme::accent()
    };
    double lx = priceR.left() + 2.0;
    const double ly = priceR.top() + 2.0;
    for (int i = 0; i < legend.size(); ++i) {
        const double w = QFontMetrics(f).horizontalAdvance(legend.at(i)) + 16.0;
        p.setPen(QPen(colors.at(i), 2));
        p.drawLine(QPointF(lx, ly + 7.0), QPointF(lx + 9.0, ly + 7.0));
        p.setPen(Theme::textMuted());
        p.drawText(QPointF(lx + 12.0, ly + 11.0), legend.at(i));
        lx += w;
    }
}
