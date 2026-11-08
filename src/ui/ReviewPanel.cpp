#include "ui/ReviewPanel.h"

#include "review/ReviewAssistant.h"
#include "storage/Database.h"
#include "storage/JournalStore.h"
#include "ui/Components.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <algorithm>

namespace {

QTableWidget* makeTable(const QStringList& headers, QWidget* parent)
{
    auto* t = new QTableWidget(0, headers.size(), parent);
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->setAlternatingRowColors(true);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->horizontalHeader()->setStretchLastSection(true);
    Theme::polishTable(t);
    return t;
}

void put(QTableWidget* t, int row, int col, const QString& text,
         const QColor& color = QColor(), const QString& tip = QString())
{
    auto* it = new QTableWidgetItem(text);
    if (color.isValid()) it->setForeground(color);
    it->setToolTip(tip.isEmpty() ? text : tip);   // 完整文本永远可在 tooltip 里核对
    t->setItem(row, col, it);
}

// 数值列统一右对齐：行情数字右对齐是既定规范，扫读时小数点对齐更易比较
void putNum(QTableWidget* t, int row, int col, const QString& text,
            const QColor& color = QColor(), const QString& tip = QString())
{
    put(t, row, col, text, color, tip);
    if (auto* it = t->item(row, col))
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

QString brief(const QString& s, int n = 34)
{
    return s.length() <= n ? s : s.left(n) + QStringLiteral("…");
}

QString num(double v, int d = 2) { return QString::number(v, 'f', d); }
QString pct(double v, int d = 2) { return num(v, d) + QStringLiteral("%"); }
QString esc(const QString& s) { return s.toHtmlEscaped(); }
QString dateText(const QDate& d) { return d.isValid() ? d.toString(Qt::ISODate) : QStringLiteral("—"); }

QString htmlWrap(const QString& body)
{
    return QStringLiteral(
        "<div style=\"font-family:'Microsoft YaHei',Arial,sans-serif;"
        "color:%1;font-size:%2px;line-height:1.6\">%3</div>")
        .arg(Theme::text().name()).arg(Theme::fontSm()).arg(body);
}

const char* kTd = "padding:5px 7px;border-bottom:1px solid #eef1f5;vertical-align:top";
const char* kTh = "text-align:left;padding:5px 7px;background:#f2f5fa;"
                  "border-bottom:1px solid #e2e6ed;font-weight:600";

} // namespace

// ---------------------------------------------------------------- 构造
ReviewPanel::ReviewPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(Theme::Space::Sm);

