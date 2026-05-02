#pragma once

#include "common/config.h"

namespace db {

enum class IsolationLevel { READ_COMMITTED, REPEATABLE_READ, SERIALIZABLE };

// 事务上下文
class Transaction {
public:
    Transaction(txn_id_t txn_id, IsolationLevel iso_level)
        : txn_id_(txn_id), isolation_level_(iso_level) {}

    txn_id_t GetTransactionId() const { return txn_id_; }
    // 记录事务持有的锁、Undo/Redo日志等，用于回滚

private:
    txn_id_t txn_id_;
    IsolationLevel isolation_level_;
    lsn_t prev_lsn_ = -1; // 串联该事务的日志链
    // std::unordered_set<RID> shared_lock_set_; 
    // std::unordered_set<RID> exclusive_lock_set_;
};

} // namespace db
