#include <iostream>
#include <memory>
#include <signal.h>
#include <thread>
#include <fstream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "common/config.h"
#include "storage/storage_engine.h"
#include "sql/sql_engine.h"
#include "changefeed/changefeed_engine.h"
#include "changefeed/sql_subscription.h"
#include "websocket/websocket_server.h"
#include "wasm/wasm_engine.h"
#include "wasm/wasm_subscription.h"

#ifdef GRPC_AVAILABLE
#include "grpc/grpc_server.h"
#endif

using namespace instantdb;

class InstantDBServer {
public:
    InstantDBServer(const ServerConfig& config)
        : config_(config), shutdown_requested_(false) {
        InitializeLogging();
    }

    ~InstantDBServer() {
        Shutdown();
    }

    bool Initialize() {
        spdlog::info("🚀 Starting InstantDB Server");
        spdlog::info("=====================================");

        // Initialize storage engine
        spdlog::info("📦 Initializing Storage Engine...");
        storage_engine_ = std::make_shared<StorageEngine>(config_.storage);
        if (!storage_engine_->Initialize()) {
            spdlog::error("❌ Failed to initialize storage engine");
            return false;
        }
        spdlog::info("✅ Storage Engine ready");

        // Initialize changefeed engine first (needed by SQL engine)
        spdlog::info("📡 Initializing Changefeed Engine...");
        changefeed_engine_ = std::make_shared<ChangefeedEngine>(storage_engine_, config_.changefeed);
        if (!changefeed_engine_->Initialize() || !changefeed_engine_->Start()) {
            spdlog::error("❌ Failed to initialize changefeed engine");
            return false;
        }
        spdlog::info("✅ Changefeed Engine ready");

        // Wire storage engine to automatically emit changefeed events
        storage_engine_->SetChangefeedEngine(changefeed_engine_);
        spdlog::info("🔗 Storage Engine linked to Changefeed Engine for automatic event emission");

        // Initialize SQL engine with changefeed support
        spdlog::info("🔍 Initializing SQL Engine...");
        sql_engine_ = std::make_shared<SqlEngine>(storage_engine_, changefeed_engine_);
        if (!sql_engine_->Initialize()) {
            spdlog::error("❌ Failed to initialize SQL engine");
            return false;
        }
        spdlog::info("✅ SQL Engine ready with changefeed support");

        // Initialize WASM engine
        spdlog::info("⚡ Initializing WASM Engine...");
        wasm_engine_ = std::make_shared<WasmEngine>(storage_engine_, changefeed_engine_);
        if (!wasm_engine_->Initialize()) {
            spdlog::error("❌ Failed to initialize WASM engine");
            return false;
        }
        spdlog::info("✅ WASM Engine ready");

        // Initialize WASM subscription manager
        spdlog::info("🔔 Initializing WASM Subscription Manager...");
        wasm_subscription_manager_ = std::make_shared<WasmSubscriptionManager>(
            wasm_engine_, changefeed_engine_, storage_engine_);
        if (!wasm_subscription_manager_->Initialize() || !wasm_subscription_manager_->Start()) {
            spdlog::error("❌ Failed to initialize WASM subscription manager");
            return false;
        }
        spdlog::info("✅ WASM Subscription Manager ready");

        // Initialize SQL subscription manager
        spdlog::info("📋 Initializing SQL Subscription Manager...");
        sql_subscription_manager_ = std::make_shared<SqlSubscriptionManager>();
        spdlog::info("✅ SQL Subscription Manager ready");

        // Initialize WebSocket server
        spdlog::info("🌐 Initializing WebSocket Server...");

        // Extract port from listen_address (format: "host:port")
        uint16_t ws_port = 8080; // default
        auto colon_pos = config_.websocket.listen_address.find_last_of(':');
        if (colon_pos != std::string::npos) {
            ws_port = static_cast<uint16_t>(std::stoi(config_.websocket.listen_address.substr(colon_pos + 1)));
        }

        websocket_server_ = std::make_shared<WebSocketServer>(ws_port);
        websocket_server_->SetChangefeedEngine(changefeed_engine_);
        websocket_server_->SetWasmSubscriptionManager(wasm_subscription_manager_);
        websocket_server_->SetSqlSubscriptionManager(sql_subscription_manager_);
        websocket_server_->SetSqlEngine(sql_engine_);
        websocket_server_->SetStorageEngine(storage_engine_);
        if (!websocket_server_->Start()) {
            spdlog::error("❌ Failed to start WebSocket server");
            return false;
        }
        spdlog::info("✅ WebSocket Server ready on {}", config_.websocket.listen_address);

#ifdef GRPC_AVAILABLE
        // Initialize gRPC server
        spdlog::info("🔌 Initializing gRPC Server...");
        GrpcServer::Config grpc_config;
        grpc_config.listen_address = config_.grpc.listen_address;
        grpc_config.max_message_size = static_cast<int>(config_.grpc.max_message_size);

        grpc_server_ = std::make_shared<GrpcServer>(grpc_config, sql_engine_, storage_engine_,
                                                   changefeed_engine_, websocket_server_, wasm_engine_);
        if (!grpc_server_->Start()) {
            spdlog::error("❌ Failed to start gRPC server");
            return false;
        }
        spdlog::info("✅ gRPC Server ready on {}", config_.grpc.listen_address);
#else
        spdlog::info("ℹ️  gRPC Server not available (compiled without gRPC support)");
#endif

        spdlog::info("");
        spdlog::info("🎯 InstantDB Server fully initialized!");
        spdlog::info("=====================================");
        spdlog::info("📊 Server Status:");
        spdlog::info("  Storage: {} tables", storage_engine_->ListTables().size());
        spdlog::info("  WebSocket: {} active connections", websocket_server_->GetActiveConnections());
        spdlog::info("  Changefeed: {} active subscriptions", changefeed_engine_->GetMetrics().active_subscriptions);
        spdlog::info("  WASM: {} loaded modules", wasm_engine_->ListModules().size());
        spdlog::info("  WASM Subscriptions: {} active", wasm_subscription_manager_->GetMetrics().active_subscriptions);
        spdlog::info("");
        spdlog::info("🔗 Endpoints:");
        // Extract port for display
        uint16_t display_port = 8080;
        auto display_colon_pos = config_.websocket.listen_address.find_last_of(':');
        if (display_colon_pos != std::string::npos) {
            display_port = static_cast<uint16_t>(std::stoi(config_.websocket.listen_address.substr(display_colon_pos + 1)));
        }
        spdlog::info("  WebSocket: ws://localhost:{}", display_port);

#ifdef GRPC_AVAILABLE
        // Extract gRPC port for display
        std::string grpc_display = config_.grpc.listen_address;
        if (grpc_display.find("0.0.0.0:") == 0) {
            grpc_display = "localhost:" + grpc_display.substr(8);
        }
        spdlog::info("  gRPC API: {}", grpc_display);
#endif
        spdlog::info("");

        return true;
    }

