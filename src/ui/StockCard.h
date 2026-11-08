#pragma once

#include "core/TradePlan.h"
#include <QFrame>
#include <QString>
#include <QColor>

class QLabel;
class QPushButton;
class QGridLayout;
class QMouseEvent;
class QResizeEvent;

// ============================================================================
// StockCard：AI 选股结果的一张白色卡片（一只标的 = 一张卡片）
//
//  · 上半部：名称 / 代码 / 状态徽章 + 当前价格 + 买入 / 卖出 / 止损价格区间
//  · 底部：黑色实心「查看详情」+ 白底黑框「看K线」，两个动作宽度均分
//  · 卡片本身只负责呈现与发出意图，不持有业务状态、不直接取数
//  · 选中态用动态属性 selected 驱动 QSS（见 Theme::styleSheet），布局不随选中抖动
// ============================================================================
class StockCard : public QFrame
{
    Q_OBJECT
public:
    explicit StockCard(const TradePlan& plan, QWidget* parent = nullptr);

    QString code() const { return m_code; }
    QString name() const { return m_name; }

    void setSelected(bool selected);

Q_SIGNALS:
    void cardClicked(const QString& code);                       // 点击卡片空白处 → 选中该标的
    void detailRequested(const QString& code);                   // 查看详情
    void klineRequested(const QString& code, const QString& name); // 看 K 线

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    QFrame* divider();
    void addMetric(QGridLayout* grid, int row, const QString& label,
                   const QString& value, const QColor& color);
    void elideReason();

    QString m_code;
    QString m_name;
    QString m_reasonFull;
    bool m_selected = false;

    QLabel* m_reason = nullptr;
    QPushButton* m_btnDetail = nullptr;
    QPushButton* m_btnKline = nullptr;
};
