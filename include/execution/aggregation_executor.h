#pragma once

#include "execution/executor.h"
#include "storage/tuple.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace db {

// ============================================================
// 聚合函数类型枚举
// ============================================================
enum class AggregationType {
    COUNT,   // 计数（COUNT(*) 或 COUNT(col)）
    SUM,     // 求和
    AVG,     // 平均值
    MIN,     // 最小值
    MAX      // 最大值
};

// ============================================================
// 聚合表达式 — 描述一个聚合操作
// ============================================================
struct AggregateExpr {
    AggregationType agg_type;   // 聚合类型
    std::string col_name;       // 目标列名（COUNT(*) 时为空，表示全列计数）

    AggregateExpr() = default;
    AggregateExpr(AggregationType type, std::string col)
        : agg_type(type), col_name(std::move(col)) {}
};

// ============================================================
// 聚合执行器 — 支持 COUNT / SUM / AVG / MIN / MAX + GROUP BY
// ============================================================
class AggregateExecutor : public AbstractExecutor {
public:
    /**
     * @param child         子执行器（通常是 SeqScan + Filter 的组合）
     * @param aggregates    SELECT 中的聚合表达式列表
     * @param group_by_cols GROUP BY 列名列表（空表示全表聚合）
     * @param schema        子执行器的输出 schema（列名 → 索引映射）
     */
    AggregateExecutor(std::unique_ptr<AbstractExecutor> child,
                      const std::vector<AggregateExpr>& aggregates,
                      const std::vector<std::string>& group_by_cols,
                      const std::vector<std::string>& schema);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    // ============================================================
    // 聚合累积器 — 记录一组（按 GROUP BY 键分组）的聚合中间状态
    // ============================================================
    struct Accumulator {
        int count = 0;          // COUNT 计数
        double sum = 0.0;       // SUM / AVG 累加
        double min_val = 0.0;   // MIN 最小值
        double max_val = 0.0;   // MAX 最大值
        bool has_value = false; // 是否已摄入过任何数值（MIN/MAX 初始值处理）

        void AddValue(double v) {
            count++;
            sum += v;
            if (!has_value) {
                min_val = max_val = v;
                has_value = true;
            } else {
                if (v < min_val) min_val = v;
                if (v > max_val) max_val = v;
            }
        }

        void AddCountOnly() {
            count++;
        }
    };

    // ============================================================
    // 分组键 — 由 GROUP BY 列的值组成
    // ============================================================
    struct GroupKey {
        std::vector<std::string> key_values;

        bool operator==(const GroupKey& other) const {
            return key_values == other.key_values;
        }
    };

    struct GroupKeyHash {
        size_t operator()(const GroupKey& k) const {
            size_t h = 0;
            for (const auto& v : k.key_values) {
                h ^= std::hash<std::string>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
            }
            return h;
        }
    };

    // 辅助：尝试将字符串转为数值（用于 SUM/AVG/MIN/MAX）
    static bool TryParseDouble(const std::string& s, double* out);

    // 辅助：在 schema 中查找列名对应的索引
    static int FindColIndex(const std::vector<std::string>& schema,
                            const std::string& col_name);

    // 辅助：格式化 double 输出
    static std::string FormatDouble(double v);

    std::unique_ptr<AbstractExecutor> child_;
    std::vector<AggregateExpr> aggregates_;
    std::vector<std::string> group_by_cols_;
    std::vector<std::string> schema_;   // 子执行器的原始列名列表

    // 聚合哈希表：GROUP BY 键 → 累积器
    std::unordered_map<GroupKey, Accumulator, GroupKeyHash> groups_;

    // Next() 遍历状态
    using GroupIterator = decltype(groups_)::const_iterator;
    GroupIterator iter_;
    bool initialized_{false};
};

} // namespace db
