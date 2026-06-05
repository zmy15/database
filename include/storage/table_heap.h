#pragma once

#include "storage/table_page.h"
#include "buffer/buffer_pool_manager.h"
#include <optional>

namespace db {

class TableIterator; // 前置声明迭代器

/**
 * @brief TableHeap (表堆) 代表磁盘上存放某张表所有记录的数据结构。
 * 它实际上是一个由多个 TablePage 串联而成的双向链表。
 */
class TableHeap {
public:
    TableHeap(BufferPoolManager* bpm);

    // 初始化或创建表（将会在磁盘上分配第一页并赋值为 root/first page）
    void Init();

    // 从已有首页恢复表（用于数据库重启后重建 TableHeap）
    void Load(page_id_t first_page_id);

    // 将 Tuple 插入到表堆中，返回新分配或更新的 RID
    bool InsertTuple(const Tuple& tuple, RID* rid);

    // 从表中读取数据
    std::optional<Tuple> GetTuple(const RID& rid);

        // 删改
        bool DeleteTuple(const RID& rid);
        bool UpdateTuple(const RID& rid, const Tuple& new_tuple);

    // 迭代器入口
    TableIterator Begin();
    TableIterator End();

    page_id_t GetFirstPageId() const { return first_page_id_; }

    // 删除整张表：遍历所有页面，回收空间
    void Drop();
private:
    BufferPoolManager* bpm_;
    page_id_t first_page_id_{INVALID_PAGE_ID}; // 表头页 (相当于表结构的总入口)
    page_id_t last_page_id_{INVALID_PAGE_ID};  // 链表尾页 (常用来快速新增数据)
};

} // namespace db
