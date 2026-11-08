#include "ui/DisclaimerDialog.h"

#include "core/LegalNotice.h"
#include "ui/Components.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDebug>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {

QString acceptedAtText()
{
    QSettings settings(LegalNotice::settingsOrganization(),
                       LegalNotice::settingsApplication());
    const QString iso = settings.value(LegalNotice::acceptedAtKey()).toString();
    if (iso.trimmed().isEmpty()) return QString();
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : iso;
}

} // namespace

DisclaimerDialog::DisclaimerDialog(Mode mode, QWidget* parent)
    : QDialog(parent), m_mode(mode)
{
    setObjectName(QStringLiteral("DisclaimerDialog"));
    setWindowTitle(QStringLiteral("免责声明与风险揭示"));
    setModal(mode == Mode::Consent);
    setSizeGripEnabled(true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    // 尺寸按可用屏幕收敛：1366×768 的笔记本也能完整看到正文与底部同意区
    const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800);
    const int w = qBound(680, avail.width() - 120, 920);
    const int h = qBound(520, avail.height() - 100, 860);
    resize(w, h);
    setMinimumSize(qMin(680, w), qMin(520, h));

    buildUi();
}

void DisclaimerDialog::buildUi()
{
    const LegalNotice::Document& doc = LegalNotice::document();
    const bool consent = (m_mode == Mode::Consent);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ---------------------------------------------------------------- 页头
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("DisclaimerHeader"));
    auto* hv = new QVBoxLayout(header);
    hv->setContentsMargins(Theme::Space::Xxl, Theme::Space::Xl,
                           Theme::Space::Xxl, Theme::Space::Lg);
    hv->setSpacing(Theme::Space::Sm);

    auto* topRow = new QWidget(header);
    auto* th = new QHBoxLayout(topRow);
    th->setContentsMargins(0, 0, 0, 0);
    th->setSpacing(Theme::Space::Sm);

    auto* brand = new QLabel(doc.product, topRow);
    brand->setObjectName(QStringLiteral("BrandTitle"));
    th->addWidget(brand, 0);

    th->addWidget(Comp::badge(QStringLiteral("合规必读"), Comp::BadgeTone::Danger), 0);
    th->addStretch(1);

    auto* meta = new QLabel(QStringLiteral("版本 %1 ｜ 生效日期 %2")
                                .arg(doc.version, doc.effectiveDate), topRow);
    meta->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));
    th->addWidget(meta, 0);
    hv->addWidget(topRow);

    auto* title = new QLabel(doc.title, header);
    title->setStyleSheet(Theme::textStyle(Theme::text(), 22, true));
    hv->addWidget(title);

    auto* subtitle = new QLabel(
        consent
            ? QStringLiteral("请完整阅读以下条款。未勾选同意前，本软件不会展示任何选股、行情或分析内容。")
            : QStringLiteral("以下为当前生效的声明全文。您可随时回到本页核对条款与本机同意记录。"),
        header);
    subtitle->setObjectName(QStringLiteral("Muted"));
    subtitle->setWordWrap(true);
    hv->addWidget(subtitle);

    // 醒目风险条：一句话说清「可能亏光本金 / 不构成投资建议」
    auto* banner = new QFrame(header);
    banner->setObjectName(QStringLiteral("DisclaimerBanner"));
    auto* bh = new QHBoxLayout(banner);
    bh->setContentsMargins(Theme::Space::Md, Theme::Space::Sm + 2,
                           Theme::Space::Md, Theme::Space::Sm + 2);
    bh->setSpacing(Theme::Space::Sm);

    auto* bannerTag = new QLabel(QStringLiteral("重要提示"), banner);
    bannerTag->setStyleSheet(Theme::textStyle(Theme::danger(), Theme::fontSm(), true));
    bh->addWidget(bannerTag, 0, Qt::AlignTop);

    auto* bannerText = new QLabel(doc.summary, banner);
    bannerText->setWordWrap(true);
    bannerText->setStyleSheet(Theme::textStyle(Theme::text(), Theme::fontMd(), false));
    bh->addWidget(bannerText, 1);
    hv->addWidget(banner);

    outer->addWidget(header, 0);

    // ---------------------------------------------------------------- 声明正文
    auto* body = new QTextBrowser(this);
    body->setObjectName(QStringLiteral("DisclaimerBody"));
    body->setFrameShape(QFrame::NoFrame);
    body->setOpenExternalLinks(false);
    body->setOpenLinks(true);      // 支持文内目录跳转
    body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 正文是阅读区：只在点击时取焦点（不自动聚焦、不显示文本光标），
    // 初始焦点留给同意勾选框，键盘用户不会误落到「同意并继续」上。
    body->setFocusPolicy(Qt::ClickFocus);
    body->document()->setDocumentMargin(22);
    body->setHtml(renderHtml());
    outer->addWidget(body, 1);

    // ---------------------------------------------------------------- 页脚：同意 / 关闭
    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("DisclaimerFooter"));
    auto* fv = new QVBoxLayout(footer);
    fv->setContentsMargins(Theme::Space::Xxl, Theme::Space::Md,
                           Theme::Space::Xxl, Theme::Space::Lg);
    fv->setSpacing(Theme::Space::Md);

    if (consent) {
        m_consentCard = new QFrame(footer);
        m_consentCard->setObjectName(QStringLiteral("DisclaimerConsent"));
        auto* cv = new QVBoxLayout(m_consentCard);
        cv->setContentsMargins(Theme::Space::Lg, Theme::Space::Md,
                               Theme::Space::Lg, Theme::Space::Md);
        cv->setSpacing(Theme::Space::Xs + 2);

        m_agree = new QCheckBox(
            QStringLiteral("我已阅读并理解上述《免责声明与风险揭示》全部内容，并同意受其约束"),
            m_consentCard);
        m_agree->setObjectName(QStringLiteral("DisclaimerAgree"));
        m_agree->setCursor(Qt::PointingHandCursor);
        cv->addWidget(m_agree);

        auto* note = new QLabel(
            QStringLiteral("勾选即表示您知悉：股票交易可能导致本金全部损失，所有投资决策及后果由您自行承担。"),
            m_consentCard);
        note->setWordWrap(true);
        note->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));
        cv->addWidget(note);

        connect(m_agree, &QCheckBox::toggled, this, [this](bool) { updateConsentState(); });
        fv->addWidget(m_consentCard);
    }

    auto* actionRow = new QWidget(footer);
    auto* ah = new QHBoxLayout(actionRow);
    ah->setContentsMargins(0, 0, 0, 0);
    ah->setSpacing(Theme::Space::Sm);

    m_hint = new QLabel(actionRow);
    m_hint->setWordWrap(true);
    ah->addWidget(m_hint, 1);

    if (consent) {
        auto* reject = new QPushButton(QStringLiteral("不同意并退出"), actionRow);
        reject->setObjectName(QStringLiteral("Ghost"));
        reject->setMinimumSize(124, 40);
        reject->setAutoDefault(false);
        reject->setCursor(Qt::PointingHandCursor);
        connect(reject, &QPushButton::clicked, this, &QDialog::reject);
        ah->addWidget(reject, 0);

        m_accept = new QPushButton(QStringLiteral("同意并继续"), actionRow);
        m_accept->setObjectName(QStringLiteral("Primary"));
        // 未勾选时必须一眼看出「不可点」：禁用态走灰底灰字，不用强调色冒充可点
        m_accept->setProperty("consentAction", true);
        m_accept->setMinimumSize(132, 40);
        m_accept->setAutoDefault(false);   // 必须显式点击，避免回车误确认
        m_accept->setCursor(Qt::PointingHandCursor);
        connect(m_accept, &QPushButton::clicked, this, &DisclaimerDialog::acceptConsent);
        ah->addWidget(m_accept, 0);
    } else {
        QSettings settings(LegalNotice::settingsOrganization(),
                           LegalNotice::settingsApplication());
        const QString ver = settings.value(LegalNotice::acceptedVersionKey()).toString();
        const QString at  = acceptedAtText();
        m_hint->setText(at.isEmpty()
            ? QStringLiteral("本机暂无同意记录。")
            : QStringLiteral("已同意版本 %1 ｜ 确认时间 %2（记录仅保存在本机）").arg(ver, at));
        m_hint->setStyleSheet(Theme::textStyle(Theme::textMuted(), Theme::fontSm(), false));

        auto* close = new QPushButton(QStringLiteral("关闭"), actionRow);
        close->setObjectName(QStringLiteral("Primary"));
        close->setMinimumSize(96, 40);
        close->setAutoDefault(false);
        close->setCursor(Qt::PointingHandCursor);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        ah->addWidget(close, 0);
    }

    fv->addWidget(actionRow);
    outer->addWidget(footer, 0);

    if (consent) updateConsentState();
}

