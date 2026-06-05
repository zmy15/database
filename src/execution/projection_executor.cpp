#include "execution/projection_executor.h"

namespace db {

void ProjectionExecutor::Init() {
    if (child_) {
        child_->Init();
    }
}

bool ProjectionExecutor::Next(Tuple* tuple) {
    if (!child_) {
        return false;
    }

    Tuple full_tuple;
    if (!child_->Next(&full_tuple)) {
        return false;
    }

    const auto& all_vals = full_tuple.GetValues();
    std::vector<std::string> projected_vals;
    projected_vals.reserve(col_indices_.size());

    for (int idx : col_indices_) {
        if (idx >= 0 && static_cast<size_t>(idx) < all_vals.size()) {
            projected_vals.push_back(all_vals[idx]);
        } else {
            // 索引越界时填空字符串，避免崩溃
            projected_vals.push_back("");
        }
    }

    *tuple = Tuple(projected_vals);
    return true;
}

} // namespace db
