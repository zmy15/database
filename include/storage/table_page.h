#pragma once

#include "buffer/page.h"
#include "storage/tuple.h"
#include "buffer/buffer_pool_manager.h"
#include <optional>
#include <cstring>

namespace db {

/**
 * @brief TablePage 继承自基类 Page。实际上它不持有新的内存，
 * 只是提供了一层解析 4KB `data_` 的抽象逻辑。
 *
 * Slotted Page 的数据布局设计如下:
 * ========================================================
 * | 页面元数据Header | 槽位数组(从前往后) ->   ...   <- 实际数据内容(从后往前) |
 * ========================================================
 * Header (24 bytes):
 * - PageId (4 bytes)
 * - LSN (4 bytes)
 * - 前一页ID (4 bytes)
 * - 后一页ID (4 bytes)
 * - 空闲空间的开始偏移 (4 bytes)    <- 从页尾往回算的数据区的尽头
 * - Tuple 的数量 (4 bytes)
 *
 * 槽位 (TUPLE_SLOT, 每条占 8 bytes):
 * - 数据的偏移量 (4 bytes)
 * - 数据的长度 (4 bytes)
 */
class TablePage : public Page {
public:
    void Init(page_id_t page_id, page_id_t prev_page_id, BufferPoolManager* bpm);
    // 删改
    bool MarkDelete(uint32_t slot_num);
    bool UpdateTuple(uint32_t slot_num, const Tuple& new_tuple);


    page_id_t GetTablePageId() { return *reinterpret_cast<page_id_t*>(GetData()); }
    page_id_t GetPrevPageId() { return *reinterpret_cast<page_id_t*>(GetData() + 8); }
    page_id_t GetNextPageId() { return *reinterpret_cast<page_id_t*>(GetData() + 12); }
    void SetPrevPageId(page_id_t prev_page_id) { std::memcpy(GetData() + 8, &prev_page_id, 4); }
    void SetNextPageId(page_id_t next_page_id) { std::memcpy(GetData() + 12, &next_page_id, 4); }

    // 增查
    bool InsertTuple(const Tuple& tuple, RID* rid);
    std::optional<Tuple> GetTuple(const RID& rid);

    // 页面内一共有几条 Tuple 了？
    uint32_t GetTupleCount() { return *reinterpret_cast<uint32_t*>(GetData() + 20); }

private:
    static constexpr size_t SIZE_TABLE_PAGE_HEADER = 24;
    static constexpr size_t SIZE_TUPLE_SLOT = 8;

    // 空闲空间指针（从后面一直往前面长）
    uint32_t GetFreeSpacePointer() { return *reinterpret_cast<uint32_t*>(GetData() + 16); }
    void SetFreeSpacePointer(uint32_t free_space_pointer) { std::memcpy(GetData() + 16, &free_space_pointer, 4); }

    void SetTupleCount(uint32_t tuple_count) { std::memcpy(GetData() + 20, &tuple_count, 4); }

    // 槽位访问器（每个槽位占 SIZE_TUPLE_SLOT 字节）
    uint32_t GetTupleOffset(uint32_t slot_id) {
        return *reinterpret_cast<uint32_t*>(GetData() + SIZE_TABLE_PAGE_HEADER + slot_id * SIZE_TUPLE_SLOT);
    }
    uint32_t GetTupleSize(uint32_t slot_id) {
        return *reinterpret_cast<uint32_t*>(GetData() + SIZE_TABLE_PAGE_HEADER + slot_id * SIZE_TUPLE_SLOT + 4);
    }
    void SetTupleOffset(uint32_t slot_id, uint32_t offset) {
        std::memcpy(GetData() + SIZE_TABLE_PAGE_HEADER + slot_id * SIZE_TUPLE_SLOT, &offset, 4);
    }
    void SetTupleSize(uint32_t slot_id, uint32_t size) {
        std::memcpy(GetData() + SIZE_TABLE_PAGE_HEADER + slot_id * SIZE_TUPLE_SLOT + 4, &size, 4);
    }
};

} // namespace db
