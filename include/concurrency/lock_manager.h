#pragma once

#include "common/config.h"
#include "concurrency/transaction.h"
#include "storage/log_manager.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <vector>

namespace db {

// 前向声明
class TransactionManager;

// 锁管理器接口：实现两阶段锁协议 (2PL)
class LockManager {
public:
    virtual ~LockManager() = default;

    virtual bool LockShared(Transaction* txn, const std::string& record_id) = 0;
    virtual bool LockExclusive(Transaction* txn, const std::string& record_id) = 0;
    virtual bool Unlock(Transaction* txn, const std::string& record_id) = 0;
    virtual bool UnlockAll(Transaction* txn) = 0;
    virtual void SetTransactionManager(TransactionManager* mgr) = 0;
};

// TwoPLManager — 两阶段锁协议的具体实现
class TwoPLManager : public LockManager {
public:
    TwoPLManager() = default;

    // 设置事务管理器（用于死锁检测时回调 Abort）
    void SetTransactionManager(TransactionManager* mgr) override { txn_manager_ = mgr; }

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
    std::recursive_mutex mutex_;

    // 死锁检测：等待图 (waits-for graph)
    // waits_for_[A] = {B, C} 表示事务 A 在等待 B 和 C 释放锁
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> waits_for_;
    TransactionManager* txn_manager_{nullptr};

    // 死锁检测辅助方法
    bool DetectCycle(txn_id_t start);
    std::vector<txn_id_t> CollectReachableNodes(txn_id_t start);
    txn_id_t SelectVictim(const std::vector<txn_id_t>& nodes);
    void RemoveWaitsFor(txn_id_t tid);

    // 内部无重入锁版本（供递归重试用）
    bool LockSharedInternal(Transaction* txn, const std::string& record_id, int depth);
    bool LockExclusiveInternal(Transaction* txn, const std::string& record_id, int depth);
};

// TransactionManager — 事务生命周期管理
class TransactionManager {
public:
    TransactionManager(LockManager* lock_manager, LogManager* log_manager = nullptr);
    Transaction* Begin(IsolationLevel iso_level = IsolationLevel::READ_COMMITTED);
    void Commit(Transaction* txn);
    void Abort(Transaction* txn);
    // 通过事务 ID 获取事务对象（供死锁检测使用）
    Transaction* GetTransaction(txn_id_t txn_id);

private:
    LockManager* lock_manager_;
    LogManager* log_manager_;
    std::mutex mutex_;
    std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> txn_map_;
    txn_id_t next_txn_id_{0};
};

} // namespace db
