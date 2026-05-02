#pragma once

#include "execution/executor.h"
#include "execution/seq_scan_executor.h"
#include "buffer/buffer_pool_manager.h"
#include "concurrency/transaction.h"
#include <string>
#include <memory>

namespace db {

// 模拟的 SQL 解析器 (将字符串转为执行计划树)
class SQLParser {
public:
    // 简单模拟：返回树的根执行器
    std::unique_ptr<AbstractExecutor> ParseAndPlan(const std::string& sql, BufferPoolManager* bpm, Transaction* txn) {
        // TODO: 词法分析、语法分析、生成AST、优化器、生成物理执行计划
        // 这里只是硬编码演示返回一个全表扫描算子
        if (sql.find("SELECT") != std::string::npos) {
            return std::make_unique<SeqScanExecutor>(bpm, "my_table", txn);
        }
        return nullptr;
    }
};

} // namespace db
