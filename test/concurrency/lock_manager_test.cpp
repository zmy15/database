#include <gtest/gtest.h>
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include <iostream>

using namespace db;

// ============================================================
// 测试 1: 基本共享锁获取
// ============================================================
TEST(LockManagerTest, BasicSharedLock) {
    TwoPLManager mgr;
    Transaction txn(1, IsolationLevel::READ_COMMITTED);

    EXPECT_TRUE(mgr.LockShared(&txn, "record_1"));
    EXPECT_EQ(txn.GetSharedLockSet().size(), 1);
}

// ============================================================
// 测试 2: 基本独占锁获取
// ============================================================
TEST(LockManagerTest, BasicExclusiveLock) {
    TwoPLManager mgr;
    Transaction txn(1, IsolationLevel::READ_COMMITTED);

    EXPECT_TRUE(mgr.LockExclusive(&txn, "record_1"));
    EXPECT_EQ(txn.GetExclusiveLockSet().size(), 1);
}

// ============================================================
// 测试 3: 共享锁兼容——多个事务可同时持有共享锁
// ============================================================
TEST(LockManagerTest, SharedLockCompatibility) {
    TwoPLManager mgr;
    Transaction txn1(1, IsolationLevel::READ_COMMITTED);
    Transaction txn2(2, IsolationLevel::READ_COMMITTED);

    EXPECT_TRUE(mgr.LockShared(&txn1, "record_1"));
    EXPECT_TRUE(mgr.LockShared(&txn2, "record_1"));
    EXPECT_EQ(txn1.GetSharedLockSet().size(), 1);
    EXPECT_EQ(txn2.GetSharedLockSet().size(), 1);
}

// ============================================================
// 测试 4: 独占锁与共享锁冲突——独占锁持有者阻止共享锁
// ============================================================
TEST(LockManagerTest, ExclusiveBlocksShared) {
    TwoPLManager mgr;
    Transaction txn1(1, IsolationLevel::READ_COMMITTED);
    Transaction txn2(2, IsolationLevel::READ_COMMITTED);

    EXPECT_TRUE(mgr.LockExclusive(&txn1, "record_1"));
    EXPECT_FALSE(mgr.LockShared(&txn2, "record_1"));
}

// ============================================================
// 测试 5: 解锁后其他事务可获取锁
// ============================================================
TEST(LockManagerTest, UnlockAllowsOthers) {
    TwoPLManager mgr;
    Transaction txn1(1, IsolationLevel::READ_COMMITTED);
    Transaction txn2(2, IsolationLevel::READ_COMMITTED);

    EXPECT_TRUE(mgr.LockExclusive(&txn1, "record_1"));
    EXPECT_TRUE(mgr.Unlock(&txn1, "record_1"));
    EXPECT_TRUE(mgr.LockExclusive(&txn2, "record_1"));
}

// ============================================================
// 测试 6: UnlockAll 释放所有锁
// ============================================================
TEST(LockManagerTest, UnlockAllReleasesAll) {
    TwoPLManager mgr;
    Transaction txn1(1, IsolationLevel::READ_COMMITTED);
    Transaction txn2(2, IsolationLevel::READ_COMMITTED);

    EXPECT_TRUE(mgr.LockShared(&txn1, "record_1"));
    EXPECT_TRUE(mgr.LockExclusive(&txn1, "record_2"));
    EXPECT_TRUE(mgr.UnlockAll(&txn1));

    // 释放后其他事务应能获取这些锁
    EXPECT_TRUE(mgr.LockExclusive(&txn2, "record_1"));
    EXPECT_TRUE(mgr.LockExclusive(&txn2, "record_2"));
    EXPECT_EQ(txn1.GetSharedLockSet().size(), 0);
    EXPECT_EQ(txn1.GetExclusiveLockSet().size(), 0);
}

