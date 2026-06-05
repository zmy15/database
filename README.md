# 🗄️ Micro Database Engine — 微型关系数据库引擎

一个使用 **C++20** 从零构建的微型关系数据库引擎，旨在深入理解数据库内核实现原理，涵盖存储引擎、缓冲池、B+ 树索引、SQL 解析执行、事务与并发控制等核心模块。

## 📁 项目结构

```
database/
├── CMakeLists.txt                    # CMake 构建配置
├── CMakePresets.json                 # 构建预设 (x64/x86, debug/release)
├── LICENSE.txt                       # GPL-3.0 许可证
├── README.md
├── include/                          # 头文件（以 header-only 为主）
│   ├── db_engine.h                   # 引擎入口 - DBEngine 类
│   ├── buffer/                       # 缓冲池模块
│   │   ├── buffer_pool_manager.h     # 缓冲池管理器 (LRU 淘汰)
│   │   ├── lru_replacer.h            # LRU 页面置换策略
│   │   └── page.h                    # 内存页 (4KB, pin_count, dirty, 读写锁)
│   ├── common/                       # 公共定义
│   │   ├── config.h                  # 类型别名: page_id_t, txn_id_t, lsn_t; PAGE_SIZE=4096
│   │   └── rid.h                     # 记录标识符 RID (page_id + slot_num)
│   ├── concurrency/                  # 并发控制
│   │   ├── lock_manager.h            # 两阶段锁 (2PL) 管理器
│   │   └── transaction.h             # 事务上下文 (隔离级别, 锁集合)
│   ├── execution/                    # SQL 执行引擎
│   │   ├── sql_parser.h              # SQL 解析器 (递归下降)
│   │   ├── expression.h              # 比较/逻辑表达式
│   │   ├── planner.h                 # 执行计划生成器 (选择 IndexScan 或 SeqScan)
│   │   ├── executor.h                # 执行器基类 (火山模型: Init → Next)
│   │   ├── seq_scan_executor.h       # 全表扫描
│   │   ├── index_scan_executor.h     # 索引扫描
│   │   ├── filter_executor.h         # WHERE 过滤
│   │   ├── projection_executor.h     # 列投影
│   │   └── aggregation_executor.h    # 聚合计算 (COUNT/SUM/AVG/MIN/MAX + GROUP BY)
│   ├── index/                        # 索引模块
│   │   ├── b_plus_tree.h             # B+ 树 (插入/查询/范围扫描/删除/回收)
│   │   └── b_plus_tree_page.h        # B+ 树页面格式 (内部节点 & 叶子节点)
│   └── storage/                      # 存储层
│       ├── disk_manager.h            # 抽象磁盘 I/O 接口
│       ├── file_disk_manager.h       # 文件磁盘管理器实现
│       ├── log_manager.h             # WAL 日志管理器接口
│       ├── file_log_manager.h        # 文件 WAL 日志管理器实现
│       ├── table_heap.h              # 表堆 (TablePage 链表)
│       ├── table_page.h              # 槽式页面布局 (slotted-page)
│       ├── table_iterator.h          # 表扫描迭代器
│       └── tuple.h                   # 元组 (行数据序列化/反序列化)
├── src/                              # 实现文件 (.cpp)
│   ├── main.cpp                      # 演示/测试入口
│   ├── db_engine.cpp                 # DBEngine: 初始化, 恢复, ExecuteQuery 分发
│   ├── buffer/                       # 缓冲池实现
│   ├── concurrency/                  # 并发控制实现
│   ├── execution/                    # 执行器实现
│   ├── index/                        # B+ 树实现
│   └── storage/                      # 存储层实现
└── test/                             # 单元测试 (Google Test)
    ├── storage/                      # 表页面 & 表堆测试
    ├── index/                        # B+ 树索引测试
    └── concurrency/                  # 锁管理器 & 事务管理器测试
```

## 🏗️ 架构设计

```
┌──────────────────────────────────────────────────────┐
│  DBEngine — 数据库引擎入口                            │
│  · 启动时执行 WAL 崩溃恢复 (REDO)                      │
│  · ExecuteQuery() 分发 SQL 到解析→计划→执行管线        │
├──────────────────────────────────────────────────────┤
│  SQL解析器 → 执行计划生成器 → 执行器 (火山模型)         │
├──────────────────────────────────────────────────────┤
│  B+树索引  │  缓冲池管理器  │  锁管理器 (2PL)           │
├──────────────────────────────────────────────────────┤
│  磁盘管理器 (文件I/O)  │  WAL 日志管理器                │
└──────────────────────────────────────────────────────┘
```

## ✨ 核心特性

### 1. SQL 解析器（递归下降）
支持以下 DDL / DML / 查询语句：

