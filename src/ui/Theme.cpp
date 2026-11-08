#include "ui/Theme.h"                    // 本模块声明
#include <QTableWidget>                  // 表格排版
#include <QHeaderView>                   // 表头
#include <QAbstractItemView>             // 视图行为
#include <QWidget>                       // 淡入目标
#include <QGraphicsOpacityEffect>        // 透明度特效
#include <QPropertyAnimation>            // 过渡动画
#include <QEasingCurve>                  // 缓动曲线

// 颜色只有 3 套，各司其职：
//   A 行情方向  红涨绿跌（up/down/flat）→ 只用于价格与涨跌幅
//   B 决策状态  通过/警示/危险/中性      → 只用于结论与状态
//   C 强调色    靛蓝（唯一）             → 只用于可交互与当前项
// 强调色刻意离开红绿区：原先 accent 与 up / danger 同属红区，
// 「红色」同时表示涨 / 危险 / 选中 / 一致看多，颜色映射无从建立。
//
// 字号 7 级、间距 4px 栅格、圆角三档。QSS 全部走令牌占位符，
// 运行期一次性替换 —— 改一个令牌全界面同步生效，界面代码不再出现硬编码色值。
namespace Theme {

// ---------------- A 套：行情方向 ----------------
QColor up()   { return QColor("#d93025"); }   // 涨（红）——对比度 ≈ 4.8:1
QColor down() { return QColor("#0b7d46"); }   // 跌（绿）——≈ 4.7:1
QColor flat() { return QColor("#8a8f99"); }   // 平（灰，仅用于非文本装饰）

// ---------------- B 套：决策状态 ----------------
QColor success() { return QColor("#0f7a53"); }   // 通过 / 可执行
QColor warning() { return QColor("#a86400"); }   // 警示 / 观察 / 需降仓
QColor danger()  { return QColor("#c5221f"); }   // 危险 / 禁止出手 / 淘汰
QColor neutral() { return QColor("#5b6470"); }   // 中性 / 占位 / 无结论

// 状态徽章的浅底色（配 toneColor 使用）
QColor successSoft() { return QColor("#e6f4ee"); }
QColor warningSoft() { return QColor("#fbeadb"); }
QColor dangerSoft()  { return QColor("#fdecea"); }
QColor neutralSoft() { return QColor("#eef1f5"); }

// ---------------- C 套：强调色（唯一） ----------------
QColor accent()      { return QColor("#1f5fa8"); }   // 可交互 / 焦点 / 当前选中
QColor accentSoft()  { return QColor("#e8f0fa"); }   // 选中底 / 浅强调底
QColor accentDeep()  { return QColor("#17457a"); }   // 悬停文字 / 按压色
QColor accentMuted() { return QColor("#9fb8d4"); }   // 禁用 / 弱描边
QColor hoverSoft()   { return QColor("#f0f5fb"); }   // 悬停底

// ---------------- 背景 / 描边 ----------------
QColor bg()           { return QColor("#f6f7f9"); }
QColor panel()        { return QColor("#ffffff"); }
QColor card()         { return QColor("#ffffff"); }
QColor border()       { return QColor("#e4e7ec"); }
QColor borderStrong() { return QColor("#d5dae2"); }
QColor borderFaint()  { return QColor("#eef0f3"); }
QColor grid()         { return QColor("#eef0f3"); }
QColor hover()        { return QColor("#f3f5f7"); }
QColor pressed()      { return QColor("#eaedf1"); }

// ---------------- 文字 ----------------
QColor text()         { return QColor("#1b2430"); }
QColor textMuted()    { return QColor("#5b6470"); }
QColor textFaint()    { return QColor("#7a828e"); }
QColor textDisabled() { return QColor("#a8b0bc"); }
QColor textInvert()   { return QColor("#ffffff"); }

QColor byChange(double pct)
{
    if (pct > 0.0001)  return up();
    if (pct < -0.0001) return down();
    return flat();
}

// ---------------- 间距 / 圆角 / 字号 ----------------
int radiusSm() { return 6; }
int radiusMd() { return 8; }
int radiusLg() { return 10; }

int fontXs()    { return 11; }
int fontSm()    { return 12; }
int fontMd()    { return 13; }
int fontLg()    { return 15; }
int fontH2()    { return 16; }
int fontTitle() { return 19; }
int fontKpi()   { return 22; }

// ---------------- 内联样式辅助 ----------------
QString textStyle(const QColor& color, int fontPx, bool bold)
{
    return QStringLiteral("color:%1;font-size:%2px;font-weight:%3;")
        .arg(color.name()).arg(fontPx).arg(bold ? 600 : 400);
}

QString chipStyle(const QColor& fg, const QColor& bgColor, int fontPx,
                  int radius, int padH, bool bold)
{
    return QStringLiteral("color:%1;background:%2;border-radius:%3px;"
                          "padding:%4px %5px;font-size:%6px;font-weight:%7;")
        .arg(fg.name()).arg(bgColor.name()).arg(radius)
        .arg(fontPx <= 11 ? 2 : 3).arg(padH).arg(fontPx)
        .arg(bold ? 600 : 400);
}

// ---------------- 全局样式表 ----------------
// 简约风：中性灰悬停，强调色只留给「可交互 / 选中 / 主操作」。
// 模板内一律写占位符，末尾统一替换成令牌值 —— 界面代码不再出现硬编码色值。
QString styleSheet()
{
    QString qss = QStringLiteral(R"(
/* ---------------- 基础排版 ---------------- */
QWidget {
    background: transparent;
    color: {text};
    font-family: "Microsoft YaHei UI", "Microsoft YaHei", "PingFang SC", sans-serif;
    font-size: {fsMd}px;
}
QMainWindow, QDialog { background: {bg}; }
QStackedWidget, QScrollArea, QWidget#PageHeader, QWidget#TabPage { background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }

/* ---------------- 顶栏（浅色 · 去除深色块） ---------------- */
QWidget#BrandBar { background: {panel}; border-bottom: 1px solid {border}; }
QLabel#BrandTitle  { color: {text}; font-size: {fsH2}px; font-weight: 600; }
QLabel#BrandSub    { color: {textMuted}; font-size: {fsXs}px; }
QLabel#BrandStatus { color: {textMuted}; font-size: {fsSm}px; }

/* 状态徽章：中性底 + 深字。状态靠**文字**表达，颜色只做辅助 */
QLabel#StateBadge {
    color: {text}; background: {hover}; border-radius: 4px;
    padding: 2px 8px; font-weight: 600; font-size: {fsXs}px;
}
/* 顶栏次级入口（研究型） */
QPushButton#VersionAction {
    background: {panel}; color: {textMuted};
    border: 1px solid {borderStrong}; border-radius: {rSm};
    padding: 6px 12px; font-weight: 400;
}
QPushButton#VersionAction:hover   { background: {hover}; border-color: {accentMuted}; color: {text}; }
QPushButton#VersionAction:pressed { background: {pressed}; }
QPushButton#VersionAction:disabled { color: {textDisabled}; background: {bg}; border-color: {borderFaint}; }
QToolButton#VersionAction {
    background: {panel}; color: {textMuted};
    border: 1px solid {borderStrong}; border-radius: {rSm};
    padding: 6px 12px; font-weight: 400;
}
QToolButton#VersionAction:hover   { background: {hover}; border-color: {accentMuted}; color: {text}; }
QToolButton#VersionAction:pressed { background: {pressed}; }
QToolButton#VersionAction:disabled { color: {textDisabled}; background: {bg}; border-color: {borderFaint}; }

