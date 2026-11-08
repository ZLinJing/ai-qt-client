#include "ui/JournalPanel.h"
#include "ui/Components.h"
#include "ui/Theme.h"
#include "ui/TableEmptyHint.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTextStream>
#include <QVBoxLayout>

namespace {

QString money(double v) { return QString::number(v, 'f', 2); }
QString pctText(double v) { return QString::number(v, 'f', 2) + QLatin1Char('%'); }

} // namespace

JournalPanel::JournalPanel(QWidget* parent) : QWidget(parent)
{
    buildUi();
}

void JournalPanel::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(Theme::Space::Md);

    // ---------------- 统计卡 ----------------
    // 标题只留「复盘统计」四个字，费用口径降级为一行小字 + tooltip；
    // 存储后端做成徽章放在标题右侧，不再单独占一行。
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("Card"));
    auto* cl = new QVBoxLayout(card);
    cl->setContentsMargins(Theme::Space::Lg, Theme::Space::Md,
                           Theme::Space::Lg, Theme::Space::Md);
    cl->setSpacing(Theme::Space::Sm);

    auto* head = new QWidget(card);
    auto* headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(Theme::Space::Sm);
    auto* title = new QLabel(QStringLiteral("复盘统计"), head);
    title->setObjectName(QStringLiteral("CardTitle"));
    headLay->addWidget(title);
    headLay->addStretch(1);
    m_lbStore = new QLabel(QStringLiteral("存储：未连接"), head);
    m_lbStore->setObjectName(QStringLiteral("JournalChip"));
    m_lbStore->setToolTip(QStringLiteral("交易流水的当前存储后端（MySQL 优先，连接失败时回退本地 SQLite）"));
    headLay->addWidget(m_lbStore, 0, Qt::AlignRight);
    cl->addWidget(head);

    auto* basis = new QLabel(
        QStringLiteral("口径：已了结按卖出价、持仓中按现价；已扣佣金万2.5双边 + 卖出印花税千1（不含过户费）"), card);
    basis->setObjectName(QStringLiteral("Muted"));
    basis->setWordWrap(true);
    basis->setToolTip(QStringLiteral(
        "已了结 = 卖出价 × 数量 − 成本 − 双边费用；持仓中 = 现价 × 数量 − 成本 − 双边费用。\n"
        "现价缺失时不做估算，明确显示「无行情」。"));
    cl->addWidget(basis);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(Theme::Space::Xl);
    grid->setVerticalSpacing(Theme::Space::Sm);

    auto mk = [&](int row, int col, const QString& name, Comp::KpiTile*& out,
                  Comp::KpiTile::Level level) {
        out = new Comp::KpiTile(name, level, card);
        out->setMinimumWidth(120);   // 防止指标标签 / 提示文字被挤到换行
        grid->addWidget(out, row, col);
    };
    // 主 KPI：一眼要能看到的四个结论
    mk(0, 0, QStringLiteral("胜率"), m_kpiWinRate, Comp::KpiTile::Level::Primary);
    mk(0, 1, QStringLiteral("累计盈亏（已实现）"), m_kpiPnl, Comp::KpiTile::Level::Primary);
    mk(0, 2, QStringLiteral("持仓浮动盈亏"), m_kpiFloat, Comp::KpiTile::Level::Primary);
    mk(0, 3, QStringLiteral("最大回撤"), m_kpiDrawdown, Comp::KpiTile::Level::Primary);
    // 次指标：支撑结论的明细，数字小一号
    mk(1, 0, QStringLiteral("记录笔数"), m_kpiTotal, Comp::KpiTile::Level::Secondary);
    mk(1, 1, QStringLiteral("每笔期望"), m_kpiExpect, Comp::KpiTile::Level::Secondary);
    mk(1, 2, QStringLiteral("平均持仓"), m_kpiHold, Comp::KpiTile::Level::Secondary);
    mk(1, 3, QStringLiteral("按计划执行"), m_kpiFollow, Comp::KpiTile::Level::Secondary);
    mk(1, 4, QStringLiteral("止损命中率"), m_kpiStopHit, Comp::KpiTile::Level::Secondary);
    mk(1, 5, QStringLiteral("现价行情"), m_kpiQuotes, Comp::KpiTile::Level::Secondary);
    // 6 列等宽：主 KPI 与次指标共用同一套栅格，不再出现列宽忽宽忽窄
    for (int c = 0; c < 6; ++c) grid->setColumnStretch(c, 1);
    cl->addLayout(grid);
    root->addWidget(card);

    // ---------------- 持仓下一步动作 ----------------
    // 标题右侧给出条数；高度上限放宽到 140px 并可滚动，持仓多时不再被裁掉。
    auto* actionCard = new QFrame(this);
    actionCard->setObjectName(QStringLiteral("Card"));
    auto* actionLay = new QVBoxLayout(actionCard);
    actionLay->setContentsMargins(Theme::Space::Lg, Theme::Space::Sm,
                                  Theme::Space::Lg, Theme::Space::Sm);
    actionLay->setSpacing(Theme::Space::Xs);

    auto* actionHead = new QWidget(actionCard);
    auto* actionHeadLay = new QHBoxLayout(actionHead);
    actionHeadLay->setContentsMargins(0, 0, 0, 0);
    actionHeadLay->setSpacing(Theme::Space::Sm);
    auto* actionTitle = new QLabel(QStringLiteral("持仓 · 下一步动作"), actionHead);
    actionTitle->setObjectName(QStringLiteral("CardTitle"));
    actionHeadLay->addWidget(actionTitle);
    actionHeadLay->addStretch(1);
    m_actionCount = new QLabel(actionHead);
    m_actionCount->setObjectName(QStringLiteral("Muted"));
    actionHeadLay->addWidget(m_actionCount, 0, Qt::AlignRight);
    actionLay->addWidget(actionHead);

    m_actions = new QTextBrowser(actionCard);
    m_actions->setObjectName(QStringLiteral("ActionList"));
    m_actions->setOpenExternalLinks(false);
    m_actions->setAccessibleName(QStringLiteral("持仓下一步动作清单"));
    m_actions->setMinimumHeight(40);
    m_actions->setMaximumHeight(140);
    m_actions->setToolTip(QStringLiteral("与表格「下一步动作」列使用同一套判定；内容多时可滚动"));
    actionLay->addWidget(m_actions);
    root->addWidget(actionCard);

    // ---------------- 工具栏（页内）----------------
    // 行级操作、视图切换、搜索与结果计数都在一处；按钮随选中态启用 / 禁用。
    auto* bar = new QWidget(this);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(Theme::Space::Sm);

    m_btnEdit = new QPushButton(QStringLiteral("编辑"), bar);
    m_btnEdit->setObjectName(QStringLiteral("Ghost"));
    m_btnEdit->setAccessibleName(QStringLiteral("编辑选中的交易记录"));
    m_btnEdit->setToolTip(QStringLiteral("补录卖出价 / 卖出日 / 备注（Ctrl+E，或双击表格行）"));
    m_btnEdit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_btnDel = new QPushButton(QStringLiteral("删除"), bar);
    m_btnDel->setObjectName(QStringLiteral("Ghost"));
    m_btnDel->setAccessibleName(QStringLiteral("删除选中的交易记录"));
    m_btnDel->setToolTip(QStringLiteral("删除选中的交易记录（Delete）"));
    m_btnDel->setShortcut(QKeySequence(Qt::Key_Delete));
    h->addWidget(m_btnEdit);
    h->addWidget(m_btnDel);
    h->addSpacing(Theme::Space::Sm);

    // 视图分段开关：用 QButtonGroup 管互斥，键盘可切换且状态唯一
    auto* viewGroup = new QWidget(bar);
    viewGroup->setObjectName(QStringLiteral("ViewModeGroup"));
    auto* viewLay = new QHBoxLayout(viewGroup);
    viewLay->setContentsMargins(0, 0, 0, 0);
    viewLay->setSpacing(0);
    m_viewButtons = new QButtonGroup(this);
    m_viewButtons->setExclusive(true);
    auto mkView = [&](const QString& text, int mode, const QString& tip) {
        auto* b = new QPushButton(text, viewGroup);
        b->setObjectName(QStringLiteral("ViewModeSegment"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumHeight(28);
        b->setAccessibleName(text);
        b->setToolTip(tip);
        m_viewButtons->addButton(b, mode);
        connect(b, &QPushButton::clicked, this, [this, mode]() {
            if (m_viewMode == mode) return;
            m_viewMode = mode;
            applyFilter();
        });
        viewLay->addWidget(b);
        return b;
    };
    m_viewOpen = mkView(QStringLiteral("持仓中"), 0,
                        QStringLiteral("只看尚未卖出的持仓，盈亏按现价浮动计算"));
    m_viewAll = mkView(QStringLiteral("全部流水"), 1,
                       QStringLiteral("查看全部记录（含已了结）"));
    m_viewClosed = mkView(QStringLiteral("已了结"), 2,
                          QStringLiteral("只看已补录卖出信息的记录，盈亏为已实现"));
    m_viewAll->setChecked(true);
    h->addWidget(viewGroup);

    m_search = new QLineEdit(bar);
    m_search->setPlaceholderText(QStringLiteral("搜索 代码 / 名称 / 备注"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("搜索交易记录"));
    m_search->setToolTip(QStringLiteral("按代码、名称或备注模糊过滤（Ctrl+F 聚焦）"));
    m_search->setMinimumWidth(120);
    m_search->setMaximumWidth(200);
    auto* focusSearch = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(focusSearch, &QShortcut::activated, this, [this]() { m_search->setFocus(); });
    h->addWidget(m_search);

    m_countLabel = new QLabel(bar);
    m_countLabel->setObjectName(QStringLiteral("Muted"));
    m_countLabel->setToolTip(QStringLiteral("当前筛选结果 / 全部记录"));
    h->addWidget(m_countLabel);

    h->addStretch(1);
    auto* legend = new QLabel(QStringLiteral("红 涨/盈 · 绿 跌/亏 · 黄底 持仓中"), bar);
    legend->setObjectName(QStringLiteral("Muted"));
    legend->setToolTip(QStringLiteral("表格配色说明：状态同时用文字与底色表达，不单靠颜色"));
    // 短文案 + 不设下限：既不撑大窗口最小宽度，也不会被弹性空间压成 0 宽
    legend->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    legend->setMinimumWidth(0);
    h->addWidget(legend, 0, Qt::AlignRight);

    m_btnToggleDetail = new QPushButton(QStringLiteral("收起详情"), bar);
    m_btnToggleDetail->setObjectName(QStringLiteral("Ghost"));
    m_btnToggleDetail->setAccessibleName(QStringLiteral("折叠或展开交易详情栏"));
    m_btnToggleDetail->setToolTip(QStringLiteral("折叠详情栏可把纵向空间全部让给表格"));
    h->addWidget(m_btnToggleDetail, 0, Qt::AlignRight);
    root->addWidget(bar);

    connect(m_btnEdit, &QPushButton::clicked, this, &JournalPanel::onEdit);
    connect(m_btnDel, &QPushButton::clicked, this, &JournalPanel::onDelete);
    connect(m_btnToggleDetail, &QPushButton::clicked, this, &JournalPanel::toggleDetail);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { applyFilter(); });

    // ---------------- 明细表 + 可折叠详情 ----------------
    // 表格与详情用垂直 QSplitter：详情不再固定 320px 霸占首屏，用户可拖动或一键收起。
    m_bodySplit = new QSplitter(Qt::Vertical, this);
    m_bodySplit->setObjectName(QStringLiteral("JournalBodySplit"));
    m_bodySplit->setChildrenCollapsible(false);
    m_bodySplit->setHandleWidth(6);

    m_table = new QTableWidget(0, 13, m_bodySplit);
    m_table->setObjectName(QStringLiteral("JournalTable"));
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("买入日"), QStringLiteral("代码"), QStringLiteral("名称"),
        QStringLiteral("状态"), QStringLiteral("买入价"), QStringLiteral("现价"),
        QStringLiteral("数量"), QStringLiteral("卖出日"), QStringLiteral("卖出价"),
        QStringLiteral("盈亏(元)"), QStringLiteral("盈亏%"), QStringLiteral("持仓(天)"),
        QStringLiteral("下一步动作")
    });
    const QStringList headTips = {
        QStringLiteral("买入日期；点表头可排序，行标识跟随记录 id，排序后不串票"),
        QStringLiteral("6 位股票代码"),
        QStringLiteral("股票名称"),
        QStringLiteral("持仓中（黄底）/ 已了结（灰底）；状态由文字与底色共同表达"),
        QStringLiteral("实际买入价（元）"),
        QStringLiteral("最新快照价；停牌或行情未就绪时显示「无行情」，不拿昨收冒充"),
        QStringLiteral("买入股数"),
        QStringLiteral("卖出日期；未卖出显示 -"),
        QStringLiteral("卖出价（元）；未卖出显示 -"),
        QStringLiteral("盈亏金额：已了结按卖出价、持仓中按现价，均已扣双边费用"),
        QStringLiteral("盈亏百分比，与盈亏金额同源"),
        QStringLiteral("持有天数（已了结按卖出日，持仓中按今天）"),
        QStringLiteral("持仓期唯一要执行的动作（止损离场 / 分批兑现 / T+1 可兑现 / 持有待涨 / 持有观察 / 待行情）；已了结显示「已了结」")
    };
    for (int c = 0; c < 13; ++c) {
        if (auto* it = m_table->horizontalHeaderItem(c)) it->setToolTip(headTips.at(c));
    }
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSortingEnabled(true);
    m_table->setAccessibleName(QStringLiteral("交易记录明细表"));
    m_table->setToolTip(QStringLiteral("双击任意行可编辑该笔记录"));
    Theme::polishTable(m_table);
    // 「下一步动作」吸收剩余宽度，其余列固定，窄屏也不会横向溢出
    const int w[13] = { 86, 66, 86, 72, 74, 74, 70, 86, 74, 86, 68, 68, 110 };
    for (int c = 0; c < 12; ++c) m_table->setColumnWidth(c, w[c]);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(12, QHeaderView::Stretch);
    m_bodySplit->addWidget(m_table);

    // 详情栏做成白色卡片，与统计卡 / 动作卡同一套视觉语言
    m_detailPane = new QFrame(m_bodySplit);
    m_detailPane->setObjectName(QStringLiteral("Card"));
    auto* dl = new QVBoxLayout(m_detailPane);
    dl->setContentsMargins(Theme::Space::Lg, Theme::Space::Sm,
                           Theme::Space::Lg, Theme::Space::Sm);
    dl->setSpacing(Theme::Space::Xs);
    auto* detailHead = new QLabel(QStringLiteral("选中记录 · 完整摘要"), m_detailPane);
    detailHead->setObjectName(QStringLiteral("CardTitle"));
    dl->addWidget(detailHead);
    m_detail = new QTextBrowser(m_detailPane);
    m_detail->setObjectName(QStringLiteral("JournalDetail"));
    m_detail->setOpenExternalLinks(false);
    m_detail->setAccessibleName(QStringLiteral("选中交易的完整记录与计划执行情况"));
    m_detail->setMinimumHeight(80);
    dl->addWidget(m_detail, 1);
    m_bodySplit->addWidget(m_detailPane);
    m_bodySplit->setStretchFactor(0, 1);
    m_bodySplit->setStretchFactor(1, 0);
    m_bodySplit->setSizes({ 460, 120 });
    root->addWidget(m_bodySplit, 1);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        showDetail();
        updateSelectionActions();
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int) { onEdit(); });
    m_emptyHint = new TableEmptyHint(m_table, QString(), this);
    updateEmptyHint();
    updateActionList();
    updateSelectionActions();

    // 上下文行动条不进页面布局：由主窗口 reparent 到顶栏右上角
    m_pageActions = buildPageActions();
}

