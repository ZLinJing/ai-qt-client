#include "ui/AiStockPanel.h"       // 本面板头文件，包含成员声明与信号定义
#include "ui/Components.h"           // 通用组件：列可见性菜单等
#include "ui/Theme.h"                // 全局主题色与表格样式工具
#include "ui/TableEmptyHint.h"       // 表格无数据时的占位提示控件
#include "ui/PlanCellDelegate.h"     // 自定义委托：实现显示文本与排序键分离
#include "ui/StockCard.h"            // 一只标的 = 一张白色卡片
#include "ui/KlineThumbnail.h"       // 详情弹窗里的 K 线缩略图
#include "core/Auction.h"            // 竞价评估模块，提供竞价结论名称等工具函数
#include "core/Indicators.h"         // MA / 量能计算
#include <QVBoxLayout>               // 垂直布局管理器
#include <QHBoxLayout>               // 水平布局管理器
#include <QLabel>                    // 文本标签控件
#include <QFrame>                    // 框架容器，用于卡片样式
#include <QTableWidget>              // 表格控件，展示荐股清单
#include <QTableWidgetItem>          // 表格单元格项
#include <QHeaderView>               // 表头视图，控制排序指示器与列拉伸
#include <QPushButton>               // 按钮控件
#include <QComboBox>                 // 下拉框，用于筛选标的类型
#include <QLineEdit>                 // 单行输入框，用于关键词搜索
#include <QBrush>                    // 单元格背景画刷
#include <QColor>                    // 前景色 / 背景色
#include <QStringList>               // tooltip 多行文本拼接
#include <QDate>                     // 判断结论是否属于非交易日回退
#include <QSignalBlocker>            // 回填配置时不触发回写
#include <QResizeEvent>              // 卡片列表列数随宽度自适应
#include <QScrollArea>               // 卡片列表滚动容器
#include <QGridLayout>               // 卡片自适应列网格
#include <QDialog>                   // 「查看详情」弹窗
#include <QHash>                     // 日 K / 快照 / 详情数据缓存
#include <cmath>                     // std::isfinite 判断均线数值是否有效

namespace {

// 将金额格式化为两位小数字符串，统一货币显示精度
QString money(double v)
{
    return QString::number(v, 'f', 2);
}

// 拼接价格区间字符串，如 "19.34 ~ 19.94"，用于买入/止盈/止损列显示
QString rangeStr(double lo, double hi)
{
    return money(lo) + QStringLiteral(" ~ ") + money(hi);
}

// 判定原文里的【标签】与「先行指标」串是研究员口径，
// 结论卡只取第一段原因，用一句人话回答「今天为什么不能做」。
QString plainGateReason(const MarketRegime& r)
{
    if (!r.valid()) return QStringLiteral("等待当日行情数据，结论尚未生成");
    QString s = r.note;
    if (s.startsWith(QStringLiteral("【"))) {
        const int end = s.indexOf(QChar(0x3011));      // 全角「】」
        if (end > 0) s = s.mid(end + 1);
    }
    int cut = s.indexOf(QChar(0xff5c));          // 全角「｜」
    if (cut > 0) s = s.left(cut);
    cut = s.indexOf(QLatin1Char('|'));            // 半角「|」
    if (cut > 0) s = s.left(cut);
    s = s.trimmed();
    if (s.isEmpty()) {
        s = r.tradeAllowed ? QStringLiteral("市场情绪满足出手条件")
                           : QStringLiteral("市场情绪不满足出手条件，今日空仓");
    }
    return s;
}

QString bundleFreshnessSummary(const RecommendBundle& b)
{
    QStringList parts;
    if (b.cached) {
        parts << QStringLiteral("上次保存的结果 · 今日不作为下单依据");
    } else if (!b.updatedAt.isEmpty() && b.updatedAt != QStringLiteral("synthetic")) {
        const QString stamp = b.updatedAt.length() >= 16
            ? b.updatedAt.mid(5, 11) : b.updatedAt;
        parts << QStringLiteral("数据截至 %1").arg(stamp);
    }
    if (b.tradeDate.isValid() && b.tradeDate != QDate::currentDate()) {
        parts << QStringLiteral("回退到 %1 交易日数据")
                 .arg(b.tradeDate.toString(QStringLiteral("MM-dd")));
    }
    if (b.notice.contains(QStringLiteral("非实时"))) {
        parts << QStringLiteral("行情非实时 · 仅供观察");
    }
    // 定价依据里显式注明「保守估计」的，汇总成一条可见提示
    bool conservative = false;
    for (const TradePlan& p : b.plans) {
        if (p.buyBasis.contains(QStringLiteral("保守"))
            || p.buyBasis.contains(QStringLiteral("缺失"))
            || p.buyBasis.contains(QStringLiteral("未就绪"))) {
            conservative = true;
            break;
        }
    }
    if (conservative) parts << QStringLiteral("部分价格区间为保守估计");
    if (parts.isEmpty()) return QStringLiteral("数据已更新");
    return parts.join(QStringLiteral(" ｜ "));
}

// 定价依据压成一句白话（完整依据仍在表格 tooltip 里可核对）
QString shortBasis(const QString& s)
{
    QString t = s.simplified();
    if (t.length() > 96) t = t.left(96) + QStringLiteral("…");
    return t;
}

QString pctText(double v, int digits = 0)
{
    return QStringLiteral("%1%").arg(v, 0, 'f', digits);
}

// 亿元（市值 / 营收 / 净利润）
QString yi(double yuan)
{
    return QStringLiteral("%1 亿元").arg(yuan / 1e8, 0, 'f', 2);
}

// 带符号的百分比：+4.42% / -6.03%
QString signPct(double v, int digits = 2)
{
    return QStringLiteral("%1%2%").arg(v >= 0.0 ? QStringLiteral("+") : QString(),
                                      QString::number(v, 'f', digits));
}

// 资金净额：绝对值 >= 1 亿用亿元，否则万元；正数补 + 号
QString flowMoney(double v)
{
    const QString s = qAbs(v) >= 1e8
                          ? QString::number(v / 1e8, 'f', 2) + QStringLiteral(" 亿元")
                          : QString::number(v / 1e4, 'f', 0) + QStringLiteral(" 万元");
    return (v > 0.0 ? QStringLiteral("+") : QString()) + s;
}

// 市值规模分档（按流通市值）
QString capScale(double floatCap)
{
    const double y = floatCap / 1e8;
    if (y >= 500.0) return QStringLiteral("大盘（≥500 亿）");
    if (y >= 100.0) return QStringLiteral("中盘（100~500 亿）");
    if (y > 0.0)    return QStringLiteral("小盘（<100 亿）");
    return QStringLiteral("暂无市值口径");
}

QString volumeTrend(double changePct)
{
    if (changePct >= 15.0)  return QStringLiteral("明显放量");
    if (changePct >= 3.0)   return QStringLiteral("温和放量");
    if (changePct <= -15.0) return QStringLiteral("明显缩量");
    if (changePct <= -3.0)  return QStringLiteral("温和缩量");
    return QStringLiteral("量能基本持平");
}

// 当前价格与买入价格区间的位置关系（详情弹窗的核心结论之一）
QString priceVsBuyRange(const TradePlan& p)
{
    if (p.currentPrice <= 0.0 || p.buyLow <= 0.0 || p.buyHigh <= 0.0)
        return QStringLiteral("暂无实时价或买入区间，无法判断位置关系");
    const double price = p.currentPrice;
    if (price < p.buyLow) {
        return QStringLiteral("现价 %1 低于买入下沿 %2（%3），尚未进入计划买点")
            .arg(money(price), money(p.buyLow),
                 signPct((price / p.buyLow - 1.0) * 100.0, 2));
    }
    if (price > p.buyHigh) {
        return QStringLiteral("现价 %1 高于买入上沿 %2（%3），追高需谨慎")
            .arg(money(price), money(p.buyHigh),
                 signPct((price / p.buyHigh - 1.0) * 100.0, 2));
    }
    return QStringLiteral("现价 %1 位于买入区间 %2 内（距下沿 %3 / 距上沿 %4）")
        .arg(money(price), rangeStr(p.buyLow, p.buyHigh),
             signPct((price / p.buyLow - 1.0) * 100.0, 2),
             signPct((price / p.buyHigh - 1.0) * 100.0, 2));
}

} // namespace

