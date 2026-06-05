#pragma once

#include "execution/executor.h"
#include "storage/tuple.h"
#include <memory>
#include <vector>
#include <string>

namespace db {

/**
 * @brief 列投影执行器 — 从子执行器的元组中裁剪指定列
 *
 * 用于支持 SELECT col1, col2 FROM table 语法。
 * 构造函数接收需要保留的列索引列表（根据 schema 确定）。
 */
class ProjectionExecutor : public AbstractExecutor {
public:
    /**
     * @param child        子执行器（提供完整元组）
     * @param col_indices  需要投影的列索引列表（0-based）
     * @param schema       输出 schema（列名列表，仅包含投影的列）
     */
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> child,
                       const std::vector<int>& col_indices,
                       const std::vector<std::string>& schema)
        : child_(std::move(child)),
          col_indices_(col_indices),
          schema_(schema) {}

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    std::vector<int> col_indices_;
    std::vector<std::string> schema_;
};

} // namespace db
