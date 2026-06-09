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

    lock_manager_->SetTransactionManager(txn_manager_.get());

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
        std::cout << "[Recovery] No log records found, skipping recovery." << std::endl;
        return;
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

    // 3. 收集活跃但无终止标记的事务（崩溃时正在运行的事务）
    //    按 ARIES 协议，未提交事务应视为 aborted 并执行 UNDO
    for (const auto& rec : records) {
        if (rec.op_type == LogOpType::INSERT ||
            rec.op_type == LogOpType::DELETE ||
            rec.op_type == LogOpType::UPDATE) {
            txn_id_t tid = rec.txn_id;
            if (committed_txns.count(tid) == 0 && aborted_txns.count(tid) == 0) {
                aborted_txns.insert(tid);
            }
        }
    }

    // 4. REDO：按 LSN 顺序重放已提交事务的操作
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

    // 5. UNDO：对每个中止的事务调用 ApplyUndoForTransaction
    for (txn_id_t tid : aborted_txns) {
        ApplyUndoForTransaction(tid);
    }

    // 6. 将恢复后的所有脏页刷盘
    buffer_pool_manager_->FlushAllPages();

    // 7. 截断 WAL（恢复完成后日志可丢弃）
    lsn_t last_lsn = records.back().lsn;
    log_manager_->TruncateAfter(last_lsn);

    // 8. 输出恢复摘要
    std::cout << "[Recovery] REDO " << committed_txns.size()
              << " committed + UNDO " << aborted_txns.size()
              << " aborted transactions recovered." << std::endl;
}

