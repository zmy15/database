#include "db_engine.h"
#include "storage/file_disk_manager.h"
#include "storage/file_log_manager.h"
#include "storage/table_iterator.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include <iostream>

namespace db {

DBEngine::DBEngine(const std::string& db_file, size_t buffer_pool_size) {
    // 1. 创建磁盘管理器（基于本地文件）
    disk_manager_ = std::make_unique<FileDiskManager>(db_file);

    // 2. 创建 WAL 日志管理器
    log_manager_ = std::make_unique<FileLogManager>(db_file + ".wal");

    // 3. 创建缓冲池（传入 log_manager 用于 WAL 先行刷盘）
    buffer_pool_manager_ = std::make_unique<BufferPoolManager>(
        buffer_pool_size, disk_manager_.get(), log_manager_.get());

    // 4. 创建锁管理器与事务管理器
    lock_manager_ = std::make_unique<TwoPLManager>();
    txn_manager_ = std::make_unique<TransactionManager>(lock_manager_.get(), log_manager_.get());

    // 5. 创建 SQL 解析器
    parser_ = std::make_unique<SQLParser>();

    // 6. 创建执行计划构建器（持有对 schemas / indexes / tables / bpm 的引用）
    planner_ = std::make_unique<Planner>(
        table_schemas_, indexes_, tables_,
        buffer_pool_manager_.get(),
        /*txn=*/nullptr,
        lock_manager_.get());

    // 7. 崩溃恢复：REDO 已提交事务
    DoRecovery();
}

DBEngine::~DBEngine() {
    // 析构顺序：先清理 table_heap（会 unpin 页面），再清理 BPM，最后清理磁盘
    tables_.clear();
    indexes_.clear();
    if (log_manager_) {
        log_manager_->FlushLogs();
    }
    if (buffer_pool_manager_) {
        buffer_pool_manager_->Destroy();
    }
}

TableHeap* DBEngine::GetOrCreateTable(const std::string& table_name) {
    auto it = tables_.find(table_name);
    if (it != tables_.end()) {
        return it->second.get();
    }
    // 创建新表
    auto heap = std::make_unique<TableHeap>(buffer_pool_manager_.get());
    heap->Init();
    TableHeap* raw = heap.get();
    tables_[table_name] = std::move(heap);
    return raw;
}

void DBEngine::DoRecovery() {
    // 1. 读取 WAL 中所有日志记录
    auto records = log_manager_->ReadLogRecords();
    if (records.empty()) {
        return; // WAL 为空，无需恢复
    }

    // 2. 构建事务状态表：每个 txn_id 的最终状态
    //    committed_txns: 已提交的事务集合
    //    aborted_txns: 已中止的事务集合
    std::unordered_set<txn_id_t> committed_txns;
    std::unordered_set<txn_id_t> aborted_txns;

    for (const auto& rec : records) {
        if (rec.op_type == LogOpType::COMMIT_TXN) {
            committed_txns.insert(rec.txn_id);
            aborted_txns.erase(rec.txn_id);
        } else if (rec.op_type == LogOpType::ABORT_TXN) {
            aborted_txns.insert(rec.txn_id);
            committed_txns.erase(rec.txn_id);
        }
    }

    // 3. REDO：按 LSN 顺序重放已提交事务的操作
    for (const auto& rec : records) {
        if (committed_txns.count(rec.txn_id) == 0) {
            continue; // 跳过未提交/已中止的事务
        }

        // 只重放数据操作（INSERT/DELETE/UPDATE），跳过控制记录
        if (rec.op_type != LogOpType::INSERT &&
            rec.op_type != LogOpType::DELETE &&
            rec.op_type != LogOpType::UPDATE) {
            continue;
        }

        // 获取目标页面
        Page* page = buffer_pool_manager_->FetchPage(rec.page_id);
        if (!page) continue;

        auto* tp = reinterpret_cast<TablePage*>(page);

        switch (rec.op_type) {
        case LogOpType::INSERT:
            // REDO INSERT：写入 new_tuple
            if (rec.new_tuple.GetSize() > 0) {
                tp->InsertTuple(rec.new_tuple, nullptr);
            }
            break;
        case LogOpType::DELETE:
            // REDO DELETE：标记删除
            tp->MarkDelete(rec.slot_num);
            break;
        case LogOpType::UPDATE:
            // REDO UPDATE：写入 new_tuple
            if (rec.new_tuple.GetSize() > 0) {
                tp->UpdateTuple(rec.slot_num, rec.new_tuple);
            }
            break;
        default:
            break;
        }

        buffer_pool_manager_->UnpinPage(rec.page_id, true);
    }

    // 4. 将恢复后的所有脏页刷盘
    buffer_pool_manager_->FlushAllPages();

    // 5. 截断 WAL（恢复完成后日志可丢弃）
    if (!records.empty()) {
        lsn_t last_lsn = records.back().lsn;
        log_manager_->TruncateAfter(last_lsn);
    }

    std::cout << "[Recovery] REDO complete: " << committed_txns.size()
              << " committed transactions recovered." << std::endl;
}

void DBEngine::ExecuteQuery(const std::string& sql) {
    // 1. 解析 SQL
    auto stmt = parser_->Parse(sql);
    if (!stmt) {
        std::cerr << "[DBEngine] Failed to parse SQL: " << sql << std::endl;
        return;
    }

    // 2. 根据语句类型执行
    switch (stmt->type) {
    case SQLStmtType::CREATE_TABLE: {
        auto& ct = static_cast<CreateTableStmt&>(*stmt);
        GetOrCreateTable(ct.table_name);
        table_schemas_[ct.table_name] = ct.columns;
        std::cout << "[OK] Table '" << ct.table_name << "' created." << std::endl;
        break;
    }
    case SQLStmtType::INSERT: {
        auto& ins = static_cast<InsertStmt&>(*stmt);
        TableHeap* heap = GetOrCreateTable(ins.table_name);
        RID rid;
        if (heap->InsertTuple(ins.tuple, &rid)) {
            // 索引维护：为新插入的元组更新索引
            auto idx_it = indexes_.find(ins.table_name);
            if (idx_it != indexes_.end()) {
                const auto& vals = ins.tuple.GetValues();
                if (!vals.empty()) {
                    idx_it->second->Insert(vals[0], ins.tuple, 0);
                }
            }
            std::cout << "[OK] Inserted 1 row into '" << ins.table_name << "'." << std::endl;
        } else {
            std::cerr << "[ERR] Insert failed." << std::endl;
        }
        break;
    }
    case SQLStmtType::SELECT: {
        auto& sel = static_cast<SelectStmt&>(*stmt);
        auto it = tables_.find(sel.table_name);
        if (it == tables_.end()) {
            std::cerr << "[ERR] Table '" << sel.table_name << "' not found." << std::endl;
            return;
        }

        // 开始事务并持有共享锁
        auto txn = txn_manager_->Begin();
        if (lock_manager_) {
            lock_manager_->LockShared(txn, sel.table_name);
        }

        // 使用 Planner 构建执行器树（SeqScan → Filter → Projection）
        auto executor = planner_->CreatePlan(&sel);
        if (!executor) {
            std::cerr << "[ERR] Failed to create execution plan." << std::endl;
            txn_manager_->Abort(txn);
            return;
        }

        executor->Init();

        int count = 0;
        Tuple tuple;
        while (executor->Next(&tuple)) {
            count++;
            const auto& vals = tuple.GetValues();
            std::cout << "[";
            for (size_t i = 0; i < vals.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << vals[i];
            }
            std::cout << "]" << std::endl;
        }

        txn_manager_->Commit(txn);
        std::cout << "[OK] " << count << " row(s) returned." << std::endl;
        break;
    }
    case SQLStmtType::DELETE: {
        auto& del = static_cast<DeleteStmt&>(*stmt);
        auto it = tables_.find(del.table_name);
        if (it == tables_.end()) {
            std::cerr << "[ERR] Table '" << del.table_name << "' not found." << std::endl;
            return;
        }

        // 开始事务并持有排他锁
        auto txn = txn_manager_->Begin();
        if (lock_manager_) {
            lock_manager_->LockExclusive(txn, del.table_name);
        }

        int count = 0;
        TableHeap* heap = it->second.get();

        // 获取 schema（用于 WHERE 条件求值）
        std::vector<std::string> schema;
        auto schema_iter = table_schemas_.find(del.table_name);
        if (schema_iter != table_schemas_.end()) {
            schema = schema_iter->second;
        }

        // 遍历表堆，对匹配 WHERE 条件的元组执行标记删除
        for (auto iter = heap->Begin(); iter != heap->End(); ++iter) {
            auto opt = iter.Get();
            if (!opt.has_value()) continue;

            // 存在 WHERE 条件则求值，不匹配则跳过
            if (del.condition && !del.condition->Evaluate(opt.value(), schema)) {
                continue;
            }

            // 标记删除
            // 索引维护：从索引中移除即将删除的元组
            auto idx_it_del = indexes_.find(del.table_name);
            if (idx_it_del != indexes_.end()) {
                const auto& vals = opt.value().GetValues();
                if (!vals.empty()) {
                    idx_it_del->second->Remove(vals[0]);
                }
            }

            if (heap->DeleteTuple(iter.GetRID())) {
                count++;
            }
        }

        txn_manager_->Commit(txn);
        std::cout << "[OK] " << count << " row(s) deleted from '"
                  << del.table_name << "'." << std::endl;
        break;
    }
    case SQLStmtType::UPDATE: {
        auto& upd = static_cast<UpdateStmt&>(*stmt);
        auto it = tables_.find(upd.table_name);
        if (it == tables_.end()) {
            std::cerr << "[ERR] Table '" << upd.table_name << "' not found." << std::endl;
            return;
        }

        // 开始事务并持有排他锁
        auto txn = txn_manager_->Begin();
        if (lock_manager_) {
            lock_manager_->LockExclusive(txn, upd.table_name);
        }

        int count = 0;
        TableHeap* heap = it->second.get();

        // 获取 schema
        std::vector<std::string> schema;
        auto schema_iter = table_schemas_.find(upd.table_name);
        if (schema_iter != table_schemas_.end()) {
            schema = schema_iter->second;
        }

        // 遍历表堆，对匹配 WHERE 条件的元组执行更新
        // 使用两遍扫描：先收集 RID 和新旧元组，再逐个更新（避免迭代器失效）
        struct UpdateEntry {
            RID rid;
            Tuple old_tuple;
            Tuple new_tuple;
        };
        std::vector<UpdateEntry> to_update;
        for (auto iter = heap->Begin(); iter != heap->End(); ++iter) {
            auto opt = iter.Get();
            if (!opt.has_value()) continue;

            if (upd.condition && !upd.condition->Evaluate(opt.value(), schema)) {
                continue;
            }

            // 构造新元组：复制原值，替换指定列
            auto old_vals = opt->GetValues();
            for (size_t i = 0; i < upd.col_names.size(); ++i) {
                // 在 schema 中查找列索引
                for (size_t j = 0; j < schema.size(); ++j) {
                    if (schema[j] == upd.col_names[i]) {
                        if (j < old_vals.size()) {
                            old_vals[j] = upd.values[i];
                        }
                        break;
                    }
                }
            }

            Tuple new_tuple(old_vals);
            to_update.push_back({iter.GetRID(), opt.value(), new_tuple});
        }

        // 第二遍：执行更新（含索引维护）
        for (auto& entry : to_update) {
            // 索引维护：先移除旧索引条目
            auto idx_it_upd = indexes_.find(upd.table_name);
            if (idx_it_upd != indexes_.end()) {
                const auto& old_vals = entry.old_tuple.GetValues();
                if (!old_vals.empty()) {
                    idx_it_upd->second->Remove(old_vals[0]);
                }
            }

            if (heap->UpdateTuple(entry.rid, entry.new_tuple)) {
                count++;
                // 索引维护：插入新索引条目
                if (idx_it_upd != indexes_.end()) {
                    const auto& new_vals = entry.new_tuple.GetValues();
                    if (!new_vals.empty()) {
                        idx_it_upd->second->Insert(new_vals[0], entry.new_tuple, 0);
                    }
                }
            } else {
                // 更新失败：恢复旧索引条目
                if (idx_it_upd != indexes_.end()) {
                    const auto& old_vals = entry.old_tuple.GetValues();
                    if (!old_vals.empty()) {
                        idx_it_upd->second->Insert(old_vals[0], entry.old_tuple, 0);
                    }
                }
            }
        }

        txn_manager_->Commit(txn);
        std::cout << "[OK] " << count << " row(s) updated in '"
                  << upd.table_name << "'." << std::endl;
        break;
    }
    default:
        std::cerr << "[ERR] Unsupported SQL statement." << std::endl;
        break;
    }
}

} // namespace db
