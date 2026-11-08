#include "notify/PushService.h"          // 本模块声明
#include "notify/SmtpClient.h"           // 实际发信
#include "storage/Database.h"            // 去重指纹读写

#include <QCryptographicHash>            // SHA1 内容指纹
#include <QFile>                         // 迁移后清理旧明文配置
#include <QDate>                         // 默认日期
#include <QDateTime>                     // 时间戳
#include <QStringList>                   // 列表拼接
#include <QTimer>                        // 重试延时
#include <algorithm>                     // 通用算法

namespace {

QString esc(const QString& s)            // HTML 转义，防止名称里的特殊字符破坏排版
{
    return s.toHtmlEscaped();            // 交给 Qt 处理
}

QString money(double v)                  // 价格统一保留 2 位
{
    return QString::number(v, 'f', 2);
}

const char* kTableStyle =                // 表格整体样式
    "width:100%;border-collapse:collapse;font-size:13px;margin:6px 0 14px 0;";
const char* kThStyle =                   // 表头单元格样式
    "text-align:left;padding:6px 8px;background:#f2f5fa;color:#4a5260;"
    "border-bottom:1px solid #e2e6ed;font-weight:600;";
const char* kTdStyle =                   // 普通单元格样式
    "padding:6px 8px;border-bottom:1px solid #eef1f5;color:#1b2430";
const char* kTdNum =                     // 数字单元格样式（右对齐）
    "padding:6px 8px;border-bottom:1px solid #eef1f5;color:#1b2430;text-align:right";

// 单只标的的一行表格（列与站内荐股表完全一致：代码 名称 推荐理由 买入 目标 止损 仓位 状态）
QString planRow(const TradePlan& p, bool actionable)
{
    QString row =
        QStringLiteral("<tr>"
                       "<td style=\"%1\"><b>%2</b><br/><span style=\"color:#6b7480\">%3</span></td>"  // 标的
                       "<td style=\"%4\">%5</td>"             // 推荐理由
                       "<td style=\"%6\">%7 ~ %8</td>"        // 买入价
                       "<td style=\"%6;color:#c0392b\">%9 ~ %10</td>"   // 目标价
                       "<td style=\"%6;color:#1e8e5a\">%11 ~ %12</td>"  // 止损价
                       "<td style=\"%6\">%13%</td>"           // 仓位
                       "<td style=\"%4;color:%14\">%15</td>"  // 状态
                       "</tr>");
    row = row.arg(QLatin1String(kTdStyle))                        // 0 单元格样式
             .arg(esc(p.code))                                    // 1 代码
             .arg(esc(p.name))                                    // 2 名称
             .arg(QLatin1String(kTdStyle))                        // 3 文本单元格样式
             .arg(esc(p.reasonLine()))                            // 4 推荐理由
             .arg(QLatin1String(kTdNum))                          // 5 数字样式
             .arg(money(p.buyLow))                                // 6 买入下沿
             .arg(money(p.buyHigh))                               // 7 买入上沿
             .arg(money(p.takeProfitLow))                         // 8 目标下沿
             .arg(money(p.takeProfitHigh))                        // 9 目标上沿
             .arg(money(p.stopLossLow))                           // 10 止损下沿
             .arg(money(p.stopLossHigh))                          // 11 止损上沿
             .arg(p.positionPct, 0, 'f', 1)                       // 12 建议仓位
             .arg(actionable ? QStringLiteral("#c0392b") : QStringLiteral("#6b7480"))  // 13 状态颜色
             .arg(esc(p.statusTag()));                            // 14 状态标签
    return row;                                                  // 返回该行
}

// 按「是否可执行」筛选后生成整张表格
QString planTable(const QVector<TradePlan>& plans, bool actionable, const QString& lastHeader)
{
    QString rows;                            // 行累积
    for (const TradePlan& p : plans) {       // 遍历计划
        if (p.actionable == actionable) rows += planRow(p, actionable);  // 只取同类
    }
    if (rows.isEmpty()) return QString();    // 无内容则不输出表

    QString table = QStringLiteral("<table style=\"%1\"><tr>"
                                   "<th style=\"%2\">标的</th><th style=\"%2\">推荐理由</th>"
                                   "<th style=\"%2\">买入价</th><th style=\"%2\">目标价</th>"
                                   "<th style=\"%2\">止损价</th><th style=\"%2\">仓位</th>"
                                   "<th style=\"%2\">%3</th></tr>")
                        .arg(QLatin1String(kTableStyle))   // 表格样式
                        .arg(QLatin1String(kThStyle))      // 表头样式
                        .arg(lastHeader);                  // 末列表头文案
    return table + rows + QStringLiteral("</table>");      // 表头 + 行 + 结束
}

} // namespace

