#pragma once

// ============================================================================
// SectorFlowSource：当日「概念板块」涨跌幅 + 主力资金流的客观数据
//
// 复盘页需要回答「当日领涨 / 领跌板块的资金流向」这类客观问题，
// 而不是让人去猜题材能持续几天。本源只做一件事：取一次东财板块列表，
// 把涨幅（f3）、主力净流入净额（f62，元）、主力净占比（f184，%）原样解析出来，
// 连同字段口径与抓取时间一起交给复盘层；不排序推断、不给持续性结论。
//
// 任何字段缺失 / 量纲异常 / 接口失败，都用 failed(reason) 明确上报，
// 调用方按「数据缺失」展示，绝不用题材涨幅反推资金流、也绝不填 0。
// ============================================================================

#include "review/DecisionReview.h"       // SectorFlowRow
#include <QDateTime>
#include <QObject>
#include <QVector>

class HttpClient;

class SectorFlowSource : public QObject
{
    Q_OBJECT
public:
    explicit SectorFlowSource(QObject* parent = nullptr);

    void fetch();                        // 拉取当日全部概念板块（单次请求）

    // 字段与单位口径（界面「数据来源」栏直接展示，可核对）
    static QString sourceText();

Q_SIGNALS:
    void ready(const QVector<DecisionReview::SectorFlowRow>& rows,
               const QDateTime& fetchedAt);
    void failed(const QString& reason);

private:
    void onReply(const QByteArray& raw);

    HttpClient* m_http = nullptr;
};
