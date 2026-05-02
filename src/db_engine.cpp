#include "db_engine.h"
#include "storage/file_disk_manager.h"
#include "storage/table_iterator.h"
#include <iostream>

namespace db {

DBEngine::DBEngine(const std::string& db_file, size_t buffer_pool_size) {
    // 1. 创建磁盘管理器（基于本地文件）
    disk_manager_ = std::make_unique<FileDiskManager>(db_file);

    // 2. 创建缓冲池
    buffer_pool_manager_ = std::make_unique<BufferPoolManager>(
        buffer_pool_size, disk_manager_.get());

    // 3. 创建 SQL 解析器
    parser_ = std::make_unique<SQLParser>();
}

DBEngine::~DBEngine() {
    // 析构顺序：先清理 table_heap（会 unpin 页面），再清理 BPM，最后清理磁盘
    tables_.clear();
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
        std::cout << "[OK] Table '" << ct.table_name << "' created." << std::endl;
        break;
    }
    case SQLStmtType::INSERT: {
        auto& ins = static_cast<InsertStmt&>(*stmt);
        TableHeap* heap = GetOrCreateTable(ins.table_name);
        RID rid;
        if (heap->InsertTuple(ins.tuple, &rid)) {
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

        int count = 0;
        for (auto iter = it->second->Begin(); iter != it->second->End(); ++iter) {
            auto t = iter.Get();
            if (t.has_value()) {
                count++;
                const auto& vals = t->GetValues();
                std::cout << "[";
                for (size_t i = 0; i < vals.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << vals[i];
                }
                std::cout << "]" << std::endl;
            }
        }
        std::cout << "[OK] " << count << " row(s) returned." << std::endl;
        break;
    }
    default:
        std::cerr << "[ERR] Unsupported SQL statement." << std::endl;
        break;
    }
}

} // namespace db