PushService::PushService(QObject* parent) : QObject(parent)
{
    m_smtp = new SmtpClient(this);                                    // 建 SMTP 客户端
    connect(m_smtp, &SmtpClient::transcript, this, &PushService::log); // 协议日志转发
    connect(m_smtp, &SmtpClient::finished, this, [this](bool ok, const QString& detail) {
        // 网络抖动 / 临时拒绝自动重试一次：邮件通道偶发失败不该让当日结论直接丢掉
        if (!ok && m_retryLeft > 0 && retryable(detail)) {
            --m_retryLeft;                                            // 消耗一次重试
            Q_EMIT log(QStringLiteral("发送失败（%1），1.5 秒后自动重试…").arg(detail));
            QTimer::singleShot(1500, this, [this]() {
                if (m_lastMsg.subject.isEmpty()) return;              // 无内容可重发
                m_smtp->send(m_cfg, m_lastMsg);                       // 原样重发
            });
            return;                                                   // 不解除忙状态
        }
        m_busy = false;                                               // 解除忙
        Q_EMIT busyChanged(false);                                    // 通知闲
        if (ok && !m_pendingDigest.isEmpty() && m_db && m_db->isOpen()) {  // 发送成功才记录指纹
            m_db->setSetting(QStringLiteral("notify.lastDigest"), m_pendingDigest);  // 写入内容指纹
            m_db->setSetting(QStringLiteral("notify.lastAt"),
                             QDateTime::currentDateTime().toString(Qt::ISODate));    // 写入时间
        }
        m_pendingDigest.clear();                                      // 清空待写入指纹
        m_retryLeft = 0;                                              // 复位重试额度
        Q_EMIT finished(ok, detail);                                  // 转发结果
    });
}

// 判断失败是否属于可自动重试的临时性问题（连接/超时/4xx 临时拒绝）
bool PushService::retryable(const QString& detail)
{
    return detail.contains(QStringLiteral("连接失败"))
        || detail.contains(QStringLiteral("超时"))
        || detail.contains(QStringLiteral("（421"))
        || detail.contains(QStringLiteral("（450"))
        || detail.contains(QStringLiteral("（451"))
        || detail.contains(QStringLiteral("（452"));
}

// 加载配置：顺带把历史明文授权码加密迁移到用户私有目录并删除旧文件
bool PushService::loadConfig()
{
    m_cfg = NotifyConfig::load();
    const QString canonical = NotifyConfig::canonicalPath();
    const QString loaded = NotifyConfig::defaultPath();

    if (m_cfg.passwordNeedsUpgrade || (loaded != canonical && !m_cfg.password.isEmpty())) {
        if (m_cfg.save(canonical)) {
            m_cfg.passwordNeedsUpgrade = false;
            if (loaded != canonical) QFile::remove(loaded);   // 清除含明文凭据的旧文件
            Q_EMIT log(QStringLiteral("SMTP 凭据已加密迁移到 %1").arg(canonical));
        } else {
            m_cfg.password.clear();          // 迁移失败宁可停用推送，也不让明文继续驻留
            Q_EMIT log(QStringLiteral("凭据加密保存失败，自动推送已停用，请在推送设置中重新输入授权码"));
        }
    }
    return true;                             // 始终成功（失败则用默认值）
}

// 保存配置
bool PushService::saveConfig()
{
    const bool ok = m_cfg.save();            // 写盘（凭据已加密）
    const QString target = NotifyConfig::canonicalPath();
    Q_EMIT log(ok ? QStringLiteral("配置已保存：%1").arg(target)         // 成功日志
                  : QStringLiteral("配置保存失败（目录不可写）：%1").arg(target));  // 失败日志
    return ok;                               // 返回结果
}

