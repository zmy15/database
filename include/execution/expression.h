#pragma once

#include "storage/tuple.h"
#include <string>
#include <vector>
#include <memory>
#include <cctype>
#include <algorithm>

namespace db {

// 比较运算符
enum class ComparisonOp {
    EQUAL,
    NOT_EQUAL,
    LESS_THAN,
    LESS_EQUAL,
    GREATER_THAN,
    GREATER_EQUAL
};

// 逻辑连接词
enum class ConjunctionOp {
    AND,
    OR
};

/**
 * @brief 表达式基类 — 用于 WHERE 子句的条件求值
 */
class Expression {
public:
    virtual ~Expression() = default;

    // 在给定行和表的列名列表上求值，返回 true/false
    // schema[i] = 第 i 列的列名
    virtual bool Evaluate(const Tuple& tuple,
                          const std::vector<std::string>& schema) const = 0;
};

/**
 * @brief 比较表达式：col op literal
 * 例如：age > 18，name = 'Alice'
 */
class ComparisonExpression : public Expression {
public:
    ComparisonExpression(std::string col, ComparisonOp op, std::string literal)
        : col_name_(std::move(col)), op_(op), literal_(std::move(literal)) {}

    const std::string& GetColName() const { return col_name_; }
    ComparisonOp GetOp() const { return op_; }
    const std::string& GetValue() const { return literal_; }

    bool Evaluate(const Tuple& tuple,
                  const std::vector<std::string>& schema) const override {
        // 在 schema 中查找列名对应的索引
        int col_idx = -1;
        for (size_t i = 0; i < schema.size(); ++i) {
            if (schema[i] == col_name_) {
                col_idx = static_cast<int>(i);
                break;
            }
        }
        // 列不存在则返回 false
        if (col_idx < 0) return false;

        const auto& vals = tuple.GetValues();
        if (static_cast<size_t>(col_idx) >= vals.size()) return false;

        const std::string& val = vals[col_idx];

        // 尝试数值比较，失败则回退字符串比较
        return Compare(val, literal_, op_);
    }

private:
    std::string col_name_;
    ComparisonOp op_;
    std::string literal_;

    // 智能比较：尝试转 double，失败则字符串比较
    static bool Compare(const std::string& left, const std::string& right,
                        ComparisonOp op) {
        // 尝试数值比较
        bool left_is_num = IsNumber(left);
        bool right_is_num = IsNumber(right);
        if (left_is_num && right_is_num) {
            double l = std::stod(left);
            double r = std::stod(right);
            switch (op) {
            case ComparisonOp::EQUAL:        return l == r;
            case ComparisonOp::NOT_EQUAL:    return l != r;
            case ComparisonOp::LESS_THAN:    return l < r;
            case ComparisonOp::LESS_EQUAL:   return l <= r;
            case ComparisonOp::GREATER_THAN: return l > r;
            case ComparisonOp::GREATER_EQUAL:return l >= r;
            }
            return false;
        }

        // 回退字符串比较
        int cmp = left.compare(right);
        switch (op) {
        case ComparisonOp::EQUAL:        return cmp == 0;
        case ComparisonOp::NOT_EQUAL:    return cmp != 0;
        case ComparisonOp::LESS_THAN:    return cmp < 0;
        case ComparisonOp::LESS_EQUAL:   return cmp <= 0;
        case ComparisonOp::GREATER_THAN: return cmp > 0;
        case ComparisonOp::GREATER_EQUAL:return cmp >= 0;
        }
        return false;
    }

    static bool IsNumber(const std::string& s) {
        if (s.empty()) return false;
        size_t start = (s[0] == '-') ? 1 : 0;
        if (start >= s.size()) return false;
        bool has_dot = false;
        for (size_t i = start; i < s.size(); ++i) {
            if (s[i] == '.') {
                if (has_dot) return false;
                has_dot = true;
            } else if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
                return false;
            }
        }
        return true;
    }
};

/**
 * @brief 逻辑连接表达式：left AND/OR right
 * 支持短路求值
 */
class ConjunctionExpression : public Expression {
public:
    ConjunctionExpression(std::unique_ptr<Expression> left,
                          ConjunctionOp op,
                          std::unique_ptr<Expression> right)
        : left_(std::move(left)), op_(op), right_(std::move(right)) {}

    const Expression* GetLeft() const { return left_.get(); }
    const Expression* GetRight() const { return right_.get(); }
    ConjunctionOp GetConjunctionOp() const { return op_; }

    bool Evaluate(const Tuple& tuple,
                  const std::vector<std::string>& schema) const override {
        if (op_ == ConjunctionOp::AND) {
            // 短路：左为 false 则直接返回 false
            return left_->Evaluate(tuple, schema) && right_->Evaluate(tuple, schema);
        } else {
            // 短路：左为 true 则直接返回 true
            return left_->Evaluate(tuple, schema) || right_->Evaluate(tuple, schema);
        }
    }

private:
    std::unique_ptr<Expression> left_;
    ConjunctionOp op_;
    std::unique_ptr<Expression> right_;
};

} // namespace db
