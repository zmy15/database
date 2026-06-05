# storage 模块单元测试
# TODO: 以下测试文件尚未实现，暂时注释
# add_executable(table_page_test
#     "test/storage/table_page_test.cpp"
#     "src/storage/table_page.cpp"
#     "src/storage/tuple.cpp"
#     "src/storage/file_disk_manager.cpp"
#     "src/buffer/lru_replacer.cpp"
#     "src/buffer/buffer_pool_manager.cpp"
# )
# target_include_directories(table_page_test PRIVATE include)
# target_link_libraries(table_page_test PRIVATE gtest_main)
# 
# add_executable(table_heap_test
#     "test/storage/table_heap_test.cpp"
#     "src/storage/table_heap.cpp"
#     "src/storage/table_page.cpp"
#     "src/storage/tuple.cpp"
#     "src/storage/file_disk_manager.cpp"
#     "src/buffer/lru_replacer.cpp"
#     "src/buffer/buffer_pool_manager.cpp"
# )
# target_include_directories(table_heap_test PRIVATE include)
# target_link_libraries(table_heap_test PRIVATE gtest_main)
# 
# include(GoogleTest)
# gtest_discover_tests(table_page_test)
# gtest_discover_tests(table_heap_test)
