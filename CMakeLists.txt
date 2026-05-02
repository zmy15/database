# CMakeList.txt: database 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.10)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  # set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("database")

set(CMAKE_CXX_STANDARD 20)

# 将源代码添加到此项目的可执行文件。
add_executable (database
    "src/main.cpp"
    "src/db_engine.cpp"
    "src/storage/file_disk_manager.cpp"
    "src/buffer/lru_replacer.cpp"
    "src/buffer/buffer_pool_manager.cpp"
    "src/storage/tuple.cpp"
    "src/storage/table_page.cpp"
    "src/storage/table_heap.cpp"
)

include_directories(include)

# TODO: 如有需要，请添加测试并安装目标。
