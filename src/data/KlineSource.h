#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 KlineSeries / Candle
#include "data/HttpClient.h"                     // 复用统一取数器
#include <QObject>                               // 基类（信号槽）
#include <QStringList>                           // 批量代码列表
#include <QHash>                                 // 待回填状态表

// KlineSource：腾讯前复权日 K
// URL: web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh600519,day,,,180,qfq
// 返回 data.<symbol>.qfqday = [[日期, 开, 收, 高, 低, 成交量(手)], ...]（顺序是 开/收/高/低）

class KlineSource : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit KlineSource(HttpClient* http, QObject* parent = nullptr);  // 注入取数器

    void fetch(const QStringList& codes, int days = 120);  // 批量拉取（内部限流并发）

    static QString toSymbol(const QString& code);    // 6 位代码 → 腾讯 symbol（6/9/5 开头为 sh）
    static QString toCode(const QString& symbol);    // symbol → 6 位代码

Q_SIGNALS:
    void klineReady(const QString& code, const KlineSeries& series);  // 单只 K 线就绪
    void allDone(int success, int failed);                            // 整批结束

private:
    struct Pending {                             // 单只票的待回填状态
        int remaining = 0;     // 该代码还有几个请求未回来（qfq + 不复权）
        KlineSeries series;                      // 累积的 K 线数据
    };

    void settle(const QString& code);            // 两个请求都回来后再广播

    HttpClient* m_http = nullptr;                // 外部注入的取数器
    QHash<QString, Pending> m_pending;           // 代码 → 待回填状态
    int m_total = 0;                             // 本批总数
    int m_ok = 0;                                // 本批成功数
    int m_fail = 0;                              // 本批失败数
};
