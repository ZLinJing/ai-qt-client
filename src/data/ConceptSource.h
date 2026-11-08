#pragma once                                     // 防止头文件被重复包含

#include "data/HttpClient.h"                     // 复用统一取数器
#include <QObject>                               // 基类（信号槽）
#include <QDate>                                 // 日期类型
#include <QHash>                                 // 代码→题材、题材→涨幅
#include <QStringList>                           // 题材列表
#include <QTimer>                                // 匀速请求定时器

// ConceptSource：概念题材板块（打板用的是题材，不是行业）
// 行业口径会把「算力/机器人/固态电池」拆到不同行业，导致龙头被判孤板、杂毛凑成假板块
// 数据路径：概念板块列表 fs=m:90+t:3；成分股 fs=b:BKxxxx（只取涨幅前 N 个热门题材）

// 统计类 / 资金属性类「伪题材」判定：概念题材与板块资金流共用同一份黑名单，
// 避免两处排行榜口径不一致（例如「昨日涨停」「融资融券」顶掉真正的题材板块）。
bool isPseudoTheme(const QString& name);

class ConceptSource : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit ConceptSource(HttpClient* http, QObject* parent = nullptr);  // 注入取数器并建定时器

    void fetchThemes(int topN = 30);             // 拉取涨幅前 topN 的题材及其成分股

    bool ready() const { return m_ready; }       // 数据是否可用
    QDateTime fetchedAt() const { return m_fetchedAt; }  // 上次拉取时间

Q_SIGNALS:
    void themesReady(const QHash<QString, QStringList>& themesOfStock,  // 代码 → 所属题材列表
                     const QHash<QString, double>& themeChangePct);     // 题材 → 板块涨幅
    void failed(const QString& message);                                // 拉取失败

private:
    struct Board {                               // 单个概念板块
        QString code;      // BKxxxx       板块代码
        QString name;                        // 板块名称
        double changePct = 0.0;              // 板块涨幅 %
    };

    void onBoardList(const QByteArray& raw, int topN);  // 解析板块列表
    void requestMembers(int index);                     // 请求单个板块的成分股
    void pumpMembers();     // 匀速发出成分股请求（东财对突发并发会直接断连）

    HttpClient* m_http = nullptr;                // 外部注入的取数器
    QVector<Board> m_boards;                     // 排序后的目标板块
    QHash<QString, QStringList> m_themesOfStock; // 代码 → 题材列表
    QHash<QString, double> m_themeChange;        // 题材 → 涨幅
    int m_pending = 0;                           // 未回包的成分股请求数
    int m_nextIndex = 0;                         // 下一个待请求的板块下标
    QTimer* m_pump = nullptr;                    // 匀速泵定时器
    bool m_ready = false;                        // 是否已就绪
    QDateTime m_fetchedAt;                       // 拉取完成时间
};
