#pragma once

#include "common/config.h"
#include "buffer/buffer_pool_manager.h"
#include "storage/tuple.h"
#include <string>
#include <optional>
#include <shared_mutex>

namespace db {

// B+ 树索引结构，依赖 BufferPoolManager 读取和修改节点（Page）
class BPlusTree {
public:
    BPlusTree(std::string index_name, BufferPoolManager* bpm) : name_(std::move(index_name)), bpm_(bpm) {}

    // 插入键值对
    bool Insert(const std::string& key, const Tuple& value, txn_id_t txn_id);
    // 点查
    std::optional<Tuple> GetValue(const std::string& key, txn_id_t txn_id);
    // 范围查询（返回迭代器）
    // IndexIterator Begin(const std::string& start_key);

private:
    std::string name_;
    page_id_t root_page_id_ = -1;
    BufferPoolManager* bpm_;
    std::shared_mutex root_latch_; // 保护根节点变化的锁（如Crabbing协议）
};

} // namespace db
