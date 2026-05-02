#include "storage/table_heap.h"
#include "storage/table_iterator.h"

namespace db {

TableHeap::TableHeap(BufferPoolManager* bpm) : bpm_(bpm) {}

void TableHeap::Init() {
    // 分配这张表的第一页
    Page* page = bpm_->NewPage(&first_page_id_);
    if (page == nullptr) {
        throw std::runtime_error("TableHeap Init Failed: out of memory.");
    }

    // 初始化页布局结构
    auto* table_page = reinterpret_cast<TablePage*>(page);
    table_page->Init(first_page_id_, INVALID_PAGE_ID, bpm_);

    // 首尾相连，只有一页
    last_page_id_ = first_page_id_;
    bpm_->UnpinPage(first_page_id_, true);
}

bool TableHeap::InsertTuple(const Tuple& tuple, RID* rid) {
    if (last_page_id_ == INVALID_PAGE_ID) { Init(); }

    Page* last_page = bpm_->FetchPage(last_page_id_);
    auto* table_page = reinterpret_cast<TablePage*>(last_page);

    // 尝试在当前的最后一页中插入
    if (table_page->InsertTuple(tuple, rid)) {
        // 成功！
        bpm_->UnpinPage(last_page_id_, true);
        return true;
    }

    // ==========================================
    // 现有的这一页装不下了！我们需要向后扩充链表
    // ==========================================
    page_id_t new_page_id;
    Page* new_page = bpm_->NewPage(&new_page_id);
    if (!new_page) {
        // 缓冲池耗尽或者无法开启新页
        bpm_->UnpinPage(last_page_id_, false);
        return false;
    }

    auto* new_table_page = reinterpret_cast<TablePage*>(new_page);
    new_table_page->Init(new_page_id, last_page_id_, bpm_);

    // 重新连接链表 (老页的 Next 指向 新页)
    table_page->SetNextPageId(new_page_id);

    // 释放老页
    bpm_->UnpinPage(last_page_id_, true);

    // 将 Tuple 插入到新鲜出炉的这一页去
    new_table_page->InsertTuple(tuple, rid);

    // 更新堆信息
    last_page_id_ = new_page_id;
    bpm_->UnpinPage(new_page_id, true);

    return true;
}

std::optional<Tuple> TableHeap::GetTuple(const RID& rid) {
    Page* page = bpm_->FetchPage(rid.GetPageId());
    if (!page) { return std::nullopt; }

    auto* table_page = reinterpret_cast<TablePage*>(page);
    auto result = table_page->GetTuple(rid);

    bpm_->UnpinPage(rid.GetPageId(), false); // 只是读不修改，设为 false

    return result;
}

TableIterator TableHeap::Begin() {
    if (first_page_id_ == INVALID_PAGE_ID) {
        return End();
    }

    // 定位到表的第一页的第 0 号槽位
    // 注意：如果首个槽位被删除了或者当前无数据该怎么办？这里迭代器++内部会处理空洞的情况。
    RID first_rid(first_page_id_, 0); 

    // 如果正好第一页啥都没有，就通过自增让它自己找下一个有效记录
    TableIterator it(this, first_rid, bpm_);
    if (!it.Get().has_value()) {
        ++it; 
    }
    return it;
}

TableIterator TableHeap::End() {
    // End 用了一个不可能生效的状态(无效页ID) 来充当岗哨
    return TableIterator(this, RID(INVALID_PAGE_ID, 0), bpm_);
}


// ====================================================
// 迭代器部分实现
// ====================================================

TableIterator::TableIterator(TableHeap* table_heap, RID rid, BufferPoolManager* bpm)
    : table_heap_(table_heap), rid_(rid), bpm_(bpm) {}

std::optional<Tuple> TableIterator::Get() {
    return table_heap_->GetTuple(rid_);
}

TableIterator& TableIterator::operator++() {
    while (true) {
        page_id_t current_page_id = rid_.GetPageId();
        if (current_page_id == INVALID_PAGE_ID) {
            return *this; 
        }

        Page* page = bpm_->FetchPage(current_page_id);
        auto* table_page = reinterpret_cast<TablePage*>(page);

        RID next_rid;
        uint32_t current_slot = rid_.GetSlotNum();

        // 假设简单起见这页后面的槽位满了
        if (current_slot + 1 < table_page->GetTupleCount()) {
            // 同一页内，跳到下一条
            next_rid.Set(current_page_id, current_slot + 1);
        } else {
            // 本页扫描完了，顺着链表找下一页！
            page_id_t next_page_id = table_page->GetNextPageId();
            if (next_page_id == INVALID_PAGE_ID) {
                // 没有下一页了，结束扫描，游标指向 End
                next_rid.Set(INVALID_PAGE_ID, 0);
            } else {
                // 跳到了下一页的开头
                next_rid.Set(next_page_id, 0);
            }
        }

        bpm_->UnpinPage(current_page_id, false);
        rid_ = next_rid;

        // 如果已经到达表尾，直接退出循环
        if (rid_.GetPageId() == INVALID_PAGE_ID) {
            break;
        }

        // 检查新游标对应的数据是否有效（非删除/空洞）
        // 如果有效，停止跳跃；如果无效，继续大循环找下一条
        if (Get().has_value()) {
            break;
        }
    }

    return *this;
}

} // namespace db
