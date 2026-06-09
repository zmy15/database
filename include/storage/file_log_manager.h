#pragma once

#include "storage/log_manager.h"
#include "storage/disk_manager.h"
#include <fstream>
#include <mutex>
#include <vector>
#include <optional>
#include <string>

namespace db {

/**
 * @brief FileLogManager — 基于文件的 WAL 日志管理器实现
 *
 * 日志文件格式（每条记录）：
 *   [lsn (4B)] [txn_id (4B)] [op_type (1B)] [page_id (4B)] [slot_num (4B)]
 *   [prev_lsn (4B)]
 *   [old_tuple_size (4B)] [old_tuple_data...]
 *   [new_tuple_size (4B)] [new_tuple_data...]
 *   [record_size (4B)]  ← 尾部校验：整条记录的总字节数
 */
class FileLogManager : public LogManager {
public:
    explicit FileLogManager(const std::string& wal_file_path);
    ~FileLogManager() override;

    lsn_t AppendLogRecord(const LogRecord& record) override;
    void FlushLogs() override;
    lsn_t GetFlushedLSN() const override;
    lsn_t GetNextLSN() const override;

    // 从 WAL 文件读取所有日志记录（用于崩溃恢复）
    std::vector<LogRecord> ReadLogRecords() override;

    // 截断 WAL 文件到指定 LSN 之后（恢复完成后清理）
    void TruncateAfter(lsn_t target_lsn) override;

    // 完全清空 WAL 文件（正常关闭时调用）
    void TruncateAll() override;
private:
    // 将一条 LogRecord 序列化写入文件缓冲区
    void WriteRecordToFile(const LogRecord& record);

    // 打开 WAL 文件（如不存在则创建）
    void OpenWALFile();

    // 从文件当前位置读取一条日志记录
    std::optional<LogRecord> ReadSingleRecord();

    // 不加锁的内部刷盘版本，仅供已持有 mutex_ 的方法调用（避免 std::mutex 重入死锁）
    void FlushLogsInternal();

    static constexpr size_t kWriteBufferSize = 4096 * 4; // 16KB 写缓冲

    std::string wal_file_path_;
    std::fstream wal_file_;
    std::vector<char> write_buffer_;
    lsn_t next_lsn_ = 0;
    lsn_t flushed_lsn_ = -1;
    std::mutex mutex_;
};

} // namespace db
