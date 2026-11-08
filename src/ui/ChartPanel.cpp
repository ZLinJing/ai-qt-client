#include "ui/ChartPanel.h"
#include "ui/Theme.h"
#include "ui/QcpAdapter.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

// ============================================================================
// QCustomPlot 2.x 约定（本文件已规避的坑）：
//  · plot->addGraph() 永远落在 axisRect(0)（价格区），副图必须自己 new QCPGraph(ax, ay)；
//  · 图必须挂到它要使用的轴上，否则量能会画到价格轴（量级差 6 个数量级，直接看不见）；
//  · 蜡烛图是 QCPFinancial、柱是 QCPBars，都不属于 QCPGraph ——
//    清理必须用 clearPlottables()，clearGraphs() 会留下残影；
//  · clearItems() 会 delete 十字光标等 overlay 对象，重建前必须先把指针置空。
// ============================================================================

namespace {

QString volText(double volShou)
{
    if (volShou >= 1e4) return QStringLiteral("%1 万手").arg(volShou / 1e4, 0, 'f', 2);
    return QStringLiteral("%1 手").arg(volShou, 0, 'f', 0);
}

QString pctText(double pct)
{
    return pct >= 0 ? QStringLiteral("+%1%").arg(pct, 0, 'f', 2)
                    : QStringLiteral("%1%").arg(pct, 0, 'f', 2);
}

bool isNaN(double v) { return std::isnan(v); }

void showEmptyHint(QCustomPlot* plot, const QString& text)
{
    plot->clearItems();
    auto* hint = new QCPItemText(plot);
    hint->position->setType(QCPItemPosition::ptAxisRectRatio);
    hint->position->setCoords(0.5, 0.5);
    hint->setText(text);
    hint->setColor(Theme::textMuted());
    hint->setClipToAxisRect(false);
    QFont f = hint->font();
    f.setPointSize(11);
    hint->setFont(f);
    plot->replot();
}

// QCPAxisRect 默认自带 bottom/left 两根轴；installAxes 再 addAxis 会「叠出一层」
// 0~5 的空刻度。这里把没用到的轴全部隐藏，避免图上出现第二套坐标。
void hideUnusedAxes(QCPAxisRect* rect, std::initializer_list<QCPAxis*> keep)
{
    if (!rect) return;
    for (QCPAxis* ax : rect->axes()) {
        bool used = false;
        for (QCPAxis* k : keep) {
            if (ax == k) { used = true; break; }
        }
        if (!used) ax->setVisible(false);
    }
}

QVector<double> sma(const QVector<double>& v, int period)
{
    QVector<double> out(v.size(), qQNaN());
    if (period <= 0 || v.size() < period) return out;
    double sum = 0.0;
    for (int i = 0; i < v.size(); ++i) {
        sum += v.at(i);
        if (i >= period) sum -= v.at(i - period);
        if (i >= period - 1) out[i] = sum / period;
    }
    return out;
}

QVector<double> ema(const QVector<double>& v, int period)
{
    QVector<double> out(v.size(), qQNaN());
    if (v.isEmpty() || period <= 0) return out;
    const double a = 2.0 / (period + 1.0);
    double prev = v.first();
    for (int i = 0; i < v.size(); ++i) {
        prev = (i == 0) ? v.at(i) : a * v.at(i) + (1.0 - a) * prev;
        out[i] = prev;
    }
    return out;
}

QVector<double> rsi(const QVector<double>& close, int period)
{
    QVector<double> out(close.size(), qQNaN());
    if (close.size() <= period || period <= 0) return out;
    double avgGain = 0.0;
    double avgLoss = 0.0;
    for (int i = 1; i < close.size(); ++i) {
        const double diff = close.at(i) - close.at(i - 1);
        const double gain = qMax(0.0, diff);
        const double loss = qMax(0.0, -diff);
        if (i <= period) {
            avgGain += gain;
            avgLoss += loss;
            if (i == period) {
                avgGain /= period;
                avgLoss /= period;
                out[i] = (avgGain + avgLoss) > 0.0 ? avgGain / (avgGain + avgLoss) * 100.0 : 50.0;
            }
        } else {
            avgGain = (avgGain * (period - 1) + gain) / period;
            avgLoss = (avgLoss * (period - 1) + loss) / period;
            out[i] = (avgGain + avgLoss) > 0.0 ? avgGain / (avgGain + avgLoss) * 100.0 : 50.0;
        }
    }
    return out;
}

QCPGraph* addLine(QCustomPlot* plot, QCPAxis* ax, QCPAxis* ay,
                  const QVector<double>& keys, const QVector<double>& values,
                  const QColor& color, double width, Qt::PenStyle style = Qt::SolidLine)
{
    if (keys.isEmpty() || values.isEmpty() || keys.size() != values.size()) return nullptr;
    auto* g = new QCPGraph(ax, ay);
    g->setData(keys, values, true);
    g->setPen(QPen(color, width, style));
    g->setSelectable(QCP::stNone);
    g->removeFromLegend();
    Q_UNUSED(plot);
    return g;
}

// 一条贯穿轴矩形的水平参考线（零轴 / 20-80 超买超卖区）
void addGuideLine(QCustomPlot* plot, QCPAxis* ax, QCPAxis* ay, double y,
                  const QColor& color, Qt::PenStyle style = Qt::DashLine)
{
    auto* line = new QCPItemStraightLine(plot);
    line->point1->setAxes(ax, ay);
    line->point2->setAxes(ax, ay);
    line->point1->setCoords(0.0, y);
    line->point2->setCoords(1.0, y);
    line->setPen(QPen(color, 1, style));
    line->setLayer(QStringLiteral("overlay"));
    line->setClipToAxisRect(true);
    line->setSelectable(QCP::stNone);
}

} // namespace

ChartPanel::ChartPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    buildToolbar(root);

    // 上下文行动条不进页面布局：由主窗口 reparent 到顶栏右上角
    m_pageActions = buildPageActions();

    // 图表与页签边框之间留 8px 呼吸空间
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("TabPage"));
    auto* pl = new QVBoxLayout(page);
    pl->setContentsMargins(8, 4, 8, 8);
    pl->setSpacing(0);

    m_plot = new QCustomPlot(page);
    pl->addWidget(m_plot);
    root->addWidget(page, 1);

    setupPlot();
    m_hint->setText(QStringLiteral("滚轮缩放 · 拖动平移 · 双击复位 · 移动鼠标看逐根读数"));
}

