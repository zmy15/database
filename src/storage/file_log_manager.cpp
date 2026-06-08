#include "storage/file_log_manager.h"
#include <cstring>
#include <iostream>

namespace db {

FileLogManager::FileLogManager(const std::string& wal_file_path)
    : wal_file_path_(wal_file_path) {
    write_buffer_.reserve(kWriteBufferSize);

    // 打开 WAL 文件（二进制模式，读写，文件不存在时创建）
    wal_file_.open(wal_file_path_,
                   std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    if (!wal_file_.is_open()) {
        // 文件可能不存在，先创建
        wal_file_.clear();
        wal_file_.open(wal_file_path_,
                       std::ios::binary | std::ios::out);
        wal_file_.close();
        wal_file_.open(wal_file_path_,
                       std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    }

    // 读取已有的 WAL 文件，恢复 next_lsn_ 和 flushed_lsn_
    // 简化实现：从头扫描文件，找到最后一条记录的 LSN
    wal_file_.seekg(0, std::ios::end);
    std::streamoff file_size = wal_file_.tellg();
    if (file_size > 0) {
        // 读取最后一条记录的 LSN
        // 记录格式末尾有 record_size (4B)，倒数第 5-8 字节是 record_size
        wal_file_.seekg(-4, std::ios::end);
        uint32_t last_record_size = 0;
        wal_file_.read(reinterpret_cast<char*>(&last_record_size), sizeof(uint32_t));
        if (wal_file_.good() && last_record_size > 0 && last_record_size <= static_cast<uint32_t>(file_size)) {
            // 跳到该记录开头读 LSN
            wal_file_.seekg(-static_cast<std::streamoff>(last_record_size), std::ios::end);
            lsn_t last_lsn = -1;
            wal_file_.read(reinterpret_cast<char*>(&last_lsn), sizeof(lsn_t));
            if (wal_file_.good()) {
                next_lsn_ = last_lsn + 1;
                flushed_lsn_ = last_lsn; // 假设文件中的数据都已刷盘
            }
        }
    }

    wal_file_.seekp(0, std::ios::end); // 写指针移到末尾
}

FileLogManager::~FileLogManager() {
    FlushLogs();
    if (wal_file_.is_open()) {
        wal_file_.close();
    }
}

lsn_t FileLogManager::AppendLogRecord(const LogRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);

    lsn_t lsn = next_lsn_++;

    // 构建带 LSN 的记录副本
    LogRecord r = record;
    r.lsn = lsn;

    WriteRecordToFile(r);
    return lsn;
}

void FileLogManager::FlushLogs() {
    std::lock_guard<std::mutex> lock(mutex_);
    FlushLogsInternal();
}

lsn_t FileLogManager::GetFlushedLSN() const {
    return flushed_lsn_;
}

lsn_t FileLogManager::GetNextLSN() const {
    return next_lsn_;
}

void FileLogManager::WriteRecordToFile(const LogRecord& record) {
    // 序列化格式：
    // [lsn (4B)] [txn_id (4B)] [op_type (1B)] [page_id (4B)] [slot_num (4B)]
    // [prev_lsn (4B)]
    // [old_tuple: size (4B) + data]
    // [new_tuple: size (4B) + data]
    // [record_size (4B)]

    uint32_t old_tuple_size = record.old_tuple.GetSize();
    uint32_t new_tuple_size = record.new_tuple.GetSize();

    // 动态分配序列化缓冲区（old/new tuple 需要临时序列化）
    std::vector<char> old_buf(old_tuple_size);
    std::vector<char> new_buf(new_tuple_size);
    if (old_tuple_size > 0) {
        record.old_tuple.SerializeTo(old_buf.data());
    }
    if (new_tuple_size > 0) {
        record.new_tuple.SerializeTo(new_buf.data());
    }

    // 计算整条记录大小
    uint32_t record_size = sizeof(lsn_t) + sizeof(txn_id_t) + sizeof(uint8_t)
                         + sizeof(page_id_t) + sizeof(uint32_t) + sizeof(lsn_t)
                         + sizeof(uint32_t) + old_tuple_size
                         + sizeof(uint32_t) + new_tuple_size
                         + sizeof(uint32_t); // record_size 自身

    // 辅助写入函数：追加到 write_buffer_，满了自动刷盘
    auto append_bytes = [this](const void* data, size_t size) {
        const char* ptr = static_cast<const char*>(data);
        while (size > 0) {
            size_t space = kWriteBufferSize - write_buffer_.size();
            size_t to_write = (size < space) ? size : space;
            write_buffer_.insert(write_buffer_.end(), ptr, ptr + to_write);
            ptr += to_write;
            size -= to_write;
            if (write_buffer_.size() >= kWriteBufferSize) {
                wal_file_.write(write_buffer_.data(), write_buffer_.size());
                write_buffer_.clear();
            }
        }
    };

    uint8_t op_byte = static_cast<uint8_t>(record.op_type);

    append_bytes(&record.lsn, sizeof(lsn_t));
    append_bytes(&record.txn_id, sizeof(txn_id_t));
    append_bytes(&op_byte, sizeof(uint8_t));
    append_bytes(&record.page_id, sizeof(page_id_t));
    append_bytes(&record.slot_num, sizeof(uint32_t));
    append_bytes(&record.prev_lsn, sizeof(lsn_t));

    append_bytes(&old_tuple_size, sizeof(uint32_t));
    if (old_tuple_size > 0) {
        append_bytes(old_buf.data(), old_tuple_size);
    }

    append_bytes(&new_tuple_size, sizeof(uint32_t));
    if (new_tuple_size > 0) {
        append_bytes(new_buf.data(), new_tuple_size);
    }

    append_bytes(&record_size, sizeof(uint32_t));
}

std::vector<LogRecord> FileLogManager::ReadLogRecords() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogRecord> records;

    // 保存当前读位置
    auto old_pos = wal_file_.tellg();

    // 从头扫描 WAL 文件
    wal_file_.seekg(0, std::ios::beg);
    wal_file_.clear();

    while (wal_file_.good() && wal_file_.peek() != EOF) {
        auto record = ReadSingleRecord();
        if (record.has_value()) {
            records.push_back(std::move(record.value()));
        } else {
            break; // 读取失败，可能到达文件末尾
        }
    }

    // 恢复读位置
    wal_file_.clear();
    wal_file_.seekg(old_pos);

    return records;
}

std::optional<LogRecord> FileLogManager::ReadSingleRecord() {
    // 记录格式：
    // [lsn (4B)] [txn_id (4B)] [op_type (1B)] [page_id (4B)] [slot_num (4B)]
    // [prev_lsn (4B)]
    // [old_tuple_size (4B)] [old_tuple_data...]
    // [new_tuple_size (4B)] [new_tuple_data...]
    // [record_size (4B)]

    LogRecord rec;

    // 读取固定头部
    uint8_t op_byte = 0;
    wal_file_.read(reinterpret_cast<char*>(&rec.lsn), sizeof(lsn_t));
    wal_file_.read(reinterpret_cast<char*>(&rec.txn_id), sizeof(txn_id_t));
    wal_file_.read(reinterpret_cast<char*>(&op_byte), sizeof(uint8_t));
    wal_file_.read(reinterpret_cast<char*>(&rec.page_id), sizeof(page_id_t));
    wal_file_.read(reinterpret_cast<char*>(&rec.slot_num), sizeof(uint32_t));
    wal_file_.read(reinterpret_cast<char*>(&rec.prev_lsn), sizeof(lsn_t));

    if (!wal_file_.good()) return std::nullopt;

    rec.op_type = static_cast<LogOpType>(op_byte);

    // 读取 old_tuple
    uint32_t old_size = 0;
    wal_file_.read(reinterpret_cast<char*>(&old_size), sizeof(uint32_t));
    if (!wal_file_.good()) return std::nullopt;

    if (old_size > 0) {
        std::vector<char> old_data(old_size);
        wal_file_.read(old_data.data(), old_size);
        if (!wal_file_.good()) return std::nullopt;
        rec.old_tuple = Tuple(old_data.data(), old_size);
    }

    // 读取 new_tuple
    uint32_t new_size = 0;
    wal_file_.read(reinterpret_cast<char*>(&new_size), sizeof(uint32_t));
    if (!wal_file_.good()) return std::nullopt;

    if (new_size > 0) {
        std::vector<char> new_data(new_size);
        wal_file_.read(new_data.data(), new_size);
        if (!wal_file_.good()) return std::nullopt;
        rec.new_tuple = Tuple(new_data.data(), new_size);
    }

    // 读取尾部 record_size
    uint32_t record_size = 0;
    wal_file_.read(reinterpret_cast<char*>(&record_size), sizeof(uint32_t));
    if (!wal_file_.good()) return std::nullopt;

    return rec;
}

void FileLogManager::FlushLogsInternal() {
    // 将写缓冲区刷入文件
    if (!write_buffer_.empty()) {
        wal_file_.write(write_buffer_.data(), write_buffer_.size());
        write_buffer_.clear();
    }

    // fsync：强制将文件数据写入磁盘
    wal_file_.flush();

    // 更新 flushed_lsn_（已刷盘的最后一条 LSN）
    if (next_lsn_ > 0) {
        flushed_lsn_ = next_lsn_ - 1;
    }
}

void FileLogManager::TruncateAfter(lsn_t target_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 先刷盘确保数据一致
    FlushLogsInternal();

    // 找到 target_lsn 之后的位置并截断
    // 简化实现：重新打开文件，截断到目标位置
    wal_file_.close();

    // 以读写模式重新打开，找到截断点
    wal_file_.open(wal_file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!wal_file_.is_open()) return;

    // 扫描找到 target_lsn 对应的记录末尾
    wal_file_.seekg(0, std::ios::beg);
    lsn_t last_kept_lsn = -1;
    std::streamoff truncate_pos = 0;

    while (wal_file_.good() && wal_file_.peek() != EOF) {
        auto start_pos = wal_file_.tellg();
        auto record = ReadSingleRecord();
        if (!record.has_value()) break;

        auto end_pos = wal_file_.tellg();

        if (record->lsn <= target_lsn) {
            last_kept_lsn = record->lsn;
            truncate_pos = end_pos;
        } else {
            break;
        }
    }

    wal_file_.clear();

    if (truncate_pos > 0 && last_kept_lsn >= 0) {
        // 使用平台相关方式截断文件
        // 关闭文件，用 ofstream 以 trunc 模式重写
        wal_file_.close();

        // 读取截断点之前的内容
        std::ifstream in_file(wal_file_path_, std::ios::binary);
        std::vector<char> kept_data(static_cast<size_t>(truncate_pos));
        in_file.read(kept_data.data(), truncate_pos);
        in_file.close();

        // 重写文件
        std::ofstream out_file(wal_file_path_, std::ios::binary | std::ios::trunc);
        out_file.write(kept_data.data(), truncate_pos);
        out_file.close();

        // 重新打开
        wal_file_.open(wal_file_path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    }

    // 更新内部状态
    if (next_lsn_ > target_lsn + 1) {
        next_lsn_ = target_lsn + 1;
    }
    if (flushed_lsn_ > target_lsn) {
        flushed_lsn_ = target_lsn;
    }
}

void FileLogManager::OpenWALFile() {
    wal_file_.open(wal_file_path_,
                   std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    if (!wal_file_.is_open()) {
        wal_file_.clear();
        wal_file_.open(wal_file_path_, std::ios::binary | std::ios::out);
        wal_file_.close();
        wal_file_.open(wal_file_path_,
                       std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    }
}

} // namespace db