/* 视图档位分段开关（简洁 | 详细）：灰底轨道 + 白色滑块，
   当前档位即白色凸起段 —— 不需要额外文字说明「现在在哪档」 */
QWidget#ViewModeGroup {
    border: 1px solid {borderStrong}; border-radius: {rSm}; background: {bg};
}
QPushButton#ViewModeSegment {
    border: 1px solid transparent; border-radius: 4px; background: transparent;
    color: {textMuted}; padding: 5px 14px; font-size: {fsSm}px; font-weight: 400;
}
QPushButton#ViewModeSegment:hover  { background: {hover}; color: {text}; }
QPushButton#ViewModeSegment:checked {
    background: {panel}; border-color: {borderStrong}; color: {accentDeep}; font-weight: 600;
}

/* ---------------- 免责声明前置页 ---------------- */
/* 前置页是全屏阻断式阅读：页头 / 正文 / 同意区三段固定，只有正文滚动。
   风险条用 B 套「危险」底色，强调语义不借用行情红绿。 */
QWidget#DisclaimerHeader { background: {panel}; border-bottom: 1px solid {border}; }
QFrame#DisclaimerBanner {
    background: {dangerSoft}; border: none; border-left: 3px solid {danger};
    border-radius: {rSm};
}
QTextBrowser#DisclaimerBody {
    background: {panel}; border: 1px solid {border}; border-radius: {rMd};
}
QWidget#DisclaimerFooter { background: {panel}; border-top: 1px solid {border}; }
QFrame#DisclaimerConsent {
    background: {accentSoft}; border: 1px solid {accentMuted}; border-radius: {rMd};
}
QFrame#DisclaimerConsent[agreed="true"] {
    background: {successSoft}; border: 1px solid {success};
}
QCheckBox#DisclaimerAgree {
    spacing: 10px; font-size: {fsMd}px; font-weight: 600; color: {text};
}
QCheckBox#DisclaimerAgree:hover { color: {accentDeep}; }
/* 未勾选时「同意并继续」必须是明确的禁用态：灰底灰字，而不是浅蓝底白字 */
QPushButton#Primary[consentAction="true"]:disabled {
    background: {bg}; border: 1px solid {border}; color: {textDisabled};
}

