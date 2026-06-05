#pragma once

#include "common/config.h"
#include "concurrency/transaction.h"
#include "storage/log_manager.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>

namespace db {

// 锁管理器接口：实现两阶段锁协议 (2PL)
class LockManager {
public:
    virtual ~LockManager() = default;

    virtual bool LockShared(Transaction* txn, const std::string& record_id) = 0;
    virtual bool LockExclusive(Transaction* txn, const std::string& record_id) = 0;
    virtual bool Unlock(Transaction* txn, const std::string& record_id) = 0;
    virtual bool UnlockAll(Transaction* txn) = 0;
};

// TwoPLManager — 两阶段锁协议的具体实现
class TwoPLManager : public LockManager {
public:
    TwoPLManager() = default;

    bool LockShared(Transaction* txn, const std::string& record_id) override;
    bool LockExclusive(Transaction* txn, const std::string& record_id) override;
    bool Unlock(Transaction* txn, const std::string& record_id) override;
    bool UnlockAll(Transaction* txn) override;

private:
    struct LockEntry {
        bool is_exclusive = false;
        std::unordered_set<txn_id_t> shared_holders;
        txn_id_t exclusive_holder = -1;
    };
    std::unordered_map<std::string, LockEntry> lock_table_;
    std::mutex mutex_;
};

// TransactionManager — 事务生命周期管理
class TransactionManager {
public:
    TransactionManager(LockManager* lock_manager, LogManager* log_manager = nullptr);
    Transaction* Begin(IsolationLevel iso_level = IsolationLevel::READ_COMMITTED);
    void Commit(Transaction* txn);
    void Abort(Transaction* txn);

private:
    LockManager* lock_manager_;
    LogManager* log_manager_;
    std::mutex mutex_;
    std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> txn_map_;
    txn_id_t next_txn_id_{0};
};

} // namespace db