void JournalPanel::setStore(JournalStore* store)
{
    m_store = store;
    if (m_lbStore) {
        const bool connected = (m_store != nullptr);
        m_lbStore->setText(connected
            ? QStringLiteral("存储：%1").arg(m_store->describe())
            : QStringLiteral("存储：未连接"));
        m_lbStore->setStyleSheet(Theme::chipStyle(
            connected ? Theme::success() : Theme::neutral(),
            connected ? Theme::successSoft() : Theme::neutralSoft(),
            Theme::fontXs(), 4, 8, true));
    }
    refresh();
}

void JournalPanel::refresh()
{
    m_rows = m_store ? m_store->trades() : QVector<TradeRecord>();
    ++m_contextVersion;          // 记录变化 → 诊断缓存整体失效（编辑后必须重算）
    applyFilter();
    updateStats();
    updateActionList();
    showDetail();
}

void JournalPanel::setSnapshots(const QHash<QString, Snapshot>& snapshots,
                               const QString& stamp)
{
    const bool hadQuotes = !m_snaps.isEmpty();
    m_snaps.clear();
    for (auto it = snapshots.constBegin(); it != snapshots.constEnd(); ++it) {
        // 停牌 / 快照缺价：保留空价条目，rebuildTable 会显示「无行情」而不是拿昨收冒充现价
        if (it->price > 0.0) m_snaps.insert(it.key(), it.value());
    }
    if (!stamp.isEmpty()) m_quoteStamp = stamp;

    // 行情变化影响现价 / 盈亏列与 AI 诊断：走 applyFilter 复用同一套筛选与排序，
    // 保证「刷新行情」后表格行序、筛选态、统计卡与诊断结论四者仍然一致。
    if (hadQuotes || !m_snaps.isEmpty()) {
        applyFilter();
        updateStats();
        updateActionList();
        showDetail();
    }
}

