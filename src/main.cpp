#include <QApplication>       // Qt 应用程序类，管理 GUI 控制流和设置
#include <QStyleFactory>      // 创建内置或自定义样式（如 Fusion）
#include <QLocale>            // 本地化设置，用于日期/数字格式
#include <QCommandLineParser> // 解析命令行参数
#include <QDir>               // 目录路径操作
#include <QTextStream>        // 文本流读写
#include <QThread>            // 线程支持（此处仅引入头文件）
#include <QFile>              // 文件 I/O 操作
#include <QFileInfo>          // 获取文件元信息（大小、存在性等）
#include <QTableWidget>       // 基于项的表格控件
#include <QHeaderView>        // 表头视图，控制排序指示器等
#include <QStackedWidget>     // 多页堆叠容器，一次只显示一页
#include <QTimer>             // 定时器，用于轮询或超时
#include <QElapsedTimer>      // 高精度计时器，测量耗时
#include <QEventLoop>         // 截图前等待淡入动画结束
// QTextStream 重复包含，编译器会忽略
#include <QMouseEvent>        // 鼠标事件类，用于模拟交互

#include "ui/MainWindow.h"
#include "ui/DisclaimerDialog.h" // 启动前置：免责声明 / 风险揭示门禁
#include "core/LegalNotice.h"    // 免责声明合规要点（冒烟与正文同源）
#include "data/DataHub.h"      // --detailprobe：详情数据链路自检
#include "tests/CoreChecks.h"  // 离线门禁断言集（--corechecks，与 console 门禁同源）
#include "ui/Theme.h"         // 全局主题样式表
#include "core/Diagnostics.h" // 文件日志 + 崩溃转储
#include "core/SingleInstance.h" // 单实例保护
#include "ui/ChartPanel.h"    // K 线图表面板
#include "ui/PlanCellDelegate.h" // 荐股表单元格代理，提供 planCellText
#include "ui/AiStockPanel.h"   // 荐股计划面板
#include "ui/ReviewPanel.h"     // 决策复盘页断言（--uismoke）
#include "core/TradePlan.h"      // 交易计划数据结构

#include <QLabel>                // 标签控件
#include <QFrame>                // 选股卡片断言
#include <QDialog>               // 卡片按钮弹窗断言
#include <QTextBrowser>          // 日志页 AI 诊断栏断言
#include <QPushButton>           // 顶栏按钮断言
#include <QCheckBox>             // 免责声明同意勾选断言
#include <QAction>               // 研究型菜单项断言（应已全部移除）
#include <QSet>                  // 冒烟测试的行标识去重
#include "core/Types.h"          // 核心类型定义（Candle, LimitEntry 等）
#include "qcustomplot.h"         // 第三方绘图库
#include <cmath>                 // 数学函数（sin, cos, fabs 等）

#ifdef Q_OS_WIN                  // Windows 平台专用头文件
#  include <cstdio>              // C 标准 I/O（freopen_s）
#  include <fcntl.h>             // 文件控制选项（_O_WRONLY 等）
#  include <io.h>                // 低级 I/O（_open_osfhandle, _dup2）
#  include <windows.h>           // Win32 API（控制台句柄操作）
#endif

namespace { // 匿名命名空间，限制内部函数链接范围

// 将 GUI 子进程的 stdout/stderr 绑定到父控制台或重定向句柄
void attachParentConsole()
{
#ifdef Q_OS_WIN
    const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); // 获取标准输出句柄
    const DWORD type = hOut ? GetFileType(hOut) : FILE_TYPE_UNKNOWN; // 判断句柄类型
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) { // 输出已被重定向到文件或管道
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hOut), _O_WRONLY | _O_TEXT); // 将 OS 句柄转为 CRT fd
        if (fd != -1 && _dup2(fd, 1) == 0) _dup2(1, 2); // 复制 fd 到 stdout(1)，再复制到 stderr(2)
        return; // 重定向绑定完成，直接返回
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return; // 尝试附加父进程控制台，失败则静默退出
    FILE* f = nullptr; // freopen_s 所需的临时指针
    freopen_s(&f, "CONOUT$", "w", stdout); // 将 stdout 重定向到控制台输出
    freopen_s(&f, "CONOUT$", "w", stderr); // 将 stderr 重定向到控制台输出
    freopen_s(&f, "CONIN$", "r", stdin);   // 将 stdin 重定向到控制台输入
#endif
}

} // namespace

namespace { // 个股详情数据链路自检
    
int runDetailProbe(QApplication& app, const QString& code)
{
    DataHub hub;
    hub.init();
    StockDetailData got;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);

    QObject::connect(&hub, &DataHub::stockDetailReady, &app,
                     [&got](const QString&, const StockDetailData& d) { got = d; });
    QObject::connect(&hub, &DataHub::stockDetailReady, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(30000);
    hub.refreshStockDetail(code);
    loop.exec();

    QTextStream ts(stdout);
    ts << "========== detail data probe: " << code << " ==========\n";
    ts << "finance      : " << (got.finance.valid ? 1 : 0)
       << "  " << got.finance.period << "  revenueYoy=" << got.finance.revenueYoy
       << "  profitYoy=" << got.finance.netProfitYoy << "  roe=" << got.finance.roe << "\n";
    ts << "moneyflow    : days=" << got.flows.size();
    if (!got.flows.isEmpty()) {
        const MoneyFlowDay& d = got.flows.last();
        ts << "  last=" << d.date.toString(QStringLiteral("yyyy-MM-dd"))
           << "  mainNet=" << d.mainNet << "  bigPct=" << d.bigPct
           << "  superBigPct=" << d.superBigPct;
    }
    ts << "\n";
    ts << "institutions : " << (got.org.valid ? 1 : 0)
       << "  " << got.org.reportDate << "  orgNum=" << got.org.orgNum
       << "  sharesRatio=" << got.org.sharesRatio << "\n";
    ts << "error        : " << got.error << "\n";
    const bool ok = got.finance.valid && !got.flows.isEmpty();
    ts << (ok ? "[PASS] detail data parsed\n" : "[FAIL] detail data unavailable\n");
    ts.flush();
    return ok ? 0 : 2;
}

} // namespace

