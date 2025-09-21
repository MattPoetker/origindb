#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <spdlog/spdlog.h>

#include "common/config.h"
#include "storage/storage_engine.h"
#include "sql/sql_engine.h"
#include "changefeed/changefeed_engine.h"
#include "websocket/websocket_server.h"

using namespace instantdb;

// Simple prototype demo showing the end-to-end flow:
// INSERT → WAL → Raft → Apply → Changefeed → WebSocket Client

int main(int argc, char* argv[]) {
    // Set log level to debug to see our detailed debugging output
    spdlog::set_level(spdlog::level::debug);

    std::cout << "🚀 InstantDB Prototype Demo\n";
    std::cout << "==============================\n\n";

    // Parse command line arguments
    uint16_t websocket_port = 8080; // Default port
    if (argc > 1) {
        try {
            websocket_port = static_cast<uint16_t>(std::stoi(argv[1]));
            std::cout << "Using WebSocket port: " << websocket_port << "\n\n";
        } catch (const std::exception& e) {
            std::cerr << "Invalid port number: " << argv[1] << ". Using default port 8080.\n\n";
        }
    }

    // Initialize configuration
    ServerConfig config;
    config.storage.data_dir = "/tmp/instantdb_demo";
    config.websocket.listen_address = "127.0.0.1:" + std::to_string(websocket_port);

    try {
        // 1. Initialize Storage Engine
        std::cout << "📦 Initializing Storage Engine...\n";
        auto storage = std::make_shared<StorageEngine>(config.storage);
        if (!storage->Initialize()) {
            std::cerr << "❌ Failed to initialize storage engine\n";
            return 1;
        }
        std::cout << "✅ Storage Engine ready\n\n";

        // 2. Create demo table (if it doesn't exist)
        auto existing_tables = storage->ListTables();
        bool users_table_exists = std::find(existing_tables.begin(), existing_tables.end(), "users") != existing_tables.end();

        if (users_table_exists) {
            std::cout << "🗂️  Table 'users' already exists (recovered from WAL)\n";
            std::cout << "✅ Using existing table\n\n";
        } else {
            std::cout << "🗂️  Creating demo table 'users'...\n";
            TableSchema users_schema;
            users_schema.name = "users";
            users_schema.columns = {
                {"id", DataType::INT64, false, true, std::monostate{}},
                {"name", DataType::STRING, false, false, std::monostate{}}
            };
            users_schema.primary_key = {"id"};

            if (!storage->CreateTable(users_schema)) {
                std::cerr << "❌ Failed to create users table\n";
                return 1;
            }
            std::cout << "✅ Table 'users' created\n\n";
        }

        // 3. Initialize SQL Engine
        std::cout << "🔍 Initializing SQL Engine...\n";
        auto sql_engine = std::make_shared<SqlEngine>(storage);
        if (!sql_engine->Initialize()) {
            std::cerr << "❌ Failed to initialize SQL engine\n";
            return 1;
        }
        std::cout << "✅ SQL Engine ready\n\n";

        // 4. Initialize Changefeed Engine
        std::cout << "📡 Initializing Changefeed Engine...\n";
        auto changefeed = std::make_shared<ChangefeedEngine>(storage, config.changefeed);
        if (!changefeed->Initialize() || !changefeed->Start()) {
            std::cerr << "❌ Failed to initialize changefeed engine\n";
            return 1;
        }
        std::cout << "✅ Changefeed Engine ready\n\n";

        // 5. Initialize WebSocket Server
        std::cout << "🌐 Initializing WebSocket Server...\n";
        auto websocket_server = std::make_shared<WebSocketServer>(websocket_port);
        websocket_server->SetChangefeedEngine(changefeed);

        if (!websocket_server->Start()) {
            std::cerr << "❌ Failed to start WebSocket server\n";
            return 1;
        }
        std::cout << "✅ WebSocket Server ready on port " << websocket_port << "\n\n";

        // 6. Simulate the end-to-end flow
        std::cout << "🎯 Starting End-to-End Demo Flow\n";
        std::cout << "=================================\n\n";

        // Create a subscription for changefeed events
        SubscriptionFilter filter;
        filter.table_pattern = "users";
        std::string sub_id = changefeed->CreateSubscription(filter, 0);

        // Set up changefeed callback to show events
        changefeed->Subscribe(sub_id, [](const std::string& subscription_id,
                                        const ChangefeedEvent& event) {
            std::cout << "📤 Changefeed Event Received:\n";
            std::cout << "   Subscription: " << subscription_id << "\n";
            std::cout << "   Offset: " << event.offset << "\n";
            std::cout << "   Table: " << event.table << "\n";
            std::cout << "   Operation: " << event.operation << "\n";
            std::cout << "   Key: " << std::string(event.key.begin(), event.key.end()) << "\n\n";
        });

        // Wait a moment for everything to settle
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 7. Check existing data and conditionally insert demo records
        std::cout << "🔍 Checking existing data in users table...\n";
        auto existing_data = sql_engine->Execute("SELECT * FROM users");

        if (existing_data.success && !existing_data.rows.empty()) {
            std::cout << "📋 Found " << existing_data.rows.size() << " existing records in users table\n";
            std::cout << "✅ Skipping demo inserts (data already exists)\n\n";
        } else {
            std::cout << "📝 Users table is empty, inserting demo data...\n";

            std::vector<std::string> demo_inserts = {
                "INSERT INTO users VALUES (1, 'Alice')",
                "INSERT INTO users VALUES (2, 'Bob')",
                "INSERT INTO users VALUES (3, 'Charlie')"
            };

            for (const auto& sql : demo_inserts) {
                std::cout << "💻 Executing SQL: " << sql << "\n";

                // Parse and execute SQL
                auto result = sql_engine->Execute(sql);

                if (result.success) {
                    std::cout << "✅ SQL executed successfully\n";

                    // For demo purposes, manually emit changefeed event
                    // (In real implementation, this would be done in the commit path)
                    ChangefeedEvent event;
                    event.table = "users";
                    event.operation = "INSERT";
                    event.transaction_id = "demo-txn-" + std::to_string(std::time(nullptr));

                    // Extract key from SQL (simplified)
                    size_t start = sql.find('(') + 1;
                    size_t end = sql.find(',', start);
                    std::string key = sql.substr(start, end - start);
                    event.key = std::vector<uint8_t>(key.begin(), key.end());

                    // Extract name
                    start = sql.find(',', start) + 2; // Skip comma and space
                    end = sql.find(')', start);
                    std::string name = sql.substr(start, end - start);
                    if (name.front() == '\'' && name.back() == '\'') {
                        name = name.substr(1, name.length() - 2);
                    }
                    event.new_value = std::vector<uint8_t>(name.begin(), name.end());

                    changefeed->PublishEvent(event);
                    std::cout << "📡 Change event published\n";
                } else {
                    std::cout << "❌ SQL execution failed: " << result.error << "\n";
                }

                std::cout << "---\n";
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        // 8. Show table contents
        std::cout << "\n📊 Current table contents:\n";
        auto select_result = sql_engine->Execute("SELECT * FROM users");
        if (select_result.success) {
            std::cout << "Users table:\n";
            for (const auto& row : select_result.rows) {
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
                            std::cout << v;
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

        // 9. Display statistics
        std::cout << "\n📈 System Statistics:\n";
        auto storage_stats = storage->GetStats();
        std::cout << "  Tables: " << storage_stats.total_tables << "\n";
        std::cout << "  Rows: " << storage_stats.total_rows << "\n";
        std::cout << "  Storage bytes: " << storage_stats.total_bytes << "\n";

        auto cf_metrics = changefeed->GetMetrics();
        std::cout << "  Events published: " << cf_metrics.total_events_published << "\n";
        std::cout << "  Active subscriptions: " << cf_metrics.active_subscriptions << "\n";
        std::cout << "  WebSocket connections: " << websocket_server->GetActiveConnections() << "\n";

        std::cout << "\n🎉 Minimal Demo completed successfully!\n";
        std::cout << "\nThis demonstrates the core flow:\n";
        std::cout << "  SQL → Storage → WAL → Changefeed Events\n\n";
        std::cout << "Full version would add:\n";
        std::cout << "  - Raft consensus for clustering\n";
        std::cout << "  - WASM module execution\n";
        std::cout << "  - Complete SQL parser\n";
        std::cout << "  - Authentication and authorization\n\n";

        std::cout << "🔗 WebSocket Endpoint: ws://localhost:" << websocket_port << "\n";
        std::cout << "   Connect with Postman or your WebSocket client!\n\n";
        std::cout << "💡 Usage: ./instantdb_demo [port]\n";
        std::cout << "   Example: ./instantdb_demo 9090\n\n";

        std::cout << "Press Enter to shutdown...\n";
        std::cin.get();

        // Cleanup
        std::cout << "🛑 Shutting down...\n";
        websocket_server->Stop();
        changefeed->Stop();
        storage->Shutdown();

        std::cout << "✅ Shutdown complete\n";

    } catch (const std::exception& e) {
        std::cerr << "💥 Demo failed with exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}