void JournalPanel::setKlines(const QHash<QString, KlineSeries>& klines)
{
    // 行情刷新会频繁调用本方法：K 线没变就不重算诊断（筹码分布是 O(根数×格数)）
    if (m_klines.size() == klines.size()) {
        bool same = true;
        for (auto it = klines.constBegin(); it != klines.constEnd(); ++it) {
            const auto old = m_klines.constFind(it.key());
            if (old == m_klines.constEnd()
                || old->candles.size() != it->candles.size()
                || old->rawCandles.size() != it->rawCandles.size()) {
                same = false;
                break;
            }
            // 同一根数但最后一根被盘中更新（收盘价 / 成交量变化）也要重算
            if (!it->candles.isEmpty() && !old->candles.isEmpty()) {
                const Candle& a = old->candles.back();
                const Candle& b = it->candles.back();
                if (a.date != b.date || !qFuzzyCompare(a.close + 1.0, b.close + 1.0)
                    || !qFuzzyCompare(a.volume + 1.0, b.volume + 1.0)) {
                    same = false;
                    break;
                }
            }
        }
        if (same) return;
    }
    m_klines = klines;
    ++m_contextVersion;          // 均线 / ATR / 筹码压力变化 → 诊断失效
    applyFilter();
    showDetail();
}

void JournalPanel::setMarketPhase(MarketPhase phase, bool known)
{
    if (m_phase == phase && m_phaseKnown == known) return;
    m_phase = phase;
    m_phaseKnown = known;
    ++m_contextVersion;
    applyFilter();
    showDetail();
}

QStringList JournalPanel::codes() const
{
    QStringList out;
    for (const TradeRecord& r : m_rows) {
        if (!r.code.isEmpty() && !out.contains(r.code)) out << r.code;
    }
    return out;
}

double JournalPanel::quoteOf(const QString& code, bool* ok) const
{
    const auto it = m_snaps.constFind(code);
    if (it != m_snaps.constEnd() && it.value().price > 0.0) {
        if (ok) *ok = true;
        return it.value().price;
    }
    if (ok) *ok = false;
    return 0.0;
}

void JournalPanel::applyFilter()
{
    // 视图分段开关：0 持仓中 / 1 全部流水 / 2 已了结
    const QString kw = m_search ? m_search->text().trimmed().toLower() : QString();

    m_view.clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        const TradeRecord& r = m_rows.at(i);
        if (m_viewMode == 0 && r.closed()) continue;
        if (m_viewMode == 2 && !r.closed()) continue;
        if (!kw.isEmpty()
            && !r.code.toLower().contains(kw)
            && !r.name.toLower().contains(kw)
            && !r.note.toLower().contains(kw)) {
            continue;
        }
        m_view.append(i);
    }
    ensureInsights();       // 先算完全部诊断（含被筛掉的记录），总览与明细共用同一份结果
    rebuildTable();
    updateEmptyHint();      // 空态文案区分「无记录」与「筛选无结果」
    updateActionList();     // 动作清单与表格使用同一份筛选结果
    updateSelectionActions();  // 选中行可能被筛掉，行级按钮要同步禁用
}

// 空态与结果计数：筛选后没结果时给出可执行的下一步，而不是笼统的「暂无数据」
void JournalPanel::updateEmptyHint()
{
    if (m_countLabel) {
        m_countLabel->setText(
            QStringLiteral("显示 %1 / %2 笔").arg(m_view.size()).arg(m_rows.size()));
    }
    if (!m_emptyHint) return;
    m_emptyHint->setVisible(m_view.isEmpty());
    if (m_rows.isEmpty()) {
        m_emptyHint->setText(QStringLiteral(
            "还没有交易记录\n在「AI选股结果」点卡片查看标的后，用顶栏「记录买入」记一笔；"
            "成交后在「编辑」里补录卖出信息"));
    } else if (m_view.isEmpty()) {
        const QString mode = m_viewMode == 0 ? QStringLiteral("持仓中")
                           : (m_viewMode == 2 ? QStringLiteral("已了结")
                                              : QStringLiteral("全部流水"));
        const QString kw = m_search ? m_search->text().trimmed() : QString();
        m_emptyHint->setText(QStringLiteral(
            "「%1」%2下没有匹配的记录\n切回「全部流水」或清除搜索关键词即可看全部流水")
            .arg(mode, kw.isEmpty() ? QString() : QStringLiteral(" + 「%1」").arg(kw)));
    }
}

