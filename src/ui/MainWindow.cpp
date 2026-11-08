#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include "ui/Components.h"
#include "ui/DisclaimerDialog.h"         // 「免责声明」查阅入口（只读，不重复写同意记录）
#include "ui/AiStockPanel.h"
#include "ui/ChartPanel.h"
#include "ui/JournalPanel.h"
#include "ui/ReviewPanel.h"
#include "data/DataHub.h"                // 个股资金流详情缓存（复盘信号溯源的事实来源）
#include "sentiment/LongSignalEngine.h"
#include "notify/PushService.h"
#include "review/ReviewAssistant.h"
#include "core/TradingCalendar.h"
#include "strategy/FactorConfig.h"
#include "notify/CredentialStore.h"
#if DABAN_ENABLE_MYSQL_JOURNAL
#include "storage/MySqlStore.h"
#endif
#include <QVBoxLayout>
#include <QTime>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include <QScrollArea>
#include <QStatusBar>
#include <QSplitter>
#include <QScreen>
#include <QShortcut>
#include <QResizeEvent>
#include <QStyle>
#include <QDateTime>
#include <QApplication>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDialog>
#include <algorithm>                 // 按卖出日排序，统计最近连亏
#include <QTimer>
#include <QDebug>
#include <cstdio>

namespace {

// 交易日志 MySQL 连接配置的持久化键（存本地 SQLite 的 app_setting）
constexpr const char* kMySqlCfgKey    = "journal.mysql.config";
// 一次性搬迁标记：本地历史流水是否已并入 MySQL
constexpr const char* kMySqlMigratedKey = "journal.mysql.migrated";

#if DABAN_ENABLE_MYSQL_JOURNAL
// 读取交易日志的 MySQL 连接配置。
// 密码用 DPAPI 保护后落盘；没有配置时使用部署默认值（root / 230374）。
MySqlStore::Config loadMySqlConfig(const Database* db)
{
    MySqlStore::Config cfg;                  // 默认：127.0.0.1:3306 root/230374 daban_quant
    if (!db || !db->isOpen()) return cfg;
    const QString raw = db->getSetting(QString::fromLatin1(kMySqlCfgKey));
    if (raw.trimmed().isEmpty()) return cfg;
    const QJsonObject o = QJsonDocument::fromJson(raw.toUtf8()).object();
    if (o.isEmpty()) return cfg;
    cfg.host     = o.value("host").toString(cfg.host);
    cfg.port     = o.value("port").toInt(cfg.port);
    cfg.user     = o.value("user").toString(cfg.user);
    cfg.database = o.value("database").toString(cfg.database);
    // 历史明文也能读（CredentialStore 兼容），读到后再用 protect 重写为密文
    cfg.password = CredentialStore::unprotect(o.value("password").toString());
    if (cfg.password.isEmpty()) cfg.password = MySqlStore::Config().password;
    return cfg;
}

// 把连接配置写回 app_setting（密码 DPAPI 加密，不留明文）
void saveMySqlConfig(Database* db, const MySqlStore::Config& cfg)
{
    if (!db || !db->isOpen()) return;
    QJsonObject o;
    o["host"]     = cfg.host;
    o["port"]     = cfg.port;
    o["user"]     = cfg.user;
    o["database"] = cfg.database;
    o["password"] = CredentialStore::protect(cfg.password);
    db->setSetting(QString::fromLatin1(kMySqlCfgKey),
                   QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}
#endif // DABAN_ENABLE_MYSQL_JOURNAL

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("做多量化选股系统"));
    setMinimumSize(1040, 680);       // 下调最小尺寸，适配 1366×768 笔记本与高分屏缩放

    // 默认按可用屏幕的 94% / 92% 开窗，避免在高 DPI 或小屏上超出显示区域
    const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1600, 1000);
    resize(qMin(1560, int(avail.width() * 0.94)),
           qMin(940, int(avail.height() * 0.92)));

    m_data = new DataHub(this);
    m_db = new Database(this);
    m_regime = new RegimeEngine(this);
    m_pipeline = new FilterPipeline(this);
    m_recEngine = new RecommendEngine(this);

    buildUi();

    connect(m_data, &DataHub::statusMessage, this, &MainWindow::onDataStatus);
    connect(m_data, &DataHub::errorOccurred, this, &MainWindow::onDataError);
    connect(m_data, &DataHub::limitPoolUpdated, this, &MainWindow::onPoolsUpdated);
    connect(m_data, &DataHub::snapshotUpdated, this, &MainWindow::onSnapshotUpdated);
    connect(m_data, &DataHub::klineUpdated, this, &MainWindow::onKlineUpdated);
    connect(m_data, &DataHub::prevLimitPoolUpdated, this, &MainWindow::onPrevLimitPoolUpdated);
    // 卡片「看K线」：图表专用长历史 K 线就绪后再喂给弹窗里的 ChartPanel
    connect(m_data, &DataHub::chartKlineReady, this,
            [this](const QString& code, const KlineSeries& series) {
                // 同一份长历史 K 线同时喂给详情弹窗的缩略图（取最近 60 根）
                m_recPanel->setStockKline(code, series);
                renderChartKline(code, series);
            });
    connect(m_data, &DataHub::themesUpdated, this,
            [this](const QHash<QString, QStringList>&, const QHash<QString, double>&) {
                m_screenDirty = true;
                runScreen();     // 题材到位后重算板块效应与身位
            });

    connect(m_recPanel, &AiStockPanel::refreshRequested, this, &MainWindow::onRefreshRequested);
    connect(m_recPanel, &AiStockPanel::klineRequested, this, &MainWindow::openKline);
    // 卡片「查看详情」：按需拉取财报 / 资金流 / 机构持仓，结果回灌面板
    connect(m_recPanel, &AiStockPanel::detailDataRequested, this,
            [this](const QString& code, bool force) {
                m_data->refreshStockDetail(code, force);
                // 没有筛选日 K 时补一份图表长历史（30 分钟缓存），缩略图才画得出来
                if (!m_klines.contains(code)) m_data->refreshChartKline(code);
            });
    connect(m_data, &DataHub::stockDetailReady, this,
            [this](const QString& code, const StockDetailData& data) {
                m_recPanel->setStockDetail(code, data);
            });

    // 初始化
    m_data->init();
    m_currentDate = QDate::currentDate();

    if (!m_db->open()) {
        onDataError(QStringLiteral("数据库打开失败：%1（功能受限，但仍可正常选股）")
                    .arg(m_db->lastError()));
        qWarning().noquote() << QStringLiteral("[做多量化] 数据库打开失败：%1")
                                 .arg(m_db->lastError());
    } else {
        qInfo().noquote() << QStringLiteral("[做多量化] 数据库就绪：%1（schema v%2）")
                              .arg(m_db->path()).arg(m_db->schemaVersion());
    }
    TradingCalendar::instance().setDatabase(m_db);
    TradingCalendar::instance().load();      // 恢复已学习的休市日（仅 open 成功时生效）

    // 因子权重配置：GUI 与无头回测读取同一份 app_setting
    const bool dbReady = m_db->isOpen();
    const QString storedConfig = dbReady ? m_db->getSetting(QStringLiteral("factor.config")) : QString();
    if (!storedConfig.trimmed().isEmpty()) {
        m_factorCfg = FactorConfig::fromJsonText(storedConfig);
    } else if (dbReady) {
        // 兼容旧版多版本库：迁移一次，取当时启用版本（「均衡做多」优先）的权重
        const QString legacyVersions = m_db->getSetting(QStringLiteral("strategy.versions"));
        if (!legacyVersions.trimmed().isEmpty())
            m_factorCfg = FactorConfig::fromLegacyVersionsText(legacyVersions);
    }
    m_recEngine->setFactorConfig(m_factorCfg);
    persistFactorConfig();                    // 首次运行把「均衡做多」配置落库

    // 列可见性：跨会话恢复 / 保留（app_setting.ui.plan.columns）
    if (dbReady) {
        const QString cols = m_db->getSetting(QStringLiteral("ui.plan.columns"));
        if (!cols.isEmpty()) m_recPanel->restoreColumnVisibility(cols);
    }
    connect(m_recPanel, &AiStockPanel::columnVisibilityChanged, this, [this](const QString& enc) {
        if (m_db && m_db->isOpen())
            m_db->setSetting(QStringLiteral("ui.plan.columns"), enc);
    });

    qInfo().noquote() << QStringLiteral("[做多量化] 启用策略：%1 ｜ %2")
                          .arg(FactorConfig::strategyName(), m_factorCfg.summary());

    // 交易日志存储后端：MySQL 优先，连接不可用时回退本地 SQLite
    setupJournalStore();
    connect(m_recPanel, &AiStockPanel::recordTradeRequested, this,
            [this](const TradePlan& plan) {
                m_journalPanel->importPlan(plan);
            });
    connect(m_journalPanel, &JournalPanel::statusMessage, this, &MainWindow::onDataStatus);
    // 日志页「刷新行情」：用户主动拉一次全市场快照，持仓中盈亏随之更新
    connect(m_journalPanel, &JournalPanel::quoteRefreshRequested, this,
            [this]() { m_data->refreshSnapshot(); });

    // 消息推送：配置落 notify_config.json（凭据不入源码），去重指纹落 SQLite
    m_push = new PushService(this);
    m_push->setDatabase(m_db);
    m_push->loadConfig();
    connect(m_push, &PushService::log, this, [this](const QString& line) {
        statusBar()->showMessage(line, 6000);
    });
    connect(m_push, &PushService::finished, this, [this](bool ok, const QString& detail) {
        statusBar()->showMessage(QStringLiteral("消息推送：%1").arg(detail), 12000);
        if (!ok && m_pushManual) {
            QMessageBox::warning(this, QStringLiteral("消息推送失败"), detail);
        }
        m_pushManual = false;
    });

    // 复盘助手：收盘复盘 + 决策复盘（复用同一条 SMTP 通道）
    m_assistant = new ReviewAssistant(m_db, m_push, this);
    // 决策复盘的数据源：历史计划 / K 线留档、交易日志、内存日 K。
    // 页面自己按所选交易日组装事实，不反向依赖策略管线。
    m_reviewPanel->setContext(m_db, m_journalStore, &m_klines);
    m_reviewPanel->setAssistant(m_assistant);
    connect(m_assistant, &ReviewAssistant::log, this, [this](const QString& line) {
        statusBar()->showMessage(QStringLiteral("复盘助手：%1").arg(line), 12000);
    });
    auto* assistantTimer = new QTimer(this);
    assistantTimer->setInterval(30 * 1000);
    connect(assistantTimer, &QTimer::timeout, m_assistant, &ReviewAssistant::tick);
    assistantTimer->start();
    QTimer::singleShot(15000, m_assistant, &ReviewAssistant::tick);   // 启动补跑（错过时点也能补）

    // ★ 先把上次的荐股计划（本地缓存）铺到界面上：即使网络不通，点开也有内容可看
    loadCachedRecommend();

    updateStatusBar();
    // 启动时自动拉一次数据；结果到达后自动覆盖为最新荐股计划
    QTimer::singleShot(300, this, [this]() { requestPoolWithFallback(QDate::currentDate()); });

    // 盘中自动刷新（原来只在「实时监控」页手动开启，首页从早到晚不变）
    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(60 * 1000);
    connect(m_autoTimer, &QTimer::timeout, this, &MainWindow::onAutoRefresh);
    m_autoTimer->start();

    // 盘前提示：9:15 前当天涨停池本来就是空的，会自动回退到上一交易日
    const QTime now = QTime::currentTime();
    if (QDate::currentDate().dayOfWeek() <= Qt::Friday && now < QTime(9, 15)) {
        m_statusLabel->setText(QStringLiteral("盘前：涨停池尚未生成，当前展示上一交易日数据"));
    }
}