// 推送荐股结果：三重闸门 + 去重
void PushService::pushBundle(const RecommendBundle& bundle, const SentimentState& state,
                             bool manual)
{
    if (m_busy) {                            // 上一封还在发
        Q_EMIT finished(false, QStringLiteral("上一条推送仍在进行中"));
        return;                              // 直接拒绝
    }

    QString why;                             // 配置缺失原因
    if (!m_cfg.ready(&why)) {                // 配置不完整
        if (manual) {                        // 手动推送才提示
            Q_EMIT log(QStringLiteral("推送未配置：%1").arg(why));
            Q_EMIT finished(false, QStringLiteral("推送未配置：%1").arg(why));
        }
        return;                              // 自动模式静默跳过，避免刷屏
    }

    if (!manual && (!m_cfg.enabled || !m_cfg.autoPush)) return;  // 自动推送需总开关与自动开关都开

    int act = 0;                             // 可执行数量
    for (const TradePlan& p : bundle.plans) {  // 统计
        if (p.actionable) ++act;
    }
    if (act == 0 && !manual && !m_cfg.pushWhenNoSignal) {   // 无信号且未勾选「无信号也推」
        Q_EMIT log(QStringLiteral("自动推送跳过：今日没有可执行信号"
                                  "（如需推送观察名单，可勾选「无信号也推送」）"));
        return;                              // 跳过
    }

    const QString digest = digestOf(bundle);  // 内容指纹
    if (!manual && m_db && m_db->isOpen()     // 自动模式且库可用
        && m_db->getSetting(QStringLiteral("notify.lastDigest")) == digest) {  // 指纹相同
        Q_EMIT log(QStringLiteral("同内容已推送过，跳过（避免盘中重复轰炸）"));
        return;                              // 跳过
    }

    m_pendingDigest = digest;                // 暂存指纹（成功后才落库）
    const MailMessage msg = compose(m_cfg, bundle, state);  // 排版
    dispatch(msg.subject, msg.html, digest);                // 发信
}

// 纯排版：主题 + 正文
MailMessage PushService::compose(const NotifyConfig& cfg, const RecommendBundle& bundle,
                                 const SentimentState& state)
{
    MailMessage msg;                                          // 结果
    msg.subject = buildSubject(cfg, bundle, state);           // 生成主题
    msg.html = buildHtml(bundle, state);                      // 生成正文
    return msg;                                               // 返回
}

// 发送测试邮件
void PushService::sendTest()
{
    if (m_busy) {                                               // 正在发送
        Q_EMIT finished(false, QStringLiteral("上一条推送仍在进行中"));
        return;                                                 // 拒绝
    }
    QString why;                                                // 配置缺失原因
    if (!m_cfg.ready(&why)) {                                   // 配置不完整
        Q_EMIT log(QStringLiteral("测试发送失败：%1").arg(why));
        Q_EMIT finished(false, QStringLiteral("测试发送失败：%1").arg(why));
        return;                                                 // 结束
    }

    const QString now = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));  // 发送时间
    QString html;                                               // 正文
    html += QStringLiteral("<div style=\"font-family:'Microsoft YaHei',sans-serif;color:#1b2430\">");
    html += QStringLiteral("<h2 style=\"margin:0 0 10px 0\">做多量化选股系统 · 通道测试</h2>");  // 标题
    html += QStringLiteral("<p>这是一封配置验证邮件，收到即表示 SMTP 通道可用。</p>");     // 说明
    html += QStringLiteral("<p style=\"color:#8a93a1;font-size:12px\">发送时间：%1</p>").arg(now);  // 时间
    html += QStringLiteral("<p style=\"color:#8a93a1;font-size:12px\">服务器：%1:%2（%3）</p>")
                .arg(esc(m_cfg.host))                           // 服务器
                .arg(m_cfg.port)                                // 端口
                .arg(m_cfg.implicitSsl ? QStringLiteral("SSL") : QStringLiteral("STARTTLS"));  // 加密方式
    html += QStringLiteral("</div>");                           // 结束标签

    m_pendingDigest.clear();     // 测试邮件不参与去重
    dispatch(m_cfg.subjectPrefix + QStringLiteral(" 邮件通道测试"), html, QString());  // 发信
}

