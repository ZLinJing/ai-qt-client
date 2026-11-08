#pragma once                                     // 防止头文件被重复包含

#include <QDialog>
#include <QString>

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QTextBrowser;

// ============================================================================
// DisclaimerDialog：启动前置的免责声明 / 风险揭示页
//
// 两种形态：
//   Consent  —— 启动门禁。正文可滚动阅读；必须勾选「我已阅读并理解…」后，
//               「同意并继续」才可用；点击后写入本机同意记录并放行。
//               点击「不同意并退出」/ 关闭窗口 / 按 Esc 一律拒绝，调用方退出程序。
//   Document —— 已同意后从主界面「免责声明」入口查阅，只读、不重复写入记录。
//
// 未同意的阻断点只有一处：main.cpp 在创建 MainWindow **之前**调用
// ensureAccepted()，因此未同意时主窗口（及任何行情 / 选股内容）根本不会构建。
// ============================================================================

class DisclaimerDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Mode {
        Consent,     // 启动前置：必须明确同意
        Document,    // 只读查阅
    };

    explicit DisclaimerDialog(Mode mode = Mode::Consent, QWidget* parent = nullptr);

    // 是否已通过本对话框完成同意（Consent 模式下 accept() 时置 true）
    bool consented() const { return m_consented; }

    // 当前声明版本是否已在本机确认同意
    static bool isAccepted();

    // 启动门禁：已同意返回 true；否则弹出前置页，同意返回 true，拒绝返回 false
    static bool ensureAccepted(QWidget* parent = nullptr);

    // 打开只读全文（主界面「免责声明」入口）
    static void showDocument(QWidget* parent = nullptr);

private:
    void buildUi();
    QString renderHtml() const;
    void updateConsentState();
    void acceptConsent();

    Mode          m_mode = Mode::Consent;
    bool          m_consented = false;
    QFrame*       m_consentCard = nullptr;
    QCheckBox*    m_agree = nullptr;
    QPushButton*  m_accept = nullptr;
    QLabel*       m_hint = nullptr;
};
