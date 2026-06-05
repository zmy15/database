#pragma once

#include "common/config.h"
#include "common/rid.h"
#include <unordered_set>

namespace db {

enum class IsolationLevel { READ_COMMITTED, REPEATABLE_READ, SERIALIZABLE };

// 事务上下文
class Transaction {
public:
    Transaction(txn_id_t txn_id, IsolationLevel iso_level)
        : txn_id_(txn_id), isolation_level_(iso_level) {}

    txn_id_t GetTransactionId() const { return txn_id_; }
    IsolationLevel GetIsolationLevel() const { return isolation_level_; }
    lsn_t GetPrevLSN() const { return prev_lsn_; }
    void SetPrevLSN(lsn_t lsn) { prev_lsn_ = lsn; }

    // 锁集合管理（供 LockManager 维护）
    std::unordered_set<RID>& GetSharedLockSet() { return shared_lock_set_; }
    std::unordered_set<RID>& GetExclusiveLockSet() { return exclusive_lock_set_; }
    void AddSharedLock(const RID& rid) { shared_lock_set_.insert(rid); }
    void AddExclusiveLock(const RID& rid) { exclusive_lock_set_.insert(rid); }
    void RemoveLock(const RID& rid) {
        shared_lock_set_.erase(rid);
        exclusive_lock_set_.erase(rid);
    }

private:
    txn_id_t txn_id_;
    IsolationLevel isolation_level_;
    lsn_t prev_lsn_ = -1;
    std::unordered_set<RID> shared_lock_set_;
    std::unordered_set<RID> exclusive_lock_set_;
};

} // namespace db
