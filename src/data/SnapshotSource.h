#pragma once                                     // 防止头文件被重复包含

#include "core/Types.h"                          // 依赖 Snapshot
#include "data/HttpClient.h"                     // 复用统一取数器
#include <QObject>                               // 基类（信号槽）
#include <QHash>                                 // 代码索引

// SnapshotSource：新浪全市场快照
// URL: Market_Center.getHQNodeData?page=N&num=100&node=hs_a（单页 100，全市场约 55 页并发）
// 返回非严格 JSON（部分环境 key 无引号），解析时做正则兜底
// 唯一全量估值来源：pe / pb / 总市值 / 流通市值 / 换手率

class SnapshotSource : public QObject
{
    Q_OBJECT                                     // 启用 moc
public:
    explicit SnapshotSource(HttpClient* http, QObject* parent = nullptr);  // 注入取数器

    void fetchAll(const QString& node = QStringLiteral("hs_a"));  // 拉全市场（hs_a 沪深A股）

    void fetchCodes(const QStringList& codes);   // 只拉指定代码（候选池实时刷新）

    int lastPageCount() const { return m_lastPages; }  // 上一轮的分页数

Q_SIGNALS:
    void snapshotsReady(const QVector<Snapshot>& list);   // 全量快照就绪
    void partialReady(const QVector<Snapshot>& list);     // 部分快照就绪（按批回调）
    void failed(const QString& message);                  // 全量拉取失败

private:
    void requestPage(const QString& node, int page, int totalPages);  // 拉取单页

    HttpClient* m_http = nullptr;                // 外部注入的取数器
    QVector<Snapshot> m_accum;                   // 累积结果
    QHash<QString, Snapshot> m_codeIndex;        // 代码 → 快照索引
    int m_pages = 0;                             // 本轮计划页数
    int m_received = 0;                          // 已回包页数
    int m_lastPages = 0;                         // 上一轮页数（对外查询）
};