    void Run() {
        spdlog::info("💡 Server running - send SIGINT (Ctrl+C) to shutdown gracefully");

        // Wait for shutdown signal
        while (!shutdown_requested_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Log periodic stats
            LogPeriodicStats();
        }
    }

    void Shutdown() {
        if (shutdown_requested_) return;

        spdlog::info("🛑 Shutting down InstantDB Server...");
        shutdown_requested_ = true;

#ifdef GRPC_AVAILABLE
        if (grpc_server_) {
            grpc_server_->Stop();
            spdlog::info("✅ gRPC server stopped");
        }
#endif

        if (websocket_server_) {
            websocket_server_->Stop();
            spdlog::info("✅ WebSocket server stopped");
        }

        if (wasm_engine_) {
            wasm_engine_->Shutdown();
            spdlog::info("✅ WASM engine stopped");
        }

        if (changefeed_engine_) {
            changefeed_engine_->Stop();
            spdlog::info("✅ Changefeed engine stopped");
        }

        if (storage_engine_) {
            storage_engine_->Shutdown();
            spdlog::info("✅ Storage engine shutdown");
        }

        spdlog::info("🏁 Server shutdown complete");
    }

    // Signal handler
    void RequestShutdown() {
        shutdown_requested_ = true;
    }

    // Public API for executing SQL
    SqlResult ExecuteSQL(const std::string& sql) {
        return sql_engine_->Execute(sql);
    }

    // Get server stats
    struct ServerStats {
        size_t total_tables;
        size_t total_rows;
        size_t storage_bytes;
        size_t active_connections;
        size_t active_subscriptions;
        size_t events_published;
        size_t loaded_modules;
    };

