#pragma once

#include "execution/executor.h"
#include "index/b_plus_tree.h"
#include "storage/table_heap.h"
#include "buffer/buffer_pool_manager.h"
#include "concurrency/transaction.h"
#include "concurrency/lock_manager.h"
#include <string>
#include <optional>

namespace db {

/**
 * @brief 索引扫描执行器 — 利用 B+树进行等值点查
 *
 * 当前版本仅支持等值点查（通过 BPlusTree::GetValue 直接返回 Tuple）。
 * 范围扫描将在 B+树支持 ScanKey 后扩展。
 * 非等值条件或无法索引化时，由优化器（Planner）回退到全表扫描 + Filter。
 */
class IndexScanExecutor : public AbstractExecutor {
public:
    IndexScanExecutor(BPlusTree* bptree, const std::string& key,
                      TableHeap* table_heap, BufferPoolManager* bpm,
                      Transaction* txn = nullptr,
                      LockManager* lock_mgr = nullptr,
                      const std::string& table_name = "")
        : bptree_(bptree), key_(key),
          table_heap_(table_heap), bpm_(bpm),
          txn_(txn), lock_mgr_(lock_mgr), table_name_(table_name) {}

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    BPlusTree* bptree_;
    std::string key_;
    TableHeap* table_heap_;       // 保留用于未来扩展
    BufferPoolManager* bpm_;      // 保留用于未来扩展
    Transaction* txn_;             // 当前事务（用于锁获取与 txn_id 传递）
    LockManager* lock_mgr_;        // 锁管理器（用于 Init 时获取共享锁）
    std::string table_name_;       // 表名（锁粒度标识）

    std::optional<Tuple> result_tuple_;  // 点查结果缓存
    bool returned_{false};               // 是否已返回结果
};

} // namespace db