void ChartPanel::buildToolbar(QVBoxLayout* root)
{
    auto* head = new QWidget(this);
    auto* hl = new QHBoxLayout(head);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(8);

    m_title = new QLabel(QStringLiteral("从「今日计划」选择标的后自动载入 K 线"), head);
    m_title->setObjectName(QStringLiteral("Muted"));
    hl->addWidget(m_title);
    hl->addStretch();
    m_hint = new QLabel(head);
    m_hint->setObjectName(QStringLiteral("Muted"));
    hl->addWidget(m_hint);
    root->addWidget(head);

    auto* bar = new QWidget(this);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(10);

    // 周期
    h->addWidget(new QLabel(QStringLiteral("周期"), bar));
    m_periodBox = new QComboBox(bar);
    m_periodBox->addItems({ QStringLiteral("日 K"), QStringLiteral("周 K"), QStringLiteral("月 K") });
    m_periodBox->setFixedWidth(88);
    m_periodBox->setToolTip(QStringLiteral("周/月 K 由日线聚合：开=首开、收=末收、高/低取极值、量额求和"));
    h->addWidget(m_periodBox);

    h->addSpacing(6);

    // 价格区叠加指标
    m_chkMa = new QCheckBox(QStringLiteral("均线 MA5/10/20/60"), bar);
    m_chkMa->setChecked(true);
    m_chkBoll = new QCheckBox(QStringLiteral("BOLL(20,2)"), bar);
    m_chkBoll->setChecked(false);
    h->addWidget(m_chkMa);
    h->addWidget(m_chkBoll);

    h->addSpacing(6);

    // 副图指标
    h->addWidget(new QLabel(QStringLiteral("副图"), bar));
    m_indBox = new QComboBox(bar);
    m_indBox->addItems({ QStringLiteral("MACD"), QStringLiteral("KDJ"), QStringLiteral("RSI") });
    m_indBox->setFixedWidth(88);
    h->addWidget(m_indBox);

    m_chkVolMa = new QCheckBox(QStringLiteral("量能均线"), bar);
    m_chkVolMa->setChecked(true);
    h->addWidget(m_chkVolMa);

    h->addStretch();

    root->addWidget(bar);

    connect(m_periodBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        const Period p = (idx == 1) ? Period::Week : (idx == 2 ? Period::Month : Period::Day);
        if (p == m_period) return;
        m_period = p;
        rebuild();
    });
    connect(m_indBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { rebuild(); });
    connect(m_chkMa, &QCheckBox::toggled, this, [this](bool) { rebuild(); });
    connect(m_chkBoll, &QCheckBox::toggled, this, [this](bool) { rebuild(); });
    connect(m_chkVolMa, &QCheckBox::toggled, this, [this](bool) { rebuild(); });
}

// 上下文行动条：图表页只有一个主动作（复位视图），放到顶栏与其它页位置一致
QWidget* ChartPanel::buildPageActions()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("PageActions"));
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(Theme::Space::Sm);

    m_btnReset = new QPushButton(QStringLiteral("复位视图"), bar);
    m_btnReset->setToolTip(QStringLiteral("回到全量区间（等价于在图上双击）"));
    h->addWidget(m_btnReset);
    connect(m_btnReset, &QPushButton::clicked, this, [this]() { resetRange(); });

    // 未挂到顶栏前保持隐藏（否则会以 (0,0) 浮在面板上）
    bar->hide();
    return bar;
}

// 视图档位：详细视图额外显示操作提示（简洁档只留最必要的一句）
// ---------------------------------------------------------------- 绘图骨架
void ChartPanel::setupPlot()
{
    QCPLayoutGrid* grid = m_plot->plotLayout();
    grid->setRowSpacing(0);

    // 主图：QCustomPlot 默认的 axisRect(0)
    m_priceRect = m_plot->axisRect(0);
    QcpAdapter::AxisSet pa = QcpAdapter::installAxes(m_priceRect, false, true);
    m_px = pa.bottom;
    m_py = pa.left;
    m_px->setTickLabels(false);           // 日期统一显示在最下方的指标副图上
    m_py->setLabel(QStringLiteral("价格 (元)"));
    pa.top->setTickLabels(false);
    pa.top->setBasePen(Qt::NoPen);
    pa.top->setTickPen(Qt::NoPen);

    // 量能副图
    m_volRect = new QCPAxisRect(m_plot, false);
    QcpAdapter::AxisSet va = QcpAdapter::installAxes(m_volRect, true, false);
    m_vx = va.bottom;
    m_vy = va.left;
    m_vx->setTickLabels(false);
    m_vy->setLabel(QStringLiteral("成交量 (万手)"));
    va.right->setTickLabels(false);
    va.right->setBasePen(Qt::NoPen);
    va.right->setTickPen(Qt::NoPen);

    // 指标副图
    m_indRect = new QCPAxisRect(m_plot, false);
    QcpAdapter::AxisSet ia = QcpAdapter::installAxes(m_indRect, true, false);
    m_ix = ia.bottom;
    m_iy = ia.left;
    m_ix->setLabel(QStringLiteral("日期"));
    m_iy->setLabel(QStringLiteral("MACD"));
    ia.right->setTickLabels(false);
    ia.right->setBasePen(Qt::NoPen);
    ia.right->setTickPen(Qt::NoPen);

    grid->addElement(1, 0, m_volRect);
    grid->addElement(2, 0, m_indRect);
    grid->setRowStretchFactor(0, 5);
    grid->setRowStretchFactor(1, 1.3);
    grid->setRowStretchFactor(2, 1.9);

    hideUnusedAxes(m_priceRect, { m_px, m_py, pa.top });
    hideUnusedAxes(m_volRect, { m_vx, m_vy, va.right });
    hideUnusedAxes(m_indRect, { m_ix, m_iy, ia.right });

    m_plot->setBackground(QBrush(Theme::panel()));
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    m_plot->setNoAntialiasingOnDrag(true);
    for (QCPAxisRect* r : { m_priceRect, m_volRect, m_indRect }) {
        // 只允许水平拖动/缩放：Y 轴由 fitY() 依据可见区间自动重算，
        // 手动再拖 Y 会与自动缩放互相打架，反而「zoom 之后 K 线跑没影」。
        r->setRangeDrag(Qt::Horizontal);
        r->setRangeZoom(Qt::Horizontal);
        r->setRangeZoomFactor(0.85);
    }

    for (QCPAxis* ax : { m_px, m_vx, m_ix }) {
        connect(ax, QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged), this,
                [this, ax](const QCPRange&) { syncX(ax); });
    }

    connect(m_plot, &QCustomPlot::mouseMove, this, &ChartPanel::onMouseMove);
    connect(m_plot, &QCustomPlot::mouseDoubleClick, this, &ChartPanel::onMouseDoubleClick);
    m_plot->installEventFilter(this);

    // 顺序不能反：showEmptyHint 内部会 clearItems()，把刚建好的 overlay 一起删掉
    showEmptyHint(m_plot, QStringLiteral("暂无 K 线 —— 在「荐股计划」中选择一只标的后自动载入"));
    ensureOverlay();
}

