#pragma once                                     // 防止头文件被重复包含

#include <QWidget>
#include <QString>
#include <QFrame>
#include <QLabel>
#include <QColor>
#include <QVector>

class QVBoxLayout;
class QToolButton;
class QTableWidget;
class QPushButton;
class QAction;
class QTimer;

// Components：通用界面组件（Badge / CollapsibleSection / EmptyState / KpiTile / SectionHeader）
//
// 收敛前的现状：徽章 4 种手写实现、折叠区 4 处各写各的、空态 5 套写法、统计指标格逐面板手搓。
// 同一语义在不同页面长得不一样，用户要重新学一遍。这里把这几类「重复出现的界面零件」
// 固化成组件，全局只保留一套长相。
//
// 约定：
//   · 组件一律以 QSS 的 objectName 取样式，颜色与字号只从 Theme 令牌取，界面代码不再出现硬编码色值。
//   · 组件只做「呈现」，不含业务逻辑；需要交互的（折叠、按钮）用信号把意图抛出去。

namespace Comp {

// ---------------- Badge：状态徽章 ----------------
// 语义一律由**文字**承载，颜色只做辅助 —— 形状差异保证不靠颜色单独传达信息，
// 也让色觉障碍用户能分辨（详见 Theme.h 的颜色语义说明）。
enum class BadgeTone {
    Neutral,   // 中性：占位 / 无结论
    Info,      // 提示：数据状态、备���
    Success,   // 通过：可执行、已触发
    Warning,   // 警示：观察、需降仓
    Danger,    // 危险：禁止出手、淘汰
    Accent,    // 强调：当前选中项
};

QColor toneColor(BadgeTone tone);
QColor toneBackground(BadgeTone tone);

// 构造一个徽章标签（QLabel）。tone 决定配色，text 是文案。
QLabel* badge(const QString& text, BadgeTone tone = BadgeTone::Neutral);

// 给已有标签套上徽章样式（不新建控件）
void applyBadge(QLabel* label, BadgeTone tone);

// ---------------- SectionHeader：区块标题 ----------------
// 标题 + 右侧动作位（把「本区块能做什么」放在标题旁边，而不是散落在别处）。
// 返回的容器是 QFrame#Card 风格的横条；调用方把内容 add 到 actions 布局里。
QFrame* sectionHeader(const QString& title, QWidget** outActionsHost = nullptr);

// ---------------- CollapsibleSection：可折叠区块 ----------------
// 统一了原先 4 处手写的折叠：文案格式（▸ / ▾）、计数徽章、展开态记忆。
// 计数为负数表示「没有可折叠的内容」，此时整个区块不显示（不留空卡片）。
class CollapsibleSection : public QFrame
{
    Q_OBJECT
public:
    explicit CollapsibleSection(const QString& title, QWidget* parent = nullptr);

    // 标题右侧的计数徽章（如「0/6」「45 只」）；负数表示不显示徽章
    void setCount(int count, const QString& suffix = QString());

    // 折叠体：调用方把内容加进来
    QWidget* body() const;

    bool isExpanded() const;
    void setExpanded(bool expanded);

    // 标题文案变化时同步（如「今日提醒 · 2/6 已触发」）
    void setTitle(const QString& title);

signals:
    void expandedChanged(bool expanded);

private:
    void refresh();

    QToolButton*  m_toggle = nullptr;
    QLabel*       m_count = nullptr;
    QWidget*      m_body = nullptr;
    QVBoxLayout*  m_bodyLay = nullptr;
    QString       m_title;
    int           m_countValue = -1;
    QString       m_countSuffix;
    bool          m_expanded = false;
};

// ---------------- EmptyState：空态 ----------------
// 统一「结论 + 下一步动作 + 可选按钮」三段式。空状态的价值在于告诉用户下一步做什么，
// 而不是只说「暂无数据」。
QWidget* emptyState(const QString& title, const QString& hint = QString(),
                     const QString& actionText = QString());

// 表格专用空态：把提示压在表格视口中央（沿用 TableEmptyHint 的做法，
// 但文案与样式走统一组件，避免两套空态长得不一样）。
void showTableEmptyHint(QTableWidget* table, const QString& title,
                        const QString& hint = QString());

// ---------------- KpiTile：关键指标块 ----------------
// 统计指标分两级：**主 KPI**（22px 数字，全界面仅此一处大号）与**次指标**（15px）。
// 原先 10 个同等大小的数字并排 —— 等于没有重点。
class KpiTile : public QFrame
{
    Q_OBJECT
public:
    enum class Level { Primary, Secondary };

    explicit KpiTile(const QString& label, Level level = Level::Secondary,
                     QWidget* parent = nullptr);

    void setValue(const QString& text);
    void setValueColor(const QColor& c);
    void setHint(const QString& hint);

private:
    QLabel* m_value = nullptr;
    QLabel* m_hint  = nullptr;
};

// ---------------- ColumnPicker：表格列可见性 ----------------
// 数据表越宽越难读，但每列都有用（价格区间、盈亏比、止损幅度…）。
// 与其让用户横向滚动找列，不如给一个「列」菜单逐列勾选 —— 选择结果跨会话保留。
class ColumnPicker : public QObject
{
    Q_OBJECT
public:
    ColumnPicker(QTableWidget* table, QPushButton* button,
                 const QVector<bool>& defaults, QObject* parent = nullptr);

    // 恢复上次保存的可见列；返回 false 表示没有存档（用默认值）
    bool restore(const QString& encoded);
    QString encode() const;          // 逗号分隔的可见列索引

signals:
    void changed(const QString& encoded);

private:
    void refreshActions();

    QTableWidget*  m_table = nullptr;
    QPushButton*   m_button = nullptr;
    QVector<bool>  m_visible;
};

// ---------------- FeedbackCenter：全局提示中心 ----------------
// 4 类反馈固定在结论条下方：**info / progress / warn / error**。
// 错误态保留到被解决为止，其余自动淡出；点一下能看到最近的消息列表，
// 不再出现「提示被状态栏的新消息冲掉、用户根本没看见」的情况。
class FeedbackCenter : public QFrame
{
    Q_OBJECT
public:
    enum class Kind { Info, Progress, Warn, Error };

    explicit FeedbackCenter(QWidget* parent = nullptr);

    void push(Kind kind, const QString& text);
    void clearAll();

signals:
    void activated();

private:
    struct Entry { Kind kind; QString text; };

    void refresh();

    QLabel* m_icon = nullptr;
    QLabel* m_text = nullptr;
    QVector<Entry> m_entries;
    Kind m_current = Kind::Info;
    bool m_currentError = false;
    QTimer* m_expire = nullptr;      // 进度类消息自动淡出
};

} // namespace Comp

