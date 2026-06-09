#pragma once

#include "common/config.h"
#include "buffer/buffer_pool_manager.h"
#include "storage/tuple.h"
#include "concurrency/transaction.h"
#include <string>
#include <stack>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace db {

// 前向声明
class TransactionManager;
class BPlusTreePage;

/**
 * @brief B+ 树索引结构
 *
 * 内部节点约定：
 *   - prev_page_id 存储 child[0]（第一个子节点，覆盖 < key[0] 的范围）
 *   - slot[i] 存储 (key[i], child[i+1])，child[i+1] 覆盖 >= key[i] 的范围
 *   - N 个 key 对应 N+1 个子节点
 */
class BPlusTree {
public:
    BPlusTree(std::string index_name, BufferPoolManager* bpm)
        : name_(std::move(index_name)), bpm_(bpm) {}

    /** 插入键值对 */
    bool Insert(const std::string& key, const Tuple& value, txn_id_t txn_id,
                TransactionManager* txn_mgr = nullptr);

    /** 点查 */
    std::optional<Tuple> GetValue(const std::string& key, txn_id_t txn_id,
                                   TransactionManager* txn_mgr = nullptr,
                                   IsolationLevel iso_level = IsolationLevel::READ_COMMITTED);

    /** 删除指定键（标记删除，不回收空间） */
   bool Remove(const std::string& key, txn_id_t txn_id = 0,
                TransactionManager* txn_mgr = nullptr,
                IsolationLevel iso_level = IsolationLevel::READ_COMMITTED);

    /** 范围扫描：返回 [start_key, end_key] 范围内的所有 Tuple */
    std::vector<Tuple> ScanRange(const std::string& start_key,
                                  const std::string& end_key,
                                  txn_id_t txn_id,
                                  TransactionManager* txn_mgr = nullptr,
                                  IsolationLevel iso_level = IsolationLevel::READ_COMMITTED);

    /** 获取根页 ID（供测试用） */
    page_id_t GetRootPageId() const { return root_page_id_; }

    /** 删除整棵 B+ 树：遍历所有页面，回收空间 */
    void Drop();

private:
    bool InsertIntoEmptyTree(const std::string& key, const Tuple& value);
    bool InsertIntoLeaf(page_id_t leaf_id, BPlusTreePage* leaf_node,
                        const std::string& key, const Tuple& value,
                        std::stack<std::pair<page_id_t, BPlusTreePage*>>& path);
    bool InsertIntoParent(page_id_t old_page_id, const std::string& split_key, page_id_t new_page_id);
    void UpdateParentPointers(page_id_t page_id, BPlusTreePage* node);

    // 从指定页开始递归收集所有后代页 ID（用于清理）
    void CollectAllPageIds(page_id_t page_id, std::vector<page_id_t>& ids);

    std::string name_;
    page_id_t root_page_id_ = INVALID_PAGE_ID;
    BufferPoolManager* bpm_;
    std::shared_mutex root_latch_;
};

} // namespace db
