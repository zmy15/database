#pragma once

#include "buffer/page.h"
#include "storage/tuple.h"
#include <string>
#include <cstring>

namespace db {

/**
 * @brief B+ 树节点页格式（Slotted Page）
 *
 * 页面布局（与 TablePage 一致）：
 * ========================================================
 * | 28B Header | 槽位数组(从前往后) -> ... <- 数据区(从后往前) |
 * ========================================================
 *
 * Header (28 bytes):
 *   [0:3]   page_id
 *   [4:7]   lsn
 *   [8:11]  prev_page_id (内部节点用作 child[0]，叶子节点用作兄弟链表前驱)
 *   [12:15] next_page_id (叶子节点用作兄弟链表后继，内部节点保留)
 *   [16:19] free_space_pointer（数据区末尾，从 PAGE_SIZE 向低地址增长）
 *   [20:23] slot_count
 *   [24:27] is_leaf 标志（1 字节有效，其余 3 字节保留）
 *
 * 槽位 (每条 8 bytes): [offset(4B) | size(4B)]
 *
 * 数据区内容：
 *   内部节点: child_page_id(4B) + key_len(4B) + key_data(变长)
 *   叶子节点: key_len(4B) + key_data(变长) + tuple_size(4B) + tuple_data(变长)
 *
 * 内部节点约定：
 *   - prev_page_id 存储 child[0]（覆盖 < key[0] 的范围）
 *   - slot[i] 存储 (key[i], child[i+1])，覆盖 >= key[i] 的范围
 *   - N 个 key 对应 N+1 个子节点
 */
class BPlusTreePage : public Page {
public:
    // 标记删除（将槽位 size 设为 0）
    void MarkSlotDeleted(uint32_t slot_idx);
    bool IsSlotDeleted(uint32_t slot_idx);
    // 物理移除槽位（压缩槽位数组，释放一个槽位空间）
    bool RemoveSlot(uint32_t slot_idx);
    // 获取活跃（非删除）槽位数
    uint32_t GetActiveSlotCount();
    // 判断是否欠满（小于最小填充度，需触发合并或重分配）
    bool IsUnderfull();
    void Init(page_id_t page_id, bool is_leaf);

    bool IsLeaf();
    void SetLeaf(bool is_leaf);

    page_id_t GetPageId();
    page_id_t GetPrevPageId();
    page_id_t GetNextPageId();
    void SetPrevPageId(page_id_t prev_id);
    void SetNextPageId(page_id_t next_id);

    uint32_t GetSlotCount();

    // 在指定位置插入键值对（叶子节点用）
    // force=true 时跳过容量检查，供分裂时暂时超出容量使用
    bool InsertSlot(uint32_t pos, const std::string& key, const Tuple& value, bool force = false);
    // 在指定位置插入键+子节点引用（内部节点用）
    // force=true 时跳过容量检查，供分裂时暂时超出容量使用
    bool InsertKeyAt(uint32_t pos, const std::string& key, page_id_t child_id, bool force = false);

    // 读取槽位数据
    std::string GetKey(uint32_t slot_idx);
    Tuple GetValue(uint32_t slot_idx);
    page_id_t GetChildId(uint32_t slot_idx);

    // 分裂：将本页的一半数据移动到目标页
    void MoveHalfTo(BPlusTreePage* target);
    // 获取本页的分裂键（中间位置的 key）
    std::string GetSplitKey();

    // 二分查找
    int FindInsertPos(const std::string& key);  // 返回插入位置（首个 >= key 的位置）
    int FindKey(const std::string& key);         // 精确查找，返回索引或 -1

private:
    static constexpr size_t SIZE_PAGE_HEADER = 28;
    static constexpr size_t SIZE_SLOT = 8;

    uint32_t GetFreeSpacePointer();
    void SetFreeSpacePointer(uint32_t ptr);
    void SetSlotCount(uint32_t count);

    uint32_t GetSlotOffset(uint32_t slot_idx);
    uint32_t GetSlotSize(uint32_t slot_idx);
    void SetSlotOffset(uint32_t slot_idx, uint32_t offset);
    void SetSlotSize(uint32_t slot_idx, uint32_t size);
};

} // namespace db
