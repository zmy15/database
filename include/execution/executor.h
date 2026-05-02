#pragma once

#include "common/config.h"
#include "storage/tuple.h"

namespace db {

// 执行器基类（火山模型/迭代器模型）
class AbstractExecutor {
public:
    virtual ~AbstractExecutor() = default;
    virtual void Init() = 0;                    // 初始化（如打开文件，重置游标）
    virtual bool Next(Tuple* tuple) = 0;        // 获取下一行数据
};

} // namespace db
