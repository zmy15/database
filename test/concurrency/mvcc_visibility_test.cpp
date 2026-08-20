#include <gtest/gtest.h>
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include "storage/tuple.h"
#include <memory>
#include <vector>

using namespace db;

// ============================================================
// MVCC 可见性专项测试：验证 Tuple::IsVisible 的全部 4 条规则
// 以及 REPEATABLE_READ 快照隔离语义
// ============================================================
class MvccVisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        lock_mgr_ = std::make_unique<TwoPLManager>();
        txn_mgr_ = std::make_unique<TransactionManager>(lock_mgr_.get(), /*log_manager=*/nullptr);
    }

    // 构造带 MVCC 版本字段的测试元组
    static Tuple MakeTuple(txn_id_t xmin, txn_id_t xmax,
                           const std::vector<std::string>& vals = {"a"}) {
        Tuple t(vals);
        t.SetXmin(xmin);
        t.SetXmax(xmax);
        return t;
    }

    std::unique_ptr<TwoPLManager> lock_mgr_;
    std::unique_ptr<TransactionManager> txn_mgr_;
};

// ============================================================
// 规则 1：自己创建的总是可见（即使未提交）
// ============================================================
TEST_F(MvccVisibilityTest, SelfCreatedAlwaysVisible) {
    Tuple t = MakeTuple(/*xmin=*/7, /*xmax=*/0);
    EXPECT_TRUE(Tuple::IsVisible(t, /*reader_txn=*/7, txn_mgr_.get(),
                                 IsolationLevel::READ_COMMITTED));
}

// ============================================================
// 规则 2：xmin == 0 表示系统数据，视为已提交
// ============================================================
TEST_F(MvccVisibilityTest, SystemDataNotDeleted) {
    Tuple t = MakeTuple(/*xmin=*/0, /*xmax=*/0);
    EXPECT_TRUE(Tuple::IsVisible(t, 1, txn_mgr_.get(), IsolationLevel::READ_COMMITTED));
}

TEST_F(MvccVisibilityTest, SystemDataDeletedBySelf) {
    Tuple t = MakeTuple(/*xmin=*/0, /*xmax=*/5);
    EXPECT_TRUE(Tuple::IsVisible(t, /*reader_txn=*/5, txn_mgr_.get(),
                                 IsolationLevel::READ_COMMITTED));
}

TEST_F(MvccVisibilityTest, SystemDataDeletedByAbortedTxnVisible) {
    // 事务 9 提交、事务 10 中止：删除者 10 已回滚 → 系统数据仍可见
    // 注意：必须在 Commit/Abort 之前保存 txn_id，之后对象已被销毁
    Transaction* c = txn_mgr_->Begin();
    txn_id_t cid = c->GetTransactionId();
    txn_mgr_->Commit(c);

    Transaction* a = txn_mgr_->Begin();
    txn_id_t aid = a->GetTransactionId();
    txn_mgr_->Abort(a);

    Tuple t = MakeTuple(/*xmin=*/0, /*xmax=*/aid);
    EXPECT_TRUE(Tuple::IsVisible(t, 1, txn_mgr_.get(), IsolationLevel::READ_COMMITTED));
}

TEST_F(MvccVisibilityTest, SystemDataDeletedByCommittedTxnInvisible) {
    // 删除者已提交 → 系统数据已被删除，不可见
    Transaction* c = txn_mgr_->Begin();
    txn_id_t cid = c->GetTransactionId();
    txn_mgr_->Commit(c);

    Tuple t = MakeTuple(/*xmin=*/0, /*xmax=*/cid);
    EXPECT_FALSE(Tuple::IsVisible(t, 1, txn_mgr_.get(), IsolationLevel::READ_COMMITTED));
}

// ============================================================
// 规则 3：创建者未提交 → 不可见
// ============================================================
TEST_F(MvccVisibilityTest, CreatorUncommittedInvisible) {
    // 事务 2 保持活跃（未提交），其创建的元组对他人不可见
    Transaction* txn = txn_mgr_->Begin();
    txn_id_t txn_id = txn->GetTransactionId();
    Tuple t = MakeTuple(/*xmin=*/txn_id, /*xmax=*/0);
    EXPECT_FALSE(Tuple::IsVisible(t, 1, txn_mgr_.get(), IsolationLevel::READ_COMMITTED));
    txn_mgr_->Abort(txn);
}

TEST_F(MvccVisibilityTest, CreatorCommittedVisible) {
    Transaction* c = txn_mgr_->Begin();
    txn_id_t cid = c->GetTransactionId();
    txn_mgr_->Commit(c);
    Tuple t = MakeTuple(/*xmin=*/cid, /*xmax=*/0);
    EXPECT_TRUE(Tuple::IsVisible(t, 1, txn_mgr_.get(), IsolationLevel::READ_COMMITTED));
}

// ============================================================
// 规则 4：xmax 删除标记
// ============================================================
TEST_F(MvccVisibilityTest, DeletedByAbortedTxnVisible) {
    // 创建者已提交；删除者已回滚 → 删除无效，仍可见
    Transaction* c = txn_mgr_->Begin();
    txn_id_t cid = c->GetTransactionId();
    txn_mgr_->Commit(c);

    Transaction* a = txn_mgr_->Begin();
    txn_id_t aid = a->GetTransactionId();
    txn_mgr_->Abort(a);

    Tuple t = MakeTuple(/*xmin=*/cid, /*xmax=*/aid);
    EXPECT_TRUE(Tuple::IsVisible(t, 1, txn_mgr_.get(), IsolationLevel::READ_COMMITTED));
}

