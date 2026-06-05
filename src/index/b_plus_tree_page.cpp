#include "index/b_plus_tree_page.h"
#include <algorithm>
#include <cstring>

namespace db {

// ============================================================
// 初始化
// ============================================================

void BPlusTreePage::Init(page_id_t page_id, bool is_leaf) {
    std::memset(GetData(), 0, PAGE_SIZE);
    std::memcpy(GetData(), &page_id, 4);          // [0:3] page_id
    // [4:7] lsn 保持 0
    SetPrevPageId(INVALID_PAGE_ID);                // [8:11]
    SetNextPageId(INVALID_PAGE_ID);                // [12:15]
    SetFreeSpacePointer(PAGE_SIZE);                // [16:19]
    SetSlotCount(0);                               // [20:23]
    // [24:27] 标记是否为叶子节点（header 已扩展到 28 字节，不再与 slot 重叠）
    GetData()[24] = is_leaf ? 1 : 0;
}

// ============================================================
// 基础访问器
// ============================================================

bool BPlusTreePage::IsLeaf() {
    return GetData()[24] == 1;
}

void BPlusTreePage::SetLeaf(bool is_leaf) {
    GetData()[24] = is_leaf ? 1 : 0;
}

page_id_t BPlusTreePage::GetPageId() {
    return *reinterpret_cast<const page_id_t*>(GetData());
}

page_id_t BPlusTreePage::GetPrevPageId() {
    return *reinterpret_cast<const page_id_t*>(GetData() + 8);
}

page_id_t BPlusTreePage::GetNextPageId() {
    return *reinterpret_cast<const page_id_t*>(GetData() + 12);
}

void BPlusTreePage::SetPrevPageId(page_id_t prev_id) {
    std::memcpy(GetData() + 8, &prev_id, 4);
}

void BPlusTreePage::SetNextPageId(page_id_t next_id) {
    std::memcpy(GetData() + 12, &next_id, 4);
}

uint32_t BPlusTreePage::GetSlotCount() {
    return *reinterpret_cast<const uint32_t*>(GetData() + 20);
}

// ============================================================
// 内部工具方法
// ============================================================

uint32_t BPlusTreePage::GetFreeSpacePointer() {
    return *reinterpret_cast<const uint32_t*>(GetData() + 16);
}

void BPlusTreePage::SetFreeSpacePointer(uint32_t ptr) {
    std::memcpy(GetData() + 16, &ptr, 4);
}

void BPlusTreePage::SetSlotCount(uint32_t count) {
    std::memcpy(GetData() + 20, &count, 4);
}

uint32_t BPlusTreePage::GetSlotOffset(uint32_t slot_idx) {
    return *reinterpret_cast<const uint32_t*>(GetData() + SIZE_PAGE_HEADER + slot_idx * SIZE_SLOT);
}

uint32_t BPlusTreePage::GetSlotSize(uint32_t slot_idx) {
    return *reinterpret_cast<const uint32_t*>(GetData() + SIZE_PAGE_HEADER + slot_idx * SIZE_SLOT + 4);
}

void BPlusTreePage::SetSlotOffset(uint32_t slot_idx, uint32_t offset) {
    std::memcpy(GetData() + SIZE_PAGE_HEADER + slot_idx * SIZE_SLOT, &offset, 4);
}

void BPlusTreePage::SetSlotSize(uint32_t slot_idx, uint32_t size) {
    std::memcpy(GetData() + SIZE_PAGE_HEADER + slot_idx * SIZE_SLOT + 4, &size, 4);
}

// ============================================================
// 数据序列化大小计算
// ============================================================

// 叶子节点数据大小: key_len(4) + key + tuple_size(4) + tuple_data
static uint32_t LeafDataSize(const std::string& key, const Tuple& value) {
    return sizeof(uint32_t) + static_cast<uint32_t>(key.size())
         + sizeof(uint32_t) + value.GetSize();
}

// 内部节点数据大小: child_id(4) + key_len(4) + key
static uint32_t InternalDataSize(const std::string& key) {
    return sizeof(page_id_t) + sizeof(uint32_t) + static_cast<uint32_t>(key.size());
}

// ============================================================
// 插入操作
// ============================================================

bool BPlusTreePage::InsertSlot(uint32_t pos, const std::string& key, const Tuple& value, bool force) {
    uint32_t data_size = LeafDataSize(key, value);
    uint32_t slot_count = GetSlotCount();

    // 检查空间是否够用
    // force 模式下允许槽位区与数据区暂时重叠（供分裂时用），但必须防止下溢
    if (GetFreeSpacePointer() < data_size) {
        return false; // 连一个数据项都放不下，防止 uint32_t 下溢
    }
    if (!force) {
        uint32_t required = SIZE_PAGE_HEADER + (slot_count + 1) * SIZE_SLOT + data_size;
        if (GetFreeSpacePointer() < required) {
            return false; // 页面已满
        }
    }

    // 1. 将 pos 及之后的槽位整体后移一个位置
    for (int i = static_cast<int>(slot_count) - 1; i >= static_cast<int>(pos); --i) {
        uint32_t off = GetSlotOffset(i);
        uint32_t sz = GetSlotSize(i);
        SetSlotOffset(i + 1, off);
        SetSlotSize(i + 1, sz);
    }

    // 2. 写入数据到空闲区末尾
    uint32_t new_offset = GetFreeSpacePointer() - data_size;
    SetFreeSpacePointer(new_offset);

    char* dest = GetData() + new_offset;
    // 写入 key_len + key
    uint32_t key_len = static_cast<uint32_t>(key.size());
    std::memcpy(dest, &key_len, sizeof(uint32_t));
    dest += sizeof(uint32_t);
    std::memcpy(dest, key.data(), key_len);
    dest += key_len;
    // 写入 tuple_size + tuple_data
    uint32_t tuple_size = value.GetSize();
    std::memcpy(dest, &tuple_size, sizeof(uint32_t));
    dest += sizeof(uint32_t);
    value.SerializeTo(dest);

    // 3. 记录槽位
    SetSlotOffset(pos, new_offset);
    SetSlotSize(pos, data_size);
    SetSlotCount(slot_count + 1);

    return true;
}

bool BPlusTreePage::InsertKeyAt(uint32_t pos, const std::string& key, page_id_t child_id, bool force) {
    uint32_t data_size = InternalDataSize(key);
    uint32_t slot_count = GetSlotCount();

    // 检查空间
    // force 模式下允许槽位区与数据区暂时重叠（供分裂时用），但必须防止下溢
    if (GetFreeSpacePointer() < data_size) {
        return false; // 连一个数据项都放不下，防止 uint32_t 下溢
    }
    if (!force) {
        uint32_t required = SIZE_PAGE_HEADER + (slot_count + 1) * SIZE_SLOT + data_size;
        if (GetFreeSpacePointer() < required) {
            return false;
        }
    }

    // 1. 后移槽位
    for (int i = static_cast<int>(slot_count) - 1; i >= static_cast<int>(pos); --i) {
        uint32_t off = GetSlotOffset(i);
        uint32_t sz = GetSlotSize(i);
        SetSlotOffset(i + 1, off);
        SetSlotSize(i + 1, sz);
    }

    // 2. 写入数据：child_id + key_len + key
    uint32_t new_offset = GetFreeSpacePointer() - data_size;
    SetFreeSpacePointer(new_offset);

    char* dest = GetData() + new_offset;
    std::memcpy(dest, &child_id, sizeof(page_id_t));
    dest += sizeof(page_id_t);
    uint32_t key_len = static_cast<uint32_t>(key.size());
    std::memcpy(dest, &key_len, sizeof(uint32_t));
    dest += sizeof(uint32_t);
    std::memcpy(dest, key.data(), key_len);

    // 3. 记录槽位
    SetSlotOffset(pos, new_offset);
    SetSlotSize(pos, data_size);
    SetSlotCount(slot_count + 1);

    return true;
}

// ============================================================
// 读取操作
// ============================================================

std::string BPlusTreePage::GetKey(uint32_t slot_idx) {
    uint32_t offset = GetSlotOffset(slot_idx);
    const char* src = GetData() + offset;

    if (IsLeaf()) {
        // 叶子节点格式: key_len + key + tuple_size + tuple_data
        uint32_t key_len = *reinterpret_cast<const uint32_t*>(src);
        // 安全检查：key_len 不应超过页面大小，防止读取损坏数据导致溢出
        if (key_len > PAGE_SIZE || offset + sizeof(uint32_t) + key_len > PAGE_SIZE) {
            return "";
        }
        return std::string(src + sizeof(uint32_t), key_len);
    } else {
        // 内部节点格式: child_id + key_len + key
        uint32_t key_len = *reinterpret_cast<const uint32_t*>(src + sizeof(page_id_t));
        // 安全检查
        if (key_len > PAGE_SIZE || offset + sizeof(page_id_t) + sizeof(uint32_t) + key_len > PAGE_SIZE) {
            return "";
        }
        return std::string(src + sizeof(page_id_t) + sizeof(uint32_t), key_len);
    }
}

Tuple BPlusTreePage::GetValue(uint32_t slot_idx) {
    uint32_t offset = GetSlotOffset(slot_idx);
    const char* src = GetData() + offset;

    // 叶子节点格式: key_len + key + tuple_size + tuple_data
    uint32_t key_len = *reinterpret_cast<const uint32_t*>(src);
    src += sizeof(uint32_t) + key_len;
    uint32_t tuple_size = *reinterpret_cast<const uint32_t*>(src);
    src += sizeof(uint32_t);

    return Tuple(src, tuple_size);
}

page_id_t BPlusTreePage::GetChildId(uint32_t slot_idx) {
    uint32_t offset = GetSlotOffset(slot_idx);
    // 内部节点格式: child_id + key_len + key
    return *reinterpret_cast<const page_id_t*>(GetData() + offset);
}

// ============================================================
// 分裂相关
// ============================================================

std::string BPlusTreePage::GetSplitKey() {
    uint32_t count = GetSlotCount();
    // 分裂键为中间位置的 key
    uint32_t mid = count / 2;
    return GetKey(mid);
}

void BPlusTreePage::MoveHalfTo(BPlusTreePage* target) {
    uint32_t count = GetSlotCount();
    uint32_t mid = count / 2;

    // 将 [mid, count) 的所有槽位移动到 target
    for (uint32_t i = mid; i < count; ++i) {
        std::string key = GetKey(i);
        if (IsLeaf()) {
            Tuple val = GetValue(i);
            target->InsertSlot(target->GetSlotCount(), key, val);
        } else {
            page_id_t child = GetChildId(i);
            target->InsertKeyAt(target->GetSlotCount(), key, child);
        }
    }

    // 如果是内部节点，把 target 的 prev_page_id 设为 child[mid]
    // 注意：slot[i] 存储 child[i+1]，所以 child[mid] = GetChildId(mid-1)（若 mid>0）或 GetPrevPageId()（若 mid==0）
    if (!IsLeaf()) {
        page_id_t first_child;
        if (mid > 0) {
            first_child = GetChildId(mid - 1);  // child[mid]
        } else {
            first_child = GetPrevPageId();       // child[0]
        }
        target->SetPrevPageId(first_child);
    }

    // 本页截断：减少 slot count，并回收空间
    SetSlotCount(mid);
    // 计算新的空闲指针（最后一个保留的 slot 的 offset）
    if (mid > 0) {
        // 遍历所有保留槽位，找到最小偏移量作为新的空闲指针
        // （因为非顺序插入可能导致偏移量不按槽位索引排序）
        uint32_t min_offset = GetSlotOffset(0);
        for (uint32_t i = 1; i < mid; ++i) {
            uint32_t off = GetSlotOffset(i);
            if (off < min_offset) min_offset = off;
        }
        SetFreeSpacePointer(min_offset);
    } else {
        SetFreeSpacePointer(PAGE_SIZE);
    }

    // 叶子节点：维护兄弟链表
    if (IsLeaf()) {
        target->SetNextPageId(GetNextPageId());
        target->SetPrevPageId(GetPageId());
        SetNextPageId(target->GetPageId());
    }
}

// ============================================================
// 标记删除
// ============================================================

void BPlusTreePage::MarkSlotDeleted(uint32_t slot_idx) {
    // 将槽位 size 设为 0 表示已删除
    SetSlotSize(slot_idx, 0);
}

bool BPlusTreePage::IsSlotDeleted(uint32_t slot_idx) {
    return GetSlotSize(slot_idx) == 0;
}

// ============================================================
// 二分查找
// ============================================================

int BPlusTreePage::FindInsertPos(const std::string& key) {
    uint32_t count = GetSlotCount();
    if (count == 0) return 0;

    int lo = 0, hi = static_cast<int>(count) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (GetKey(mid) < key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return lo; // 首个 >= key 的位置
}

int BPlusTreePage::FindKey(const std::string& key) {
    uint32_t count = GetSlotCount();
    int lo = 0, hi = static_cast<int>(count) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        // 注意：GetKey 返回值类型，不要用 const& 绑定到临时对象（悬垂引用）
        std::string mid_key = GetKey(mid);
        if (mid_key < key) {
            lo = mid + 1;
        } else if (mid_key > key) {
            hi = mid - 1;
        } else {
            return mid; // 精确匹配
        }
    }
    return -1; // 未找到
}

} // namespace db
