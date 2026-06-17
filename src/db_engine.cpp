#include "db_engine.h"
#include "storage/file_disk_manager.h"
#include "storage/file_log_manager.h"
#include "storage/table_iterator.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include <iostream>
#include <cstring>

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

    // 注册 UNDO 回调：死锁 victim 和用户 ABORT 统一由此路径回滚数据变更
    txn_manager_->SetUndoCallback([this](txn_id_t tid) {
        ApplyUndoForTransaction(tid);
    });

    // 5. 创建 SQL 解析器
    parser_ = std::make_unique<SQLParser>();

    // 6. 创建执行计划构建器（持有对 schemas / indexes / tables / bpm 的引用）
    planner_ = std::make_unique<Planner>(
        table_schemas_, indexes_, tables_,
        buffer_pool_manager_.get(),
        /*txn=*/nullptr,
        lock_manager_.get(),
        txn_manager_.get());

    // 7. 从 Catalog 加载持久化的表结构
    LoadCatalog();

    // 8. 崩溃恢复：REDO 已提交事务
    DoRecovery();
}

DBEngine::~DBEngine() {
    // 析构顺序：先清理 table_heap（会 unpin 页面），再清理 BPM，最后清理磁盘
    // 正常退出前保存 Catalog
    SaveCatalog();
    tables_.clear();
    indexes_.clear();
    if (log_manager_) {
        log_manager_->FlushLogs();
        log_manager_->TruncateAll();  // 正常关闭：所有脏页已刷盘，WAL 可安全清空
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
            continue;
        }

        if (rec.op_type != LogOpType::INSERT &&
            rec.op_type != LogOpType::DELETE &&
            rec.op_type != LogOpType::UPDATE) {
            continue;
        }

        Page* page = buffer_pool_manager_->FetchPage(rec.page_id);
        if (!page) continue;

        if (page->GetLSN() >= rec.lsn) {
            buffer_pool_manager_->UnpinPage(rec.page_id, false);
            continue;
        }

        auto* tp = reinterpret_cast<TablePage*>(page);

        switch (rec.op_type) {
        case LogOpType::INSERT:
            if (rec.new_tuple.GetSize() > 0) {
                tp->InsertTuple(rec.new_tuple, nullptr);
            }
            break;
        case LogOpType::DELETE:
            tp->MarkDelete(rec.slot_num);
            break;
        case LogOpType::UPDATE:
            if (rec.new_tuple.GetSize() > 0) {
                tp->UpdateTuple(rec.slot_num, rec.new_tuple);
            }
            break;
        default:
            break;
        }

        page->SetLSN(rec.lsn);
        buffer_pool_manager_->UnpinPage(rec.page_id, true);
    }

    // 5. UNDO：对每个中止的事务调用 ApplyUndoForTransaction
    for (txn_id_t tid : aborted_txns) {
        ApplyUndoForTransaction(tid);
    }

    // 5.5 将恢复识别出的已提交/已中止事务注册到 TransactionManager
    for (txn_id_t tid : committed_txns) {
        txn_manager_->MarkCommitted(tid);
    }
    for (txn_id_t tid : aborted_txns) {
        txn_manager_->MarkAborted(tid);
    }

    // 6. 将恢复后的所有脏页刷盘
    buffer_pool_manager_->FlushAllPages();

    // 7. 截断 WAL
    lsn_t last_lsn = records.back().lsn;
    log_manager_->TruncateAfter(last_lsn);

    // 8. 输出恢复摘要
    std::cout << "[Recovery] REDO " << committed_txns.size()
              << " committed + UNDO " << aborted_txns.size()
              << " aborted transactions recovered." << std::endl;
}