void DBEngine::ApplyUndoForTransaction(txn_id_t txn_id) {
    // 1. 读取 WAL 中所有日志记录
    auto records = log_manager_->ReadLogRecords();

    // 2. 逆序遍历，只处理该事务的 INSERT/DELETE/UPDATE
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const auto& rec = *it;
        if (rec.txn_id != txn_id) continue;

        // 只处理数据操作，跳过控制记录（BEGIN/COMMIT/ABORT）
        if (rec.op_type != LogOpType::INSERT &&
            rec.op_type != LogOpType::DELETE &&
            rec.op_type != LogOpType::UPDATE) {
            continue;
        }

        // 3. 获取目标页面
        Page* page = buffer_pool_manager_->FetchPage(rec.page_id);
        if (!page) {
            std::cerr << "[UNDO] txn #" << txn_id
                      << ": failed to fetch page " << rec.page_id << std::endl;
            continue;
        }

        auto* tp = reinterpret_cast<TablePage*>(page);

        // 4. 执行 TablePage 级补偿操作 + B+ 树索引恢复
        switch (rec.op_type) {
        case LogOpType::INSERT:
            // UNDO INSERT：标记删除 + 移除索引
            tp->MarkDelete(rec.slot_num);
            if (!rec.table_name.empty()) {
                auto idx_it = indexes_.find(rec.table_name);
                if (idx_it != indexes_.end() && rec.new_tuple.GetSize() > 0) {
                    const auto& vals = rec.new_tuple.GetValues();
                    if (!vals.empty()) {
                        idx_it->second->Remove(vals[0]);
                    }
                }
            }
            break;
        case LogOpType::DELETE:
            // UNDO DELETE：重新插入旧元组 + 恢复索引
            if (rec.old_tuple.GetSize() > 0) {
                tp->InsertTuple(rec.old_tuple, nullptr);
                if (!rec.table_name.empty()) {
                    auto idx_it = indexes_.find(rec.table_name);
                    if (idx_it != indexes_.end()) {
                        const auto& vals = rec.old_tuple.GetValues();
                        if (!vals.empty()) {
                            idx_it->second->Insert(vals[0], rec.old_tuple, 0);
                        }
                    }
                }
            }
            break;
        case LogOpType::UPDATE:
            // UNDO UPDATE：恢复旧值 + 恢复旧索引 key
            if (rec.old_tuple.GetSize() > 0) {
                tp->UpdateTuple(rec.slot_num, rec.old_tuple);
                if (!rec.table_name.empty()) {
                    auto idx_it = indexes_.find(rec.table_name);
                    if (idx_it != indexes_.end()) {
                        // 移除 new_tuple 的 key（如果不同于 old key）
                        if (rec.new_tuple.GetSize() > 0) {
                            const auto& new_vals = rec.new_tuple.GetValues();
                            if (!new_vals.empty()) {
                                idx_it->second->Remove(new_vals[0]);
                            }
                        }
                        // 恢复 old_tuple 的 key
                        const auto& old_vals = rec.old_tuple.GetValues();
                        if (!old_vals.empty()) {
                            idx_it->second->Insert(old_vals[0], rec.old_tuple, 0);
                        }
                    }
                }
            }
            break;
        default:
            break;
        }

        buffer_pool_manager_->UnpinPage(rec.page_id, true);
    }
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
    case SQLStmtType::DROP_TABLE: {
        auto& dt = static_cast<DropTableStmt&>(*stmt);
        // 对不存在的表报错
        if (tables_.find(dt.table_name) == tables_.end()) {
            std::cerr << "[ERR] Table '" << dt.table_name << "' not found." << std::endl;
            return;
        }
        // 1. 删除索引
        auto idx_it = indexes_.find(dt.table_name);
        if (idx_it != indexes_.end()) {
            idx_it->second->Drop();
            indexes_.erase(idx_it);
        }
        // 2. 删除表堆（回收磁盘空间）
        auto tbl_it = tables_.find(dt.table_name);
        if (tbl_it != tables_.end()) {
            tbl_it->second->Drop();
            tables_.erase(tbl_it);
        }
        // 3. 清理 schema
        table_schemas_.erase(dt.table_name);
        std::cout << "[OK] Table '" << dt.table_name << "' dropped." << std::endl;
        break;
    }
    case SQLStmtType::INSERT: {
        auto& ins = static_cast<InsertStmt&>(*stmt);
        TableHeap* heap = GetOrCreateTable(ins.table_name);

        // 事务处理：多语句事务复用 current_txn_，否则 auto-commit
        Transaction* txn = nullptr;
        bool auto_commit = false;
        if (current_txn_) {
            txn = current_txn_;
        } else {
            txn = txn_manager_->Begin();
            auto_commit = true;
            // auto-commit 模式下获取排他锁
            if (lock_manager_) {
                lock_manager_->LockExclusive(txn, ins.table_name);
            }
        }

        RID rid;
        if (heap->InsertTuple(ins.tuple, &rid)) {
            // WAL 日志：记录 INSERT 操作（先插入后写日志以获取 slot_num）
            if (log_manager_) {
                LogRecord record;
                record.txn_id = txn->GetTransactionId();
                record.op_type = LogOpType::INSERT;
                record.table_name = ins.table_name;
                record.page_id = rid.GetPageId();
                record.slot_num = rid.GetSlotNum();
                record.new_tuple = ins.tuple;
                record.prev_lsn = txn->GetPrevLSN();
                lsn_t lsn = log_manager_->AppendLogRecord(record);
                txn->SetPrevLSN(lsn);
            }

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

        // auto-commit：立即提交（释放锁 + 写 COMMIT 日志）
        if (auto_commit) {
            txn_manager_->Commit(txn);
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

        // 事务处理：多语句事务复用 current_txn_，否则 auto-commit
        Transaction* txn = nullptr;
        bool auto_commit = false;
        if (current_txn_) {
            txn = current_txn_;
        } else {
            txn = txn_manager_->Begin();
            auto_commit = true;
        }
        if (lock_manager_ && auto_commit) {
            // 根据隔离级别选择锁策略（auto-commit 模式下立即获取锁）
            switch (txn->GetIsolationLevel()) {
            case IsolationLevel::SERIALIZABLE:
                lock_manager_->LockExclusiveForRead(txn, sel.table_name);
                break;
            case IsolationLevel::REPEATABLE_READ:
                lock_manager_->LockShared(txn, sel.table_name);
                break;
            case IsolationLevel::READ_COMMITTED:
            default:
                lock_manager_->LockSharedForRead(txn, sel.table_name);
                break;
            }
        }

        // 使用 Planner 构建执行器树（SeqScan → Filter → Projection）
        auto executor = planner_->CreatePlan(&sel);
        if (!executor) {
            std::cerr << "[ERR] Failed to create execution plan." << std::endl;
            if (auto_commit) txn_manager_->Abort(txn);
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

        // auto-commit：立即提交（释放锁）
        if (auto_commit) {
            txn_manager_->Commit(txn);
        }
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

        // 事务处理：多语句事务复用 current_txn_，否则 auto-commit
        Transaction* txn = nullptr;
        bool auto_commit = false;
        if (current_txn_) {
            txn = current_txn_;
        } else {
            txn = txn_manager_->Begin();
            auto_commit = true;
            if (lock_manager_) {
                lock_manager_->LockExclusive(txn, del.table_name);
            }
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

            // 保存旧值（用于 WAL 日志和 UNDO）
            Tuple old_tuple = opt.value();

            // 索引维护：从索引中移除即将删除的元组
            auto idx_it_del = indexes_.find(del.table_name);
            if (idx_it_del != indexes_.end()) {
                const auto& vals = old_tuple.GetValues();
                if (!vals.empty()) {
                    idx_it_del->second->Remove(vals[0]);
                }
            }

            if (heap->DeleteTuple(iter.GetRID())) {
                count++;
                // WAL 日志：记录 DELETE 操作（含 old_tuple 用于 UNDO）
                if (log_manager_) {
                    LogRecord record;
                    record.txn_id = txn->GetTransactionId();
                    record.op_type = LogOpType::DELETE;
                    record.table_name = del.table_name;
                    record.page_id = iter.GetRID().GetPageId();
                    record.slot_num = iter.GetRID().GetSlotNum();
                    record.old_tuple = old_tuple;
                    record.prev_lsn = txn->GetPrevLSN();
                    lsn_t lsn = log_manager_->AppendLogRecord(record);
                    txn->SetPrevLSN(lsn);
                }
            }
        }

        // auto-commit：立即提交
        if (auto_commit) {
            txn_manager_->Commit(txn);
        }
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

        // 事务处理：多语句事务复用 current_txn_，否则 auto-commit
        Transaction* txn = nullptr;
        bool auto_commit = false;
        if (current_txn_) {
            txn = current_txn_;
        } else {
            txn = txn_manager_->Begin();
            auto_commit = true;
            if (lock_manager_) {
                lock_manager_->LockExclusive(txn, upd.table_name);
            }
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

        // 第二遍：执行更新（含索引维护 + WAL 日志）
        for (auto& entry : to_update) {
            auto idx_it_upd = indexes_.find(upd.table_name);

            // 判断是否可原地更新（尺寸相同 → 原地覆盖；尺寸不同 → 删旧插新）
            if (entry.old_tuple.GetSize() == entry.new_tuple.GetSize()) {
                // === 原地更新路径 ===
                // 索引维护：先移除旧索引条目
                if (idx_it_upd != indexes_.end()) {
                    const auto& old_vals = entry.old_tuple.GetValues();
                    if (!old_vals.empty()) {
                        idx_it_upd->second->Remove(old_vals[0]);
                    }
                }

                if (heap->UpdateTuple(entry.rid, entry.new_tuple)) {
                    count++;
                    // WAL 日志：记录 UPDATE 操作（含 old_tuple/new_tuple）
                    if (log_manager_) {
                        LogRecord record;
                        record.txn_id = txn->GetTransactionId();
                        record.op_type = LogOpType::UPDATE;
                        record.table_name = upd.table_name;
                        record.page_id = entry.rid.GetPageId();
                        record.slot_num = entry.rid.GetSlotNum();
                        record.old_tuple = entry.old_tuple;
                        record.new_tuple = entry.new_tuple;
                        record.prev_lsn = txn->GetPrevLSN();
                        lsn_t lsn = log_manager_->AppendLogRecord(record);
                        txn->SetPrevLSN(lsn);
                    }
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
            } else {
                // === 尺寸不匹配：拆为 DELETE + INSERT 两条 WAL 日志 ===
                // 1. 写 DELETE WAL 日志（记录旧元组所在槽位，供 UNDO 恢复）
                if (log_manager_) {
                    LogRecord del_record;
                    del_record.txn_id = txn->GetTransactionId();
                    del_record.op_type = LogOpType::DELETE;
                    del_record.table_name = upd.table_name;
                    del_record.page_id = entry.rid.GetPageId();
                    del_record.slot_num = entry.rid.GetSlotNum();
                    del_record.old_tuple = entry.old_tuple;
                    del_record.prev_lsn = txn->GetPrevLSN();
                    lsn_t del_lsn = log_manager_->AppendLogRecord(del_record);
                    txn->SetPrevLSN(del_lsn);
                }

                // 索引维护：移除旧索引条目
                if (idx_it_upd != indexes_.end()) {
                    const auto& old_vals = entry.old_tuple.GetValues();
                    if (!old_vals.empty()) {
                        idx_it_upd->second->Remove(old_vals[0]);
                    }
                }

                // 2. 标记删除旧元组
                heap->DeleteTuple(entry.rid);

                // 3. 插入新元组，获取新 RID
                RID new_rid;
                if (heap->InsertTuple(entry.new_tuple, &new_rid)) {
                    count++;
                    // 写 INSERT WAL 日志（记录新元组所在槽位，供 UNDO 回滚）
                    if (log_manager_) {
                        LogRecord ins_record;
                        ins_record.txn_id = txn->GetTransactionId();
                        ins_record.op_type = LogOpType::INSERT;
                        ins_record.table_name = upd.table_name;
                        ins_record.page_id = new_rid.GetPageId();
                        ins_record.slot_num = new_rid.GetSlotNum();
                        ins_record.new_tuple = entry.new_tuple;
                        ins_record.prev_lsn = txn->GetPrevLSN();
                        lsn_t ins_lsn = log_manager_->AppendLogRecord(ins_record);
                        txn->SetPrevLSN(ins_lsn);
                    }
                    // 索引维护：插入新索引条目
                    if (idx_it_upd != indexes_.end()) {
                        const auto& new_vals = entry.new_tuple.GetValues();
                        if (!new_vals.empty()) {
                            idx_it_upd->second->Insert(new_vals[0], entry.new_tuple, 0);
                        }
                    }
                }
                // 注意：InsertTuple 失败时，旧元组已被标记删除但新元组未插入，
                // 旧索引条目已移除，数据处于不一致状态。实际场景中极少发生（磁盘满等）。
            }
        }

        // auto-commit：立即提交
        if (auto_commit) {
            txn_manager_->Commit(txn);
        }
        std::cout << "[OK] " << count << " row(s) updated in '"
                  << upd.table_name << "'." << std::endl;
        break;
    }
    case SQLStmtType::BEGIN_TXN: {
        auto& begin_stmt = static_cast<BeginStmt&>(*stmt);
        if (current_txn_) {
            std::cerr << "[ERR] Transaction #" << current_txn_->GetTransactionId()
                      << " already active. Commit or abort it first." << std::endl;
            break;
        }
        current_txn_ = txn_manager_->Begin(begin_stmt.iso_level);
        std::cout << "[OK] Transaction #" << current_txn_->GetTransactionId()
                  << " started." << std::endl;
        break;
    }
    case SQLStmtType::COMMIT_TXN: {
        if (!current_txn_) {
            std::cerr << "[ERR] No active transaction to commit." << std::endl;
            break;
        }
        txn_id_t tid = current_txn_->GetTransactionId();
        txn_manager_->Commit(current_txn_);
        current_txn_ = nullptr;
        std::cout << "[OK] Transaction #" << tid << " committed." << std::endl;
        break;
    }
    case SQLStmtType::ABORT_TXN: {
        if (!current_txn_) {
            std::cerr << "[ERR] No active transaction to abort." << std::endl;
            break;
        }
        txn_id_t tid = current_txn_->GetTransactionId();
        // 执行 UNDO 回滚（逆序补偿 DML 操作 + 恢复索引）
        ApplyUndoForTransaction(tid);
        txn_manager_->Abort(current_txn_);
        current_txn_ = nullptr;
        std::cout << "[OK] Transaction #" << tid << " aborted (rolled back)." << std::endl;
        break;
    }
    default:
        std::cerr << "[ERR] Unsupported SQL statement." << std::endl;
        break;
    }
}

} // namespace db
