#include "db_engine.h"
#include <iostream>
#include <string>

using namespace db;

int main() {
    std::cout << "=== Database System Initializing ===" << std::endl;

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

    std::cout << "\n=== All Tests Complete ===" << std::endl;
    return 0;
}