void ChartPanel::ensureOverlay()
{
    if (!m_crossH || !m_probe) {
        if (!m_px || !m_py) return;

        // 竖线：每个轴矩形一条（同一条直线无法跨轴矩形裁剪）
        if (m_crossV.isEmpty()) {
            const struct { QCPAxisRect* rect; QCPAxis* x; QCPAxis* y; } panes[] = {
                { m_priceRect, m_px, m_py },
                { m_volRect,   m_vx, m_vy },
                { m_indRect,   m_ix, m_iy },
            };
            for (const auto& p : panes) {
                if (!p.rect || !p.x || !p.y) continue;
                auto* line = new QCPItemStraightLine(m_plot);
                line->point1->setAxes(p.x, p.y);
                line->point2->setAxes(p.x, p.y);
                line->setPen(QPen(QColor(90, 100, 115, 140), 1, Qt::DashLine));
                line->setLayer(QStringLiteral("overlay"));
                line->setClipToAxisRect(true);
                line->setClipAxisRect(p.rect);
                line->setSelectable(QCP::stNone);
                line->setVisible(false);
                m_crossV.append(line);
            }
        }

        // 横线：只在价格区显示（量能轴的量纲与价格无关，画横线会误导）
        m_crossH = new QCPItemStraightLine(m_plot);
        m_crossH->point1->setAxes(m_px, m_py);
        m_crossH->point2->setAxes(m_px, m_py);
        m_crossH->setPen(QPen(QColor(90, 100, 115, 140), 1, Qt::DashLine));
        m_crossH->setLayer(QStringLiteral("overlay"));
        m_crossH->setClipToAxisRect(true);
        m_crossH->setClipAxisRect(m_priceRect);
        m_crossH->setSelectable(QCP::stNone);
        m_crossH->setVisible(false);

        m_probe = new QCPItemText(m_plot);
        m_probe->position->setType(QCPItemPosition::ptAbsolute);
        m_probe->setPositionAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_probe->setTextAlignment(Qt::AlignLeft);
        m_probe->setBrush(QBrush(QColor(255, 255, 255, 242)));
        m_probe->setPen(QPen(Theme::border()));
        m_probe->setPadding(QMargins(8, 6, 8, 6));
        m_probe->setColor(Theme::text());
        m_probe->setFont(QFont(QString(), 9));
        m_probe->setClipToAxisRect(false);
        m_probe->setLayer(QStringLiteral("overlay"));
        m_probe->setVisible(false);
    }
}

// ---------------------------------------------------------------- 数据聚合
void ChartPanel::aggregate()
{
    m_bars.clear();
    const QVector<Candle>& src = m_series.candles;
    if (src.isEmpty()) return;
    m_bars.reserve(m_period == Period::Day ? src.size() : src.size() / 4 + 2);

    auto sameGroup = [this](const Bar& bar, const QDate& d) {
        switch (m_period) {
        case Period::Day:
            return false;
        case Period::Week: {
            int y1 = 0, y2 = 0;
            return bar.to.weekNumber(&y1) == d.weekNumber(&y2) && y1 == y2;
        }
        case Period::Month:
            return bar.to.year() == d.year() && bar.to.month() == d.month();
        }
        return false;
    };

    for (const Candle& c : src) {
        if (c.date.isValid() == false) continue;
        if (m_bars.isEmpty() || !sameGroup(m_bars.last(), c.date)) {
            Bar b;
            b.c = c;
            b.from = c.date;
            b.to = c.date;
            b.days = 1;
            m_bars.append(b);
        } else {
            Bar& b = m_bars.last();
            b.c.high = qMax(b.c.high, c.high);
            b.c.low = (b.c.low > 0.0) ? qMin(b.c.low, c.low) : c.low;
            b.c.close = c.close;
            b.c.volume += c.volume;
            b.c.amount += c.amount;
            b.to = c.date;
            ++b.days;
        }
    }
}