// 未选中记录时禁用行级操作：避免点了以后才弹「请先选中」
void JournalPanel::updateSelectionActions()
{
    const bool has = (selectedRecord() != nullptr);
    if (m_btnEdit) m_btnEdit->setEnabled(has);
    if (m_btnDel) m_btnDel->setEnabled(has);
}

// 详情栏一键折叠：小屏或记录多时把纵向空间全部让给表格
void JournalPanel::toggleDetail()
{
    if (!m_detailPane) return;
    m_detailCollapsed = !m_detailCollapsed;
    m_detailPane->setVisible(!m_detailCollapsed);
    if (m_btnToggleDetail) {
        m_btnToggleDetail->setText(m_detailCollapsed
            ? QStringLiteral("展开详情") : QStringLiteral("收起详情"));
    }
}

// 上下文行动条（顶栏右上角）：本页最常做的三件事
QWidget* JournalPanel::buildPageActions()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("PageActions"));
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(Theme::Space::Sm);

    auto* btnAdd = new QPushButton(QStringLiteral("记录买入"), bar);
    btnAdd->setObjectName(QStringLiteral("Primary"));
    btnAdd->setToolTip(QStringLiteral("把一笔真实成交记入日志（带计划买点 / 止损 / 止盈，用于事后复盘）"));
    h->addWidget(btnAdd);
    connect(btnAdd, &QPushButton::clicked, this, &JournalPanel::onAdd);

    auto* btnExport = new QPushButton(QStringLiteral("导出 CSV"), bar);
    btnExport->setToolTip(QStringLiteral("导出全部流水（含 AI 诊断九列），便于离线复盘"));
    h->addWidget(btnExport);
    connect(btnExport, &QPushButton::clicked, this, &JournalPanel::onExport);

    auto* btnRefresh = new QPushButton(QStringLiteral("刷新行情"), bar);
    btnRefresh->setToolTip(QStringLiteral("重新汇总记录，并拉取一次最新现价（持仓盈亏随行情重算）"));
    h->addWidget(btnRefresh);
    connect(btnRefresh, &QPushButton::clicked, this, [this]() {
        refresh();
        Q_EMIT quoteRefreshRequested();       // 让主窗口拉一次全市场快照
    });

    // 未挂到顶栏前保持隐藏（否则会以 (0,0) 浮在面板上）
    bar->hide();
    return bar;
}

void JournalPanel::rebuildTable()
{
    if (!m_table) return;
    m_table->setSortingEnabled(false);   // 填充期间关闭排序，避免触发重排
    m_table->setRowCount(m_view.size());

    for (int row = 0; row < m_view.size(); ++row) {
        const TradeRecord& r = m_rows.at(m_view.at(row));
        auto set = [&](int col, const QString& text, const QColor& color = QColor(),
                       Qt::Alignment align = Qt::AlignVCenter | Qt::AlignLeft) {
            auto* item = new QTableWidgetItem(text);
            item->setTextAlignment(align);
            if (color.isValid()) item->setForeground(color);
            m_table->setItem(row, col, item);
        };

        // 买入日列：文字显示日期，UserRole 存 id（排序后仍能正确定位记录）
        auto* idItem = new QTableWidgetItem(r.buyDate.toString(QStringLiteral("yyyy-MM-dd")));
        idItem->setData(Qt::UserRole, r.id);
        idItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignCenter);
        idItem->setForeground(Theme::text());
        m_table->setItem(row, 0, idItem);

        set(1, r.code, Theme::text(), Qt::AlignVCenter | Qt::AlignCenter);
        set(2, r.name);
        // 状态用「文字 + 淺底色」共同表达，不靠颜色单独承载语义
        {
            const bool closed = r.closed();
            set(3, closed ? QStringLiteral("已了结") : QStringLiteral("持仓中"),
                closed ? Theme::neutral() : Theme::warning(),
                Qt::AlignVCenter | Qt::AlignCenter);
            if (auto* st = m_table->item(row, 3)) {
                st->setBackground(closed ? Theme::neutralSoft() : Theme::warningSoft());
                st->setToolTip(closed
                    ? QStringLiteral("已补录卖出信息，盈亏为已实现")
                    : QStringLiteral("尚未卖出，盈亏按现价浮动计算"));
            }
        }
        set(4, money(r.buyPrice), Theme::accent(), Qt::AlignVCenter | Qt::AlignRight);

        // ---- 现价：仅持仓中有意义；拿不到行情就明确说「无行情」，不拿昨收/成本冒充 ----
        bool hasQuote = false;
        const double quote = r.closed() ? 0.0 : quoteOf(r.code, &hasQuote);
        if (r.closed()) {
            set(5, QStringLiteral("-"), Theme::flat(), Qt::AlignVCenter | Qt::AlignRight);
        } else if (hasQuote) {
            set(5, money(quote), quote >= r.buyPrice ? Theme::up() : Theme::down(),
                Qt::AlignVCenter | Qt::AlignRight);
        } else {
            set(5, QStringLiteral("无行情"), Theme::flat(), Qt::AlignVCenter | Qt::AlignRight);
            m_table->item(row, 5)->setToolTip(QStringLiteral("尚未拿到该股的实时快照（停牌或行情未就绪）"));
        }

        set(6, QString::number(r.qty), Theme::text(), Qt::AlignVCenter | Qt::AlignRight);
        set(7, r.closed() ? r.sellDate.toString(QStringLiteral("yyyy-MM-dd"))
                          : QStringLiteral("-"),
            r.closed() ? Theme::text() : Theme::flat(), Qt::AlignVCenter | Qt::AlignCenter);
        set(8, r.closed() ? money(r.sellPrice) : QStringLiteral("-"),
            Theme::text(), Qt::AlignVCenter | Qt::AlignRight);

        // ---- 盈亏：已了结按卖出价、持仓中按现价，两者同源（TradeRecord::pnlAt）----
        const bool havePnl = r.closed() || hasQuote;
        const double pnl = r.closed() ? r.pnl() : (hasQuote ? r.pnlAt(quote) : 0.0);
        const double pnlPct = r.closed() ? r.pnlPct() : (hasQuote ? r.pnlPctAt(quote) : 0.0);
        if (havePnl) {
            const QColor c = pnl >= 0 ? Theme::up() : Theme::down();
            // 正负号让盈亏方向不只靠红绿传达（色觉障碍 / 打印场景同样可读）
            set(9, (pnl >= 0 ? QStringLiteral("+") : QString()) + money(pnl),
                c, Qt::AlignVCenter | Qt::AlignRight);
            set(10, (pnlPct >= 0 ? QStringLiteral("+") : QString()) + pctText(pnlPct),
                c, Qt::AlignVCenter | Qt::AlignRight);
            const QString basis = r.closed()
                ? QStringLiteral("已实现：卖出价 %1 × %2 股 − 成本 − 双边费用").arg(money(r.sellPrice)).arg(r.qty)
                : QStringLiteral("浮动：现价 %1 × %2 股 − 成本 − 双边费用（按现价预估卖出）")
                      .arg(money(quote)).arg(r.qty);
            m_table->item(row, 9)->setToolTip(basis);
            m_table->item(row, 10)->setToolTip(basis);
        } else {
            set(9, QStringLiteral("-"), Theme::flat(), Qt::AlignVCenter | Qt::AlignRight);
            set(10, QStringLiteral("-"), Theme::flat(), Qt::AlignVCenter | Qt::AlignRight);
        }
        set(11, QString::number(r.holdingDays(QDate::currentDate())),
            r.closed() ? Theme::text() : Theme::textMuted(), Qt::AlignVCenter | Qt::AlignRight);

        // ---- 下一步动作列：持仓期唯一要执行的动作（与动作清单共用同一判定）----
        const NextAction act = nextActionFor(r);
        set(12, act.label, act.color, Qt::AlignVCenter | Qt::AlignCenter);
        if (QTableWidgetItem* it = m_table->item(row, 12)) {
            QString tip = act.why;
            if (!r.note.isEmpty())
                tip += QStringLiteral("\n备注：%1").arg(r.note);
            if (r.planBuy > 0.0 || r.planStop > 0.0 || r.planTake > 0.0) {
                tip += QStringLiteral("\n计划：买 %1 / 止盈 %2 / 止损 %3")
                    .arg(r.planBuy > 0 ? money(r.planBuy) : QStringLiteral("—"),
                         r.planTake > 0 ? money(r.planTake) : QStringLiteral("—"),
                         r.planStop > 0 ? money(r.planStop) : QStringLiteral("—"));
            }
            it->setToolTip(tip);
        }
    }
    m_table->setSortingEnabled(true);
}