    // ---- 控制行：全部高频操作集中在这一条，不需要到各处找入口 ----
    auto* bar = new QWidget(this);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(Theme::Space::Sm);
    auto* dateLabel = new QLabel(QStringLiteral("复盘交易日"), bar);
    dateLabel->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));
    h->addWidget(dateLabel);
    m_dateBox = new QComboBox(bar);
    m_dateBox->setFixedWidth(140);
    m_dateBox->setToolTip(QStringLiteral(
        "切换交易日回看「当时的想法」与事后走势；非交易日会自动定位到最近有留档的交易日"));
    h->addWidget(m_dateBox);

    auto* filterLabel = new QLabel(QStringLiteral("筛选"), bar);
    filterLabel->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));
    h->addWidget(filterLabel);
    m_filterBox = new QLineEdit(bar);
    m_filterBox->setFixedWidth(200);
    m_filterBox->setPlaceholderText(QStringLiteral("代码 / 名称 / 关键词"));
    m_filterBox->setClearButtonEnabled(true);
    m_filterBox->setToolTip(QStringLiteral(
        "按关键词隐藏当前页签里不匹配的行，快速定位标的；切换页签时筛选词继续生效，清空即恢复"));
    h->addWidget(m_filterBox);
    h->addStretch(1);
    m_status = new QLabel(QStringLiteral("等待数据…"), bar);
    m_status->setObjectName(QStringLiteral("Muted"));
    h->addWidget(m_status);
    root->addWidget(bar);

    // ---- 摘要徽章行：一屏看清本轮复盘全貌，缺口徽章可点击直达核对页 ----
    m_kpiRow = new QWidget(this);
    m_kpiLay = new QHBoxLayout(m_kpiRow);
    m_kpiLay->setContentsMargins(0, 0, 0, 0);
    m_kpiLay->setSpacing(Theme::Space::Sm);
    m_kpiLay->addStretch(1);
    root->addWidget(m_kpiRow);

    // ---- 首次使用引导（可关闭，记忆「不再提示」）----
    m_guideCard = qobject_cast<QFrame*>(buildGuideCard());
    if (m_guideCard) root->addWidget(m_guideCard);

    // ---- 数据库未就绪的整页空态：结论 + 原因 + 恢复建议，而不是一片空白 ----
    m_dbEmpty = buildDbEmpty();
    m_dbEmpty->setVisible(false);
    root->addWidget(m_dbEmpty);

    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs, 1);

    // ---- ① 信号溯源：信号表 / 判定条件全文（含数据缺口与空态说明）----
    auto* sigSplit = new QSplitter(Qt::Vertical, m_tabs);
    m_signalTable = makeTable({
        QStringLiteral("交易日"), QStringLiteral("代码"), QStringLiteral("名称"),
        QStringLiteral("信号方向"), QStringLiteral("档位"), QStringLiteral("条件(成立/有数据/总数)"),
        QStringLiteral("数据完整性"), QStringLiteral("当时的策略预期") }, sigSplit);
    m_criteriaView = new QTextBrowser(sigSplit);
    m_criteriaView->setOpenExternalLinks(false);
    sigSplit->addWidget(m_signalTable);
    sigSplit->addWidget(m_criteriaView);
    sigSplit->setStretchFactor(0, 3);
    sigSplit->setStretchFactor(1, 2);
    m_tabTitles << QStringLiteral("信号溯源");
    m_tabs->addTab(sigSplit, m_tabTitles.last());

    // ---- ② 预期 vs 实际 ----
    m_tabTitles << QStringLiteral("预期 vs 实际");
    m_tabs->addTab(buildRecordPage({
        QStringLiteral("代码"), QStringLiteral("名称"), QStringLiteral("信号日收盘"),
        QStringLiteral("计划止盈"), QStringLiteral("计划止损"), QStringLiteral("区间最高 / MFE"),
        QStringLiteral("区间最低 / MAE"), QStringLiteral("末收 / 涨跌"),
        QStringLiteral("目标偏差"), QStringLiteral("触及止盈"), QStringLiteral("触及止损"),
        QStringLiteral("观察交易日"), QStringLiteral("白话结论") },
        &m_outcomeTable, &m_outcomeDetail), m_tabTitles.last());

    // ---- ③ 跳过的机会 ----
    m_tabTitles << QStringLiteral("跳过的机会");
    m_tabs->addTab(buildRecordPage({
        QStringLiteral("信号日"), QStringLiteral("代码"), QStringLiteral("名称"),
        QStringLiteral("计划买入中枢"), QStringLiteral("未执行判定"),
        QStringLiteral("后续最高 / MFE"), QStringLiteral("后续最低 / MAE"),
        QStringLiteral("末收 / 涨跌"), QStringLiteral("机械假设收益"),
        QStringLiteral("触及止盈"), QStringLiteral("触及止损"),
        QStringLiteral("观察交易日"), QStringLiteral("数据说明") },
        &m_skippedTable, &m_skippedDetail), m_tabTitles.last());

    // ---- ④ 持仓隐性风险 ----
    m_tabTitles << QStringLiteral("持仓隐性风险");
    m_tabs->addTab(buildRecordPage({
        QStringLiteral("代码"), QStringLiteral("名称"), QStringLiteral("风险类型"),
        QStringLiteral("判定"), QStringLiteral("实测值"), QStringLiteral("判定式 / 阈值"),
        QStringLiteral("数据来源与窗口"), QStringLiteral("结论（带数值）") },
        &m_riskTable, &m_riskDetail), m_tabTitles.last());

    // ---- ⑤ 板块轮动数据：数据状态与重试入口放在页签内（语境就近）----
    m_tabTitles << QStringLiteral("板块轮动数据");
    auto* sectorSplit = new QSplitter(Qt::Vertical, m_tabs);
    m_sectorTable = makeTable({
        QStringLiteral("标记"), QStringLiteral("板块"), QStringLiteral("当日涨跌幅"),
        QStringLiteral("主力净流入(亿元)"), QStringLiteral("主力净占比"),
        QStringLiteral("资金流数据"), QStringLiteral("字段来源") }, sectorSplit);
    auto* sectorBottom = new QWidget(sectorSplit);
    auto* sl2 = new QVBoxLayout(sectorBottom);
    sl2->setContentsMargins(0, 0, 0, 0);
    sl2->setSpacing(Theme::Space::Xs);
    auto* retryRow = new QWidget(sectorBottom);
    auto* rh = new QHBoxLayout(retryRow);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->setSpacing(Theme::Space::Sm);
    m_sectorStatus = new QLabel(QStringLiteral("数据状态：待生成"), retryRow);
    m_sectorStatus->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontXs(), false));
    rh->addWidget(m_sectorStatus);
    rh->addStretch(1);
    m_btnSectorRetry = new QPushButton(QStringLiteral("重新拉取板块数据"), retryRow);
    m_btnSectorRetry->setToolTip(QStringLiteral(
        "拉取当日领涨/领跌板块的资金流快照；失败原因会留档并显示在这里，不用涨幅反推资金流"));
    rh->addWidget(m_btnSectorRetry);
    sl2->addWidget(retryRow);
    m_sectorDetail = new QTextBrowser(sectorBottom);
    m_sectorDetail->setOpenExternalLinks(false);
    sl2->addWidget(m_sectorDetail);
    sectorSplit->addWidget(m_sectorTable);
    sectorSplit->addWidget(sectorBottom);
    sectorSplit->setStretchFactor(0, 4);
    sectorSplit->setStretchFactor(1, 1);
    m_tabs->addTab(sectorSplit, m_tabTitles.last());

    // ---- ⑥ 原有市场复盘 / 早盘简报（辅助，不参与决策复盘结论）----
    auto* legacySplit = new QSplitter(Qt::Horizontal, m_tabs);
    m_reviewView = new QTextBrowser(legacySplit);
    m_briefView = new QTextBrowser(legacySplit);
    m_reviewView->setOpenExternalLinks(false);
    m_briefView->setOpenExternalLinks(false);
    legacySplit->addWidget(m_reviewView);
    legacySplit->addWidget(m_briefView);
    m_tabTitles << QStringLiteral("市场复盘与早盘简报");
    m_tabs->addTab(legacySplit, m_tabTitles.last());

    // ---- ⑦ 原有预判准确率 / 规则 / 教训 ----
    auto* statPage = new QWidget(m_tabs);
    auto* sl = new QVBoxLayout(statPage);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(Theme::Space::Sm);
    auto* statSplit = new QSplitter(Qt::Vertical, statPage);
    m_evalTable = makeTable({ QStringLiteral("交易日"), QStringLiteral("评分"),
                              QStringLiteral("结果"), QStringLiteral("涨停(实际)"),
                              QStringLiteral("高度(实际)"), QStringLiteral("主要偏差") }, statSplit);
    m_ruleTable = makeTable({ QStringLiteral("规则"), QStringLiteral("触发"),
                              QStringLiteral("命中"), QStringLiteral("准确率"),
                              QStringLiteral("权重"), QStringLiteral("状态") }, statSplit);
    m_lessonList = new QListWidget(statSplit);
    m_lessonList->setAlternatingRowColors(true);
    statSplit->addWidget(m_evalTable);
    statSplit->addWidget(m_ruleTable);
    statSplit->addWidget(m_lessonList);
    statSplit->setStretchFactor(0, 1);
    statSplit->setStretchFactor(1, 1);
    statSplit->setStretchFactor(2, 1);
    sl->addWidget(statSplit, 1);
    m_strategyLabel = new QLabel(QStringLiteral("有效策略库：暂无"), statPage);
    m_strategyLabel->setObjectName(QStringLiteral("Muted"));
    m_strategyLabel->setWordWrap(true);
    sl->addWidget(m_strategyLabel);
    m_tabTitles << QStringLiteral("预判准确率与教训");
    m_tabs->addTab(statPage, m_tabTitles.last());

    connect(m_dateBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (m_updatingDates) return;
                m_date = m_dateBox->currentData().toDate();
                rebuildDecisionReview(true);
            });
    connect(m_filterBox, &QLineEdit::textChanged, this,
            [this](const QString&) { applyFilter(); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { applyFilter(); });
    connect(m_outcomeTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { renderOutcomeDetail(row); });
    connect(m_skippedTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { renderSkippedDetail(row); });
    connect(m_riskTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { renderRiskDetail(row); });
    connect(m_btnSectorRetry, &QPushButton::clicked, this, [this]() {
        if (!m_assistant) return;
        m_sectorStatus->setText(QStringLiteral("正在拉取当日板块资金流…（失败原因会显示在这里）"));
        m_assistant->refreshSectorFlows(true);
    });

    // Ctrl+R / F5：随手重算，不用到顶栏找按钮
    auto* sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+R")), this);
    sc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(sc, &QShortcut::activated, this, &ReviewPanel::refresh);
    auto* sc2 = new QShortcut(QKeySequence(QStringLiteral("F5")), this);
    sc2->setContext(Qt::WidgetWithChildrenShortcut);
    connect(sc2, &QShortcut::activated, this, &ReviewPanel::refresh);

    m_pageActions = buildPageActions();
    m_tabs->setCurrentIndex(0);
}

