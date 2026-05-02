#pragma once

#include "common/config.h"

namespace db {

/**
 * @brief RID (Record Identifier) 用于唯一定位一条记录的位置
 * 包含：该条记录所在的页号 (page_id) + 它在该页内的槽位号 (slot_num)
 */
class RID {
public:
    RID() = default;
    RID(page_id_t page_id, uint32_t slot_num) : page_id_(page_id), slot_num_(slot_num) {}

    inline page_id_t GetPageId() const { return page_id_; }
    inline uint32_t GetSlotNum() const { return slot_num_; }

    inline void Set(page_id_t page_id, uint32_t slot_num) {
        page_id_ = page_id;
        slot_num_ = slot_num;
    }

    bool operator==(const RID& other) const {
        return page_id_ == other.page_id_ && slot_num_ == other.slot_num_;
    }

private:
    page_id_t page_id_{INVALID_PAGE_ID};
    uint32_t slot_num_{0};
};

} // namespace db
