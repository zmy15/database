#pragma once

#include <list>
#include <mutex>
#include <unordered_map>

#include "common/config.h"

namespace db {

/**
 * @brief LRUReplacer 实现了经典的最近最少使用替换策略。
 * 它维护了当前哪些内存槽（frame_id）是未被使用的，可以在缓冲池满时被替换出去。
 */
class LRUReplacer {
public:
    explicit LRUReplacer(size_t num_pages);
    ~LRUReplacer();

    /**
     * @brief 尝试淘汰一个页面，将其 frame_id 写入 victim 中。
     * @param[out] frame_id 淘汰的内存槽 ID
     * @return 如果有可以淘汰的页面返回 true，否则返回 false（全都正在被使用中）
     */
    bool Victim(int* frame_id);

    /**
     * @brief 页面被某个线程借用了（固定在缓冲池中），应当从 LRU 的候选池中移除。
     * @param frame_id 被借用的内存槽 ID
     */
    void Pin(int frame_id);

    /**
     * @brief 页面被归还了，此时它可能随时被覆盖，加入到 LRU 队列中。
     * @param frame_id 归还的内存槽 ID
     */
    void Unpin(int frame_id);

    /**
     * @brief 返回当前在候选淘汰队列中有多少个页面槽
     */
    size_t Size();

private:
    std::mutex latch_;
    size_t max_size_;
    std::list<int> lru_list_; // 靠近 back() 的是最近归还的，front() 的是最久未用的
    std::unordered_map<int, std::list<int>::iterator> lru_hash_; 
};

} // namespace db
