#include "concurrency/lock_manager.h"
#include <cassert>
#include <queue>
#include <algorithm>

namespace db {

// ============================================================
// TwoPLManager 实现
// ============================================================

bool TwoPLManager::LockShared(Transaction* txn, const std::string& record_id) {
        return LockSharedInternal(txn, record_id, 0);
}

bool TwoPLManager::LockExclusive(Transaction* txn, const std::string& record_id) {
        return LockExclusiveInternal(txn, record_id, 0);
}

// ============================================================
// 隔离级别感知的读锁方法
// ============================================================

bool TwoPLManager::LockSharedForRead(Transaction* txn, const std::string& record_id) {
    // 读操作用共享锁：READ_COMMITTED 下由调用方在语句结束后释放，
    // REPEATABLE_READ 下由调用方持有到事务提交
    return LockSharedInternal(txn, record_id, 0);
}

bool TwoPLManager::LockExclusiveForRead(Transaction* txn, const std::string& record_id) {
    // SERIALIZABLE：读操作也使用排他锁，完全串行化所有访问，防止幻读
    return LockExclusiveInternal(txn, record_id, 0);
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
        txn->RemoveLock(record_id);
        // 如果锁表项已空，清理
        if (entry.shared_holders.empty() && entry.exclusive_holder == -1) {
            lock_table_.erase(it);
        }
        return true;
    }

    if (entry.shared_holders.count(tid) > 0) {
        entry.shared_holders.erase(tid);
        txn->RemoveLock(record_id);
        if (entry.shared_holders.empty() && entry.exclusive_holder == -1) {
            lock_table_.erase(it);
        }
        return true;
    }
    return false;
}

    // ============================================================
    // 死锁检测辅助方法
    // ============================================================
    
    void TwoPLManager::RemoveWaitsFor(txn_id_t tid) {
        // 移除出边：该事务不再等待任何人
        waits_for_.erase(tid);
    
        // 移除入边：其他事务的等待集合中移除该事务
        for (auto& pair : waits_for_) {
            pair.second.erase(tid);
        }
    }
    
    std::vector<txn_id_t> TwoPLManager::CollectReachableNodes(txn_id_t start) {
        std::vector<txn_id_t> result;
        std::unordered_set<txn_id_t> visited;
        std::queue<txn_id_t> q;
    
        // 从 start 的邻居开始 BFS（不自包含 start，通过检测能否回到 start 判断环）
        auto it = waits_for_.find(start);
        if (it == waits_for_.end()) {
            return result;
        }
    
        for (txn_id_t neighbor : it->second) {
            if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                q.push(neighbor);
                result.push_back(neighbor);
            }
        }
    
        while (!q.empty()) {
            txn_id_t current = q.front();
            q.pop();
    
            auto nit = waits_for_.find(current);
            if (nit != waits_for_.end()) {
                for (txn_id_t neighbor : nit->second) {
                    if (visited.find(neighbor) == visited.end()) {
                        visited.insert(neighbor);
                        q.push(neighbor);
                        result.push_back(neighbor);
                    }
                }
            }
        }
    
        return result;
    }
    
    txn_id_t TwoPLManager::SelectVictim(const std::vector<txn_id_t>& nodes) {
        txn_id_t victim = -1;
        for (txn_id_t id : nodes) {
            // 确认事务仍然存活
            if (txn_manager_ && txn_manager_->GetTransaction(id) != nullptr) {
                // 选择最年轻的事务（txn_id 最大）作为受害者
                if (id > victim) {
                    victim = id;
                }
            }
        }
        return victim;
    }
    
    bool TwoPLManager::DetectCycle(txn_id_t start) {
        // 检查自环（A 直接等待 A）
        auto it = waits_for_.find(start);
        bool has_self_loop = (it != waits_for_.end() && it->second.count(start) > 0);
    
        // 收集从 start 出发可达的所有节点
        auto reachable = CollectReachableNodes(start);
    
        // 检查是否存在环（start 能通过其他节点回到自己）
        bool has_cycle = has_self_loop;
        if (!has_cycle) {
            for (txn_id_t node : reachable) {
                if (node == start) {
                    has_cycle = true;
                    break;
                }
            }
        }
    
        if (has_cycle) {
            // 构建候选受害者列表
            std::vector<txn_id_t> candidates = reachable;
            candidates.push_back(start);
    
            txn_id_t victim = SelectVictim(candidates);
            if (victim != -1 && txn_manager_) {
                Transaction* victim_txn = txn_manager_->GetTransaction(victim);
                if (victim_txn) {
                    txn_manager_->Abort(victim_txn);
                }
            }
            // 清理等待图中的受害者边
            RemoveWaitsFor(victim);
            return true;
        }
    
        return false;
    }
    
    // ============================================================
    // 内部锁方法（含死锁检测 + 重试逻辑）
    // ============================================================
    
    bool TwoPLManager::LockSharedInternal(Transaction* txn, const std::string& record_id, int depth) {
        constexpr int MAX_RETRY_DEPTH = 100;
        if (depth > MAX_RETRY_DEPTH) {
            return false;
        }
    
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        txn_id_t tid = txn->GetTransactionId();
        auto& entry = lock_table_[record_id];
    
        // 情况1：当前事务已持有独占锁，允许共存
        if (entry.exclusive_holder == tid) {
            entry.shared_holders.insert(tid);
            txn->AddSharedLock(record_id);
            return true;
        }
    
        // 情况2：无独占锁持有者，可以直接加共享锁
        if (entry.exclusive_holder == -1) {
            entry.shared_holders.insert(tid);
            txn->AddSharedLock(record_id);
            return true;
        }
    
        // 情况3：冲突 —— 其他事务持有独占锁
        txn_id_t blocker = entry.exclusive_holder;
        waits_for_[tid].insert(blocker);
    
        // 死锁检测
        bool had_cycle = DetectCycle(tid);
    
        if (had_cycle) {
            // 死锁已解决，检查阻塞是否解除
            auto check = lock_table_.find(record_id);
            if (check == lock_table_.end() || check->second.exclusive_holder == -1) {
                // 阻塞解除，重试
                return LockSharedInternal(txn, record_id, depth + 1);
            }
        }
    
        return false;
    }
    
    bool TwoPLManager::LockExclusiveInternal(Transaction* txn, const std::string& record_id, int depth) {
        constexpr int MAX_RETRY_DEPTH = 100;
        if (depth > MAX_RETRY_DEPTH) {
            return false;
        }
    
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        txn_id_t tid = txn->GetTransactionId();
        auto& entry = lock_table_[record_id];
    
        // 情况1：当前事务已持有独占锁
        if (entry.exclusive_holder == tid) {
            return true;
        }
    
        // 情况2：无任何持有者
        if (entry.exclusive_holder == -1 && entry.shared_holders.empty()) {
            entry.exclusive_holder = tid;
            entry.is_exclusive = true;
            txn->AddExclusiveLock(record_id);
            return true;
        }
    
        // 检查是否可以锁升级（只有当前事务持有共享锁）
        bool has_other_shared = false;
        for (auto holder : entry.shared_holders) {
            if (holder != tid) {
                has_other_shared = true;
                break;
            }
        }
    
        if (!has_other_shared && entry.exclusive_holder == -1) {
            // 锁升级：清除共享锁，设置独占锁
            entry.shared_holders.erase(tid);
            entry.exclusive_holder = tid;
            entry.is_exclusive = true;
            txn->AddExclusiveLock(record_id);
            return true;
        }
    
        // 情况3：冲突 —— 有其他持有者
        if (entry.exclusive_holder != -1 && entry.exclusive_holder != tid) {
            waits_for_[tid].insert(entry.exclusive_holder);
        }
        for (auto holder : entry.shared_holders) {
            if (holder != tid) {
                waits_for_[tid].insert(holder);
            }
        }
    
        // 死锁检测
        bool had_cycle = DetectCycle(tid);
    
        if (had_cycle) {
            auto check = lock_table_.find(record_id);
            if (check == lock_table_.end() ||
                (check->second.exclusive_holder == -1 && check->second.shared_holders.empty())) {
                return LockExclusiveInternal(txn, record_id, depth + 1);
            }
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
        // 记录事务开始时的 WAL LSN（作为 REPEATABLE_READ 快照基准）
        raw->SetBeginLSN(lsn);
    }

    return raw;
}

