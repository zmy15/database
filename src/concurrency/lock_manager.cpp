#include "concurrency/lock_manager.h"
#include "common/rid.h"
#include <cassert>

namespace db {

// ============================================================
// TwoPLManager 实现
// ============================================================

bool TwoPLManager::LockShared(Transaction* txn, const std::string& record_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto& entry = lock_table_[record_id];

    // 如果当前没有独占锁，或者独占锁就是当前事务自己持有的，允许加共享锁
    if (entry.exclusive_holder == txn->GetTransactionId()) {
        // 当前事务已持有独占锁，可以降级或共存（这里允许再加共享锁）
        entry.shared_holders.insert(txn->GetTransactionId());
        txn->AddSharedLock(RID(0, 0)); // 简化：用 dummy RID 记录
        return true;
    }

    if (entry.exclusive_holder != -1 && entry.exclusive_holder != txn->GetTransactionId()) {
        // 其他事务持有独占锁，拒绝
        return false;
    }

    // 没有独占锁，可以加共享锁
    entry.shared_holders.insert(txn->GetTransactionId());
    txn->AddSharedLock(RID(0, 0));
    return true;
}

bool TwoPLManager::LockExclusive(Transaction* txn, const std::string& record_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto& entry = lock_table_[record_id];

    // 如果当前事务已经持有独占锁
    if (entry.exclusive_holder == txn->GetTransactionId()) {
        return true;
    }

    // 如果有其他共享锁持有者（且不是当前事务自己），或者有其他独占锁持有者
    bool has_other_shared = false;
    for (auto holder : entry.shared_holders) {
        if (holder != txn->GetTransactionId()) {
            has_other_shared = true;
            break;
        }
    }

    if (has_other_shared || (entry.exclusive_holder != -1 && entry.exclusive_holder != txn->GetTransactionId())) {
        return false;
    }

    // 可以加独占锁
    // 如果当前事务之前持有共享锁，先清除
    entry.shared_holders.erase(txn->GetTransactionId());
    entry.exclusive_holder = txn->GetTransactionId();
    entry.is_exclusive = true;
    txn->AddExclusiveLock(RID(0, 0));
    return true;
}

bool TwoPLManager::Unlock(Transaction* txn, const std::string& record_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = lock_table_.find(record_id);
    if (it == lock_table_.end()) {
        return false;
    }

    auto& entry = it->second;
    txn_id_t tid = txn->GetTransactionId();

    if (entry.exclusive_holder == tid) {
        entry.exclusive_holder = -1;
        entry.is_exclusive = false;
        txn->RemoveLock(RID(0, 0));
        // 如果锁表项已空，清理
        if (entry.shared_holders.empty() && entry.exclusive_holder == -1) {
            lock_table_.erase(it);
        }
        return true;
    }

    if (entry.shared_holders.count(tid) > 0) {
        entry.shared_holders.erase(tid);
        txn->RemoveLock(RID(0, 0));
        if (entry.shared_holders.empty() && entry.exclusive_holder == -1) {
            lock_table_.erase(it);
        }
        return true;
    }

    return false;
}

bool TwoPLManager::UnlockAll(Transaction* txn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    txn_id_t tid = txn->GetTransactionId();

    // 遍历锁表，释放该事务持有的所有锁
    auto it = lock_table_.begin();
    while (it != lock_table_.end()) {
        auto& entry = it->second;
        bool erased = false;

        if (entry.exclusive_holder == tid) {
            entry.exclusive_holder = -1;
            entry.is_exclusive = false;
        }
        entry.shared_holders.erase(tid);

        // 清理空项
        if (entry.shared_holders.empty() && entry.exclusive_holder == -1) {
            it = lock_table_.erase(it);
            erased = true;
        }

        if (!erased) {
            ++it;
        }
    }

    // 清理事务中的锁集合
    txn->GetSharedLockSet().clear();
    txn->GetExclusiveLockSet().clear();

    return true;
}

// ============================================================
// TransactionManager 实现
// ============================================================

TransactionManager::TransactionManager(LockManager* lock_manager, LogManager* log_manager)
    : lock_manager_(lock_manager), log_manager_(log_manager) {}

Transaction* TransactionManager::Begin(IsolationLevel iso_level) {
    std::lock_guard<std::mutex> lock(mutex_);

    txn_id_t txn_id = next_txn_id_++;
    auto txn = std::make_unique<Transaction>(txn_id, iso_level);
    Transaction* raw = txn.get();
    txn_map_[txn_id] = std::move(txn);

    // 写入 BEGIN 日志记录
    if (log_manager_) {
        LogRecord record;
        record.txn_id = txn_id;
        record.op_type = LogOpType::BEGIN_TXN;
        record.page_id = INVALID_PAGE_ID;
        record.slot_num = 0;
        record.prev_lsn = -1;
        lsn_t lsn = log_manager_->AppendLogRecord(record);
        raw->SetPrevLSN(lsn);
    }

    return raw;
}

void TransactionManager::Commit(Transaction* txn) {
    // 写入 COMMIT 日志记录（在释放锁之前，确保崩溃后可判定事务已提交）
    if (log_manager_) {
        LogRecord record;
        record.txn_id = txn->GetTransactionId();
        record.op_type = LogOpType::COMMIT_TXN;
        record.page_id = INVALID_PAGE_ID;
        record.slot_num = 0;
        record.prev_lsn = txn->GetPrevLSN();
        lsn_t lsn = log_manager_->AppendLogRecord(record);
        txn->SetPrevLSN(lsn);
    }

    if (!txn) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // 释放事务持有的所有锁（进入缩减阶段）
    lock_manager_->UnlockAll(txn);

    // 从事务表中移除
    txn_map_.erase(txn->GetTransactionId());
}

void TransactionManager::Abort(Transaction* txn) {
    // 写入 ABORT 日志记录（在释放锁之前）
    if (log_manager_) {
        LogRecord record;
        record.txn_id = txn->GetTransactionId();
        record.op_type = LogOpType::ABORT_TXN;
        record.page_id = INVALID_PAGE_ID;
        record.slot_num = 0;
        record.prev_lsn = txn->GetPrevLSN();
        lsn_t lsn = log_manager_->AppendLogRecord(record);
        txn->SetPrevLSN(lsn);
    }

    if (!txn) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // 回滚时同样需要释放所有锁
    lock_manager_->UnlockAll(txn);

    // 从事务表中移除
    txn_map_.erase(txn->GetTransactionId());
}

} // namespace db
