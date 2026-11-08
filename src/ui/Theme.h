#pragma once                                     // 防止头文件被重复包含

#include <QString>                               // styleSheet() 返回值
#include <QColor>                                // 语义色

class QTableWidget;                              // 表格统一排版
class QWidget;                                   // 淡入过渡

// Theme：全局视觉规范（浅色主题 · 4px 栅格 · 单一强调色）
//
// ── 颜色语义只有 3 套，各司其职，不互相借用 ──────────────────────────
//   A 套 · 行情方向（A股红涨绿跌）：up / down / flat
//        只用于「价格、涨跌幅、止盈止损价、K 线」—— 数值本身就是行情。
//   B 套 · 决策状态：success / warning / danger / neutral
//        只用于「结论、可执行与否、闸门、提醒、风控、状态徽章」。
//        状态一律用**带底色的 chip**呈现，形状差异保证不靠颜色单独承载语义。
//   C 套 · 强调色（唯一）：accent
//        只用于「可交互、焦点环、当前选中、当前页」。刻意避开红绿区 ——
//        否则「红色」会同时表示涨 / 危险 / 选中 / 一致看多，映射立刻失效。
//
// ── 排版 ──────────────────────────────────────────────────────────
//   字号 7 级（11 / 12 / 13 / 15 / 16 / 19 / 22），间距取 4 的整数倍，
//   圆角三档（6 / 8 / 10）。正文与背景对比度 ≥ 4.5:1，次要文字不低于 4.5:1。

namespace Theme {

// ---------------- A 套：行情方向（红涨绿跌） ----------------
QColor up();        // #d93025 涨（红）——对比度 ≈ 4.8:1
QColor down();      // #0b7d46 跌（绿）——≈ 4.7:1
QColor flat();      // #8a8f99 平（灰，仅用于非文本装饰）

// ---------------- B 套：决策状态 ----------------
QColor success();   // #0f7a53 通过 / 可执行
QColor warning();   // #a86400 警示 / 观察 / 需降仓
QColor danger();    // #c5221f 危险 / 禁止出手 / 淘汰
QColor neutral();   // #5b6470 中性 / 占位 / 无结论

// 状态徽章的浅底色（配 toneColor 使用）
QColor successSoft();  // #e6f4ee
QColor warningSoft();  // #fbeadb
QColor dangerSoft();   // #fdecea
QColor neutralSoft();  // #eef1f5 弱提示底

// ---------------- C 套：强调色（唯一，靛蓝） ----------------
QColor accent();       // #1f5fa8 可交互 / 焦点 / 当前选中
QColor accentSoft();   // #e8f0fa 选中底 / 浅强调底
QColor accentDeep();   // #17457a 悬停文字 / 按压色
QColor accentMuted();  // #9fb8d4 禁用 / 弱描边
QColor hoverSoft();    // #f0f5fb 悬停底

// ---------------- 背景 / 描边 ----------------
QColor bg();           // 窗口底色
QColor panel();        // 面板底色
QColor card();         // 卡片底色
QColor border();       // 常规描边
QColor borderStrong(); // 控件描边
QColor borderFaint();  // 分隔线 / 极淡描边
QColor grid();         // 图表网格
QColor hover();        // 悬停底（中性灰）
QColor pressed();      // 按压底

// ---------------- 文字 ----------------
QColor text();         // 主文字
QColor textMuted();    // 次要文字（仍保证 4.5:1 对比度）
QColor textFaint();    // 弱化说明 / 分组标题
QColor textDisabled(); // 禁用态
QColor textInvert();   // 反白

QColor byChange(double pct);   // 按涨跌幅取 A 套语义色

// ---------------- 间距栅格（4px 基准）----------------
namespace Space {
constexpr int Xs = 4;    // 图标与文字、紧凑内边距
constexpr int Sm = 8;    // 控件之间
constexpr int Md = 12;   // 分组之间
constexpr int Lg = 16;   // 卡片内边距
constexpr int Xl = 20;   // 区块之间
constexpr int Xxl = 24;  // 页面边距
}
inline int space(int steps) { return steps * Space::Xs; }   // steps × 4px

// ---------------- 圆角 / 字号 ----------------
int radiusSm();          // 6 —— 按钮、输入框
int radiusMd();          // 8 —— 卡片、表格
int radiusLg();          // 10 —— 对话框、大容器

int fontXs();            // 11px —— 辅助说明、角标、图例
int fontSm();            // 12px —— 次要文字
int fontMd();            // 13px —— 正文
int fontLg();            // 15px —— 卡片标题
int fontH2();            // 16px —— 卡片主标题 / 标的名称
int fontTitle();         // 19px —— 页面标题
int fontKpi();           // 22px —— 关键结论数字（全界面仅此一处大号）

// ---------------- 样式 ----------------
QString styleSheet();    // 全局 QSS（应用级，只装一次；内部全部走令牌，无硬编码色值）

// 内联样式辅助：替代各面板里散落的 setStyleSheet 硬编码色值
QString textStyle(const QColor& color, int fontPx, bool bold = false);
QString chipStyle(const QColor& fg, const QColor& bg, int fontPx = 11,
                  int radius = 4, int padH = 8, bool bold = true);

// 表格统一排版：不换行、隐藏网格、固定行高、像素级滚动、悬停高亮
void polishTable(QTableWidget* table);

// 轻量淡入过渡（页面切换 / 结果刷新）：动画结束自动移除特效，不影响后续绘制
void fadeIn(QWidget* w, int ms = 160);

} // namespace Theme