    ServerStats GetStats() const {
        auto storage_stats = storage_engine_->GetStats();
        auto cf_metrics = changefeed_engine_->GetMetrics();

        return {
            .total_tables = storage_stats.total_tables,
            .total_rows = storage_stats.total_rows,
            .storage_bytes = storage_stats.total_bytes,
            .active_connections = websocket_server_->GetActiveConnections(),
            .active_subscriptions = cf_metrics.active_subscriptions,
            .events_published = cf_metrics.total_events_published,
            .loaded_modules = wasm_engine_->ListModules().size()
        };
    }

private:
    void InitializeLogging() {
        // Set log level
        spdlog::set_level(spdlog::level::info);

        // Create color console sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::info);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        // Create file sink if configured
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(console_sink);

        if (!config_.logging.log_file.empty()) {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config_.logging.log_file, 1048576 * 5, 3);
            file_sink->set_level(spdlog::level::trace);
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
            sinks.push_back(file_sink);
        }

        // Create logger
        auto logger = std::make_shared<spdlog::logger>("instantdb", sinks.begin(), sinks.end());
        logger->set_level(config_.logging.level);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(1));
    }

    void LogPeriodicStats() {
        static auto last_log = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();

        // Log stats every 30 seconds
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 30) {
            auto stats = GetStats();
            spdlog::info("📈 Server Stats: {} tables, {} rows, {} bytes, {} WS connections, {} subscriptions, {} events, {} modules",
                        stats.total_tables, stats.total_rows, stats.storage_bytes,
                        stats.active_connections, stats.active_subscriptions, stats.events_published, stats.loaded_modules);
            last_log = now;
        }
    }

private:
    ServerConfig config_;
    std::atomic<bool> shutdown_requested_;

    std::shared_ptr<StorageEngine> storage_engine_;
    std::shared_ptr<SqlEngine> sql_engine_;
    std::shared_ptr<ChangefeedEngine> changefeed_engine_;
    std::shared_ptr<SqlSubscriptionManager> sql_subscription_manager_;
    std::shared_ptr<WebSocketServer> websocket_server_;
    std::shared_ptr<WasmEngine> wasm_engine_;
    std::shared_ptr<WasmSubscriptionManager> wasm_subscription_manager_;
#ifdef GRPC_AVAILABLE
    std::shared_ptr<GrpcServer> grpc_server_;
#endif
};

// Global server instance for signal handling
static InstantDBServer* g_server = nullptr;

// Signal handler
void signal_handler(int signal) {
    spdlog::info("Received signal {}, requesting graceful shutdown...", signal);
    if (g_server) {
        g_server->RequestShutdown();
    }
}

// Configuration parsing
ServerConfig ParseConfig(const std::string& config_file) {
    ServerConfig config;

    // TODO: Parse actual config file when needed
    (void)config_file; // Suppress unused parameter warning

    // Set defaults
    config.storage.data_dir = "./instantdb_data";
    config.websocket.listen_address = "0.0.0.0:8080";
    config.grpc.listen_address = "0.0.0.0:50051";
    config.logging.level = spdlog::level::info;
    config.logging.log_file = "./logs/instantdb.log";

    // TODO: Parse actual config file (JSON/YAML)
    // For now, use environment variables as overrides
    const char* data_dir = getenv("INSTANTDB_DATA_DIR");
    if (data_dir) {
        config.storage.data_dir = data_dir;
    }

    const char* ws_port = getenv("INSTANTDB_WS_PORT");
    if (ws_port) {
        config.websocket.listen_address = "0.0.0.0:" + std::string(ws_port);
    }

    const char* grpc_port = getenv("INSTANTDB_GRPC_PORT");
    if (grpc_port) {
        config.grpc.listen_address = "0.0.0.0:" + std::string(grpc_port);
    }

    const char* log_level = getenv("INSTANTDB_LOG_LEVEL");
    if (log_level) {
        std::string level_str = log_level;
        if (level_str == "trace") config.logging.level = spdlog::level::trace;
        else if (level_str == "debug") config.logging.level = spdlog::level::debug;
        else if (level_str == "info") config.logging.level = spdlog::level::info;
        else if (level_str == "warn") config.logging.level = spdlog::level::warn;
        else if (level_str == "error") config.logging.level = spdlog::level::err;
    }

    return config;
}

void PrintUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  -p, --port PORT          WebSocket port (default: 8080)\n";
    std::cout << "  -g, --grpc-port PORT     gRPC port (default: 50051)\n";
    std::cout << "  -d, --data-dir DIR       Data directory (default: ./instantdb_data)\n";
    std::cout << "  -l, --log-level LEVEL    Log level: trace,debug,info,warn,error (default: info)\n";
    std::cout << "  -c, --config FILE        Config file path (default: instantdb.conf)\n";
    std::cout << "  -h, --help               Show this help message\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << "                    # Start with defaults\n";
    std::cout << "  " << program_name << " -p 9090            # WebSocket on port 9090\n";
    std::cout << "  " << program_name << " -g 50052           # gRPC on port 50052\n";
    std::cout << "  " << program_name << " 9090               # WebSocket port 9090 (short form)\n";
    std::cout << "\nEnvironment Variables:\n";
    std::cout << "  INSTANTDB_WS_PORT       WebSocket port\n";
    std::cout << "  INSTANTDB_GRPC_PORT     gRPC port\n";
    std::cout << "  INSTANTDB_DATA_DIR      Data directory\n";
    std::cout << "  INSTANTDB_LOG_LEVEL     Log level\n";
}

int main(int argc, char* argv[]) {
    try {
        // Parse command line arguments
        std::string config_file = "instantdb.conf";
        std::string port_arg;
        std::string grpc_port_arg;
        std::string data_dir_arg;
        std::string log_level_arg;

        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            if (arg == "-h" || arg == "--help") {
                PrintUsage(argv[0]);
                return 0;
            } else if (arg == "-p" || arg == "--port") {
                if (i + 1 < argc) {
                    port_arg = argv[++i];
                } else {
                    std::cerr << "Error: " << arg << " requires a port number\n";
                    return 1;
                }
            } else if (arg == "-g" || arg == "--grpc-port") {
                if (i + 1 < argc) {
                    grpc_port_arg = argv[++i];
                } else {
                    std::cerr << "Error: " << arg << " requires a port number\n";
                    return 1;
                }
            } else if (arg == "-d" || arg == "--data-dir") {
                if (i + 1 < argc) {
                    data_dir_arg = argv[++i];
                } else {
                    std::cerr << "Error: " << arg << " requires a directory path\n";
                    return 1;
                }
            } else if (arg == "-l" || arg == "--log-level") {
                if (i + 1 < argc) {
                    log_level_arg = argv[++i];
                } else {
                    std::cerr << "Error: " << arg << " requires a log level\n";
                    return 1;
                }
            } else if (arg == "-c" || arg == "--config") {
                if (i + 1 < argc) {
                    config_file = argv[++i];
                } else {
                    std::cerr << "Error: " << arg << " requires a config file path\n";
                    return 1;
                }
            } else if (arg[0] != '-') {
                // Assume it's a port number (short form)
                port_arg = arg;
            } else {
                std::cerr << "Error: Unknown option " << arg << "\n";
                PrintUsage(argv[0]);
                return 1;
            }
        }

        ServerConfig config = ParseConfig(config_file);

        // Override config with command line arguments
        if (!port_arg.empty()) {
            config.websocket.listen_address = "0.0.0.0:" + port_arg;
        }
        if (!grpc_port_arg.empty()) {
            config.grpc.listen_address = "0.0.0.0:" + grpc_port_arg;
        }
        if (!data_dir_arg.empty()) {
            config.storage.data_dir = data_dir_arg;
        }
        if (!log_level_arg.empty()) {
            if (log_level_arg == "trace") config.logging.level = spdlog::level::trace;
            else if (log_level_arg == "debug") config.logging.level = spdlog::level::debug;
            else if (log_level_arg == "info") config.logging.level = spdlog::level::info;
            else if (log_level_arg == "warn") config.logging.level = spdlog::level::warn;
            else if (log_level_arg == "error") config.logging.level = spdlog::level::err;
            else {
                std::cerr << "Error: Invalid log level '" << log_level_arg << "'\n";
                std::cerr << "Valid levels: trace, debug, info, warn, error\n";
                return 1;
            }
        }

        // Create server
        InstantDBServer server(config);
        g_server = &server;

        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // Initialize and run
        if (!server.Initialize()) {
            spdlog::error("💥 Failed to initialize server");
            return 1;
        }

        server.Run();

        // Cleanup
        g_server = nullptr;
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("💥 Server failed with exception: {}", e.what());
        return 1;
    }
}