QString DisclaimerDialog::renderHtml() const
{
    const LegalNotice::Document& d = LegalNotice::document();
    const QString cText      = Theme::text().name();
    const QString cMuted     = Theme::textMuted().name();
    const QString cAccent    = Theme::accent().name();
    const QString cBorder    = Theme::border().name();
    const QString cTocBg     = Theme::bg().name();

    auto esc = [](const QString& s) { return s.toHtmlEscaped(); };
    auto para = [&cText, &esc](const QString& text) {
        return QStringLiteral(
                   "<p style=\"font-size:13px;color:%1;margin:0 0 8px 0;line-height:175%;\">%2</p>")
            .arg(cText, esc(text));
    };

    QString h;
    h += QStringLiteral(
             "<html><body style=\"font-family:'Microsoft YaHei UI','Microsoft YaHei',sans-serif;"
             "font-size:13px;color:%1;margin:0;\">").arg(cText);

    // 页头风险条常驻可见，正文不再重复同一段摘要，直接从协议导语开始
    h += para(d.lead);

    // 文内目录：六部分一屏可见，点击跳转
    QStringList links;
    for (int i = 0; i < d.blocks.size(); ++i) {
        links << QStringLiteral("<a href=\"#s%1\" style=\"color:%2;text-decoration:none;\">%3</a>")
                     .arg(QString::number(i), cAccent, esc(d.blocks.at(i).heading));
    }
    h += QStringLiteral(
             "<div style=\"background-color:%1;padding:10px 14px;margin:0 0 6px 0;"
             "font-size:12px;color:%2;line-height:185%;\"><b>本声明共 %3 部分：</b><br/>%4</div>")
             .arg(cTocBg, cMuted, QString::number(d.blocks.size()),
                  links.join(QStringLiteral("　·　")));

    for (int i = 0; i < d.blocks.size(); ++i) {
        const LegalNotice::Block& b = d.blocks.at(i);
        h += QStringLiteral(
                 "<a name=\"s%1\"></a>"
                 "<div style=\"font-size:15px;font-weight:600;color:%2;"
                 "margin:22px 0 8px 0;padding-bottom:6px;border-bottom:1px solid %3;\">%4</div>")
                 .arg(QString::number(i), cText, cBorder, esc(b.heading));

        for (const QString& p : b.paragraphs) h += para(p);

        if (!b.items.isEmpty()) {
            h += QStringLiteral("<ul style=\"margin:4px 0 2px 0;padding-left:22px;\">");
            for (const QString& item : b.items) {
                // 条目统一写成「标签：说明」，标签加粗，长段落仍由 QTextBrowser 自动换行
                const int sep = item.indexOf(QStringLiteral("："));
                const QString label = sep > 0 ? item.left(sep) : QString();
                const QString rest  = sep > 0 ? item.mid(sep + 1) : item;
                h += QStringLiteral(
                         "<li style=\"font-size:13px;color:%1;margin:0 0 8px 0;line-height:175%;\">")
                         .arg(cText);
                if (!label.isEmpty())
                    h += QStringLiteral("<b>%1</b>：").arg(esc(label));
                h += esc(rest) + QStringLiteral("</li>");
            }
            h += QStringLiteral("</ul>");
        }
    }

    h += QStringLiteral(
             "<div style=\"margin-top:22px;padding-top:12px;border-top:1px solid %1;"
             "font-size:11px;color:%2;line-height:180%;\">%3<br/>"
             "适用产品：%4 ｜ 版本 %5 ｜ 生效日期 %6</div>")
             .arg(cBorder, cMuted, esc(d.footer), esc(d.product), d.version, d.effectiveDate);

    h += QStringLiteral("</body></html>");
    return h;
}

