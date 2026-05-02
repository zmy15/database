#pragma once

#include "common/config.h"
#include "concurrency/transaction.h"
#include <string>

namespace db {

// 锁管理器：实现两阶段锁协议 (2PL)
class LockManager {
public:
    virtual ~LockManager() = default;

    // 获取共享锁（读锁）
    virtual bool LockShared(Transaction* txn, const std::string& record_id) = 0;
    // 获取排他锁（写锁）
    virtual bool LockExclusive(Transaction* txn, const std::string& record_id) = 0;
    // 释放锁
    virtual bool Unlock(Transaction* txn, const std::string& record_id) = 0;
};

class TransactionManager {
public:
    Transaction* Begin(IsolationLevel iso_level = IsolationLevel::READ_COMMITTED);
    void Commit(Transaction* txn);
    void Abort(Transaction* txn);
};

} // namespace db