// ============================================================
// 测试 7: 死锁检测 — A 持有锁1等锁2，B 持有锁2等锁1
// ============================================================
TEST(LockManagerTest, DeadlockDetection) {
    TwoPLManager mgr;
    TransactionManager txn_mgr(&mgr);
    mgr.SetTransactionManager(&txn_mgr);

    Transaction* txnA = txn_mgr.Begin();  // txn_id = 0
    Transaction* txnB = txn_mgr.Begin();  // txn_id = 1

    // A 获取 table_1 独占锁，B 获取 table_2 独占锁
    EXPECT_TRUE(mgr.LockExclusive(txnA, "table_1"));
    EXPECT_TRUE(mgr.LockExclusive(txnB, "table_2"));

    // A 尝试获取 table_2 → 冲突，记录 A→B 等待
    EXPECT_FALSE(mgr.LockExclusive(txnA, "table_2"));

    // B 尝试获取 table_1 → 冲突 → 检测到环 B→A→B
    // 最年轻的事务 B (id=1) 被中止
    EXPECT_FALSE(mgr.LockExclusive(txnB, "table_1"));

    // B 被中止后其锁被释放，A 重试应能获取 table_2
    EXPECT_TRUE(mgr.LockExclusive(txnA, "table_2"));
}

// ============================================================
// 测试 8: 死锁检测 — 三事务环形等待
// ============================================================
TEST(LockManagerTest, DeadlockDetectionThreeTxns) {
    TwoPLManager mgr;
    TransactionManager txn_mgr(&mgr);
    mgr.SetTransactionManager(&txn_mgr);

    Transaction* txnA = txn_mgr.Begin();  // id = 0
    Transaction* txnB = txn_mgr.Begin();  // id = 1
    Transaction* txnC = txn_mgr.Begin();  // id = 2

    // A 持有 table_1, B 持有 table_2, C 持有 table_3
    EXPECT_TRUE(mgr.LockExclusive(txnA, "table_1"));
    EXPECT_TRUE(mgr.LockExclusive(txnB, "table_2"));
    EXPECT_TRUE(mgr.LockExclusive(txnC, "table_3"));

    // A→B (A 等 table_2)
    EXPECT_FALSE(mgr.LockExclusive(txnA, "table_2"));
    // B→C (B 等 table_3)
    EXPECT_FALSE(mgr.LockExclusive(txnB, "table_3"));

    // C→A (C 等 table_1) → 形成环 C→A→B→C，最年轻 C (id=2) 被中止
    EXPECT_FALSE(mgr.LockExclusive(txnC, "table_1"));

    // C 被中止后，B 应能获取 table_3
    EXPECT_TRUE(mgr.LockExclusive(txnB, "table_3"));
}

// ============================================================
// 测试 9: 无死锁时锁获取失败返回 false（不触发 Abort）
// ============================================================
TEST(LockManagerTest, NoDeadlockWhenNoCycle) {
    TwoPLManager mgr;
    TransactionManager txn_mgr(&mgr);
    mgr.SetTransactionManager(&txn_mgr);

    Transaction* txnA = txn_mgr.Begin();  // id = 0
    Transaction* txnB = txn_mgr.Begin();  // id = 1

    // A 持有 table_1 独占锁
    EXPECT_TRUE(mgr.LockExclusive(txnA, "table_1"));

    // B 尝试获取 table_1 → 冲突但无环（A 不在等待任何人）
    EXPECT_FALSE(mgr.LockExclusive(txnB, "table_1"));

    // B 未被中止，仍可提交
    txn_mgr.Commit(txnB);
}

// ============================================================
// 测试 10: LockShared 返回值检查 — 锁冲突时正确返回 false
// ============================================================
TEST(LockManagerTest, LockSharedReturnsFalseOnConflict) {
    TwoPLManager mgr;
    TransactionManager txn_mgr(&mgr);
    mgr.SetTransactionManager(&txn_mgr);

    Transaction* txnA = txn_mgr.Begin();  // id = 0
    Transaction* txnB = txn_mgr.Begin();  // id = 1

    // A 持有独占锁
    EXPECT_TRUE(mgr.LockExclusive(txnA, "table_1"));

    // B 尝试共享锁 → 冲突
    EXPECT_FALSE(mgr.LockShared(txnB, "table_1"));

    txn_mgr.Commit(txnA);
    txn_mgr.Commit(txnB);
}
