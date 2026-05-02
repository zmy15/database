#include "db_engine.h"
#include <iostream>
#include <string>

using namespace db;

int main() {
    std::cout << "=== Database System Initializing ===" << std::endl;

    // 创建数据库引擎，缓冲池 10 页
    DBEngine engine("test.db", 10);

    // ==========================================
    // 测试: CREATE TABLE
    // ==========================================
    std::cout << "\n=== [Test] CREATE TABLE ===" << std::endl;
    engine.ExecuteQuery("CREATE TABLE users (name, data)");

    // ==========================================
    // 测试: INSERT INTO (批量插入)
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