MainWindow::~MainWindow() = default;

QWidget* MainWindow::buildBrandBar()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("BrandBar"));   // 浅色顶栏：全局唯一真相
    // ★ 高度统一为 64：原先初始 72、响应式再改 64/52，首帧到 resize 会跳一次。
    bar->setFixedHeight(64);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(Theme::Space::Xl, Theme::Space::Sm, Theme::Space::Xl, Theme::Space::Sm);
    h->setSpacing(Theme::Space::Lg);

    auto* titleBox = new QWidget(bar);
    auto* tv = new QVBoxLayout(titleBox);
    tv->setContentsMargins(0, 0, 0, 0);
    tv->setSpacing(0);
    auto* title = new QLabel(QStringLiteral("做多量化选股系统"), titleBox);
    title->setObjectName(QStringLiteral("BrandTitle"));
    m_brandSub = new QLabel(QStringLiteral("均衡做多 · 多维度 · 做多信号"), titleBox);
    m_brandSub->setObjectName(QStringLiteral("BrandSub"));
    tv->addWidget(title);
    tv->addWidget(m_brandSub);
    h->addWidget(titleBox, 0);

    // ---- 品牌条读数：只保留当日执行口径（计划仓位合计 / 持仓只数）----
    // 复合情绪指数、有效总仓上限与闸门状态属于研究员口径，不再上顶栏；
    // 「今天能不能出手」由结论卡 / 结论条的文字结论表达。
    auto* usedBox = new QWidget(bar);
    auto* usedV = new QVBoxLayout(usedBox);
    usedV->setContentsMargins(0, 0, 0, 0);
    usedV->setSpacing(0);
    m_usedLabel = new QLabel(QStringLiteral("计划 — ｜ 持仓 —"), usedBox);
    m_usedLabel->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));
    // 说明性文字不该把窗口最小宽度顶到 1100+（否则 1366 屏放不下）。
    // 这里不用 QSizePolicy::Ignored —— 那会让标签在空间紧张时被压成 0 宽，
    // 文字溢出会互相重叠。正确做法是**把文案写短**（见 updateUsedPct）。
    m_usedLabel->setToolTip(QStringLiteral("当日计划仓位合计（可执行只数）与当前持仓只数"));
    usedV->addWidget(m_usedLabel);
    h->addWidget(usedBox, 0);

    // 视图只有一种「简洁」模式：品牌条不再放视图档位开关，
    // 研究型入口（因子权重 / 判定依据 / API Key / AI 分析 / 消息推送）已整体移除。

    h->addStretch(1);

    // 合规入口：已同意后仍可随时核对声明全文与本机同意记录。
    // 放在品牌条而不是「⋯ 更多」菜单里 —— 合规文档不应被折叠或置灰。
    auto* legalBtn = new QPushButton(QStringLiteral("免责声明"), bar);
    legalBtn->setObjectName(QStringLiteral("VersionAction"));
    legalBtn->setCursor(Qt::PointingHandCursor);
    legalBtn->setToolTip(QStringLiteral("查看《免责声明与风险揭示》全文与同意记录"));
    connect(legalBtn, &QPushButton::clicked, this,
            [this]() { DisclaimerDialog::showDocument(this); });
    h->addWidget(legalBtn, 0);

    m_brandStatus = new QLabel(QStringLiteral("行情加载中…"), bar);
    m_brandStatus->setObjectName(QStringLiteral("BrandStatus"));
    h->addWidget(m_brandStatus, 0);

    return bar;
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_brandBar = buildBrandBar();
    outer->addWidget(m_brandBar, 0);

    // 跨页常驻的今日结论条：无论停在哪个页面，打开就能看到「今天能不能做」。
    // 点击直达「AI选股结果」。
    // ★ 首页即「AI选股结果」，该页自带完整结论卡；此时把这条收起，避免同一句话占两处。
    auto* verdictBar = new QWidget(central);
    verdictBar->setObjectName(QStringLiteral("GlobalVerdictBar"));
    auto* verdictLay = new QHBoxLayout(verdictBar);
    verdictLay->setContentsMargins(0, 0, 0, 0);
    verdictLay->setSpacing(0);

    m_globalVerdict = new QPushButton(verdictBar);
    m_globalVerdict->setObjectName(QStringLiteral("GlobalVerdict"));
    m_globalVerdict->setCursor(Qt::PointingHandCursor);
    m_globalVerdict->setMinimumHeight(38);
    m_globalVerdict->setText(QStringLiteral("正在加载行情…"));
    m_globalVerdict->setToolTip(QStringLiteral("点此打开「AI选股结果」查看今日结论与选股卡片"));
    m_globalVerdict->setStyleSheet(QStringLiteral(
        "QPushButton#GlobalVerdict{border:0;border-bottom:1px solid %1;"
        "background:%2;color:%3;font-size:%4px;text-align:left;padding:0 20px;}")
        .arg(Theme::border().name(), Theme::bg().name(),
             Theme::textMuted().name()).arg(Theme::fontMd()));
    connect(m_globalVerdict, &QPushButton::clicked, this, [this]() { onNavChanged(0); });
    verdictLay->addWidget(m_globalVerdict, 1);

    // 数据状态角标：与结论同处一条，避免「数据状态」在多处重复出现
    m_globalFreshness = new QLabel(verdictBar);
    m_globalFreshness->setObjectName(QStringLiteral("Muted"));
    m_globalFreshness->setStyleSheet(
        Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false)
        + QStringLiteral("padding-right:20px;"));
    verdictLay->addWidget(m_globalFreshness, 0);
    outer->addWidget(verdictBar, 0);
    m_globalVerdictBar = verdictBar;
    // 首页即「今日计划」，该页自带完整结论卡：启动时先收起全局结论条
    verdictBar->setVisible(false);

    // 全局提示中心：4 类反馈固定在结论条下方，不再被状态栏的新消息冲掉
    m_feedback = new Comp::FeedbackCenter(central);
    outer->addWidget(m_feedback, 0);

    auto* body = new QWidget(central);
    auto* root = new QHBoxLayout(body);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    buildNavBar();
    outer->addWidget(m_navBar, 0);

    m_stack = new QStackedWidget(body);
    m_stack->setObjectName(QStringLiteral("MainStack"));   // 供 UI 冒烟测试定位当前页
    m_recPanel = new AiStockPanel(m_stack);
    m_journalPanel = new JournalPanel(m_stack);
    m_reviewPanel = new ReviewPanel(m_stack);

    // 首页「AI选股结果」：AI 结论 + 选股卡片，点卡片查看详情与 K 线。
    m_stack->addWidget(buildPage(m_recPanel, QStringLiteral("AI选股结果"),
        QStringLiteral("AI 结论 + 选股卡片，点卡片查看详情与 K 线")));
    m_stack->addWidget(buildPage(m_journalPanel, QStringLiteral("交易日志"),
        QStringLiteral("记录真实成交 → 持仓按现价算浮动盈亏；了结后计入已实现盈亏")));
    m_stack->addWidget(buildPage(m_reviewPanel, QStringLiteral("复盘助手"),
        QStringLiteral("15:00 自动复盘 → 次日 9:25 简报 → 收盘校验预判、归因偏差、迭代权重")));

    root->addWidget(m_stack, 1);
    outer->addWidget(body, 1);

    // 启动即挂上首页的主动作区（切页时会自动换）
    setPageActions(m_recPanel->pageActions());

    setCentralWidget(central);

    // 状态栏：只留进度 / 消息与数据源说明；
    // 情绪闸门与复合指数不再上状态栏（见 refreshRegime 的留档说明）。
    m_statusLabel = new QLabel(QStringLiteral("就绪"), this);
    statusBar()->addWidget(m_statusLabel, 1);
    auto* srcLabel = new QLabel(QStringLiteral("数据源：东财 / 腾讯 / 新浪（免密钥）"), this);
    srcLabel->setObjectName(QStringLiteral("Muted"));
    statusBar()->addPermanentWidget(srcLabel);
}

