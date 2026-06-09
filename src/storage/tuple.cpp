#include "storage/tuple.h"
#include <cstring>

namespace db {

Tuple::Tuple(std::vector<std::string> values) : values_(std::move(values)) {
    // 计算序列化后的大小：
    // 每条记录格式我们设定为: 
    // [字段数 (4 byte)] + [字段1长度 (4 byte)] [字段1内容] + [字段2长度 (4 byte)] [字段2内容]...
    size_ = sizeof(uint32_t); 
    for (const auto& val : values_) {
        size_ += sizeof(uint32_t) + val.size();
    }
}

Tuple::Tuple(const char* data, uint32_t size) : size_(size) {
    uint32_t offset = 0;

    // 读取字段数
    uint32_t field_count = *reinterpret_cast<const uint32_t*>(data + offset);
    offset += sizeof(uint32_t);

    // 依次读取每个字段
    for (uint32_t i = 0; i < field_count; ++i) {
        uint32_t val_size = *reinterpret_cast<const uint32_t*>(data + offset);
        offset += sizeof(uint32_t);

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

} // namespace db