// 数据状态摘要：对外唯一入口，主窗口的全局结论条与本页结论卡共用同一口径
QString AiStockPanel::freshnessSummary(const RecommendBundle& bundle)
{
    return bundleFreshnessSummary(bundle);
}

// 数据状态摘要（短版）：顶栏角标位置有限，只给可核对的关键词；
// 完整说明放 tooltip。**短版不参与窗口最小宽度计算** —— 顶栏文案一旦写长，
// 窗口最小宽度就会顶到 1100+，1366×768 的笔记本直接放不下。
QString AiStockPanel::freshnessShort(const RecommendBundle& b)
{
    QStringList parts;
    if (b.cached) {
        parts << QStringLiteral("上次结果 · 勿下单");
    } else if (!b.updatedAt.isEmpty() && b.updatedAt != QStringLiteral("synthetic")) {
        const QString stamp = b.updatedAt.length() >= 16
            ? b.updatedAt.mid(11, 5) : b.updatedAt;
        parts << QStringLiteral("更新 %1").arg(stamp);
    }
    if (b.tradeDate.isValid() && b.tradeDate != QDate::currentDate()) {
        parts << QStringLiteral("回退 %1").arg(b.tradeDate.toString(QStringLiteral("MM-dd")));
    }
    if (b.notice.contains(QStringLiteral("非实时"))) {
        parts << QStringLiteral("非实时");
    }
    for (const TradePlan& p : b.plans) {
        if (p.buyBasis.contains(QStringLiteral("保守"))
            || p.buyBasis.contains(QStringLiteral("缺失"))
            || p.buyBasis.contains(QStringLiteral("未就绪"))) {
            parts << QStringLiteral("含保守估计");
            break;
        }
    }
    if (parts.isEmpty()) return QStringLiteral("已更新");
    return parts.join(QStringLiteral(" · "));
}

// 构造函数：初始化荐股面板的整体布局与子控件
AiStockPanel::AiStockPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("AiStockPanel")); // 设置对象名，供 QSS 样式表选择器使用
    auto* root = new QVBoxLayout(this);              // 创建根垂直布局，自动设为本 widget 的布局
    root->setContentsMargins(0, 0, 0, 0);            // 去除外边距，让内容贴边以最大化利用空间
    root->setSpacing(12);                            // 子控件间距 12px，保持视觉呼吸感

    root->addWidget(buildSummaryBar());              // 今日结论卡 + 数据新鲜度角标
    root->addWidget(buildToolbar());                 // 页内工具条（推送/筛选/搜索/状态）
    // 上下文行动条不进页面布局：由主窗口 reparent 到顶栏右上角（每页动作位置统一）
    m_pageActions = buildPageActions();

    // ---- 荐股表：只作为内部数据源（排序 / 选中 / 兼容性校验），可见结果改由卡片列表呈现 ----
    m_table = new QTableWidget(0, ColCount, this);   // 初始 0 行，列数由枚举 ColCount 决定
    m_table->setObjectName(QStringLiteral("RecommendTable")); // 表格对象名，供主题样式使用
    m_table->setHorizontalHeaderLabels({             // 设置表头文本
        QStringLiteral("代码"), QStringLiteral("名称"), QStringLiteral("推荐理由"),
        QStringLiteral("买入价"), QStringLiteral("目标价"), QStringLiteral("止损价"),
        QStringLiteral("仓位"),
        QStringLiteral("盈亏比"), QStringLiteral("止损幅度"), QStringLiteral("成交概率"),
        QStringLiteral("状态")
    });
    // 「推荐理由」列吸收剩余宽度
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(ColReason, QHeaderView::Stretch);
    m_table->setAlternatingRowColors(true);          // 启用交替行背景色，提升可读性
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows); // 点击任意单元格选中整行
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);  // 禁止编辑，表格纯展示
    m_table->setSortingEnabled(false);               // 先关闭排序，避免填充数据时反复重排影响性能
    // 默认排序：按仓位降序（仓位已综合评分/盈亏比/成交概率），用户点表头后以指示器为准
    m_table->horizontalHeader()->setSortIndicator(ColPosition, Qt::DescendingOrder);
    m_table->setSortingEnabled(true);                // 重新开启排序，应用上方设定的默认排序规则
    // 自定义委托：数值列按原始数值排序，界面仍显示格式化文本（如 "19.34 ~ 19.94"）
    m_table->setItemDelegate(new PlanCellDelegate(m_table));
    Theme::polishTable(m_table);                     // 应用全局表格主题样式（边框/字体/颜色等）
    {
        const int w[ColCount] = { 64, 71, 150, 108, 108, 108, 56, 60, 71, 71, 92 };
        for (int c = 0; c < ColCount; ++c) m_table->setColumnWidth(c, w[c]); // 逐列设置宽度
    }
    m_columnPicker = new Comp::ColumnPicker(m_table, m_btnColumns,
                                             QVector<bool>(ColCount, true), this);
    connect(m_columnPicker, &Comp::ColumnPicker::changed, this,
            [this](const QString& enc) { Q_EMIT columnVisibilityChanged(enc); });
    // 表格已不在界面上，「列」菜单不再暴露给用户；控件保留以维持列可见性存档 / 冒烟契约
    m_btnColumns->hide();

    // 空态提示控件，作为表格的子控件居中覆盖显示
    m_emptyHint = new TableEmptyHint(m_table,
        QStringLiteral("正在拉取行情并计算荐股…\n结果到达后会自动填充，无需手动操作"), this);
    m_table->setMinimumWidth(280);
    
    m_table->hide();

    // 结果区只保留卡片列表：整块宽度都给卡片，列数随窗口宽度自动变化
    root->addWidget(buildCardView(), 1);
    m_cardAreaWidth = qMax(kCardMinWidth, width());

    // 选中行变化时更新按钮提示与卡片高亮
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &AiStockPanel::onRowSelected);
}

QWidget* AiStockPanel::buildSummaryBar()
{
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("Card"));
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(6);

    m_headline = new QLabel(QStringLiteral("正在加载行情…"), card);
    m_headline->setObjectName(QStringLiteral("DecisionHeadline"));
    m_headline->setWordWrap(true);
    m_headline->setStyleSheet(Theme::textStyle(Theme::text(), Theme::fontH2(), true));
    lay->addWidget(m_headline);

    // 决策数字与数据状态并排一行：数字在左（读数），状态在右（可信度）
    auto* metaRow = new QWidget(card);
    auto* metaLay = new QHBoxLayout(metaRow);
    metaLay->setContentsMargins(0, 0, 0, 0);
    metaLay->setSpacing(12);

    m_meta = new QLabel(metaRow);
    m_meta->setObjectName(QStringLiteral("Muted"));
    m_meta->setWordWrap(true);
    metaLay->addWidget(m_meta, 1);

    m_freshness = new QLabel(metaRow);
    m_freshness->setObjectName(QStringLiteral("FreshnessBadge"));
    m_freshness->setWordWrap(true);
    metaLay->addWidget(m_freshness, 0);
    lay->addWidget(metaRow);

    // 一句人话原因：始终可见（缓存 / 空仓日时这是最关键的一句）
    m_reason = new QLabel(card);
    m_reason->setObjectName(QStringLiteral("Muted"));
    m_reason->setWordWrap(true);
    lay->addWidget(m_reason);

    // 判定原文（研究员口径）默认折叠，展开后仍可逐字核对
    m_reasonToggle = new QPushButton(QStringLiteral("▸ 判定说明（判定原文）"), card);
    m_reasonToggle->setObjectName(QStringLiteral("Ghost"));
    m_reasonToggle->setCheckable(true);
    m_reasonToggle->setChecked(false);
    m_reasonToggle->setCursor(Qt::PointingHandCursor);
    m_reasonToggle->setToolTip(QStringLiteral("展开查看情绪判定的原始口径（研究用）"));
    lay->addWidget(m_reasonToggle);

    m_verdict = new QLabel(card);
    m_verdict->setObjectName(QStringLiteral("Muted"));
    m_verdict->setWordWrap(true);
    m_verdict->setVisible(false);
    lay->addWidget(m_verdict);

    connect(m_reasonToggle, &QPushButton::toggled, this, [this](bool on) {
        m_verdict->setVisible(on && !m_verdict->text().isEmpty());
        m_reasonToggle->setText(on
            ? QStringLiteral("▾ 收起判定说明")
            : QStringLiteral("▸ 判定说明（判定原文）"));
    });

    // 全部结论文案都可换行：文本本身写短，不靠压缩标签宽度来省空间
    // （QSizePolicy::Ignored 会让标签在空间紧张时被压成 0 宽，文字互相重叠）

    return card;
}

void AiStockPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    // 卡片区占满整行：列数直接由面板宽度决定，不再依赖停靠宽度
    m_cardAreaWidth = qMax(kCardMinWidth, width());
    reflowCards();
}

// 恢复上次保存的列可见性（主窗口启动时调用）
void AiStockPanel::restoreColumnVisibility(const QString& encoded)
{
    if (m_columnPicker) m_columnPicker->restore(encoded);
}

QWidget* AiStockPanel::buildPageActions()
{
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("PageActions"));
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(Theme::Space::Sm);

    // 「重新分析」按钮：触发重新拉取涨停池并重算交易计划
    auto* btnRefresh = new QPushButton(QStringLiteral("重新分析"), bar);
    btnRefresh->setObjectName(QStringLiteral("Primary")); // 主按钮样式
    btnRefresh->setMinimumWidth(96);                 // 最小宽度，防止文字过窄
    btnRefresh->setToolTip(QStringLiteral("重新拉取涨停池并重算买入/止盈/止损区间"));
    h->addWidget(btnRefresh);
    connect(btnRefresh, &QPushButton::clicked, this, &AiStockPanel::onRefreshClicked); // 绑定刷新槽

    // 「记录买入」按钮：将选中计划写入交易日志，用于事后复盘
    auto* btnTrade = new QPushButton(QStringLiteral("记录买入"), bar);
    btnTrade->setToolTip(QStringLiteral("把选中标的记入交易日志（带计划买点/止损/止盈，用于事后复盘）"));
    h->addWidget(btnTrade);
    m_btnTrade = btnTrade;                           // 保存指针，后续动态更新 tooltip
    connect(btnTrade, &QPushButton::clicked, this, [this] { // 点击时发射记录交易信号
        const TradePlan p = currentPlan();           // 获取当前选中行的交易计划
        if (!p.valid()) { warnNoSelection(QStringLiteral("记录买入")); return; } // 未选中则警告并返回
        Q_EMIT recordTradeRequested(p);              // 发射信号，携带完整计划数据
    });

    // 未挂到顶栏前保持隐藏（否则会以 (0,0) 浮在面板上）
    bar->hide();
    return bar;
}

// 页内工具条：次级操作（筛选 / 搜索 / 状态读数）
QWidget* AiStockPanel::buildToolbar()
{
    auto* bar = new QWidget(this);                   // 工具栏容器
    auto* h = new QHBoxLayout(bar);                  // 工具栏水平布局
    h->setContentsMargins(0, 0, 0, 0);               // 工具栏无外边距，由父布局统一控制间距
    h->setSpacing(8);                                // 按钮与输入框之间间距 8px

    m_kindFilter = new QComboBox(bar);
    m_kindFilter->addItems({ QStringLiteral("全部"), QStringLiteral("仅可执行"),
                             QStringLiteral("仅观察") });
    m_kindFilter->setFixedWidth(128);                // 固定宽度，容纳「仅可执行（99）」不跳动
    m_kindFilter->setToolTip(QStringLiteral(
        "按标的类型过滤清单：\n"
        "· 全部 —— 可执行与观察一起看\n"
        "· 仅可执行 —— 只看今天能下单的标的\n"
        "· 仅观察 —— 只看备选 / 近失 / 情绪不足的标的（仓位恒为 0）"));
    h->addWidget(m_kindFilter);

    // 列可见性菜单：12 列全开时需要横向滚动，与其让用户滚，不如让他自己挑
    m_btnColumns = new QPushButton(QStringLiteral("列"), bar);
    m_btnColumns->setObjectName(QStringLiteral("Ghost"));
    h->addWidget(m_btnColumns);

    // 关键词搜索框：支持按代码/名称/板块/题材模糊过滤
    m_keyword = new QLineEdit(bar);
    m_keyword->setPlaceholderText(QStringLiteral("代码 / 名称 / 板块")); // 占位提示文本
    m_keyword->setClearButtonEnabled(true);          // 启用一键清除按钮
    m_keyword->setMinimumWidth(96);                 // 下限可压：不让工具栏撑大窗口最小宽度
    m_keyword->setMaximumWidth(220);                 // 限制最大宽度，防止窗口拉宽时搜索框过长
    h->addWidget(m_keyword);

    h->addStretch(1);                                // 弹性空间，将右侧状态标签推到最右端
    m_status = new QLabel(QStringLiteral("等待数据…"), bar); // 状态标签，显示统计或错误信息
    m_status->setObjectName(QStringLiteral("Muted")); // 次要文本样式（灰色）
    // 状态文字长度随筛选结果变化，不应参与窗口最小宽度计算
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    h->addWidget(m_status);

    // 筛选条件变化时实时过滤表格内容
    connect(m_kindFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AiStockPanel::applyFilters);
    connect(m_keyword, &QLineEdit::textChanged, this, &AiStockPanel::applyFilters);

    return bar;                                      // 返回构建好的工具栏控件
}

QWidget* AiStockPanel::buildCardView()
{
    m_cardScroll = new QScrollArea(this);
    m_cardScroll->setObjectName(QStringLiteral("CardScroll"));
    m_cardScroll->setWidgetResizable(true);
    m_cardScroll->setFrameShape(QFrame::NoFrame);
    m_cardScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cardScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_cardScroll->setMinimumWidth(280);

    auto* content = new QWidget(m_cardScroll);
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(0, 0, 4, 0);
    lay->setSpacing(kCardSpacing);

    m_cardEmpty = new QLabel(content);
    m_cardEmpty->setObjectName(QStringLiteral("CardEmptyHint"));
    m_cardEmpty->setAlignment(Qt::AlignCenter);
    m_cardEmpty->setWordWrap(true);
    m_cardEmpty->hide();
    lay->addWidget(m_cardEmpty);

    m_cardGrid = new QGridLayout();
    m_cardGrid->setContentsMargins(0, 0, 0, 0);
    m_cardGrid->setSpacing(kCardSpacing);
    lay->addLayout(m_cardGrid);
    lay->addStretch(1);

    m_cardScroll->setWidget(content);
    return m_cardScroll;
}

// 按表格当前行序重建卡片：排序（默认仓位降序）在卡片列表里同样成立
void AiStockPanel::rebuildCards()
{
    if (!m_cardGrid || !m_cardScroll || !m_table) return;

    for (StockCard* card : m_cards) {
        m_cardGrid->removeWidget(card);
        delete card;
    }
    m_cards.clear();
    m_cardLayoutDirty = true;

    QWidget* host = m_cardScroll->widget();
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* it = m_table->item(r, ColCode);
        if (!it) continue;
        const QString code = it->data(Qt::UserRole).toString();
        for (const TradePlan& p : m_filtered) {
            if (p.code != code) continue;
            auto* card = new StockCard(p, host);
            connect(card, &StockCard::cardClicked, this, &AiStockPanel::selectCode);
            connect(card, &StockCard::detailRequested, this,
                    [this](const QString& c) { showCardDetail(c); });
            connect(card, &StockCard::klineRequested,
                    this, &AiStockPanel::klineRequested);
            m_cards.append(card);
            break;
        }
    }

    if (m_cardEmpty) m_cardEmpty->setVisible(m_cards.isEmpty());
    reflowCards();
    updateCardSelection();
}

void AiStockPanel::reflowCards()
{
    if (!m_cardGrid || !m_cardScroll) return;
    const int basis = m_cardAreaWidth > 0
                          ? m_cardAreaWidth
                          : m_cardScroll->viewport()->width();
    const int available = qMax(kCardMinWidth, basis - 4);
    const int cols = qBound(1,
                            (available + kCardSpacing) / (kCardMinWidth + kCardSpacing),
                            kCardMaxColumns);
    if (!m_cardLayoutDirty && cols == m_cardColumns) return;
    m_cardColumns = cols;
    m_cardLayoutDirty = false;

    for (StockCard* card : m_cards) m_cardGrid->removeWidget(card);
    for (int i = 0; i < m_cards.size(); ++i)
        m_cardGrid->addWidget(m_cards.at(i), i / cols, i % cols);
}

