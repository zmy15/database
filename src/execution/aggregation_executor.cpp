#include "execution/aggregation_executor.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace db {

AggregateExecutor::AggregateExecutor(
    std::unique_ptr<AbstractExecutor> child,
    const std::vector<AggregateExpr>& aggregates,
    const std::vector<std::string>& group_by_cols,
    const std::vector<std::string>& schema)
    : child_(std::move(child)),
      aggregates_(aggregates),
      group_by_cols_(group_by_cols),
      schema_(schema) {}

void AggregateExecutor::Init() {
    if (child_) {
        child_->Init();
    }

    groups_.clear();

    // 消费子执行器的所有元组，按 GROUP BY 键聚合
    Tuple tuple;
    while (child_->Next(&tuple)) {
        const auto& vals = tuple.GetValues();

        // 构建 GROUP BY 键
        GroupKey key;
        for (const auto& gb_col : group_by_cols_) {
            int idx = FindColIndex(schema_, gb_col);
            if (idx >= 0 && static_cast<size_t>(idx) < vals.size()) {
                key.key_values.push_back(vals[idx]);
            } else {
                key.key_values.push_back("");
            }
        }

        auto& acc = groups_[key];

        // 对每个聚合表达式累积
        for (const auto& agg : aggregates_) {
            if (agg.agg_type == AggregationType::COUNT && agg.col_name.empty()) {
                // COUNT(*)
                acc.AddCountOnly();
            } else {
                // 其他聚合：找到列值
                int col_idx = FindColIndex(schema_, agg.col_name);
                if (col_idx < 0 || static_cast<size_t>(col_idx) >= vals.size()) {
                    continue; // 列不存在，跳过
                }

                if (agg.agg_type == AggregationType::COUNT) {
                    acc.AddCountOnly();
                } else {
                    double num_val = 0.0;
                    if (TryParseDouble(vals[col_idx], &num_val)) {
                        acc.AddValue(num_val);
                    }
                }
            }
        }
    }

    // 初始化迭代器
    iter_ = groups_.begin();
    initialized_ = true;
}

bool AggregateExecutor::Next(Tuple* tuple) {
    if (!initialized_ || iter_ == groups_.end()) {
        return false;
    }

    const auto& [key, acc] = *iter_;
    std::vector<std::string> result;

    // 先输出 GROUP BY 键的值
    for (const auto& kv : key.key_values) {
        result.push_back(kv);
    }

    // 再输出聚合结果
    for (const auto& agg : aggregates_) {
        switch (agg.agg_type) {
        case AggregationType::COUNT:
            result.push_back(std::to_string(acc.count));
            break;
        case AggregationType::SUM:
            result.push_back(FormatDouble(acc.sum));
            break;
        case AggregationType::AVG:
            if (acc.count > 0) {
                result.push_back(FormatDouble(acc.sum / acc.count));
            } else {
                result.push_back("0");
            }
            break;
        case AggregationType::MIN:
            if (acc.has_value) {
                result.push_back(FormatDouble(acc.min_val));
            } else {
                result.push_back("");
            }
            break;
        case AggregationType::MAX:
            if (acc.has_value) {
                result.push_back(FormatDouble(acc.max_val));
            } else {
                result.push_back("");
            }
            break;
        }
    }

    *tuple = Tuple(result);
    ++iter_;
    return true;
}

// static
bool AggregateExecutor::TryParseDouble(const std::string& s, double* out) {
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        *out = std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

// static
int AggregateExecutor::FindColIndex(const std::vector<std::string>& schema,
                                     const std::string& col_name) {
    for (size_t i = 0; i < schema.size(); ++i) {
        if (schema[i] == col_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// static
std::string AggregateExecutor::FormatDouble(double v) {
    std::ostringstream oss;
    oss << std::fixed;
    // 如果是整数，不显示小数部分
    if (v == static_cast<long long>(v)) {
        oss.precision(0);
    } else {
        oss.precision(6);
    }
    oss << v;
    // 去掉多余的尾部零
    std::string result = oss.str();
    // 如果包含小数点，去掉末尾的零
    size_t dot = result.find('.');
    if (dot != std::string::npos) {
        while (result.size() > dot + 1 && result.back() == '0') {
            result.pop_back();
        }
        if (result.back() == '.') {
            result.pop_back();
        }
    }
    return result;
}

} // namespace db
