#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace db {

// 1. 基础类型与常量定义 (Common Types)
using page_id_t = int32_t;     // 物理页ID
using txn_id_t = int32_t;      // 事务ID
using lsn_t = int32_t;         // 日志序列号 (Log Sequence Number)
constexpr int PAGE_SIZE = 4096; // 每页 4KB
constexpr page_id_t INVALID_PAGE_ID = -1; // 无效的页编号
constexpr int TRANSACTION_TIMEOUT_MS = 5000; // 死锁等待超时（毫秒）

// Catalog 持久化常量
constexpr uint32_t DB_CATALOG_MAGIC = 0x00434244;  // "DBC\0" in little-endian
constexpr uint32_t DB_CATALOG_VERSION = 2;
constexpr page_id_t CATALOG_PAGE_ID = 0;  // Catalog 使用 page_id=0 预留区域

} // namespace db
