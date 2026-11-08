#include "ui/StockCard.h"
#include "ui/Theme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFontMetrics>
#include <QStyle>

namespace {

QString money(double v)
{
    return QString::number(v, 'f', 2);
}

QString rangeText(double lo, double hi)
{
    return money(lo) + QStringLiteral(" ~ ") + money(hi);
}

QString pctText(double v, int digits = 0)
{
    return QStringLiteral("%1%").arg(v, 0, 'f', digits);
}

} // namespace

StockCard::StockCard(const TradePlan& plan, QWidget* parent)
    : QFrame(parent), m_code(plan.code), m_name(plan.name)
{
    setObjectName(QStringLiteral("StockCard"));
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(0);                 // 窄屏（单列）时允许卡片被压到容器宽度以内
    setCursor(Qt::PointingHandCursor);
    setProperty("selected", false);     // 动态属性驱动 QSS 选中态

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::Space::Lg, Theme::Space::Lg,
                             Theme::Space::Lg, Theme::Space::Lg);
    root->setSpacing(Theme::Space::Sm);

    // ---- 第一行：名称 / 代码 / 状态 ----
    auto* head = new QWidget(this);
    auto* headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(Theme::Space::Sm);

    auto* name = new QLabel(plan.name, head);
    name->setObjectName(QStringLiteral("StockName"));
    name->setToolTip(plan.name);
    headLay->addWidget(name, 0);

    auto* code = new QLabel(plan.code, head);
    code->setObjectName(QStringLiteral("StockCode"));
    headLay->addWidget(code, 0);
    headLay->addStretch(1);

    auto* status = new QLabel(plan.actionable
                                  ? QStringLiteral("可执行") : QStringLiteral("仅观察"), head);
    status->setAlignment(Qt::AlignCenter);
    status->setStyleSheet(Theme::chipStyle(
        plan.actionable ? Theme::success() : Theme::neutral(),
        plan.actionable ? Theme::successSoft() : Theme::neutralSoft(),
        Theme::fontXs(), 4, 8, true));
    headLay->addWidget(status, 0, Qt::AlignRight);
    root->addWidget(head);

    // ---- 第二行：推荐理由（单行省略，完整内容放 tooltip）----
    m_reasonFull = plan.reasonLine().trimmed().isEmpty()
                       ? QStringLiteral("题材 / 板数 / 评级待补全")
                       : plan.reasonLine();
    m_reason = new QLabel(this);
    m_reason->setObjectName(QStringLiteral("Muted"));
    m_reason->setWordWrap(false);
    m_reason->setToolTip(m_reasonFull);
    m_reason->setText(m_reasonFull);
    root->addWidget(m_reason);

    root->addWidget(divider());

    // ---- 当前价格（主读数）+ 建议仓位 ----
    auto* priceRow = new QWidget(this);
    auto* priceLay = new QHBoxLayout(priceRow);
    priceLay->setContentsMargins(0, 0, 0, 0);
    priceLay->setSpacing(Theme::Space::Md);

    auto* priceBox = new QWidget(priceRow);
    auto* priceBoxLay = new QVBoxLayout(priceBox);
    priceBoxLay->setContentsMargins(0, 0, 0, 0);
    priceBoxLay->setSpacing(2);
    auto* priceCaption = new QLabel(QStringLiteral("当前价格（元）"), priceBox);
    priceCaption->setObjectName(QStringLiteral("CardLabel"));
    priceBoxLay->addWidget(priceCaption);
    auto* priceValue = new QLabel(plan.currentPrice > 0.0
                                      ? money(plan.currentPrice)
                                      : QStringLiteral("暂无实时价"), priceBox);
    priceValue->setObjectName(QStringLiteral("CardPrice"));
    priceValue->setToolTip(plan.currentPrice > 0.0
                               ? QStringLiteral("行情快照中的最新价")
                               : QStringLiteral("本地缓存未含实时价；联网刷新后会自动更新"));
    priceBoxLay->addWidget(priceValue);
    priceLay->addWidget(priceBox, 0);

    priceLay->addStretch(1);
    auto* position = new QLabel(QStringLiteral("建议仓位 %1").arg(
        plan.actionable ? pctText(plan.positionPct, 1) : QStringLiteral("0%（仅观察）")), priceRow);
    position->setObjectName(QStringLiteral("CardLabel"));
    position->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    priceLay->addWidget(position, 0, Qt::AlignRight | Qt::AlignBottom);
    root->addWidget(priceRow);

    // ---- 三段核心价格区间 ----
    auto* metrics = new QGridLayout();
    metrics->setContentsMargins(0, 0, 0, 0);
    metrics->setHorizontalSpacing(Theme::Space::Md);
    metrics->setVerticalSpacing(Theme::Space::Xs);
    metrics->setColumnStretch(1, 1);
    addMetric(metrics, 0, QStringLiteral("买入价格区间"),
              rangeText(plan.buyLow, plan.buyHigh), Theme::accent());
    addMetric(metrics, 1, QStringLiteral("卖出价格区间"),
              rangeText(plan.takeProfitLow, plan.takeProfitHigh), Theme::up());
    addMetric(metrics, 2, QStringLiteral("止损价格区间"),
              rangeText(plan.stopLossLow, plan.stopLossHigh), Theme::down());
    root->addLayout(metrics);

    // ---- 一行补充读数：盈亏比 / 成交概率 / 评分 ----
    auto* extra = new QLabel(QStringLiteral("盈亏比 %1 : 1 ｜ 成交概率 %2 ｜ 评分 %3 %4")
                                 .arg(plan.riskReward > 0.0
                                          ? QString::number(plan.riskReward, 'f', 2)
                                          : QStringLiteral("—"),
                                      pctText(plan.fillProbability * 100.0),
                                      QString::number(plan.score, 'f', 0),
                                      plan.grade), this);
    extra->setObjectName(QStringLiteral("Muted"));
    extra->setWordWrap(false);
    root->addWidget(extra);

    root->addWidget(divider());

    // ---- 底部两个操作按钮 ----
    auto* actions = new QWidget(this);
    auto* actionsLay = new QHBoxLayout(actions);
    actionsLay->setContentsMargins(0, 0, 0, 0);
    actionsLay->setSpacing(Theme::Space::Sm);

    m_btnDetail = new QPushButton(QStringLiteral("查看详情"), actions);
    m_btnDetail->setObjectName(QStringLiteral("CardDetailButton"));
    m_btnDetail->setMinimumWidth(0);
    m_btnDetail->setMinimumHeight(36);
    m_btnDetail->setCursor(Qt::PointingHandCursor);
    m_btnDetail->setToolTip(QStringLiteral("查看完整的定价依据、竞价结论与风险提示"));
    actionsLay->addWidget(m_btnDetail, 1);

    m_btnKline = new QPushButton(QStringLiteral("看K线"), actions);
    // 不用 RowKlineButton：那是旧版已移除的行内按钮名，冒烟测试会检查它不存在
    m_btnKline->setObjectName(QStringLiteral("CardKlineButton"));
    m_btnKline->setMinimumWidth(0);
    m_btnKline->setMinimumHeight(36);
    m_btnKline->setCursor(Qt::PointingHandCursor);
    m_btnKline->setToolTip(QStringLiteral("打开该标的的长周期日 K（可切换周 / 月与指标）"));
    actionsLay->addWidget(m_btnKline, 1);

    root->addWidget(actions);

    connect(m_btnDetail, &QPushButton::clicked, this,
            [this]() { Q_EMIT detailRequested(m_code); });
    connect(m_btnKline, &QPushButton::clicked, this,
            [this]() { Q_EMIT klineRequested(m_code, m_name); });

    elideReason();
}