TEST_F(MvccVisibilityTest, DeletedByCommittedTxnInvisible) {
    // 创建者与删除者均已提交 → 已被删除，不可见
    Transaction* c = txn_mgr_->Begin();
    txn_id_t cid = c->GetTransactionId();
    txn_mgr_->Commit(c);

    Transaction* d = txn_mgr_->Begin();
    txn_id_t did = d->GetTransactionId();
    txn_mgr_->Commit(d);

    Tuple t = MakeTuple(/*xmin=*/cid, /*xmax=*/did);
    EXPECT_FALSE(Tuple::IsVisible(t, 1, txn_mgr_.get(), IsolationLevel::READ_COMMITTED));
}

// ============================================================
// REPEATABLE_READ 快照隔离语义
// ============================================================
TEST_F(MvccVisibilityTest, RRUsesSnapshotNotLiveState) {
    // 读者 T1 (REPEATABLE_READ) 在 T2 提交【之前】BEGIN：
    // T1 的快照中不含 T2 → 即使 T2 之后提交，T1 也看不到 T2 创建的元组
    Transaction* t1 = txn_mgr_->Begin(IsolationLevel::REPEATABLE_READ);
    EXPECT_TRUE(t1->HasSnapshot());
    txn_id_t t1_id = t1->GetTransactionId();

    // T2 在 T1 BEGIN 之后才提交
    Transaction* t2 = txn_mgr_->Begin();
    txn_id_t t2_id = t2->GetTransactionId();
    txn_mgr_->Commit(t2);

    Tuple t = MakeTuple(/*xmin=*/t2_id, /*xmax=*/0);

    // REPEATABLE_READ：基于快照（无 T2）→ 不可见
    EXPECT_FALSE(Tuple::IsVisible(t, t1_id, txn_mgr_.get(),
                                  IsolationLevel::REPEATABLE_READ));

    // READ_COMMITTED：基于实时状态（T2 已提交）→ 可见
    EXPECT_TRUE(Tuple::IsVisible(t, t1_id, txn_mgr_.get(),
                                 IsolationLevel::READ_COMMITTED));

    txn_mgr_->Abort(t1);
}

TEST_F(MvccVisibilityTest, RRSnapshotIncludesPriorCommitted) {
    // T2 先提交，T1 (REPEATABLE_READ) 后 BEGIN：快照含 T2 → T1 可见 T2 的元组
    Transaction* t2 = txn_mgr_->Begin();
    txn_id_t t2_id = t2->GetTransactionId();
    txn_mgr_->Commit(t2);

    Transaction* t1 = txn_mgr_->Begin(IsolationLevel::REPEATABLE_READ);
    txn_id_t t1_id = t1->GetTransactionId();
    Tuple t = MakeTuple(/*xmin=*/t2_id, /*xmax=*/0);
    EXPECT_TRUE(Tuple::IsVisible(t, t1_id, txn_mgr_.get(),
                                 IsolationLevel::REPEATABLE_READ));
    txn_mgr_->Abort(t1);
}

TEST_F(MvccVisibilityTest, RRDeleterAbortedInSnapshotVisible) {
    // T2 中止 → T1 (REPEATABLE_READ) BEGIN（快照已中止集合含 T2）
    // T3 先提交创建元组 → T1 看到"删除者已回滚"的元组
    Transaction* t2 = txn_mgr_->Begin();
    txn_id_t t2_id = t2->GetTransactionId();
    txn_mgr_->Abort(t2);

    Transaction* t3 = txn_mgr_->Begin();
    txn_id_t t3_id = t3->GetTransactionId();
    txn_mgr_->Commit(t3);

    Transaction* t1 = txn_mgr_->Begin(IsolationLevel::REPEATABLE_READ);
    txn_id_t t1_id = t1->GetTransactionId();
    Tuple t = MakeTuple(/*xmin=*/t3_id, /*xmax=*/t2_id);
    EXPECT_TRUE(Tuple::IsVisible(t, t1_id, txn_mgr_.get(),
                                 IsolationLevel::REPEATABLE_READ));
    txn_mgr_->Abort(t1);
}

TEST_F(MvccVisibilityTest, RRDeleterCommittedInSnapshotInvisible) {
    // T3 提交创建元组 → T2 提交（删除者）→ T1 (REPEATABLE_READ) BEGIN（快照含 T2、T3）
    // T1 认为该元组已被删除
    Transaction* t3 = txn_mgr_->Begin();
    txn_id_t t3_id = t3->GetTransactionId();
    txn_mgr_->Commit(t3);

    Transaction* t2 = txn_mgr_->Begin();
    txn_id_t t2_id = t2->GetTransactionId();
    txn_mgr_->Commit(t2);

    Transaction* t1 = txn_mgr_->Begin(IsolationLevel::REPEATABLE_READ);
    txn_id_t t1_id = t1->GetTransactionId();
    Tuple t = MakeTuple(/*xmin=*/t3_id, /*xmax=*/t2_id);
    EXPECT_FALSE(Tuple::IsVisible(t, t1_id, txn_mgr_.get(),
                                  IsolationLevel::REPEATABLE_READ));
    txn_mgr_->Abort(t1);
}