void ChartPanel::computeIndicators()
{
    const int n = m_bars.size();
    m_ma5.clear(); m_ma10.clear(); m_ma20.clear(); m_ma60.clear();
    m_bollUp.clear(); m_bollMid.clear(); m_bollDn.clear();
    m_dif.clear(); m_dea.clear(); m_macd.clear();
    m_k.clear(); m_d.clear(); m_j.clear();
    m_rsi6.clear(); m_rsi12.clear(); m_rsi24.clear();
    m_volMa5.clear();
    m_volMax = 0.0;
    if (n == 0) return;

    QVector<double> close(n), high(n), low(n), vol(n);
    for (int i = 0; i < n; ++i) {
        close[i] = m_bars.at(i).c.close;
        high[i] = m_bars.at(i).c.high;
        low[i] = m_bars.at(i).c.low;
        vol[i] = m_bars.at(i).c.volume / 1e4;   // 万手，与副图轴口径一致
        m_volMax = qMax(m_volMax, vol[i]);
    }

    m_ma5 = sma(close, 5);
    m_ma10 = sma(close, 10);
    m_ma20 = sma(close, 20);
    m_ma60 = sma(close, 60);

    // ---- BOLL(20, 2) ----
    m_bollMid = m_ma20;
    m_bollUp = QVector<double>(n, qQNaN());
    m_bollDn = QVector<double>(n, qQNaN());
    const int bp = 20;
    for (int i = bp - 1; i < n; ++i) {
        double sum = 0.0;
        for (int j = i - bp + 1; j <= i; ++j) sum += close.at(j);
        const double mean = sum / bp;
        double var = 0.0;
        for (int j = i - bp + 1; j <= i; ++j) {
            const double d = close.at(j) - mean;
            var += d * d;
        }
        const double sd = std::sqrt(var / bp);
        m_bollMid[i] = mean;
        m_bollUp[i] = mean + 2.0 * sd;
        m_bollDn[i] = mean - 2.0 * sd;
    }

    // ---- MACD(12, 26, 9) ----
    const QVector<double> e12 = ema(close, 12);
    const QVector<double> e26 = ema(close, 26);
    m_dif.resize(n);
    for (int i = 0; i < n; ++i) m_dif[i] = e12.at(i) - e26.at(i);
    m_dea = ema(m_dif, 9);
    m_macd.resize(n);
    for (int i = 0; i < n; ++i) m_macd[i] = 2.0 * (m_dif.at(i) - m_dea.at(i));

    // ---- KDJ(9, 3, 3) ----
    m_k.resize(n);
    m_d.resize(n);
    m_j.resize(n);
    {
        double k = 50.0, d = 50.0;
        for (int i = 0; i < n; ++i) {
            const int from = qMax(0, i - 8);
            double hh = high.at(from), ll = low.at(from);
            for (int j = from; j <= i; ++j) {
                hh = qMax(hh, high.at(j));
                ll = qMin(ll, low.at(j));
            }
            const double rsv = (hh > ll) ? (close.at(i) - ll) / (hh - ll) * 100.0 : 50.0;
            k = (2.0 * k + rsv) / 3.0;
            d = (2.0 * d + k) / 3.0;
            m_k[i] = k;
            m_d[i] = d;
            m_j[i] = 3.0 * k - 2.0 * d;
        }
    }

    // ---- RSI(6/12/24) ----
    m_rsi6 = rsi(close, 6);
    m_rsi12 = rsi(close, 12);
    m_rsi24 = rsi(close, 24);

    // ---- 量能均线 ----
    m_volMa5 = sma(vol, 5);
}

// ---------------------------------------------------------------- 重绘
void ChartPanel::rebuild()
{
    if (!m_plot) return;

    // clearItems() 会 delete overlay 对象，指针必须先失效再重建
    m_crossV.clear();
    m_crossH = nullptr;
    m_probe = nullptr;
    m_maNote = nullptr;
    m_plot->clearPlottables();   // 蜡烛/柱不是 QCPGraph，clearGraphs 会残留
    m_plot->clearItems();

    m_bars.clear();
    m_hasData = false;

    if (!m_series.valid()) {
        m_title->setText(m_code.isEmpty()
            ? QStringLiteral("从「荐股计划」选择标的后自动载入 K 线")
            : QStringLiteral("当前标的：%1 %2（暂无 K 线数据）").arg(m_code, m_name));
        showEmptyHint(m_plot, QStringLiteral("暂无 K 线数据 —— 行情接口未返回，稍后自动重试"));
        ensureOverlay();
        return;
    }

    aggregate();
    computeIndicators();
    if (m_bars.isEmpty()) {
        showEmptyHint(m_plot, QStringLiteral("暂无 K 线数据"));
        ensureOverlay();
        return;
    }

    plotPrice();
    plotVolume();
    plotIndicator();

    m_hasData = true;
    ensureOverlay();
    resetRange();

    const QString periodName = (m_period == Period::Week) ? QStringLiteral("周K")
                            : (m_period == Period::Month ? QStringLiteral("月K") : QStringLiteral("日K"));
    m_title->setText(QStringLiteral("当前标的：%1 %2 ｜ %3 %4 根（数据窗口 %5 个交易日）")
                         .arg(m_code, m_name, periodName)
                         .arg(m_bars.size())
                         .arg(m_series.candles.size()));
}

