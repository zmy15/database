#include "execution/seq_scan_executor.h"

namespace db {

void SeqScanExecutor::Init() {
    // 定位到表的第一条有效记录
    if (table_heap_ != nullptr) {
        // 对全表获取共享锁（读锁）
        if (lock_mgr_ && txn_ && !table_name_.empty()) {
            lock_mgr_->LockShared(txn_, table_name_);
        }
        iterator_ = table_heap_->Begin();
    }
    initialized_ = true;
}

bool SeqScanExecutor::Next(Tuple* tuple) {
    if (!initialized_ || table_heap_ == nullptr) {
        return false;
    }

    // 判断是否已到表尾
    if (iterator_ == table_heap_->End()) {
        return false;
    }

    // 读取当前 Tuple
    auto opt = iterator_.Get();
    if (!opt.has_value()) {
        return false;
    }

    *tuple = std::move(opt.value());

    // 步进到下一条
    ++iterator_;

    return true;
}

} // namespace db
