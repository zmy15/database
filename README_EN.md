# 🗄️ Micro Database Engine — A Miniature Relational Database Engine

[![Language](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/License-GPL--3.0-green.svg)](LICENSE.txt)
[![Build](https://img.shields.io/badge/Build-CMake-brightgreen.svg)](CMakeLists.txt)
[![Tests](https://img.shields.io/badge/Tests-Google%20Test-orange.svg)](test/)

> 🇬🇧 English &nbsp;|&nbsp; 🇨🇳 [中文](README.md)

A fully functional, lightweight relational database engine built **from scratch in C++20**. This project implements the complete kernel pipeline of a database system:

**Storage Engine → Buffer Pool (LRU) → B+ Tree Index → SQL Parser (Recursive Descent) → Query Execution (Volcano Model) → WAL Crash Recovery → Concurrency Control (2PL + Deadlock Detection)**

> ⚠️ **This project is intended for learning and research. It is not suitable for production use.**

---

## 📐 System Architecture

```
┌─────────────────────────────────────────────────────┐
│                    SQL Query Input                    │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│           SQLParser (Recursive Descent)              │
│   CREATE / INSERT / SELECT / DELETE / UPDATE / DROP TABLE  │
│   WHERE (AND/OR/comparison) / ORDER BY / GROUP BY    │
│   Aggregate functions / BEGIN / COMMIT / ABORT       │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│        Planner (Execution Plan Builder)              │
│   IndexScan if index available, else SeqScan+Filter  │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│           Volcano Model Executors                    │
│  SeqScan → Filter → Projection → Aggregation         │
│  IndexScan (B+ Tree point/range query)               │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│       BufferPoolManager (Buffer Pool + LRU)          │
│        In-memory 4KB page cache with eviction        │
└──────────┬────────────────────┬─────────────────────┘
           ▼                    ▼
┌──────────────────┐  ┌──────────────────┐
│   DiskManager    │  │   LogManager     │
│  (File I/O)      │  │  (WAL Logging)   │
└──────────────────┘  └──────────────────┘
```

---

## ✨ Core Features

### 1. Recursive Descent SQL Parser

- **DDL**: `CREATE TABLE table_name (col1, col2, ...)` / `DROP TABLE table_name`
- **DML**: `INSERT INTO ... VALUES (...)` / `SELECT ... FROM ... WHERE ...` / `DELETE FROM ...` / `UPDATE ... SET ...`
- **WHERE clause**: supports `AND` / `OR`, comparison operators (`=` `<>` `>` `<` `>=` `<=`), parenthesized nesting
- **ORDER BY**: supports `ASC` (default) and `DESC`
- **GROUP BY**: grouping with aggregation
- **Aggregate functions**: `COUNT(*)` `COUNT(col)` `SUM` `AVG` `MIN` `MAX`
- **Transaction control**: `BEGIN` / `COMMIT` / `ABORT` (equivalent to `ROLLBACK`)

### 2. Storage Engine

| Component | File | Description |
|-----------|------|-------------|
| **DiskManager** | `include/storage/disk_manager.h` | Abstract disk interface: ReadPage / WritePage / AllocatePage / DeallocatePage |
| **FileDiskManager** | `src/storage/file_disk_manager.cpp` | File-based disk implementation |
| **TablePage** | `include/storage/table_page.h` | Slotted page layout with Insert / MarkDelete / Update / iteration |
| **TableHeap** | `include/storage/table_heap.h` | Doubly-linked list of TablePages with iterator support |
| **Tuple** | `include/storage/tuple.h` | Tuple serialization/deserialization with variable-length string fields |

- Page size: **4KB** (`PAGE_SIZE = 4096`)
- TableHeap uses a doubly-linked page chain for efficient append

### 3. Buffer Pool

| Component | File | Description |
|-----------|------|-------------|
| **BufferPoolManager** | `include/buffer/buffer_pool_manager.h` | Page table (page_id → frame_id) + FetchPage / NewPage / UnpinPage / FlushPage |
| **LRUReplacer** | `include/buffer/lru_replacer.h` | LRU eviction policy, cold pages written back to disk |
| **Page** | `include/buffer/page.h` | 4KB in-memory page with pin_count / is_dirty / page_id metadata |

- WAL-before-write: `FlushPage` calls `LogManager::FlushLogs()` before disk write
- Thread-safe: protected by `std::mutex`

### 4. B+ Tree Index

| Component | File | Description |
|-----------|------|-------------|
| **BPlusTree** | `include/index/b_plus_tree.h` | Insert / point query / range scan / delete / Drop (recursive cleanup) |
| **BPlusTreePage** | `include/index/b_plus_tree_page.h` | B+ tree page layout (internal: key + child pointers; leaf: doubly-linked list) |

- Internal nodes: N keys → N+1 children, `prev_page_id` stores child[0]
- Leaf nodes: doubly-linked for efficient range scans (`ScanRange`)
- Root-level `std::shared_mutex` for concurrent access protection
- Automatically creates an index on the first column of each table

### 5. Query Execution (Volcano Model)

```
AbstractExecutor (base class)
  ├── SeqScanExecutor      Full table sequential scan
  ├── FilterExecutor       WHERE clause filtering (wraps a child executor)
  ├── ProjectionExecutor   Column projection + ORDER BY sorting
  ├── AggregationExecutor  Aggregate computation (COUNT/SUM/AVG/MIN/MAX) + GROUP BY
  └── IndexScanExecutor    B+ tree index scan (point + range query)
```

- **Planner** selects IndexScan or SeqScan+Filter based on index availability
- Volcano model: each executor implements `Init()` + `Next(Tuple*)`, pulling data on demand

### 6. Transactions & Concurrency Control

| Component | File | Description |
|-----------|------|-------------|
| **Transaction** | `include/concurrency/transaction.h` | Transaction context: txn_id / isolation level / state / LSN chain / lock sets |
| **TwoPLManager** | `include/concurrency/lock_manager.h` | Two-phase locking: shared / exclusive locks + waits-for graph deadlock detection |
| **TransactionManager** | `include/concurrency/lock_manager.h` | Transaction lifecycle: Begin / Commit / Abort |

**Isolation Levels:**

| Level | Read Lock Strategy | Characteristics |
|-------|-------------------|-----------------|
| `READ_COMMITTED` | Shared lock, released after statement | Allows non-repeatable reads |
| `REPEATABLE_READ` | Shared lock, held until commit | Guarantees repeatable reads |
| `SERIALIZABLE` | Exclusive lock | Full serialization, prevents phantom reads |

**Deadlock Detection:**
- Waits-for graph modeling
- DFS cycle detection, selecting the youngest transaction as victim for abort

### 7. WAL Crash Recovery

- **ARIES-style** REDO / UNDO protocol
- `DBEngine` performs `DoRecovery()` automatically on startup:
  1. Reads all log records from the `.wal` file
  2. Builds transaction state table (committed / aborted)
  3. **REDO**: replays INSERT/DELETE/UPDATE of committed transactions in LSN order
  4. **UNDO**: rolls back uncommitted transactions in reverse LSN order
  5. Flushes all dirty pages and truncates WAL

---

## 🚀 Quick Start

### Prerequisites

- **C++20** compiler (MSVC 2022+ / GCC 12+ / Clang 16+)
- **CMake** ≥ 3.10
- **Ninja** (recommended on Windows) or Make

### Build

```bash
# Configure (using x64 Debug preset)
cmake --preset x64-debug

# Build
cmake --build out/build/x64-debug
```

Or manually:

```bash
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

### Run

```bash
./out/build/x64-debug/database
```

The program executes a series of built-in tests (CREATE TABLE / INSERT / SELECT / WHERE / DELETE / UPDATE / transactions / aggregation / GROUP BY / ORDER BY, etc.).

### Run Unit Tests

```bash
# Build test targets
cmake --build out/build/x64-debug --target concurrency_test
cmake --build out/build/x64-debug --target index_test
cmake --build out/build/x64-debug --target table_page_test
cmake --build out/build/x64-debug --target table_heap_test

# Run
ctest --test-dir out/build/x64-debug
```

---

## 📁 Project Structure

```
database/
├── include/                     # Header files
│   ├── db_engine.h              # Database engine entry point
│   ├── common/
│   │   ├── config.h             # Type definitions (page_id_t, txn_id_t, PAGE_SIZE, etc.)
│   │   └── rid.h                # Record ID (RID)
│   ├── storage/
│   │   ├── disk_manager.h       # Disk manager interface
│   │   ├── file_disk_manager.h  # File-based disk manager
│   │   ├── log_manager.h        # WAL log manager interface + LogRecord
│   │   ├── file_log_manager.h   # File-based log manager
│   │   ├── table_heap.h         # Table heap (page linked list)
│   │   ├── table_page.h         # Slotted page layout
│   │   ├── table_iterator.h     # Table iterator
│   │   └── tuple.h              # Tuple serialization
│   ├── buffer/
│   │   ├── buffer_pool_manager.h # Buffer pool manager
│   │   ├── lru_replacer.h       # LRU replacement policy
│   │   └── page.h               # In-memory page structure
│   ├── index/
│   │   ├── b_plus_tree.h        # B+ tree index
│   │   └── b_plus_tree_page.h   # B+ tree page layout
│   ├── execution/
│   │   ├── sql_parser.h         # Recursive descent SQL parser + AST
│   │   ├── planner.h            # Execution plan builder
│   │   ├── executor.h           # Executor base class (volcano model)
│   │   ├── seq_scan_executor.h   # Sequential scan executor
│   │   ├── filter_executor.h    # Filter executor
│   │   ├── projection_executor.h # Projection + sorting executor
│   │   ├── aggregation_executor.h # Aggregation + grouping executor
│   │   ├── index_scan_executor.h # Index scan executor
│   │   └── expression.h         # WHERE expression evaluation
│   └── concurrency/
│       ├── lock_manager.h       # 2PL + deadlock detection + transaction manager
│       └── transaction.h        # Transaction context
├── src/                         # Source implementation
│   ├── main.cpp                 # Entry point + built-in tests
│   ├── db_engine.cpp            # Engine implementation (including DoRecovery)
│   ├── storage/                 # Storage engine implementation
│   ├── buffer/                  # Buffer pool implementation
│   ├── index/                   # B+ tree implementation
│   ├── execution/               # Executor + Planner implementation
│   └── concurrency/             # Lock manager + transaction implementation
├── test/                        # Unit tests
│   ├── concurrency/             # lock_manager_test, transaction_manager_test
│   ├── index/                   # b_plus_tree_test
│   └── storage/                 # table_page_test, table_heap_test
├── CMakeLists.txt               # CMake build configuration
├── CMakePresets.json            # CMake presets (x64/x86 Debug/Release)
└── LICENSE.txt                  # GPL-3.0 license
```

---

## 📖 SQL Syntax Reference

```sql
-- DDL
CREATE TABLE users (name, age, email);

-- DDL: Drop table
DROP TABLE users;

-- DML: Insert
INSERT INTO users VALUES ('Alice', '30', 'alice@example.com');
INSERT INTO users VALUES ('Bob', '25', 'bob@example.com');

-- Query: full table scan
SELECT * FROM users;

-- Query: specific columns
SELECT name, email FROM users;

-- Query: WHERE conditions
SELECT * FROM users WHERE name = 'Alice';
SELECT * FROM users WHERE age > '20' AND age < '40';
SELECT * FROM users WHERE name = 'Alice' OR name = 'Bob';

-- Query: ORDER BY
SELECT * FROM users ORDER BY age;
SELECT * FROM users ORDER BY age DESC;

-- Query: aggregation + GROUP BY
SELECT COUNT(*) FROM users;
SELECT AVG(age) FROM users;
SELECT name, COUNT(*) FROM users GROUP BY name;

-- Update
UPDATE users SET age = '31' WHERE name = 'Alice';
UPDATE users SET email = 'new@example.com';

-- Delete
DELETE FROM users WHERE name = 'Bob';
DELETE FROM users;  -- delete all rows

-- Transactions
BEGIN;
INSERT INTO users VALUES ('Charlie', '28', 'charlie@ex.com');
COMMIT;

BEGIN;
INSERT INTO users VALUES ('Eve', '35', 'eve@ex.com');
ABORT;  -- rollback
```

---

## 🧪 Test Coverage

| Test Suite | File | Coverage |
|------------|------|----------|
| `table_page_test` | `test/storage/table_page_test.cpp` | TablePage insert/delete/update/iteration |
| `table_heap_test` | `test/storage/table_heap_test.cpp` | TableHeap multi-page operations |
| `index_test` | `test/index/b_plus_tree_test.cpp` | B+ tree insert/lookup/range scan/delete |
| `concurrency_test` | `test/concurrency/lock_manager_test.cpp` | 2PL lock acquire/release/upgrade |
| `concurrency_test` | `test/concurrency/transaction_manager_test.cpp` | Transaction Begin/Commit/Abort lifecycle |

---

## 🛠️ Tech Stack

- **Language**: C++20 (concepts / `std::make_unique` / `std::optional` / `std::shared_mutex`)
- **Build**: CMake 3.10+ + Ninja
- **Testing**: Google Test v1.14.0 (auto-downloaded via FetchContent)
- **License**: GNU General Public License v3.0

---

## 🔮 Roadmap

- [x] Complete DROP TABLE implementation
- [ ] Multi-column and composite indexes
- [ ] JOIN operators (Nested Loop / Hash Join)
- [ ] Query optimizer (cost estimation based on statistics)
- [ ] Network protocol layer (MySQL / PostgreSQL compatible)
- [ ] MVCC (Multi-Version Concurrency Control)
- [ ] Persistent catalog system
