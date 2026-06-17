#pragma once

#include "execution/executor.h"
#include "execution/expression.h"
#include "storage/tuple.h"
#include <memory>
#include <vector>
#include <string>

namespace db {

/**
 * @brief NestedLoopJoinExecutor — 嵌套循环连接执行器
 *
 * 实现 INNER JOIN 和 CROSS JOIN 的火山模型迭代器。
 * - 外循环：遍历左孩子（left_child_）的每一行
 * - 内循环：对每个左行，从头遍历右孩子（right_child_）的每一行
 * - 检查 join_condition（若为空则为 CROSS JOIN，所有行都匹配）
 * - 匹配成功则调用 Tuple::Merge() 返回合并行
 */
class NestedLoopJoinExecutor : public AbstractExecutor {
public:
    /**
     * @param left_child     左表执行器（外循环）
     * @param right_child    右表执行器（内循环）
     * @param join_condition JOIN...ON 条件表达式（为 nullptr 表示 CROSS JOIN）
     * @param left_schema    左表的列名列表（用于条件求值）
     * @param right_schema   右表的列名列表（用于条件求值）
     */
    NestedLoopJoinExecutor(
        std::unique_ptr<AbstractExecutor> left_child,
        std::unique_ptr<AbstractExecutor> right_child,
        const Expression* join_condition,
        const std::vector<std::string>& left_schema,
        const std::vector<std::string>& right_schema);

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    std::unique_ptr<AbstractExecutor> left_child_;
    std::unique_ptr<AbstractExecutor> right_child_;
    const Expression* join_condition_;          // 不拥有所有权，由 SelectStmt 持有

    std::vector<std::string> left_schema_;
    std::vector<std::string> right_schema_;
    std::vector<std::string> merged_schema_;    // 拼接后的完整 schema（left_schema_ + right_schema_）

    Tuple current_left_;                        // 当前外循环行
    bool has_left_{false};                      // 是否有有效的当前左行
};

} // namespace db