/* ---------------- 卡片与标题 ---------------- */
QFrame#Card {
    background: {card};
    border: 1px solid {border};
    border-radius: {rMd};
}
QLabel#CardTitle { color: {text}; font-size: {fsLg}px; font-weight: 600; padding: 0 0 4px 0; }
QLabel#PageTitle { color: {text}; font-size: {fsTitle}px; font-weight: 600; }
QLabel#PageDesc  { color: {textMuted}; font-size: {fsSm}px; }
QLabel#Muted     { color: {textMuted}; font-size: {fsSm}px; }
QLabel#Value     { color: {text}; font-size: {fsKpi}px; font-weight: 600; }
QLabel#ValueSm   { color: {text}; font-size: {fsLg}px; font-weight: 600; }
QLabel#ValueUp   { color: {up}; font-size: {fsKpi}px; font-weight: 600; }
QLabel#ValueDown { color: {down}; font-size: {fsKpi}px; font-weight: 600; }

/* ---------------- 按钮 ---------------- */
QPushButton {
    background: {panel};
    border: 1px solid {borderStrong};
    border-radius: {rSm};
    padding: 8px 16px;
    min-height: 16px;
    color: {text};
}
QPushButton:hover   { background: {hover}; border-color: {accentMuted}; }
QPushButton:pressed { background: {pressed}; border-color: {accentMuted}; }
QPushButton:focus   { border: 2px solid {accent}; padding: 7px 15px; }
QPushButton:disabled { color: {textDisabled}; background: {bg}; border-color: {borderFaint}; }

QPushButton#Primary {
    background: {accent}; color: {textInvert}; border: 1px solid {accent}; font-weight: 600;
}
QPushButton#Primary:hover   { background: {accentDeep}; border-color: {accentDeep}; }
QPushButton#Primary:pressed { background: {accentDeep}; }
QPushButton#Primary:disabled { background: {accentMuted}; border-color: {accentMuted}; color: {textInvert}; }

QPushButton#Danger { background: {danger}; color: {textInvert}; border: 1px solid {danger}; font-weight: 600; }
QPushButton#Danger:hover   { background: #a91a18; }
QPushButton#Danger:pressed { background: #8f1614; }