QWidget* MainWindow::buildPage(QWidget* content, const QString& title, const QString& desc)
{
    auto* page = new QWidget(m_stack);
    auto* lay = new QVBoxLayout(page);
    // 页面留白统一取 4px 栅格：左右 20、上 16、下 16
    lay->setContentsMargins(Theme::Space::Xl, Theme::Space::Lg, Theme::Space::Xl, Theme::Space::Lg);
    lay->setSpacing(Theme::Space::Md);

    auto* header = new QWidget(page);
    header->setObjectName(QStringLiteral("PageHeader"));
    auto* hv = new QVBoxLayout(header);
    hv->setContentsMargins(0, 0, 0, 0);
    hv->setSpacing(2);

    auto* t = new QLabel(title, header);
    t->setObjectName(QStringLiteral("PageTitle"));
    auto* d = new QLabel(desc, header);
    d->setObjectName(QStringLiteral("PageDesc"));
    // 页面说明可换行：窄屏下不会把一行长文顶出屏幕。
    // 文案本身控制在 40 字以内 —— 顶栏与页面说明一旦写长，
    // 窗口最小宽度就会被顶到 1100+，1366×768 的笔记本直接放不下。
    d->setWordWrap(true);
    hv->addWidget(t);
    hv->addWidget(d);

    lay->addWidget(header);
    lay->addWidget(content, 1);
    return page;
}

// 卡片「看K线」：复用一个弹窗，先铺本地缓存，再异步补长周期日 K
void MainWindow::openKline(const QString& code, const QString& name)
{
    if (code.isEmpty()) return;

    if (!m_klineDialog) {
        m_klineDialog = new QDialog(this);
        m_klineDialog->setObjectName(QStringLiteral("KlineDialog"));
        m_klineDialog->resize(1120, 720);
        auto* lay = new QVBoxLayout(m_klineDialog);
        lay->setContentsMargins(Theme::Space::Lg, Theme::Space::Lg,
                                Theme::Space::Lg, Theme::Space::Lg);
        lay->setSpacing(Theme::Space::Sm);

        m_klineStatus = new QLabel(m_klineDialog);
        m_klineStatus->setObjectName(QStringLiteral("Muted"));
        m_klineStatus->setWordWrap(true);
        lay->addWidget(m_klineStatus);

        m_klinePanel = new ChartPanel(m_klineDialog);
        lay->addWidget(m_klinePanel, 1);

        auto* close = new QPushButton(QStringLiteral("关闭"), m_klineDialog);
        close->setObjectName(QStringLiteral("Ghost"));
        close->setFixedWidth(96);
        connect(close, &QPushButton::clicked, m_klineDialog, &QDialog::accept);
        lay->addWidget(close, 0, Qt::AlignRight);
    }

    m_klineCode = code;
    m_klineName = name;
    m_klineDialog->setWindowTitle(QStringLiteral("K 线 · %1 %2").arg(code, name));

    const KlineSeries* cached = m_data ? m_data->chartKline(code) : nullptr;
    if (cached && cached->valid()) {
        renderChartKline(code, *cached);
    } else {
        m_klinePanel->clear();
        m_klineStatus->setText(
            QStringLiteral("正在拉取 %1 %2 的长周期日 K…（可在图上切换周 / 月与指标）")
                .arg(code, name));
    }

    m_klineDialog->show();
    m_klineDialog->raise();
    m_klineDialog->activateWindow();
    if (m_data) m_data->refreshChartKline(code);   // 命中 30 分钟缓存时即时回调
}

void MainWindow::renderChartKline(const QString& code, const KlineSeries& series)
{
    if (!m_klinePanel || code != m_klineCode) return;   // 用户已切到别的标的
    QVector<LimitEntry> marks;
    for (const LimitEntry& e : m_limitUp) {
        if (e.code == code) marks.append(e);
    }
    for (const LimitEntry& e : m_prevLimitUp) {
        if (e.code == code) marks.append(e);
    }
    m_klinePanel->showKline(code, m_klineName, series, marks);
    if (m_klineStatus) {
        m_klineStatus->setText(
            QStringLiteral("%1 %2 · 已载入 %3 根日 K（滚轮缩放、拖动平移、双击复位）")
                .arg(code, m_klineName).arg(series.candles.size()));
    }
}

