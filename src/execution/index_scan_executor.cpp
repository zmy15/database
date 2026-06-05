 #include "execution/index_scan_executor.h"
 
 namespace db {
 
 void IndexScanExecutor::Init() {
     // 重置状态，执行 B+ 树点查并缓存结果
     // 对索引/表获取共享锁（读锁）
     if (lock_mgr_ && txn_ && !table_name_.empty()) {
         lock_mgr_->LockShared(txn_, table_name_);
     }
     returned_ = false;
     result_tuple_ = std::nullopt;
 
     if (bptree_ != nullptr) {
         txn_id_t tid = (txn_ != nullptr) ? txn_->GetTransactionId() : 0;
         result_tuple_ = bptree_->GetValue(key_, tid);
     }
 }
 
 bool IndexScanExecutor::Next(Tuple* tuple) {
     // 点查最多返回一行，首次调用已缓存结果则返回，否则返回 false
     if (returned_ || !result_tuple_.has_value()) {
         return false;
     }
 
     *tuple = std::move(result_tuple_.value());
     result_tuple_ = std::nullopt;
     returned_ = true;
     return true;
 }
 
 } // namespace db
