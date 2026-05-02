#pragma once

#include "execution/executor.h"
#include "buffer/buffer_pool_manager.h"
#include "concurrency/transaction.h"
#include <string>

namespace db {

// 顺序扫描算子 (SeqScan)
class SeqScanExecutor : public AbstractExecutor {
public:
    SeqScanExecutor(BufferPoolManager* bpm, std::string table_name, Transaction* txn)
        : bpm_(bpm), table_name_(std::move(table_name)), txn_(txn) {}

    void Init() override { /* 定位到表的第一页的第一条记录 */ }
    bool Next(Tuple* tuple) override {
        // 1. 通过 bpm_->FetchPage() 获取当前页
        // 2. 读取下一条 Tuple
        // 3. 遇到页尾则请求下一页
        // 4. 将读到的数据赋给 tuple，返回 true；若扫描结束返回 false
        return false;
    }
private:
    BufferPoolManager* bpm_;
    std::string table_name_;
    Transaction* txn_;
};

} // namespace db