// 顶部一级导航 + 上下文行动条。
// 为什么不再用左侧栏：
//   ① 横向空间是最紧的资源 —— 9 / 12 / 16 列的数据表在 1366 屏上本来就横向溢出，
//      侧栏再占 176px 只会让更多列跑到屏幕外；改成顶栏后这 176px 直接还给表格。
//   ② 释放出来的顶栏行正好承载「上下文行动条」—— 当前页的主动作固定在右上角，
//      不随页面滚动跑掉，也不用每页各发明一套工具条位置。
//   ③ 纵向扫视顺序变成「结论（品牌条）→ 导航 + 动作 → 内容」，符合从上到下的阅读习惯。
void MainWindow::buildNavBar()
{
    m_navBar = new QWidget(this);
    m_navBar->setObjectName(QStringLiteral("NavBar"));
    m_navBar->setFixedHeight(kNavBarHeight);

    auto* lay = new QHBoxLayout(m_navBar);
    lay->setContentsMargins(Theme::Space::Xl, 0, Theme::Space::Xl, 0);
    lay->setSpacing(Theme::Space::Xs);

    struct NavItem { const char* text; const char* tip; };
    // 顺序与 m_stack 的入栈顺序严格一致：0 AI选股结果（首页，三页合一）→ 1 交易日志 → 2 复盘助手
    const NavItem items[] = {
        { "AI选股结果", "AI 结论 / 选股卡片" },
        { "交易日志",   "胜率 / 溢价 / 回撤复盘" },
        { "复盘助手",   "收盘复盘 / 早盘简报 / 规则迭代" },
    };
    constexpr int kNavCount = int(sizeof(items) / sizeof(items[0]));

    for (int i = 0; i < kNavCount; ++i) {
        auto* b = new QPushButton(QString::fromUtf8(items[i].text), m_navBar);
        b->setObjectName(QStringLiteral("Nav"));    // 样式集中在 Theme：选中态 = 强调色 + 下划线
        b->setCheckable(true);
        b->setMinimumHeight(kNavBarHeight - 2);      // 命中区覆盖整条导航带
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, i]() { onNavChanged(i); });
        auto* sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), this);
        connect(sc, &QShortcut::activated, this, [this, i]() { onNavChanged(i); });
        b->setToolTip(QStringLiteral("%1（Ctrl+%2）").arg(QString::fromUtf8(items[i].tip)).arg(i + 1));
        lay->addWidget(b, 0);
        m_navButtons.append(b);
    }

    lay->addStretch(1);

    // 上下文行动条：当前页的主动作统一挂在这里（每页 ≤ 3 个）
    m_pageActionHost = new QWidget(m_navBar);
    m_pageActionHost->setObjectName(QStringLiteral("PageActionHost"));   // 供 UI 冒烟定位
    auto* hostLay = new QHBoxLayout(m_pageActionHost);
    hostLay->setContentsMargins(0, 0, 0, 0);
    hostLay->setSpacing(Theme::Space::Sm);
    lay->addWidget(m_pageActionHost, 0);

    m_navButtons.first()->setChecked(true);
    applyCompactMode(width() < kCompactWidth, width() < kNarrowWidth);
}

// 切换当前页的主动作区：旧行动区从布局里摘下并隐藏（不销毁，保留状态）。
void MainWindow::setPageActions(QWidget* actions)
{
    if (!m_pageActionHost) return;
    if (m_currentPageActions == actions) return;

    if (m_currentPageActions) {
        auto* lay = qobject_cast<QHBoxLayout*>(m_pageActionHost->layout());
        if (lay) lay->removeWidget(m_currentPageActions);
        m_currentPageActions->setVisible(false);
    }
    m_currentPageActions = actions;
    if (actions) {
        auto* lay = qobject_cast<QHBoxLayout*>(m_pageActionHost->layout());
        if (lay) lay->addWidget(actions);
        actions->setVisible(true);
    }
}

// 两级响应式：紧凑档收起次要读数与页面说明；窄档进一步精简品牌条
void MainWindow::applyCompactMode(bool compact, bool narrow)
{
    m_compact = compact;
    if (m_brandSub) m_brandSub->setVisible(!narrow);
    if (m_brandStatus) m_brandStatus->setVisible(!narrow);
    // 紧凑档收起「计划仓位 / 持仓」读数：顶栏宽度有限，
    // 与其把每一段都压扁（文字重叠），不如整段收起；完整读数在状态栏与计划页都有。
    if (m_usedLabel) m_usedLabel->setVisible(!compact);
    if (m_brandBar) m_brandBar->setFixedHeight(narrow ? 52 : 64);
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    applyCompactMode(width() < kCompactWidth, width() < kNarrowWidth);
}

void MainWindow::onNavChanged(int index)
{
    if (index < 0 || index >= m_navButtons.size() || !m_stack) return;
    for (int i = 0; i < m_navButtons.size(); ++i)
        m_navButtons[i]->setChecked(i == index);
    m_stack->setCurrentIndex(index);
    Theme::fadeIn(m_stack->currentWidget(), 140);     // 切页淡入，避免生硬跳变

    // 上下文行动条：切换到当前页的主动作区（每页 ≤ 3 个，固定在右上角）
    switch (index) {
        case 0: setPageActions(m_recPanel ? m_recPanel->pageActions() : nullptr); break;
        case 1: setPageActions(m_journalPanel ? m_journalPanel->pageActions() : nullptr); break;
        case 2: setPageActions(m_reviewPanel ? m_reviewPanel->pageActions() : nullptr); break;
        default: setPageActions(nullptr); break;
    }

    // 首页即「AI选股结果」：该页自带完整结论卡，全局结论条在此收起，
    // 避免同一句结论在屏幕上出现两次（其余页面仍然常驻）。
    if (m_globalVerdictBar) m_globalVerdictBar->setVisible(index != 0);

    // 交易日志页（1）：重新汇总 + 补注入最新现价（持仓中股票要显示浮动盈亏）
    if (index == 1 && m_journalPanel) {
        pushQuotesToJournal();
        m_journalPanel->refresh();
    }
    // 复盘助手页（2）：每次进入重新读库（可能有新的复盘/评估）
    if (index == 2 && m_reviewPanel) m_reviewPanel->refresh();
}

void MainWindow::onDataStatus(const QString& text)
{
    // 反馈语义化：普通进度用中性色，避免每条状态都抢视觉焦点
    m_statusLabel->setObjectName(QStringLiteral("Muted"));
    m_statusLabel->setText(text);
    m_statusLabel->style()->unpolish(m_statusLabel);   // 更换 objectName 后需重新应用样式
    m_statusLabel->style()->polish(m_statusLabel);
    // 同一条信息也进提示中心（进度态，不抢焦点）
    if (m_feedback) m_feedback->push(Comp::FeedbackCenter::Kind::Progress, text);
}

void MainWindow::onDataError(const QString& text)
{
    qWarning().noquote() << QStringLiteral("[做多量化] 数据错误：") << text;
    m_statusLabel->setObjectName(QStringLiteral("StatusErr"));  // 错误：红色 + 加粗（颜色与文字双通道）
    m_statusLabel->setText(QStringLiteral("⚠ ") + text);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
    // 错误进提示中心并**保留到被解决为止**（不再被下一条状态消息冲掉）
    if (m_feedback) m_feedback->push(Comp::FeedbackCenter::Kind::Error, text);
    m_recPanel->setError(text);
    updateStatusBar();
}