QFrame* StockCard::divider()
{
    auto* line = new QFrame(this);
    line->setObjectName(QStringLiteral("CardDivider"));
    line->setFrameShape(QFrame::HLine);
    line->setLineWidth(0);
    line->setMidLineWidth(0);
    line->setFixedHeight(1);
    line->setAttribute(Qt::WA_StyledBackground, true);
    return line;
}

void StockCard::addMetric(QGridLayout* grid, int row, const QString& label,
                          const QString& value, const QColor& color)
{
    auto* key = new QLabel(label, this);
    key->setObjectName(QStringLiteral("CardLabel"));
    key->setMinimumWidth(96);
    grid->addWidget(key, row, 0, Qt::AlignVCenter | Qt::AlignLeft);

    auto* val = new QLabel(value, this);
    val->setObjectName(QStringLiteral("CardValue"));
    val->setStyleSheet(Theme::textStyle(color, Theme::fontMd(), true));
    val->setToolTip(label + QStringLiteral("：") + value);
    grid->addWidget(val, row, 1, Qt::AlignVCenter | Qt::AlignRight);
}

void StockCard::setSelected(bool selected)
{
    if (m_selected == selected) return;
    m_selected = selected;
    setProperty("selected", selected);
    // 动态属性变化后必须重新 polish，QSS 属性选择器才会重新求值
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void StockCard::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) Q_EMIT cardClicked(m_code);
    QFrame::mousePressEvent(e);
}

void StockCard::resizeEvent(QResizeEvent* e)
{
    QFrame::resizeEvent(e);
    elideReason();
}

void StockCard::elideReason()
{
    if (!m_reason) return;
    const QFontMetrics fm(m_reason->font());
    m_reason->setText(fm.elidedText(m_reasonFull, Qt::ElideRight,
                                    qMax(60, m_reason->width())));
}
