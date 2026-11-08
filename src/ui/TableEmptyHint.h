#pragma once

#include <QObject>
#include <QString>

class QTableWidget;
class QLabel;

// ============================================================================
// TableEmptyHint：表格空态提示
//  · 作为 viewport 子控件居中显示说明文字，随表格尺寸自适应
//  · 不拦截鼠标事件，不影响排序/选择等既有交互
// ============================================================================

class TableEmptyHint : public QObject
{
    Q_OBJECT
public:
    TableEmptyHint(QTableWidget* table, const QString& text, QObject* parent = nullptr);

    void setText(const QString& text);
    void setVisible(bool visible);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QTableWidget* m_table = nullptr;
    QLabel* m_label = nullptr;
};