void MainWindow::onPoolsUpdated(const QDate& date, const QVector<LimitEntry>& up,
                                const QVector<LimitEntry>& broken, const QVector<LimitEntry>& down)
{
    // ★ 非交易日回退：当天涨停池为空（周末/节假日）时，向前找最近的有数据交易日，
    //   保证「点开就有荐股」，而不是一片空白。
    //   注意：仅对「今天」这类自动请求回退；用户手动指定日期时按其选择执行。
    const bool autoRequest = (m_fallbackDepth == 0);
    if (up.isEmpty() && autoRequest && m_fallbackDepth < 7) {
        // 工作日开盘后确认无行情：学习为休市日，避免节假日每 60 秒空跑轮询
        if (date == QDate::currentDate()
            && QTime::currentTime() >= QTime(9, 35)
            && TradingCalendar::instance().isTradingDay(date)) {
            TradingCalendar::instance().markNonTrading(date);
            qInfo().noquote() << QStringLiteral("[做多量化] %1 数据源无行情，已标记为非交易日")
                                  .arg(date.toString(Qt::ISODate));
        }
        requestPoolWithFallback(date.addDays(-1), m_fallbackDepth + 1);
        return;
    }

    m_limitUp = up;
    m_broken = broken;
    m_limitDown = down;
    m_currentDate = date;
    m_screenDirty = true;

    // ---- 盘中告警：炸板 / 新晋涨停（只在「今天」的数据上判断）----
    QSet<QString> nowZt;
    for (const LimitEntry& e : up) nowZt.insert(e.code);
    if (date == QDate::currentDate() && !m_lastZtCodes.isEmpty()) {
        QSet<QString> brokenNow;
        for (const LimitEntry& e : broken) brokenNow.insert(e.code);

        QStringList brokenOff;
        for (const QString& c : m_lastZtCodes) {
            if (!nowZt.contains(c) && brokenNow.contains(c)) brokenOff << c;
        }
        if (!brokenOff.isEmpty()) {
            raiseAlert(QStringLiteral("炸板 %1 只：%2")
                           .arg(brokenOff.size())
                           .arg(brokenOff.mid(0, 5).join(',')));
        }
    }
    if (date == QDate::currentDate()) m_lastZtCodes = nowZt;

    // 补昨日涨停池：晋级率 / 昨日涨停今日溢价这两个先行指标依赖它
    m_prevPoolDepth = 0;
    m_prevRequestDate = QDate();
    m_data->refreshPrevLimitPool(date.addDays(-1));

    // 概念题材（15 分钟缓存）：板块效应的主口径
    m_data->refreshThemes();

    refreshRegime();
    updateStatusBar();

    // 自动触发一次选股（数据到位即出结果）
    runScreen();
}

void MainWindow::onPrevLimitPoolUpdated(const QDate& date, const QVector<LimitEntry>& limitUp)
{
    // 节假日/周末：昨日池为空时继续向前找（最多 7 天）
    if (limitUp.isEmpty() && m_prevPoolDepth < 7) {
        ++m_prevPoolDepth;
        const QDate probe = date.addDays(-1);
        if (probe != m_prevRequestDate) {
            m_prevRequestDate = probe;
            m_data->refreshPrevLimitPool(probe);
        }
        return;
    }
    m_prevLimitUp = limitUp;
    refreshRegime();
    // 昨日数据到位后重算：情绪阶段可能从「主升」降级为「退潮」
    runScreen();
}

void MainWindow::requestPoolWithFallback(const QDate& date, int depth)
{
    m_fallbackDepth = depth;
    if (depth == 0) {
        // 首次请求：同时拉快照
        m_data->refreshAll(date);
    } else {
        m_data->refreshLimitPool(date);
    }
}

void MainWindow::onSnapshotUpdated(const QVector<Snapshot>& list)
{
    // 行情时间戳：交易日志用它标注「浮动盈亏是按几点几分算的」
    m_quoteStamp = QDateTime::currentDateTime().toString(QStringLiteral("MM-dd HH:mm"));
    Q_UNUSED(list);
    m_recPanel->setSnapshots(m_data->snapshots());   // 详情弹窗的市值 / 估值读数保持最新
    pushQuotesToJournal();
    if (m_screenDirty) runScreen();
    checkStopLossAlerts();
}

// ---------------------------------------------------------------- 交易日志存储
// 交易日志是唯一需要长期留档、可能跨设备查看的数据，因此优先落到 MySQL。
// 连接不可用时（服务未启动 / 密码不对 / 网络不通）自动回退本地 SQLite，
// 保证「记录买入」这类操作永远可用，不因为存储后端故障而丢数据。
void MainWindow::setupJournalStore()
{
#if DABAN_ENABLE_MYSQL_JOURNAL
    const MySqlStore::Config cfg = loadMySqlConfig(m_db);

    auto* mysql = new MySqlStore(cfg, this);
    if (mysql->open()) {
        m_mySqlStore   = mysql;
        m_journalStore = mysql;
        // 连接成功：把连接参数（含 DPAPI 加密后的密码）固化到 app_setting，
        // 之后不再以明文落盘
        saveMySqlConfig(m_db, cfg);
        qInfo().noquote() << QStringLiteral("[做多量化] 交易日志存储：%1").arg(mysql->describe());

        // 一次性搬迁：把升级前留在本地 SQLite 里的历史流水并入 MySQL
        if (m_db->isOpen()
            && m_db->getSetting(QString::fromLatin1(kMySqlMigratedKey)) != QStringLiteral("1")) {
            const QVector<TradeRecord> legacy = m_db->trades();
            if (!legacy.isEmpty()) {
                const int moved = mysql->importFrom(legacy);
                if (moved > 0)
                    qInfo().noquote() << QStringLiteral("[做多量化] 已迁移 %1 条本地交易流水到 MySQL")
                                             .arg(moved);
            }
            m_db->setSetting(QString::fromLatin1(kMySqlMigratedKey), QStringLiteral("1"));
        }
        onDataStatus(QStringLiteral("交易日志存储：%1").arg(mysql->describe()));
        m_journalPanel->setStore(m_journalStore);
        return;
    }

    const QString reason = mysql->lastError();
    qWarning().noquote() << QStringLiteral("[做多量化] MySQL 不可用（%1），交易日志回退本地 SQLite")
                                 .arg(reason);
    delete mysql;
    m_mySqlStore   = nullptr;
    m_journalStore = m_db;          // Database 同样实现 JournalStore，作为兜底后端
    onDataStatus(QStringLiteral("交易日志存储：本地 SQLite（MySQL 不可用：%1）").arg(reason));
#else
    // 未启用 MySQL 后端：只写本地 SQLite
    m_journalStore = m_db;
    onDataStatus(QStringLiteral("交易日志存储：本地 SQLite（未启用 MySQL 后端）"));
#endif

    m_journalPanel->setStore(m_journalStore);
}

void MainWindow::pushQuotesToJournal()
{
    if (!m_journalPanel) return;
    m_journalPanel->setSnapshots(m_data->snapshots(), m_quoteStamp);

    // AI 诊断需要日 K：优先用本次筛选已拉取的内存 K 线，其次读本地缓存；
    // 两者都没有时分析层自动降级（只用计划价 + 费用口径），不会阻塞日志页。
    // 只补齐日志涉及的代码，避免把整份筛选 K 线复制进日志页。
    QHash<QString, KlineSeries> klines = m_journalPanel->klines();
    for (const QString& code : m_journalPanel->codes()) {
        if (klines.contains(code)) continue;
        if (const KlineSeries* k = m_data->kline(code)) { klines.insert(code, *k); continue; }
        if (m_klines.contains(code)) { klines.insert(code, m_klines.value(code)); continue; }
        if (m_db && m_db->isOpen()) {
            const KlineSeries cached = m_db->loadKline(code);
            if (cached.valid()) klines.insert(code, cached);
        }
    }
    m_journalPanel->setKlines(klines);

    // 情绪阶段参与「回本修复周期」与止盈节奏的判断
    m_journalPanel->setMarketPhase(m_sentimentState.regime.phase,
                                   m_sentimentState.fact.valid());
}

void MainWindow::onAutoRefresh()
{
    if (!m_data->isTradingTime()) return;                 // 只在交易时段自动刷新
    if (m_currentDate != QDate::currentDate()) return;    // 看历史数据时不打扰

    ++m_autoTick;
    m_data->refreshLimitPool(m_currentDate);
    // 全市场快照 55 页，2 分钟一次足够（涨停池才是秒级关键）
    if (m_autoTick % 2 == 0) m_data->refreshSnapshot();
    checkReminders();                                     // ⑤ 到价 / 到时提醒
}

