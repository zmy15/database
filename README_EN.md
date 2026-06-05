# Micro Relational Database Engine

A fully functional, lightweight relational database engine built **from scratch in C++20**. This project implements the complete kernel pipeline of a database system: storage engine, buffer pool with LRU eviction, B+ tree indexing, recursive-descent SQL parsing, volcano-model query execution, WAL-based crash recovery, and two-phase locking (2PL) concurrency control.

> ⚠️ **This project is intended for learning and research. It is not suitable for production use.**

---

## Architecture

