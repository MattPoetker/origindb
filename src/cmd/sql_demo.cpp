#include <iostream>
#include <string>
#include <spdlog/spdlog.h>

#include "common/config.h"
#include "storage/storage_engine.h"
#include "sql/sql_engine.h"

using namespace origindb;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " \"SQL_STATEMENT\"\n";
        std::cout << "Examples:\n";
        std::cout << "  " << argv[0] << " \"CREATE TABLE users (id INT64 PRIMARY KEY, name STRING)\"\n";
        std::cout << "  " << argv[0] << " \"INSERT INTO users VALUES (1, 'Alice')\"\n";
        std::cout << "  " << argv[0] << " \"SELECT * FROM users\"\n";
        return 1;
    }

    std::string sql = argv[1];

    // Set log level to info to see operations
    spdlog::set_level(spdlog::level::info);

    try {
        // Initialize storage engine
        StorageConfig storage_config;
        storage_config.data_dir = "./origindb_data";

        auto storage = std::make_shared<StorageEngine>(storage_config);
        if (!storage->Initialize()) {
            std::cerr << "❌ Failed to initialize storage engine\n";
            return 1;
        }

        // Initialize SQL engine
        auto sql_engine = std::make_shared<SqlEngine>(storage);
        if (!sql_engine->Initialize()) {
            std::cerr << "❌ Failed to initialize SQL engine\n";
            return 1;
        }

        std::cout << "🔍 Executing SQL: " << sql << "\n";

        // Execute the SQL
        auto result = sql_engine->Execute(sql);

        if (result.success) {
            std::cout << "✅ SQL executed successfully\n";
            std::cout << "⏱️  Execution time: " << result.execution_time_ms << " ms\n";
            std::cout << "📊 Rows affected: " << result.rows_affected << "\n";

            if (!result.rows.empty()) {
                std::cout << "\n📋 Results:\n";
                for (const auto& row : result.rows) {
                    std::cout << "  Key: " << row.key;
                    for (const auto& [col_name, value] : row.columns) {
                        std::cout << ", " << col_name << ": ";
                        std::visit([](const auto& v) {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, std::monostate>) {
                                std::cout << "NULL";
                            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                                std::cout << "[binary data]";
                            } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                                std::cout << "[timestamp]";
                            } else if constexpr (std::is_same_v<T, std::string>) {
                                std::cout << "\"" << v << "\"";
                            } else if constexpr (std::is_same_v<T, int32_t>) {
                                std::cout << v;
                            } else if constexpr (std::is_same_v<T, int64_t>) {
                                std::cout << v;
                            } else if constexpr (std::is_same_v<T, float>) {
                                std::cout << v;
                            } else if constexpr (std::is_same_v<T, double>) {
                                std::cout << v;
                            } else if constexpr (std::is_same_v<T, bool>) {
                                std::cout << (v ? "true" : "false");
                            } else {
                                std::cout << "[unknown type]";
                            }
                        }, value);
                    }
                    std::cout << "\n";
                }
            }
        } else {
            std::cout << "❌ SQL execution failed: " << result.error << "\n";
            return 1;
        }

        // Cleanup
        storage->Shutdown();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "💥 Error: " << e.what() << "\n";
        return 1;
    }
}