const TradeRecord* JournalPanel::selectedRecord() const
{
    if (!m_table) return nullptr;
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_table->rowCount()) return nullptr;
    QTableWidgetItem* it = m_table->item(row, 0);
    if (!it) return nullptr;
    const int id = it->data(Qt::UserRole).toInt();
    for (const TradeRecord& r : m_rows) if (r.id == id) return &r;
    return nullptr;
}

void JournalPanel::showDetail()
{
    if (!m_detail) return;
    const TradeRecord* r = selectedRecord();
    if (!r) {
        m_detail->setHtml(QStringLiteral("<div style='color:%1;padding:6px 2px;'>"
                                         "选中表格任意一行，这里会显示该笔交易的完整记录、"
                                         "盈亏口径与计划执行情况。</div>")
                              .arg(Theme::textMuted().name()));
        return;
    }

    auto esc = [](const QString& s) { return s.toHtmlEscaped(); };
    auto row = [&](const QString& key, const QString& value) {
        return QStringLiteral("<tr><td style='color:%1;padding:2px 16px 2px 0;'>%2</td>"
                              "<td style='padding:2px 0;'><b>%3</b></td></tr>")
            .arg(Theme::textMuted().name(), esc(key), esc(value));
    };
    auto rowRich = [&](const QString& key, const QString& html) {
        return QStringLiteral("<tr><td style='color:%1;padding:2px 16px 2px 0;'>%2</td>"
                              "<td style='padding:2px 0;'>%3</td></tr>")
            .arg(Theme::textMuted().name(), esc(key), html);
    };
    auto signedMoney = [&](double v) {
        return (v >= 0.0 ? QStringLiteral("+") : QString()) + money(v);
    };
    auto pnlHtml = [&](double v, double vp) {
        const QColor c = v >= 0.0 ? Theme::up() : Theme::down();
        return QStringLiteral("<b style='color:%1'>%2 元（%3%）</b>")
            .arg(c.name(), signedMoney(v),
                 (vp >= 0.0 ? QStringLiteral("+") : QString()) + pctText(vp));
    };

    QStringList body;
    body << QStringLiteral("<h3 style='margin:0 0 6px 0;'>%1　%2</h3>")
                .arg(esc(r->code), esc(r->name));
    body << QStringLiteral("<table cellpadding='0' cellspacing='0'>");
    body << row(QStringLiteral("买入"),
                QStringLiteral("%1 元 × %2 股（%3）")
                    .arg(money(r->buyPrice)).arg(r->qty)
                    .arg(r->buyDate.toString(QStringLiteral("yyyy-MM-dd"))));
    if (r->closed()) {
        body << row(QStringLiteral("卖出"),
                    QStringLiteral("%1 元（%2）")
                        .arg(money(r->sellPrice))
                        .arg(r->sellDate.toString(QStringLiteral("yyyy-MM-dd"))));
        body << rowRich(QStringLiteral("已实现盈亏"), pnlHtml(r->pnl(), r->pnlPct()));
        body << row(QStringLiteral("持仓天数"), QString::number(r->holdDays()) + QStringLiteral(" 天"));
    } else {
        bool hasQuote = false;
        const double quote = quoteOf(r->code, &hasQuote);
        body << rowRich(QStringLiteral("状态"),
                        QStringLiteral("<b style='color:%1'>持仓中</b>").arg(Theme::warning().name()));
        if (hasQuote) {
            body << row(QStringLiteral("现价"), money(quote) + QStringLiteral(" 元"));
            body << rowRich(QStringLiteral("浮动盈亏"),
                            pnlHtml(r->pnlAt(quote), r->pnlPctAt(quote)));
            body << row(QStringLiteral("市值 / 费用"),
                        QStringLiteral("%1 元 / %2 元")
                            .arg(money(quote * r->qty), money(r->feeAt(quote))));
        } else {
            body << rowRich(QStringLiteral("现价"),
                            QStringLiteral("<span style='color:%1'>无行情（停牌或行情未就绪），"
                                           "暂不计浮动盈亏</span>").arg(Theme::textMuted().name()));
        }
        body << row(QStringLiteral("持仓天数"),
                    QString::number(r->holdingDays(QDate::currentDate())) + QStringLiteral(" 天"));
    }
    body << row(QStringLiteral("计划 买 / 止盈 / 止损"),
                QStringLiteral("%1 / %2 / %3")
                    .arg(r->planBuy > 0 ? money(r->planBuy) : QStringLiteral("—"),
                         r->planTake > 0 ? money(r->planTake) : QStringLiteral("—"),
                         r->planStop > 0 ? money(r->planStop) : QStringLiteral("—")));
    body << row(QStringLiteral("执行"),
                r->executedAsPlanned()
                    ? QStringLiteral("按计划（买入价落在计划区间 ±3%）")
                    : QStringLiteral("偏离计划"));
    if (!r->note.isEmpty()) body << row(QStringLiteral("备注"), r->note);
    body << QStringLiteral("</table>");
    m_detail->setHtml(body.join(QString()));
}

// ---------------------------------------------------------------- AI 诊断
// 缓存键 = 记录 id + 上下文版本 + 评估价：行情没变就不重复算（快照每 60 秒刷新一次）
void JournalPanel::ensureInsights()
{
    for (const TradeRecord& r : m_rows) {
        bool hasQuote = false;
        const double quote = r.closed() ? r.sellPrice : quoteOf(r.code, &hasQuote);
        AiCache& cache = m_aiCache[r.id];
        if (cache.version == m_contextVersion
            && qAbs(cache.price - quote) < 1e-9
            && cache.insight.valid) {
            continue;
        }

        TradeAI::Input in;
        in.trade = r;
        in.asOf = QDate::currentDate();
        in.phase = m_phase;
        in.phaseKnown = m_phaseKnown;
        if (!r.closed() && hasQuote) {
            in.hasQuote = true;
            in.snapshot = m_snaps.value(r.code);
        } else if (!r.closed() && m_snaps.contains(r.code)) {
            // 有快照但不满足有效价（停牌）：仍传入当日量价，Input::hasQuote 保持 false
            in.snapshot = m_snaps.value(r.code);
        }
        const auto kit = m_klines.constFind(r.code);
        if (kit != m_klines.constEnd()) in.kline = &kit.value();

        cache.insight = TradeAI::analyze(in);
        cache.version = m_contextVersion;
        cache.price = quote;
    }

    // 清理已删除记录留下的缓存，避免 id 复用时串味
    QSet<int> alive;
    for (const TradeRecord& r : m_rows) alive.insert(r.id);
    for (auto it = m_aiCache.begin(); it != m_aiCache.end(); ) {
        if (alive.contains(it.key())) ++it;
        else it = m_aiCache.erase(it);
    }
}