// 发送任意 HTML（简报 / 复盘）
void PushService::sendCustom(const QString& subject, const QString& html)
{
    if (m_busy) {                                               // 正在发送
        Q_EMIT finished(false, QStringLiteral("上一条推送仍在进行中"));
        return;                                                 // 拒绝
    }
    QString why;                                                // 配置缺失原因
    if (!m_cfg.ready(&why)) {                                   // 配置不完整
        Q_EMIT log(QStringLiteral("简报未发送：%1").arg(why));
        Q_EMIT finished(false, QStringLiteral("简报未发送：%1").arg(why));
        return;                                                 // 结束
    }
    m_pendingDigest.clear();     // 简报不参与荐股去重
    dispatch(subject, html, QString());                         // 发信
}

// 实际发信：置忙、记日志、交给 SMTP 客户端
void PushService::dispatch(const QString& subject, const QString& html, const QString& digest)
{
    Q_UNUSED(digest);                        // 指纹已由调用方处理，此处仅保留签名对称
    m_busy = true;                           // 置忙
    Q_EMIT busyChanged(true);                // 通知忙
    Q_EMIT log(QStringLiteral("开始推送：%1 → %2")
                   .arg(subject, m_cfg.effectiveRecipients().join(QStringLiteral(", "))));  // 日志

    MailMessage msg;                         // 组装邮件
    msg.subject = subject;                   // 主题
    msg.html = html;                         // 正文
    m_lastMsg = msg;                         // 留存副本，失败时原样重发
    m_retryLeft = 1;                         // 每次新发送给一次自动重试额度
    m_smtp->send(m_cfg, msg);                // 异步发送
}

// 生成主题：前缀 + 日期 + 阶段档位 + 复合指数 + 可执行/观察数 + 仓位上限
QString PushService::buildSubject(const NotifyConfig& cfg, const RecommendBundle& bundle,
                                  const SentimentState& state)
{
    int act = 0, obs = 0;                    // 可执行 / 观察 计数
    for (const TradePlan& p : bundle.plans) {  // 统计
        if (p.actionable) ++act; else ++obs;
    }

    const QString day = bundle.tradeDate.isValid()                        // 交易日优先
                            ? bundle.tradeDate.toString(QStringLiteral("MM-dd"))
                            : QDate::currentDate().toString(QStringLiteral("MM-dd"));  // 否则用今天

    return QStringLiteral("%1 %2 复合%3 可执行%4/观察%5 上限%6%")
        .arg(cfg.subjectPrefix)              // 主题前缀
        .arg(day)                            // 日期
        .arg(state.composite, 0, 'f', 0)     // 复合指数
        .arg(act)                            // 可执行数
        .arg(obs)                            // 观察数
        .arg(state.positionCapPct, 0, 'f', 0);  // 仓位上限
}

