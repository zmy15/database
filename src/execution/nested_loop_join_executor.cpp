#include "execution/nested_loop_join_executor.h"

namespace db {

NestedLoopJoinExecutor::NestedLoopJoinExecutor(
    std::unique_ptr<AbstractExecutor> left_child,
    std::unique_ptr<AbstractExecutor> right_child,
    const Expression* join_condition,
    const std::vector<std::string>& left_schema,
    const std::vector<std::string>& right_schema)
    : left_child_(std::move(left_child)),
      right_child_(std::move(right_child)),
      join_condition_(join_condition),
      left_schema_(left_schema),
      right_schema_(right_schema),
      merged_schema_(left_schema) {
    // 拼接左/右 schema 得到 merged_schema_（左表列 + 右表列）
    merged_schema_.insert(merged_schema_.end(),
                          right_schema_.begin(), right_schema_.end());
}

void NestedLoopJoinExecutor::Init() {
    // 初始化左孩子（外循环）
    if (left_child_) {
        left_child_->Init();
    }
    has_left_ = false;
}

bool NestedLoopJoinExecutor::Next(Tuple* tuple) {
    if (!left_child_ || !right_child_) {
        return false;
    }

    while (true) {
        // === 步骤 1：如果还没有当前左行，拉取下一行左表 ===
        if (!has_left_) {
            if (!left_child_->Next(&current_left_)) {
                return false;  // 左表耗尽，JOIN 结束
            }
            has_left_ = true;
            // 对新的当前左行，重置右孩子迭代器
            right_child_->Init();
        }

        // === 步骤 2：遍历右孩子，找到第一个匹配项 ===
        Tuple right_tuple;
        while (right_child_->Next(&right_tuple)) {
            // 使用 Tuple::Merge 合并左右行
            Tuple merged = Tuple::Merge(current_left_, right_tuple);

            // 若 ON 条件为空（CROSS JOIN）或条件满足
            if (!join_condition_ || join_condition_->Evaluate(merged, merged_schema_)) {
                *tuple = std::move(merged);
                return true;
            }
            // 不匹配：继续内循环下一行
        }

        // === 步骤 3：右表耗尽，移动左表 ===
        has_left_ = false;
    }
}

} // namespace db