namespace { // UI 冒烟测试相关辅助函数

QWidget* currentPage(QWidget* root)
{
    QStackedWidget* stack = root->findChild<QStackedWidget*>(QStringLiteral("MainStack"));
    return stack ? stack->currentWidget() : nullptr;
}

// 切页淡入动画 140ms；不等它结束就截图会把黑色按钮拍成灰色
void settleFade(QApplication& app, QWidget* page, int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    // 截图必须反映最终样式：清掉可能残留的淡入特效（0.4 透明会把黑按钮拍成灰色）
    if (page) page->setGraphicsEffect(nullptr);
    app.processEvents();
}

// 在根控件下查找荐股表，优先按 objectName 精确匹配
QTableWidget* firstTable(QWidget* root)
{
    if (QTableWidget* t = root->findChild<QTableWidget*>(QStringLiteral("RecommendTable"))) // 按名称查找
        return t; // 找到即返回
    const QList<QTableWidget*> all = root->findChildren<QTableWidget*>(); // 回退：查找所有表格
    return all.isEmpty() ? nullptr : all.first(); // 返回第一个或空指针
}

// 检查指定行列是否包含有效区间文本（含 '~' 分隔符）
bool hasRange(QTableWidget* t, int row, int col)
{
    QTableWidgetItem* it = t->item(row, col); // 获取单元格项
    if (!it) return false; // 项不存在则无区间
    const QString s = planCellText(it).trimmed(); // 用代理取显示文本而非排序键
    return !s.isEmpty() && s.contains(QLatin1Char('~')); // 非空且含波浪号才算有效区间
}

// 执行 UI 冒烟测试：验证启动后首屏荐股表有数据且字段完整
int runUiSmoke(QApplication& app)
{
    MainWindow w; // 创建主窗口实例
    w.resize(1560, 940); // 设置固定测试分辨率
    w.show(); // 显示窗口以触发渲染

    // 前置免责声明页：正文合规要点齐全 + 未勾选时「同意并继续」必须不可用。
    // 只切换勾选状态、不点击同意 —— 冒烟测试不写入本机同意记录。
    QString disclaimerLine;
    int rcDisclaimer = 0;
    {
        DisclaimerDialog dlg(DisclaimerDialog::Mode::Consent, nullptr);
        QTextBrowser* body = dlg.findChild<QTextBrowser*>(QStringLiteral("DisclaimerBody"));
        QCheckBox* agree = dlg.findChild<QCheckBox*>(QStringLiteral("DisclaimerAgree"));
        QPushButton* accept = nullptr;
        for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
            if (b && b->text() == QStringLiteral("同意并继续")) { accept = b; break; }
        }
        const QString text = body ? body->toPlainText() : QString();
        int missing = 0;
        for (const QString& topic : LegalNotice::requiredTopics()) {
            if (!text.contains(topic)) ++missing;
        }

        // 截取初始态（未勾选、按钮禁用）供人工核对；show() 不阻塞事件循环
        dlg.show();
        app.processEvents();
        const QString shotPath = QDir::current().filePath(QStringLiteral("uismoke_disclaimer.png"));
        dlg.grab().save(shotPath);
        const qint64 shotBytes = QFileInfo(shotPath).size();

        const bool gated = accept && !accept->isEnabled();
        bool enabledAfterCheck = false;
        if (agree) {
            agree->setChecked(true);
            enabledAfterCheck = accept && accept->isEnabled();
        }
        const bool ok = body && agree && accept && missing == 0
                     && gated && enabledAfterCheck;
        QTextStream line(&disclaimerLine);
        line << "disclaimer   : topics="
             << (LegalNotice::requiredTopics().size() - missing) << "/"
             << LegalNotice::requiredTopics().size()
             << " gatedBeforeCheck=" << (gated ? 1 : 0)
             << " enabledAfterCheck=" << (enabledAfterCheck ? 1 : 0)
             << (ok ? "  [OK 未同意前阻断访问]" : "  [FAIL 免责声明门禁异常]") << "\n";
        line << "disclaimer shot: " << shotPath << " (" << shotBytes << " bytes)\n";
        line.flush();
        dlg.hide();
        if (!ok) rcDisclaimer = 30;
    }

    QElapsedTimer timer; // 高精度计时器
    timer.start(); // 开始计时

    QTimer poll; // 轮询定时器，等待表格填充
    poll.setInterval(300); // 每 300ms 检查一次
    bool detailPrimed = false;
    QObject::connect(&poll, &QTimer::timeout, [&]() { // 超时回调
        if (detailPrimed) return;
        QTableWidget* t = firstTable(&w); // 查找荐股表
        if (!t || t->rowCount() == 0) return;
        detailPrimed = true;
        poll.stop();
        // ★ 详情里的财报 / 资金流 / 机构持仓是异步网络请求：必须在主事件循环
        //   仍然运行时触发，等数据回来再退出，否则后续断言只能看到「正在拉取」。
        if (QPushButton* btn = w.findChild<QPushButton*>(QStringLiteral("CardDetailButton")))
            btn->click();
        QTimer::singleShot(6000, &app, [&app]() { app.quit(); });
    });
    poll.start(); // 启动轮询

    QTimer deadline; // 超时截止定时器
    deadline.setSingleShot(true); // 单次触发
    deadline.setInterval(30000); // 30 秒超时
    QObject::connect(&deadline, &QTimer::timeout, [&]() { poll.stop(); app.quit(); }); // 超时强制退出
    deadline.start(); // 启动截止计时

    app.exec(); // 进入事件循环，直到 quit 被调用

    QString report; // 报告字符串缓冲
    QTextStream ts(&report); // 文本流写入报告
    ts << "========== UI smoke test: recommendations on launch ==========\n"; // 报告标题
    ts << "elapsed ms   : " << timer.elapsed() << "\n"; // 记录耗时
    {
        const QList<QTableWidget*> all = w.findChildren<QTableWidget*>(); // 列出所有表格用于诊断
        ts << "tables       : " << all.size() << "\n"; // 表格总数
        for (QTableWidget* t : all) { // 遍历每张表
            ts << "  - " << t->objectName() << " rows=" << t->rowCount() // 输出表名和行列数
               << " cols=" << t->columnCount() << "\n";
        }
    }

