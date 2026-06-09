#include "db_engine.h"
#include <iostream>
#include <string>
#include <system_error>

using namespace db;

int main() {
    std::cout << "=== Database System Initializing ===" << std::endl;

    try {
        // 创建数据库引擎，缓冲池 20 页（更多页面以容纳多表测试）
        DBEngine engine("test.db", 20);
        // 测试 1: CREATE TABLE（多表）
        std::cout << "\n=== [Test 1] CREATE TABLE ===" << std::endl;
        engine.ExecuteQuery("CREATE TABLE users (name, data)");
        engine.ExecuteQuery("CREATE TABLE products (pid, pname, price)");
        std::cout << "Created 2 tables: users, products." << std::endl;
        // 测试 2: INSERT INTO（两表分别插入）
        std::cout << "\n=== [Test 2] INSERT INTO users (100 records) ===" << std::endl;
        for (int i = 0; i < 100; ++i) {
            std::string name = "User_" + std::to_string(i);
            std::string data = std::string(100, 'X');
            std::string sql = "INSERT INTO users VALUES ('" + name + "', '" + data + "')";
            engine.ExecuteQuery(sql);
        }
        std::cout << "Inserted 100 records into users." << std::endl;
        std::cout << "\n=== [Test 2b] INSERT INTO products (3 records) ===" << std::endl;
        engine.ExecuteQuery("INSERT INTO products VALUES ('100', 'Laptop', '999')");
        engine.ExecuteQuery("INSERT INTO products VALUES ('200', 'Mouse', '25')");
        engine.ExecuteQuery("INSERT INTO products VALUES ('300', 'Keyboard', '75')");
        std::cout << "Inserted 3 records into products." << std::endl;

        // 测试 3: SELECT * FROM（全表扫描）
        std::cout << "\n=== [Test 3] SELECT * FROM users (100 rows) ===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users");  // 应输出 100 行

        std::cout << "\n=== [Test 3b] SELECT * FROM products (3 rows) ===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products"); // 应输出 3 行

        // ==========================================
        // 测试 4: SELECT 指定列
        // ==========================================
        std::cout << "\n=== [Test 4] SELECT name FROM users ===" << std::endl;
        engine.ExecuteQuery("SELECT name FROM users");
        // ==========================================
        // 测试 5: SELECT with WHERE（解析器支持 WHERE 语法）
        // ==========================================
        std::cout << "\n=== [Test 5] SELECT * FROM users WHERE name = 'User_50' ===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users WHERE name = 'User_50'");

        // ==========================================
        // 测试 6: DELETE 全部行
        // ==========================================
        std::cout << "\n=== [Test 6] DELETE FROM products ===" << std::endl;
        engine.ExecuteQuery("DELETE FROM products");

        std::cout << "\n=== [Test 6b] SELECT after DELETE (should be empty) ===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products");

        // ==========================================
        // 测试 7: UPDATE 全部行
        // ==========================================
        std::cout << "\n=== [Test 7] UPDATE users SET data = 'UPDATED' ===" << std::endl;
        engine.ExecuteQuery("UPDATE users SET data = 'UPDATED'");

        std::cout << "\n=== [Test 7b] SELECT after UPDATE（前3行验证）===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users");

        // ==========================================
        // 测试 8: DELETE with WHERE（删除特定行后验证）
        // ==========================================
        std::cout << "\n=== [Test 8] DELETE FROM users WHERE name = 'User_0' ===" << std::endl;
        engine.ExecuteQuery("DELETE FROM users WHERE name = 'User_0'");

        std::cout << "\n=== [Test 8b] SELECT after conditional DELETE ===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users");

        // ==========================================
        // 测试 9: 多表独立性验证
        // ==========================================
        std::cout << "\n=== [Test 9] Re-insert into products ===" << std::endl;
        engine.ExecuteQuery("INSERT INTO products VALUES ('400', 'Monitor', '299')");
        engine.ExecuteQuery("INSERT INTO products VALUES ('500', 'Desk', '450')");
        engine.ExecuteQuery("SELECT * FROM products");

        std::cout << "\n=== [Test 9b] users table still intact ===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users");

        // ==========================================
        std::cout << "\n=== [Test] INSERT INTO ===" << std::endl;
        for (int i = 0; i < 800; ++i) {
            std::string name = "User_" + std::to_string(i);
            std::string data = std::string(100, 'X');
            std::string sql = "INSERT INTO users VALUES ('" + name + "', '" + data + "')";
            engine.ExecuteQuery(sql);
        }
        std::cout << "Successfully inserted 800 records via SQL." << std::endl;

        // ==========================================
        // 测试: SELECT * FROM
        // ==========================================
        std::cout << "\n=== [Test] SELECT * FROM users ===" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users");

        // ==========================================
        // 测试 10: 复杂 WHERE 条件 (AND / OR / 数值比较)
        // ==========================================
        std::cout << "\n=== [Test 10] Complex WHERE conditions ===" << std::endl;
        std::cout << "--- AND condition ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users WHERE name = 'User_10' AND data = 'UPDATED'");
        std::cout << "--- OR condition ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM users WHERE name = 'User_1' OR name = 'User_2'");
        std::cout << "--- Numeric comparison (>  <) ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE price > '50'");
        engine.ExecuteQuery("SELECT * FROM products WHERE price < '200'");
        std::cout << "--- Combined AND + numeric ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE price > '50' AND price < '500'");
        std::cout << "--- NOT_EQUAL (<>) ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE pname <> 'Monitor'");

        // ==========================================
        // 测试 11: 聚合查询 (COUNT / SUM / AVG / MIN / MAX)
        // ==========================================
        std::cout << "\n=== [Test 11] Aggregate Queries ===" << std::endl;
        engine.ExecuteQuery("SELECT COUNT(*) FROM users");
        engine.ExecuteQuery("SELECT COUNT(pname) FROM products");
        engine.ExecuteQuery("SELECT SUM(price) FROM products");
        engine.ExecuteQuery("SELECT AVG(price) FROM products");
        engine.ExecuteQuery("SELECT MIN(price) FROM products");
        engine.ExecuteQuery("SELECT MAX(price) FROM products");

        // ==========================================
        // 测试 12: GROUP BY + ORDER BY
        // ==========================================
        std::cout << "\n=== [Test 12] GROUP BY ===" << std::endl;
        engine.ExecuteQuery("SELECT data, COUNT(*) FROM users GROUP BY data");
        std::cout << "\n--- ORDER BY ASC ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products ORDER BY price");
        std::cout << "\n--- ORDER BY DESC ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products ORDER BY price DESC");

        // ==========================================
        // 测试 13: 条件 UPDATE（带 WHERE 的更新）
        // ==========================================
        std::cout << "\n=== [Test 13] Conditional UPDATE ===" << std::endl;
        engine.ExecuteQuery("UPDATE users SET data = 'SPECIAL' WHERE name = 'User_5'");
        engine.ExecuteQuery("SELECT * FROM users WHERE name = 'User_5'");
        engine.ExecuteQuery("UPDATE users SET data = 'RESTORED' WHERE name = 'User_5'");
        std::cout << "Restored User_5 data." << std::endl;

        // ==========================================
        // 测试 14: 事务控制 - 单语句回滚 (BEGIN / INSERT / ABORT)
        // ==========================================
        std::cout << "\n=== [Test 14] Transaction Control ===" << std::endl;
        engine.ExecuteQuery("BEGIN");
        engine.ExecuteQuery("INSERT INTO products VALUES ('999', 'TxnProduct', '99.99')");
        engine.ExecuteQuery("SELECT * FROM products WHERE pid = '999'");
        engine.ExecuteQuery("ABORT");
        std::cout << "After ABORT (事务已回滚, 数据不应存在):" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE pid = '999'");
        engine.ExecuteQuery("BEGIN");
        engine.ExecuteQuery("COMMIT");

        // ==========================================
        // 测试 14b: 多 DML 事务回滚 (INSERT + UPDATE 全部 ABORT)
        // ==========================================
        std::cout << "\n=== [Test 14b] Multi-DML Transaction Rollback (INSERT+UPDATE) ===" << std::endl;
        std::cout << "--- Step 1: BEGIN + INSERT + UPDATE ---" << std::endl;
        engine.ExecuteQuery("BEGIN");
        engine.ExecuteQuery("INSERT INTO products VALUES ('777', 'RollbackTest', '777.77')");
        engine.ExecuteQuery("UPDATE products SET price = '888.88' WHERE pid = '777'");
        std::cout << "--- Step 2: SELECT within transaction (should see the data) ---" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE pid = '777'");
        std::cout << "--- Step 3: ABORT (rollback all changes) ---" << std::endl;
        engine.ExecuteQuery("ABORT");
        std::cout << "After ABORT (should return 0 rows):" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE pid = '777'");

        // ==========================================
        // 测试 14c: 多 DML 事务回滚 (INSERT + DELETE 全部 ABORT)
        // ==========================================
        std::cout << "\n=== [Test 14c] Multi-DML Transaction Rollback (INSERT+DELETE) ===" << std::endl;
        engine.ExecuteQuery("BEGIN");
        engine.ExecuteQuery("INSERT INTO products VALUES ('666', 'DeleteTest', '666.66')");
        engine.ExecuteQuery("DELETE FROM products WHERE pid = '666'");
        std::cout << "After ABORT (data should NOT exist, DELETE was rolled back):" << std::endl;
        engine.ExecuteQuery("ABORT");
        engine.ExecuteQuery("SELECT * FROM products WHERE pid = '666'");

        // ==========================================
        // 测试 14d: auto-commit 不受影响（无 BEGIN 的 DML 照常提交）
        // ==========================================
        std::cout << "\n=== [Test 14d] Auto-commit DML (no BEGIN) ===" << std::endl;
        engine.ExecuteQuery("INSERT INTO products VALUES ('555', 'AutoCommit', '555.55')");
        std::cout << "After auto-commit INSERT (should find the row):" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE pid = '555'");
        engine.ExecuteQuery("DELETE FROM products WHERE pid = '555'");
        std::cout << "After auto-commit DELETE (should return 0 rows):" << std::endl;
        engine.ExecuteQuery("SELECT * FROM products WHERE pid = '555'");

        // ==========================================
        // 测试 DROP TABLE
        // ==========================================
        std::cout << "\n=== [Test DROP TABLE] ===" << std::endl;
        // 创建临时表
        engine.ExecuteQuery("CREATE TABLE temp_drop (col1, col2)");
        engine.ExecuteQuery("INSERT INTO temp_drop VALUES ('hello', 'world')");
        engine.ExecuteQuery("INSERT INTO temp_drop VALUES ('foo', 'bar')");
        std::cout << "Before DROP:" << std::endl;
        engine.ExecuteQuery("SELECT * FROM temp_drop");
        // 执行 DROP TABLE
        engine.ExecuteQuery("DROP TABLE temp_drop");
        // 验证表已删除（应报错）
        std::cout << "After DROP (expect error):" << std::endl;
        engine.ExecuteQuery("SELECT * FROM temp_drop");
        // 验证 DROP 不存在的表应报错
        std::cout << "DROP non-existent table (expect error):" << std::endl;
        engine.ExecuteQuery("DROP TABLE non_existent");

        std::cout << "\n=== All Tests Complete ===" << std::endl;
        return 0;
    } catch (const std::system_error& e) {
        std::cerr << "[DEBUG] system_error caught: " << e.what() << " code=" << e.code() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[DEBUG] exception caught: " << e.what() << std::endl;
        return 1;
    }
}
