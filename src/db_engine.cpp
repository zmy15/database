#include "db_engine.h"

namespace db {

DBEngine::DBEngine() {
    // 初始化所有核心组件
    disk_manager_ = std::make_unique<MockDiskManager>(); // 需自己实现
    log_manager_ = std::make_unique<MockLogManager>();   // 需自己实现
    buffer_pool_manager_ = std::make_unique<MockBPM>();  // 需自己实现 (集成LRU)
    lock_manager_ = std::make_unique<MockLockManager>(); // 需自己实现
    txn_manager_ = std::make_unique<TransactionManager>();
    parser_ = std::make_unique<SQLParser>();
}

void DBEngine::ExecuteQuery(const std::string& sql) {
    // 1. 开启事务
    Transaction* txn = txn_manager_->Begin();

    // 2. 解析 SQL，生成执行计划
    auto executor = parser_->ParseAndPlan(sql, buffer_pool_manager_.get(), txn);

    //if (executor) {
    //    // 3. 火山模型执行：Init -> 循环 Next
    //    executor->Init();
    //    Tuple result;
    //    while (executor->Next(&result)) {
    //        // 打印或返回结果集
    //        for (const auto& val : result.values) {
    //            std::cout << val << " ";
    //        }
    //        std::cout << "\n";
    //    }
    //}

    // 4. 提交事务
    txn_manager_->Commit(txn);
}

// TransactionManager dummy implementation
Transaction* TransactionManager::Begin(IsolationLevel iso_level) { return new Transaction(0, iso_level); }
void TransactionManager::Commit(Transaction* txn) { delete txn; }
void TransactionManager::Abort(Transaction* txn) { delete txn; }

} // namespace db