void MainWindow::raiseAlert(const QString& text)
{
    m_statusLabel->setText(QStringLiteral("⚠ ") + text);
    QApplication::beep();
    setWindowTitle(QStringLiteral("⚠ %1").arg(text.left(48)));
    QTimer::singleShot(8000, this, [this]() {
        setWindowTitle(QStringLiteral("做多量化选股系统"));
    });
}

void MainWindow::checkStopLossAlerts()
{
    if (!m_db || !m_db->isOpen()) return;
    const QVector<TradeRecord> open = m_db->trades();
    for (const TradeRecord& r : open) {
        if (r.closed() || r.planStop <= 0.0) continue;
        if (m_stopAlerted.contains(r.code)) continue;
        auto it = m_data->snapshots().constFind(r.code);
        if (it == m_data->snapshots().constEnd() || !it->isValid()) continue;
        if (it->price > 0.0 && it->price <= r.planStop) {
            m_stopAlerted.insert(r.code);
            raiseAlert(QStringLiteral("%1 %2 跌破计划止损 %3（现价 %4）—— 打板铁律：无条件走")
                           .arg(r.code, r.name)
                           .arg(r.planStop, 0, 'f', 2)
                           .arg(it->price, 0, 'f', 2));
        }
    }
}

// ⑤ 提醒只做提示，不产生任何下单动作
void MainWindow::fireReminder(const QString& id, const QString& text)
{
    if (m_firedReminders.contains(id)) return;
    m_firedReminders.append(id);
    raiseAlert(text);
}

void MainWindow::checkReminders()
{
    const QDate today = QDate::currentDate();
    if (m_reminderDate != today) {          // 跨交易日清空去重集合
        m_reminderDate = today;
        m_firedReminders.clear();
        m_priceAlerted.clear();
        m_stopAlerted.clear();
    }
    if (m_currentDate != today) return;     // 回看历史数据不提醒
    const QTime now = QTime::currentTime();
    const int mins = now.hour() * 60 + now.minute();

    // 时点类：过点提醒一次，盘后才启动不补发竞价提醒
    if (mins >= 9 * 60 + 25 && mins < 10 * 60
        && !m_firedReminders.contains(QStringLiteral("auction"))) {
        fireReminder(QStringLiteral("auction"),
            QStringLiteral("09:25 竞价已出：请到「计划明细」核对竞价判定，再决定是否接力"));
    }
    if (mins >= 14 * 60 + 45
        && !m_firedReminders.contains(QStringLiteral("close"))) {
        fireReminder(QStringLiteral("close"),
            QStringLiteral("14:45 收盘前核对：今日是否按计划执行？未成交的原因是什么？"));
    }

    // 价格类：只用当日实时结果，缓存 / 历史数据一律不触发
    const RecommendBundle& b = m_recEngine->lastBundle();
    if (!b.cached && b.tradeDate == today) {
        const QHash<QString, Snapshot>& snaps = m_data->snapshots();
        for (const TradePlan& p : b.plans) {
            auto it = snaps.constFind(p.code);
            if (it == snaps.constEnd() || !it->isValid() || it->price <= 0.0) continue;
            const QString buyKey  = p.code + QStringLiteral(":buy");
            const QString takeKey = p.code + QStringLiteral(":take");
            if (p.actionable && it->price >= p.buyLow && it->price <= p.buyHigh
                && !m_priceAlerted.contains(buyKey)) {
                m_priceAlerted.insert(buyKey);
                fireReminder(QStringLiteral("buy"),
                    QStringLiteral("%1 %2 现价 %3 已进入买入区间 %4 ~ %5")
                        .arg(p.code, p.name)
                        .arg(it->price, 0, 'f', 2)
                        .arg(p.buyLow, 0, 'f', 2)
                        .arg(p.buyHigh, 0, 'f', 2));
            }
            if (it->price >= p.takeProfitLow && !m_priceAlerted.contains(takeKey)) {
                m_priceAlerted.insert(takeKey);
                fireReminder(QStringLiteral("take"),
                    QStringLiteral("%1 %2 现价 %3 已触及止盈区间，按计划分批兑现")
                        .arg(p.code, p.name)
                        .arg(it->price, 0, 'f', 2));
            }
        }
    }

    // 止损告警沿用既有 checkStopLossAlerts，这里只把清单打勾
    if (!m_stopAlerted.isEmpty()
        && !m_firedReminders.contains(QStringLiteral("stop"))) {
        m_firedReminders.append(QStringLiteral("stop"));
    }
    // T+1 可卖：持仓的买入日早于今天
    if (m_journalStore) {
        for (const TradeRecord& r : m_journalStore->trades()) {
            if (r.closed() || !r.buyDate.isValid() || r.buyDate >= today) continue;
            fireReminder(QStringLiteral("t1"),
                QStringLiteral("%1 %2 已过 T+1，可按计划兑现").arg(r.code, r.name));
            break;
        }
    }
}

void MainWindow::onKlineUpdated(const QString& code, const KlineSeries& series)
{
    m_klines.insert(code, series);
    if (m_db->isOpen()) m_db->saveKline(series);
    m_recPanel->setStockKline(code, series);         // 详情弹窗的 K 线缩略图 / 均线 / 量能

    // 日志里正好有这只票：把新 K 线同步给 AI 诊断（均线 / ATR / 筹码压力会变）。
    // 在已注入的集合上增量合并，避免用筛选 K 线覆盖掉从本地缓存补进来的记录。
    if (m_journalPanel && m_journalPanel->codes().contains(code)) {
        QHash<QString, KlineSeries> ks = m_journalPanel->klines();
        ks.insert(code, series);
        m_journalPanel->setKlines(ks);
    }

    m_screenDirty = true;
    runScreen();
}

MarketRegime MainWindow::computeRegime() const
{
    // ---- 情绪维度主路径：六维复合指数 → 阶段 / 档位 / 有效仓位上限 ----
    // 输出仍是 MarketRegime，下游（筛选 / 荐股 / UI）零改动。
    const QVector<QPair<QDate, double>> history =
        (m_db && m_db->isOpen())
            ? m_db->sentimentHistory(m_currentDate, SentimentEngine::kHistoryDays)
            : QVector<QPair<QDate, double>>();

    m_sentimentState = SentimentEngine::run(
        m_currentDate, m_limitUp, m_broken, m_limitDown, m_prevLimitUp,
        m_data->snapshots(), m_data->themesOfStock(), history);
    return m_sentimentState.regime;
}

void MainWindow::refreshRegime()
{
    computeRegime();   // 结果存入 m_sentimentState / m_sentimentState.regime，供荐股与日志页使用

    // 状态栏不再展示「闸门放行 / 拦截 + 复合指数 + 仓位上限」：
    // 那是研究员口径的三个数字，普通执行场景读不懂也用不上。
    // 「今天能不能出手」由结论卡 / 结论条的白话结论与可执行数量表达。

    // 情绪留档：趋势/加速度依赖跨日序列，必须落库才能形成闭环。
    // 仅在当日涨停池已就绪时写入，避免盘前空池把「0 家涨停」永久记成冰点。
    if (m_sentimentState.fact.limitUpCount > 0 && m_db->isOpen()) {
        m_db->saveSentiment(m_sentimentState);
    }
}