QSplitter* ReviewPanel::buildRecordPage(const QStringList& headers, QTableWidget** outTable,
                                        QTextBrowser** outDetail)
{
    auto* split = new QSplitter(Qt::Vertical, m_tabs);
    *outTable = makeTable(headers, split);
    *outDetail = new QTextBrowser(split);
    (*outDetail)->setOpenExternalLinks(false);
    split->addWidget(*outTable);
    split->addWidget(*outDetail);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    return split;
}

QWidget* ReviewPanel::buildPageActions()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("PageActions"));
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(Theme::Space::Sm);

    auto* btnRebuild = new QPushButton(QStringLiteral("重算"), bar);
    btnRebuild->setObjectName(QStringLiteral("Primary"));
    btnRebuild->setToolTip(QStringLiteral(
        "按所选交易日重新生成全部复盘视图（快捷键 Ctrl+R）；结论落库，下次打开可直接回看"));
    h->addWidget(btnRebuild);

    m_btnReview = new QPushButton(QStringLiteral("立即复盘"), bar);
    m_btnReview->setToolTip(QStringLiteral(
        "按当前数据重算市场收盘复盘（15:00 后自动执行过一次），并顺带刷新决策复盘"));
    h->addWidget(m_btnReview);

    m_btnBrief = new QPushButton(QStringLiteral("早盘简报"), bar);
    m_btnBrief->setToolTip(QStringLiteral(
        "基于最近一次市场复盘生成次日预判并发送邮件（辅助功能，与决策复盘结论无关）"));
    h->addWidget(m_btnBrief);

    connect(btnRebuild, &QPushButton::clicked, this, &ReviewPanel::refresh);
    connect(m_btnReview, &QPushButton::clicked, this, [this]() {
        if (!m_assistant) return;
        m_assistant->runCloseReview(true);
        refresh();
    });
    connect(m_btnBrief, &QPushButton::clicked, this, [this]() {
        if (!m_assistant) return;
        m_assistant->runMorningBrief(true);
        refresh();
    });

    bar->hide();   // 未挂到顶栏前保持隐藏
    return bar;
}

QWidget* ReviewPanel::buildGuideCard()
{
    QSettings settings(QStringLiteral("DeepInvestQuant"), QStringLiteral("ReviewGuide"));
    if (settings.value(QStringLiteral("dismissed"), false).toBool()) return nullptr;

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("GuideCard"));
    card->setStyleSheet(QStringLiteral(
        "QFrame#GuideCard{background:%1;border:1px solid %2;border-radius:8px;}")
        .arg(Theme::accentSoft().name(), Theme::accentMuted().name()));
    auto* l = new QHBoxLayout(card);
    l->setContentsMargins(Theme::Space::Lg, Theme::Space::Md, Theme::Space::Lg, Theme::Space::Md);
    l->setSpacing(Theme::Space::Lg);

    auto* text = new QLabel(card);
    text->setWordWrap(true);
    text->setText(QStringLiteral(
        "<b>第一次用决策复盘？三步看懂</b><br>"
        "① 顶部选交易日：回看「当时的想法」与事后走势（周末自动定位最近有留档的交易日）<br>"
        "② 五个页签各回答一个问题：条件是什么 → 偏差多大 → 没做的后来怎样 → 持仓有什么隐患 → 板块资金去了哪<br>"
        "③ 每条结论都带判定式与数据来源：点表格任意行看明细；数据缺失会标明原因；Ctrl+R 随时重算"));
    text->setStyleSheet(Theme::textStyle(Theme::text(), Theme::fontSm(), false)
                        + QStringLiteral("background:transparent;"));
    l->addWidget(text, 1);

    auto* dismiss = new QPushButton(QStringLiteral("知道了，不再提示"), card);
    dismiss->setToolTip(QStringLiteral("关闭引导；各页签空态里保留了同样的说明，随时可以找回"));
    l->addWidget(dismiss, 0);
    connect(dismiss, &QPushButton::clicked, this, [this]() {
        QSettings settings(QStringLiteral("DeepInvestQuant"), QStringLiteral("ReviewGuide"));
        settings.setValue(QStringLiteral("dismissed"), true);
        if (m_guideCard) m_guideCard->hide();
    });
    return card;
}

QWidget* ReviewPanel::buildDbEmpty()
{
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("Card"));
    auto* l = new QVBoxLayout(card);
    l->setContentsMargins(Theme::Space::Lg, Theme::Space::Lg, Theme::Space::Lg, Theme::Space::Lg);
    l->setSpacing(Theme::Space::Sm);

    auto* title = new QLabel(QStringLiteral("本地数据库未就绪，决策复盘暂不可用"), card);
    title->setStyleSheet(Theme::textStyle(Theme::danger(), Theme::fontLg(), true));
    l->addWidget(title);

    auto* body = new QLabel(card);
    body->setWordWrap(true);
    body->setText(QStringLiteral(
        "决策复盘依赖本地 SQLite 留档：历史交易计划、日 K 缓存与板块资金流快照都存在里面，\n"
        "没有这份留档就无法核对「当时的条件」，也不会用今天的行情补造历史信号。\n\n"
        "恢复建议：① 重启程序（首次启动会自动建库）；② 检查磁盘空间与杀毒软件是否拦截了数据库文件；\n"
        "③ 仍失败时查看日志目录里的最新错误。"));
    body->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));
    l->addWidget(body);
    l->addStretch(1);
    return card;
}

void ReviewPanel::setAssistant(ReviewAssistant* assistant)
{
    m_assistant = assistant;
    if (!m_assistant) return;
    connect(m_assistant, &ReviewAssistant::updated, this, &ReviewPanel::refresh);
    refresh();
}

void ReviewPanel::setContext(Database* db, JournalStore* journal,
                             const QHash<QString, KlineSeries>* klines)
{
    m_db = db;
    m_journal = journal;
    m_klines = klines;
    fillDates();
    refresh();
}

int ReviewPanel::tabCount() const { return m_tabs ? m_tabs->count() : 0; }
QString ReviewPanel::currentTabText() const
{
    return m_tabs ? m_tabs->tabText(m_tabs->currentIndex()) : QString();
}

void ReviewPanel::setTabCount(int index, const QString& suffix)
{
    if (index < 0 || index >= m_tabTitles.size()) return;
    m_tabs->setTabText(index, suffix.isEmpty()
        ? m_tabTitles.at(index)
        : QStringLiteral("%1 · %2").arg(m_tabTitles.at(index), suffix));
}

