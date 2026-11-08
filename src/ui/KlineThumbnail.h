#pragma once

#include "core/Types.h"
#include <QWidget>
#include <QVector>

// KlineThumbnail：详情弹窗里的轻量 K 线缩略图（自绘，不引入 QCustomPlot 的重量级依赖）
//  · 最近 60 根日 K（不复权口径，与买卖价格区间一致）
//  · MA5 / MA20 / MA60 三条均线
//  · 买入区间浅色底带 + 当前价虚线，直观呈现「现价与买入区间的关系」
//  · 下方成交量柱，看量能变化趋势
class KlineThumbnail : public QWidget
{
public:
    explicit KlineThumbnail(QWidget* parent = nullptr);

    void setData(const KlineSeries& series, double buyLow, double buyHigh,
                 double currentPrice);

    QSize minimumSizeHint() const override { return QSize(240, 150); }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QVector<Candle> m_bars;
    QVector<double> m_ma5, m_ma20, m_ma60;
    double m_buyLow = 0.0;
    double m_buyHigh = 0.0;
    double m_price = 0.0;
};