void AiStockPanel::updateCardSelection()
{
    const QString code = currentCode();
    for (StockCard* card : m_cards)
        card->setSelected(!code.isEmpty() && card->code() == code);
}

// 「查看详情」：行业 / 市值 / 财报 / K 线缩略图 / 均线 / 量能 / 资金 / 交易计划
void AiStockPanel::showCardDetail(const QString& code)
{
    bool exists = false;
    for (const TradePlan& p : m_filtered) {
        if (p.code == code) { exists = true; break; }
    }
    if (!exists) return;

    m_detailCode = code;
    m_detailLoading = !m_details.contains(code);

    // 非模态 + 复用窗口：切不同标的时直接换内容，也能一边看列表一边对照详情
    if (!m_detailDialog) {
        m_detailDialog = new QDialog(this);
        m_detailDialog->setObjectName(QStringLiteral("PlanDetailDialog"));
        m_detailDialog->resize(560, 700);
        auto* lay = new QVBoxLayout(m_detailDialog);
        lay->setContentsMargins(Theme::Space::Xl, Theme::Space::Lg,
                                Theme::Space::Xl, Theme::Space::Lg);
        lay->setSpacing(Theme::Space::Sm);

        m_thumb = new KlineThumbnail(m_detailDialog);
        m_thumb->setFixedHeight(170);
        lay->addWidget(m_thumb, 0);

        auto* scroll = new QScrollArea(m_detailDialog);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        m_detailBody = new QLabel(scroll);
        m_detailBody->setWordWrap(true);
        m_detailBody->setTextFormat(Qt::RichText);
        m_detailBody->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_detailBody->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_detailBody->setContentsMargins(4, 4, 12, 4);
        scroll->setWidget(m_detailBody);
        lay->addWidget(scroll, 1);

        auto* btnRow = new QWidget(m_detailDialog);
        auto* bh = new QHBoxLayout(btnRow);
        bh->setContentsMargins(0, 0, 0, 0);
        bh->setSpacing(Theme::Space::Sm);
        bh->addStretch(1);
        auto* refresh = new QPushButton(QStringLiteral("刷新数据"), btnRow);
        refresh->setObjectName(QStringLiteral("Ghost"));
        refresh->setToolTip(QStringLiteral("重新拉取财报 / 资金流 / 机构持仓"));
        connect(refresh, &QPushButton::clicked, this, [this]() {
            if (m_detailCode.isEmpty()) return;
            m_detailLoading = true;
            refreshCardDetail();
            Q_EMIT detailDataRequested(m_detailCode, true);
        });
        bh->addWidget(refresh);
        auto* close = new QPushButton(QStringLiteral("关闭"), btnRow);
        close->setObjectName(QStringLiteral("Primary"));
        connect(close, &QPushButton::clicked, m_detailDialog, &QDialog::hide);
        bh->addWidget(close);
        lay->addWidget(btnRow, 0);
    }

    refreshCardDetail();
    m_detailDialog->show();
    m_detailDialog->raise();
    m_detailDialog->activateWindow();

    if (m_detailLoading) Q_EMIT detailDataRequested(code, false);
}

// 数据到达 / 刷新数据 / 切换标的后重绘详情弹窗
void AiStockPanel::refreshCardDetail()
{
    if (!m_detailDialog || !m_detailBody || m_detailCode.isEmpty()) return;
    const TradePlan* found = nullptr;
    for (const TradePlan& p : m_filtered) {
        if (p.code == m_detailCode) { found = &p; break; }
    }
    if (!found) return;
    const TradePlan& p = *found;

    m_detailDialog->setWindowTitle(
        QStringLiteral("标的详情 · %1 %2").arg(p.code, p.name));
    if (m_thumb) m_thumb->setData(m_klines.value(p.code), p.buyLow, p.buyHigh,
                                  p.currentPrice);
    m_detailBody->setText(buildDetailHtml(p));
}

