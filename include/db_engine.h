#pragma once

#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/file_log_manager.h"
#include "storage/table_heap.h"
#include "buffer/buffer_pool_manager.h"
#include "execution/sql_parser.h"
#include "execution/planner.h"
#include "index/b_plus_tree.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace db {

/**
 * @brief DBEngine — 微型数据库引擎入口
 * 整合磁盘管理、缓冲池、SQL 解析、执行计划、索引等模块。
 */
class DBEngine {
public:
    // 崩溃恢复：扫描 WAL 日志，REDO 已提交事务，UNDO 未提交事务
    void DoRecovery();
    DBEngine(const std::string& db_file, size_t buffer_pool_size = 64);
    ~DBEngine();

    // 提供给用户的执行接口
    void ExecuteQuery(const std::string& sql);

    // 获取内部组件（供测试用）
    BufferPoolManager* GetBPM() { return buffer_pool_manager_.get(); }

private:
    // 注册/查找表
    TableHeap* GetOrCreateTable(const std::string& table_name);

    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<LogManager> log_manager_;
    std::unique_ptr<LockManager> lock_manager_;
    std::unique_ptr<TransactionManager> txn_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<SQLParser> parser_;
    std::unique_ptr<Planner> planner_;

    // 简易 catalog：表名 → TableHeap
    std::unordered_map<std::string, std::unique_ptr<TableHeap>> tables_;

    // 索引目录：表名 → B+ 树索引（默认对第一列建索引）
    std::unordered_map<std::string, std::unique_ptr<BPlusTree>> indexes_;

    // 表模式：表名 → 列名列表（CREATE TABLE 时注册）
    std::unordered_map<std::string, std::vector<std::string>> table_schemas_;
};

} // namespace db
