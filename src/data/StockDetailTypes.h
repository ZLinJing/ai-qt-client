#pragma once

#include <QString>
#include <QVector>
#include <QDate>
#include <QMetaType>

// 「查看详情」补充数据：财报、资金流、机构持仓。
// 全部来自东方财富公开接口（见 StockDetailSource），按标的异步加载；
// 任何字段缺失都保持默认值 + valid=false，界面显示「暂无数据」，绝不填充猜测值。

// 最近一期财报核心指标（东财 RPT_LICO_FN_CPD）
struct FinanceReport {
    QString period;             // "2026年 三季报"
    QString reportDate;         // "2026-09-30"
    double revenue = 0.0;       // 营业收入（元）
    double revenueYoy = 0.0;    // 营收同比（%）
    double netProfit = 0.0;     // 归母净利润（元）
    double netProfitYoy = 0.0;  // 净利润同比（%）
    double roe = 0.0;           // 加权净资产收益率（%）
    bool valid = false;
};

// 单日资金流（东财 fflow/daykline，口径：主力 = 大单 + 超大单）
struct MoneyFlowDay {
    QDate date;
    double mainNet = 0.0;       // 主力净流入（元）
    double smallNet = 0.0;      // 小单净额（元）
    double midNet = 0.0;        // 中单净额（元）
    double bigNet = 0.0;        // 大单净额（元）
    double superBigNet = 0.0;   // 超大单净额（元）
    double mainPct = 0.0;       // 主力净额占成交额比（%）
    double bigPct = 0.0;        // 大单净额占成交额比（%）
    double superBigPct = 0.0;   // 超大单净额占成交额比（%）
};

// 机构持仓（东财 RPT_F10_MAIN_ORGHOLD，季度口径）
struct OrgHolding {
    QString reportDate;
    int orgNum = 0;             // 持仓机构家数
    int orgNumChange = 0;       // 较上期家数变化
    double sharesRatio = 0.0;   // 占流通股比例（%）
    double sharesChange = 0.0;  // 持股数变化（股）
    double marketCap = 0.0;     // 持仓市值（元）
    bool valid = false;
};

struct StockDetailData {
    QString code;
    bool financeLoaded = false;
    bool flowLoaded = false;
    bool orgLoaded = false;
    FinanceReport finance;
    OrgHolding org;
    QVector<MoneyFlowDay> flows;   // 按日期升序，最多保留最近 20 个交易日
    QString error;                 // 拉取失败的说明（供界面提示，不隐藏失败）
};

Q_DECLARE_METATYPE(StockDetailData)
