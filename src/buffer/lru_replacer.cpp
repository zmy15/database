#include "buffer/lru_replacer.h"

namespace db {

LRUReplacer::LRUReplacer(size_t num_pages) : max_size_(num_pages) {}

LRUReplacer::~LRUReplacer() = default;

bool LRUReplacer::Victim(int* frame_id) {
    std::lock_guard<std::mutex> lock(latch_);

    // 如果没有任何页面是 unpinned 状态（都没被放出，或缓冲池本就是空的）
    if (lru_list_.empty()) {
        return false;
    }

    // 链表的头部就是最久没人使用的
    *frame_id = lru_list_.front();

    // 移出队列
    lru_hash_.erase(*frame_id);
    lru_list_.pop_front();

    return true;
}

void LRUReplacer::Pin(int frame_id) {
    std::lock_guard<std::mutex> lock(latch_);

    // Pin 操作表示这个槽位被某个事务/操作拿去用了，不应该被替换。
    // 如果它在我们的备选淘汰名单里，我们需要把它划掉。
    auto it = lru_hash_.find(frame_id);
    if (it != lru_hash_.end()) {
        lru_list_.erase(it->second);
        lru_hash_.erase(it);
    }
}

void LRUReplacer::Unpin(int frame_id) {
    std::lock_guard<std::mutex> lock(latch_);

    // Unpin 表示槽位已经没在工作了。加入替换名单。
    // 如果已经在名单内，不用重复插入
    if (lru_hash_.find(frame_id) != lru_hash_.end()) {
        return;
    }

    // 插入到链表尾部（表示刚刚新鲜归还，最不容易被淘汰）
    lru_list_.push_back(frame_id);
    // 记录它的位置到哈希表，实现 O(1) 修改
    lru_hash_[frame_id] = std::prev(lru_list_.end());
}

size_t LRUReplacer::Size() {
    std::lock_guard<std::mutex> lock(latch_);
    return lru_list_.size();
}

} // namespace db