QString AiStockPanel::buildDetailHtml(const TradePlan& p) const
{
    auto esc = [](const QString& s) { return s.toHtmlEscaped(); };
    auto textOr = [](const QString& s, const QString& fallback) {
        return s.trimmed().isEmpty() ? fallback : s;
    };
    auto line = [&](const QString& key, const QString& value) {
        return QStringLiteral("<p style='margin:0 0 6px 0;'><b>%1</b>：%2</p>")
            .arg(esc(key), esc(value));
    };
    auto section = [&](const QString& title) {
        return QStringLiteral("<h4 style='margin:14px 0 6px 0;color:%1;'>%2</h4>")
            .arg(Theme::accentDeep().name(), esc(title));
    };
    auto hint = [&](const QString& text) {
        return QStringLiteral("<p style='margin:0 0 6px 0;color:%1;font-size:12px;'>%2</p>")
            .arg(Theme::textMuted().name(), esc(text));
    };

    QStringList h;
    h << QStringLiteral("<h3 style='margin:0 0 8px 0;'>%1　%2</h3>")
             .arg(esc(p.name), esc(p.code));
    h << line(QStringLiteral("状态"), p.statusTag()
                 + (p.actionable ? QStringLiteral("（可执行）")
                                 : QStringLiteral("（今日不开仓）")));
    h << line(QStringLiteral("当前价格"), p.currentPrice > 0.0
                 ? money(p.currentPrice) + QStringLiteral(" 元")
                 : QStringLiteral("暂无实时行情价"));
    h << line(QStringLiteral("推荐理由"), textOr(p.reasonLine(),
                                                  QStringLiteral("题材 / 板数 / 评级待补全")));

    // ---- 01 行业 / 板块 / 市值 ----
    h << section(QStringLiteral("01 · 所属行业 / 板块 / 市值规模"));
    h << line(QStringLiteral("所属行业（东财 hybk）"), textOr(p.sector, QStringLiteral("未标注")));
    h << line(QStringLiteral("所属板块"), boardName(boardOfCode(p.code)));
    h << line(QStringLiteral("题材 / 身位"),
              textOr(p.theme, QStringLiteral("未标注"))
                  + (p.themeRank > 0
                         ? QStringLiteral(" · 同题材第 %1 身位").arg(p.themeRank)
                         : QString()));
    const Snapshot snap = m_snapshots.value(p.code);
    if (snap.isValid()) {
        h << line(QStringLiteral("流通市值 / 总市值"),
                  yi(snap.floatCap) + QStringLiteral(" / ") + yi(snap.totalCap));
        h << line(QStringLiteral("市值规模"), capScale(snap.floatCap));
        h << line(QStringLiteral("PE / PB"),
                  (snap.pe > 0.0 ? QString::number(snap.pe, 'f', 2) : QStringLiteral("—"))
                      + QStringLiteral(" / ")
                      + (snap.pb > 0.0 ? QString::number(snap.pb, 'f', 2) : QStringLiteral("—")));
        h << line(QStringLiteral("换手率 / 成交额"),
                  pctText(snap.turnover, 2) + QStringLiteral(" / ") + yi(snap.amount));
    } else {
        h << hint(QStringLiteral("全市场快照尚未就绪，市值与估值数据暂不可见"));
    }

    // ---- 02 财报 ----
    h << section(QStringLiteral("02 · 近期财报核心数据"));
    const auto dit = m_details.constFind(p.code);
    const bool hasDetail = (dit != m_details.constEnd());
    if (!hasDetail) {
        h << hint(m_detailLoading
                      ? QStringLiteral("正在拉取最近一期财报…")
                      : QStringLiteral("暂无数据：点击「刷新数据」重新获取"));
        h << hint(QStringLiteral("营收增速 / 净利润增速 / 加权 ROE 来自东方财富财报接口"));
    } else if (dit.value().finance.valid) {
        const FinanceReport& f = dit.value().finance;
        h << line(QStringLiteral("报告期"), textOr(f.period, f.reportDate));
        h << line(QStringLiteral("营业收入 / 同比增速"),
                  yi(f.revenue) + QStringLiteral(" / ") + signPct(f.revenueYoy));
        h << line(QStringLiteral("归母净利润 / 同比增速"),
                  yi(f.netProfit) + QStringLiteral(" / ") + signPct(f.netProfitYoy));
        h << line(QStringLiteral("加权净资产收益率 ROE"), pctText(f.roe, 2));
    } else {
        h << hint(QStringLiteral("财报接口未返回数据")
                  + (dit.value().error.isEmpty() ? QString()
                     : QStringLiteral("（%1）").arg(dit.value().error)));
    }

    // ---- 03 K 线 / 均线 / 量能 ----
    h << section(QStringLiteral("03 · K 线走势 / 关键均线 / 成交量趋势"));
    h << hint(QStringLiteral("上方缩略图：最近 60 根日 K + MA5/20/60 + 买入区间底带 + 现价虚线，底部为成交量柱。"));
    const KlineSeries ks = m_klines.value(p.code);
    const QVector<Candle>& bars = ks.hasRaw() ? ks.rawCandles : ks.candles;
    if (!bars.isEmpty()) {
        QVector<double> closes;
        closes.reserve(bars.size());
        for (const Candle& c : bars) closes.append(c.close);
        const QVector<double> ma5 = Ind::sma(closes, 5);
        const QVector<double> ma20 = Ind::sma(closes, 20);
        const QVector<double> ma60 = Ind::sma(closes, 60);
        auto maText = [](const QVector<double>& v) {
            return (v.isEmpty() || !std::isfinite(v.last()))
                       ? QStringLiteral("—")
                       : QString::number(v.last(), 'f', 2);
        };
        auto maGap = [&](const QVector<double>& v) {
            if (p.currentPrice <= 0.0 || v.isEmpty() || !std::isfinite(v.last())
                || v.last() <= 0.0)
                return QStringLiteral("—");
            return signPct((p.currentPrice / v.last() - 1.0) * 100.0, 2);
        };
        h << line(QStringLiteral("MA5 / MA20 / MA60（元）"),
                  maText(ma5) + QStringLiteral(" / ") + maText(ma20)
                      + QStringLiteral(" / ") + maText(ma60));
        if (p.currentPrice > 0.0) {
            h << line(QStringLiteral("现价相对 MA5 / MA20 / MA60"),
                      maGap(ma5) + QStringLiteral(" / ") + maGap(ma20)
                          + QStringLiteral(" / ") + maGap(ma60));
        }
        const int n = bars.size();
        auto avgVol = [&](int start, int count) {
            if (start < 0 || count <= 0) return 0.0;
            double s = 0.0;
            int c = 0;
            for (int i = start; i < qMin(start + count, n); ++i) {
                s += bars.at(i).volume;
                ++c;
            }
            return c > 0 ? s / c : 0.0;
        };
        const double recent5 = avgVol(qMax(0, n - 5), 5);
        const double prev5 = avgVol(qMax(0, n - 10), 5);
        const double chg = prev5 > 0.0 ? (recent5 / prev5 - 1.0) * 100.0 : 0.0;
        const double beforeLast5 = avgVol(qMax(0, n - 6), 5);
        const double ratio = beforeLast5 > 0.0 ? bars.last().volume / beforeLast5 : 0.0;
        h << line(QStringLiteral("近 5 日 / 前 5 日均量"),
                  QString::number(recent5 / 1e4, 'f', 1) + QStringLiteral(" 万手 / ")
                      + QString::number(prev5 / 1e4, 'f', 1) + QStringLiteral(" 万手"));
        h << line(QStringLiteral("成交量变化趋势"),
                  volumeTrend(chg) + QStringLiteral("（") + signPct(chg, 1)
                      + QStringLiteral("）"));
        h << line(QStringLiteral("最新一日量比（对前 5 日均量）"),
                  ratio > 0.0 ? QString::number(ratio, 'f', 2) + QStringLiteral(" 倍")
                              : QStringLiteral("—"));
    } else {
        h << hint(QStringLiteral("暂无日 K 缓存，等待行情数据到位"));
    }
    h << line(QStringLiteral("当前价格与买入区间的关系"), priceVsBuyRange(p));

    // ---- 04 资金面 ----
    h << section(QStringLiteral("04 · 主力资金 / 大单占比 / 机构持仓"));
    if (!hasDetail) {
        h << hint(m_detailLoading
                      ? QStringLiteral("正在拉取主力资金、大单与机构持仓数据…")
                      : QStringLiteral("暂无数据：点击「刷新数据」重新获取"));
    } else {
        const StockDetailData& d = dit.value();
        if (!d.flows.isEmpty()) {
            const MoneyFlowDay& last = d.flows.last();
            h << line(QStringLiteral("最近交易日（%1）主力净流入")
                          .arg(last.date.toString(QStringLiteral("MM-dd"))),
                      flowMoney(last.mainNet) + QStringLiteral("（占成交额 ")
                          + signPct(last.mainPct) + QStringLiteral("）"));
            double sum5 = 0.0;
            int upDays = 0;
            for (int i = qMax(0, d.flows.size() - 5); i < d.flows.size(); ++i) {
                sum5 += d.flows.at(i).mainNet;
                if (d.flows.at(i).mainNet > 0.0) ++upDays;
            }
            h << line(QStringLiteral("近 5 日主力累计净额"),
                      flowMoney(sum5) + QStringLiteral("（净流入 ")
                          + QString::number(upDays) + QStringLiteral(" / 5 天）"));
            h << line(QStringLiteral("大单 / 超大单净额"),
                      flowMoney(last.bigNet) + QStringLiteral(" / ")
                          + flowMoney(last.superBigNet));
            h << line(QStringLiteral("大单 / 超大单占成交额比"),
                      signPct(last.bigPct) + QStringLiteral(" / ")
                          + signPct(last.superBigPct));
        } else {
            h << hint(QStringLiteral("资金流接口未返回数据"));
        }
        if (d.org.valid) {
            h << line(QStringLiteral("机构持仓（%1，季度）").arg(d.org.reportDate),
                      QString::number(d.org.orgNum) + QStringLiteral(" 家 / 占流通股 ")
                          + pctText(d.org.sharesRatio, 2) + QStringLiteral(" / 市值 ")
                          + yi(d.org.marketCap));
            h << line(QStringLiteral("机构家数变化 / 持股数变化"),
                      (d.org.orgNumChange >= 0 ? QStringLiteral("+") : QString())
                          + QString::number(d.org.orgNumChange) + QStringLiteral(" 家 / ")
                          + QString::number(d.org.sharesChange / 1e4, 'f', 0)
                          + QStringLiteral(" 万股"));
        } else {
            h << hint(QStringLiteral("机构持仓接口未返回数据（季度披露，部分标的无记录）"));
        }
        if (!d.error.isEmpty())
            h << hint(QStringLiteral("补充数据获取提示：%1").arg(d.error));
    }
    h << hint(QStringLiteral("北向资金：个股北向持股无公开日频数据源，当前不展示（不以估算值代替真实数据）"));

    // ---- 05 交易计划 ----
    h << section(QStringLiteral("05 · 交易计划与风险"));
    h << line(QStringLiteral("买入价格区间"), rangeStr(p.buyLow, p.buyHigh));
    h << line(QStringLiteral("买入依据"), textOr(p.buyBasis, QStringLiteral("定价依据正在补全")));
    h << line(QStringLiteral("卖出价格区间（止盈）"),
              rangeStr(p.takeProfitLow, p.takeProfitHigh));
    h << line(QStringLiteral("止盈依据"), textOr(p.tpBasis, QStringLiteral("定价依据正在补全")));
    h << line(QStringLiteral("止损价格区间"), rangeStr(p.stopLossLow, p.stopLossHigh));
    h << line(QStringLiteral("止损依据"), textOr(p.slBasis, QStringLiteral("定价依据正在补全")));
    h << line(QStringLiteral("建议仓位"), p.actionable
                 ? pctText(p.positionPct, 1) : QStringLiteral("0%（仅观察）"));
    h << line(QStringLiteral("盈亏比 / 止损幅度"),
              QStringLiteral("%1 : 1　/　%2")
                  .arg(p.riskReward > 0.0 ? QString::number(p.riskReward, 'f', 2)
                                          : QStringLiteral("—"),
                       pctText(p.riskPct, 2)));
    h << line(QStringLiteral("成交概率"), pctText(p.fillProbability * 100.0)
                 + QStringLiteral("　") + textOr(p.fillNote, QString()));
    h << line(QStringLiteral("竞价结论"), p.auctionEvaluated
                 ? Auction::verdictName(static_cast<AuctionAdvice::Verdict>(p.auctionVerdict))
                 : QStringLiteral("可接 %1 ~ %2")
                       .arg(money(p.auctionBuyLow), money(p.auctionBuyHigh)));
    h << line(QStringLiteral("风险标记"), p.riskFlags.isEmpty()
                 ? QStringLiteral("无特别风险标记") : p.riskFlags.join(QStringLiteral(" / ")));
    h << line(QStringLiteral("风险提示"), textOr(p.riskNote, QStringLiteral("无")));
    h << line(QStringLiteral("评级 / 评分"),
              textOr(p.grade, QStringLiteral("—")) + QStringLiteral(" / ")
                  + QString::number(p.score, 'f', 0));
    if (!p.note.isEmpty()) h << line(QStringLiteral("备注"), p.note);

    h << QStringLiteral("<hr>");
    h << hint(QStringLiteral("数据来源：财报 / 资金流 / 机构持仓来自东方财富公开接口（按需加载，10 分钟缓存）；"
                             "行业题材来自涨停池与概念板块接口；K 线、均线与量能来自本地日 K 缓存；"
                             "市值与估值来自新浪全市场快照。主力口径 = 大单 + 超大单。"));
    return h.join(QString());
}

