#pragma once

#include "common/config.h"
#include "common/rid.h"
#include <vector>
#include <string>

namespace db {

/**
 * @brief 在真实的数据库中，Tuple 应该是无模式的二进制数据。
 * 为了简化理解，这边我们强化 Tuple 的设计。
 * 此处的 Tuple 用于将内存中零散的数据序列化 (Serialize) 成一长串连续的字节，
 * 或将磁盘中读出的字节反序列化 (Deserialize) 为业务代码看得懂的结构。
 */
class Tuple {
public:
    Tuple() = default;

    // 从一组数值构建一条记录
    explicit Tuple(std::vector<std::string> values);

    // 将二进制直接加载还原为 Tuple
    Tuple(const char* data, uint32_t size);

    // 获取包含的数值记录
    const std::vector<std::string>& GetValues() const { return values_; }

    // 返回将这条记录转换成二进制数组后应该占用多少字节
    uint32_t GetSize() const;

    // 将其序列化后写到指定的内存地址（通常位于页面的 data_ 中）
    void SerializeTo(char* storage) const;

    // 定位标识
    RID GetRID() const { return rid_; }
    void SetRID(RID rid) { rid_ = rid; }

private:
    std::vector<std::string> values_;
    RID rid_;
    uint32_t size_{0}; // 序列化后该占用的字节数
};

} // namespace db
