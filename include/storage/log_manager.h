#pragma once

#include "common/config.h"

namespace db {

// 日志记录结构 (Write-Ahead Log Record)
struct LogRecord {
    lsn_t lsn;
    txn_id_t txn_id;
    // ... 操作类型(INSERT/UPDATE/DELETE), 旧值, 新值等
};

// 日志管理器：负责 WAL 的写入与刷盘
class LogManager {
public:
    virtual ~LogManager() = default;
    virtual lsn_t AppendLogRecord(const LogRecord& record) = 0;
    virtual void FlushLogs() = 0; // fsync 到磁盘
};

} // namespace db