// ---- 详情弹窗的数据注入（由 MainWindow 转发）----
void AiStockPanel::setMarketData(const QHash<QString, KlineSeries>& klines,
                                 const QHash<QString, Snapshot>& snapshots)
{
    m_klines = klines;
    m_snapshots = snapshots;
    if (m_detailDialog && !m_detailDialog->isHidden()) refreshCardDetail();
}

void AiStockPanel::setSnapshots(const QHash<QString, Snapshot>& snapshots)
{
    m_snapshots = snapshots;
    if (m_detailDialog && !m_detailDialog->isHidden()) refreshCardDetail();
}

void AiStockPanel::setStockKline(const QString& code, const KlineSeries& series)
{
    if (code.isEmpty() || !series.valid()) return;
    m_klines.insert(code, series);
    if (m_detailDialog && !m_detailDialog->isHidden() && m_detailCode == code)
        refreshCardDetail();
}

void AiStockPanel::setStockDetail(const QString& code, const StockDetailData& data)
{
    m_details.insert(code, data);
    if (code == m_detailCode) m_detailLoading = false;
    if (m_detailDialog && !m_detailDialog->isHidden() && m_detailCode == code)
        refreshCardDetail();
}

// 设置加载态：更新结论卡与状态标签
void AiStockPanel::setLoading(const QString& text)
{
    m_headline->setText(text);                       // 显示加载进度描述
    m_headline->setStyleSheet(Theme::textStyle(Theme::text(), Theme::fontTitle(), true));
    m_reason->clear();                               // 结论尚未生成，清空上一轮的结论要素
    m_meta->clear();
    m_freshness->clear();
    m_verdict->clear();
    m_verdict->setToolTip(QString());
    if (m_reasonToggle) m_reasonToggle->setVisible(false);
    m_status->setText(QStringLiteral("加载中…"));      // 状态标签同步显示加载态
    m_status->setStyleSheet(QString());              // 清除可能残留的错误告警色
}

// 设置错误态：在状态标签显示红色告警信息
void AiStockPanel::setError(const QString& text)
{
    m_status->setText(QStringLiteral("⚠ ") + text);  // 前缀警告符号，增强辨识度
    m_status->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::danger().name())); // 使用主题危险色
}

// 接收完整的荐股数据包，刷新界面
void AiStockPanel::setBundle(const RecommendBundle& bundle)
{
    m_bundle = bundle;                               // 缓存原始数据，供后续筛选复用
    m_status->setStyleSheet(QString());              // 清除之前的错误/警告样式
    m_shownPool = bundle.plans;
    for (const TradePlan& p : bundle.backups) {
        bool dup = false;
        for (const TradePlan& q : m_shownPool) {
            if (q.code == p.code) { dup = true; break; }
        }
        if (!dup) m_shownPool.append(p);
    }
    applyConcentratedPicks(m_shownPool);

    updateKindFilterCounts();                        // 筛选项带上实时数量（全部/可执行/观察）
    applyFilters();                                  // 基于新数据重新执行筛选并重建表格

    // 判定原文只留一句摘要，完整内容悬停可查；避免长文本把结论卡撑高
    m_verdict->setText(bundle.notice.isEmpty()
        ? QString()
        : QStringLiteral("判定说明：%1").arg(shortBasis(bundle.notice)));
    m_verdict->setToolTip(bundle.notice);
    m_verdict->setStyleSheet(QStringLiteral("color:%1;font-size:12px;")
                             .arg(Theme::textMuted().name()));
    // 没有原文就不给折叠入口（不留空按钮）
    if (m_reasonToggle) {
        m_reasonToggle->setVisible(!m_verdict->text().isEmpty());
        if (m_verdict->text().isEmpty()) {
            m_reasonToggle->setChecked(false);
            m_verdict->setVisible(false);
        }
    }

    updateDecisionCard();                            // ① 白话结论 + ④ 数据新鲜度

    // 「打开即见」：无选中行时默认选中建议仓位最高的一只（卡片高亮与按钮提示依赖它）
    if (currentCode().isEmpty() && !m_filtered.isEmpty()) {
        const TradePlan* best = &m_filtered.first();
        for (const TradePlan& p : m_filtered) {
            if (p.positionPct > best->positionPct) best = &p;
        }
        selectCode(best->code);
    }
}

// ① 今日结论卡 + ④ 数据新鲜度角标
void AiStockPanel::updateDecisionCard()
{
    const RecommendBundle& b = m_bundle;
    int act = 0;
    for (const TradePlan& p : b.plans) if (p.actionable) ++act;
    const int obs = int(b.plans.size()) - act;

    QString head, why;
    QColor headColor = Theme::success();
    if (b.cached) {
        head = QStringLiteral("这是上次保存的结果，今日请勿据此下单");
        why = QStringLiteral("网络尚未就绪，先展示已保存的计划；拿到最新行情后会自动更新，"
                             "期间所有标的都只作「观察」，仓位为 0。");
        headColor = Theme::warning();
    } else if (b.regime.valid() && !b.regime.tradeAllowed) {
        head = QStringLiteral("今日建议空仓");
        why = plainGateReason(b.regime);
        headColor = Theme::danger();
    } else if (act > 0) {
        head = QStringLiteral("今日可以出手 · %1 只可执行").arg(act);
        why = QStringLiteral("市场情绪满足出手条件。下方卡片按建议仓位排序，先看仓位最高的那一只；"
                             "最终买点以次日竞价为准。");
    } else {
        head = QStringLiteral("今日暂无可执行标的");
        why = QStringLiteral("市场情绪满足出手条件，但没有标的通过全部分层筛选。");
        headColor = Theme::warning();
    }

    m_headline->setText(head);
    m_headline->setStyleSheet(QStringLiteral("font-size:16px;font-weight:600;color:%1;")
                              .arg(headColor.name()));
    m_reason->setText(why);

    const QString meta = QStringLiteral("可执行 %1 只 ｜ 观察 %2 只 ｜ 最晚决策：次日 9:25 竞价")
                             .arg(act).arg(obs);
    m_meta->setText(meta);

    // 角标：缓存 / 回退 / 非实时 / 保守估计用暖色底，正常态用中性底
    const QString fresh = bundleFreshnessSummary(b);
    const bool stale = b.cached || fresh.contains(QStringLiteral("回退"))
                    || fresh.contains(QStringLiteral("非实时"))
                    || fresh.contains(QStringLiteral("保守估计"));
    m_freshness->setText(QStringLiteral("数据状态：%1").arg(fresh));
    m_freshness->setStyleSheet(stale
        ? QStringLiteral("color:#8f4314;background:#fbeadb;border-radius:6px;"
                         "padding:4px 8px;font-size:12px;")
        : QStringLiteral("color:#5b6470;background:#f2f4f7;border-radius:6px;"
                         "padding:4px 8px;font-size:12px;"));
}

// 刷新筛选下拉框三项的数量标签：让「切过去有几只」在选择前就可见
void AiStockPanel::updateKindFilterCounts()
{
    if (!m_kindFilter) return;
    int act = 0;
    for (const TradePlan& p : m_shownPool) if (p.actionable) ++act;
    const int total = m_shownPool.size();
    const int cur = m_kindFilter->currentIndex();
    QSignalBlocker blocker(m_kindFilter);            // 改文本不触发 currentIndexChanged
    m_kindFilter->setItemText(0, QStringLiteral("全部（%1）").arg(total));
    m_kindFilter->setItemText(1, QStringLiteral("仅可执行（%1）").arg(act));
    m_kindFilter->setItemText(2, QStringLiteral("仅观察（%1）").arg(total - act));
    if (m_kindFilter->currentIndex() != cur) m_kindFilter->setCurrentIndex(cur);
}

