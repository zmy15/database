#pragma once

#include "common/config.h"
#include "storage/tuple.h"

namespace db {

// 日志操作类型枚举
enum class LogOpType : uint8_t {
    INSERT = 0,
    DELETE = 1,
    UPDATE = 2,
    BEGIN_TXN = 3,
    COMMIT_TXN = 4,
    ABORT_TXN = 5
};

// 日志记录结构 (Write-Ahead Log Record)
struct LogRecord {
    lsn_t lsn;               // 本条日志的 LSN
    txn_id_t txn_id;         // 所属事务 ID
    LogOpType op_type;       // 操作类型
    page_id_t page_id;       // 受影响的页 ID
    uint32_t slot_num;       // 受影响的槽位号
    Tuple old_tuple;         // 旧值（UNDO 用，DELETE/UPDATE 时有效）
    Tuple new_tuple;         // 新值（REDO 用，INSERT/UPDATE 时有效）
    lsn_t prev_lsn;          // 事务中上一条日志的 LSN（事务级 LSN 链，用于回滚）
};

// 日志管理器：负责 WAL 的写入与刷盘
class LogManager {
public:
    virtual ~LogManager() = default;
    virtual lsn_t AppendLogRecord(const LogRecord& record) = 0;
    virtual void FlushLogs() = 0; // fsync 到磁盘

    // 返回最近一次成功刷盘（fsync）的 LSN
    virtual lsn_t GetFlushedLSN() const = 0;

    // 返回下一条日志将分配的 LSN（供调用方提前知道）
    virtual lsn_t GetNextLSN() const = 0;

    // 从 WAL 文件读取所有日志记录（用于崩溃恢复）
    virtual std::vector<LogRecord> ReadLogRecords() = 0;

    // 截断 WAL 文件到指定 LSN 之后（恢复完成后清理）
    virtual void TruncateAfter(lsn_t target_lsn) = 0;
};

} // namespace db
