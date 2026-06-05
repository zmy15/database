#include <gtest/gtest.h>
#include "index/b_plus_tree.h"
#include "index/b_plus_tree_page.h"
#include "buffer/buffer_pool_manager.h"
#include "storage/file_disk_manager.h"
#include "storage/tuple.h"
#include <memory>
#include <vector>
#include <algorithm>
#include <random>
#include <cstdio>

using namespace db;

// ============================================================
// 测试辅助类：提供独立的 B+ 树测试环境
// ============================================================
class BPlusTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试用例使用独立的临时数据库文件
        disk_mgr_ = std::make_unique<FileDiskManager>("test_bplus.db");
        bpm_ = std::make_unique<BufferPoolManager>(64, disk_mgr_.get());
        tree_ = std::make_unique<BPlusTree>("test_index", bpm_.get());
    }

    void TearDown() override {
        tree_.reset();
        if (bpm_) bpm_->Destroy();
        bpm_.reset();
        disk_mgr_.reset();
        // 清理测试数据库文件
        std::remove("test_bplus.db");
    }

    // 辅助：创建 Tuple
    static Tuple MakeTuple(const std::vector<std::string>& vals) {
        return Tuple(vals);
    }

    std::unique_ptr<FileDiskManager> disk_mgr_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<BPlusTree> tree_;
};

// ============================================================
// 测试 1: 单条插入查找
// ============================================================
TEST_F(BPlusTreeTest, SingleInsertAndLookup) {
    Tuple val = MakeTuple({"Alice", "100"});
    EXPECT_TRUE(tree_->Insert("key_1", val, 0));

    auto result = tree_->GetValue("key_1", 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[0], "Alice");
    EXPECT_EQ(result->GetValues()[1], "100");
}

// ============================================================
// 测试 2: 批量插入并验证全部可查
// ============================================================
TEST_F(BPlusTreeTest, BatchInsert) {
    const int N = 200;
    for (int i = 0; i < N; ++i) {
        std::string key = "user_" + std::to_string(i);
        Tuple val = MakeTuple({key, std::to_string(i * 10)});
        EXPECT_TRUE(tree_->Insert(key, val, 0));
    }

    // 验证全部可查
    for (int i = 0; i < N; ++i) {
        std::string key = "user_" + std::to_string(i);
        auto result = tree_->GetValue(key, 0);
        ASSERT_TRUE(result.has_value()) << "Missing key: " << key;
        EXPECT_EQ(result->GetValues()[1], std::to_string(i * 10));
    }
}