void MainWindow::runScreen()
{
    if (m_limitUp.isEmpty()) {
        m_candidates.clear();
        updateRecommend();
        return;
    }

    // 情绪闸门（复合情绪指数 → MarketRegime）
    const MarketRegime reg = computeRegime();

    // 多因子评分输入：与回测共用 makeInputs / sliceTo(asOf) 口径
    m_recEngine->setFactorData(&m_data->snapshots(), &m_klines, m_currentDate);

    // 构建上下文
    FilterContext ctx;
    ctx.klines = m_klines;
    ctx.snapshots = m_data->snapshots();
    ctx.regime = reg;
    ctx.sectorCount = FilterPipeline::buildSectorCounts(m_limitUp);
    ctx.themesOfStock = m_data->themesOfStock();
    ctx.themeChangePct = m_data->themeChangePct();
    // 情绪域：阶段允许的连板高度窗口（选股范围的情绪维度）
    ctx.sentiment = &m_sentimentState;

    m_candidates = m_pipeline->run(m_limitUp, ctx);

    // 需要 K 线的候选：先拉 K 线，拉到后自动重跑
    QStringList needKline;
    for (const Candidate& c : m_candidates) {
        if (!m_klines.contains(c.entry.code)) needKline.append(c.entry.code);
    }
    // 观察名单（闸门拦截时的 pre-gate / near-miss）也需要 K 线来精确算止损位
    for (const Candidate& c : m_pipeline->lastPassedPreGate()) {
        if (!m_klines.contains(c.entry.code) && !needKline.contains(c.entry.code))
            needKline.append(c.entry.code);
    }
    for (const Candidate& c : m_pipeline->lastNearMiss()) {
        if (!m_klines.contains(c.entry.code) && !needKline.contains(c.entry.code))
            needKline.append(c.entry.code);
    }
    if (!needKline.isEmpty()) {
        m_data->refreshKlines(needKline, 120);
    }

    if (m_db->isOpen()) {
        if (m_sentimentState.fact.limitUpCount > 0)
            m_db->saveSentiment(m_sentimentState);
    }
    m_screenDirty = false;

    updateRecommend();
}

void MainWindow::updateRecommend()
{
    const MarketRegime reg = computeRegime();

    RecommendBundle bundle =
        m_recEngine->build(m_candidates, m_klines, reg, m_pipeline);

    // ---- 情绪驱动的做多信号与仓位（执行层）----
    // 用「闸门拦截前的完整名单」生成信号：禁止开仓时也能给出观察信号，
    // 这是既有「打开即见观察名单」契约的延续。
    if (m_sentimentState.valid()) {
        const QVector<Candidate>& signalSource =
            m_pipeline->lastPassedPreGate().isEmpty() ? m_candidates
                                                      : m_pipeline->lastPassedPreGate();
        const QVector<LongSignal> longs =
            LongSignalEngine::generate(signalSource, m_klines, m_sentimentState);
        LongSignalEngine::apply(bundle, longs, m_sentimentState);
    }

    // 非交易日回退：明确标注这是历史交易日的数据，避免误当作今日
    if (m_currentDate.isValid() && m_currentDate != QDate::currentDate()) {
        bundle.notice = QStringLiteral("【%1 交易日数据】%2")
                            .arg(m_currentDate.toString(QStringLiteral("yyyy-MM-dd")), bundle.notice);
    }

    // 数据新鲜度硬保护：缓存 / 陈旧 / 历史数据一律不得给出「可执行」结论与仓位
    if (m_data->isDataStale() || m_currentDate != QDate::currentDate()) {
        for (TradePlan& p : bundle.plans) {
            p.actionable = false;
            p.positionPct = 0.0;
        }
        bundle.notice = QStringLiteral("【行情非实时，仅供观察，禁止据此下单】") + bundle.notice;
        qWarning().noquote()
            << QStringLiteral("[做多量化] 行情陈旧或为历史交易日，荐股已全部降级为观察名单");
    }

    fillCurrentPrices(bundle);
    restoreCachedKlines(bundle);
    m_recPanel->setBundle(bundle);
    m_recPanel->setMarketData(m_klines, m_data->snapshots());  // 详情弹窗的 K 线 / 快照
    updateGlobalVerdict(bundle);

    updateUsedPct();                              // 顶栏实际仓位读数（计划 / 持仓）

    // 落库：下次打开（即使断网）直接展示这份已算好的结果
    if (m_db->isOpen() && !bundle.plans.isEmpty()) {
        m_db->saveRecommend(bundle);
    }

    // 自动推送：只在「今天」的新结果上触发（看历史日期不该补发邮件）；
    // 是否有票、是否同内容重复，全部由 PushService 内部闸门判断。
    if (m_push && m_currentDate == QDate::currentDate()) {
        pushCurrentResult(false);
    }

    // 复盘助手：把当次分析结果留作 15:00 复盘的输入
    publishReviewInput(bundle);
}

void MainWindow::buildReviewInput(ReviewInput& out, const RecommendBundle& bundle) const
{
    out = ReviewInput();    out.date = m_currentDate;
    out.sentiment = m_sentimentState;
    const FilterPipeline::Funnel f = m_pipeline->lastFunnel();
    out.funnelInput = f.input;
    out.funnelHard = f.afterHardFilter;
    out.funnelSector = f.afterSector;
    out.funnelQuant = f.afterQuant;
    out.funnelOut = f.output;
    out.candidates = m_candidates;
    out.bundle = bundle;

    // ---- 决策复盘的事实快照：只记录当日真实可得的数据，历史日期由复盘页从本地库补齐 ----
    out.klines = m_klines;
    if (m_journalStore) out.trades = m_journalStore->trades();
    for (const Candidate& c : m_candidates) {
        const QString& code = c.entry.code;
        if (c.entry.amount > 0.0 && c.entry.sealOrder > 0.0)
            out.sealToAmount.insert(code, c.entry.sealToAmountRatio());
        if (!m_data) continue;
        const StockDetailData* detail = m_data->stockDetail(code);
        if (!detail || !detail->flowLoaded || detail->flows.isEmpty()) continue;
        const MoneyFlowDay& day = detail->flows.last();
        out.mainNetYi.insert(code, day.mainNet / 1.0e8);   // 元 → 亿元
        out.mainNetSource.insert(code,
            QStringLiteral("东财个股资金流日线 %1 · fflow 主力净额 %2 元（%3 亿元）· 占成交额 %4%")
                .arg(day.date.toString(Qt::ISODate))
                .arg(QString::number(day.mainNet, 'f', 0))
                .arg(QString::number(day.mainNet / 1.0e8, 'f', 2))
                .arg(QString::number(day.mainPct, 'f', 2)));
    }
}

void MainWindow::publishReviewInput(const RecommendBundle& bundle)
{
    if (!m_assistant) return;
    ReviewInput in;
    buildReviewInput(in, bundle);
    m_assistant->setInput(in);
}

void MainWindow::pushCurrentResult(bool manual)
{
    if (!m_push) return;

    const RecommendBundle& bundle = m_recPanel->bundle();
    if (bundle.plans.isEmpty()) {
        if (manual) {
            QMessageBox::information(this, QStringLiteral("消息推送"),
                                     QStringLiteral("当前还没有可推送的荐股结果，请等待分析完成。"));
        }
        return;
    }

    m_pushManual = manual;
    m_push->pushBundle(bundle, m_sentimentState, manual);
}

void MainWindow::persistFactorConfig()
{
    if (!m_db->isOpen()) return;
    // 无头回测 CLI（--backtest）读取同一份配置，保证实盘 / 回测口径一致
    m_db->setSetting(QStringLiteral("factor.config"),
        QString::fromUtf8(QJsonDocument(m_factorCfg.toJson()).toJson(QJsonDocument::Compact)));
}