const TradeAI::Insight* JournalPanel::insightOf(int id) const
{
    const auto it = m_aiCache.constFind(id);
    return it == m_aiCache.constEnd() ? nullptr : &it.value().insight;
}

// ⑥ 持仓下一步动作：逐只给单一动作 + 一句依据，亏损时不给「再等等」的借口
// ⑥ 持仓下一步动作：**单一事实来源** —— 表格的「下一步动作」列与顶部的动作清单
// 都走这里，避免两处各判一套导致口径漂移。
JournalPanel::NextAction JournalPanel::nextActionFor(const TradeRecord& r) const
{
    NextAction act;
    if (r.closed()) {
        act.label = QStringLiteral("已了结");
        act.why = QStringLiteral("该笔已平仓，累计盈亏见左列");
        act.color = Theme::flat();
        return act;
    }

    bool ok = false;
    const double px = quoteOf(r.code, &ok);
    const QDate today = QDate::currentDate();

    if (!ok || px <= 0.0) {
        act.label = QStringLiteral("待行情");
        act.why = QStringLiteral("拿不到现价，不做盈亏估算，别把 0 元当成真的");
        act.color = Theme::textMuted();
    } else if (r.planStop > 0.0 && px <= r.planStop) {
        act.label = QStringLiteral("止损离场");
        act.why = QStringLiteral("现价 %1 已跌破计划止损 %2，不等回本")
                  .arg(px, 0, 'f', 2).arg(r.planStop, 0, 'f', 2);
        act.color = Theme::danger();
    } else if (r.planTake > 0.0 && px >= r.planTake) {
        act.label = QStringLiteral("分批兑现");
        act.why = QStringLiteral("现价 %1 已到计划止盈 %2，按计划分批落袋")
                  .arg(px, 0, 'f', 2).arg(r.planTake, 0, 'f', 2);
        act.color = Theme::up();
    } else if (r.buyDate.isValid() && r.buyDate < today) {
        act.label = QStringLiteral("T+1 可兑现");
        act.why = QStringLiteral("已过 T+1，浮动 %1，可按计划择机了结")
                  .arg(pctText(r.pnlPctAt(px)));
        act.color = Theme::success();
    } else {
        const double p = r.pnlPctAt(px);
        act.label = p >= 0.0 ? QStringLiteral("持有待涨") : QStringLiteral("持有观察");
        act.why = QStringLiteral("浮动 %1，距止损 %2、止盈 %3")
                  .arg(pctText(p))
                  .arg(r.planStop > 0.0 ? QString::number(r.planStop, 'f', 2)
                                        : QStringLiteral("—"))
                  .arg(r.planTake > 0.0 ? QString::number(r.planTake, 'f', 2)
                                        : QStringLiteral("—"));
        act.color = p >= 0.0 ? Theme::success() : Theme::warning();
    }
    return act;
}

void JournalPanel::updateActionList()
{
    if (!m_actions) return;
    QStringList lines;
    int shown = 0;

    // 与表格共用同一份筛选结果：切到「已了结」时，动作清单不再列出所有持仓
    for (int idx : m_view) {
        const TradeRecord& r = m_rows.at(idx);
        if (r.closed()) continue;
        const NextAction act = nextActionFor(r);
        ++shown;
        lines << QStringLiteral(
            "<div style='margin-bottom:3px'><b>%1 %2</b> ｜ "
            "<span style='color:%3;font-weight:600'>%4</span> ｜ %5</div>")
            .arg(r.code.toHtmlEscaped())
            .arg(r.name.toHtmlEscaped())
            .arg(act.color.name())
            .arg(act.label.toHtmlEscaped())
            .arg(act.why.toHtmlEscaped());
    }

    if (m_actionCount) {
        m_actionCount->setText(shown > 0
            ? QStringLiteral("共 %1 条").arg(shown)
            : QStringLiteral("暂无持仓"));
    }
    if (lines.isEmpty()) {
        const bool filtered = !m_rows.isEmpty() && m_view.size() < m_rows.size();
        m_actions->setHtml(QStringLiteral("<div style='color:%1'>%2</div>")
            .arg(Theme::textMuted().name(),
                 filtered
                     ? QStringLiteral("当前筛选下没有持仓。切回「全部流水」可查看全部持仓动作。")
                     : QStringLiteral("当前无持仓。今日能否出手、可执行清单见「AI选股结果」页。")));
    } else {
        m_actions->setHtml(lines.join(QString()));   // 字号由 QSS #ActionList 统一控制
    }
    // 按内容条数收缩卡片高度：只有一笔持仓时不留一大块空白
    const int wanted = lines.isEmpty() ? 40 : qMin(140, 14 + lines.size() * 24);
    m_actions->setMaximumHeight(qMax(40, wanted));
}

