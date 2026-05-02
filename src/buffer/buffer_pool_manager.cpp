#include "buffer/buffer_pool_manager.h"
#include <iostream>
#include <cstring>

namespace db {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager, LogManager* log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager) {

    // 预先分配一块足够大的连续内存存放这些 Page
    pages_ = new Page[pool_size_];

    // 初始化 LRU 淘汰器
    replacer_ = std::make_unique<LRUReplacer>(pool_size);

    // 一开始，所有的内存槽（frame）都是空闲的
    for (size_t i = 0; i < pool_size_; ++i) {
        free_list_.push_back(i);
    }
}
BufferPoolManager::~BufferPoolManager() {
    Destroy();
}
void BufferPoolManager::Destroy() {
    // 销毁前把所有脏页都先刷盘
    if (pages_ != nullptr) {
        for (size_t i = 0; i < pool_size_; ++i) {
            if (pages_[i].is_dirty_ && pages_[i].page_id_ != -1) {
                disk_manager_->WritePage(pages_[i].page_id_, pages_[i].data_);
            }
        }
        delete[] pages_;
        pages_ = nullptr;
    }
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end() || page_id == -1) {
        return false;
    }

    int frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 如果启用了 WAL（日志管理），这里必须在刷入数据前确保日志也落盘
    // if (log_manager_ && page->GetLSN() > log_manager_->GetFlushedLSN()) { log_manager_->Flush(); }

    disk_manager_->WritePage(page->page_id_, page->data_);
    page->is_dirty_ = false;
    return true;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    // 1. 如果该页已经在内存中，直接返回
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        int frame_id = it->second;
        Page* page = &pages_[frame_id];
        page->pin_count_++;
        replacer_->Pin(frame_id); // 既然借出去了，就不能被淘汰
        return page;
    }

    // 2. 如果不在内存中，我们需要找一个空的内存槽(frame)
    int frame_id = -1;
    if (!free_list_.empty()) {
        // 还有没被填过的全新槽位，直接用
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else {
        // 所有的槽位都用过了，请求 LRU 强制征用（淘汰）一个不活跃的也没被锁定的槽
        if (!replacer_->Victim(&frame_id)) {
            // Buffer pool 全部塞满了而且由于都在借用状态没有一个能替出去的...
            return nullptr; 
        }

        // 拿下淘汰的槽位，如果是脏页则要先写入磁盘！
        Page* old_page = &pages_[frame_id];
        if (old_page->is_dirty_) {
            disk_manager_->WritePage(old_page->page_id_, old_page->data_);
        }
        // 从页表中删除旧记录
        page_table_.erase(old_page->page_id_);
    }

    // 3. 开始将新页载入这个腾出来的 frame
    Page* page = &pages_[frame_id];
    page->page_id_ = page_id;
    page->pin_count_ = 1;      // 被当前线程使用
    page->is_dirty_ = false;   // 刚读出来的全新状态
    disk_manager_->ReadPage(page_id, page->data_); // 从磁盘加载数据

    // 4. 更新页表与替换器记录
    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id); // 因为马上要在外面使用，不能被淘汰

    return page;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    int frame_id = it->second;
    Page* page = &pages_[frame_id];

    if (page->pin_count_ <= 0) {
        return false;
    }

    // 合并修改状态（如果是脏页绝对不能丢失）
    if (is_dirty) {
        page->is_dirty_ = true;
    }

    page->pin_count_--;
    // 如果没有人在用了，就可以交给 LRU 候补排队了
    if (page->pin_count_ == 0) {
        replacer_->Unpin(frame_id); 
    }

    return true;
}

Page* BufferPoolManager::NewPage(page_id_t* page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    int frame_id = -1;

    // 获取可用的空闲槽（要么是本来就没用的，要么淘汰一个出来）
    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else {
        if (!replacer_->Victim(&frame_id)) {
            return nullptr; 
        }
        Page* old_page = &pages_[frame_id];
        if (old_page->is_dirty_) {
            disk_manager_->WritePage(old_page->page_id_, old_page->data_);
        }
        page_table_.erase(old_page->page_id_);
    }

    // 呼叫 DiskManager 分配一个全新的页 ID
    *page_id = disk_manager_->AllocatePage();

    // 初始化该槽位
    Page* page = &pages_[frame_id];
    page->page_id_ = *page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = true; // 刚创建就被认为是脏的，需要写盘一次
    std::memset(page->data_, 0, PAGE_SIZE); // 清空遗留的内存数据

    // 记录
    page_table_[*page_id] = frame_id;
    replacer_->Pin(frame_id);

    return page;
}

} // namespace db
