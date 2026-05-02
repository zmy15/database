#pragma once

#include "common/config.h"
#include "buffer/page.h"
#include "buffer/lru_replacer.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <memory>

namespace db {

// 缓冲池管理器：负责在内存和磁盘间调度 Page (包含 LRU 淘汰算法)
class BufferPoolManager {
public:
    BufferPoolManager() = default;
    BufferPoolManager(size_t pool_size, DiskManager* disk_manager, LogManager* log_manager = nullptr);
    virtual ~BufferPoolManager();

    // 获取指定页，如果不在内存中，则从磁盘加载
    virtual Page* FetchPage(page_id_t page_id);
    // 线程用完后释放页的引用
    virtual bool UnpinPage(page_id_t page_id, bool is_dirty);
    // 创建新页
    virtual Page* NewPage(page_id_t* page_id);
    // 强制将脏页刷入磁盘 (刷盘前会调用 LogManager 确保 WAL 先落盘)
    virtual bool FlushPage(page_id_t page_id);

    void Destroy();

private:
    size_t pool_size_;                             // 缓冲池大小（页数）
    Page* pages_;                                  // 缓冲池中存放页的数组
    DiskManager* disk_manager_;                    // 磁盘管理器
    LogManager* log_manager_;                      // 日志管理器
    std::unordered_map<page_id_t, int> page_table_;// 页表：记录 page_id 映射到哪个 frame_id (pages_ 数组索引)
    std::list<int> free_list_;                     // 空闲帧列表（刚初始化还未被用过的槽位）
    std::unique_ptr<LRUReplacer> replacer_;        // LRU 替换器
    std::mutex latch_;                             // 保护缓冲池内部数据结构的并发安全
};

} // namespace db