// 生成 HTML 正文
QString PushService::buildHtml(const RecommendBundle& bundle, const SentimentState& state)
{
    QString h;                               // 正文累积
    h += QStringLiteral("<div style=\"font-family:'Microsoft YaHei',Arial,sans-serif;"
                        "color:#1b2430;max-width:780px\">");  // 外层容器

    h += QStringLiteral("<h2 style=\"margin:0 0 4px 0;font-size:20px\">做多量化选股系统 · %1</h2>")
             .arg(bundle.tradeDate.isValid()
                      ? bundle.tradeDate.toString(QStringLiteral("yyyy-MM-dd"))
                      : QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));  // 标题带日期
    h += QStringLiteral("<p style=\"margin:0 0 12px 0;color:#4a5260;font-size:14px\">"
                        "复合指数 <b>%1</b> ｜ 有效总仓 <b>≤%2%</b> ｜ 闸门 <b>%3</b></p>")
             .arg(state.composite, 0, 'f', 1)      // 复合指数
             .arg(state.positionCapPct, 0, 'f', 0) // 仓位上限
             .arg(state.tradeAllowed ? QStringLiteral("放行") : QStringLiteral("拦截"));

    h += QStringLiteral("<div style=\"background:#f6f8fb;border-left:3px solid #2f6fed;"
                        "padding:10px 12px;font-size:13px;line-height:160%;margin-bottom:14px\">%1</div>")
             .arg(esc(bundle.notice.isEmpty() ? state.note : bundle.notice));  // 顶部结论

    int act = 0, obs = 0;                    // 可执行 / 观察 计数
    for (const TradePlan& p : bundle.plans) {  // 统计
        if (p.actionable) ++act; else ++obs;
    }

    h += QStringLiteral("<h3 style=\"font-size:15px;margin:14px 0 4px 0\">可执行 %1 只</h3>").arg(act);  // 小节标题
    if (act == 0) {                          // 无可执行
        h += QStringLiteral("<p style=\"margin:0 0 10px 0;color:#8a93a1;font-size:13px\">"
                            "今日无可执行信号（情绪允许 ≠ 可以买，个股门槛未同时达标）。</p>");
    } else {
        h += planTable(bundle.plans, true, QStringLiteral("信号"));   // 可执行表格
    }

    if (obs > 0) {                           // 有观察项才输出
        h += QStringLiteral("<h3 style=\"font-size:15px;margin:14px 0 4px 0\">观察 %1 只</h3>").arg(obs);
        h += planTable(bundle.plans, false, QStringLiteral("未过"));  // 观察表格
    }

    if (!state.subScores.isEmpty()) {        // 有情绪分项才输出六维表
        h += QStringLiteral("<h3 style=\"font-size:15px;margin:16px 0 4px 0\">情绪六维</h3>");
        h += QStringLiteral("<table style=\"%1\"><tr>"
                            "<th style=\"%2\">维度</th><th style=\"%2\">得分</th>"
                            "<th style=\"%2\">权重</th><th style=\"%2\">明细</th></tr>")
                 .arg(QLatin1String(kTableStyle))   // 表格样式
                 .arg(QLatin1String(kThStyle));      // 表头样式
        for (const SentimentSubScore& s : state.subScores) {   // 逐维输出
            h += QStringLiteral("<tr><td style=\"%1\">%2 %3</td>"
                                "<td style=\"%4\">%5</td><td style=\"%4\">%6</td>"
                                "<td style=\"%1;color:#4a5260\">%7</td></tr>")
                     .arg(QLatin1String(kTdStyle))    // 单元格样式
                     .arg(esc(s.key))                 // 编号
                     .arg(esc(s.name))                // 名称
                     .arg(QLatin1String(kTdNum))      // 数字样式
                     .arg(s.available ? QString::number(s.score, 'f', 1) : QStringLiteral("—"))  // 得分
                     .arg(s.weight, 0, 'f', 2)        // 权重
                     .arg(esc(s.detail));             // 明细
        }
        h += QStringLiteral("</table>");      // 结束表格
    }

    h += QStringLiteral("<p style=\"margin-top:18px;color:#8a93a1;font-size:12px;line-height:170%\">"
                        "生成时间：%1 ｜ 数据源：东财 / 腾讯 / 新浪<br/>"
                        "本邮件由做多量化选股系统自动生成，仅用于策略研究，不构成投资建议。"
                        "做多交易有风险，请严格执行止损与仓位纪律。</p>")
             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));  // 免责声明
    h += QStringLiteral("</div>");            // 结束容器
    return h;                                 // 返回正文
}

// 内容指纹：日期 + 结论 + 每只（代码/是否可执行/仓位/信号级别）的 SHA1 前 24 位
QString PushService::digestOf(const RecommendBundle& bundle)
{
    QString raw = bundle.tradeDate.toString(Qt::ISODate);  // 日期
    raw += QLatin1Char('|');                              // 分隔符
    raw += bundle.notice;                                 // 顶部结论
    for (const TradePlan& p : bundle.plans) {             // 逐只计划
        raw += QStringLiteral("|%1:%2:%3:%4")
                   .arg(p.code)                            // 代码
                   .arg(p.actionable ? 1 : 0)              // 是否可执行
                   .arg(p.positionPct, 0, 'f', 1)          // 仓位
                   .arg(p.signalLevel);                    // 信号级别
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1).toHex().left(24));  // SHA1 截断
}
