#pragma once

#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "concurrency/lock_manager.h"
#include "execution/sql_parser.h"
#include <memory>
#include <iostream>

namespace db {

class DBEngine {
public:
    DBEngine();

    // 提供给用户的执行接口
    void ExecuteQuery(const std::string& sql);

private:
    class MockDiskManager : public DiskManager { public: void ReadPage(page_id_t, char*) override {} void WritePage(page_id_t, const char*) override {} page_id_t AllocatePage() override { return 0; } };
    class MockLogManager : public LogManager { public: lsn_t AppendLogRecord(const LogRecord&) override { return 0; } void FlushLogs() override {} };
    class MockBPM : public BufferPoolManager { public: MockBPM() = default; ~MockBPM() override {} Page* FetchPage(page_id_t) override { return nullptr; } bool UnpinPage(page_id_t, bool) override { return true; } Page* NewPage(page_id_t*) override { return nullptr; } bool FlushPage(page_id_t) override { return true; } };
    class MockLockManager : public LockManager { public: bool LockShared(Transaction*, const std::string&) override { return true; } bool LockExclusive(Transaction*, const std::string&) override { return true; } bool Unlock(Transaction*, const std::string&) override { return true; } };

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<LogManager> log_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<LockManager> lock_manager_;
    std::unique_ptr<TransactionManager> txn_manager_;
    std::unique_ptr<SQLParser> parser_;
};

} // namespace db