void JournalPanel::updateStats()
{
    const TradeStats s = computeTradeStats(m_rows);
    const int openCount = s.total - s.closedCount;

    // ---- 4 个主 KPI：胜率 / 累计盈亏 / 持仓浮盈 / 最大回撤 ----
    m_kpiWinRate->setValue(s.closedCount > 0
        ? QStringLiteral("%1%").arg(s.winRate * 100, 0, 'f', 1)
        : QStringLiteral("-"));
    m_kpiWinRate->setValueColor(s.closedCount > 0
        ? (s.winRate >= 0.5 ? Theme::success() : Theme::danger())
        : Theme::flat());
    m_kpiWinRate->setHint(s.closedCount > 0
        ? QStringLiteral("%1 胜 / %2 笔").arg(s.winCount).arg(s.closedCount)
        : QStringLiteral("尚无了结记录"));

    m_kpiPnl->setValue(s.closedCount > 0
        ? QStringLiteral("%1 元").arg(money(s.totalPnl)) : QStringLiteral("-"));
    m_kpiPnl->setValueColor(s.closedCount > 0
        ? (s.totalPnl >= 0 ? Theme::up() : Theme::down()) : Theme::flat());
    m_kpiPnl->setHint(QStringLiteral("已实现（已扣双边费用）"));

    // ---- 持仓浮动盈亏：只统计「有现价」的持仓，缺行情的单独计数，不把 0 当盈亏 ----
    double floating = 0.0;
    double floatingCost = 0.0;
    int priced = 0;
    int unpriced = 0;
    for (const TradeRecord& r : m_rows) {
        if (r.closed()) continue;
        bool ok = false;
        const double px = quoteOf(r.code, &ok);
        if (!ok) { ++unpriced; continue; }
        floating += r.pnlAt(px);
        floatingCost += r.buyAmount();
        ++priced;
    }
    const double floatingPct = floatingCost > 0.0 ? floating / floatingCost * 100.0 : 0.0;
    if (priced > 0) {
        m_kpiFloat->setValue(QStringLiteral("%1 元").arg(money(floating)));
        m_kpiFloat->setValueColor(floating >= 0 ? Theme::up() : Theme::down());
        m_kpiFloat->setHint(unpriced > 0
            ? QStringLiteral("%1%，%2 只无行情").arg(QString::number(floatingPct, 'f', 2)).arg(unpriced)
            : QStringLiteral("%1%").arg(QString::number(floatingPct, 'f', 2)));
    } else {
        m_kpiFloat->setValue(openCount > 0 ? QStringLiteral("现价未就绪") : QStringLiteral("-"));
        m_kpiFloat->setValueColor(Theme::flat());
        m_kpiFloat->setHint(QStringLiteral("已扣双边费用"));
    }

    m_kpiDrawdown->setValue(s.closedCount > 0 ? pctText(s.maxDrawdown) : QStringLiteral("-"));
    m_kpiDrawdown->setValueColor(s.closedCount > 0 ? Theme::danger() : Theme::flat());
    m_kpiDrawdown->setHint(QStringLiteral("已了结序列的最大回撤"));

    // ---- 6 个次指标：支撑上面四个结论的明细 ----
    m_kpiTotal->setValue(QStringLiteral("%1 笔").arg(s.total));
    m_kpiTotal->setHint(QStringLiteral("持仓 %1 / 已了结 %2").arg(openCount).arg(s.closedCount));

    m_kpiExpect->setValue(s.closedCount > 0 ? pctText(s.expectancy) : QStringLiteral("-"));
    m_kpiExpect->setValueColor(s.closedCount > 0
        ? (s.expectancy >= 0 ? Theme::success() : Theme::danger()) : Theme::flat());
    m_kpiExpect->setHint(QStringLiteral("每笔期望收益"));

    m_kpiHold->setValue(s.closedCount > 0
        ? QStringLiteral("%1 天").arg(s.avgHoldDays, 0, 'f', 1) : QStringLiteral("-"));
    m_kpiHold->setHint(QStringLiteral("平均持仓"));

    m_kpiFollow->setValue(s.closedCount > 0
        ? QStringLiteral("%1%").arg(double(s.planFollowed) / s.closedCount * 100, 0, 'f', 0)
        : QStringLiteral("-"));
    m_kpiFollow->setHint(QStringLiteral("按计划执行率"));

    m_kpiStopHit->setValue(s.closedCount > 0
        ? QStringLiteral("%1%").arg(s.stopLossHitRate * 100, 0, 'f', 0) : QStringLiteral("-"));
    m_kpiStopHit->setHint(QStringLiteral("止损命中率"));

    // 行情时间：让用户知道盈亏是按哪一刻的价格算的
    if (!m_snaps.isEmpty()) {
        m_kpiQuotes->setValue(m_quoteStamp.isEmpty()
            ? QStringLiteral("%1 只可报价").arg(priced) : m_quoteStamp);
        m_kpiQuotes->setValueColor(Theme::textMuted());
    } else {
        m_kpiQuotes->setValue(QStringLiteral("未就绪"));
        m_kpiQuotes->setValueColor(Theme::warning());
    }
}

void JournalPanel::importPlan(const TradePlan& plan)
{
    if (!m_store) return;
    openRecordDialog(plan.valid() ? &plan : nullptr, nullptr);
}

void JournalPanel::onAdd()
{
    openRecordDialog(nullptr, nullptr);
}

void JournalPanel::onEdit()
{
    const TradeRecord* r = selectedRecord();
    if (!r) {
        QMessageBox::information(this, QStringLiteral("编辑"), QStringLiteral("请先选中一条记录"));
        return;
    }
    openRecordDialog(nullptr, r);
}

void JournalPanel::openRecordDialog(const TradePlan* plan, const TradeRecord* existing)
{
    if (!m_store) return;

    QDialog dlg(this);
    dlg.setWindowTitle(existing ? QStringLiteral("编辑交易记录")
                                : (plan ? QStringLiteral("记录买入 - %1 %2").arg(plan->code, plan->name)
                                        : QStringLiteral("记录买入")));
    auto* form = new QFormLayout(&dlg);

    auto* date = new QDateEdit((existing ? existing->buyDate : QDate::currentDate()), &dlg);
    date->setCalendarPopup(true);
    date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto* code = new QLineEdit(existing ? existing->code : (plan ? plan->code : QString()), &dlg);
    auto* name = new QLineEdit(existing ? existing->name : (plan ? plan->name : QString()), &dlg);
    auto* price = new QDoubleSpinBox(&dlg);
    price->setRange(0.01, 10000);
    price->setDecimals(2);
    price->setValue(existing ? existing->buyPrice
                             : (plan ? (plan->buyHigh > 0 ? plan->buyHigh : plan->buyLow)
                                     : 10.0));
    auto* qty = new QSpinBox(&dlg);
    qty->setRange(100, 10000000);
    qty->setSingleStep(100);
    qty->setValue(existing ? existing->qty : 10000);

    auto* planBuy = new QDoubleSpinBox(&dlg);
    auto* planStop = new QDoubleSpinBox(&dlg);
    auto* planTake = new QDoubleSpinBox(&dlg);
    for (QDoubleSpinBox* sp : { planBuy, planStop, planTake }) {
        sp->setRange(0.0, 10000);
        sp->setDecimals(2);
        sp->setSpecialValueText(QStringLiteral("-"));   // 0 表示未设置
    }
    planBuy->setValue(existing ? existing->planBuy : (plan ? (plan->buyLow + plan->buyHigh) * 0.5 : 0.0));
    planStop->setValue(existing ? existing->planStop : (plan ? plan->stopLossHigh : 0.0));
    planTake->setValue(existing ? existing->planTake : (plan ? plan->takeProfitLow : 0.0));

    auto* chkClosed = new QCheckBox(QStringLiteral("已了结（补录卖出信息）"), &dlg);
    chkClosed->setChecked(existing && existing->closed());
    auto* sellDate = new QDateEdit((existing && existing->sellDate.isValid())
                                       ? existing->sellDate : QDate::currentDate(), &dlg);
    sellDate->setCalendarPopup(true);
    sellDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto* sellPrice = new QDoubleSpinBox(&dlg);
    sellPrice->setRange(0.01, 10000);
    sellPrice->setDecimals(2);
    sellPrice->setValue(existing && existing->sellPrice > 0 ? existing->sellPrice : price->value());
    sellDate->setEnabled(chkClosed->isChecked());
    sellPrice->setEnabled(chkClosed->isChecked());

    auto* note = new QLineEdit(existing ? existing->note : QString(), &dlg);
    note->setPlaceholderText(QStringLiteral("例如：竞价高开 3%，按计划接力"));

    auto* preview = new QLabel(&dlg);
    preview->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));

    form->addRow(QStringLiteral("买入日期"), date);
    form->addRow(QStringLiteral("代码"), code);
    form->addRow(QStringLiteral("名称"), name);
    form->addRow(QStringLiteral("买入价"), price);
    form->addRow(QStringLiteral("数量（股）"), qty);
    form->addRow(QStringLiteral("计划买 / 止盈 / 止损"),
                 [&]() {
                     auto* row = new QWidget(&dlg);
                     auto* l = new QHBoxLayout(row);
                     l->setContentsMargins(0, 0, 0, 0);
                     l->addWidget(planBuy); l->addWidget(planStop); l->addWidget(planTake);
                     return row;
                 }());
    form->addRow(chkClosed);
    form->addRow(QStringLiteral("卖出日期"), sellDate);
    form->addRow(QStringLiteral("卖出价"), sellPrice);
    form->addRow(QStringLiteral("备注"), note);
    form->addRow(QStringLiteral("预估"), preview);

    auto updatePreview = [&]() {
        if (!chkClosed->isChecked()) {
            preview->setText(QStringLiteral("持仓中（未了结）"));
            return;
        }
        const double buy = price->value() * qty->value();
        const double fee = buy * 0.00025 + sellPrice->value() * qty->value() * 0.00125;
        const double pnl = sellPrice->value() * qty->value() - buy - fee;
        const double pnlPct = buy > 0 ? pnl / buy * 100.0 : 0.0;
        const QColor c = pnl >= 0 ? Theme::up() : Theme::down();
        preview->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(c.name()));
        preview->setText(QStringLiteral("盈亏 %1 元（%2%）")
                             .arg(money(pnl), QString::number(pnlPct, 'f', 2)));
    };
    connect(chkClosed, &QCheckBox::toggled, &dlg, [&](bool on) {
        sellDate->setEnabled(on);
        sellPrice->setEnabled(on);
        updatePreview();
    });
    for (QDoubleSpinBox* sp : { price, sellPrice }) {
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dlg, [&](double) {
            updatePreview();
        });
    }
    connect(qty, QOverload<int>::of(&QSpinBox::valueChanged), &dlg, [&](int) { updatePreview(); });
    updatePreview();

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    TradeRecord r;
    r.id = existing ? existing->id : 0;
    r.buyDate = date->date();
    r.code = code->text().trimmed();
    r.name = name->text().trimmed();
    r.buyPrice = price->value();
    r.qty = qty->value();
    r.planBuy = planBuy->value();
    r.planStop = planStop->value();
    r.planTake = planTake->value();
    if (chkClosed->isChecked()) {
        r.sellDate = sellDate->date();
        r.sellPrice = sellPrice->value();
    }
    r.note = note->text().trimmed();

    if (r.code.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("代码不能为空"));
        return;
    }
    if (r.buyPrice <= 0 || r.qty <= 0) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("买入价与数量必须大于 0"));
        return;
    }
    if (r.closed() && r.sellPrice <= 0) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("已了结时卖出价必须大于 0"));
        return;
    }

    const bool ok = existing ? m_store->updateTrade(r) : m_store->addTrade(r);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), m_store->lastError());
        return;
    }
    refresh();
    Q_EMIT statusMessage(existing
        ? QStringLiteral("已更新记录：%1 %2").arg(r.code, r.name)
        : QStringLiteral("已记录买入：%1 %2 @ %3 × %4")
              .arg(r.code, r.name, money(r.buyPrice)).arg(r.qty));
}

