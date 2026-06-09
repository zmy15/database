#include "storage/tuple.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include <cstring>

namespace db {

Tuple::Tuple(std::vector<std::string> values) : values_(std::move(values)) {
    // 计算序列化后的大小：
    // 每条记录格式我们设定为: 
    // [xmin (4 byte)] [xmax (4 byte)] [字段数 (4 byte)] + [字段1长度 (4 byte)] [字段1内容] + ...
    xmin_ = 0;
    xmax_ = 0;
    size_ = sizeof(uint32_t) * 3; // xmin + xmax + field_count
    for (const auto& val : values_) {
        size_ += sizeof(uint32_t) + val.size();
    }
}

Tuple::Tuple(const char* data, uint32_t size) : size_(size) {
    uint32_t offset = 0;
    const uint32_t kMinHeader = sizeof(txn_id_t) * 2 + sizeof(uint32_t);  // xmin+xmax+field_count = 12B

    // 防御：数据不足最小头部，说明格式不兼容或数据损坏，视为空元组
    if (size < kMinHeader) {
        return;
    }

    // 读取 MVCC 头部：xmin (4B) + xmax (4B)
    xmin_ = *reinterpret_cast<const txn_id_t*>(data + offset);
    offset += sizeof(txn_id_t);
    xmax_ = *reinterpret_cast<const txn_id_t*>(data + offset);
    offset += sizeof(txn_id_t);

    // 读取字段数
    uint32_t field_count = *reinterpret_cast<const uint32_t*>(data + offset);
    offset += sizeof(uint32_t);

    // 防御：field_count 过大则拒绝（每个字段至少 4B val_size，绝不超过剩余字节/4）
    if (field_count > (size - offset) / sizeof(uint32_t)) {
        return;
    }

    // 依次读取每个字段
    for (uint32_t i = 0; i < field_count; ++i) {
        if (offset + sizeof(uint32_t) > size) break;
        uint32_t val_size = *reinterpret_cast<const uint32_t*>(data + offset);
        offset += sizeof(uint32_t);

        if (offset + val_size > size) break;
        std::string val(data + offset, val_size);
        values_.push_back(std::move(val));
        offset += val_size;
    }
}

uint32_t Tuple::GetSize() const {
    return size_;
}

void Tuple::SerializeTo(char* storage) const {
    uint32_t offset = 0;

    // 写入 MVCC 头部：xmin (4B) + xmax (4B)
    std::memcpy(storage + offset, &xmin_, sizeof(txn_id_t));
    offset += sizeof(txn_id_t);
    std::memcpy(storage + offset, &xmax_, sizeof(txn_id_t));
    offset += sizeof(txn_id_t);

    // 写入字段数
    uint32_t field_count = values_.size();
    std::memcpy(storage + offset, &field_count, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // 依次写入每个字段
    for (const auto& val : values_) {
        uint32_t val_size = val.size();
        std::memcpy(storage + offset, &val_size, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        std::memcpy(storage + offset, val.data(), val_size);
        offset += val_size;
    }
}

Tuple Tuple::Merge(const Tuple& left, const Tuple& right) {
    std::vector<std::string> merged;
    const auto& left_vals = left.GetValues();
    const auto& right_vals = right.GetValues();
    merged.reserve(left_vals.size() + right_vals.size());
    merged.insert(merged.end(), left_vals.begin(), left_vals.end());
    merged.insert(merged.end(), right_vals.begin(), right_vals.end());
    return Tuple(merged);
}

// ============================================================
// MVCC 可见性判断
// ============================================================

bool Tuple::IsVisible(const Tuple& tuple, txn_id_t reader_txn,
                      TransactionManager* txn_mgr, IsolationLevel iso_level) {
    (void)iso_level; // 当前 READ_COMMITTED/REPEATABLE_READ/SERIALIZABLE 共用相同逻辑
                     // TODO: REPEATABLE_READ 需基于快照 LSN 做更严格的可见性过滤

    txn_id_t xmin = tuple.GetXmin();
    txn_id_t xmax = tuple.GetXmax();

    // 规则 1：自己创建的，总是可见（INSERT 未提交也能看到）
    if (xmin == reader_txn) {
        return true;
    }

    // 规则 2：xmin == 0 表示系统数据（无归属事务），视为已提交
    if (xmin == 0) {
        // 仍需检查 xmax 是否标记了删除
        if (xmax == 0 || xmax == reader_txn) {
            return true;  // 未删除 或 被自己删除
        }
        if (txn_mgr && txn_mgr->IsAborted(xmax)) {
            return true;  // 删除者已回滚
        }
        return false;     // xmax 已提交 → 已被他人删除
    }

    // 规则 3：创建者未提交 → 不可见
    if (!txn_mgr || !txn_mgr->IsCommitted(xmin)) {
        return false;
    }

    // 规则 4：检查删除标记 xmax
    if (xmax == 0) {
        return true;  // 未被删除
    }
    if (xmax == reader_txn) {
        return true;  // 被自己删除（UNDO 需要仍可看到）
    }
    if (txn_mgr->IsAborted(xmax)) {
        return true;  // 删除者已回滚 → 删除无效
    }

    // xmax 已提交 → 被其他事务删除，不可见
    return false;
}

} // namespace db
