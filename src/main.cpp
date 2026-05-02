#include "db_engine.h"
#include "storage/file_disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "storage/table_heap.h"
#include "storage/table_iterator.h"
#include <iostream>
#include <string>

using namespace db;

int main() {
    std::cout << "=== Database System Initializing ===" << std::endl;

    // 1. 初始化磁盘管理器与缓冲池 (我们给缓冲池设置较小容量，强迫它翻页)
    FileDiskManager disk_manager("test.db");
    BufferPoolManager bpm(5, &disk_manager);

    // ==========================================
    // 测试: TableHeap (表堆) 插入与大容量跨页测试
    // ==========================================
    std::cout << "\n=== [Test] TableHeap Bulk Insert ===" << std::endl;
    
    TableHeap table_heap(&bpm);
    table_heap.Init();

    // 我们向表中强行插入 800 条带有长文本数据的记录。
    // 这绝对会撑爆最初的 4KB 单一页面！TableHeap 应当自动向后开拓新的页面链表。
    for (int i = 0; i < 800; ++i) {
        std::vector<std::string> fields;
        fields.push_back("User_" + std::to_string(i));
        // 我们塞点长文本，保证 4KB 容量很快消耗光
        fields.push_back(std::string(100, 'X')); 
        
        Tuple t(fields);
        RID rid;
        table_heap.InsertTuple(t, &rid);
    }
    std::cout << "Successfully inserted 800 records into TableHeap." << std::endl;

    // ==========================================
    // 测试: TableIterator (游标) 全表顺序扫描
    // ==========================================
    std::cout << "\n=== [Test] SeqScan with TableIterator ===" << std::endl;
    
    int scan_count = 0;
    // 使用范围 for 循环和 C++ 迭代器
    for (auto it = table_heap.Begin(); it != table_heap.End(); ++it) {
        auto t = it.Get();
        if (t.has_value()) {
            scan_count++;
            // 我们只打印特定的几个来查看结果，避免刷屏
            if (scan_count == 1 || scan_count == 400 || scan_count == 800) {
                std::cout << "Scanned Record [" << scan_count << "]: " 
                          << t->GetValues()[0] << std::endl;
            }
        }
    }

    std::cout << "\nTotal scanned records count: " << scan_count << std::endl;
    std::cout << "=== Test Complete ===" << std::endl;
    return 0;
}
