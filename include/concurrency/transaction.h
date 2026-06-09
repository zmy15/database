#pragma once

#include "common/config.h"
#include <string>
#include <unordered_set>

namespace db {

enum class IsolationLevel { READ_COMMITTED, REPEATABLE_READ, SERIALIZABLE };
// 事务生命周期状态
enum class TransactionState {
    ACTIVE,    // 事务正在执行中
    COMMITTED, // 事务已提交
    ABORTED    // 事务已中止
};

// 事务上下文
class Transaction {
public:
    Transaction(txn_id_t txn_id, IsolationLevel iso_level)
        : txn_id_(txn_id), isolation_level_(iso_level) {}

    txn_id_t GetTransactionId() const { return txn_id_; }
    IsolationLevel GetIsolationLevel() const { return isolation_level_; }

    // 事务状态管理
    TransactionState GetState() const { return state_; }
    void SetState(TransactionState s) { state_ = s; }

    // 事务开始时的 LSN（REPEATABLE_READ 快照基准）
    lsn_t GetBeginLSN() const { return begin_lsn_; }
    void SetBeginLSN(lsn_t lsn) { begin_lsn_ = lsn; }

    lsn_t GetPrevLSN() const { return prev_lsn_; }
    void SetPrevLSN(lsn_t lsn) { prev_lsn_ = lsn; }

    // 锁集合管理（供 LockManager 维护）
    std::unordered_set<std::string>& GetSharedLockSet() { return shared_lock_set_; }
    std::unordered_set<std::string>& GetExclusiveLockSet() { return exclusive_lock_set_; }
    void AddSharedLock(const std::string& lock_key) { shared_lock_set_.insert(lock_key); }
    void AddExclusiveLock(const std::string& lock_key) { exclusive_lock_set_.insert(lock_key); }
    void RemoveLock(const std::string& lock_key) {
        shared_lock_set_.erase(lock_key);
        exclusive_lock_set_.erase(lock_key);
    }

private:
    txn_id_t txn_id_;
    IsolationLevel isolation_level_;
    TransactionState state_ = TransactionState::ACTIVE;
    lsn_t begin_lsn_ = 0;  // 事务开始时的 WAL LSN
    lsn_t prev_lsn_ = -1;
    std::unordered_set<std::string> shared_lock_set_;
    std::unordered_set<std::string> exclusive_lock_set_;
};

} // namespace db
