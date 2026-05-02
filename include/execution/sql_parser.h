#pragma once

#include "storage/tuple.h"
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace db {

// ============================================================
// SQL 语句 AST 节点定义
// ============================================================

enum class SQLStmtType {
    CREATE_TABLE,
    INSERT,
    SELECT,
    UNKNOWN
};

struct SQLStmt {
    SQLStmtType type = SQLStmtType::UNKNOWN;
    virtual ~SQLStmt() = default;
};

struct CreateTableStmt : SQLStmt {
    std::string table_name;
    CreateTableStmt() { type = SQLStmtType::CREATE_TABLE; }
};

struct InsertStmt : SQLStmt {
    std::string table_name;
    Tuple tuple;
    InsertStmt() { type = SQLStmtType::INSERT; }
};

struct SelectStmt : SQLStmt {
    std::string table_name;
    SelectStmt() { type = SQLStmtType::SELECT; }
};

// ============================================================
// SQL 解析器 — 基于简单字符串拆分的轻量解析
// ============================================================

class SQLParser {
public:
    SQLParser() = default;

    // 解析 SQL 字符串，返回 AST 根节点
    std::unique_ptr<SQLStmt> Parse(const std::string& sql) {
        std::string upper = ToUpper(Trim(sql));

        if (upper.find("CREATE TABLE") == 0) {
            return ParseCreateTable(sql);
        }
        if (upper.find("INSERT INTO") == 0) {
            return ParseInsert(sql);
        }
        if (upper.find("SELECT") == 0) {
            return ParseSelect(sql);
        }

        return nullptr;
    }

private:
    // ---------- 工具函数 ----------

    static std::string Trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n;");
        return s.substr(start, end - start + 1);
    }

    static std::string ToUpper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return std::toupper(c); });
        return s;
    }

    // ---------- 各语句解析 ----------

    std::unique_ptr<SQLStmt> ParseCreateTable(const std::string& sql) {
        // 格式: CREATE TABLE <name> ( ... )
        auto stmt = std::make_unique<CreateTableStmt>();

        std::string s = Trim(sql);
        // 去掉 "CREATE TABLE " 前缀
        std::string after = s.substr(13); // "CREATE TABLE" is 12 chars
        after = Trim(after);

        // 提取表名（到第一个空格或 '(' 为止）
        size_t end = after.find_first_of(" (");
        stmt->table_name = after.substr(0, end);

        return stmt;
    }

    std::unique_ptr<SQLStmt> ParseInsert(const std::string& sql) {
        // 格式: INSERT INTO <name> VALUES (v1, v2, ...)
        auto stmt = std::make_unique<InsertStmt>();

        std::string s = Trim(sql);
        // 去掉 "INSERT INTO " 前缀 (12 chars)
        std::string after = s.substr(12);
        after = Trim(after);

        // 提取表名（到 "VALUES" 之前）
        std::string upper_after = ToUpper(after);
        size_t val_pos = upper_after.find("VALUES");
        if (val_pos == std::string::npos) return nullptr;

        stmt->table_name = Trim(after.substr(0, val_pos));

        // 提取 VALUES 括号内的内容
        std::string val_part = after.substr(val_pos + 6); // skip "VALUES"
        val_part = Trim(val_part);

        // 去掉括号
        if (!val_part.empty() && val_part.front() == '(') val_part = val_part.substr(1);
        if (!val_part.empty() && val_part.back() == ')') val_part.pop_back();

        // 按逗号拆分字段值
        std::vector<std::string> fields;
        std::stringstream ss(val_part);
        std::string field;
        while (std::getline(ss, field, ',')) {
            field = Trim(field);
            // 去掉引号
            if (field.size() >= 2 && field.front() == '\'' && field.back() == '\'') {
                field = field.substr(1, field.size() - 2);
            } else if (field.size() >= 2 && field.front() == '"' && field.back() == '"') {
                field = field.substr(1, field.size() - 2);
            }
            fields.push_back(field);
        }

        stmt->tuple = Tuple(fields);
        return stmt;
    }

    std::unique_ptr<SQLStmt> ParseSelect(const std::string& sql) {
        // 格式: SELECT * FROM <name>
        auto stmt = std::make_unique<SelectStmt>();

        std::string s = Trim(sql);
        std::string upper = ToUpper(s);

        size_t from_pos = upper.find(" FROM ");
        if (from_pos == std::string::npos) return nullptr;

        stmt->table_name = Trim(s.substr(from_pos + 6));
        return stmt;
    }
};

} // namespace db
