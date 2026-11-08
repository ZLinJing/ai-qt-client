#pragma once

#include <QVector>
#include <QStringList>
#include <QList>
#include <QSharedPointer>
#include "qcustomplot.h"

// ============================================================================
// QCustomPlot 2.x 图表适配层
//
// QCustomPlot 2.x 相对 1.x 有几处关键 API 变化，本模块统一封装，业务代码
// 不必关心差异：
//   1) 轴：统一为 QCPAxis（1.x 的 QCPPriceAxis 已移除）；
//      安装通过 QCPAxisRect::addAxis(QCPAxis::AxisType, QCPAxis*)，
//      plot->addAxis() 与 plot->xAxis/yAxis 快捷成员均已移除。
//   2) 刻度：没有 setTickLabels(QStringList)/setTicks(QList<double>)；
//      需继承 QCPAxisTicker 并重写 generate() 来产出自定义标签。
//   3) 柱状图：没有 QCPBarSet，统一用 QCPBars（构造需指定 key/value 轴）+ QCPBarsGroup。
//   4) 图例背景：legend->setBackground() 已移除，改用 setBrush()/setBorderPen()。
// ============================================================================

namespace QcpAdapter {

// ---- 轴 ----

struct AxisSet {
    QCPAxis* bottom = nullptr;
    QCPAxis* left = nullptr;
    QCPAxis* top = nullptr;
    QCPAxis* right = nullptr;
};

AxisSet installAxes(QCPAxisRect* rect, bool withRight, bool withTop);

// 统一视觉规范（细边框、点状网格、暗色刻度文字）
void styleAxis(QCPAxis* axis, bool gridOn = true);

// 用自定义文本标签覆盖坐标轴刻度
// 2.x 中需自定义 QCPAxisTicker 子类重写 generate()
void setCategoryLabels(QCPAxis* axis, const QList<double>& positions,
                       const QList<QString>& labels);

} // namespace QcpAdapter
