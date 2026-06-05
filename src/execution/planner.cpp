#include "execution/planner.h"
#include "execution/seq_scan_executor.h"
#include "execution/filter_executor.h"
#include "execution/projection_executor.h"
#include "execution/aggregation_executor.h"
#include "execution/index_scan_executor.h"

namespace db {

std::unique_ptr<AbstractExecutor> Planner::CreatePlan(const SelectStmt* stmt) {
    if (!stmt) return nullptr;

    // 查找目标表
    auto it = tables_.find(stmt->table_name);
    if (it == tables_.end()) {
        return nullptr;
    }
    TableHeap* heap = it->second.get();

    // 获取表模式（用于列名解析和索引判断）
    std::vector<std::string> schema;
    auto schema_iter = table_schemas_.find(stmt->table_name);
    if (schema_iter != table_schemas_.end()) {
        schema = schema_iter->second;
    }

    // ---------- 分析 WHERE 条件，判断是否可用索引 ----------
    bool use_index = false;
    std::string index_key;

    if (stmt->condition) {
        auto* comp = dynamic_cast<const ComparisonExpression*>(stmt->condition.get());
        if (comp != nullptr && comp->GetOp() == ComparisonOp::EQUAL) {
            // 检查该列是否是第一列（默认索引列）
            if (!schema.empty() && comp->GetColName() == schema[0]) {
                auto idx_it = indexes_.find(stmt->table_name);
                if (idx_it != indexes_.end()) {
                    use_index = true;
                    index_key = comp->GetValue();
                }
            }
        }
    }

    // ---------- 构建执行器树 ----------
    std::unique_ptr<AbstractExecutor> executor;

    if (use_index) {
        // 索引等值点查
        auto idx_it = indexes_.find(stmt->table_name);
        executor = std::make_unique<IndexScanExecutor>(
            idx_it->second.get(), index_key, heap, bpm_, txn_, lock_mgr_, stmt->table_name);
    } else {
        // 全表顺序扫描
        executor = std::make_unique<SeqScanExecutor>(heap, bpm_, txn_, lock_mgr_, stmt->table_name);

        // 存在 WHERE 条件但不可索引化 → 包装 FilterExecutor
        if (stmt->condition) {
            executor = std::make_unique<FilterExecutor>(
                std::move(executor), stmt->condition.get(), schema);
        }
    }

    // 指定了列列表（非 SELECT *）且 schema 非空 → 包装 ProjectionExecutor
    //    注意：聚合查询跳过列投影（AggregateExecutor 负责输出形状）
    if (!stmt->columns.empty() && !schema.empty() && !stmt->has_aggregation) {
        std::vector<int> col_indices;
        for (const auto& col : stmt->columns) {
            int idx = -1;
            for (size_t i = 0; i < schema.size(); ++i) {
                if (schema[i] == col) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            col_indices.push_back(idx);  // 未找到则为 -1
        }
        executor = std::make_unique<ProjectionExecutor>(
            std::move(executor), col_indices, stmt->columns);
    }

    // 4. 聚合包装：如果 SELECT 包含聚合函数，包裹 AggregateExecutor
    if (stmt->has_aggregation) {
        std::vector<AggregateExpr> aggs;
        for (const auto& col : stmt->columns) {
            AggregateExpr expr;
            // 尝试从 "COUNT(*)", "COUNT(col)", "SUM(col)" 等解析
            std::string upper_col = col;
            for (auto& c : upper_col) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            size_t paren = upper_col.find('(');
            if (paren != std::string::npos) {
                size_t rparen = upper_col.rfind(')');
                if (rparen != std::string::npos && rparen > paren) {
                    std::string func_name = upper_col.substr(0, paren);
                    std::string col_name = col.substr(paren + 1, rparen - paren - 1);
                    // trim col_name
                    size_t cs = 0, ce = col_name.size();
                    while (cs < ce && std::isspace(static_cast<unsigned char>(col_name[cs]))) ++cs;
                    while (ce > cs && std::isspace(static_cast<unsigned char>(col_name[ce - 1]))) --ce;
                    col_name = col_name.substr(cs, ce - cs);

                    AggregationType agg_type;
                    std::string agg_col_name;
                    if (func_name == "COUNT" && col_name == "*") { agg_type = AggregationType::COUNT; agg_col_name = ""; }
                    else if (func_name == "COUNT") { agg_type = AggregationType::COUNT; agg_col_name = col_name; }
                    else if (func_name == "SUM")   { agg_type = AggregationType::SUM;   agg_col_name = col_name; }
                    else if (func_name == "AVG")   { agg_type = AggregationType::AVG;   agg_col_name = col_name; }
                    else if (func_name == "MIN")   { agg_type = AggregationType::MIN;   agg_col_name = col_name; }
                    else if (func_name == "MAX")   { agg_type = AggregationType::MAX;   agg_col_name = col_name; }
                    else continue;
                    aggs.push_back(AggregateExpr(agg_type, agg_col_name));
                }
            }
        }
        // 若无显式聚合列但有 GROUP BY，添加默认 COUNT(*)
        if (aggs.empty() && !stmt->group_by_cols.empty()) {
            aggs.push_back(AggregateExpr(AggregationType::COUNT, ""));
        }
        if (!aggs.empty()) {
            executor = std::make_unique<AggregateExecutor>(
                std::move(executor), aggs, stmt->group_by_cols, schema);
        }
    }

    return executor;
}

} // namespace db
