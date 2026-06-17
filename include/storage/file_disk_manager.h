#pragma once

#include "storage/disk_manager.h"
#include <fstream>
#include <string>
#include <atomic>
#include <mutex>

namespace db {

/**
 * @brief FileDiskManager 负责直接读写磁盘文件
 * 将磁盘文理视为一个无限长的字节数组，通过 page_id 定位偏移量
 */
class FileDiskManager : public DiskManager {
public:
    explicit FileDiskManager(const std::string& db_file);
    ~FileDiskManager() override;

    // 从磁盘读取一个指定的页到内存 (page_data 需要有 PAGE_SIZE 大小)
    void ReadPage(page_id_t page_id, char* page_data) override;

    // 将内存中的页 (page_data) 写入到磁盘中
    void WritePage(page_id_t page_id, const char* page_data) override;

    // 分配一个新的页ID
    page_id_t AllocatePage() override;
     void DeallocatePage(page_id_t page_id) override;

    // ========== Catalog 持久化接口 ==========
    // 检查 DB 文件是否包含 Catalog 头部（Magic + Version）
    bool HasCatalogHeader() const { return has_catalog_header_; }

    // 从 Catalog 页（page_id=0 偏移处）读取完整 4KB 数据
    void ReadCatalogPage(char* page_data);

    // 将 4KB 数据写入 Catalog 页（page_id=0 偏移处）
    void WriteCatalogPage(const char* page_data);

private:
    int GetFileSize(const std::string& file_name);

    std::string file_name_;
    std::fstream db_io_;                     // 本地文件流
    std::atomic<page_id_t> next_page_id_{0}; // 下一个可用的页ID
    std::mutex db_io_latch_;                 // 保护文件读写并发
    bool has_catalog_header_ = false;         // DB 文件是否包含 Catalog Magic+Version 头部
};

} // namespace db
