#pragma once

#include "core/Types.h"
#include <QWidget>
#include <QVector>

class QLabel;
class QComboBox;
class QCheckBox;
class QPushButton;
class QMouseEvent;
class QVBoxLayout;
class QCustomPlot;
class QCPAxis;
class QCPAxisRect;
class QCPRange;
class QCPItemStraightLine;
class QCPItemText;

// ============================================================================
// ChartPanel：K 线分析（唯一的图表视图）
//
//  v2 起移除「因子对比」「情绪周期」两个视图 —— 它们与 K 线不是同一类决策依据，
//  混在同一页里既稀释了 K 线的操作空间，也带来两份难以维护的绘图代码。
//  现在整页只做一件事：把一只票的 K 线看清楚。
//
//  能力：
//   · 周期切换：日 K / 周 K / 月 K（由日线聚合，开=首开、收=末收、高/低取极值、量额求和）
//   · 指标叠加：MA5/10/20/60、BOLL(20,2)，副图 MACD / KDJ / RSI 三选一
//   · 成交量副图：红涨绿跌 + 5 周期均量线
//   · 交互：滚轮缩放、拖动平移、双击复位、十字光标逐根读数（含指标实测值）
//   · 涨停标记：三角 + 板数，涨停价虚线参考位
//
//  实现要点（QCustomPlot 2.x）：
//    · 三个轴矩形（价格 / 量能 / 指标）共用同一组 X 轴范围，任一变化即同步；
//    · 缩放后按「可见区间」自动重算 Y 轴，避免放大后 K 线跑出画面；
//    · clearItems() 会 delete 十字光标对象，重建前必须先把指针置空。
// ============================================================================

class ChartPanel : public QWidget
{
    Q_OBJECT
public:
    enum class Period { Day = 0, Week = 1, Month = 2 };

    explicit ChartPanel(QWidget* parent = nullptr);

    // 展示个股 K 线（叠加涨停点标记）
    void showKline(const QString& code, const QString& name,
                   const KlineSeries& series,
                   const QVector<LimitEntry>& limitMarks);
    void clear();
    bool hasData() const { return m_hasData; }   // 是否已展示过一根 K（详情页签按需请求数据）

    // 供冒烟测试 / 外部切换到指定周期与副图指标
    void setPeriod(Period p);
    Period period() const { return m_period; }
    void setIndicator(int index);      // 0 = MACD, 1 = KDJ, 2 = RSI
    int indicator() const;

    // 上下文行动条：挂到顶栏右上角，由主窗口在切页时挂载 / 摘下
    QWidget* pageActions() const { return m_pageActions; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 聚合后的一根 K（周期 > 日线时 from != to）
    struct Bar {
        Candle c;
        QDate from;
        QDate to;
        int days = 1;
    };

    void buildToolbar(QVBoxLayout* root);
    QWidget* buildPageActions();      // 上下文行动条（顶栏右上角）
    void setupPlot();
    void ensureOverlay();

    void rebuild();          // 数据 / 周期 / 指标开关变化 → 整体重绘
    void aggregate();        // 日线 → 日/周/月
    void computeIndicators();
    void plotPrice();
    void plotVolume();
    void plotIndicator();

    void resetRange();                       // 复位：全量可见
    void applyRange(const QCPRange& xr);     // 三轴同步 + 按可见区间重算 Y
    void fitY(const QCPRange& xr);
    void updateDateTicks(const QCPRange& xr);
    void syncX(QCPAxis* source);

    void hideProbe();
    void onMouseMove(QMouseEvent* e);
    void onMouseDoubleClick(QMouseEvent* e);
    QString probeText(int idx) const;

    QCustomPlot* m_plot = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_hint = nullptr;
    QComboBox* m_periodBox = nullptr;
    QComboBox* m_indBox = nullptr;
    QCheckBox* m_chkMa = nullptr;
    QCheckBox* m_chkVolMa = nullptr;
    QCheckBox* m_chkBoll = nullptr;
    QPushButton* m_btnReset = nullptr;
    QWidget* m_pageActions = nullptr; // 上下文行动条（主窗口挂到顶栏右上角）

    QCPAxisRect* m_priceRect = nullptr;
    QCPAxisRect* m_volRect = nullptr;
    QCPAxisRect* m_indRect = nullptr;

    QCPAxis* m_px = nullptr;   // 价格区
    QCPAxis* m_py = nullptr;
    QCPAxis* m_vx = nullptr;   // 量能区
    QCPAxis* m_vy = nullptr;
    QCPAxis* m_ix = nullptr;   // 指标区
    QCPAxis* m_iy = nullptr;

    QVector<QCPItemStraightLine*> m_crossV;   // 竖线：每个轴矩形一条
    QCPItemStraightLine* m_crossH = nullptr;  // 横线：只在价格区
    QCPItemText* m_probe = nullptr;
    QCPItemText* m_maNote = nullptr;

    // ---- 数据 ----
    KlineSeries m_series;
    QVector<Bar> m_bars;
    QVector<LimitEntry> m_marks;
    QString m_code;
    QString m_name;
    Period m_period = Period::Day;
    bool m_hasData = false;
    bool m_syncing = false;

    // ---- 指标序列（与 m_bars 等长）----
    QVector<double> m_ma5, m_ma10, m_ma20, m_ma60;
    QVector<double> m_bollUp, m_bollMid, m_bollDn;
    QVector<double> m_dif, m_dea, m_macd;
    QVector<double> m_k, m_d, m_j;
    QVector<double> m_rsi6, m_rsi12, m_rsi24;
    QVector<double> m_volMa5;
    double m_volMax = 0.0;   // 万手
};