void DBEngine::ApplyUndoForTransaction(txn_id_t txn_id) {
    auto records = log_manager_->ReadLogRecords();

    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const auto& rec = *it;
        if (rec.txn_id != txn_id) continue;

        if (rec.op_type != LogOpType::INSERT &&
            rec.op_type != LogOpType::DELETE &&
            rec.op_type != LogOpType::UPDATE) {
            continue;
        }

        Page* page = buffer_pool_manager_->FetchPage(rec.page_id);
        if (!page) {
            std::cerr << "[UNDO] txn #" << txn_id
                      << ": failed to fetch page " << rec.page_id << std::endl;
            continue;
        }

        auto* tp = reinterpret_cast<TablePage*>(page);

        switch (rec.op_type) {
        case LogOpType::INSERT:
            tp->MarkDelete(rec.slot_num);
            if (!rec.table_name.empty()) {
                auto idx_it = indexes_.find(rec.table_name);
                if (idx_it != indexes_.end() && rec.new_tuple.GetSize() > 0) {
                    const auto& vals = rec.new_tuple.GetValues();
                    if (!vals.empty()) {
                        idx_it->second->Remove(vals[0], txn_id, txn_manager_.get(), IsolationLevel::READ_COMMITTED);
                    }
                }
            }
            break;
        case LogOpType::DELETE:
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
            if (rec.old_tuple.GetSize() > 0) {
                tp->UpdateTuple(rec.slot_num, rec.old_tuple);
                if (!rec.table_name.empty()) {
                    auto idx_it = indexes_.find(rec.table_name);
                    if (idx_it != indexes_.end()) {
                        if (rec.new_tuple.GetSize() > 0) {
                            const auto& new_vals = rec.new_tuple.GetValues();
                            if (!new_vals.empty()) {
                                idx_it->second->Remove(new_vals[0]);
                            }
                        }
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

// ============================================================
// Catalog 持久化：从 page_id=0 读取/写入表结构
// ============================================================

void DBEngine::LoadCatalog() {
    auto* fdm = dynamic_cast<FileDiskManager*>(disk_manager_.get());
    if (!fdm || !fdm->HasCatalogHeader()) {
        std::cout << "[Catalog] No catalog header found, starting with empty schema." << std::endl;
        return;
    }

    char page_data[PAGE_SIZE];
    fdm->ReadCatalogPage(page_data);

    // 读取 version 判断格式版本
    uint32_t catalog_version = *reinterpret_cast<uint32_t*>(page_data + 4);

    // 跳过 8 字节头部 (magic 4B + version 4B)
    uint32_t offset = 8;
    uint32_t num_tables = *reinterpret_cast<uint32_t*>(page_data + offset);
    offset += sizeof(uint32_t);

    for (uint32_t i = 0; i < num_tables; ++i) {
        // 读取表名
        if (offset + sizeof(uint32_t) > PAGE_SIZE) break;
        uint32_t name_len = *reinterpret_cast<uint32_t*>(page_data + offset);
        offset += sizeof(uint32_t);
        if (offset + name_len > PAGE_SIZE) break;
        std::string table_name(page_data + offset, name_len);
        offset += name_len;

        // 读取列名列表
        if (offset + sizeof(uint32_t) > PAGE_SIZE) break;
        uint32_t col_count = *reinterpret_cast<uint32_t*>(page_data + offset);
        offset += sizeof(uint32_t);
        std::vector<std::string> columns;
        for (uint32_t j = 0; j < col_count; ++j) {
            if (offset + sizeof(uint32_t) > PAGE_SIZE) break;
            uint32_t col_len = *reinterpret_cast<uint32_t*>(page_data + offset);
            offset += sizeof(uint32_t);
            if (offset + col_len > PAGE_SIZE) break;
            std::string col_name(page_data + offset, col_len);
            offset += col_len;
            columns.push_back(col_name);
        }

        table_schemas_[table_name] = columns;

        // 读取表数据页指针和索引信息（v2 新增，共 16 字节）
        page_id_t first_page = INVALID_PAGE_ID;
        page_id_t last_page  = INVALID_PAGE_ID;  // Load() 会自行计算尾页
        uint32_t has_index   = 0;
        page_id_t index_root = INVALID_PAGE_ID;

        if (catalog_version >= 2) {
            if (offset + 16 <= PAGE_SIZE) {
                first_page = *reinterpret_cast<page_id_t*>(page_data + offset); offset += 4;
                last_page  = *reinterpret_cast<page_id_t*>(page_data + offset); offset += 4;
                has_index  = *reinterpret_cast<uint32_t*>(page_data + offset);  offset += 4;
                index_root = *reinterpret_cast<page_id_t*>(page_data + offset); offset += 4;
            }
        }

        // 创建或恢复 TableHeap（不再调用 GetOrCreateTable，避免分配新页）
        auto heap = std::make_unique<TableHeap>(buffer_pool_manager_.get());
        if (first_page != INVALID_PAGE_ID) {
            heap->Load(first_page);  // 重连已有数据页链表
        } else {
            heap->Init();            // 旧格式 Catalog 或空表：分配新页
        }
        tables_[table_name] = std::move(heap);

        // 恢复 B+ 树索引
        if (has_index) {
            auto idx = std::make_unique<BPlusTree>(table_name, buffer_pool_manager_.get());
            if (index_root != INVALID_PAGE_ID) {
                idx->SetRootPageId(index_root);
            }
            indexes_[table_name] = std::move(idx);
        }

        std::cout << "[Catalog] Loaded table '" << table_name
                  << "' (" << col_count << " columns, first_page=" << first_page
                  << ", index=" << (has_index ? "yes" : "no") << ")." << std::endl;
    }
}

void DBEngine::SaveCatalog() {
    auto* fdm = dynamic_cast<FileDiskManager*>(disk_manager_.get());
    if (!fdm || !fdm->HasCatalogHeader()) {
        std::cerr << "[Catalog] No catalog header, cannot save." << std::endl;
        return;
    }

    char page_data[PAGE_SIZE] = {0};

    // 写入 magic + version
    uint32_t magic = DB_CATALOG_MAGIC;
    uint32_t version = DB_CATALOG_VERSION;
    std::memcpy(page_data, &magic, 4);
    std::memcpy(page_data + 4, &version, 4);

    uint32_t offset = 8;
    uint32_t num_tables = static_cast<uint32_t>(table_schemas_.size());
    std::memcpy(page_data + offset, &num_tables, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    for (const auto& [table_name, columns] : table_schemas_) {
        // 写入表名
        uint32_t name_len = static_cast<uint32_t>(table_name.size());
        if (offset + sizeof(uint32_t) + name_len > PAGE_SIZE) {
            std::cerr << "[Catalog] Catalog page overflow!" << std::endl;
            return;
        }
        std::memcpy(page_data + offset, &name_len, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::memcpy(page_data + offset, table_name.data(), name_len);
        offset += name_len;

        // 写入列名列表
        uint32_t col_count = static_cast<uint32_t>(columns.size());
        std::memcpy(page_data + offset, &col_count, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        for (const auto& col : columns) {
            uint32_t col_len = static_cast<uint32_t>(col.size());
            if (offset + sizeof(uint32_t) + col_len > PAGE_SIZE) {
                std::cerr << "[Catalog] Catalog page overflow!" << std::endl;
                return;
            }
            std::memcpy(page_data + offset, &col_len, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            std::memcpy(page_data + offset, col.data(), col_len);
            offset += col_len;
        }

        // 写入表数据页指针和索引信息（v2 新增，共 16 字节）
        page_id_t first_page = INVALID_PAGE_ID;
        page_id_t last_page = INVALID_PAGE_ID;
        auto tbl_it = tables_.find(table_name);
        if (tbl_it != tables_.end()) {
            first_page = tbl_it->second->GetFirstPageId();
            last_page = tbl_it->second->GetLastPageId();
        }

        uint32_t has_index = 0;
        page_id_t index_root = INVALID_PAGE_ID;
        auto idx_it = indexes_.find(table_name);
        if (idx_it != indexes_.end()) {
            has_index = 1;
            index_root = idx_it->second->GetRootPageId();
        }

        if (offset + 16 > PAGE_SIZE) {
            std::cerr << "[Catalog] Catalog page overflow!" << std::endl;
            return;
        }
        std::memcpy(page_data + offset, &first_page, 4); offset += 4;
        std::memcpy(page_data + offset, &last_page, 4);  offset += 4;
        std::memcpy(page_data + offset, &has_index, 4);   offset += 4;
        std::memcpy(page_data + offset, &index_root, 4);  offset += 4;
    }

    fdm->WriteCatalogPage(page_data);
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
        // 自动对第一列创建 B+ 树索引（索引根页在首次 INSERT 时延迟分配）
        if (!ct.columns.empty()) {
            auto idx = std::make_unique<BPlusTree>(ct.table_name, buffer_pool_manager_.get());
            indexes_[ct.table_name] = std::move(idx);
        }
        SaveCatalog();  // 持久化到 Catalog
        std::cout << "[OK] Table '" << ct.table_name << "' created." << std::endl;
        break;
    }
    case SQLStmtType::DROP_TABLE: {
        auto& dt = static_cast<DropTableStmt&>(*stmt);
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
        SaveCatalog();  // 从 Catalog 中移除该表
        std::cout << "[OK] Table '" << dt.table_name << "' dropped." << std::endl;
        break;
    }
    case SQLStmtType::INSERT: {
        auto& ins = static_cast<InsertStmt&>(*stmt);
        TableHeap* heap = GetOrCreateTable(ins.table_name);

        Transaction* txn = nullptr;
        bool auto_commit = false;
        if (current_txn_) {
            txn = current_txn_;
        } else {
            txn = txn_manager_->Begin();
            auto_commit = true;
            if (lock_manager_) {
                lock_manager_->LockExclusive(txn, ins.table_name);
            }
        }

        RID rid;
        if (heap->InsertTuple(ins.tuple, &rid)) {
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

            auto idx_it = indexes_.find(ins.table_name);
            if (idx_it != indexes_.end()) {
                const auto& vals = ins.tuple.GetValues();
                if (!vals.empty()) {
                    idx_it->second->Insert(vals[0], ins.tuple, txn->GetTransactionId(), txn_manager_.get());
                }
            }
            std::cout << "[OK] Inserted 1 row into '" << ins.table_name << "'." << std::endl;
        } else {
            std::cerr << "[ERR] Insert failed." << std::endl;
        }

        if (auto_commit) {
            txn_manager_->Commit(txn);
        }
        break;
    }
    case SQLStmtType::SELECT: {
        auto& sel = static_cast<SelectStmt&>(*stmt);

        Transaction* txn = nullptr;
        bool auto_commit = false;
        if (current_txn_) {
            txn = current_txn_;
        } else {
            txn = txn_manager_->Begin();
            auto_commit = true;
        }
        if (lock_manager_ && auto_commit) {
            if (sel.has_join) {
                for (const auto& tbl : sel.table_names) {
                    switch (txn->GetIsolationLevel()) {
                    case IsolationLevel::SERIALIZABLE:
                        lock_manager_->LockExclusiveForRead(txn, tbl);
                        break;
                    case IsolationLevel::REPEATABLE_READ:
                        lock_manager_->LockShared(txn, tbl);
                        break;
                    case IsolationLevel::READ_COMMITTED:
                    default:
                        lock_manager_->LockSharedForRead(txn, tbl);
                        break;
                    }
                }
            } else {
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
        }

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

        std::vector<std::string> schema;
        auto schema_iter = table_schemas_.find(del.table_name);
        if (schema_iter != table_schemas_.end()) {
            schema = schema_iter->second;
        }

        for (auto iter = heap->Begin(); iter != heap->End(); ++iter) {
            auto opt = iter.Get();
            if (!opt.has_value()) continue;

            if (del.condition && !del.condition->Evaluate(opt.value(), schema)) {
                continue;
            }

            Tuple old_tuple = opt.value();

            auto idx_it_del = indexes_.find(del.table_name);
            if (idx_it_del != indexes_.end()) {
                const auto& vals = old_tuple.GetValues();
                if (!vals.empty()) {
                    idx_it_del->second->Remove(vals[0]);
                }
            }

            if (heap->DeleteTuple(iter.GetRID())) {
                count++;
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

        std::vector<std::string> schema;
        auto schema_iter = table_schemas_.find(upd.table_name);
        if (schema_iter != table_schemas_.end()) {
            schema = schema_iter->second;
        }

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

            auto old_vals = opt->GetValues();
            for (size_t i = 0; i < upd.col_names.size(); ++i) {
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

        for (auto& entry : to_update) {
            auto idx_it_upd = indexes_.find(upd.table_name);

            if (entry.old_tuple.GetSize() == entry.new_tuple.GetSize()) {
                if (idx_it_upd != indexes_.end()) {
                    const auto& old_vals = entry.old_tuple.GetValues();
                    if (!old_vals.empty()) {
                        idx_it_upd->second->Remove(old_vals[0], txn->GetTransactionId(), txn_manager_.get(), txn->GetIsolationLevel());
                    }
                }

                if (heap->UpdateTuple(entry.rid, entry.new_tuple)) {
                    count++;
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
                    if (idx_it_upd != indexes_.end()) {
                        const auto& new_vals = entry.new_tuple.GetValues();
                        if (!new_vals.empty()) {
                            idx_it_upd->second->Insert(new_vals[0], entry.new_tuple, txn->GetTransactionId(), txn_manager_.get());
                        }
                    }
                } else {
                    if (idx_it_upd != indexes_.end()) {
                        const auto& old_vals = entry.old_tuple.GetValues();
                        if (!old_vals.empty()) {
                            idx_it_upd->second->Insert(old_vals[0], entry.old_tuple, txn->GetTransactionId(), txn_manager_.get());
                        }
                    }
                }
            } else {
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

                if (idx_it_upd != indexes_.end()) {
                    const auto& old_vals = entry.old_tuple.GetValues();
                    if (!old_vals.empty()) {
                        idx_it_upd->second->Remove(old_vals[0]);
                    }
                }

                heap->DeleteTuple(entry.rid);

                RID new_rid;
                if (heap->InsertTuple(entry.new_tuple, &new_rid)) {
                    count++;
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
                    if (idx_it_upd != indexes_.end()) {
                        const auto& new_vals = entry.new_tuple.GetValues();
                        if (!new_vals.empty()) {
                            idx_it_upd->second->Insert(new_vals[0], entry.new_tuple, txn->GetTransactionId(), txn_manager_.get());
                        }
                    }
                }
            }
        }

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
        // UNDO 已由 Abort() 内部 undo_callback_ 统一执行
        txn_manager_->Abort(current_txn_);
        current_txn_ = nullptr;
        // 将 UNDO 后的脏页刷盘确保持久化
        buffer_pool_manager_->FlushAllPages();
        std::cout << "[OK] Transaction #" << tid << " aborted (all changes rolled back)." << std::endl;
        break;
    }
    default:
        std::cerr << "[ERR] Unsupported SQL statement." << std::endl;
        break;
    }
}

} // namespace db