// 跨页常驻结论：一句白话 + 可核对数字，颜色只作辅助
void MainWindow::updateGlobalVerdict(const RecommendBundle& b)
{
    if (!m_globalVerdict) return;
    int act = 0;
    for (const TradePlan& p : b.plans) if (p.actionable) ++act;

    QString text;
    QColor color = Theme::textMuted();
    if (b.cached) {
        text = QStringLiteral("今日结论：这是上次保存的结果，请勿据此下单");
        color = Theme::warning();
    } else if (b.regime.valid() && !b.regime.tradeAllowed) {
        QString note = b.regime.note;
        if (note.startsWith(QStringLiteral("【"))) {
            const int end = note.indexOf(QChar(0x3011));
            if (end > 0) note = note.mid(end + 1);
        }
        const int cut = note.indexOf(QChar(0xff5c));
        if (cut > 0) note = note.left(cut);
        note = note.trimmed();
        if (note.length() > 72) note = note.left(72) + QStringLiteral("…");
        text = note.isEmpty()
            ? QStringLiteral("今日建议空仓：市场情绪不满足出手条件")
            : QStringLiteral("今日建议空仓：%1").arg(note);
        color = Theme::danger();
    } else if (act > 0) {
        text = QStringLiteral("今日可以出手 · %1 只可执行 ｜ 最晚决策 9:25 竞价")
                   .arg(act);
        color = Theme::success();
    } else {
        text = QStringLiteral("今日暂无可执行标的 ｜ 可查看「为什么没选」");
        color = Theme::warning();
    }

    m_globalVerdict->setText(text + QStringLiteral("　·　点此查看 AI 选股结果"));
    m_globalVerdict->setStyleSheet(QStringLiteral(
        "QPushButton#GlobalVerdict{border:0;border-bottom:1px solid %1;"
        "background:%2;color:%3;font-size:%4px;font-weight:600;"
        "text-align:left;padding:0 20px;}")
        .arg(Theme::border().name(), Theme::bg().name(), color.name())
        .arg(Theme::fontMd()));

    // 数据状态角标：与结论同处一条，缓存 / 回退 / 保守估计一眼可见
    if (m_globalFreshness) {
        const QString longFresh = AiStockPanel::freshnessSummary(b);
        const QString shortFresh = AiStockPanel::freshnessShort(b);
        const bool stale = b.cached
                        || longFresh.contains(QStringLiteral("回退"))
                        || longFresh.contains(QStringLiteral("非实时"))
                        || longFresh.contains(QStringLiteral("保守"));
        m_globalFreshness->setText(shortFresh);
        m_globalFreshness->setStyleSheet(
            Theme::textStyle(stale ? Theme::warning() : Theme::textMuted(),
                             Theme::fontSm(), false)
            + QStringLiteral("padding-right:20px;"));
        m_globalFreshness->setToolTip(QStringLiteral("数据状态：%1").arg(longFresh));
    }
}

// 状态栏实际仓位读数：当日可执行计划的仓位合计与持仓只数（执行口径）
void MainWindow::updateUsedPct()
{
    double planned = 0.0;
    int actionable = 0;
    const RecommendBundle& b = m_recEngine->lastBundle();
    for (const TradePlan& p : b.plans) {
        if (p.actionable) {
            planned += p.positionPct;
            ++actionable;
        }
    }
    int held = 0;
    if (m_db->isOpen()) {
        for (const TradeRecord& r : m_db->trades())
            if (!r.closed()) ++held;
    }

    if (m_usedLabel) {
        m_usedLabel->setText(QStringLiteral("计划 %1%（%2 只）｜ 持仓 %3")
                                 .arg(planned, 0, 'f', 1).arg(actionable).arg(held));
        m_usedLabel->setStyleSheet(QStringLiteral(
            "font-size:12px;font-weight:400;color:%1;").arg(Theme::textMuted().name()));
        m_usedLabel->setToolTip(QStringLiteral(
            "当日计划仓位合计 %1%（可执行 %2 只）｜ 当前持仓 %3 只")
            .arg(planned, 0, 'f', 1).arg(actionable).arg(held));
    }
}

void MainWindow::loadCachedRecommend()
{
    if (!m_db->isOpen()) {
        m_recPanel->setLoading(QStringLiteral("正在拉取行情并计算荐股…"));
        return;
    }
    const QDate d = m_db->latestRecommendDate();
    if (!d.isValid()) {
        m_recPanel->setLoading(QStringLiteral("正在拉取行情并计算荐股…"));
        return;
    }
    RecommendBundle bundle = m_db->loadRecommend(d);
    if (bundle.plans.isEmpty()) {
        m_recPanel->setLoading(QStringLiteral("正在拉取行情并计算荐股…"));
        return;
    }
    bundle.cached = true;
    // 离线缓存仅供回看：一律降级为观察名单，避免拿昨天的价位今天下单
    for (TradePlan& p : bundle.plans) {
        p.actionable = false;
        p.positionPct = 0.0;
    }
    for (TradePlan& p : bundle.backups) {
        p.actionable = false;
        p.positionPct = 0.0;
    }
    if (bundle.notice.isEmpty()) {
        bundle.notice = QStringLiteral("本地缓存的荐股计划（%1）—— 正在拉取最新行情，结果到达后自动覆盖")
                            .arg(d.toString(QStringLiteral("yyyy-MM-dd")));
    } else {
        bundle.notice = QStringLiteral("本地缓存（%1）｜ %2")
                            .arg(d.toString(QStringLiteral("yyyy-MM-dd")), bundle.notice);
    }
    fillCurrentPrices(bundle);
    restoreCachedKlines(bundle);
    m_recPanel->setBundle(bundle);
    m_recPanel->setMarketData(m_klines, m_data->snapshots());
    updateGlobalVerdict(bundle);

    updateUsedPct();
}

// 启动时把 SQLite 里的日 K 缓存补进 m_klines：
// 详情弹窗的缩略图 / 均线 / 量能在「打开即见」的缓存态也能画出来，不必等网络回包。
void MainWindow::restoreCachedKlines(const RecommendBundle& bundle)
{
    if (!m_db || !m_db->isOpen()) return;
    auto ensure = [this](const QString& code) {
        if (code.isEmpty() || m_klines.contains(code)) return;
        const KlineSeries ks = m_db->loadKline(code);
        if (ks.valid()) m_klines.insert(code, ks);
    };
    for (const TradePlan& p : bundle.plans) ensure(p.code);
    for (const TradePlan& p : bundle.backups) ensure(p.code);
}

// 旧版本缓存里没有 currentPrice：用实时快照 / 涨停池最新价补齐卡片上的「当前价格」
void MainWindow::fillCurrentPrices(RecommendBundle& bundle) const
{
    if (!m_data) return;
    const QHash<QString, Snapshot>& snaps = m_data->snapshots();
    auto fill = [&](TradePlan& p) {
        if (p.currentPrice > 0.0) return;
        const auto it = snaps.constFind(p.code);
        if (it != snaps.constEnd() && it->isValid()) {
            p.currentPrice = it->price;
            return;
        }
        for (const LimitEntry& e : m_limitUp) {
            if (e.code == p.code && e.price > 0.0) {
                p.currentPrice = e.price;
                return;
            }
        }
    };
    for (TradePlan& p : bundle.plans) fill(p);
    for (TradePlan& p : bundle.backups) fill(p);
}

void MainWindow::onScreenRequested(const QDate& date)
{
    m_currentDate = date;
    m_fallbackDepth = 0;
    requestPoolWithFallback(date);
}

void MainWindow::onRefreshRequested()
{
    m_fallbackDepth = 0;
    m_recPanel->setLoading(QStringLiteral("正在重新拉取行情并计算荐股…"));
    requestPoolWithFallback(m_currentDate);
    m_data->refreshSnapshot();   // 持仓盈亏也一起刷新
}

void MainWindow::updateStatusBar()
{
    const RecommendBundle& b = m_recEngine->lastBundle();
    int actionable = 0;
    for (const TradePlan& p : b.plans) if (p.actionable) ++actionable;

    m_statusLabel->setText(QStringLiteral("交易日 %1 ｜ 涨停 %2 / 炸板 %3 / 跌停 %4 ｜ 荐股 %5 只（可执行 %6）")
        .arg(m_currentDate.toString("yyyy-MM-dd"))
        .arg(m_limitUp.size()).arg(m_broken.size()).arg(m_limitDown.size())
        .arg(b.plans.size()).arg(actionable));
    if (m_brandStatus) {
        // 顶栏文案写短：策略名已在品牌条副标题里，这里不重复
        m_brandStatus->setText(QStringLiteral("%1 ｜ 涨停 %2")
            .arg(m_currentDate.toString("MM-dd"))
            .arg(m_limitUp.size()));
        m_brandStatus->setToolTip(QStringLiteral("%1 ｜ 涨停 %2 ｜ 炸板 %3 ｜ 跌停 %4 ｜ 策略：%5")
            .arg(m_currentDate.toString("yyyy-MM-dd"))
            .arg(m_limitUp.size()).arg(m_broken.size()).arg(m_limitDown.size())
            .arg(FactorConfig::strategyName()));
    }
}