// 根据当前筛选条件过滤数据并重建表格
void AiStockPanel::applyFilters()
{
    QVector<TradePlan> shown;                        // 存储通过筛选的计划列表
    const int kind = m_kindFilter->currentIndex();   // 0=全部, 1=仅可执行, 2=仅观察
    const QString kw = m_keyword->text().trimmed();  // 去除首尾空白的搜索关键词

    for (const TradePlan& p : m_shownPool) {         // 遍历候选池（plans + 备选）进行过滤
        if (kind == 1 && !p.actionable) continue;    // 仅可执行模式：跳过观察标的
        if (kind == 2 && p.actionable) continue;     // 仅观察模式：跳过可执行标的
        if (!kw.isEmpty()) {                         // 有关键词时执行模糊匹配
            if (!p.code.contains(kw) && !p.name.contains(kw) // 代码和名称均不匹配
                && !p.sector.contains(kw) && !p.theme.contains(kw)) // 板块和题材也不匹配
                continue;                            // 四项均不包含关键词则跳过
        }
        shown.append(p);                             // 通过所有筛选条件，加入结果集
    }
    m_filtered = shown;                              // 更新已过滤数据缓存
    rebuildTable();                                  // 用过滤后的数据重建表格
}

// 根据 m_filtered 重建表格内容
void AiStockPanel::rebuildTable()
{
    if (!m_table) return;                            // 防御性检查，表格未初始化则直接返回

    const QString keep = currentCode();              // 记住当前选中行的股票代码，重建后恢复选中

    m_table->setSortingEnabled(false);               // 填充期间关闭排序，避免每插入一行都触发重排
    m_table->setRowCount(m_filtered.size());         // 一次性设定行数，减少动态扩容开销
    for (int i = 0; i < m_filtered.size(); ++i) {    // 逐行填充数据
        const TradePlan& p = m_filtered[i];          // 当前行的交易计划引用

        // lambda：封装单元格创建逻辑，统一设置显示文本、排序键、颜色、对齐与提示
        auto setCell = [&](int col, const QString& text, const QVariant& sortKey,
                           const QColor& color,
                           Qt::Alignment align = Qt::AlignVCenter | Qt::AlignLeft,
                           const QString& tip = QString()) {
            auto* item = new QTableWidgetItem;       // 创建新的表格项
            item->setData(Qt::DisplayRole, sortKey); // DisplayRole 存排序键，供委托读取后排序
            item->setData(kPlanTextRole, text);      // 自定义角色存显示文本，与排序键解耦
            item->setData(Qt::UserRole, p.code);     // UserRole 存股票代码，用于行标识与选中恢复
            item->setTextAlignment(align);           // 设置文本对齐方式
            if (color.isValid()) item->setForeground(color); // 有效颜色时设置前景色
            if (!tip.isEmpty()) item->setToolTip(tip);       // 非空时设置悬浮提示
            m_table->setItem(i, col, item);          // 将单元格放入表格指定位置
        };

        // 构建悬浮提示：将次要信息（盈亏比/成交概率/竞价/风险/备注）聚合到 tooltip
        QStringList tip;                             // 提示文本行列表
        if (!p.reasonLine().isEmpty())  tip << p.reasonLine(); // 推荐理由摘要
        if (p.riskReward > 0.0)         tip << QStringLiteral("盈亏比 %1 : 1（保守）")
                                                .arg(p.riskReward, 0, 'f', 2); // 盈亏比
        tip << QStringLiteral("成交概率 %1%").arg(p.fillProbability * 100, 0, 'f', 0); // 成交概率百分比
        tip << QStringLiteral("单笔止损幅度 %1%").arg(p.riskPct, 0, 'f', 2); // 单笔最大亏损比例
        if (p.auctionEvaluated) {                    // 已有竞价评估结论时显示具体判定
            tip << QStringLiteral("竞价 %1 %2%")
                       .arg(Auction::verdictName(static_cast<AuctionAdvice::Verdict>(p.auctionVerdict)))
                       .arg(p.auctionOpenPct, 0, 'f', 2); // 竞价结论名 + 开盘涨幅
        } else {                                     // 尚未评估时显示可接价格区间
            tip << QStringLiteral("竞价可接 %1 ~ %2")
                       .arg(money(p.auctionBuyLow), money(p.auctionBuyHigh));
        }
        if (!p.riskFlags.isEmpty())     tip << QStringLiteral("风险：") + p.riskFlags.join(QStringLiteral(" / ")); // 风险标签
        if (!p.note.isEmpty())          tip << p.note; // 额外备注
        const QString tooltip = tip.join(QStringLiteral("\n")); // 各行用换行符拼接成完整提示

        // 代码列：居中对齐，tooltip 包含全部次要信息
        setCell(ColCode, p.code, p.code, Theme::text(), Qt::AlignVCenter | Qt::AlignCenter, tooltip);
        // 名称列：左对齐，共享同一份 tooltip
        setCell(ColName, p.name, p.name, Theme::text(), Qt::AlignVCenter | Qt::AlignLeft, tooltip);

        // 推荐理由列：可执行用正常色，观察用弱化色；排序键为综合评分
        setCell(ColReason, p.reasonLine(), p.score,
                p.actionable ? Theme::text() : Theme::textMuted(),
                Qt::AlignVCenter | Qt::AlignLeft, tooltip);

        // 买入价列：显示价格区间，排序键取区间中值，强调色突出挂单参考
        setCell(ColBuy, rangeStr(p.buyLow, p.buyHigh), (p.buyLow + p.buyHigh) * 0.5,
                Theme::accent(), Qt::AlignVCenter | Qt::AlignRight,
                QStringLiteral("次日挂单价格带\n%1").arg(p.buyBasis)); // tooltip 补充定价依据
        // 目标价列：绿色表示盈利方向，tooltip 补充止盈依据
        setCell(ColTake, rangeStr(p.takeProfitLow, p.takeProfitHigh),
                (p.takeProfitLow + p.takeProfitHigh) * 0.5,
                Theme::up(), Qt::AlignVCenter | Qt::AlignRight,
                QStringLiteral("目标价（止盈区间）\n%1").arg(p.tpBasis));
        // 止损价列：红色表示风险方向，tooltip 强调无条件离场纪律
        setCell(ColStop, rangeStr(p.stopLossLow, p.stopLossHigh),
                (p.stopLossLow + p.stopLossHigh) * 0.5,
                Theme::down(), Qt::AlignVCenter | Qt::AlignRight,
                QStringLiteral("跌破止损上沿无条件离场\n%1").arg(p.slBasis));

        // 仓位列：可执行显示建议仓位百分比，观察固定为 0%；排序键为原始数值
        setCell(ColPosition, p.actionable
                ? QStringLiteral("%1%").arg(p.positionPct, 0, 'f', 1)
                : QStringLiteral("0%"),
                p.positionPct, p.actionable ? Theme::warning() : Theme::flat(),
                Qt::AlignVCenter | Qt::AlignRight,
                p.actionable ? QStringLiteral("建议单票仓位（占总资金）")
                             : QStringLiteral("仅观察：今日不开仓，仓位恒为 0"));

        {
            const double rr = p.riskReward;
            QColor c = Theme::textMuted();
            if (rr > 0.0) {
                if (rr < 1.00)      c = Theme::danger();
                else if (rr < 1.50) c = Theme::warning();
                else                c = Theme::success();
            }
            setCell(ColRiskReward,
                    rr > 0.0 ? QString::number(rr, 'f', 2) : QStringLiteral("—"),
                    rr, c, Qt::AlignVCenter | Qt::AlignRight,
                    rr > 0.0 ? QStringLiteral("盈亏比 %1 : 1（保守口径：止盈下沿兑现）").arg(rr, 0, 'f', 2)
                             : QStringLiteral("盈亏比未算出（区间数据缺失）"));
        }
        // 单笔止损幅度列：结构止损过远（riskTooHigh）标红，其余按距离深浅提示
        {
            const QColor c = p.riskTooHigh ? Theme::danger()
                             : (p.riskPct >= 4.0 ? Theme::warning() : Theme::text());
            setCell(ColRiskPct, pctText(p.riskPct, 2), p.riskPct, c,
                    Qt::AlignVCenter | Qt::AlignRight,
                    p.riskTooHigh
                        ? QStringLiteral("单笔止损幅度 %1%（结构止损过远，已降级）").arg(p.riskPct, 0, 'f', 2)
                        : QStringLiteral("单笔止损幅度 %1%（买入中枢 → 止损中枢）").arg(p.riskPct, 0, 'f', 2));
        }
        // 成交概率列：<30% 红 / <60% 黄 / 其余绿（打板常买不到，低概率必须显性）
        {
            const double prob = p.fillProbability * 100.0;
            const QColor c = prob < 30.0 ? Theme::danger()
                             : (prob < 60.0 ? Theme::warning() : Theme::success());
            const QString note = p.fillNote.isEmpty()
                ? QStringLiteral("成交概率 %1%").arg(prob, 0, 'f', 0)
                : QStringLiteral("成交概率 %1%\n依据：%2").arg(prob, 0, 'f', 0).arg(p.fillNote);
            setCell(ColFillProb, pctText(prob), prob, c,
                    Qt::AlignVCenter | Qt::AlignRight, note);
        }

        // 状态列：可执行绿色标签，观察灰色标签；排序键 1/0 使可执行排在前面
        setCell(ColStatus, p.statusTag(), p.actionable ? 1 : 0,
                p.actionable ? Theme::success() : Theme::flat(),
                Qt::AlignVCenter | Qt::AlignCenter, p.note);

        // 观察名单整行暖白底：与可执行标的形成视觉分区，避免混淆
        if (!p.actionable) {
            const QBrush bg(QColor(253, 251, 244));  // 柔和暖白背景（克制分区，不抢数据）
            for (int c = 0; c < ColCount; ++c) {     // 遍历该行所有列
                if (QTableWidgetItem* it = m_table->item(i, c)) it->setBackground(bg); // 设置背景色
            }
        }
    }
    m_table->setSortingEnabled(true);                // 数据填充完毕，重新启用排序

    // 校验当前排序列是否合法，非法时回退到默认的仓位降序
    QHeaderView* hh = m_table->horizontalHeader();
    int col = hh->sortIndicatorSection();            // 获取当前排序列索引
    if (col < 0 || col >= ColCount) {                // 列索引越界或无效
        col = ColPosition;                           // 回退到仓位列
        hh->setSortIndicator(col, Qt::DescendingOrder); // 重置为仓位降序
    }
    m_table->sortItems(col, hh->sortIndicatorOrder()); // 按当前排序规则手动触发一次排序

    if (m_emptyHint) {
        m_emptyHint->setVisible(m_filtered.isEmpty());
        const int kind = m_kindFilter->currentIndex();
        const QString kw = m_keyword->text().trimmed();
        QString hint;
        if (m_shownPool.isEmpty()) {
            hint = QStringLiteral("暂无荐股结果\n市场情绪未达出手条件且无合格观察标的，或行情数据尚未到达");
        } else if (kind == 1) {
            hint = QStringLiteral("今日没有可执行标的\n可切回「全部」查看观察名单");
        } else if (kind == 2) {
            hint = QStringLiteral("没有观察标的\n可切回「全部」查看可执行清单");
        } else if (!kw.isEmpty()) {
            hint = QStringLiteral("没有匹配「%1」的标的\n试试其它关键词，或清除筛选条件").arg(kw);
        } else {
            hint = QStringLiteral("当前筛选条件下无标的");
        }
        m_emptyHint->setText(hint);
        if (m_cardEmpty) m_cardEmpty->setText(hint);   // 卡片区空态沿用同一文案
    }

    int act = 0, obs = 0;
    for (const TradePlan& p : m_shownPool) {
        if (p.actionable) ++act; else ++obs;
    }
    const int kind = m_kindFilter->currentIndex();
    const QString kw = m_keyword->text().trimmed();
    m_status->setStyleSheet(QString());              // 复位状态标签样式，清除可能的告警色
    if (kind == 0 && kw.isEmpty()) {
        m_status->setText(QStringLiteral("共 %1 只（可执行 %2 / 观察 %3）")
                          .arg(m_shownPool.size()).arg(act).arg(obs));
    } else {
        QStringList cond;
        if (kind == 1)      cond << QStringLiteral("仅可执行");
        else if (kind == 2) cond << QStringLiteral("仅观察");
        if (!kw.isEmpty())  cond << QStringLiteral("含「%1」").arg(kw);
        m_status->setText(QStringLiteral("显示 %1 / %2 只 · %3")
                              .arg(m_filtered.size()).arg(m_shownPool.size())
                              .arg(cond.join(QStringLiteral(" + "))));
    }

    // 恢复之前选中的行；若该标的已被筛选掉则保持无选中状态
    if (!keep.isEmpty()) selectCode(keep);
    updateActionTips();                              // 根据当前选中行更新按钮 tooltip
    rebuildCards();                                  // 可见结果：按当前行序重建卡片列表
}

