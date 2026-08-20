#include <gtest/gtest.h>
#include "execution/executor.h"
#include "execution/expression.h"
#include "execution/nested_loop_join_executor.h"
#include "storage/tuple.h"
#include <memory>
#include <vector>
#include <string>

using namespace db;

// ============================================================
// Mock 子执行器：按预置的固定 Tuple 序列返回数据
// 用于测试 NestedLoopJoinExecutor 的火山模型迭代行为
// ============================================================
class MockExecutor : public AbstractExecutor {
public:
    explicit MockExecutor(std::vector<Tuple> tuples) : tuples_(std::move(tuples)) {}

    void Init() override { pos_ = 0; }

    bool Next(Tuple* tuple) override {
        if (pos_ >= tuples_.size()) {
            return false;
        }
        *tuple = tuples_[pos_++];
        return true;
    }

private:
    std::vector<Tuple> tuples_;
    size_t pos_ = 0;
};

// ============================================================
// 自定义 JOIN 条件：比较合并 Tuple 中左右两列的相等性
// merged = 左表列 + 右表列，left_idx / right_idx 为合并后的列下标
// ============================================================
class ColEqCondition : public Expression {
public:
    ColEqCondition(size_t left_idx, size_t right_idx)
        : left_idx_(left_idx), right_idx_(right_idx) {}

    bool Evaluate(const Tuple& tuple,
                  const std::vector<std::string>& schema) const override {
        (void)schema;
        const auto& vals = tuple.GetValues();
        if (left_idx_ >= vals.size() || right_idx_ >= vals.size()) {
            return false;
        }
        return vals[left_idx_] == vals[right_idx_];
    }

private:
    size_t left_idx_;
    size_t right_idx_;
};

// ============================================================
// 工具：收集执行器的全部输出行
// ============================================================
static std::vector<Tuple> DrainExecutor(AbstractExecutor* executor) {
    std::vector<Tuple> result;
    Tuple t;
    while (executor->Next(&t)) {
        result.push_back(t);
    }
    return result;
}

// ============================================================
// 测试用例
// ============================================================

// CROSS JOIN：join_condition 为 nullptr 时所有行组合均匹配
TEST(NestedLoopJoinTest, CrossJoinReturnsCartesianProduct) {
    // 左表 2 行（schema: id, name）
    std::vector<Tuple> left_tuples = {
        Tuple({"1", "Alice"}),
        Tuple({"2", "Bob"}),
    };
    // 右表 2 行（schema: id, dept）
    std::vector<Tuple> right_tuples = {
        Tuple({"1", "Eng"}),
        Tuple({"3", "Sales"}),
    };

    auto left = std::make_unique<MockExecutor>(std::move(left_tuples));
    auto right = std::make_unique<MockExecutor>(std::move(right_tuples));

    NestedLoopJoinExecutor join(std::move(left), std::move(right),
                                /*join_condition=*/nullptr,
                                /*left_schema=*/{"id", "name"},
                                /*right_schema=*/{"id", "dept"});
    join.Init();

    auto rows = DrainExecutor(&join);
    // 2 × 2 = 4 行笛卡尔积
    ASSERT_EQ(rows.size(), 4u);

    // 每行应为 4 列（左 2 + 右 2）
    for (const auto& row : rows) {
        EXPECT_EQ(row.GetValues().size(), 4u);
    }
    // 验证第一行是 Alice × Eng
    EXPECT_EQ(rows[0].GetValues()[0], "1");
    EXPECT_EQ(rows[0].GetValues()[1], "Alice");
    EXPECT_EQ(rows[0].GetValues()[2], "1");
    EXPECT_EQ(rows[0].GetValues()[3], "Eng");
}

// INNER JOIN：仅返回满足 ON 条件的组合
TEST(NestedLoopJoinTest, InnerJoinReturnsMatchingRows) {
    std::vector<Tuple> left_tuples = {
        Tuple({"1", "Alice"}),
        Tuple({"2", "Bob"}),
    };
    std::vector<Tuple> right_tuples = {
        Tuple({"1", "Eng"}),
        Tuple({"3", "Sales"}),
    };

    auto left = std::make_unique<MockExecutor>(std::move(left_tuples));
    auto right = std::make_unique<MockExecutor>(std::move(right_tuples));

    // ON 条件：左表 id（合并后下标 0）== 右表 id（合并后下标 2）
    ColEqCondition condition(0, 2);

    NestedLoopJoinExecutor join(std::move(left), std::move(right),
                                &condition,
                                /*left_schema=*/{"id", "name"},
                                /*right_schema=*/{"id", "dept"});
    join.Init();

    auto rows = DrainExecutor(&join);
    // 仅 (1, Alice) × (1, Eng) 匹配
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].GetValues()[0], "1");
    EXPECT_EQ(rows[0].GetValues()[1], "Alice");
    EXPECT_EQ(rows[0].GetValues()[2], "1");
    EXPECT_EQ(rows[0].GetValues()[3], "Eng");
}

// INNER JOIN：无匹配时返回空
TEST(NestedLoopJoinTest, InnerJoinNoMatchReturnsEmpty) {
    std::vector<Tuple> left_tuples = {
        Tuple({"10", "Charlie"}),
    };
    std::vector<Tuple> right_tuples = {
        Tuple({"20", "Diana"}),
    };

    auto left = std::make_unique<MockExecutor>(std::move(left_tuples));
    auto right = std::make_unique<MockExecutor>(std::move(right_tuples));

    ColEqCondition condition(0, 2);

    NestedLoopJoinExecutor join(std::move(left), std::move(right),
                                &condition,
                                /*left_schema=*/{"id", "name"},
                                /*right_schema=*/{"id", "dept"});
    join.Init();

    auto rows = DrainExecutor(&join);
    EXPECT_EQ(rows.size(), 0u);
}

// 空左表/右表：JOIN 结果为空
TEST(NestedLoopJoinTest, EmptyInputReturnsEmpty) {
    auto left = std::make_unique<MockExecutor>(std::vector<Tuple>{});
    auto right = std::make_unique<MockExecutor>(std::vector<Tuple>{Tuple({"1", "Eng"})});

    NestedLoopJoinExecutor join(std::move(left), std::move(right),
                                /*join_condition=*/nullptr,
                                /*left_schema=*/{"id", "name"},
                                /*right_schema=*/{"id", "dept"});
    join.Init();

    auto rows = DrainExecutor(&join);
    EXPECT_EQ(rows.size(), 0u);
}