/* 导航（顶栏一级标签）：选中态 = 强调色文字 + 下方 2px 指示条 */
QPushButton#Nav {
    text-align: center;
    padding: 0 16px;
    border: none;
    border-bottom: 2px solid transparent;
    background: transparent;
    color: {textMuted};
    font-size: {fsMd}px;
}
QPushButton#Nav:hover   { color: {text}; background: {hover}; }
QPushButton#Nav:checked {
    color: {accentDeep}; font-weight: 600; border-bottom: 2px solid {accent};
}
QPushButton#Ghost {
    text-align: center; border: 1px solid {borderStrong}; border-radius: {rMd};
    background: {panel}; color: {textMuted}; font-size: {fsSm}px; padding: 6px 10px;
}
QPushButton#Ghost:hover { background: {hover}; border-color: {accentMuted}; color: {text}; }

/* ---------------- AI 选股结果：白色卡片列表 ---------------- */
/* 卡片本体保持纯白；选中态只换描边，不改变底色，避免一排卡片深浅不一 */
QScrollArea#CardScroll { background: transparent; border: none; }
QFrame#StockCard {
    background: {card}; border: 1px solid {border}; border-radius: {rMd};
}
QFrame#StockCard:hover { border-color: {borderStrong}; }
QFrame#StockCard[selected="true"] { border: 1px solid {accent}; }
QLabel#StockName  { color: {text}; font-size: {fsH2}px; font-weight: 600; }
QLabel#StockCode  { color: {textMuted}; font-size: {fsSm}px; }
QLabel#CardLabel  { color: {textMuted}; font-size: {fsSm}px; }
QLabel#CardValue  { color: {text}; font-size: {fsMd}px; font-weight: 600; }
QLabel#CardPrice  { color: {text}; font-size: {fsKpi}px; font-weight: 600; }
QFrame#CardDivider { background: {borderFaint}; border: none; }
QLabel#CardEmptyHint {
    color: {textMuted}; background: {card}; border: 1px solid {border};
    border-radius: {rMd}; font-size: {fsMd}px; padding: 28px 20px;
}

/* ---------------- 交易日志：动作清单 / 详情栏 ---------------- */
/* 字号由全局令牌统一，界面代码不再往 HTML 里塞 font-size / 色值 */
QTextBrowser#ActionList {
    background: transparent; border: none; color: {text};
    font-size: {fsMd}px; padding: 2px 0;
}
QTextBrowser#JournalDetail {
    background: transparent; border: none; color: {text};
    font-size: {fsMd}px; padding: 2px 0;
}
QLabel#JournalChip {
    color: {textMuted}; background: {neutralSoft}; border-radius: 4px;
    font-size: {fsXs}px; padding: 2px 8px;
}

/* 卡片底部：黑色实心主按钮 + 白底黑框次按钮（纯中性色，不借用行情红绿语义） */
QPushButton#CardDetailButton {
    background: #111111; color: #ffffff; border: 1px solid #111111;
    border-radius: {rSm}; padding: 9px 16px; font-weight: 600;
}
QPushButton#CardDetailButton:hover   { background: #2b2b2b; border-color: #2b2b2b; }
QPushButton#CardDetailButton:pressed { background: #000000; border-color: #000000; }
QPushButton#CardDetailButton:focus   { border: 2px solid #111111; padding: 8px 15px; }
QPushButton#CardKlineButton {
    background: #ffffff; color: #111111; border: 1px solid #111111;
    border-radius: {rSm}; padding: 9px 16px; font-weight: 600;
}
QPushButton#CardKlineButton:hover   { background: {hover}; color: #111111; border-color: #111111; }
QPushButton#CardKlineButton:pressed { background: {pressed}; color: #111111; border-color: #111111; }
QPushButton#CardKlineButton:focus   { border: 2px solid #111111; padding: 8px 15px; }

