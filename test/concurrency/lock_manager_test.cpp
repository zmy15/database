#include <gtest/gtest.h>
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"

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
