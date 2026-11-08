#pragma once

#include "data/StockDetailTypes.h"
#include <QObject>
#include <QSharedPointer>
#include <QPointer>
#include <QStringList>

class HttpClient;

// StockDetailSource：按需拉取一只标的的「详情补充数据」
//
//  ① 最近一期财报   —— 东财 datacenter RPT_LICO_FN_CPD（营收/净利同比、加权 ROE）
//  ② 近 20 日资金流 —— 东财 push2his fflow/daykline（主力/大单/超大单净额与占比）
//  ③ 机构持仓       —— 东财 datacenter RPT_F10_MAIN_ORGHOLD（季度口径）
//
// 三个请求并发，全部结束（成功或失败）后统一发 ready；失败不伪造数据，
// 只把原因放进 StockDetailData::error，由界面显示「暂无数据」。
// 个股北向持股没有公开日频数据源，故不在此请求（界面明确标注）。
class StockDetailSource : public QObject
{
    Q_OBJECT
public:
    explicit StockDetailSource(HttpClient* http, QObject* parent = nullptr);

    void fetch(const QString& code);

Q_SIGNALS:
    void ready(const QString& code, const StockDetailData& data);

private:
    struct Job {
        StockDetailData data;
        QStringList errors;
        int pending = 3;
    };

    void finish(const QPointer<StockDetailSource>& guard, const QString& code,
                const QSharedPointer<Job>& job);
    void fetchFinance(const QString& code, const QSharedPointer<Job>& job);
    void fetchFlow(const QString& code, const QSharedPointer<Job>& job);
    void fetchOrg(const QString& code, const QSharedPointer<Job>& job);

    HttpClient* m_http = nullptr;
};