```sql
-- 建表
CREATE TABLE students (id, name, score);

-- 插入
INSERT INTO students VALUES (1, 张三, 95.5);

-- 查询
SELECT id, name FROM students WHERE score > 80;

-- 更新
UPDATE students SET score = 100 WHERE id = 1;

-- 删除
DELETE FROM students WHERE name = 张三;
```

WHERE 子句支持：`=`, `<>`, `<`, `>`, `<=`, `>=`, `AND`, `OR`，具备短路求值，智能数值/字符串比较。

### 2. 火山模型执行器
可组合的执行器管线：

```
SeqScanExecutor → FilterExecutor → ProjectionExecutor
                → IndexScanExecutor (当查询命中首列索引)
                → AggregationExecutor (COUNT/SUM/AVG/MIN/MAX + GROUP BY)
```

### 3. B+ 树索引
- 内部节点存储 `(key, child_pointer)`
- 叶子节点存储 `(key, tuple_data)`
- 支持：**插入**、**点查询**、**范围扫描**、**标记删除**、**整树回收**
- 节点分裂策略：`MoveHalfTo()` 分裂为两个半满节点
- 线程安全的根节点锁 (`std::shared_mutex`)

### 4. 缓冲池 + LRU 淘汰
- 固定大小页面数组（默认 64 页，每页 4KB）
- `FetchPage()`: 按需从磁盘加载页面到内存
- `NewPage()`: 在磁盘上分配新页面
- LRU 淘汰：通过双向链表 + 哈希表实现 O(1) 淘汰
- WAL-before-write: 刷脏页前先刷日志
- 页面级读写锁 (`std::shared_mutex`)

### 5. 槽式页面布局 (Slotted Page)
```
┌──────────────┬──────────────────┬──────────────────┐
│ Page Header  │ Slot Array →→→   │ ←←← Tuple Data   │
│   (24 B)     │ (每槽 8 B)       │ (变长)           │
└──────────────┴──────────────────┴──────────────────┘
```
- 表由多个 4KB 页通过链表组成 (TableHeap)
- 每页头部 24 字节：page_id, LSN, 前后页指针, 空闲空间指针, 元组计数

### 6. WAL 崩溃恢复
- 启动时读取所有 WAL 日志记录
- 识别已提交/已中止事务
- REDO: 重放已提交的 INSERT/DELETE/UPDATE
- 恢复后刷新所有脏页并截断 WAL 文件

### 7. 两阶段锁 (2PL) 并发控制
- `TwoPLManager`: 支持共享锁 (S) 和排他锁 (X)
- `TransactionManager`: BEGIN → 操作 → COMMIT/ABORT
- 隔离级别：READ_COMMITTED / REPEATABLE_READ / SERIALIZABLE
- 锁表：基于 RID 的锁条目，跟踪持有者

> ⚠️ 当前为单线程设计，锁管理器接口已就绪但尚未完全接入多线程环境。

## 🔧 构建与运行

### 前置要求
- **CMake** ≥ 3.10
- **编译器**: MSVC 2022 (Windows) / GCC 13+ / Clang 16+
- **构建生成器**: Ninja (推荐)

### 构建步骤

```powershell
# 1. 克隆仓库
git clone <repo-url>
cd database

# 2. 使用 CMake Preset 配置 (x64-debug)
cmake --preset x64-debug

# 3. 编译
cmake --build --preset x64-debug

# 4. 运行主程序演示
.\build\x64-debug\bin\database.exe

# 5. 运行测试
ctest --preset x64-debug
```

### 构建目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `database` | 可执行文件 | 主程序演示 |
| `concurrency_test` | 测试 | 锁管理器 + 事务管理器测试 |
| `index_test` | 测试 | B+ 树索引测试 |
| `table_page_test` | 测试 | 槽式页面测试 |
| `table_heap_test` | 测试 | 表堆多页操作测试 |

## 🧪 测试覆盖

- **table_page_test**: 槽式页面的插入/读取/更新/删除/标记删除 (8+ 用例)
- **table_heap_test**: 多页表的插入/读取/更新/删除/范围迭代 (6+ 用例)
- **b_plus_tree_test**: 单键插入、批量插入 (1000 键)、点查询、范围扫描、删除、整树回收 (7+ 用例)
- **concurrency_test**: 共享锁/排他锁基础、S-S 兼容性、S-X 冲突、事务生命周期 (10+ 用例)

## 📋 已知限制 (TODO)

- 单线程设计（锁管理器未完全适配多线程）
- 不支持 JOIN 操作
- 不支持子查询 / 嵌套查询
- 所有值以字符串存储，无类型系统
- 无预编译语句
- 无基于代价的查询优化器
- 无网络协议层
- 缺失 SQL 特性: DISTINCT, LIMIT, HAVING, ORDER BY 等

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！请在提交 PR 前确保所有测试通过。

## 📄 许可证

本项目基于 **GPL-3.0** 许可证开源。详见 [LICENSE.txt](LICENSE.txt)。

---

*该项目主要用于学习与研究，不适合生产环境使用。*
