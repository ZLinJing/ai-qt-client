#pragma once

#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTableWidgetItem>
#include <QVariant>
#include <QString>

// ============================================================================
// PlanCellDelegate：荐股表的「显示文本 / 排序键」分离机制
//
// QTableWidget 的内置排序比较的是单元格的 **DisplayRole**。若 DisplayRole
// 直接放格式化字符串（"39.3" / "9.5"），点表头排序会走字典序 —— "9.5" 会被
// 排到 "39.3" 后面，评分列直接排错，用户看到的顺序是假的。
//
// 因此把两件事拆开：
//   · Qt::DisplayRole → 排序键（数值列放 double，文本列放 QString）
//   · kPlanTextRole   → 真正显示的文本（"6.0%" / "19.34 ~ 19.94" / "A"）
// 由委托把 kPlanTextRole 画进单元格，于是「排序按数值、显示按格式」两不误。
//
// ★ 注意：排序键与显示文本分离后，QTableWidgetItem::text() 返回的是**排序键**
//   的字符串形式（数值列如 "39.3"，而区间列会是中值 "19.64"）。需要「界面上
//   真正显示的那串字」的地方请用 planCellText(item)，不要直接用 item->text()。
// ============================================================================

// 显示文本角色（紧邻 UserRole，避免与业务自定义角色冲突）
inline constexpr int kPlanTextRole = Qt::UserRole + 1;

class PlanCellDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        const QVariant t = index.data(kPlanTextRole);
        if (t.isValid()) option->text = t.toString();
    }
};

// 取单元格里「界面上真正显示的那串字」（排序键 / 显示文本分离后的安全入口）
inline QString planCellText(const QTableWidgetItem* item)
{
    if (!item) return QString();
    const QVariant t = item->data(kPlanTextRole);
    if (t.isValid()) return t.toString();
    return item->text();
}