void ReviewPanel::fillDates()
{
    if (!m_dateBox) return;
    m_updatingDates = true;
    const QDate keep = m_date;
    const QDate today = QDate::currentDate();
    m_dateBox->clear();

    QVector<QDate> dates;
    if (m_db && m_db->isOpen()) dates = m_db->recommendDates();
    if (m_assistant && m_assistant->hasInput() && m_assistant->input().date.isValid()
        && !dates.contains(m_assistant->input().date))
        dates.append(m_assistant->input().date);
    // 今天只在「是交易日」或「已有留档」时进入列表：周末打开复盘助手直接看到
    // 最近交易日的复盘，而不是一张空表
    if (!dates.contains(today) && today.dayOfWeek() <= Qt::Friday)
        dates.append(today);
    std::sort(dates.begin(), dates.end(), [](const QDate& a, const QDate& b) { return a > b; });
    QVector<QDate> unique;
    for (const QDate& d : dates)
        if (d.isValid() && (unique.isEmpty() || unique.last() != d)) unique.append(d);
    for (const QDate& d : unique)
        m_dateBox->addItem(d.toString(QStringLiteral("yyyy-MM-dd")), d);

    const int idx = m_dateBox->findData(keep);
    m_dateBox->setCurrentIndex(idx >= 0 ? idx : 0);
    m_date = m_dateBox->currentData().toDate();
    m_updatingDates = false;
}

