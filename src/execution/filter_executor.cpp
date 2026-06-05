 #include "execution/filter_executor.h"
 
 namespace db {
 
 void FilterExecutor::Init() {
     if (child_) {
         child_->Init();
     }
 }
 
 bool FilterExecutor::Next(Tuple* tuple) {
     if (!child_) {
         return false;
     }
 
     Tuple temp;
     while (child_->Next(&temp)) {
         // 无 WHERE 条件，或表达式求值为 true，则返回该行
         if (!predicate_ || predicate_->Evaluate(temp, schema_)) {
             *tuple = std::move(temp);
             return true;
         }
     }
     return false;
 }
 
 } // namespace db