void ChartPanel::plotPrice()
{
    const int n = m_bars.size();
    QVector<double> ts(n), op(n), hi(n), lo(n), cl(n);
    for (int i = 0; i < n; ++i) {
        const Bar& b = m_bars.at(i);
        ts[i] = i;
        op[i] = b.c.open;
        hi[i] = b.c.high;
        lo[i] = b.c.low;
        cl[i] = b.c.close;
    }

    // ---- K 线体（红涨绿跌）----
    auto* candles = new QCPFinancial(m_px, m_py);
    candles->setData(ts, op, hi, lo, cl);
    candles->setTwoColored(true);
    candles->setBrushPositive(QBrush(Theme::up()));
    candles->setBrushNegative(QBrush(Theme::down()));
    candles->setPenPositive(QPen(Theme::up().darker(115)));
    candles->setPenNegative(QPen(Theme::down().darker(115)));
    candles->setWidth(0.62);
    candles->setName((m_period == Period::Week) ? QStringLiteral("周K")
                   : (m_period == Period::Month ? QStringLiteral("月K") : QStringLiteral("日K")));
    candles->setSelectable(QCP::stNone);

    // ---- 均线 ----
    if (m_chkMa && m_chkMa->isChecked()) {
        struct Line { const QVector<double>* v; int period; const char* color; };
        const Line lines[] = {
            { &m_ma5,  5,  "#2563eb" },
            { &m_ma10, 10, "#e08b12" },
            { &m_ma20, 20, "#8e44ad" },
            { &m_ma60, 60, "#6b7480" },
        };
        for (const Line& l : lines) {
            if (l.v->size() != n) continue;
            addLine(m_plot, m_px, m_py, ts, *l.v, QColor(QString::fromLatin1(l.color)), 1.5);
        }
    }

    // ---- BOLL：带宽用浅色填充，一眼能看出「挤压 / 张口」----
    if (m_chkBoll && m_chkBoll->isChecked() && m_bollUp.size() == n) {
        const QColor band("#00a0b0");
        QCPGraph* up = addLine(m_plot, m_px, m_py, ts, m_bollUp, band, 1.1);
        QCPGraph* dn = addLine(m_plot, m_px, m_py, ts, m_bollDn, band, 1.1);
        addLine(m_plot, m_px, m_py, ts, m_bollMid, band.darker(110), 1.2, Qt::DashLine);
        if (up && dn) {
            up->setChannelFillGraph(dn);
            up->setBrush(QBrush(QColor(0, 160, 176, 26)));
        }
    }

    // ---- 均线 / BOLL 图注（贴左上角，避免图例堆成一片）----
    {
        QStringList parts;
        if (m_chkMa && m_chkMa->isChecked() && n > 0) {
            struct Item { int period; const QVector<double>* v; };
            const Item items[] = { { 5, &m_ma5 }, { 10, &m_ma10 }, { 20, &m_ma20 }, { 60, &m_ma60 } };
            for (const Item& it : items) {
                if (it.v->size() != n) continue;
                const double v = it.v->last();
                if (isNaN(v)) continue;
                parts << QStringLiteral("MA%1 %2").arg(it.period).arg(v, 0, 'f', 2);
            }
        }
        if (m_chkBoll && m_chkBoll->isChecked() && m_bollMid.size() == n) {
            const double mid = m_bollMid.last();
            const double upv = m_bollUp.last();
            const double dnv = m_bollDn.last();
            if (!isNaN(mid))
                parts << QStringLiteral("BOLL %1 / %2 / %3")
                             .arg(upv, 0, 'f', 2).arg(mid, 0, 'f', 2).arg(dnv, 0, 'f', 2);
        }
        m_maNote = new QCPItemText(m_plot);
        m_maNote->position->setType(QCPItemPosition::ptAxisRectRatio);
        m_maNote->position->setCoords(0.012, 0.02);
        m_maNote->setPositionAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_maNote->setText(parts.join(QStringLiteral("   ")));
        m_maNote->setColor(Theme::textMuted());
        m_maNote->setFont(QFont(QString(), 8));
        m_maNote->setClipToAxisRect(false);
        m_maNote->setLayer(QStringLiteral("overlay"));
    }

    // ---- 涨停标记（单条 graph 多点，避免几十条图例）----
    // 周/月 K 下把「该周期内出现过涨停」合并成一个标记，板数取最大值，避免同月多个三角叠在一起。
    QMap<int, int> markBoard;
    if (m_period == Period::Day) {
        QHash<QDate, int> byDate;
        for (const LimitEntry& e : m_marks) {
            if (e.code != m_code || !e.tradeDate.isValid()) continue;
            byDate[e.tradeDate] = qMax(byDate.value(e.tradeDate), e.boardCount);
        }
        for (int i = 0; i < n; ++i) {
            const auto it = byDate.constFind(m_bars.at(i).to);
            if (it != byDate.constEnd()) markBoard.insert(i, it.value());
        }
    } else {
        for (const LimitEntry& e : m_marks) {
            if (e.code != m_code || !e.tradeDate.isValid()) continue;
            for (int i = 0; i < n; ++i) {
                if (e.tradeDate >= m_bars.at(i).from && e.tradeDate <= m_bars.at(i).to) {
                    markBoard[i] = qMax(markBoard.value(i), e.boardCount);
                    break;
                }
            }
        }
    }

    if (!markBoard.isEmpty()) {
        QVector<double> mkX, mkY;
        QVector<int> mkBoard;
        for (auto it = markBoard.constBegin(); it != markBoard.constEnd(); ++it) {
            mkX.append(it.key());
            mkY.append(m_bars.at(it.key()).c.low * 0.985);
            mkBoard.append(it.value());
        }
        QCPGraph* mg = new QCPGraph(m_px, m_py);
        mg->setData(mkX, mkY);
        mg->setLineStyle(QCPGraph::lsNone);
        QCPScatterStyle st;
        st.setShape(QCPScatterStyle::ssTriangle);   // 实心三角，保证可见
        st.setSize(8);
        st.setPen(QPen(Theme::up().darker(120), 1));
        st.setBrush(QBrush(Theme::up()));
        mg->setScatterStyle(st);
        mg->setName(QStringLiteral("涨停"));
        mg->setSelectable(QCP::stNone);

        // 2 板及以上标注板数（1 板太多，标了反而糊）
        for (int k = 0; k < mkX.size(); ++k) {
            if (mkBoard[k] < 2) continue;
            auto* t = new QCPItemText(m_plot);
            t->position->setAxes(m_px, m_py);
            t->position->setCoords(mkX[k], mkY[k] - (m_py->range().size() * 0.01));
            t->setText(QStringLiteral("%1板").arg(mkBoard[k]));
            t->setColor(Theme::up());
            t->setFont(QFont(QString(), 7));
            t->setPositionAlignment(Qt::AlignHCenter | Qt::AlignTop);
            t->setClipToAxisRect(true);
            t->setLayer(QStringLiteral("overlay"));
        }
    }

    // ---- 最近涨停价参考线（去重，最多两条）----
    QList<double> prices;
    for (const LimitEntry& e : m_marks) {
        if (e.code != m_code || e.limitPrice <= 0) continue;
        if (!prices.contains(e.limitPrice)) prices << e.limitPrice;
    }
    std::sort(prices.begin(), prices.end(), std::greater<double>());
    for (int i = 0; i < qMin(2, prices.size()); ++i) {
        auto* line = new QCPItemStraightLine(m_plot);
        line->point1->setAxes(m_px, m_py);
        line->point2->setAxes(m_px, m_py);
        line->point1->setCoords(0, prices[i]);
        line->point2->setCoords(1, prices[i]);
        line->setPen(QPen(QColor(217, 48, 37, 170), 1.1, Qt::DashLine));
        line->setLayer(QStringLiteral("overlay"));
        line->setClipToAxisRect(true);
        line->setSelectable(QCP::stNone);

        auto* txt = new QCPItemText(m_plot);
        txt->position->setAxes(m_px, m_py);
        txt->position->setCoords(0.6, prices[i]);   // 贴左侧，避开右上角
        txt->setText(QStringLiteral("涨停价 %1").arg(prices[i], 0, 'f', 2));
        txt->setColor(Theme::up());
        txt->setFont(QFont(QString(), 8));
        txt->setPositionAlignment(Qt::AlignLeft | Qt::AlignBottom);
        txt->setBrush(QBrush(QColor(255, 255, 255, 215)));
        txt->setPen(QPen(Theme::border()));
        txt->setPadding(QMargins(4, 1, 4, 1));
        txt->setLayer(QStringLiteral("overlay"));
    }
}

