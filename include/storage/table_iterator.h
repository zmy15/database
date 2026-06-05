#pragma once

#include "storage/table_heap.h"
#include <optional>

namespace db {

/**
 * @brief TableIterator 用于顺序扫描 (SeqScan) 遍历表堆中的所有有效 Tuple
 * 隐藏了底层物理跳页的复杂操作。
 */
class TableIterator {
public:
    TableIterator(TableHeap* table_heap, RID rid, BufferPoolManager* bpm);
    TableIterator(const TableIterator& other) = default;

    // 获取当前游标指向的记录
    std::optional<Tuple> Get();

    // 获取当前游标的 RID
    RID GetRID() const { return rid_; }

    // 迭代器步进运算符 (++it)
    TableIterator& operator++();

    // 判断迭代器是否还有效，或是否到达末尾
    bool operator==(const TableIterator& other) const { return rid_ == other.rid_; }
    bool operator!=(const TableIterator& other) const { return !(*this == other); }

private:
    TableHeap* table_heap_;
    RID rid_;
    BufferPoolManager* bpm_;
};

} // namespace db
