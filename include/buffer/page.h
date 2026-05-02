#pragma once

#include "common/config.h"
#include <shared_mutex>

namespace db {

// 内存中的数据页
class Page {
public:
    void RLock() { rwlatch_.lock_shared(); }
    void RUnlock() { rwlatch_.unlock_shared(); }
    void WLock() { rwlatch_.lock(); }
    void WUnlock() { rwlatch_.unlock(); }

    inline char* GetData() { return data_; }
    inline page_id_t GetPageId() const { return page_id_; }
    inline lsn_t GetLSN() const { return lsn_; }
    inline void SetLSN(lsn_t lsn) { lsn_ = lsn; }

private:
    friend class BufferPoolManager;
    char data_[PAGE_SIZE]{};     // 页内实际数据
    page_id_t page_id_ = -1;     // 对应的磁盘页ID
    int pin_count_ = 0;          // 被引用的次数（大于0说明有线程在用，不能被淘汰）
    bool is_dirty_ = false;      // 是否被修改过
    std::shared_mutex rwlatch_;  // 页级读写锁
    lsn_t lsn_ = 0;              // 该页最后一次修改对应的日志LSN（用于崩溃恢复）
};

} // namespace db
