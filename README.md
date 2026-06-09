# 🗄️ Micro Database Engine — 微型关系数据库引擎

[![Language](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/License-GPL--3.0-green.svg)](LICENSE.txt)
[![Build](https://img.shields.io/badge/Build-CMake-brightgreen.svg)](CMakeLists.txt)
[![Tests](https://img.shields.io/badge/Tests-Google%20Test-orange.svg)](test/)

> 🇨🇳 中文 &nbsp;|&nbsp; 🇬🇧 [English](README_EN.md)

一个使用 **C++20** 从零构建的微型关系数据库引擎，完整实现了数据库内核的核心流水线：
**存储引擎 → 缓冲池 (LRU) → B+ 树索引 → SQL 解析 (递归下降) → 查询执行 (火山模型) → WAL 崩溃恢复 → 事务并发控制 (2PL + 死锁检测)**。

> ⚠️ **本项目仅供学习与研究使用，不适用于生产环境。**

---

## 📐 系统架构

```
┌─────────────────────────────────────────────────────┐
│                    SQL 查询输入                        │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│              SQLParser（递归下降解析器）               │
│   CREATE / INSERT / SELECT / DELETE / UPDATE / DROP TABLE  │
│   WHERE (AND/OR/比较) / ORDER BY / GROUP BY / 聚合   │
│   BEGIN / COMMIT / ABORT (事务控制)                   │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│               Planner（执行计划构建器）                │
│   根据可用索引选择：IndexScan 或 SeqScan+Filter        │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│            火山模型执行器 (Volcano Model)              │
│  SeqScan → Filter → Projection → Aggregation         │
│  IndexScan（B+ 树点查/范围扫描）                       │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│           BufferPoolManager（缓冲池 + LRU）           │
│         内存中缓存 4KB 页面，淘汰冷页到磁盘             │
└──────────┬────────────────────┬─────────────────────┘
           ▼                    ▼
┌──────────────────┐  ┌──────────────────┐
│   DiskManager    │  │   LogManager     │
│  (文件磁盘读写)    │  │  (WAL 日志管理)   │
└──────────────────┘  └──────────────────┘
```

---

## ✨ 核心特性

### 1. SQL 解析器（递归下降）

- **DDL**: `CREATE TABLE table_name (col1, col2, ...)` / `DROP TABLE table_name`
- **DML**: `INSERT INTO ... VALUES (...)` / `SELECT ... FROM ... WHERE ...` / `DELETE FROM ...` / `UPDATE ... SET ...`
- **WHERE 子句**: 支持 `AND` / `OR`、比较运算符 (`=` `<>` `>` `<` `>=` `<=`)、括号嵌套
- **ORDER BY**: 支持 `ASC`（默认）和 `DESC` 排序
- **GROUP BY**: 分组聚合
- **聚合函数**: `COUNT(*)` `COUNT(col)` `SUM` `AVG` `MIN` `MAX`
- **事务控制**: `BEGIN` / `COMMIT` / `ABORT`（等价于 `ROLLBACK`）

### 2. 存储引擎

| 组件 | 文件 | 说明 |
|------|------|------|
| **DiskManager** | `include/storage/disk_manager.h` | 抽象磁盘接口：ReadPage / WritePage / AllocatePage / DeallocatePage |
| **FileDiskManager** | `src/storage/file_disk_manager.cpp` | 基于本地文件的磁盘实现 |
| **TablePage** | `include/storage/table_page.h` | 槽式页面布局（slotted page），支持 Insert / MarkDelete / Update / 遍历 |
| **TableHeap** | `include/storage/table_heap.h` | 表堆 = 双向链表串联多个 TablePage，提供迭代器 |
| **Tuple** | `include/storage/tuple.h` | 元组序列化/反序列化，支持变长字符串字段 |

- 页面大小：**4KB** (`PAGE_SIZE = 4096`)
- 表堆使用双向链表串联多个页面，支持高效追加写入

### 3. 缓冲池 (Buffer Pool)

| 组件 | 文件 | 说明 |
|------|------|------|
| **BufferPoolManager** | `include/buffer/buffer_pool_manager.h` | 页表映射 (page_id → frame_id) + FetchPage / NewPage / UnpinPage / FlushPage |
| **LRUReplacer** | `include/buffer/lru_replacer.h` | LRU 淘汰策略，冷页写回磁盘 |
| **Page** | `include/buffer/page.h` | 4KB 内存页，携带 pin_count / is_dirty / page_id 元数据 |

- WAL 先行刷盘：`FlushPage` 前先调用 `LogManager::FlushLogs()` 确保 WAL 持久化
- 线程安全：内部使用 `std::mutex` 保护

### 4. B+ 树索引

| 组件 | 文件 | 说明 |
|------|------|------|
| **BPlusTree** | `include/index/b_plus_tree.h` | 插入 / 点查 / 范围扫描 / 删除 / Drop（递归回收） |
| **BPlusTreePage** | `include/index/b_plus_tree_page.h` | B+ 树页面布局（内部节点存 key + child，叶子节点双向链表） |

- 内部节点：N 个 key 对应 N+1 个子节点，`prev_page_id` 存储 child[0]
- 叶子节点：双向链表连接，支持高效范围扫描（`ScanRange`）
- 根节点读写锁（`std::shared_mutex`）保护并发访问
- 默认对表的第一列自动建立索引

### 5. 查询执行（火山模型）

```
AbstractExecutor (基类)
  ├── SeqScanExecutor      全表顺序扫描
  ├── FilterExecutor       WHERE 条件过滤（包装子执行器）
  ├── ProjectionExecutor   SELECT 列投影 + ORDER BY 排序
  ├── AggregationExecutor  聚合计算 (COUNT/SUM/AVG/MIN/MAX) + GROUP BY
  └── IndexScanExecutor    B+ 树索引扫描（点查 + 范围扫描）
```

- **Planner** 根据 WHERE 条件中的列是否命中索引自动选择 IndexScan 或 SeqScan+Filter
- 火山模型：每个执行器实现 `Init()` + `Next(Tuple*)` 接口，按需拉取数据

### 6. 事务与并发控制

| 组件 | 文件 | 说明 |
|------|------|------|
| **Transaction** | `include/concurrency/transaction.h` | 事务上下文：txn_id / 隔离级别 / 状态 / LSN 链 / 锁集合 |
| **TwoPLManager** | `include/concurrency/lock_manager.h` | 两阶段锁协议：共享锁 / 排他锁 / 等待图死锁检测 |
| **TransactionManager** | `include/concurrency/lock_manager.h` | 事务生命周期：Begin / Commit / Abort |

**隔离级别：**

| 级别 | 读锁策略 | 特点 |
|------|----------|------|
| `READ_COMMITTED` | 共享锁，语句结束后释放 | 允许不可重复读 |
| `REPEATABLE_READ` | 共享锁，持有到事务提交 | 保证可重复读 |
| `SERIALIZABLE` | 排他锁 | 完全串行化，防止幻读 |

**死锁检测：**
- 使用等待图 (waits-for graph) 建模
- DFS 检测环路，选中最年轻事务作为 victim 并 Abort

### 7. WAL 崩溃恢复

- **ARIES 风格**的 REDO / UNDO 协议
- `DBEngine` 启动时自动执行 `DoRecovery()`：
  1. 从 `.wal` 文件读取所有日志记录
  2. 构建事务状态表（已提交 / 已中止）
  3. **REDO**：按 LSN 顺序重放已提交事务的 INSERT/DELETE/UPDATE
  4. **UNDO**：反向遍历回滚未提交事务的修改
  5. 刷盘所有脏页，截断 WAL

---

## 🚀 快速开始

### 环境要求

- **C++20** 编译器（MSVC 2022+ / GCC 12+ / Clang 16+）
- **CMake** ≥ 3.10
- **Ninja**（Windows 推荐）或 Make

### 构建

```bash
# 配置（使用 x64 Debug 预设）
cmake --preset x64-debug

# 编译
cmake --build out/build/x64-debug
```

或手动配置：

```bash
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

### 运行

```bash
./out/build/x64-debug/database
```

程序将自动执行一系列内置测试（CREATE TABLE / INSERT / SELECT / WHERE / DELETE / UPDATE / 事务 / 聚合 / GROUP BY / ORDER BY 等）。

### 运行单元测试

```bash
# 构建测试目标
cmake --build out/build/x64-debug --target concurrency_test
cmake --build out/build/x64-debug --target index_test
cmake --build out/build/x64-debug --target table_page_test
cmake --build out/build/x64-debug --target table_heap_test

# 运行
ctest --test-dir out/build/x64-debug
```

---

## 📁 项目结构

```
database/
├── include/                     # 头文件
│   ├── db_engine.h              # 数据库引擎入口
│   ├── common/
│   │   ├── config.h             # 类型定义（page_id_t, txn_id_t, PAGE_SIZE 等）
│   │   └── rid.h                # 记录标识 (RID)
│   ├── storage/
│   │   ├── disk_manager.h       # 磁盘管理器接口
│   │   ├── file_disk_manager.h  # 文件磁盘管理器实现
│   │   ├── log_manager.h        # WAL 日志管理器接口 + LogRecord
│   │   ├── file_log_manager.h   # 文件日志管理器实现
│   │   ├── table_heap.h         # 表堆（页面链表）
│   │   ├── table_page.h         # 槽式页面布局
│   │   ├── table_iterator.h     # 表迭代器
│   │   └── tuple.h              # 元组序列化
│   ├── buffer/
│   │   ├── buffer_pool_manager.h # 缓冲池管理器
│   │   ├── lru_replacer.h       # LRU 替换策略
│   │   └── page.h               # 内存页结构
│   ├── index/
│   │   ├── b_plus_tree.h        # B+ 树索引
│   │   └── b_plus_tree_page.h   # B+ 树页面布局
│   ├── execution/
│   │   ├── sql_parser.h         # SQL 递归下降解析器 + AST
│   │   ├── planner.h            # 执行计划构建器
│   │   ├── executor.h           # 执行器基类（火山模型）
│   │   ├── seq_scan_executor.h   # 全表扫描执行器
│   │   ├── filter_executor.h    # 条件过滤执行器
│   │   ├── projection_executor.h # 列投影 + 排序执行器
│   │   ├── aggregation_executor.h # 聚合 + 分组执行器
│   │   ├── index_scan_executor.h # 索引扫描执行器
│   │   └── expression.h         # WHERE 表达式求值
│   └── concurrency/
│       ├── lock_manager.h       # 两阶段锁 + 死锁检测 + 事务管理
│       └── transaction.h        # 事务上下文
├── src/                         # 源文件实现
│   ├── main.cpp                 # 入口 + 内置测试
│   ├── db_engine.cpp            # 数据库引擎实现（含 DoRecovery）
│   ├── storage/                 # 存储引擎实现
│   ├── buffer/                  # 缓冲池实现
│   ├── index/                   # B+ 树实现
│   ├── execution/               # 执行器 + Planner 实现
│   └── concurrency/             # 锁管理器 + 事务实现
├── test/                        # 单元测试
│   ├── concurrency/             # lock_manager_test, transaction_manager_test
│   ├── index/                   # b_plus_tree_test
│   └── storage/                 # table_page_test, table_heap_test
├── CMakeLists.txt               # CMake 构建配置
├── CMakePresets.json            # CMake 预设 (x64/x86 Debug/Release)
└── LICENSE.txt                  # GPL-3.0 许可证
```

---

## 📖 SQL 语法参考

```sql
-- DDL
CREATE TABLE users (name, age, email);

-- DDL: 删除表
DROP TABLE users;

-- DML: 插入
INSERT INTO users VALUES ('Alice', '30', 'alice@example.com');
INSERT INTO users VALUES ('Bob', '25', 'bob@example.com');

-- 查询: 全表
SELECT * FROM users;

-- 查询: 指定列
SELECT name, email FROM users;

-- 查询: WHERE 条件
SELECT * FROM users WHERE name = 'Alice';
SELECT * FROM users WHERE age > '20' AND age < '40';
SELECT * FROM users WHERE name = 'Alice' OR name = 'Bob';

-- 查询: ORDER BY
SELECT * FROM users ORDER BY age;
SELECT * FROM users ORDER BY age DESC;

-- 查询: 聚合 + GROUP BY
SELECT COUNT(*) FROM users;
SELECT AVG(age) FROM users;
SELECT name, COUNT(*) FROM users GROUP BY name;

-- 更新
UPDATE users SET age = '31' WHERE name = 'Alice';
UPDATE users SET email = 'new@example.com';

-- 删除
DELETE FROM users WHERE name = 'Bob';
DELETE FROM users;  -- 清空表

-- 事务
BEGIN;
INSERT INTO users VALUES ('Charlie', '28', 'charlie@ex.com');
COMMIT;

BEGIN;
INSERT INTO users VALUES ('Eve', '35', 'eve@ex.com');
ABORT;  -- 回滚
```

---

## 🧪 测试覆盖

| 测试套件 | 文件 | 覆盖内容 |
|----------|------|----------|
| `table_page_test` | `test/storage/table_page_test.cpp` | TablePage 插入/删除/更新/遍历 |
| `table_heap_test` | `test/storage/table_heap_test.cpp` | TableHeap 多页面操作 |
| `index_test` | `test/index/b_plus_tree_test.cpp` | B+ 树插入/查找/范围扫描/删除 |
| `concurrency_test` | `test/concurrency/lock_manager_test.cpp` | 2PL 锁获取/释放/升级 |
| `concurrency_test` | `test/concurrency/transaction_manager_test.cpp` | 事务 Begin/Commit/Abort 生命周期 |

---

## 🛠️ 技术栈

- **语言**: C++20（concepts / `std::make_unique` / `std::optional` / `std::shared_mutex`）
- **构建**: CMake 3.10+ + Ninja
- **测试**: Google Test v1.14.0 (通过 FetchContent 自动下载)
- **许可证**: GNU General Public License v3.0

---

## 🔮 后续规划

- [x] 完整的 DROP TABLE 实现
- [ ] 多列索引与复合索引
- [ ] JOIN 操作符（Nested Loop / Hash Join）
- [ ] 查询优化器（基于统计信息的代价估算）
- [ ] 网络协议层（MySQL / PostgreSQL 兼容协议）
- [ ] MVCC（多版本并发控制）
- [ ] 真正的持久化 Catalog 系统
