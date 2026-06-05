#pragma once

#include "storage/tuple.h"
#include "execution/expression.h"
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <cctype>

namespace db {

// SQL 语句类型枚举
enum class SQLStmtType {
    CREATE_TABLE,
    INSERT,
    SELECT,
    DELETE,
    UPDATE
};

// AST 基类
struct SQLStmt {
    SQLStmtType type;
    virtual ~SQLStmt() = default;
};

// ============ 各语句 AST 节点 ============

struct CreateTableStmt : SQLStmt {
    std::string table_name;
    std::vector<std::string> columns; // CREATE TABLE 时指定的列名列表
    CreateTableStmt() { type = SQLStmtType::CREATE_TABLE; }
};

struct InsertStmt : SQLStmt {
    std::string table_name;
    Tuple tuple;
    InsertStmt() { type = SQLStmtType::INSERT; }
};

struct SelectStmt : SQLStmt {
    std::string table_name;
    std::unique_ptr<Expression> condition; // WHERE 子句（nullptr 表示无条件）
    std::vector<std::string> columns;      // SELECT 列名列表，空表示 SELECT *
    std::string order_by_col;              // ORDER BY 排序列名（空表示无 ORDER BY）
    bool order_desc = false;               // ORDER BY 是否降序（默认 ASC）
    bool has_aggregation = false;          // 是否包含聚合函数
    std::vector<std::string> group_by_cols; // GROUP BY 列名列表
    SelectStmt() { type = SQLStmtType::SELECT; }
};

struct DeleteStmt : SQLStmt {
    std::string table_name;
    std::unique_ptr<Expression> condition; // WHERE 子句
    DeleteStmt() { type = SQLStmtType::DELETE; }
};

struct UpdateStmt : SQLStmt {
    std::string table_name;
    std::vector<std::string> col_names;
    std::vector<std::string> values;
    std::unique_ptr<Expression> condition; // WHERE 子句
    UpdateStmt() { type = SQLStmtType::UPDATE; }
};

// ============ SQL 解析器 ============

class SQLParser {
public:
    SQLParser() = default;

    // 解析 SQL 字符串，返回 AST 根节点
    std::unique_ptr<SQLStmt> Parse(const std::string& sql) {
        std::string s = Trim(sql);
        std::string upper = ToUpper(s);

        if (upper.find("CREATE TABLE ") == 0) {
            return ParseCreateTable(sql);
        }
        if (upper.find("INSERT INTO ") == 0) {
            return ParseInsert(sql);
        }
        if (upper.find("SELECT ") == 0) {
            return ParseSelect(sql);
        }
        if (upper.find("DELETE ") == 0) {
            return ParseDelete(sql);
        }
        if (upper.find("UPDATE ") == 0) {
            return ParseUpdate(sql);
        }
        return nullptr;
    }

    // ---------- WHERE 子句解析（递归下降） ----------

    // 解析 WHERE 子句入口
    std::unique_ptr<Expression> ParseCondition(const std::string& where_clause) {
        cond_input_ = where_clause;
        cond_pos_ = 0;
        auto expr = ParseOr();
        SkipCondWhitespace();
        return expr;
    }

private:
    // ============ 通用工具函数 ============

    static std::string Trim(const std::string& s) {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
        return s.substr(start, end - start);
    }

    static std::string ToUpper(const std::string& s) {
        std::string result = s;
        for (auto& c : result) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return result;
    }

    // ============ WHERE 解析状态 ============
    size_t cond_pos_ = 0;
    std::string cond_input_;

    void SkipCondWhitespace() {
        while (cond_pos_ < cond_input_.size() && std::isspace(static_cast<unsigned char>(cond_input_[cond_pos_]))) {
            ++cond_pos_;
        }
    }