void TransactionManager::Commit(Transaction* txn) {
    if (!txn) return;

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
    // 更新事务状态为已提交
    txn->SetState(TransactionState::COMMITTED);

    // 释放事务持有的所有锁（在标记 COMMITTED 之后，确保崩溃恢复可判定事务已提交）
    lock_manager_->UnlockAll(txn);

    std::lock_guard<std::mutex> lock(mutex_);

    // 记录已提交事务 ID（MVCC 可见性判断用）
    committed_txns_.insert(txn->GetTransactionId());

    // 从活跃事务表中移除（committed_txns_ 已保留其 ID 供 MVCC 可见性查询）
    txn_map_.erase(txn->GetTransactionId());
}
    
Transaction* TransactionManager::GetTransaction(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = txn_map_.find(txn_id);
    if (it != txn_map_.end()) {
        return it->second.get();
    }
    return nullptr;
}

// ============================================================
// MVCC 可见性查询：IsCommitted / IsAborted
// ============================================================

bool TransactionManager::IsCommitted(txn_id_t txn_id) const {
    // 优先查 committed_txns_ 集合（O(1)）
    if (committed_txns_.count(txn_id) > 0) return true;
    // 回退到 txn_map_ 检查状态（处理系统恢复后仍未清理的事务）
    auto it = txn_map_.find(txn_id);
    if (it != txn_map_.end() && it->second->GetState() == TransactionState::COMMITTED) {
        return true;
    }
    return false;
}

bool TransactionManager::IsAborted(txn_id_t txn_id) const {
    if (aborted_txns_.count(txn_id) > 0) return true;
    auto it = txn_map_.find(txn_id);
    if (it != txn_map_.end() && it->second->GetState() == TransactionState::ABORTED) {
        return true;
    }
    return false;
}

void TransactionManager::Abort(Transaction* txn) {
    if (!txn) return;

    // 更新事务状态为已中止
    txn->SetState(TransactionState::ABORTED);

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

    // 释放事务持有的所有锁（MVCC 模式下 Abort 无需物理回滚，但必须释放锁）
    lock_manager_->UnlockAll(txn);

    std::lock_guard<std::mutex> lock(mutex_);

    // 记录已中止事务 ID（MVCC 可见性判断用）
    aborted_txns_.insert(txn->GetTransactionId());

    // 从活跃事务表中移除（aborted_txns_ 已保留其 ID 供 MVCC 可见性查询）
    txn_map_.erase(txn->GetTransactionId());
}

} // namespace db