void ChartPanel::plotVolume()
{
    const int n = m_bars.size();
    QVector<double> vxUp, vyUp, vxDown, vyDown;
    for (int i = 0; i < n; ++i) {
        const Bar& b = m_bars.at(i);
        const double v = b.c.volume / 1e4;   // 万手
        if (b.c.close >= b.c.open) { vxUp << i; vyUp << v; }
        else                       { vxDown << i; vyDown << v; }
    }
    auto addVol = [this](const QVector<double>& kx, const QVector<double>& vy, const QColor& col) {
        if (kx.isEmpty()) return;
        auto* bars = new QCPBars(m_vx, m_vy);
        bars->setData(kx, vy);
        bars->setWidth(0.62);
        bars->setPen(Qt::NoPen);
        bars->setBrush(QBrush(col));
        bars->setSelectable(QCP::stNone);
    };
    addVol(vxUp, vyUp, QColor(217, 48, 37, 150));
    addVol(vxDown, vyDown, QColor(26, 158, 92, 150));

    if (m_chkVolMa && m_chkVolMa->isChecked() && m_volMa5.size() == n) {
        QVector<double> ts(n);
        for (int i = 0; i < n; ++i) ts[i] = i;
        addLine(m_plot, m_vx, m_vy, ts, m_volMa5, QColor("#e08b12"), 1.2);
    }
}

void ChartPanel::plotIndicator()
{
    const int n = m_bars.size();
    QVector<double> ts(n);
    for (int i = 0; i < n; ++i) ts[i] = i;

    const int kind = m_indBox ? m_indBox->currentIndex() : 0;
    if (kind == 1) {          // KDJ
        m_iy->setLabel(QStringLiteral("KDJ"));
        addGuideLine(m_plot, m_ix, m_iy, 80.0, QColor(217, 48, 37, 120));
        addGuideLine(m_plot, m_ix, m_iy, 20.0, QColor(26, 158, 92, 120));
        addLine(m_plot, m_ix, m_iy, ts, m_k, QColor("#2563eb"), 1.4);
        addLine(m_plot, m_ix, m_iy, ts, m_d, QColor("#e08b12"), 1.4);
        addLine(m_plot, m_ix, m_iy, ts, m_j, QColor("#8e44ad"), 1.2);
    } else if (kind == 2) {   // RSI
        m_iy->setLabel(QStringLiteral("RSI"));
        addGuideLine(m_plot, m_ix, m_iy, 70.0, QColor(217, 48, 37, 120));
        addGuideLine(m_plot, m_ix, m_iy, 30.0, QColor(26, 158, 92, 120));
        addLine(m_plot, m_ix, m_iy, ts, m_rsi6, QColor("#2563eb"), 1.4);
        addLine(m_plot, m_ix, m_iy, ts, m_rsi12, QColor("#e08b12"), 1.3);
        addLine(m_plot, m_ix, m_iy, ts, m_rsi24, QColor("#8e44ad"), 1.2);
    } else {                  // MACD
        m_iy->setLabel(QStringLiteral("MACD"));
        QVector<double> posX, posY, negX, negY;
        for (int i = 0; i < n; ++i) {
            const double v = m_macd.value(i);
            if (isNaN(v)) continue;
            if (v >= 0) { posX << i; posY << v; }
            else        { negX << i; negY << v; }
        }
        auto addBars = [this](const QVector<double>& kx, const QVector<double>& vy, const QColor& c) {
            if (kx.isEmpty()) return;
            auto* bars = new QCPBars(m_ix, m_iy);
            bars->setData(kx, vy);
            bars->setWidth(0.55);
            bars->setPen(Qt::NoPen);
            bars->setBrush(QBrush(c));
            bars->setSelectable(QCP::stNone);
        };
        addBars(posX, posY, QColor(217, 48, 37, 170));
        addBars(negX, negY, QColor(26, 158, 92, 170));
        addGuideLine(m_plot, m_ix, m_iy, 0.0, QColor(150, 158, 170, 130), Qt::DotLine);
        addLine(m_plot, m_ix, m_iy, ts, m_dif, QColor("#2563eb"), 1.4);
        addLine(m_plot, m_ix, m_iy, ts, m_dea, QColor("#e08b12"), 1.4);
    }
}

// ---------------------------------------------------------------- 视野
void ChartPanel::resetRange()
{
    if (!m_hasData || m_bars.isEmpty()) return;
    applyRange(QCPRange(-0.6, m_bars.size() - 0.4));
}