    QTableWidget* table = firstTable(&w); // 再次获取荐股表引用
    int rc = 0; // 返回码，0 表示成功
    ts << disclaimerLine; // 免责声明前置页断言结果
    if (rcDisclaimer != 0) rc = rcDisclaimer;
    if (!table) { // 表格未找到
        ts << "[FAIL] recommend table not found\n"; // 记录失败
        rc = 2; // 设置错误码
    } else {
        ts << "rows         : " << table->rowCount() << "\n"; // 记录行数
        if (table->rowCount() == 0) { // 表格为空
            ts << "[FAIL] empty on launch\n"; // 记录失败
            rc = 3; // 设置错误码
        } else {
            int missing = 0; // 缺失区间的计数（列位置由 UI 层提供，避免旧版 17 列残留）
            for (int r = 0; r < table->rowCount(); ++r) { // 逐行检查
                if (!hasRange(table, r, AiStockPanel::buyColumn())) ++missing;
                if (!hasRange(table, r, AiStockPanel::takeColumn())) ++missing;
                if (!hasRange(table, r, AiStockPanel::stopColumn())) ++missing;
            }
            ts << "missing rng  : " << missing << "\n"; // 记录缺失数
            {   // 定价依据抽样：首行三段区间的 tooltip（应含筹码 / 量能依据）
                auto tip = [&](int col) -> QString {
                    const QTableWidgetItem* it = table->item(0, col);
                    if (!it) return QString();
                    QString t = it->toolTip().replace(QLatin1Char('\n'), QStringLiteral(" / "));
                    return t.length() > 220 ? t.left(220) + QStringLiteral("…") : t;
                };
                ts << "pricing buy : " << tip(AiStockPanel::buyColumn()) << "\n";
                ts << "pricing take: " << tip(AiStockPanel::takeColumn()) << "\n";
                ts << "pricing stop: " << tip(AiStockPanel::stopColumn()) << "\n";
            }
            if (missing > 0) { // 存在缺失
                ts << "[FAIL] missing buy/tp/sl ranges\n"; // 记录失败
                rc = 4; // 设置错误码
            } else {
                for (int r = 0; r < table->rowCount(); ++r) { // 逐行输出详情
                    ts << QStringLiteral("  %1 %2  buy %3  tp %4  sl %5") // 格式化行数据
                           .arg(planCellText(table->item(r, 0)), planCellText(table->item(r, 1)),
                                planCellText(table->item(r, AiStockPanel::buyColumn())),
                                planCellText(table->item(r, AiStockPanel::takeColumn())),
                                planCellText(table->item(r, AiStockPanel::stopColumn())))
                       << "\n";
                }
                ts << "[PASS] recommendations visible on launch\n"; // 基础检查通过
            }
        }

        { // 卡片列表契约：一只标的一张白卡，底部两个按钮齐全
            const QList<QFrame*> cards = w.findChildren<QFrame*>(QStringLiteral("StockCard"));
            int withButtons = 0;
            for (QFrame* c : cards) {
                if (c->findChild<QPushButton*>(QStringLiteral("CardDetailButton"))
                    && c->findChild<QPushButton*>(QStringLiteral("CardKlineButton")))
                    ++withButtons;
            }
            const bool ok = !cards.isEmpty()
                         && cards.size() == table->rowCount()
                         && withButtons == cards.size();
            ts << "stock cards  : " << cards.size()
               << " withButtons=" << withButtons
               << (ok ? "  [OK 卡片与按钮一一对应]" : "  [FAIL 卡片列表缺失]") << "\n";
            if (!ok && rc == 0) rc = 24;
        }

        { // 精简契约：K线图表 / 决策依据 / 今日提醒 / 风控设置 / 为什么没选 已全部移除
            QWidget* dims = w.findChild<QWidget*>(QStringLiteral("DimensionTable"));
            QWidget* reminder = w.findChild<QWidget*>(QStringLiteral("ReminderRow"));
            QWidget* risk = w.findChild<QWidget*>(QStringLiteral("RiskNotice"));
            QWidget* klineBtn = w.findChild<QWidget*>(QStringLiteral("RowKlineButton"));
            int basisButtons = 0;
            for (QPushButton* b : w.findChildren<QPushButton*>()) {
                if (!b) continue;
                if (b->text() == QStringLiteral("查看得分由来")) ++basisButtons;
            }
            QLabel* cap = w.findChild<QLabel*>(QStringLiteral("StatusCap"));
            const bool removed = !dims && !reminder && !risk && !klineBtn && basisButtons == 0;
            ts << "detail removed: dims=" << (dims ? 1 : 0)
               << " reminder=" << (reminder ? 1 : 0)
               << " risk=" << (risk ? 1 : 0)
               << " klineBtn=" << (klineBtn ? 1 : 0)
               << " basisBtn=" << basisButtons
               << " cap=" << (cap ? cap->text() : QStringLiteral("-")) << "\n";
            if (!removed) {
                ts << "[FAIL] removed modules still present\n";
                rc = 10;
            } else {
                ts << "[PASS] removed modules are gone\n";
            }
        }

        { // 精简契约：界面只有「简洁」一种视图——视图档位开关与研究型入口
          // （因子权重 / API Key / 消息推送菜单项）应整体移除而非置灰。
            QPushButton* btnSimple = nullptr;
            QPushButton* btnDetail = nullptr;
            for (QPushButton* b : w.findChildren<QPushButton*>()) {
                if (!b || b->objectName() != QStringLiteral("ViewModeSegment")) continue;
                if (b->text() == QStringLiteral("简洁")) btnSimple = b;
                else if (b->text() == QStringLiteral("详细")) btnDetail = b;
            }
            auto actionByText = [&w](const QString& text) -> QAction* {
                for (QAction* a : w.findChildren<QAction*>()) {
                    if (a && a->text() == text) return a;
                }
                return nullptr;
            };
            QAction* weights = actionByText(QStringLiteral("因子权重…"));
            QAction* aiKey   = actionByText(QStringLiteral("API Key…"));
            QAction* notify  = actionByText(QStringLiteral("消息推送…"));
            const bool gone = !btnSimple && !btnDetail && !weights && !aiKey && !notify;
            ts << "view simple  : "
               << (gone ? "[OK 仅保留简洁视图，研究型入口已移除]" : "[FAIL 详细档入口仍存在]") << "\n";
            if (!gone) rc = 13;
        }

        { // 全局结论条 / 持仓下一步动作清单就位（保存执行单按钮已移除）
            QPushButton* global = w.findChild<QPushButton*>(QStringLiteral("GlobalVerdict"));
            QPushButton* sheet  = w.findChild<QPushButton*>(QStringLiteral("SaveSheetButton"));
            QTextBrowser* acts  = w.findChild<QTextBrowser*>(QStringLiteral("ActionList"));
            const bool ok = global && !global->text().isEmpty() && !sheet && acts;
            ts << "v1.1 widgets : global=" << (global ? 1 : 0)
               << " sheetGone=" << (sheet ? 0 : 1)
               << " actions=" << (acts ? 1 : 0)
               << (ok ? "  [OK]" : "  [FAIL]") << "\n";
            if (!ok) rc = 16;
        }

        { 
            QWidget* navBar = w.findChild<QWidget*>(QStringLiteral("NavBar"));
            QWidget* sideBar = w.findChild<QWidget*>(QStringLiteral("SideBar"));
            QWidget* host = w.findChild<QWidget*>(QStringLiteral("PageActionHost"));
            QPushButton* colBtn = nullptr;
            for (QPushButton* b : w.findChildren<QPushButton*>()) {
                if (b && b->objectName() == QStringLiteral("Ghost") && b->text() == QStringLiteral("列")) {
                    colBtn = b; break;
                }
            }
            const int navCount = (navBar ? navBar->findChildren<QPushButton*>(
                                    QStringLiteral("Nav")).size() : 0);
            const int hostActions = host ? host->findChildren<QPushButton*>().size() : 0;
            const int colItems = (colBtn && colBtn->menu()) ? colBtn->menu()->actions().size() : 0;

            const bool ok = navBar && !sideBar && host
                         && navCount == 3 && hostActions > 0
                         && colBtn && colItems == AiStockPanel::columnCount();
            ts << "nav bar      : navBar=" << (navBar ? 1 : 0)
               << " sideBar=" << (sideBar ? 1 : 0)
               << " navBtns=" << navCount
               << " pageActions=" << hostActions
               << " colMenu=" << colItems
               << (ok ? "  [OK]" : "  [FAIL]") << "\n";
            if (!ok) rc = 22;

            // 切到每个页面都应挂上该页的主动作（不是只有首页有）
            bool allPages = true;
            for (int i = 0; i < 3; ++i) {
                for (QPushButton* b : w.findChildren<QPushButton*>()) {
                    if (b && b->objectName() == QStringLiteral("Nav")) {
                        // 通过点击导航按钮切页
                        break;
                    }
                }
                QPushButton* nav = nullptr;
                int idx = 0;
                for (QPushButton* b : w.findChildren<QPushButton*>()) {
                    if (b && b->objectName() == QStringLiteral("Nav")) {
                        if (idx == i) { nav = b; break; }
                        ++idx;
                    }
                }
                if (!nav) { allPages = false; break; }
                nav->click();
                app.processEvents();
                if (!host || host->findChildren<QPushButton*>().isEmpty()) { allPages = false; break; }
            }
            ts << "page actions : " << (allPages ? "[OK 3 页均有主动作]" : "[FAIL 某页没有主动作]") << "\n";
            if (!allPages) rc = 23;
            // 切回首页
            for (QPushButton* b : w.findChildren<QPushButton*>()) {
                if (b && b->objectName() == QStringLiteral("Nav") && b->text() == QStringLiteral("AI选股结果")) {
                    b->click(); break;
                }
            }
            app.processEvents();
        }

        {
            QStackedWidget* stack = w.findChild<QStackedWidget*>(QStringLiteral("MainStack"));
            QWidget* bar = w.findChild<QWidget*>(QStringLiteral("GlobalVerdictBar"));
            const bool homeIsPlan = stack && stack->currentIndex() == 0;   // 首页 = AI选股结果
            const bool hiddenOnHome = bar && bar->isHidden();
            bool shownElsewhere = false;
            if (stack && bar) {
                for (QPushButton* b : w.findChildren<QPushButton*>()) {
                    if (!b || b->objectName() != QStringLiteral("Nav")) continue;
                    if (b->text() != QStringLiteral("交易日志")) continue;
                    b->click();
                    app.processEvents();
                    shownElsewhere = !bar->isHidden();
                    // 切回首页
                    for (QPushButton* n : w.findChildren<QPushButton*>()) {
                        if (n && n->objectName() == QStringLiteral("Nav")
                            && n->text() == QStringLiteral("AI选股结果")) {
                            n->click(); break;
                        }
                    }
                    app.processEvents();
                    break;
                }
            }
            const bool ok = homeIsPlan && hiddenOnHome && shownElsewhere;
            ts << "home = plan  : " << (homeIsPlan ? "[OK 首页是 AI选股结果]" : "[FAIL 首页不是 AI选股结果]")
               << (hiddenOnHome ? " [OK 结论条在首页收起]" : " [FAIL 结论条在首页未收起]")
               << (shownElsewhere ? " [OK 其它页结论条常驻]" : " [FAIL 其它页结论条缺失]")
               << "\n";
            if (!ok) rc = 17;
        }

        { 
            int missingCols = 0;
            const int cols[] = { AiStockPanel::riskRewardColumn(),
                                 AiStockPanel::riskPctColumn(),
                                 AiStockPanel::fillProbColumn() };
            for (int c : cols) {
                if (c < 0 || c >= table->columnCount()) { ++missingCols; continue; }
                const QString head = table->horizontalHeaderItem(c)
                                     ? table->horizontalHeaderItem(c)->text() : QString();
                if (head.isEmpty()) ++missingCols;
            }
            // 列内容也必须有值（首行）
            if (table->rowCount() > 0) {
                for (int c : cols) {
                    const QString txt = c >= 0 && c < table->columnCount()
                        ? planCellText(table->item(0, c)) : QString();
                    if (txt.trimmed().isEmpty()) ++missingCols;
                }
            }
            ts << "decision cols: rr=" << AiStockPanel::riskRewardColumn()
               << " riskPct=" << AiStockPanel::riskPctColumn()
               << " fillProb=" << AiStockPanel::fillProbColumn()
               << (missingCols == 0 ? "  [OK 三个决策列就位]" : "  [FAIL 决策列缺失]")
               << "\n";
            if (missingCols > 0) rc = 18;
        }

        { // P0 回归：验证首页交互契约
            QHeaderView* hh = table->horizontalHeader(); // 获取水平表头
            const int sortCol = hh->sortIndicatorSection(); // 当前排序列索引
            const bool desc = (hh->sortIndicatorOrder() == Qt::DescendingOrder); // 是否降序
            // 与 AiStockPanel::Column::ColPosition 保持一致（默认按仓位降序）
            const int wantCol = AiStockPanel::positionColumn();
            ts << "sort default : col=" << sortCol << (desc ? " desc" : " asc")
               << (sortCol == wantCol && desc ? "  [OK 仓位降序]" : "  [WARN 非仓位降序]") << "\n";

            const int row = table->rowCount() - 1; // 选中最后一行进行测试
            const QString wantCode = planCellText(table->item(row, 0)); // 获取该行股票代码
            table->setCurrentCell(row, 0); // 设置当前单元格
            table->selectRow(row); // 高亮整行
            app.processEvents(); // 处理事件使选中生效

            QStackedWidget* stack = w.findChild<QStackedWidget*>(QStringLiteral("MainStack")); // 查找页面堆栈
            const bool stayed = (!stack || stack->currentIndex() == 0); // 验证仍停留在首页
            ts << "select stays : " << (stayed ? "[OK 仍在 AI选股结果 页]" : "[FAIL 被跳到其它页]") << "\n";

            QTableWidgetItem* codeItem = table->item(row, 0);
            const QString roleCode = codeItem ? codeItem->data(Qt::UserRole).toString() : QString();
            const bool samePlan = !roleCode.isEmpty() && roleCode == wantCode;
            ts << "row → code   : want " << wantCode << " role " << roleCode
               << (samePlan ? " [OK 行标识一致]" : " [FAIL 行标识错位]") << "\n";

            if (!stayed || !samePlan) rc = 7; // 交互契约失败则设置错误码
        }

        { // P0 回归：合成数据验证排序后行与计划的一一对应
            AiStockPanel probe; // 独立面板实例，避免污染主窗口
            RecommendBundle b; // 构造合成数据包
            b.tradeDate = QDate::currentDate(); // 设置交易日期
            b.updatedAt = QStringLiteral("synthetic"); // 标记为合成数据
            b.notice = QStringLiteral("synthetic"); // 通知文本
            struct Seed { const char* code; const char* name; double score; }; // 种子数据结构
            const Seed seeds[] = { // 评分与代码顺序刻意相反的数据集
                { "600000", "甲公司", 90.0 },
                { "300750", "乙公司", 80.0 },
                { "002172", "丙公司", 70.0 },
                { "000001", "丁公司", 60.0 },
            };
            for (const Seed& s : seeds) { // 构建 TradePlan 列表
                TradePlan p; // 创建计划对象
                p.code = QString::fromLatin1(s.code); // 设置代码
                p.name = QString::fromUtf8(s.name); // 设置名称
                p.currentPrice = 10.20;             // 卡片上的「当前价格」
                p.score = s.score; // 设置评分
                p.grade = QStringLiteral("B"); // 评级
                p.tag = QStringLiteral("二板"); // 标签
                p.buyLow = 10.0;  p.buyHigh = 10.5; // 买入区间
                p.takeProfitLow = 11.0; p.takeProfitHigh = 12.0; // 止盈区间
                p.stopLossLow = 9.0; p.stopLossHigh = 9.5; // 止损区间
                p.positionPct = 5.0; p.riskReward = 1.2; // 仓位与盈亏比
                p.note = QStringLiteral("synthetic"); // 备注
                b.plans.append(p); // 加入包中
            }
            probe.setBundle(b); // 加载合成数据
            app.processEvents(); // 刷新 UI

            { // 执行卡已整体移除：卡片、窄屏摘要与本金换算控件都不应再出现
                QWidget* execCard = probe.findChild<QWidget*>(QStringLiteral("ExecCard"));
                QWidget* execSummary = probe.findChild<QWidget*>(QStringLiteral("ExecSummary"));
                QLabel* capRes = probe.findChild<QLabel*>(QStringLiteral("CapitalResult"));
                const bool gone = !execCard && !execSummary && !capRes;
                ts << "exec card    : " << (gone ? "[OK 已移除]" : "[FAIL 仍有残留]") << "\n";
                if (!gone) rc = 15;
            }

            QTableWidget* t = probe.findChild<QTableWidget*>(QStringLiteral("RecommendTable")); // 获取表格
            bool allMatch = (t && t->rowCount() == 4); // 前置条件检查
            QSet<QString> seen;                         // 4 行不得串票或重复
            if (allMatch) {
                t->sortByColumn(0, Qt::AscendingOrder); // 按代码升序排列（与评分序相反）
                app.processEvents(); // 刷新排序结果
                for (int r = 0; r < t->rowCount(); ++r) { // 逐行验证映射
                    QTableWidgetItem* it = t->item(r, 0);
                    const QString shown = planCellText(it);                 // 显示代码
                    const QString role = it ? it->data(Qt::UserRole).toString() : QString();  // 行标识
                    t->setCurrentCell(r, 0);
                    t->selectRow(r);
                    app.processEvents();
                    // 显示文本 / 行标识 / 定价依据三者必须同属一只票
                    const QTableWidgetItem* buy = t->item(r, AiStockPanel::buyColumn());
                    if (shown.isEmpty() || role.isEmpty() || shown != role
                        || !buy || buy->toolTip().isEmpty()) {
                        allMatch = false; break;
                    }
                    seen.insert(role);
                }
            }
            allMatch = allMatch && seen.size() == 4;
            ts << "sort→code map: "
               << (allMatch ? "[OK 4/4 行标识与依据对应正确]" : "[FAIL 行与计划错位]") << "\n";
            if (!allMatch) rc = 8; // 映射验证失败
        }

        { // P1 回归：列数以 AiStockPanel::Column::ColCount 为准，不再硬编码
            const int want = AiStockPanel::columnCount();
            ts << "columns      : " << table->columnCount()
               << (table->columnCount() == want
                       ? QStringLiteral("  [OK %1 列]").arg(want)
                       : QStringLiteral("  [FAIL 期望 %1 列]").arg(want)) << "\n";
            if (table->columnCount() != want) rc = 9;
        }

        { 
            const QFontMetrics fm(table->font());
            ts << QStringLiteral("--- 列宽校准（像素）viewport=%1 ---\n")
                   .arg(table->viewport()->width());
            int truncated = 0;
            int fixedSum = 0;
            for (int c = 0; c < table->columnCount(); ++c) {
                if (c != AiStockPanel::reasonColumn()) fixedSum += table->columnWidth(c);
                int need = fm.horizontalAdvance(table->horizontalHeaderItem(c)
                                               ? table->horizontalHeaderItem(c)->text() : QString());
                for (int r = 0; r < table->rowCount(); ++r) {
                    need = qMax(need, fm.horizontalAdvance(planCellText(table->item(r, c))));
                }
                need = int(need * 1.15) + 2;                 // 绘制余量
                const int usable = table->columnWidth(c) - 10;   // 单元格左右内边距合计（QSS: 6px 5px）
                const bool ok = usable >= need;
                if (!ok && c != AiStockPanel::reasonColumn()) ++truncated;
                ts << QStringLiteral("  col%1 %2  need=%3 usable=%4 %5\n")
                       .arg(c)
                       .arg(table->horizontalHeaderItem(c)
                                ? table->horizontalHeaderItem(c)->text() : QStringLiteral("-"), -8)
                       .arg(need, 4).arg(usable, 4)
                       .arg(ok ? QStringLiteral("[OK]") : QStringLiteral("[TRUNCATED]"));
            }
            ts << QStringLiteral("  fixed sum=%1  reason col=%2  truncated critical=%3 %4\n")
                   .arg(fixedSum).arg(table->columnWidth(AiStockPanel::reasonColumn()))
                   .arg(truncated)
                   .arg(truncated == 0 ? QStringLiteral("[OK]") : QStringLiteral("[FAIL]"));
            ts << "-----------------\n";
            if (truncated > 0) rc = 19;
        }

        { // 最小窗口宽度仍须适配 1366×768 笔记本
            const int minWin = w.minimumSizeHint().width();
            ts << "min window   : " << minWin
               << (minWin <= 1366 ? "  [OK 1366 屏放得下]" : "  [FAIL 1366 屏放不下]")
               << "\n";
            if (minWin > 1366) rc = 21;
        }

        { // 卡片列表响应式：宽屏多列 → 窄窗口少列 → 移动端宽度单列
            AiStockPanel* rp = w.findChild<AiStockPanel*>();
            const int wideCols = rp ? rp->cardColumnCount() : 0;
            int narrowCols = -1, mobileCols = -1;
            if (rp) {
                const int keepW = rp->width(), keepH = rp->height();
                auto setPanelWidth = [&](int target) {
                    rp->resize(target, keepH);
                    QResizeEvent re(QSize(target, keepH), QSize(keepW, keepH));
                    QApplication::sendEvent(rp, &re);
                    app.processEvents();
                    app.processEvents();
                };
                setPanelWidth(760); narrowCols = rp->cardColumnCount();
                setPanelWidth(420); mobileCols = rp->cardColumnCount();
                setPanelWidth(keepW);
            }
            const bool ok = wideCols > 0 && narrowCols > 0 && mobileCols == 1
                         && wideCols >= narrowCols && narrowCols >= mobileCols;
            ts << "card columns : wide=" << wideCols << " narrow=" << narrowCols
               << " mobile=" << mobileCols
               << (ok ? "  [OK 响应式列数递减]" : "  [FAIL 列自适应异常]") << "\n";
            if (!ok && rc == 0) rc = 25;
        }

        { // 卡片按钮接线 + 详情弹窗的信息分区
            QPushButton* detail = w.findChild<QPushButton*>(QStringLiteral("CardDetailButton"));
            QPushButton* kline  = w.findChild<QPushButton*>(QStringLiteral("CardKlineButton"));
            // 详情弹窗已在 poll 阶段由 CardDetailButton 打开并拉完数据；
            // 这里不再重复点击（卡片可能已重建，重复点击会切到另一只标的）。
            QDialog* detailDlg = w.findChild<QDialog*>(QStringLiteral("PlanDetailDialog"));
            if (detailDlg) settleFade(app, nullptr, 300);
            QDialog* klineDlg = nullptr;
            if (kline) {
                kline->click();
                app.processEvents();
                klineDlg = w.findChild<QDialog*>(QStringLiteral("KlineDialog"));
            }
            // 切页时 QStackedWidget 会隐藏所在页的子窗口，弹窗可能处于隐藏态；
            // 这里只校验「按钮确实创建并填充了弹窗」，不把可见性当作接线判据。
            const bool detailOk = detail && detailDlg && detailDlg->findChild<QLabel*>();
            const bool klineOk = klineDlg && klineDlg->findChild<ChartPanel*>();
            ts << "card actions : detail=" << (detailOk ? 1 : 0)
               << " kline=" << (klineOk ? 1 : 0)
               << ((detailOk && klineOk) ? "  [OK 两个动作都已接线]"
                                         : "  [FAIL 按钮未打开对应视图]") << "\n";
            if ((!detailOk || !klineOk) && rc == 0) rc = 26;

            QLabel* body = detailDlg ? detailDlg->findChild<QLabel*>() : nullptr;
            const QString text = body ? body->text() : QString();
            const QStringList sections = {
                QStringLiteral("所属行业"), QStringLiteral("近期财报核心数据"),
                QStringLiteral("K 线走势"), QStringLiteral("主力资金"),
                QStringLiteral("当前价格与买入区间"), QStringLiteral("交易计划")
            };
            int missing = 0;
            for (const QString& s : sections) if (!text.contains(s)) ++missing;
            ts << "detail layout: sections=" << (sections.size() - missing) << "/"
               << sections.size() << (missing == 0 ? "  [OK 信息分区齐全]"
                                                  : "  [FAIL 分区缺失]") << "\n";
            if (missing > 0 && rc == 0) rc = 27;
            // 外网数据只作信息输出，不因网络波动判定失败
            ts << "detail data   : finance=" << (text.contains(QStringLiteral("营业收入 / 同比增速")) ? 1 : 0)
               << " flow=" << (text.contains(QStringLiteral("主力净流入")) ? 1 : 0)
               << " org=" << (text.contains(QStringLiteral("机构家数变化")) ? 1 : 0)
               << "  [INFO 依赖东方财富接口]\n";
            if (detailDlg) {
                const QString png3 = QDir::current().filePath(QStringLiteral("uismoke_detail.png"));
                detailDlg->grab().save(png3);
                ts << "detail shot  : " << png3 << " (" << QFileInfo(png3).size() << " bytes)\n";
            }
            if (detailDlg) detailDlg->hide();
            if (klineDlg) klineDlg->hide();
        }

        { // 截取首屏截图供人工核对（放在布局断言之后：截图要反映稳定后的真实界面）
            settleFade(app, currentPage(&w), 260);
            const QString png = QDir::current().filePath(QStringLiteral("uismoke_recommend.png")); // 截图路径
            if (QFileInfo(png).exists()) QFile::remove(png); // 删除旧截图
            w.grab().save(png); // 抓取窗口内容并保存
            ts << "screenshot   : " << png << " (" << QFileInfo(png).size() << " bytes)\n"; // 记录截图信息
        }

        { // AI选股结果页（首页）：补一张截图供人工核对布局
            QPushButton* navPlan = nullptr;
            for (QPushButton* b : w.findChildren<QPushButton*>()) {
                if (b && b->objectName() == QStringLiteral("Nav")
                    && b->text() == QStringLiteral("AI选股结果")) {
                    navPlan = b; break;
                }
            }
            if (navPlan) {
                navPlan->click();                    // 回到「AI选股结果」页（首页）
                app.processEvents();
                settleFade(app, currentPage(&w), 260);
                const QString png2 = QDir::current().filePath(QStringLiteral("uismoke_plan.png"));
                w.grab().save(png2);
                ts << "plan shot    : " << png2 << "\n";
            }
        }

        { // 交易日志页：布局契约 + 截图（统计卡 / 动作卡 / 工具栏 / 表格 / 详情栏）
            QPushButton* navJournal = nullptr;
            for (QPushButton* b : w.findChildren<QPushButton*>()) {
                if (b && b->objectName() == QStringLiteral("Nav")
                    && b->text() == QStringLiteral("交易日志")) {
                    navJournal = b; break;
                }
            }
            if (navJournal) {
                navJournal->click();
                app.processEvents();
                settleFade(app, currentPage(&w), 260);

                QTextBrowser* actions = w.findChild<QTextBrowser*>(QStringLiteral("ActionList"));
                QTextBrowser* detail = w.findChild<QTextBrowser*>(QStringLiteral("JournalDetail"));
                QWidget* split = w.findChild<QWidget*>(QStringLiteral("JournalBodySplit"));
                QTableWidget* jt = w.findChild<QTableWidget*>(QStringLiteral("JournalTable"));
                const bool ok = actions && detail && split && jt && jt->columnCount() == 13;
                ts << "journal ui   : actionList=" << (actions ? 1 : 0)
                   << " detail=" << (detail ? 1 : 0)
                   << " splitter=" << (split ? 1 : 0)
                   << " cols=" << (jt ? jt->columnCount() : -1)
                   << (ok ? "  [OK 日志页布局就位]" : "  [FAIL 日志页布局缺失]") << "\n";
                if (!ok && rc == 0) rc = 28;

                const QString png4 = QDir::current().filePath(QStringLiteral("uismoke_journal.png"));
                w.grab().save(png4);
                ts << "journal shot : " << png4 << " (" << QFileInfo(png4).size() << " bytes)\n";

                for (QPushButton* b : w.findChildren<QPushButton*>()) {
                    if (b && b->objectName() == QStringLiteral("Nav")
                        && b->text() == QStringLiteral("AI选股结果")) {
                        b->click(); break;
                    }
                }
                app.processEvents();
            }
        }

        { // 复盘助手页：五个决策复盘视图 + 两个辅助页签，默认停在「信号溯源」
            QPushButton* navReview = nullptr;
            for (QPushButton* b : w.findChildren<QPushButton*>()) {
                if (b && b->objectName() == QStringLiteral("Nav")
                    && b->text() == QStringLiteral("复盘助手")) {
                    navReview = b; break;
                }
            }
            if (navReview) {
                navReview->click();
                app.processEvents();
                settleFade(app, currentPage(&w), 260);

                ReviewPanel* rp = w.findChild<ReviewPanel*>();
                const int tabs = rp ? rp->tabCount() : -1;
                const QString first = rp ? rp->currentTabText() : QString();
                const int criteria = rp ? rp->report().criteria.size() : -1;
                const bool boundary = rp && !rp->report().purpose.isEmpty()
                                   && rp->report().purpose.contains(QStringLiteral("不预测涨跌"));
                const bool ok = rp && tabs == 7
                             && first.startsWith(QStringLiteral("信号溯源"))
                             && criteria >= 10 && boundary;
                ts << "review ui    : tabs=" << tabs << " first=" << first
                   << " criteria=" << criteria
                   << (ok ? "  [OK 决策复盘视图与使用边界就位]"
                          : "  [FAIL 决策复盘视图缺失]") << "\n";
                if (!ok && rc == 0) rc = 29;

                for (QPushButton* b : w.findChildren<QPushButton*>()) {
                    if (b && b->objectName() == QStringLiteral("Nav")
                        && b->text() == QStringLiteral("AI选股结果")) {
                        b->click(); break;
                    }
                }
                app.processEvents();
            }
        }
    }
    ts.flush(); // 刷新文本流

