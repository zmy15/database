# CMakeList.txt: database 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.10)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()

project ("database")

set(CMAKE_CXX_STANDARD 20)

# 主可执行文件
add_executable (database
    "src/main.cpp"
    "src/db_engine.cpp"
    "src/storage/file_log_manager.cpp"
    "src/execution/seq_scan_executor.cpp"
    "src/execution/filter_executor.cpp"
    "src/execution/projection_executor.cpp"
    "src/execution/aggregation_executor.cpp"
    "src/execution/index_scan_executor.cpp"
    "src/storage/file_disk_manager.cpp"
    "src/buffer/lru_replacer.cpp"
    "src/buffer/buffer_pool_manager.cpp"
    "src/storage/tuple.cpp"
    "src/storage/table_page.cpp"
    "src/storage/table_heap.cpp"
    "src/concurrency/lock_manager.cpp"
    "src/concurrency/transaction.cpp"
    "src/index/b_plus_tree.cpp"
    "src/index/b_plus_tree_page.cpp"
    "src/execution/planner.cpp"
)

include_directories(include)

# ============================================================
# Google Test 集成与测试目标
# ============================================================
include(FetchContent)
# 强制 Google Test 使用动态 CRT（MDd/MD），与项目保持一致，避免 LNK2038
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# 并发模块单元测试
add_executable(concurrency_test
    "test/concurrency/lock_manager_test.cpp"
    "test/concurrency/transaction_manager_test.cpp"
    "src/concurrency/lock_manager.cpp"
    "src/concurrency/transaction.cpp"
    "src/storage/tuple.cpp"
    "src/storage/table_page.cpp"
    "src/storage/table_heap.cpp"
    "src/storage/file_disk_manager.cpp"
    "src/buffer/lru_replacer.cpp"
    "src/buffer/buffer_pool_manager.cpp"
)
target_include_directories(concurrency_test PRIVATE include)
target_link_libraries(concurrency_test PRIVATE gtest_main)

# B+ 树索引单元测试
add_executable(index_test
    "test/index/b_plus_tree_test.cpp"
    "src/index/b_plus_tree.cpp"
    "src/index/b_plus_tree_page.cpp"
    "src/storage/tuple.cpp"
    "src/storage/table_page.cpp"
    "src/storage/table_heap.cpp"
    "src/storage/file_disk_manager.cpp"
    "src/buffer/lru_replacer.cpp"
    "src/buffer/buffer_pool_manager.cpp"
    "src/concurrency/lock_manager.cpp"
    "src/concurrency/transaction.cpp"
)
target_include_directories(index_test PRIVATE include)
target_link_libraries(index_test PRIVATE gtest_main)

include(GoogleTest)
gtest_discover_tests(concurrency_test)
gtest_discover_tests(index_test)

# 存储模块单元测试
add_executable(table_page_test
    "test/storage/table_page_test.cpp"
    "src/storage/table_page.cpp"
    "src/storage/tuple.cpp"
    "src/storage/file_disk_manager.cpp"
    "src/buffer/lru_replacer.cpp"
    "src/buffer/buffer_pool_manager.cpp"
    "src/concurrency/lock_manager.cpp"
    "src/concurrency/transaction.cpp"
)
target_include_directories(table_page_test PRIVATE include)
target_link_libraries(table_page_test PRIVATE gtest_main)

add_executable(table_heap_test
    "test/storage/table_heap_test.cpp"
    "src/storage/table_heap.cpp"
    "src/storage/table_page.cpp"
    "src/storage/tuple.cpp"
    "src/storage/file_disk_manager.cpp"
    "src/buffer/lru_replacer.cpp"
    "src/buffer/buffer_pool_manager.cpp"
    "src/concurrency/lock_manager.cpp"
    "src/concurrency/transaction.cpp"
)
target_include_directories(table_heap_test PRIVATE include)
target_link_libraries(table_heap_test PRIVATE gtest_main)

gtest_discover_tests(table_page_test)
gtest_discover_tests(table_heap_test)