    // 匹配关键字（大小写不敏感），成功则推进位置
    bool MatchCondKeyword(const std::string& keyword) {
        SkipCondWhitespace();
        if (cond_pos_ + keyword.size() > cond_input_.size()) return false;

        std::string sub = cond_input_.substr(cond_pos_, keyword.size());
        bool match = true;
        for (size_t i = 0; i < keyword.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(sub[i])) !=
                std::toupper(static_cast<unsigned char>(keyword[i]))) {
                match = false;
                break;
            }
        }
        if (!match) return false;

        size_t after = cond_pos_ + keyword.size();
        if (after < cond_input_.size()) {
            char c = cond_input_[after];
            if (!std::isspace(static_cast<unsigned char>(c)) &&
                c != '=' && c != '<' && c != '>' && c != '(' && c != ')') {
                return false;
            }
        }
        cond_pos_ = after;
        return true;
    }

    // 解析标识符（列名）
    std::string ParseCondIdentifier() {
        SkipCondWhitespace();
        size_t start = cond_pos_;
        while (cond_pos_ < cond_input_.size() &&
               (std::isalnum(static_cast<unsigned char>(cond_input_[cond_pos_])) ||
                cond_input_[cond_pos_] == '_')) {
            ++cond_pos_;
        }
        if (cond_pos_ == start) return "";
        return cond_input_.substr(start, cond_pos_ - start);
    }

    // 解析比较运算符
    ComparisonOp ParseCondOperator() {
        SkipCondWhitespace();
        if (cond_pos_ >= cond_input_.size()) return ComparisonOp::EQUAL;

        char c = cond_input_[cond_pos_];
        if (c == '=') { ++cond_pos_; return ComparisonOp::EQUAL; }
        if (c == '<') {
            ++cond_pos_;
            if (cond_pos_ < cond_input_.size() && cond_input_[cond_pos_] == '>') {
                ++cond_pos_; return ComparisonOp::NOT_EQUAL;
            }
            if (cond_pos_ < cond_input_.size() && cond_input_[cond_pos_] == '=') {
                ++cond_pos_; return ComparisonOp::LESS_EQUAL;
            }
            return ComparisonOp::LESS_THAN;
        }
        if (c == '>') {
            ++cond_pos_;
            if (cond_pos_ < cond_input_.size() && cond_input_[cond_pos_] == '=') {
                ++cond_pos_; return ComparisonOp::GREATER_EQUAL;
            }
            return ComparisonOp::GREATER_THAN;
        }
        return ComparisonOp::EQUAL;
    }

    // 解析字面量（引号字符串或数字）
    std::string ParseCondLiteral() {
        SkipCondWhitespace();
        if (cond_pos_ >= cond_input_.size()) return "";

        if (cond_input_[cond_pos_] == '\'' || cond_input_[cond_pos_] == '"') {
            char quote = cond_input_[cond_pos_++];
            size_t start = cond_pos_;
            while (cond_pos_ < cond_input_.size() && cond_input_[cond_pos_] != quote) {
                ++cond_pos_;
            }
            std::string val = cond_input_.substr(start, cond_pos_ - start);
            if (cond_pos_ < cond_input_.size()) ++cond_pos_;
            return val;
        }

        size_t start = cond_pos_;
        if (cond_pos_ < cond_input_.size() && cond_input_[cond_pos_] == '-') ++cond_pos_;
        while (cond_pos_ < cond_input_.size() &&
               (std::isdigit(static_cast<unsigned char>(cond_input_[cond_pos_])) ||
                cond_input_[cond_pos_] == '.')) {
            ++cond_pos_;
        }
        if (cond_pos_ > start) {
            return cond_input_.substr(start, cond_pos_ - start);
        }

        return "";
    }

    // ---------- 递归下降解析 ----------
    // 语法（按优先级从低到高）:
    //   or_expr  := and_expr ('OR' and_expr)*
    //   and_expr := comparison ('AND' comparison)*
    //   comparison := identifier operator literal

    std::unique_ptr<Expression> ParseOr() {
        auto left = ParseAnd();
        while (MatchCondKeyword("OR")) {
            auto right = ParseAnd();
            left = std::make_unique<ConjunctionExpression>(
                std::move(left), ConjunctionOp::OR, std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expression> ParseAnd() {
        auto left = ParseComparison();
        while (MatchCondKeyword("AND")) {
            auto right = ParseComparison();
            left = std::make_unique<ConjunctionExpression>(
                std::move(left), ConjunctionOp::AND, std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expression> ParseComparison() {
        std::string col = ParseCondIdentifier();
        ComparisonOp op = ParseCondOperator();
        std::string literal = ParseCondLiteral();
        return std::make_unique<ComparisonExpression>(col, op, literal);
    }

public:
    // ---------- 各语句解析 ----------

    std::unique_ptr<SQLStmt> ParseCreateTable(const std::string& sql) {
        // 格式: CREATE TABLE <table_name> [(<col1>, <col2>, ...)]
        auto stmt = std::make_unique<CreateTableStmt>();

        std::string s = Trim(sql);
        std::string upper = ToUpper(s);

        size_t table_pos = upper.find("TABLE ");
        if (table_pos == std::string::npos) return nullptr;

        std::string after_table = Trim(s.substr(table_pos + 6));

        // 去掉可能的括号和列定义
        size_t paren = after_table.find('(');
        if (paren == std::string::npos) {
            stmt->table_name = after_table;
        } else {
            stmt->table_name = Trim(after_table.substr(0, paren));

            // 解析括号内的列名列表
            size_t end_paren = after_table.find(')', paren);
            if (end_paren != std::string::npos) {
                std::string cols_str = Trim(after_table.substr(paren + 1, end_paren - paren - 1));
                // 按逗号拆分列名
                size_t start = 0;
                while (start < cols_str.size()) {
                    size_t comma = cols_str.find(',', start);
                    std::string col = Trim(cols_str.substr(start, comma - start));
                    if (!col.empty()) {
                        stmt->columns.push_back(col);
                    }
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
        }
        return stmt;
    }

    std::unique_ptr<SQLStmt> ParseInsert(const std::string& sql) {
        // 格式: INSERT INTO <table_name> VALUES ('val1', 'val2', ...)
        auto stmt = std::make_unique<InsertStmt>();

        std::string s = Trim(sql);
        std::string upper = ToUpper(s);

        size_t into_pos = upper.find("INTO ");
        if (into_pos == std::string::npos) return nullptr;

        std::string after_into = Trim(s.substr(into_pos + 5));
        std::string upper_after = ToUpper(after_into);

        size_t values_pos = upper_after.find(" VALUES ");
        if (values_pos == std::string::npos) return nullptr;

        stmt->table_name = Trim(after_into.substr(0, values_pos));

        // 提取括号中的值
        std::string after_values = Trim(after_into.substr(values_pos + 8));
        size_t start_paren = after_values.find('(');
        size_t end_paren = after_values.find(')');
        if (start_paren == std::string::npos || end_paren == std::string::npos) return nullptr;

        std::string values_str = after_values.substr(start_paren + 1, end_paren - start_paren - 1);

        // 按逗号拆分并去除引号
        std::vector<std::string> vals;
        std::stringstream ss(values_str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = Trim(item);
            if (item.size() >= 2 && item.front() == '\'' && item.back() == '\'') {
                item = item.substr(1, item.size() - 2);
            } else if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
                item = item.substr(1, item.size() - 2);
            }
            vals.push_back(item);
        }
        stmt->tuple = Tuple(vals);
        return stmt;
    }

    std::unique_ptr<SQLStmt> ParseSelect(const std::string& sql) {
        // 格式: SELECT <cols> FROM <name> [WHERE <condition>]
        auto stmt = std::make_unique<SelectStmt>();

        std::string s = Trim(sql);
        std::string upper = ToUpper(s);

        size_t from_pos = upper.find(" FROM ");
        if (from_pos == std::string::npos) return nullptr;

        // 解析 SELECT 和 FROM 之间的列列表
        size_t select_end = 7; // strlen("SELECT ")
        std::string cols_str = Trim(s.substr(select_end, from_pos - select_end));
        if (cols_str != "*") {
            size_t start = 0;
            while (start < cols_str.size()) {
                size_t comma = cols_str.find(',', start);
                std::string col = Trim(cols_str.substr(start, comma - start));
                if (!col.empty()) {
                    stmt->columns.push_back(col);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }

        std::string after_from = s.substr(from_pos + 6);

        // 检查是否有 WHERE 子句
        std::string upper_after = ToUpper(after_from);
        size_t where_pos = upper_after.find(" WHERE ");

        if (where_pos != std::string::npos) {
            stmt->table_name = Trim(after_from.substr(0, where_pos));
            std::string where_clause = Trim(after_from.substr(where_pos + 7));
            stmt->condition = ParseCondition(where_clause);
        } else {
            stmt->table_name = Trim(after_from);
        }

        return stmt;
    }

    std::unique_ptr<SQLStmt> ParseDelete(const std::string& sql) {
        // 格式: DELETE FROM <table_name> [WHERE <condition>]
        auto stmt = std::make_unique<DeleteStmt>();

        std::string s = Trim(sql);
        std::string upper = ToUpper(s);

        // 去掉 "DELETE " 前缀，找到 "FROM "
        size_t from_pos = upper.find("FROM ");
        if (from_pos == std::string::npos) return nullptr;

        std::string after_from = s.substr(from_pos + 5);

        // 检查是否有 WHERE 子句
        std::string upper_after = ToUpper(after_from);
        size_t where_pos = upper_after.find(" WHERE ");

        if (where_pos != std::string::npos) {
            stmt->table_name = Trim(after_from.substr(0, where_pos));
            std::string where_clause = Trim(after_from.substr(where_pos + 7));
            stmt->condition = ParseCondition(where_clause);
        } else {
            stmt->table_name = Trim(after_from);
        }

        return stmt;
    }

    std::unique_ptr<SQLStmt> ParseUpdate(const std::string& sql) {
        // 格式: UPDATE <table_name> SET <col> = <val> [, <col> = <val> ...] [WHERE <condition>]
        auto stmt = std::make_unique<UpdateStmt>();

        std::string s = Trim(sql);
        std::string upper = ToUpper(s);

        // 去掉 "UPDATE " 前缀 (7 chars)
        std::string after = s.substr(7);
        after = Trim(after);
        std::string upper_after = ToUpper(after);

        // 提取表名（到 "SET" 之前）
        size_t set_pos = upper_after.find(" SET ");
        if (set_pos == std::string::npos) return nullptr;

        stmt->table_name = Trim(after.substr(0, set_pos));

        // 提取 SET 子句内容
        std::string after_set = after.substr(set_pos + 5); // skip " SET "

        // 检查是否有 WHERE 子句（在 SET 之后）
        std::string upper_after_set = ToUpper(after_set);
        size_t where_pos = upper_after_set.find(" WHERE ");
        std::string set_part;

        if (where_pos != std::string::npos) {
            set_part = Trim(after_set.substr(0, where_pos));
            std::string where_clause = Trim(after_set.substr(where_pos + 7));
            stmt->condition = ParseCondition(where_clause);
        } else {
            set_part = Trim(after_set);
        }

        // 按逗号拆分 col = val 对
        std::stringstream ss(set_part);
        std::string pair;
        while (std::getline(ss, pair, ',')) {
            pair = Trim(pair);
            // 按 '=' 拆分列名和值
            size_t eq_pos = pair.find('=');
            if (eq_pos == std::string::npos) continue;

            std::string col = Trim(pair.substr(0, eq_pos));
            std::string val = Trim(pair.substr(eq_pos + 1));

            // 去掉引号
            if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'') {
                val = val.substr(1, val.size() - 2);
            } else if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }

            stmt->col_names.push_back(col);
            stmt->values.push_back(val);
        }

        if (stmt->col_names.empty()) return nullptr;
        return stmt;
    }
};

} // namespace db
