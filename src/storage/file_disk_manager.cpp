#include "storage/file_disk_manager.h"
#include <iostream>
#include <cstring>

namespace db {

FileDiskManager::FileDiskManager(const std::string& db_file) : file_name_(db_file) {
    // 尝试以多模式打开文件 (如果没有该文件，则利用 app 模式创建，否则原样打开)
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    // 回退：文件不存在则以写入模式创建（与 FileLogManager 一致）
    if (!db_io_.is_open()) {
        db_io_.clear();
        db_io_.open(db_file, std::ios::binary | std::ios::out);
        db_io_.close();
        db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    }
    db_io_.close();

    // 重新用读写方式打开
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);

    if (!db_io_.is_open()) {
        std::cerr << "无法打开文件 " << db_file << std::endl;
        throw std::runtime_error("DiskManager error: Cannot open db file.");
    }

    // 根据文件现有多大，推算当前应该从哪个 page_id 开始分配新页
    int file_size = GetFileSize(db_file);
    next_page_id_ = file_size / PAGE_SIZE; 
}

FileDiskManager::~FileDiskManager() {
    if (db_io_.is_open()) {
        db_io_.close();
    }
}

void FileDiskManager::ReadPage(page_id_t page_id, char* page_data) {
    std::lock_guard<std::mutex> lock(db_io_latch_);

    // 偏移量
    int offset = page_id * PAGE_SIZE;

    // 获取文件当前大小，避免越界访问
    db_io_.seekg(0, std::ios::end);
    int current_size = db_io_.tellg();
    if (offset >= current_size) {
        // 如果文件太小或者越界，先用 0 填充
        std::memset(page_data, 0, PAGE_SIZE);
        return;
    }

    // 定位到偏移处，并读取一页
    db_io_.seekg(offset, std::ios::beg);
    db_io_.read(page_data, PAGE_SIZE);

    // 如果读取由于某些原因不完整
    if (db_io_.bad() || db_io_.fail()) {
        db_io_.clear();
    }
}

void FileDiskManager::WritePage(page_id_t page_id, const char* page_data) {
    std::lock_guard<std::mutex> lock(db_io_latch_);

    int offset = page_id * PAGE_SIZE;

    // 清空状态位，定位并强行写入
    db_io_.clear();
    db_io_.seekp(offset, std::ios::beg);
    db_io_.write(page_data, PAGE_SIZE);

    // 如果系统崩溃，flush 确保落盘，虽然开销较大但比较安全
    db_io_.flush(); 
}

page_id_t FileDiskManager::AllocatePage() {
    // 加1再返回当前没加之前的值，保证线程安全分配
    page_id_t new_page_id = next_page_id_++; 

    // 初始化这一页，往磁盘补齐空间
    char empty_data[PAGE_SIZE] = {0};
    WritePage(new_page_id, empty_data);

    return new_page_id;
}

void FileDiskManager::DeallocatePage(page_id_t page_id) {
    // 简化实现：将页面数据清零（标记为可回收）
    char empty_data[PAGE_SIZE] = {0};
    WritePage(page_id, empty_data);
}

int FileDiskManager::GetFileSize(const std::string& file_name) {
    // 用纯 fstream 获取文件大小，避免 std::filesystem 在 Windows 上抛 system_error
    std::ifstream in(file_name, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return 0;
    auto size = in.tellg();
    return static_cast<int>(size);
}

} // namespace db