// 获取当前选中行对应的交易计划，未选中或无效时返回默认构造的空计划
TradePlan AiStockPanel::currentPlan() const
{
    const QString code = currentCode();              // 获取当前选中行的股票代码
    if (code.isEmpty()) return TradePlan();          // 无选中行，返回无效计划
    for (const TradePlan& p : m_filtered) {          // 在已过滤列表中查找匹配计划
        if (p.code == code) return p;                // 找到则返回副本
    }
    return TradePlan();                              // 代码存在但列表中未找到（不应发生），返回无效计划
}

// 获取当前选中行的股票代码，通过 UserRole 中存储的代码标识
QString AiStockPanel::currentCode() const
{
    if (!m_table) return QString();                  // 表格未初始化
    const int row = m_table->currentRow();           // 获取当前行索引
    if (row < 0 || row >= m_table->rowCount()) return QString(); // 行索引越界
    QTableWidgetItem* it = m_table->item(row, ColCode); // 取代码列的单元格
    if (!it) return QString();                       // 单元格不存在
    return it->data(Qt::UserRole).toString();        // 从 UserRole 提取股票代码
}

// 按股票代码选中对应行，用于重建表格后恢复之前的选中状态
void AiStockPanel::selectCode(const QString& code)
{
    if (!m_table || code.isEmpty()) return;          // 前置条件检查
    for (int r = 0; r < m_table->rowCount(); ++r) {  // 逐行扫描查找目标代码
        QTableWidgetItem* it = m_table->item(r, ColCode); // 取代码列单元格
        if (!it) continue;                           // 单元格为空则跳过
        if (it->data(Qt::UserRole).toString() != code) continue; // 代码不匹配则跳过
        m_table->setCurrentCell(r, ColCode);         // 设置当前单元格焦点
        m_table->selectRow(r);                       // 高亮选中整行
        return;                                      // 找到后立即返回，无需继续扫描
    }
}

// 根据当前选中行更新「记录买入」按钮的 tooltip
void AiStockPanel::updateActionTips()
{
    const TradePlan p = currentPlan();               // 获取当前选中的交易计划
    if (m_btnTrade) {
        m_btnTrade->setToolTip(p.valid()             // 有效计划显示具体标的信息
            ? QStringLiteral("把 %1 %2 记入交易日志（带计划买点/止损/止盈，用于事后复盘）")
                  .arg(p.code, p.name)
            : QStringLiteral("请先点击一张选股卡片，再点「记录买入」")); // 无效时提示先选中
    }
}

// 在状态栏显示黄色警告，提示用户需要先选中标的才能执行某操作
void AiStockPanel::warnNoSelection(const QString& action)
{
    m_status->setText(QStringLiteral("⚠ 请先点击一张选股卡片，再点「%1」").arg(action)); // 带操作名的警告文案
    m_status->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::warning().name())); // 使用主题警告色
}

// 「重新分析」按钮点击处理：发射刷新信号，由外部控制器执行实际的数据拉取与计算
void AiStockPanel::onRefreshClicked()
{
    Q_EMIT refreshRequested();                       // 通知外部重新执行分析流程
}

// ★ 选中行联动：按钮提示 + 卡片高亮
void AiStockPanel::onRowSelected()
{
    updateActionTips();                              // 仅更新按钮 tooltip，不做页面跳转
    updateCardSelection();                           // 选中行变化 → 对应卡片高亮
}