void ChartPanel::applyRange(const QCPRange& xr)
{
    if (!m_px) return;
    m_syncing = true;
    for (QCPAxis* ax : { m_px, m_vx, m_ix }) {
        if (!ax) continue;
        QSignalBlocker block(ax);
        ax->setRange(xr);
    }
    fitY(xr);
    updateDateTicks(xr);
    m_syncing = false;
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ChartPanel::syncX(QCPAxis* source)
{
    if (m_syncing || !m_hasData || !source) return;
    applyRange(source->range());
}

void ChartPanel::fitY(const QCPRange& xr)
{
    const int n = m_bars.size();
    if (n <= 0) return;

    const int i0 = qBound(0, int(std::floor(xr.lower)), n - 1);
    const int i1 = qBound(0, int(std::ceil(xr.upper)), n - 1);
    if (i1 < i0) return;

    // ---- 价格区：可见 K 线的高低点，若叠加了均线/BOLL 也纳入 ----
    double lo = m_bars.at(i0).c.low;
    double hi = m_bars.at(i0).c.high;
    double volMax = 0.0;
    for (int i = i0; i <= i1; ++i) {
        lo = qMin(lo, m_bars.at(i).c.low);
        hi = qMax(hi, m_bars.at(i).c.high);
        volMax = qMax(volMax, m_bars.at(i).c.volume / 1e4);
    }
    auto extend = [&](const QVector<double>& v) {
        for (int i = i0; i <= i1 && i < v.size(); ++i) {
            if (isNaN(v.at(i))) continue;
            lo = qMin(lo, v.at(i));
            hi = qMax(hi, v.at(i));
        }
    };
    if (m_chkMa && m_chkMa->isChecked()) {
        extend(m_ma5); extend(m_ma10); extend(m_ma20); extend(m_ma60);
    }
    if (m_chkBoll && m_chkBoll->isChecked()) { extend(m_bollUp); extend(m_bollDn); }

    if (hi <= lo) { hi = lo + 0.01; }
    const double pad = (hi - lo) * 0.07 + 0.005;
    m_py->setRange(QCPRange(lo - pad, hi + pad));

    // ---- 量能区：可见区间最大量 ----
    if (m_chkVolMa && m_chkVolMa->isChecked()) {
        for (int i = i0; i <= i1 && i < m_volMa5.size(); ++i)
            if (!isNaN(m_volMa5.at(i))) volMax = qMax(volMax, m_volMa5.at(i));
    }
    m_vy->setRange(QCPRange(0.0, qMax(0.01, volMax * 1.18)));

    // ---- 指标区 ----
    double ilo = 0.0, ihi = 0.0;
    bool first = true;
    auto scan = [&](const QVector<double>& v) {
        for (int i = i0; i <= i1 && i < v.size(); ++i) {
            const double val = v.at(i);
            if (isNaN(val)) continue;
            if (first) { ilo = ihi = val; first = false; }
            else { ilo = qMin(ilo, val); ihi = qMax(ihi, val); }
        }
    };
    const int kind = m_indBox ? m_indBox->currentIndex() : 0;
    if (kind == 1) {
        scan(m_k); scan(m_d); scan(m_j);
        ilo = qMin(ilo, 0.0); ihi = qMax(ihi, 100.0);
    } else if (kind == 2) {
        scan(m_rsi6); scan(m_rsi12); scan(m_rsi24);
        ilo = qMin(ilo, 0.0); ihi = qMax(ihi, 100.0);
    } else {
        scan(m_dif); scan(m_dea); scan(m_macd);
        ilo = qMin(ilo, 0.0); ihi = qMax(ihi, 0.0);
    }
    if (first) { ilo = -1.0; ihi = 1.0; }
    if (ihi <= ilo) ihi = ilo + 1.0;
    const double ipad = (ihi - ilo) * 0.12 + 0.001;
    m_iy->setRange(QCPRange(ilo - ipad, ihi + ipad));
}

void ChartPanel::updateDateTicks(const QCPRange& xr)
{
    const int n = m_bars.size();
    if (n <= 0) return;
    const int i0 = qBound(0, int(std::floor(xr.lower)), n - 1);
    const int i1 = qBound(0, int(std::ceil(xr.upper)), n - 1);
    if (i1 < i0) return;

    const int visible = i1 - i0 + 1;
    const int step = qMax(1, visible / 7);
    const QString fmt = (m_period == Period::Month) ? QStringLiteral("yy-MM")
                      : QStringLiteral("MM-dd");

    QList<double> ticks;
    QList<QString> labels;
    for (int i = i0; i <= i1; i += step) {
        ticks.append(i);
        labels.append(m_bars.at(i).to.toString(fmt));
    }
    // 最后一个可见位置补一个刻度，缩放后右侧不会「没有日期」
    if (!ticks.isEmpty() && ticks.last() != double(i1)) {
        ticks.append(i1);
        labels.append(m_bars.at(i1).to.toString(fmt));
    }
    QcpAdapter::setCategoryLabels(m_ix, ticks, labels);
}

// ---------------------------------------------------------------- 交互
QString ChartPanel::probeText(int idx) const
{
    if (idx < 0 || idx >= m_bars.size()) return QString();
    const Bar& b = m_bars.at(idx);
    const double prev = idx > 0 ? m_bars.at(idx - 1).c.close : b.c.open;
    const double pct = prev > 0.0 ? (b.c.close - prev) / prev * 100.0 : 0.0;

    QString head = b.to.toString(QStringLiteral("yyyy-MM-dd"));
    if (m_period != Period::Day && b.from != b.to) {
        head += QStringLiteral("（%1 ~ %2，%3 个交易日）")
                    .arg(b.from.toString(QStringLiteral("MM-dd")),
                         b.to.toString(QStringLiteral("MM-dd")))
                    .arg(b.days);
    }
    // 涨停标记
    for (const LimitEntry& e : m_marks) {
        if (e.code != m_code) continue;
        if (e.tradeDate >= b.from && e.tradeDate <= b.to) {
            head += QStringLiteral("  ★ %1板").arg(e.boardCount);
            break;
        }
    }

    QString info = QStringLiteral("%1\n开 %2  高 %3  低 %4  收 %5\n涨跌 %6  量 %7")
                       .arg(head)
                       .arg(b.c.open, 0, 'f', 2)
                       .arg(b.c.high, 0, 'f', 2)
                       .arg(b.c.low, 0, 'f', 2)
                       .arg(b.c.close, 0, 'f', 2)
                       .arg(pctText(pct), volText(b.c.volume));
    if (b.c.amount > 0) info += QStringLiteral("  额 %1亿").arg(b.c.amount / 1e8, 0, 'f', 2);

    if (m_chkMa && m_chkMa->isChecked()) {
        QStringList ma;
        struct Item { int period; const QVector<double>* v; };
        const Item items[] = { { 5, &m_ma5 }, { 10, &m_ma10 }, { 20, &m_ma20 }, { 60, &m_ma60 } };
        for (const Item& it : items) {
            if (idx >= it.v->size()) continue;
            const double v = it.v->at(idx);
            if (isNaN(v)) continue;
            ma << QStringLiteral("MA%1 %2").arg(it.period).arg(v, 0, 'f', 2);
        }
        if (!ma.isEmpty()) info += QLatin1Char('\n') + ma.join(QStringLiteral("  "));
    }
    if (m_chkBoll && m_chkBoll->isChecked() && idx < m_bollMid.size()) {
        if (!isNaN(m_bollMid.at(idx)))
            info += QStringLiteral("\nBOLL 上 %1  中 %2  下 %3")
                        .arg(m_bollUp.at(idx), 0, 'f', 2)
                        .arg(m_bollMid.at(idx), 0, 'f', 2)
                        .arg(m_bollDn.at(idx), 0, 'f', 2);
    }

    const int kind = m_indBox ? m_indBox->currentIndex() : 0;
    if (kind == 1 && idx < m_k.size()) {
        info += QStringLiteral("\nKDJ  K %1  D %2  J %3")
                    .arg(m_k.at(idx), 0, 'f', 2)
                    .arg(m_d.at(idx), 0, 'f', 2)
                    .arg(m_j.at(idx), 0, 'f', 2);
    } else if (kind == 2 && idx < m_rsi6.size()) {
        auto rv = [idx](const QVector<double>& v) {
            return (idx < v.size() && !isNaN(v.at(idx)))
                       ? QString::number(v.at(idx), 'f', 2) : QStringLiteral("-");
        };
        info += QStringLiteral("\nRSI  6 %1  12 %2  24 %3")
                    .arg(rv(m_rsi6), rv(m_rsi12), rv(m_rsi24));
    } else if (idx < m_dif.size()) {
        info += QStringLiteral("\nMACD  DIF %1  DEA %2  MACD %3")
                    .arg(m_dif.at(idx), 0, 'f', 3)
                    .arg(m_dea.at(idx), 0, 'f', 3)
                    .arg(m_macd.at(idx), 0, 'f', 3);
    }
    return info;
}

void ChartPanel::onMouseMove(QMouseEvent* e)
{
    if (!m_hasData || m_crossV.isEmpty() || !m_crossH || !m_probe) return;

    // 落在哪个轴矩形里（价格 / 量能 / 指标都能读数）
    QCPAxisRect* hit = nullptr;
    for (QCPAxisRect* r : { m_priceRect, m_volRect, m_indRect }) {
        if (r && r->rect().contains(e->pos())) { hit = r; break; }
    }
    if (!hit) {
        hideProbe();
        return;
    }

    const double xd = m_px->pixelToCoord(e->pos().x());
    const int idx = qBound(0, qRound(xd), m_bars.size() - 1);

    for (QCPItemStraightLine* line : m_crossV) {
        line->point1->setCoords(idx, line->point1->valueAxis()->range().lower);
        line->point2->setCoords(idx, line->point1->valueAxis()->range().upper);
        line->setVisible(true);
    }

    const bool inPrice = (hit == m_priceRect);
    if (inPrice) {
        m_crossH->point1->setCoords(m_px->range().lower, m_py->pixelToCoord(e->pos().y()));
        m_crossH->point2->setCoords(m_px->range().upper, m_py->pixelToCoord(e->pos().y()));
    }
    m_crossH->setVisible(inPrice);

    // 读数框跟随鼠标，并在右侧/下方空间不足时自动翻转，避免被裁掉
    m_probe->position->setType(QCPItemPosition::ptAbsolute);
    QPointF anchor = e->pos() + QPointF(14, 14);
    Qt::Alignment align = Qt::AlignLeft | Qt::AlignTop;
    if (anchor.x() + 250 > m_plot->width()) {
        anchor.setX(e->pos().x() - 14.0);
        align = Qt::AlignRight | Qt::AlignTop;
    }
    if (anchor.y() + 120 > m_plot->height()) {
        anchor.setY(e->pos().y() - 14.0);
        align = (align & Qt::AlignRight) ? (Qt::AlignRight | Qt::AlignBottom)
                                         : (Qt::AlignLeft | Qt::AlignBottom);
    }
    m_probe->position->setCoords(anchor);
    m_probe->setPositionAlignment(align);
    m_probe->setText(probeText(idx));
    m_probe->setVisible(true);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

bool ChartPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_plot && event->type() == QEvent::Leave)
        hideProbe();
    return QWidget::eventFilter(watched, event);
}