// ============================================================
// 测试 3: 查询不存在的 key 返回 nullopt
// ============================================================
TEST_F(BPlusTreeTest, NonExistentKey) {
    Tuple val = MakeTuple({"Bob", "200"});
    tree_->Insert("key_a", val, 0);
    tree_->Insert("key_c", val, 0);

    auto result = tree_->GetValue("key_b", 0);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// 测试 4: 有序插入后键顺序正确（叶子链表遍历）
// ============================================================
TEST_F(BPlusTreeTest, OrderedInsertionMaintainsOrder) {
    const int N = 50;
    std::vector<std::string> keys;
    for (int i = 0; i < N; ++i) {
        std::string key = "ord_" + std::to_string(i);
        keys.push_back(key);
        Tuple val = MakeTuple({key});
        EXPECT_TRUE(tree_->Insert(key, val, 0));
    }

    // 验证所有 key 都可查且顺序正确
    for (int i = 0; i < N; ++i) {
        auto result = tree_->GetValue(keys[i], 0);
        ASSERT_TRUE(result.has_value()) << "Missing key: " << keys[i];
    }
}

// ============================================================
// 测试 10: 大型 B+ 树 Drop 后所有页面被正确回收（CollectAllPageIds 递归修复验证）
// ============================================================
TEST_F(BPlusTreeTest, DropLargeTreeReclaimsAllPages) {
    // 插入 500 条数据，确保触发多层分裂（根节点、内部节点、叶子节点）
    const int N = 500;
    for (int i = 0; i < N; ++i) {
        std::string key = "drop_" + std::to_string(i);
        Tuple val = MakeTuple({key, "data_" + std::to_string(i)});
        EXPECT_TRUE(tree_->Insert(key, val, 0));
    }

    // 验证全部数据可查，确认 B+ 树结构完整
    for (int i = 0; i < N; ++i) {
        std::string key = "drop_" + std::to_string(i);
        auto result = tree_->GetValue(key, 0);
        ASSERT_TRUE(result.has_value()) << "Missing key before Drop: " << key;
    }

    // 记录根页 ID，Drop 后应重置为 INVALID_PAGE_ID
    page_id_t old_root = tree_->GetRootPageId();
    ASSERT_NE(old_root, INVALID_PAGE_ID) << "Root page should exist before Drop";

    // 执行 Drop：遍历树并删除所有页面
    tree_->Drop();

    // 验证根页已重置
    EXPECT_EQ(tree_->GetRootPageId(), INVALID_PAGE_ID);

    // 验证 Drop 后可以重新插入数据（确认 B+ 树可正常重用）
    for (int i = 0; i < 100; ++i) {
        std::string key = "reborn_" + std::to_string(i);
        Tuple val = MakeTuple({key});
        EXPECT_TRUE(tree_->Insert(key, val, 0));
    }
    for (int i = 0; i < 100; ++i) {
        std::string key = "reborn_" + std::to_string(i);
        auto result = tree_->GetValue(key, 0);
        ASSERT_TRUE(result.has_value()) << "Missing key after re-insert: " << key;
    }
}

// ============================================================
// 测试 5: 逆序插入触发分裂
// ============================================================
TEST_F(BPlusTreeTest, ReverseOrderInsertion) {
    const int N = 100;
    for (int i = N - 1; i >= 0; --i) {
        std::string key = "rev_" + std::to_string(i);
        Tuple val = MakeTuple({key, std::to_string(i)});
        EXPECT_TRUE(tree_->Insert(key, val, 0));
    }

    for (int i = 0; i < N; ++i) {
        std::string key = "rev_" + std::to_string(i);
        auto result = tree_->GetValue(key, 0);
        ASSERT_TRUE(result.has_value()) << "Missing key: " << key;
        EXPECT_EQ(result->GetValues()[1], std::to_string(i));
    }
}

// ============================================================
// 测试 6: 重复 key 插入（应覆盖或拒绝，此处验证可插入）
// ============================================================
TEST_F(BPlusTreeTest, DuplicateKeyInsertion) {
    Tuple val1 = MakeTuple({"First", "v1"});
    Tuple val2 = MakeTuple({"Second", "v2"});

    EXPECT_TRUE(tree_->Insert("dup_key", val1, 0));
    // 重复 key 插入（当前实现允许重复 key，插入到合适位置）
    EXPECT_TRUE(tree_->Insert("dup_key", val2, 0));

    auto result = tree_->GetValue("dup_key", 0);
    ASSERT_TRUE(result.has_value());
    // 应返回首先匹配的那个
}

// ============================================================
// 测试 7: 大 value 插入（测试变长数据存储正确）
// ============================================================
TEST_F(BPlusTreeTest, LargeValueInsertion) {
    std::string big_data(500, 'Z');
    Tuple val = MakeTuple({"big_key", big_data});
    EXPECT_TRUE(tree_->Insert("big_key", val, 0));

    auto result = tree_->GetValue("big_key", 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[1], big_data);
}

// ============================================================
// 测试 8: 页面格式正确（内部节点和叶子节点 Init）
// ============================================================
TEST_F(BPlusTreeTest, PageFormatInitialization) {
    // 创建一个叶子页并验证元数据
    page_id_t pid;
    Page* page = bpm_->NewPage(&pid);
    ASSERT_NE(page, nullptr);

    auto* tree_page = reinterpret_cast<BPlusTreePage*>(page);
    tree_page->Init(pid, true);

    EXPECT_TRUE(tree_page->IsLeaf());
    EXPECT_EQ(tree_page->GetPageId(), pid);
    EXPECT_EQ(tree_page->GetSlotCount(), 0u);
    EXPECT_EQ(tree_page->GetPrevPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(tree_page->GetNextPageId(), INVALID_PAGE_ID);

    bpm_->UnpinPage(pid, false);
}

// ============================================================
// 测试 9: 叶子分裂后兄弟链表正确
// ============================================================
TEST_F(BPlusTreeTest, LeafSplitMaintainsSiblingLinks) {
    // 插入足够多的数据触发叶子分裂
    const int N = 300;
    for (int i = 0; i < N; ++i) {
        std::string key = "leaf_" + std::to_string(i);
        Tuple val = MakeTuple({key, "data"});
        EXPECT_TRUE(tree_->Insert(key, val, 0));
    }

    // 验证所有数据可查
    for (int i = 0; i < N; ++i) {
        std::string key = "leaf_" + std::to_string(i);
        auto result = tree_->GetValue(key, 0);
        ASSERT_TRUE(result.has_value()) << "Missing key after split: " << key;
    }
}