// ---------------------------------------------------------------- 报告组装
void ReviewPanel::rebuildDecisionReview(bool persist)
{
    const bool dbReady = m_db && m_db->isOpen();
    m_dbEmpty->setVisible(!dbReady);
    m_tabs->setVisible(dbReady);
    if (!dbReady) {
        m_status->setText(QStringLiteral("本地库未就绪 · 无法核对"));
        m_kpiRow->setVisible(false);   // 没有结论时不渲染徽章，避免「0 风险」误导
        return;
    }
    m_kpiRow->setVisible(true);
    const QDate date = m_date.isValid() ? m_date : QDate::currentDate();

    DecisionReview::Input in;
    in.date = date;
    in.asOf = date;   // 指标一律截断到复盘日，避免用后面的行情解释当时的判断

    const RecommendBundle bundle = m_db->loadRecommend(date);
    in.plans = bundle.plans;
    for (const TradePlan& p : bundle.backups) {
        bool exists = false;
        for (const TradePlan& q : in.plans) if (q.code == p.code) { exists = true; break; }
        if (!exists) in.plans.append(p);
    }
    // 当日结果可能还没来得及落库：用助手里刚注入的那份
    if (in.plans.isEmpty() && m_assistant && m_assistant->hasInput()
        && m_assistant->input().date == date)
        in.plans = m_assistant->input().bundle.plans;

    m_journalError.clear();
    if (m_journal) {
        in.trades = m_journal->trades();
        // 后端（MySQL / SQLite）失败会返回空列表并留下原因：如实展示，不当作「没有记录」
        if (in.trades.isEmpty() && !m_journal->lastError().isEmpty())
            m_journalError = m_journal->lastError();
    }

    QSet<QString> codes;
    for (const TradePlan& p : in.plans) codes.insert(p.code);
    for (const TradeRecord& t : in.trades) if (!t.closed()) codes.insert(t.code);
    for (const QString& code : codes) {
        if (m_klines && m_klines->contains(code)) {
            in.klines.insert(code, m_klines->value(code));
        } else {
            const KlineSeries k = m_db->loadKline(code);
            if (k.valid()) in.klines.insert(code, k);
        }
    }

    // 只有复盘「当日」时才使用实时情绪 / 资金流事实；历史日期绝不拿今天的数据倒推
    if (m_assistant && m_assistant->hasInput() && m_assistant->input().date == date) {
        const ReviewInput& live = m_assistant->input();
        in.sentimentKnown = live.sentiment.fact.valid();
        in.composite = live.sentiment.composite;
        in.tradeAllowed = live.sentiment.tradeAllowed;
        in.positionCapPct = live.sentiment.positionCapPct;
        in.mainNetYi = live.mainNetYi;
        in.mainNetSource = live.mainNetSource;
        in.sealToAmount = live.sealToAmount;
    }

    if (m_assistant) {
        in.sectorRows = m_assistant->sectorFlows(date, &in.sectorFetchedAt,
                                                 &in.sectorSource, &in.sectorReason);
    } else {
        in.sectorReason = QStringLiteral("复盘助手未初始化，无法读取板块资金流留档");
    }

    m_report = DecisionReview::build(in);
    if (persist) m_db->saveDecisionReview(m_report);

    m_status->setText(QStringLiteral("已重算 · %1").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    renderKpis();
    renderSignals();
    renderOutcomes();
    renderSkipped();
    renderRisks();
    renderSectors();
    renderCriteria(m_criteriaView);
}

void ReviewPanel::refresh()
{
    fillDates();                 // 保留当前所选交易日，同时纳入新产生的日期
    rebuildDecisionReview(true);
    renderLegacy();
}

// ---------------------------------------------------------------- 摘要徽章
void ReviewPanel::renderKpis()
{
    // 清空旧徽章后按本轮报告重建：每次重算都是同一套口径，避免陈旧数字
    while (QLayoutItem* it = m_kpiLay->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }

    const int signalN = m_report.signalTraces.size();
    const int skipN = m_report.skipped.size();
    const int riskN = m_report.risks.size();
    int riskHit = 0;
    for (const DecisionReview::HoldingRisk& r : m_report.risks) if (r.triggered) ++riskHit;
    bool windowOpen = false;
    for (const DecisionReview::OutcomeDeviation& d : m_report.outcomes) if (!d.known) windowOpen = true;
    const bool sectorOk = m_report.sectors.available;

    m_kpiLay->addWidget(Comp::badge(QStringLiteral("信号 %1").arg(signalN), Comp::BadgeTone::Info), 0);
    m_kpiLay->addWidget(Comp::badge(QStringLiteral("未执行 %1").arg(skipN),
                                    skipN > 0 ? Comp::BadgeTone::Accent : Comp::BadgeTone::Neutral), 0);
    m_kpiLay->addWidget(Comp::badge(
        QStringLiteral("风险触发 %1/%2").arg(riskHit).arg(riskN),
        riskHit > 0 ? Comp::BadgeTone::Danger : Comp::BadgeTone::Success), 0);
    m_kpiLay->addWidget(Comp::badge(windowOpen ? QStringLiteral("偏差窗口 未闭合")
                                               : QStringLiteral("偏差窗口 已闭合"),
                                     windowOpen ? Comp::BadgeTone::Warning
                                                : Comp::BadgeTone::Success), 0);
    m_kpiLay->addWidget(Comp::badge(
        sectorOk ? QStringLiteral("板块数据 可用") : QStringLiteral("板块数据 缺失"),
        sectorOk ? Comp::BadgeTone::Success : Comp::BadgeTone::Warning), 0);

    int gapN = m_report.gaps.size();
    if (!m_journalError.isEmpty()) ++gapN;
    // 缺口徽章可点击：直达「信号溯源」页的判定条件区核对明细，而不是让用户找
    auto* gapBtn = new QPushButton(m_kpiRow);
    gapBtn->setCursor(Qt::PointingHandCursor);
    gapBtn->setFlat(true);
    gapBtn->setText(gapN > 0 ? QStringLiteral("数据缺口 %1 ▸").arg(gapN)
                             : QStringLiteral("数据齐全"));
    gapBtn->setStyleSheet(Theme::chipStyle(
        gapN > 0 ? Theme::warning() : Theme::success(),
        gapN > 0 ? Theme::warningSoft() : Theme::successSoft(),
        Theme::fontXs(), 4, 8, true));
    gapBtn->setToolTip(m_journalError.isEmpty()
        ? QStringLiteral("点击查看本轮全部数据缺口与判定条件（信号溯源页底部）")
        : QStringLiteral("交易日志后端异常：%1 —— 点击查看明细").arg(m_journalError));
    connect(gapBtn, &QPushButton::clicked, this, [this]() { m_tabs->setCurrentIndex(0); });
    m_kpiLay->addWidget(gapBtn, 0);
}

// ---------------------------------------------------------------- 筛选定位
QTableWidget* ReviewPanel::activeTable() const
{
    if (!m_tabs) return nullptr;
    switch (m_tabs->currentIndex()) {
        case 0: return m_signalTable;
        case 1: return m_outcomeTable;
        case 2: return m_skippedTable;
        case 3: return m_riskTable;
        case 4: return m_sectorTable;
        default: return nullptr;
    }
}

void ReviewPanel::applyFilter()
{
    QTableWidget* t = activeTable();
    if (!t) return;
    const QString kw = m_filterBox ? m_filterBox->text().trimmed() : QString();
    for (int r = 0; r < t->rowCount(); ++r) {
        if (kw.isEmpty()) { t->setRowHidden(r, false); continue; }
        bool hit = false;
        for (int c = 0; c < t->columnCount() && !hit; ++c) {
            if (const QTableWidgetItem* it = t->item(r, c))
                hit = it->text().contains(kw, Qt::CaseInsensitive);
        }
        t->setRowHidden(r, !hit);
    }
}

// ---------------------------------------------------------------- 条件证据 HTML
QString ReviewPanel::evidenceHtml(const QVector<DecisionReview::Evidence>& list) const
{
    if (list.isEmpty()) return htmlWrap(QStringLiteral("<p>无可核对的条件。</p>"));
    QString h = QStringLiteral(
        "<table style=\"width:100%;border-collapse:collapse;font-size:12px\">"
        "<tr><th style=\"%1\">条件</th><th style=\"%1\">角色</th><th style=\"%1\">实测值</th>"
        "<th style=\"%1\">判定式 / 阈值</th><th style=\"%1\">结论</th>"
        "<th style=\"%1\">来源与窗口</th><th style=\"%1\">口径与限制</th></tr>")
        .arg(QString::fromLatin1(kTh));
    for (const DecisionReview::Evidence& e : list) {
        QString conclusion = QStringLiteral("数据缺失");
        QColor c = Theme::neutral();
        if (e.known) {
            conclusion = e.passed ? QStringLiteral("成立") : QStringLiteral("不成立");
            c = e.passed ? Theme::success() : Theme::neutral();
        } else {
            c = Theme::warning();
        }
        h += QStringLiteral(
            "<tr><td style=\"%1\">%2</td><td style=\"%1\">%3</td><td style=\"%1\">%4</td>"
            "<td style=\"%1\">%5</td><td style=\"%1;color:%6;font-weight:600\">%7</td>"
            "<td style=\"%1\">%8</td><td style=\"%1\">%9</td></tr>")
            .arg(QString::fromLatin1(kTd), esc(e.name), esc(e.role), esc(e.value),
                 esc(e.rule), c.name(), esc(conclusion), esc(e.source), esc(e.caveat));
    }
    return htmlWrap(h + QStringLiteral("</table>"));
}

// 空态三段式：结论 + 原因 + 下一步动作，让用户知道「接下来做什么」
void ReviewPanel::showEmptyDetail(QTextBrowser* view, const QString& title,
                                  const QString& reason, const QString& action)
{
    QString h = QStringLiteral("<h3 style=\"margin:4px 0\">%1</h3><p>%2</p>")
                    .arg(esc(title), esc(reason));
    if (!action.isEmpty())
        h += QStringLiteral("<p style=\"color:%1;font-weight:600\">下一步：%2</p>")
                 .arg(Theme::accent().name(), esc(action));
    view->setHtml(htmlWrap(h));
}

void ReviewPanel::renderCriteria(QTextBrowser* view)
{
    if (!view) return;
    QString h = QStringLiteral("<h3 style=\"margin:4px 0\">使用边界</h3><p>%1</p>")
                    .arg(esc(m_report.purpose));
    if (m_report.signalTraces.isEmpty())
        h += QStringLiteral("<p style=\"color:%1\">该交易日没有信号可核对：没有 recommend_plan 留档，"
                            "也没有持仓离场条件。不会用今日行情补造当时的信号。</p>")
                 .arg(Theme::warning().name());
    h += QStringLiteral("<h3 style=\"margin:10px 0 4px\">判定条件（与计算共用同一份常量）</h3><ol>");
    for (const QString& c : m_report.criteria) h += QStringLiteral("<li>%1</li>").arg(esc(c));
    h += QStringLiteral("</ol><h3 style=\"margin:10px 0 4px\">本轮数据缺口</h3><ul>");
    if (m_report.gaps.isEmpty() && m_journalError.isEmpty()) h += QStringLiteral("<li>无</li>");
    for (const QString& g : m_report.gaps) h += QStringLiteral("<li>%1</li>").arg(esc(g));
    if (!m_journalError.isEmpty())
        h += QStringLiteral("<li style=\"color:%1\">交易日志后端异常：%2 —— 执行 / 跳过 / 持仓"
                            "判定可能不完整，恢复后点「重算」</li>")
                 .arg(Theme::danger().name(), esc(m_journalError));
    h += QStringLiteral("</ul>");
    view->setHtml(htmlWrap(h));
}

// ---------------------------------------------------------------- ① 信号溯源
void ReviewPanel::renderSignals()
{
    const QVector<DecisionReview::SignalTrace>& sigs = m_report.signalTraces;
    {
        QSignalBlocker block(m_signalTable);
        m_signalTable->setRowCount(sigs.size());
        for (int i = 0; i < sigs.size(); ++i) {
            const DecisionReview::SignalTrace& s = sigs.at(i);
            put(m_signalTable, i, 0, dateText(s.date));
            put(m_signalTable, i, 1, s.code);
            put(m_signalTable, i, 2, s.name);
            put(m_signalTable, i, 3, s.side);
            put(m_signalTable, i, 4, s.level);
            putNum(m_signalTable, i, 5,
                QStringLiteral("%1 / %2 / %3").arg(s.passedCount).arg(s.knownCount)
                    .arg(s.conditions.size()));
            put(m_signalTable, i, 6, s.complete ? QStringLiteral("齐全") : QStringLiteral("有缺口"),
                s.complete ? Theme::success() : Theme::warning(), s.verdict);
            put(m_signalTable, i, 7, brief(s.expectation, 46), QColor(), s.expectation);
        }
        m_signalTable->resizeColumnsToContents();
        applyFilter();
    }
    setTabCount(0, QString::number(sigs.size()));
    // 条件明细不再单独列表：判定式 / 阈值 / 来源窗口集中在下方判定条件区核对
    renderCriteria(m_criteriaView);
}

// ---------------------------------------------------------------- ② 预期 vs 实际
void ReviewPanel::renderOutcomes()
{
    const QVector<DecisionReview::OutcomeDeviation>& list = m_report.outcomes;
    {
        QSignalBlocker block(m_outcomeTable);
        m_outcomeTable->setRowCount(list.size());
        for (int i = 0; i < list.size(); ++i) {
            const DecisionReview::OutcomeDeviation& d = list.at(i);
            put(m_outcomeTable, i, 0, d.code);
            put(m_outcomeTable, i, 1, d.name);
            putNum(m_outcomeTable, i, 2, d.known ? num(d.basePrice) : QStringLiteral("数据缺失"));
            putNum(m_outcomeTable, i, 3, d.targetPrice > 0.0 ? num(d.targetPrice) : QStringLiteral("—"));
            putNum(m_outcomeTable, i, 4, d.stopPrice > 0.0 ? num(d.stopPrice) : QStringLiteral("—"));
            putNum(m_outcomeTable, i, 5, d.known
                ? QStringLiteral("%1 / %2").arg(num(d.maxHigh), pct(d.mfePct))
                : QStringLiteral("数据缺失"));
            putNum(m_outcomeTable, i, 6, d.known
                ? QStringLiteral("%1 / %2").arg(num(d.minLow), pct(d.maePct))
                : QStringLiteral("数据缺失"));
            putNum(m_outcomeTable, i, 7, d.known
                ? QStringLiteral("%1 / %2").arg(num(d.lastClose), pct(d.closePct))
                : QStringLiteral("数据缺失"));
            putNum(m_outcomeTable, i, 8, d.known ? pct(d.targetDeviationPct) : QStringLiteral("数据缺失"),
                d.known ? (d.targetDeviationPct >= 0.0 ? Theme::up() : Theme::down()) : QColor());
            put(m_outcomeTable, i, 9, !d.known ? QStringLiteral("—")
                : d.targetTouched ? QStringLiteral("是") : QStringLiteral("否"));
            put(m_outcomeTable, i, 10, !d.known ? QStringLiteral("—")
                : d.stopTouched ? QStringLiteral("是") : QStringLiteral("否"),
                d.stopTouched ? Theme::danger() : QColor());
            put(m_outcomeTable, i, 11, d.known
                ? QStringLiteral("%1 根（至 %2）").arg(d.bars).arg(dateText(d.lastBarDate))
                : QStringLiteral("窗口未闭合"), QColor(), d.dataNote);
            put(m_outcomeTable, i, 12, brief(d.summary, 50), QColor(), d.summary);
        }
        m_outcomeTable->resizeColumnsToContents();
        applyFilter();
    }
    setTabCount(1, QString::number(list.size()));
    if (list.isEmpty()) {
        showEmptyDetail(m_outcomeDetail,
            QStringLiteral("暂无可核对的偏差"),
            QStringLiteral("该交易日没有信号留档。事后偏差只按计划里已写明的止盈 / 止损价格计算，"
                           "不引入新的目标价，也不会用估算值填充。"),
            QStringLiteral("切换到有留档的交易日；或在首页完成一次分析后回来点「重算」"));
    } else {
        m_outcomeTable->setCurrentCell(0, 0);
        renderOutcomeDetail(0);
    }
}

void ReviewPanel::renderOutcomeDetail(int row)
{
    const QVector<DecisionReview::OutcomeDeviation>& list = m_report.outcomes;
    if (row < 0 || row >= list.size()) return;
    const DecisionReview::OutcomeDeviation& d = list.at(row);
    QString h = QStringLiteral("<h3 style=\"margin:4px 0\">%1 %2 · 预期 vs 实际</h3><p>%3</p>")
                    .arg(esc(d.code), esc(d.name), esc(d.summary));
    if (!d.dataNote.isEmpty())
        h += QStringLiteral("<p style=\"color:%1\">数据说明：%2</p>")
                 .arg(Theme::warning().name(), esc(d.dataNote));
    h += QStringLiteral("<h4 style=\"margin:8px 0 2px\">逐条核对</h4>");
    m_outcomeDetail->setHtml(htmlWrap(h) + evidenceHtml(d.checks));
}

// ---------------------------------------------------------------- ③ 跳过的机会
void ReviewPanel::renderSkipped()
{
    const QVector<DecisionReview::SkippedOpportunity>& list = m_report.skipped;
    {
        QSignalBlocker block(m_skippedTable);
        m_skippedTable->setRowCount(list.size());
        for (int i = 0; i < list.size(); ++i) {
            const DecisionReview::SkippedOpportunity& s = list.at(i);
            put(m_skippedTable, i, 0, dateText(s.signalDate));
            put(m_skippedTable, i, 1, s.code);
            put(m_skippedTable, i, 2, s.name);
            putNum(m_skippedTable, i, 3, num(s.buyMid));
            put(m_skippedTable, i, 4, brief(s.skipRule, 40), QColor(), s.skipRule);
            putNum(m_skippedTable, i, 5, s.known
                ? QStringLiteral("%1 / %2").arg(num(s.maxHigh), pct(s.mfePct))
                : QStringLiteral("数据缺失"));
            putNum(m_skippedTable, i, 6, s.known
                ? QStringLiteral("%1 / %2").arg(num(s.minLow), pct(s.maePct))
                : QStringLiteral("数据缺失"));
            putNum(m_skippedTable, i, 7, s.known
                ? QStringLiteral("%1 / %2").arg(num(s.lastClose), pct(s.closePct))
                : QStringLiteral("数据缺失"));
            putNum(m_skippedTable, i, 8, s.known ? pct(s.hypotheticalPct) : QStringLiteral("—"),
                s.known ? (s.hypotheticalPct >= 0.0 ? Theme::up() : Theme::down()) : QColor(),
                s.basis);
            put(m_skippedTable, i, 9, !s.known ? QStringLiteral("—")
                : s.targetTouched ? QStringLiteral("是") : QStringLiteral("否"));
            put(m_skippedTable, i, 10, !s.known ? QStringLiteral("—")
                : s.stopTouched ? QStringLiteral("是") : QStringLiteral("否"),
                s.stopTouched ? Theme::warning() : QColor());
            put(m_skippedTable, i, 11, s.known
                ? QStringLiteral("%1 根（至 %2）").arg(s.bars).arg(dateText(s.lastBarDate))
                : QStringLiteral("窗口未闭合"));
            put(m_skippedTable, i, 12, brief(s.dataNote.isEmpty()
                    ? QStringLiteral("窗口已闭合") : s.dataNote, 44), QColor(),
                s.dataNote);
        }
        m_skippedTable->resizeColumnsToContents();
        applyFilter();
    }
    setTabCount(2, QString::number(list.size()));
    if (list.isEmpty()) {
        showEmptyDetail(m_skippedDetail,
            QStringLiteral("没有「被跳过的机会」"),
            QStringLiteral("判定条件：当日可执行信号在交易日志里找不到同代码、"
                           "买入日落在信号日或次日的记录。当前要么信号都已执行，要么当日无可执行信号。"),
            QStringLiteral("当日信号都已执行就无需处理；若交易日志确实漏记，去「交易日志」页补录后回来重算"));
    } else {
        m_skippedTable->setCurrentCell(0, 0);
        renderSkippedDetail(0);
    }
}

void ReviewPanel::renderSkippedDetail(int row)
{
    const QVector<DecisionReview::SkippedOpportunity>& list = m_report.skipped;
    if (row < 0 || row >= list.size()) return;
    const DecisionReview::SkippedOpportunity& s = list.at(row);
    QString h = QStringLiteral(
        "<h3 style=\"margin:4px 0\">%1 %2 · 事后统计</h3>"
        "<p style=\"color:%3;font-weight:600\">%4</p>"
        "<p>未执行判定：%5</p><p>假设收益口径：%6</p>")
        .arg(esc(s.code), esc(s.name), Theme::warning().name(), esc(s.caveat),
             esc(s.skipRule), esc(s.basis));
    if (s.known) {
        h += QStringLiteral(
            "<p>后续 %1 根日 K（至 %2）：最高 %3（%4）/ 最低 %5（%6）/ 末收 %7（%8）；"
            "机械假设收益 %9；触及止盈 %10，触及止损 %11。</p>")
            .arg(s.bars).arg(dateText(s.lastBarDate))
            .arg(num(s.maxHigh), pct(s.mfePct), num(s.minLow), pct(s.maePct),
                 num(s.lastClose), pct(s.closePct), pct(s.hypotheticalPct))
            .arg(s.targetTouched ? QStringLiteral("是") : QStringLiteral("否"),
                 s.stopTouched ? QStringLiteral("是") : QStringLiteral("否"));
    }
    if (!s.dataNote.isEmpty())
        h += QStringLiteral("<p style=\"color:%1\">%2</p>")
                 .arg(Theme::warning().name(), esc(s.dataNote));
    m_skippedDetail->setHtml(htmlWrap(h));
}

// ---------------------------------------------------------------- ④ 持仓隐性风险
void ReviewPanel::renderRisks()
{
    const QVector<DecisionReview::HoldingRisk>& list = m_report.risks;
    {
        QSignalBlocker block(m_riskTable);
        m_riskTable->setRowCount(list.size());
        for (int i = 0; i < list.size(); ++i) {
            const DecisionReview::HoldingRisk& r = list.at(i);
            const DecisionReview::Evidence first =
                r.conditions.isEmpty() ? DecisionReview::Evidence() : r.conditions.first();
            QStringList rules;
            for (const DecisionReview::Evidence& e : r.conditions) rules << e.rule;
            put(m_riskTable, i, 0, r.code);
            put(m_riskTable, i, 1, r.name);
            put(m_riskTable, i, 2, r.type);
            put(m_riskTable, i, 3, r.level,
                r.triggered ? Theme::danger()
                            : (r.known ? Theme::success() : Theme::warning()));
            put(m_riskTable, i, 4, brief(first.value, 40), QColor(), first.value);
            put(m_riskTable, i, 5, brief(rules.join(QStringLiteral(" 且 ")), 48), QColor(),
                rules.join(QStringLiteral("\n")));
            put(m_riskTable, i, 6, brief(first.source, 36), QColor(), first.source);
            put(m_riskTable, i, 7, brief(r.conclusion, 52), QColor(), r.conclusion);
        }
        m_riskTable->resizeColumnsToContents();
        applyFilter();
    }
    int hit = 0;
    for (const DecisionReview::HoldingRisk& r : list) if (r.triggered) ++hit;
    setTabCount(3, QStringLiteral("触发 %1/%2").arg(hit).arg(list.size()));
    if (list.isEmpty()) {
        showEmptyDetail(m_riskDetail,
            QStringLiteral("当前无持仓，或交易日志未就绪"),
            QStringLiteral("隐性风险只对持仓中的标的计算：RSI 极端超买 / 低点下移假企稳 / 上方多层阻力，"
                           "每项都要给出实测值与判定式。没有日 K 时标「数据不足」，不用均线或常识补位。"),
            QStringLiteral("在「交易日志」页录入买入后，回到本页点「重算」即可看到风险标记"));
    } else {
        m_riskTable->setCurrentCell(0, 0);
        renderRiskDetail(0);
    }
}

void ReviewPanel::renderRiskDetail(int row)
{
    const QVector<DecisionReview::HoldingRisk>& list = m_report.risks;
    if (row < 0 || row >= list.size()) return;
    const DecisionReview::HoldingRisk& r = list.at(row);
    QString h = QStringLiteral("<h3 style=\"margin:4px 0\">%1 %2 · %3（%4）</h3><p>%5</p>")
                    .arg(esc(r.code), esc(r.name), esc(r.type), esc(r.level), esc(r.conclusion));
    m_riskDetail->setHtml(htmlWrap(h) + evidenceHtml(r.conditions));
}

// ---------------------------------------------------------------- ⑤ 板块轮动数据
void ReviewPanel::renderSectors()
{
    const DecisionReview::SectorRotation& s = m_report.sectors;
    {
        QSignalBlocker block(m_sectorTable);
        m_sectorTable->setRowCount(s.rows.size());
        for (int i = 0; i < s.rows.size(); ++i) {
            const DecisionReview::SectorFlowRow& r = s.rows.at(i);
            put(m_sectorTable, i, 0, r.tag,
                r.tag == QStringLiteral("领涨") ? Theme::up()
                    : (r.tag == QStringLiteral("领跌") ? Theme::down() : QColor()));
            put(m_sectorTable, i, 1, QStringLiteral("%1（%2）").arg(r.name, r.code));
            putNum(m_sectorTable, i, 2, pct(r.changePct), Theme::byChange(r.changePct));
            putNum(m_sectorTable, i, 3, r.flowKnown ? num(r.mainNetYi) : QStringLiteral("数据缺失"),
                r.flowKnown ? Theme::byChange(r.mainNetYi) : QColor());
            putNum(m_sectorTable, i, 4, r.flowKnown ? pct(r.mainPct) : QStringLiteral("数据缺失"));
            put(m_sectorTable, i, 5, r.flowKnown ? QStringLiteral("可用") : QStringLiteral("字段缺失"),
                r.flowKnown ? Theme::success() : Theme::warning());
            put(m_sectorTable, i, 6, brief(s.source, 40), QColor(), s.source);
        }
        m_sectorTable->resizeColumnsToContents();
        applyFilter();
    }
    setTabCount(4, QString::number(s.rows.size()));

    // 数据状态与重试入口就近放在页签里：缺数据时不用去顶栏找按钮
    if (s.available) {
        m_sectorStatus->setText(QStringLiteral("数据可用 · %1 个板块 · 抓取时间 %2")
            .arg(s.rows.size())
            .arg(s.fetchedAt.isValid()
                     ? s.fetchedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                     : QStringLiteral("—")));
        m_sectorStatus->setStyleSheet(Theme::textStyle(Theme::success(), Theme::fontXs(), false));
    } else {
        m_sectorStatus->setText(QStringLiteral("数据缺失：%1").arg(s.reason));
        m_sectorStatus->setStyleSheet(Theme::textStyle(Theme::warning(), Theme::fontXs(), false));
    }

    QString h = QStringLiteral("<h3 style=\"margin:4px 0\">当日板块轮动 · 客观数据</h3>");
    if (!s.available) {
        h += QStringLiteral("<p style=\"color:%1;font-weight:600\">%2</p>")
                 .arg(Theme::warning().name(), esc(s.reason));
        h += QStringLiteral("<p>不以题材涨幅反推资金流，也不推断「题材能持续几天」；"
                            "点上方「重新拉取板块数据」可重试，失败原因会留档显示。</p>");
    }
    h += QStringLiteral("<p>字段口径：%1</p><p>排序口径：%2</p>")
             .arg(esc(s.source), esc(s.criteria));
    m_sectorDetail->setHtml(htmlWrap(h));
}

// ---------------------------------------------------------------- 辅助页签（原有闭环）
void ReviewPanel::renderLegacy()
{
    if (!m_assistant) return;   // 状态行保留 rebuild 的结果，不在这里覆盖

    const QString review = m_assistant->latestReviewHtml();
    m_reviewView->setHtml(review.isEmpty()
        ? htmlWrap(QStringLiteral("<p>今日市场复盘尚未生成。15:00 后自动生成；"
                                  "也可点右上「立即复盘」按当前数据重算。</p>"))
        : review);
    const QString brief = m_assistant->latestForecastHtml();
    m_briefView->setHtml(brief.isEmpty()
        ? htmlWrap(QStringLiteral("<p>今日早盘简报尚未生成。交易日 09:25 后自动生成；"
                                  "该辅助页只做市场预判，不参与决策复盘结论。</p>"))
        : brief);

    const QVector<ForecastEval> evals = m_assistant->evals(30);
    m_evalTable->setRowCount(evals.size());
    for (int i = 0; i < evals.size(); ++i) {
        const ForecastEval& e = evals.at(i);
        put(m_evalTable, i, 0, e.forDate.toString(QStringLiteral("MM-dd")));
        putNum(m_evalTable, i, 1, QString::number(e.score));
        put(m_evalTable, i, 2, e.hit ? QStringLiteral("命中") : QStringLiteral("偏差"),
            e.hit ? Theme::success() : Theme::warning());
        putNum(m_evalTable, i, 3, QString::number(e.actualLimitUp));
        putNum(m_evalTable, i, 4, QStringLiteral("%1 板").arg(e.actualMaxBoard));
        put(m_evalTable, i, 5, e.deviations.isEmpty()
            ? QStringLiteral("-") : e.deviations.first().left(48));
    }
    m_evalTable->resizeColumnsToContents();

    const QVector<AssistantRule> rules = m_assistant->rules();
    m_ruleTable->setRowCount(rules.size());
    for (int i = 0; i < rules.size(); ++i) {
        const AssistantRule& r = rules.at(i);
        put(m_ruleTable, i, 0, QStringLiteral("%1（%2）").arg(r.name, r.condition));
        putNum(m_ruleTable, i, 1, QString::number(r.total));
        putNum(m_ruleTable, i, 2, QString::number(r.hits));
        putNum(m_ruleTable, i, 3, r.total > 0
            ? QStringLiteral("%1%").arg(r.accuracy() * 100, 0, 'f', 0) : QStringLiteral("—"));
        putNum(m_ruleTable, i, 4, QString::number(r.weight, 'f', 2));
        put(m_ruleTable, i, 5, r.active ? QStringLiteral("启用") : QStringLiteral("已停用"),
            r.active ? Theme::success() : Theme::danger());
    }
    m_ruleTable->resizeColumnsToContents();

    const auto strategies = m_assistant->strategies();
    if (strategies.isEmpty()) {
        m_strategyLabel->setText(QStringLiteral("有效策略库：暂无（市场预判命中后自动沉淀）"));
    } else {
        QStringList parts;
        for (const auto& s : strategies) {
            if (s.second.second <= 0) continue;
            parts << QStringLiteral("%1 %2/%3（%4%）")
                         .arg(s.first).arg(s.second.first).arg(s.second.second)
                         .arg(s.second.first * 100.0 / s.second.second, 0, 'f', 0);
        }
        m_strategyLabel->setText(QStringLiteral("有效策略库：%1")
            .arg(parts.isEmpty() ? QStringLiteral("暂无") : parts.join(QStringLiteral(" ｜ "))));
    }

    const QStringList lessons = m_assistant->lessons(50);
    m_lessonList->clear();
    if (lessons.isEmpty())
        m_lessonList->addItem(QStringLiteral("暂无记录（市场预判偏差会在这里留下归因）"));
    else
        for (const QString& l : lessons) m_lessonList->addItem(l);
}