void JournalPanel::onDelete()
{
    const TradeRecord* r = selectedRecord();
    if (!r) {
        QMessageBox::information(this, QStringLiteral("删除"), QStringLiteral("请先选中一条记录"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("删除"),
                              QStringLiteral("删除 %1 %2 的交易记录？").arg(r->code, r->name))
        != QMessageBox::Yes) {
        return;
    }
    const int id = r->id;
    const QString code = r->code;
    const QString name = r->name;
    m_store->deleteTrade(id);
    refresh();
    Q_EMIT statusMessage(QStringLiteral("已删除记录：%1 %2").arg(code, name));
}

void JournalPanel::onExport()
{
    if (m_view.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出"), QStringLiteral("当前没有可导出的记录"));
        return;
    }
    const QString defName = QStringLiteral("journal_%1.csv")
        .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出交易日志"),
                                                      defName, QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), f.errorString());
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    // CSV 转义：加引号、双引号翻倍，避免代码/备注里的逗号破坏列
    auto esc = [](const QString& s) {
        QString t = s;
        t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"") + t + QStringLiteral("\"");
    };
    ts << QStringLiteral("买入日,代码,名称,状态,买入价,现价,数量,卖出日,卖出价,盈亏(元),盈亏%,持仓(天),"
                         "计划买,计划止盈,计划止损,纪律,AI诊断,盈亏原因,止盈策略,止盈执行,回本可行性,"
                         "含费回本价,还需涨幅%,预计天数,风险提示,备注\n");
    for (int idx : m_view) {
        const TradeRecord& r = m_rows.at(idx);
        bool hasQuote = false;
        const double quote = r.closed() ? 0.0 : quoteOf(r.code, &hasQuote);
        const bool havePnl = r.closed() || hasQuote;

        // AI 诊断随行导出：让 CSV 也能直接回答「为什么赚/亏、怎么止盈、能不能回本」
        const TradeAI::Insight* ai = insightOf(r.id);
        QStringList reasons;
        QStringList steps;
        QStringList risks;
        if (ai) {
            for (const TradeAI::ReasonItem& x : ai->reasons)
                reasons << QStringLiteral("%1：%2").arg(x.title, x.detail);
            for (const TradeAI::ExitStep& s : ai->exitSteps)
                steps << QStringLiteral("%1（%2）").arg(s.action, s.trigger);
            risks = ai->riskNotes;
        }
        const QString aiTag = ai ? ai->stateTag : QString();
        const QString aiRecovery = ai
            ? QStringLiteral("%1：%2").arg(TradeAI::feasibilityName(ai->recovery.feasibility),
                                           ai->recovery.verdict)
            : QString();
        const QString aiDays = (ai && ai->recovery.daysHigh > 0)
            ? QStringLiteral("%1~%2").arg(ai->recovery.daysLow).arg(ai->recovery.daysHigh)
            : QString();

        ts << esc(r.buyDate.toString(QStringLiteral("yyyy-MM-dd"))) << ','
           << esc(r.code) << ',' << esc(r.name) << ','
           << esc(r.closed() ? QStringLiteral("已了结") : QStringLiteral("持仓中")) << ','
           << money(r.buyPrice) << ','
           << (hasQuote ? money(quote) : QString()) << ','
           << r.qty << ','
           << esc(r.closed() ? r.sellDate.toString(QStringLiteral("yyyy-MM-dd")) : QString()) << ','
           << (r.closed() ? money(r.sellPrice) : QString()) << ','
           << (havePnl ? money(r.closed() ? r.pnl() : r.pnlAt(quote)) : QString()) << ','
           << (havePnl ? pctText(r.closed() ? r.pnlPct() : r.pnlPctAt(quote)) : QString()) << ','
           << r.holdingDays(QDate::currentDate()) << ','
           << (r.planBuy > 0 ? money(r.planBuy) : QString()) << ','
           << (r.planStop > 0 ? money(r.planStop) : QString()) << ','
           << (r.planTake > 0 ? money(r.planTake) : QString()) << ','
           << esc(r.executedAsPlanned() ? QStringLiteral("按计划") : QStringLiteral("偏离")) << ','
           << esc(aiTag) << ','
           << esc(reasons.join(QStringLiteral("；"))) << ','
           << esc(ai ? ai->exitStrategy : QString()) << ','
           << esc(steps.join(QStringLiteral("；"))) << ','
           << esc(aiRecovery) << ','
           << (ai ? money(ai->recovery.breakeven) : QString()) << ','
           << (ai ? QString::number(ai->recovery.requiredPct, 'f', 2) : QString()) << ','
           << aiDays << ','
           << esc(risks.join(QStringLiteral("；"))) << ','
           << esc(r.note) << '\n';
    }
    f.close();
    Q_EMIT statusMessage(QStringLiteral("已导出 %1 条记录 → %2").arg(m_view.size()).arg(path));
}
