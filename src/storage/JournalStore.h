#pragma once

#include "storage/TradeRecord.h"
#include <QString>
#include <QVector>

// ============================================================================
// JournalStore：交易日志的存储抽象
//
// 交易日志（成交流水）是**唯一需要长期留档、可能跨设备查看**的数据，
// 因此把它从「SQLite 的一个表」升级成独立抽象：
//
//   · storage/Database   —— SQLite 后端（本地默认，离线可用，永远兜底可用）
//   · storage/MySqlStore —— MySQL 后端（服务器集中存储，多机共享同一份流水）
//
// 界面层（ui/JournalPanel）只依赖本接口，不感知底层是 SQLite 还是 MySQL，
// 因此切换后端不需要改动任何 UI 代码。
//
// 约定：
//   · addTrade 成功后必须回填 rec.id（MySQL 侧用 LAST_INSERT_ID 取回）；
//   · trades() 统一按「买入日倒序 + id 倒序」返回，与后端无关；
//   · 失败时返回 false，并在 lastError() 里给出可读原因。
// ============================================================================

class JournalStore
{
public:
    virtual ~JournalStore() = default;

    virtual bool addTrade(TradeRecord& rec) = 0;      // 新增一条，回填 rec.id
    virtual bool updateTrade(const TradeRecord& rec) = 0; // 按 rec.id 覆盖更新
    virtual bool deleteTrade(int id) = 0;             // 删除一条
    virtual QVector<TradeRecord> trades() const = 0;  // 全量流水（买入日倒序）

    virtual QString lastError() const = 0;            // 最近一次失败的原因
    virtual QString describe() const = 0;              // 展示用：当前后端与连接信息
};