void DisclaimerDialog::updateConsentState()
{
    if (!m_agree || !m_accept) return;

    const bool agreed = m_agree->isChecked();
    m_accept->setEnabled(agreed);
    m_accept->setToolTip(agreed ? QStringLiteral("确认已阅读并同意，进入软件")
                                : QStringLiteral("请先勾选上方同意选项"));

    if (m_hint) {
        m_hint->setText(agreed
            ? QStringLiteral("已确认同意，可继续使用本软件。")
            : QStringLiteral("未同意前不会展示任何项目内容；请先勾选同意选项。"));
        m_hint->setStyleSheet(Theme::textStyle(agreed ? Theme::success() : Theme::textMuted(),
                                               Theme::fontSm(), agreed));
    }

    if (m_consentCard) {
        // 勾选后整块变为「通过」底色：状态不只靠按钮可用性表达
        m_consentCard->setProperty("agreed", agreed);
        m_consentCard->style()->unpolish(m_consentCard);
        m_consentCard->style()->polish(m_consentCard);
        m_consentCard->update();
    }
}

void DisclaimerDialog::acceptConsent()
{
    if (!m_agree || !m_agree->isChecked()) return;

    QSettings settings(LegalNotice::settingsOrganization(),
                       LegalNotice::settingsApplication());
    settings.setValue(LegalNotice::acceptedVersionKey(), LegalNotice::document().version);
    settings.setValue(LegalNotice::acceptedAtKey(),
                      QDateTime::currentDateTime().toString(Qt::ISODate));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        // 落盘失败不阻断本次使用，但下次启动会再次提示确认
        qWarning().noquote()
            << QStringLiteral("[合规] 免责声明同意记录写入失败，下次启动将重新提示");
    }

    m_consented = true;
    accept();
}

bool DisclaimerDialog::isAccepted()
{
    QSettings settings(LegalNotice::settingsOrganization(),
                       LegalNotice::settingsApplication());
    const QString version = settings.value(LegalNotice::acceptedVersionKey()).toString();
    const QString at      = settings.value(LegalNotice::acceptedAtKey()).toString();
    return version == LegalNotice::document().version && !at.trimmed().isEmpty();
}

bool DisclaimerDialog::ensureAccepted(QWidget* parent)
{
    if (isAccepted()) return true;

    DisclaimerDialog dlg(Mode::Consent, parent);
    // 无父窗口时按可用屏幕居中，避免由窗口管理器丢到屏幕角落
    if (!parent && dlg.screen()) {
        const QRect avail = dlg.screen()->availableGeometry();
        dlg.move(avail.center() - QPoint(dlg.width() / 2, dlg.height() / 2));
    }
    return dlg.exec() == QDialog::Accepted && dlg.consented();
}

void DisclaimerDialog::showDocument(QWidget* parent)
{
    auto* dlg = new DisclaimerDialog(Mode::Document, parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}
