#pragma once

#include "execution/executor.h"
#include "execution/expression.h"
#include <memory>
#include <vector>

namespace db {

class FilterExecutor : public AbstractExecutor {
public:
    FilterExecutor(std::unique_ptr<AbstractExecutor> child,
                   const Expression* predicate,
                   const std::vector<std::string>& schema)
        : child_(std::move(child)),
          predicate_(predicate),
          schema_(schema) {}

    void Init() override;
    bool Next(Tuple* tuple) override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    const Expression* predicate_;
    std::vector<std::string> schema_;
};

} // namespace db
