#pragma once
#include "concurrency/lock_manager.h"
#include <string>

#include "execution/executor.h"
#include "storage/table_heap.h"
#include "storage/table_iterator.h"
#include "buffer/buffer_pool_manager.h"
#include "concurrency/transaction.h"

namespace db {

// 顺序扫描算子 (SeqScan) — 火山模型迭代器
class SeqScanExecutor : public AbstractExecutor {
public:
    SeqScanExecutor(TableHeap* table_heap, BufferPoolManager* bpm,
                    Transaction* txn, LockManager* lock_mgr = nullptr,
                    const std::string& table_name = "")
        : table_heap_(table_heap), bpm_(bpm), txn_(txn),
          lock_mgr_(lock_mgr), table_name_(table_name) {}

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    TableHeap* table_heap_;
    BufferPoolManager* bpm_;
    Transaction* txn_;
    LockManager* lock_mgr_;
    std::string table_name_;
    TableIterator iterator_{nullptr, RID(), nullptr};  // 当前扫描游标
    bool initialized_{false};
};

} // namespace db
