#include "storage/table_page.h"
#include "buffer/buffer_pool_manager.h"
#include <optional>

namespace db {

void TablePage::Init(page_id_t page_id, page_id_t prev_page_id, BufferPoolManager* bpm) {
    std::memcpy(GetData(), &page_id, 4); // [0:3] page_id
    // LSN [4:7] initialized to 0 implicitly

    SetPrevPageId(prev_page_id);    // [8:11] prev
    SetNextPageId(INVALID_PAGE_ID); // [12:15] next

    SetFreeSpacePointer(PAGE_SIZE); // [16:19] 开始时，剩余空间的末尾就是页的末尾
    SetTupleCount(0);               // [20:23] 本页记录数
}

bool TablePage::InsertTuple(const Tuple& tuple, RID* rid) {
    uint32_t tuple_size = tuple.GetSize();
    uint32_t slot_num = GetTupleCount();

    // 检查页面空间是否还够？
    // 剩余空间 = 现在的空闲指针 - (页头大小 + 所有的槽位占的大小 + 还要再加一个新槽位)
    uint32_t required_space = SIZE_TABLE_PAGE_HEADER + (slot_num + 1) * SIZE_TUPLE_SLOT + tuple_size;
    if (GetFreeSpacePointer() < required_space) {
        // 这页装不下了！
        return false;
    }

    // 1. 从刚才空闲区的尾部，再往前挪一块地方出来给数据
    uint32_t tuple_offset = GetFreeSpacePointer() - tuple_size;
    SetFreeSpacePointer(tuple_offset); // 收紧空闲区域

    // 2. 将数据正式写入偏移处
    tuple.SerializeTo(GetData() + tuple_offset);

    // 3. 将其记录在 Slot 元数据（页头后面）中
    SetTupleOffset(slot_num, tuple_offset);
    SetTupleSize(slot_num, tuple_size);

    // 4. 更新页面配置
    SetTupleCount(slot_num + 1);

    // 5. 设置返回的 RID
    rid->Set(GetTablePageId(), slot_num);
    return true;
}

std::optional<Tuple> TablePage::GetTuple(const RID& rid) {
    uint32_t slot_num = rid.GetSlotNum();
    if (slot_num >= GetTupleCount()) {
        return std::nullopt; // 超出范围，找不到这条记录
    }

    uint32_t tuple_offset = GetTupleOffset(slot_num);
    uint32_t tuple_size = GetTupleSize(slot_num);

    // 有些记录可能被标记为删除（通过把长度设为 0 或者其他 flag 实现）
    if (tuple_size == 0) {
        return std::nullopt;
    }

    // 依据偏移量读取并完成反序列化
    Tuple tuple(GetData() + tuple_offset, tuple_size);
    tuple.SetRID(rid);

    return tuple;
}

bool TablePage::MarkDelete(uint32_t slot_num) {
    if (slot_num >= GetTupleCount()) {
        return false;
    }
    SetTupleSize(slot_num, 0);
    return true;
}

bool TablePage::UpdateTuple(uint32_t slot_num, const Tuple& new_tuple) {
    if (slot_num >= GetTupleCount()) {
        return false;
    }

    uint32_t old_size = GetTupleSize(slot_num);
    if (old_size == 0) {
        return false;
    }

    uint32_t new_size = new_tuple.GetSize();
    if (new_size != old_size) {
        return false;
    }

    uint32_t tuple_offset = GetTupleOffset(slot_num);
    new_tuple.SerializeTo(GetData() + tuple_offset);
    return true;
}

} // namespace db