void ChartPanel::onMouseDoubleClick(QMouseEvent*)
{
    if (!m_hasData) return;
    resetRange();
    m_plot->replot();
}

void ChartPanel::hideProbe()
{
    if (!m_probe) return;
    if (!m_probe->visible() && (m_crossV.isEmpty() || !m_crossV.first()->visible())) return;
    m_probe->setVisible(false);
    for (QCPItemStraightLine* line : m_crossV) line->setVisible(false);
    if (m_crossH) m_crossH->setVisible(false);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

// ---------------------------------------------------------------- 对外接口
void ChartPanel::showKline(const QString& code, const QString& name,
                           const KlineSeries& series,
                           const QVector<LimitEntry>& limitMarks)
{
    m_code = code;
    m_name = name;
    m_series = series;
    m_marks = limitMarks;
    rebuild();
}

void ChartPanel::setPeriod(Period p)
{
    if (m_period == p) return;
    m_period = p;
    if (m_periodBox) {
        QSignalBlocker block(m_periodBox);
        m_periodBox->setCurrentIndex(int(p));
    }
    rebuild();
}

void ChartPanel::setIndicator(int index)
{
    if (!m_indBox) return;
    const int idx = qBound(0, index, m_indBox->count() - 1);
    if (m_indBox->currentIndex() == idx) return;
    {
        QSignalBlocker block(m_indBox);
        m_indBox->setCurrentIndex(idx);
    }
    rebuild();
}

int ChartPanel::indicator() const
{
    return m_indBox ? m_indBox->currentIndex() : 0;
}

void ChartPanel::clear()
{
    m_crossV.clear();
    m_crossH = nullptr;
    m_probe = nullptr;
    m_maNote = nullptr;
    m_plot->clearPlottables();
    m_plot->clearItems();
    m_hasData = false;
    m_bars.clear();
    m_series = KlineSeries();
    m_marks.clear();
    m_code.clear();
    m_name.clear();
    m_title->setText(QStringLiteral("从「荐股计划」选择标的后自动载入 K 线"));
    showEmptyHint(m_plot, QStringLiteral("暂无 K 线 —— 在「荐股计划」中选择一只标的后自动载入"));
    ensureOverlay();
}