    const QString path = QDir::current().filePath(QStringLiteral("ui_smoke_report.txt")); // 报告文件路径
    QFile f(path); // 打开报告文件
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { // 以文本写模式打开
        f.write(report.toUtf8()); // 写入 UTF-8 编码的报告
        f.close(); // 关闭文件
    }
    return rc; // 返回测试结果码
}

// 执行图表冒烟测试：渲染多周期 K 线及指标副图并截图验证
int runChartSmoke(QApplication& app)
{
    ChartPanel panel; // 创建图表面板
    panel.resize(1280, 820); // 设置测试尺寸
    panel.show(); // 显示面板
    app.processEvents(); // 确保初始渲染完成

    const QString dir = QCoreApplication::applicationDirPath(); // 输出目录为程序所在路径
    QString report; // 报告缓冲
    QTextStream ts(&report); // 文本流
    ts << "========== chart smoke ==========\n"; // 标题
    ts << "out dir : " << dir << "\n"; // 输出目录

    KlineSeries series; // K 线数据序列
    series.code = QStringLiteral("001219"); // 股票代码
    {
        double px = 12.0; // 初始价格
        QDate d = QDate::currentDate().addDays(-460); // 起始日期
        for (int i = 0; i < 300; ++i) { // 生成 300 根日 K
            while (d.dayOfWeek() > Qt::Friday) d = d.addDays(1); // 跳过周末
            const double o = px; // 开盘价等于前收盘
            const double c = qMax(3.0, px + std::sin(i * 0.35) * 0.32 + 0.035); // 收盘价含正弦波动
            const double h = qMax(o, c) + std::fabs(std::sin(i * 0.9)) * 0.26; // 最高价
            const double l = qMin(o, c) - std::fabs(std::cos(i * 0.7)) * 0.24; // 最低价
            Candle cd; // K 线结构体
            cd.date = d; // 日期
            cd.open = o; // 开盘
            cd.high = h; // 最高
            cd.low = l; // 最低
            cd.close = c; // 收盘
            cd.volume = 90000.0 + std::fabs(std::sin(i * 0.5)) * 160000.0; // 成交量
            cd.amount = cd.volume * 100.0 * c; // 成交额
            series.candles << cd; // 追加到序列
            px = c; // 更新下一根开盘基准
            d = d.addDays(1); // 日期递增
        }
    }
    QVector<LimitEntry> marks; // 涨停标记列表
    const int markIdx[] = { 250, 268, 290 }; // 标记对应的 K 线索引
    for (int k = 0; k < 3; ++k) { // 添加三个涨停标记
        const int i = markIdx[k]; // 当前索引
        LimitEntry e; // 标记条目
        e.code = series.code; // 代码
        e.name = QStringLiteral("青岛食品"); // 名称
        e.tradeDate = series.candles[i].date; // 日期
        e.boardCount = k + 2; // 连板数
        e.limitPrice = series.candles[i].high; // 涨停价
        marks << e; // 追加
    }

    auto shoot = [&](const QString& file, const QString& label) -> qint64 { // 截图 lambda
        app.processEvents(); // 确保渲染完成
        const QString path = dir + QLatin1Char('/') + file; // 完整路径
        panel.grab().save(path); // 截图保存
        const qint64 sz = QFileInfo(path).size(); // 文件大小
        ts << label << " : " << sz << " bytes  " << file << "\n"; // 记录到报告
        return sz; // 返回大小用于后续校验
    };

    panel.showKline(series.code, QStringLiteral("青岛食品"), series, marks); // 加载日 K 数据
    const qint64 s1 = shoot(QStringLiteral("chartsmoke_kline_day.png"), QStringLiteral("day  ")); // 日 K 截图

    panel.setPeriod(ChartPanel::Period::Week); // 切换到周线
    const qint64 s2 = shoot(QStringLiteral("chartsmoke_kline_week.png"), QStringLiteral("week ")); // 周 K 截图

    panel.setPeriod(ChartPanel::Period::Month); // 切换到月线
    const qint64 s3 = shoot(QStringLiteral("chartsmoke_kline_month.png"), QStringLiteral("month")); // 月 K 截图

    panel.setPeriod(ChartPanel::Period::Day); // 切回日线
    panel.setIndicator(1); // 切换到 KDJ 副图
    const qint64 s4 = shoot(QStringLiteral("chartsmoke_kline_kdj.png"), QStringLiteral("kdj  ")); // KDJ 截图
    panel.setIndicator(2); // 切换到 RSI 副图
    const qint64 s5 = shoot(QStringLiteral("chartsmoke_kline_rsi.png"), QStringLiteral("rsi  ")); // RSI 截图
    panel.setIndicator(0); // 切回 MACD（默认）

    const qint64 s6 = [&]() -> qint64 { // 十字光标测试 lambda
        QCustomPlot* plot = panel.findChild<QCustomPlot*>(); // 获取绘图控件
        if (!plot) return 0; // 找不到则跳过
        const QPointF at(plot->width() * 0.62, plot->height() * 0.30); // 模拟鼠标位置
        QMouseEvent me(QEvent::MouseMove, at, plot->mapToGlobal(at.toPoint()), // 构造鼠标移动事件
                       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(plot, &me); // 发送事件触发光标
        plot->replot(QCustomPlot::rpImmediateRefresh); // 立即重绘
        return shoot(QStringLiteral("chartsmoke_kline_probe.png"), QStringLiteral("probe")); // 截图
    }();

    const bool ok = s1 > 15000 && s2 > 12000 && s3 > 10000 && s4 > 15000 && s5 > 15000; // 文件大小阈值校验
    ts << (ok ? "[PASS] kline rendered (day/week/month + MACD/KDJ/RSI)\n" // 通过
              : "[FAIL] chart looks empty\n"); // 失败
    ts << "probe diff : " << (s6 != s1 ? "yes" : "no") << "\n"; // 光标截图是否与无光标不同
    ts.flush(); // 刷新报告

    QFile f(QDir::current().filePath(QStringLiteral("chart_smoke_report.txt"))); // 报告文件
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { // 写入报告
        f.write(report.toUtf8());
        f.close();
    }
    return ok ? 0 : 5; // 返回结果码
}

} // namespace

