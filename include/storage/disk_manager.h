#pragma once

#include "common/config.h"

namespace db {

// 磁盘管理器：负责直接在文件中读写定长 Page
class DiskManager {
public:
    virtual ~DiskManager() = default;
    virtual void ReadPage(page_id_t page_id, char* page_data) = 0;
    virtual void WritePage(page_id_t page_id, const char* page_data) = 0;
    virtual page_id_t AllocatePage() = 0; // 分配新页
};

} // namespace db
