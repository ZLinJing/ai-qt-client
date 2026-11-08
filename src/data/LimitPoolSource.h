#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 LimitEntry
#include "data/HttpClient.h"                     // 复用统一取数器
#include <QObject>                               // 基类（信号槽）
#include <QDate>                                 // 交易日
#include <QSet>                                  // 待 settle 的池类型集合

// LimitPoolSource：东方财富 push2ex 涨停 / 炸板 / 跌停池
// URL: push2ex.eastmoney.com/getTopicZTPool；参数 date=YYYYMMDD、sort=fbt:asc（涨停池必带）
// 字段: c/n/p/zdp/amount/ltsz/tshare/hs/lbc/fbt/lbt/fund/zbc/hybk

class LimitPoolSource : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit LimitPoolSource(HttpClient* http, QObject* parent = nullptr);  // 注入取数器

    void fetch(const QDate& date);               // 拉取指定交易日的三类池子

    void fetchOnly(const QDate& date, const QString& kind);  // 只拉一个池子（补历史交易日用）

    // 拉取「昨日涨停池」（getYesterdayZTPool，date 参数填目标交易日）：
    // 接口自带每只昨日涨停股的「今日涨跌幅」（zdp），可直接算昨日涨停溢价，
    // 不再依赖新浪快照拼装；prevDate = 池所属交易日（即昨日），请求日期为其次日。
    // 返回为空或失败时自动回退 getTopicZTPool 历史模式（溢价退回快照口径）。
    void fetchYesterdayPool(const QDate& prevDate);

Q_SIGNALS:
    void poolsReady(const QDate& date,                        // 三类池全部就绪
                    const QVector<LimitEntry>& limitUp,       // 涨停池
                    const QVector<LimitEntry>& broken,        // 炸板池
                    const QVector<LimitEntry>& limitDown);    // 跌停池
    void partialPoolReady(const QDate& date, const QString& kind,  // 单池就绪（补历史用）
                          const QVector<LimitEntry>& pool);        // 该池内容
    void failed(const QString& message);                      // 任一池拉取失败的通知

private:
    void requestPool(const QDate& date, const QString& kind);   // 发起单个池的请求
    void onPoolResult(const QDate& date, const QString& kind, const QByteArray& raw);  // 解析响应
    void onPoolError(const QDate& date, const QString& kind, const QString& err);      // 处理失败

    // 三类池全部 settle 后才广播且只广播一次，否则先返回的池会带着空数据广播出去
    void tryBroadcast(const QDate& date);

    HttpClient* m_http = nullptr;                // 外部注入的取数器
    QVector<LimitEntry> m_limitUp;               // 涨停池缓存
    QVector<LimitEntry> m_broken;                // 炸板池缓存
    QVector<LimitEntry> m_limitDown;             // 跌停池缓存

    QSet<QString> m_pending;    // 尚未 settle 的池类型
    bool           m_broadcast = false;          // 本轮是否已广播
    bool           m_singleMode = false;   // true = 只拉单池，走 partialPoolReady
    bool           m_yesterdayMode = false;      // true = 正在拉昨日涨停池（getYesterdayZTPool）
    QDate          m_date;                       // 本轮请求所属交易日
};
