#pragma once

#include "storage/disk_manager.h"
#include "storage/table_heap.h"
#include "buffer/buffer_pool_manager.h"
#include "execution/sql_parser.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace db {

/**
 * @brief DBEngine — 微型数据库引擎入口
 * 整合磁盘管理、缓冲池、SQL 解析、执行计划等模块。
 */
class DBEngine {
public:
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
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<SQLParser> parser_;

    // 简易 catalog：表名 → TableHeap
    std::unordered_map<std::string, std::unique_ptr<TableHeap>> tables_;
};

} // namespace db