int main(int argc, char* argv[])
{
    attachParentConsole(); // 接管父控制台以便终端输出
    // Qt 6 默认启用高 DPI pixmap，AA_UseHighDpiPixmaps 已废弃，不再显式设置
    QApplication app(argc, argv); // 创建应用实例
    // 版本/组织信息必须在 addVersionOption() 之前设置，且决定日志与配置的用户目录
    QApplication::setApplicationName(QStringLiteral("DeepInvestQuant"));
    QApplication::setApplicationVersion(QStringLiteral("1.4.0"));
    QApplication::setOrganizationName(QStringLiteral("DeepInvestQuant"));

    QCommandLineParser parser; // 命令行解析器
    parser.setApplicationDescription(QStringLiteral("做多量化选股系统")); // 应用描述
    parser.addHelpOption(); // 添加 --help
    parser.addVersionOption(); // 添加 --version
    QCommandLineOption dateOpt(QStringList{QStringLiteral("d"), QStringLiteral("date")}, // 日期选项
                               QStringLiteral("指定交易日 yyyy-MM-dd"), QStringLiteral("date"));
    parser.addOption(dateOpt); // 注册日期选项
    QCommandLineOption smokeOpt(QStringLiteral("uismoke"), // UI 冒烟选项
                                QStringLiteral("UI 冒烟测试：验证打开即见荐股"));
    parser.addOption(smokeOpt);
    QCommandLineOption chartOpt(QStringLiteral("chartsmoke"), // 图表冒烟选项
                                QStringLiteral("图表冒烟：渲染 日/周/月 K 线 + MACD/KDJ/RSI 副图并落 PNG"));
    parser.addOption(chartOpt);
    QCommandLineOption coreOpt(QStringLiteral("corechecks"), // 离线门禁选项
                                QStringLiteral("离线门禁：跑完 tests/CoreChecks 的全部断言（与 DabanQuantCoreChecks.exe 同源）"));
    parser.addOption(coreOpt);
    QCommandLineOption detailOpt(QStringLiteral("detailprobe"),
                                 QStringLiteral("详情数据自检：拉取指定代码的财报/资金流/机构持仓"),
                                 QStringLiteral("code"));
    parser.addOption(detailOpt);
    parser.process(app); // 解析命令行
    Diagnostics::install();   // 文件日志 + 崩溃转储（组织名已确定，用户目录正确）
    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::China)); // 默认中文区域

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion"))); // 使用 Fusion 样式
    app.setStyleSheet(Theme::styleSheet()); // 加载自定义 QSS

    if (parser.isSet(smokeOpt)) { // 若指定 UI 冒烟
        return runUiSmoke(app); // 执行并返回
    }
    if (parser.isSet(chartOpt)) { // 若指定图表冒烟
        return runChartSmoke(app);
    }
    if (parser.isSet(coreOpt)) { // 若指定离线门禁
        return CoreChecks::run() == 0 ? 0 : 1;
    }
    if (parser.isSet(detailOpt)) {
        return runDetailProbe(app, parser.value(detailOpt));
    }

    // 单实例：冒烟模式各自独立运行；正常模式禁止多开（重复推邮件 / 并发写库 / 重复轮询）
    const QString instanceKey = QStringLiteral("DabanQuant.SingleInstance.v1");
    if (SingleInstance::tryActivateExisting(instanceKey)) {
        qInfo().noquote() << QStringLiteral("已有做多量化选股系统实例在运行，激活后退出");
        return 0;
    }

    // ---- 合规前置门禁：未明确同意免责声明前，不创建主窗口、不发起任何行情请求 ----
    // 阻断点放在 MainWindow 构造之前 —— 未同意时选股 / 行情 / 分析内容根本不会被构建，
    // 而不是「先显示再弹窗遮住」。
    // 对话框本身是唯一窗口：临时关闭「最后窗口关闭即退出」，避免嵌套事件循环提前结束。
    const bool quitOnLastWindow = QApplication::quitOnLastWindowClosed();
    QApplication::setQuitOnLastWindowClosed(false);
    const bool disclaimerAccepted = DisclaimerDialog::ensureAccepted();
    QApplication::setQuitOnLastWindowClosed(quitOnLastWindow);
    if (!disclaimerAccepted) {
        qInfo().noquote() << QStringLiteral("用户未同意免责声明，程序退出");
        return 0;
    }

    MainWindow w; // 正常模式：创建主窗口
    w.show(); // 显示
    if (!SingleInstance::listen(instanceKey, &w, [&w]() {
            w.showNormal();
            w.raise();
            w.activateWindow();
        })) {
        qWarning().noquote() << QStringLiteral("单实例监听建立失败（不影响主功能）");
    }

    const QStringList args = parser.values(dateOpt); // 获取日期参数值
    if (!args.isEmpty()) { // 若提供了日期
        const QDate d = QDate::fromString(args.first(), QStringLiteral("yyyy-MM-dd")); // 解析日期
        if (d.isValid()) { // 日期合法
            QMetaObject::invokeMethod(&w, "onScreenRequested", Qt::QueuedConnection, // 异步请求跳转
                                      Q_ARG(QDate, d));
        }
    }

    const int rc = app.exec(); // 进入主事件循环
    SingleInstance::release();
    return rc;
}