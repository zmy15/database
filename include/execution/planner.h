#pragma once

#include "execution/executor.h"
#include "execution/sql_parser.h"
#include "execution/expression.h"
#include "index/b_plus_tree.h"
#include "storage/table_heap.h"
#include "buffer/buffer_pool_manager.h"
#include "concurrency/transaction.h"
#include "concurrency/lock_manager.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace db {

/**
 * @brief 执行计划构建器 — 根据 SQL 语句和可用索引选择最优执行计划
 *
 * 当前规则：
 * - SELECT * FROM t WHERE col = val：
 *     若 col 上有索引 → IndexScanExecutor（点查）
 *     否则 → SeqScanExecutor + FilterExecutor
 * - SELECT cols FROM t WHERE cond：在上述基础上包装 ProjectionExecutor
 * - SELECT * FROM t（无条件）：SeqScanExecutor
 * - DELETE / UPDATE：由 DBEngine 直接处理（不走 Planner）
 */
class Planner {
public:
    Planner(const std::unordered_map<std::string, std::vector<std::string>>& table_schemas,
            const std::unordered_map<std::string, std::unique_ptr<BPlusTree>>& indexes,
            const std::unordered_map<std::string, std::unique_ptr<TableHeap>>& tables,
            BufferPoolManager* bpm,
            Transaction* txn = nullptr,
           LockManager* lock_mgr = nullptr,
           TransactionManager* txn_mgr = nullptr)
        : table_schemas_(table_schemas),
          indexes_(indexes),
          tables_(tables),
          bpm_(bpm),
          txn_(txn),
         lock_mgr_(lock_mgr),
         txn_mgr_(txn_mgr) {}

    /**
     * @brief 为 SELECT 语句创建执行器树
     * @param stmt  SELECT 语句
     * @return 执行器树的根节点，失败返回 nullptr
     */
    std::unique_ptr<AbstractExecutor> CreatePlan(const SelectStmt* stmt);

private:
    const std::unordered_map<std::string, std::vector<std::string>>& table_schemas_;
    const std::unordered_map<std::string, std::unique_ptr<BPlusTree>>& indexes_;
    const std::unordered_map<std::string, std::unique_ptr<TableHeap>>& tables_;
    BufferPoolManager* bpm_;
    Transaction* txn_;
    LockManager* lock_mgr_;
   TransactionManager* txn_mgr_{nullptr};
};

} // namespace db