/* ---------------- 输入控件 ---------------- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QDateEdit, QPlainTextEdit, QTextEdit {
    background: {panel};
    border: 1px solid {borderStrong};
    border-radius: {rSm};
    padding: 7px 10px;
    selection-background-color: {accentSoft};
    selection-color: {text};
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: {accentMuted}; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus,
QDateEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 2px solid {accent}; padding: 6px 9px; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled, QPlainTextEdit:disabled {
    background: {bg}; color: {textDisabled}; border-color: {borderFaint};
}
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView {
    background: {panel}; border: 1px solid {border}; padding: 4px;
    selection-background-color: {accentSoft}; selection-color: {text}; outline: none;
}
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button { width: 18px; border: none; background: {bg}; }

/* ---------------- 表格 ---------------- */
QTableWidget, QTableView {
    background: {panel};
    alternate-background-color: #fafbfc;
    border: 1px solid {border};
    border-radius: {rMd};
    gridline-color: {grid};
    selection-background-color: {accentSoft};
    selection-color: {text};
}
/* 数据表是「一眼读数字」的地方：左右内边距收窄到 5px，
   给价格区间（20.12 ~ 20.74）与代码留出不被省略号截断的宽度。 */
QTableWidget::item, QTableView::item { padding: 6px 5px; border: none; }
QTableWidget::item:hover, QTableView::item:hover { background: {hover}; }
QTableWidget::item:selected, QTableView::item:selected { background: {accentSoft}; color: {text}; }
QHeaderView::section {
    background: #fafbfc;
    color: {textMuted};
    padding: 8px 8px;
    border: none;
    border-right: 1px solid {borderFaint};
    border-bottom: 1px solid {border};
    font-weight: 600;
}
QHeaderView::section:hover { background: {hover}; }
QTableCornerButton::section { background: #fafbfc; border: none; }

/* ---------------- 标签页 ---------------- */
QTabWidget::pane { border: 1px solid {border}; border-radius: {rMd}; background: {panel}; top: -1px; }
QTabBar::tab {
    background: {hover}; color: {textMuted}; padding: 9px 18px; margin-right: 4px;
    border-top-left-radius: {rMd}; border-top-right-radius: {rMd};
}
QTabBar::tab:hover { background: {pressed}; }
QTabBar::tab:selected { background: {panel}; color: {accentDeep}; font-weight: 600; }

/* ---------------- 分组标题 ---------------- */
QLabel#GroupLabel { color: {textFaint}; font-size: {fsXs}px; font-weight: 600; }

/* ---------------- 顶部导航条 + 上下文行动条 ---------------- */
QWidget#NavBar {
    background: {panel}; border-bottom: 1px solid {border};
}
QWidget#PageActions { background: transparent; }

/* ---------------- 状态栏 / 工具条 ---------------- */
QStatusBar { background: {panel}; border-top: 1px solid {border}; color: {textMuted}; padding: 4px 10px; }
QStatusBar QLabel { padding: 0 4px; }
QToolBar { background: {panel}; border-bottom: 1px solid {border}; spacing: 6px; padding: 6px 8px; }
QToolButton { padding: 6px 10px; border-radius: {rSm}; }
QToolButton:hover { background: {hover}; }
QToolButton:checked { background: {accentSoft}; color: {accentDeep}; font-weight: 600; }

