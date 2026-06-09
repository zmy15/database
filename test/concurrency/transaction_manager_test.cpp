#include <gtest/gtest.h>
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"

using namespace db;

// ============================================================
// 测试辅助：创建 TransactionManager + LockManager 组合
// ============================================================
class TransactionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        lock_mgr_ = std::make_unique<TwoPLManager>();
        txn_mgr_ = std::make_unique<TransactionManager>(lock_mgr_.get());
    }

    std::unique_ptr<TwoPLManager> lock_mgr_;
    std::unique_ptr<TransactionManager> txn_mgr_;
};

// ============================================================
// 测试 1: 开始事务分配唯一 ID
// ============================================================
TEST_F(TransactionManagerTest, BeginAssignsUniqueId) {
    Transaction* txn1 = txn_mgr_->Begin();
    Transaction* txn2 = txn_mgr_->Begin();

    EXPECT_NE(txn1->GetTransactionId(), txn2->GetTransactionId());
    EXPECT_EQ(txn1->GetTransactionId(), 0);
    EXPECT_EQ(txn2->GetTransactionId(), 1);
}

// ============================================================
// 测试 2: 提交事务清理状态
// ============================================================
TEST_F(TransactionManagerTest, CommitCleansUp) {
    Transaction* txn = txn_mgr_->Begin();
    txn_id_t tid = txn->GetTransactionId();

    txn_mgr_->Commit(txn);

    // 新事务应能复用 ID（因为旧事务已从 map 移除）
    Transaction* txn2 = txn_mgr_->Begin();
    EXPECT_EQ(txn2->GetTransactionId(), tid + 1);
}

// ============================================================
// 测试 3: 提交时释放锁
// ============================================================
TEST_F(TransactionManagerTest, CommitReleasesLocks) {
    Transaction* txn1 = txn_mgr_->Begin();
    Transaction* txn2 = txn_mgr_->Begin();

    // txn1 获取独占锁
    lock_mgr_->LockExclusive(txn1, "record_1");

    // 提交 txn1
    txn_mgr_->Commit(txn1);

    // txn2 应该能够获取该锁
    EXPECT_TRUE(lock_mgr_->LockExclusive(txn2, "record_1"));
}

// ============================================================
// 测试 4: 中止时释放锁
// ============================================================
TEST_F(TransactionManagerTest, AbortReleasesLocks) {
    Transaction* txn1 = txn_mgr_->Begin();
    Transaction* txn2 = txn_mgr_->Begin();

    lock_mgr_->LockExclusive(txn1, "record_1");

    // 中止 txn1
    txn_mgr_->Abort(txn1);

    // txn2 应该能够获取该锁
    EXPECT_TRUE(lock_mgr_->LockExclusive(txn2, "record_1"));
}

// ============================================================
// 测试 5: 隔离级别存储正确
// ============================================================
TEST_F(TransactionManagerTest, IsolationLevelStored) {
    Transaction* txn1 = txn_mgr_->Begin(IsolationLevel::READ_COMMITTED);
    Transaction* txn2 = txn_mgr_->Begin(IsolationLevel::SERIALIZABLE);

    EXPECT_EQ(txn1->GetIsolationLevel(), IsolationLevel::READ_COMMITTED);
    EXPECT_EQ(txn2->GetIsolationLevel(), IsolationLevel::SERIALIZABLE);
}

// ============================================================
// 测试 6: 多语句事务生命周期 — BEGIN → 操作 → COMMIT 流程
// ============================================================
TEST_F(TransactionManagerTest, MultiStatementTransactionLifecycle) {
    // 模拟多语句事务：开始事务 → 执行多条操作 → 提交
    Transaction* txn = txn_mgr_->Begin(IsolationLevel::REPEATABLE_READ);
    txn_id_t tid = txn->GetTransactionId();

    // 事务初始状态为 ACTIVE
    EXPECT_EQ(txn->GetState(), TransactionState::ACTIVE);
    EXPECT_EQ(txn->GetIsolationLevel(), IsolationLevel::REPEATABLE_READ);

    // 第一步操作：获取共享锁（模拟 SELECT）
    EXPECT_TRUE(lock_mgr_->LockShared(txn, "table_users"));
    EXPECT_EQ(txn->GetSharedLockSet().size(), 1);

    // 第二步操作：获取另一个共享锁（模拟第二个 SELECT）
    EXPECT_TRUE(lock_mgr_->LockShared(txn, "table_products"));
    EXPECT_EQ(txn->GetSharedLockSet().size(), 2);

    // 提交事务
    txn_mgr_->Commit(txn);

    // 提交后锁应被释放，事务状态变为 COMMITTED
    EXPECT_EQ(txn->GetState(), TransactionState::COMMITTED);
    EXPECT_EQ(txn->GetSharedLockSet().size(), 0);
    EXPECT_EQ(txn->GetExclusiveLockSet().size(), 0);

    // 验证锁已释放：新事务可以获取这些锁
    Transaction* txn2 = txn_mgr_->Begin();
    EXPECT_TRUE(lock_mgr_->LockExclusive(txn2, "table_users"));
    txn_mgr_->Commit(txn2);
}

// ============================================================
// 测试 7: 多语句事务 ABORT — 中止后释放所有锁
// ============================================================
TEST_F(TransactionManagerTest, AbortReleasesLocksAfterMultiStatement) {
    Transaction* txn = txn_mgr_->Begin(IsolationLevel::READ_COMMITTED);

    // 执行多条操作：获取共享锁和排他锁
    EXPECT_TRUE(lock_mgr_->LockShared(txn, "table_1"));
    EXPECT_TRUE(lock_mgr_->LockExclusive(txn, "table_2"));
    EXPECT_EQ(txn->GetSharedLockSet().size(), 1);
    EXPECT_EQ(txn->GetExclusiveLockSet().size(), 1);

    // 中止事务
    txn_mgr_->Abort(txn);

    // 中止后锁应全部释放，状态变为 ABORTED
    EXPECT_EQ(txn->GetState(), TransactionState::ABORTED);
    EXPECT_EQ(txn->GetSharedLockSet().size(), 0);
    EXPECT_EQ(txn->GetExclusiveLockSet().size(), 0);

    // 验证锁已释放：其他事务可以获取
    Transaction* txn2 = txn_mgr_->Begin();
    EXPECT_TRUE(lock_mgr_->LockExclusive(txn2, "table_1"));
    EXPECT_TRUE(lock_mgr_->LockExclusive(txn2, "table_2"));
    txn_mgr_->Commit(txn2);
}
