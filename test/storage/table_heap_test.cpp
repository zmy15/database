#include <gtest/gtest.h>
#include "storage/table_heap.h"
#include "storage/table_page.h"
#include "storage/table_iterator.h"
#include "storage/tuple.h"
#include "storage/file_disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include <memory>

using namespace db;

class TableHeapTest : public ::testing::Test {
protected:
    void SetUp() override {
        disk_mgr_ = std::make_unique<FileDiskManager>("test_tableheap.db");
        bpm_ = std::make_unique<BufferPoolManager>(32, disk_mgr_.get());
        heap_ = std::make_unique<TableHeap>(bpm_.get());
        heap_->Init();
    }

    void TearDown() override {
        heap_.reset();
        if (bpm_) {
            bpm_->Destroy();
        }
        bpm_.reset();
        disk_mgr_.reset();
        std::remove("test_tableheap.db");
    }

    static Tuple MakeTuple(const std::vector<std::string>& vals) {
        return Tuple(vals);
    }

    std::unique_ptr<FileDiskManager> disk_mgr_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<TableHeap> heap_;
};

// 测试：插入并读取单个元组
TEST_F(TableHeapTest, InsertAndGetSingleTuple) {
    Tuple t = MakeTuple({"Alice", "30"});
    RID rid;
    EXPECT_TRUE(heap_->InsertTuple(t, &rid));

    auto result = heap_->GetTuple(rid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[0], "Alice");
    EXPECT_EQ(result->GetValues()[1], "30");
}

// 测试：插入多个元组并逐一读取
TEST_F(TableHeapTest, InsertMultipleTuples) {
    const int N = 20;
    std::vector<RID> rids;
    for (int i = 0; i < N; ++i) {
        Tuple t = MakeTuple({"Name_" + std::to_string(i), std::to_string(i * 10)});
        RID rid;
        EXPECT_TRUE(heap_->InsertTuple(t, &rid));
        rids.push_back(rid);
    }
    for (int i = 0; i < N; ++i) {
        auto result = heap_->GetTuple(rids[i]);
        ASSERT_TRUE(result.has_value()) << "Missing tuple at index " << i;
        EXPECT_EQ(result->GetValues()[0], "Name_" + std::to_string(i));
        EXPECT_EQ(result->GetValues()[1], std::to_string(i * 10));
    }
}

// 测试：删除元组
TEST_F(TableHeapTest, DeleteTuple) {
    Tuple t = MakeTuple({"Bob", "25"});
    RID rid;
    heap_->InsertTuple(t, &rid);
    EXPECT_TRUE(heap_->GetTuple(rid).has_value());
    EXPECT_TRUE(heap_->DeleteTuple(rid));
    EXPECT_FALSE(heap_->GetTuple(rid).has_value());
}

// 测试：删除不存在的元组返回 false
TEST_F(TableHeapTest, DeleteNonExistentTuple) {
    RID fake_rid(9999, 0);
    EXPECT_FALSE(heap_->DeleteTuple(fake_rid));
}

// 测试：原地更新元组
TEST_F(TableHeapTest, UpdateTupleInPlace) {
    Tuple t1 = MakeTuple({"OldName", "OldVal"});
    RID rid;
    heap_->InsertTuple(t1, &rid);
    Tuple t2 = MakeTuple({"NewName", "NewVal"});
    EXPECT_TRUE(heap_->UpdateTuple(rid, t2));
    auto result = heap_->GetTuple(rid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[0], "NewName");
    EXPECT_EQ(result->GetValues()[1], "NewVal");
}

// 测试：更新不存在的元组返回 false
TEST_F(TableHeapTest, UpdateNonExistentTuple) {
    Tuple t = MakeTuple({"Does", "NotExist"});
    RID fake_rid(9999, 0);
    EXPECT_FALSE(heap_->UpdateTuple(fake_rid, t));
}

// 测试：迭代器遍历所有元组
TEST_F(TableHeapTest, IteratorTraversal) {
    const int N = 15;
    for (int i = 0; i < N; ++i) {
        Tuple t = MakeTuple({"Iter_" + std::to_string(i), std::to_string(i)});
        RID rid;
        heap_->InsertTuple(t, &rid);
    }

    int count = 0;
    for (auto it = heap_->Begin(); it != heap_->End(); ++it) {
        auto tuple_opt = it.Get();
        ASSERT_TRUE(tuple_opt.has_value());
        count++;
    }
    EXPECT_EQ(count, N);
}

// 测试：迭代器在删除后跳过已删除元组
TEST_F(TableHeapTest, IteratorSkipsDeletedTuples) {
    Tuple t1 = MakeTuple({"Keep", "1"});
    Tuple t2 = MakeTuple({"Delete", "2"});
    Tuple t3 = MakeTuple({"Keep2", "3"});

    RID rid1, rid2, rid3;
    heap_->InsertTuple(t1, &rid1);
    heap_->InsertTuple(t2, &rid2);
    heap_->InsertTuple(t3, &rid3);

    // 删除中间那个
    heap_->DeleteTuple(rid2);

    int count = 0;
    for (auto it = heap_->Begin(); it != heap_->End(); ++it) {
        auto tuple_opt = it.Get();
        ASSERT_TRUE(tuple_opt.has_value());
        std::string first_val = tuple_opt->GetValues()[0];
        EXPECT_NE(first_val, "Delete") << "Deleted tuple should not appear";
        count++;
    }
    EXPECT_EQ(count, 2);
}