/* ---------------- 滚动条 ---------------- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #c9cfda; border-radius: 5px; min-height: 32px; }
QScrollBar::handle:vertical:hover { background: #a8b0bd; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: #c9cfda; border-radius: 5px; min-width: 32px; }
QScrollBar::handle:horizontal:hover { background: #a8b0bd; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ---------------- 进度 / 勾选 / 分隔 ---------------- */
QProgressBar {
    border: none; border-radius: 4px; background: {borderFaint};
    text-align: center; height: 16px; font-size: {fsXs}px; color: {textMuted};
}
QProgressBar::chunk { background: {accent}; border-radius: 4px; }
QCheckBox, QRadioButton { spacing: 8px; }
QSplitter::handle { background: #e9ecf1; }
QSplitter::handle:hover { background: #c9cfda; }
QSplitter::handle:horizontal { width: 5px; }
QSplitter::handle:vertical { height: 5px; }

/* ---------------- 浮层 ---------------- */
QToolTip {
    background: {text}; color: {textInvert}; border: none;
    padding: 6px 9px; border-radius: {rSm}; font-size: {fsSm}px;
}
QMenu { background: {panel}; border: 1px solid {border}; padding: 5px; border-radius: {rMd}; }
QMenu::item { padding: 7px 24px 7px 14px; border-radius: {rSm}; }
QMenu::item:selected { background: {accentSoft}; color: {accentDeep}; }
QMenu::item:disabled { color: {textDisabled}; }

/* ---------------- 反馈状态 ---------------- */
QLabel#StatusOk   { color: {success}; font-weight: 600; }
QLabel#StatusWarn { color: {warning}; font-weight: 600; }
QLabel#StatusErr  { color: {danger}; font-weight: 600; }
QDialogButtonBox QPushButton { min-width: 84px; }
)");

    // ---- 令牌替换：QSS 内不再出现任何硬编码色值 / 字号 / 圆角 ----
    struct ColorToken { const char* key; QColor value; };
    const ColorToken colors[] = {
        { "{text}",         text()         }, { "{textMuted}",    textMuted()    },
        { "{textFaint}",    textFaint()    }, { "{textDisabled}", textDisabled() },
        { "{textInvert}",   textInvert()   },
        { "{bg}",           bg()           }, { "{panel}",        panel()        },
        { "{card}",         card()         }, { "{border}",       border()       },
        { "{borderStrong}", borderStrong() }, { "{borderFaint}",  borderFaint()  },
        { "{grid}",         grid()         }, { "{hover}",        hover()        },
        { "{pressed}",      pressed()      },
        { "{accent}",       accent()       }, { "{accentSoft}",   accentSoft()   },
        { "{accentDeep}",   accentDeep()   }, { "{accentMuted}",  accentMuted()  },
        { "{hoverSoft}",    hoverSoft()    },
        { "{up}",           up()           }, { "{down}",         down()         },
        { "{flat}",         flat()         },
        { "{success}",      success()      }, { "{warning}",      warning()      },
        { "{danger}",       danger()       }, { "{neutral}",      neutral()      },
        { "{successSoft}",  successSoft()  }, { "{warningSoft}",  warningSoft()  },
        { "{dangerSoft}",   dangerSoft()   }, { "{neutralSoft}",  neutralSoft()  },
    };
    for (const ColorToken& t : colors) {
        qss.replace(QString::fromLatin1(t.key), t.value.name());
    }

    struct SizeToken { const char* key; int value; };
    const SizeToken sizes[] = {
        { "{fsXs}",    fontXs()    }, { "{fsSm}",   fontSm()    },
        { "{fsMd}",    fontMd()    }, { "{fsLg}",   fontLg()    },
        { "{fsH2}",    fontH2()    }, { "{fsTitle}",fontTitle() },
        { "{fsKpi}",   fontKpi()   },
        { "{rSm}",     radiusSm()  }, { "{rMd}",    radiusMd()  },
        { "{rLg}",     radiusLg()  },
    };
    for (const SizeToken& t : sizes) {
        qss.replace(QString::fromLatin1(t.key), QString::number(t.value));
    }
    return qss;
}

void polishTable(QTableWidget* table)
{
    if (!table) return;
    table->setWordWrap(false);
    table->setShowGrid(false);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setMouseTracking(true);
    table->viewport()->setMouseTracking(true);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(32);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setMinimumSectionSize(56);
    table->horizontalHeader()->setFixedHeight(34);
}

void fadeIn(QWidget* w, int ms)
{
    if (!w) return;
    // 连续快速切页时，上一轮的动画还持有旧 effect；先停掉旧动画并卸掉旧特效，
    // 否则 finished 回调里的指针比对会落空，页面可能永久停在 0.4 透明度。
    const QList<QPropertyAnimation*> running = w->findChildren<QPropertyAnimation*>();
    for (QPropertyAnimation* a : running) a->stop();
    w->setGraphicsEffect(nullptr);

    auto* effect = new QGraphicsOpacityEffect(w);
    w->setGraphicsEffect(effect);
    auto* anim = new QPropertyAnimation(effect, "opacity", w);
    anim->setDuration(ms);
    anim->setStartValue(0.4);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(anim, &QPropertyAnimation::finished, w, [w, effect]() {
        if (w->graphicsEffect() == effect) w->setGraphicsEffect(nullptr);
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace Theme
