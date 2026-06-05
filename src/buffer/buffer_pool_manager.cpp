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

void BufferPoolManager::FlushAllPages() {
    std::lock_guard<std::mutex> lock(latch_);
    if (pages_ == nullptr) return;

    for (size_t i = 0; i < pool_size_; ++i) {
        if (pages_[i].is_dirty_ && pages_[i].page_id_ != INVALID_PAGE_ID) {
            if (log_manager_ && pages_[i].GetLSN() > log_manager_->GetFlushedLSN()) {
                log_manager_->FlushLogs();
            }
            disk_manager_->WritePage(pages_[i].page_id_, pages_[i].data_);
            pages_[i].is_dirty_ = false;
        }
    }
}

void BufferPoolManager::Destroy() {
    // 销毁前把所有脏页都先刷盘
    if (pages_ != nullptr) {
        for (size_t i = 0; i < pool_size_; ++i) {
            if (pages_[i].is_dirty_ && pages_[i].page_id_ != -1) {
                // WAL 先刷盘，确保日志在数据页之前落盘
                if (log_manager_ && pages_[i].GetLSN() > log_manager_->GetFlushedLSN()) {
                    log_manager_->FlushLogs();
                }
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

    // WAL 先刷盘：数据页落盘前，确保该页相关的 WAL 日志已持久化
    if (log_manager_ && page->GetLSN() > log_manager_->GetFlushedLSN()) {
        log_manager_->FlushLogs();
    }

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
        replacer_->Pin(frame_id);
        return page;
    }

    // 2. 如果不在内存中，我们需要找一个空的内存槽(frame)
    int frame_id = -1;
    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else {
        if (!replacer_->Victim(&frame_id)) {
            return nullptr;
        }

        Page* old_page = &pages_[frame_id];
        if (old_page->is_dirty_) {
            if (log_manager_ && old_page->GetLSN() > log_manager_->GetFlushedLSN()) {
                log_manager_->FlushLogs();
            }
            disk_manager_->WritePage(old_page->page_id_, old_page->data_);
        }
        page_table_.erase(old_page->page_id_);
    }

    // 3. 开始将新页载入这个腾出来的 frame
    Page* page = &pages_[frame_id];
    page->page_id_ = page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    disk_manager_->ReadPage(page_id, page->data_);

    // 4. 更新页表与替换器记录
    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id);

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

    if (is_dirty) {
        page->is_dirty_ = true;
    }

    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->Unpin(frame_id);
    }

    return true;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        // 页不在缓冲池中，直接从磁盘清零
        char empty_data[PAGE_SIZE] = {0};
        disk_manager_->WritePage(page_id, empty_data);
        return true;
    }

    int frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 清零页面数据
    std::memset(page->data_, 0, PAGE_SIZE);
    page->is_dirty_ = true;

    // 从页表中移除
    page_table_.erase(it);

    // 将 frame 放回空闲列表
    replacer_->Pin(frame_id); // 先从 LRU 中移除
    page->page_id_ = INVALID_PAGE_ID;
    page->pin_count_ = 0;
    free_list_.push_back(frame_id);

    // 同时清空磁盘上的页面
    disk_manager_->DeallocatePage(page_id);

    return true;
}

Page* BufferPoolManager::NewPage(page_id_t* page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    int frame_id = -1;

    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else {
        if (!replacer_->Victim(&frame_id)) {
            return nullptr;
        }
        Page* old_page = &pages_[frame_id];
        if (old_page->is_dirty_) {
            if (log_manager_ && old_page->GetLSN() > log_manager_->GetFlushedLSN()) {
                log_manager_->FlushLogs();
            }
            disk_manager_->WritePage(old_page->page_id_, old_page->data_);
        }
        page_table_.erase(old_page->page_id_);
    }

    *page_id = disk_manager_->AllocatePage();

    Page* page = &pages_[frame_id];
    page->page_id_ = *page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = true;
    std::memset(page->data_, 0, PAGE_SIZE);

    page_table_[*page_id] = frame_id;
    replacer_->Pin(frame_id);

    return page;
}

} // namespace db
