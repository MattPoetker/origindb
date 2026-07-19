#include <iostream>
#include <memory>
#include <signal.h>
#include <thread>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <chrono>
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
#include "wasm/module_store.h"
#include "wasm/tick_scheduler.h"
#include "common/auth.h"

#include <filesystem>

#ifdef GRPC_AVAILABLE
#include "grpc/grpc_server.h"
#endif

using namespace origindb;

// Global shutdown flag accessible from WASM engine
std::atomic<bool> g_shutdown_requested = false;

// Signal handling with timeout enforcement
std::atomic<bool> g_signal_received = false;
std::atomic<int> g_signal_count = 0;

class OriginDBServer {
public:
    OriginDBServer(const ServerConfig& config)
        : config_(config), shutdown_requested_(false) {
        InitializeLogging();
    }

    ~OriginDBServer() {
        Shutdown();
    }

    bool Initialize() {
        spdlog::info("🚀 Starting OriginDB Server");
        spdlog::info("=====================================");

        // Initialize authentication (before any listener starts)
        if (config_.auth.enabled) {
            auth_ = std::make_shared<AuthManager>(
                std::filesystem::path(config_.storage.data_dir) / "auth",
                config_.auth.admin_token, config_.auth.client_token);
            if (!auth_->Initialize()) {
                spdlog::error("❌ Failed to initialize authentication");
                return false;
            }
            spdlog::info("🔐 Authentication enabled");
        } else {
            spdlog::warn("⚠️  Authentication DISABLED (--no-auth) — anyone who can "
                         "reach the ports can deploy code and read/write data");
        }

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
        wasm_engine_->SetTimeoutMs(
            static_cast<uint32_t>(config_.wasm.max_execution_time_ns / 1000000));
        wasm_engine_->SetMemoryLimitMB(
            static_cast<uint32_t>(config_.wasm.instance_memory_limit / (1024 * 1024)));
        if (!wasm_engine_->Initialize()) {
            spdlog::error("❌ Failed to initialize WASM engine");
            return false;
        }
        spdlog::info("✅ WASM Engine ready");

        // Restore persisted WASM modules
        module_store_ = std::make_shared<ModuleStore>(
            std::filesystem::path(config_.storage.data_dir) / "modules");
        if (module_store_->Initialize()) {
            for (const auto& m : module_store_->List()) {
                std::string err;
                auto bytes = module_store_->LoadBytecode(m.name, err);
                if (bytes && wasm_engine_->LoadModule(m.name, *bytes, m.version)) {
                    spdlog::info("♻️  Restored persisted module: {} (v{})", m.name,
                                 m.version.empty() ? "?" : m.version);
                } else {
                    spdlog::warn("⚠️  Skipping persisted module {}: {}", m.name,
                                 bytes ? wasm_engine_->GetLastLoadError() : err);
                }
            }
        } else {
            spdlog::warn("⚠️  Module store unavailable; deployed modules will not persist");
        }

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
        websocket_server_->SetAuthManager(auth_);
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
        grpc_config.tls_cert_path = config_.grpc.tls_cert;
        grpc_config.tls_key_path = config_.grpc.tls_key;

        grpc_server_ = std::make_shared<GrpcServer>(grpc_config, sql_engine_, storage_engine_,
                                                   changefeed_engine_, websocket_server_, wasm_engine_,
                                                   module_store_, auth_);
        if (!grpc_server_->Start()) {
            spdlog::error("❌ Failed to start gRPC server");
            return false;
        }
        spdlog::info("✅ gRPC Server ready on {}", config_.grpc.listen_address);
#else
        spdlog::info("ℹ️  gRPC Server not available (compiled without gRPC support)");
#endif

        // Native tick scheduler — drive sharded modules at a fixed rate in-process
        // (no external gRPC tick driver). Ticks fan out across a worker pool.
        if (config_.tick.count > 0 && !config_.tick.module_prefix.empty()) {
            unsigned workers = config_.tick.threads > 0
                ? config_.tick.threads
                : std::max(1u, std::thread::hardware_concurrency());
            uint32_t hz = config_.tick.hz > 0 ? config_.tick.hz : 60;
            auto period = std::chrono::nanoseconds(1000000000ULL / hz);
            int64_t frame_ms = static_cast<int64_t>(1000 / hz);
            std::string reducer = config_.tick.reducer.empty() ? "tick" : config_.tick.reducer;
            tick_scheduler_ = std::make_unique<TickScheduler>(wasm_engine_, workers);
            uint32_t added = 0;
            for (uint32_t idx = 0; idx < config_.tick.count; ++idx) {
                std::string mod = config_.tick.module_prefix + std::to_string(idx);
                if (!wasm_engine_->GetModule(mod)) {
                    spdlog::warn("⏱️  tick: module {} not loaded; skipping", mod);
                    continue;
                }
                TickScheduler::Entry e;
                e.module = mod;
                e.reducer = reducer;
                e.args = {WasmValue(frame_ms),
                          WasmValue(static_cast<int64_t>(config_.tick.substeps)),
                          WasmValue(static_cast<int64_t>(idx))};
                e.period = period;
                tick_scheduler_->AddEntry(std::move(e));
                ++added;
            }
            tick_scheduler_->Start();
            spdlog::info("⏱️  Native tick: {} modules '{}*' @ {} Hz, {} workers, substeps {}",
                         added, config_.tick.module_prefix, hz, workers, config_.tick.substeps);
        }

        spdlog::info("");
        spdlog::info("🎯 OriginDB Server fully initialized!");
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

        if (auth_ && auth_->GeneratedThisBoot()) {
            spdlog::warn("════════════════════════════════════════════════════════════");
            spdlog::warn("🔑 New auth tokens generated (shown once, stored in {}/auth):",
                         config_.storage.data_dir);
            spdlog::warn("   ADMIN  (deploy/SQL):  {}", auth_->AdminToken());
            spdlog::warn("   CLIENT (call/subscribe): {}", auth_->ClientToken());
            spdlog::warn("   CLI:  origindb_client --token <ADMIN> ...");
            spdlog::warn("   WS:   ws://host:port/?token=<CLIENT>");
            spdlog::warn("════════════════════════════════════════════════════════════");
        }

        return true;
    }

    void Run() {
        spdlog::info("💡 Server running - send SIGINT (Ctrl+C) to shutdown gracefully");

        // Wait for shutdown signal with immediate response
        while (!shutdown_requested_ && !g_signal_received.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Faster polling for responsiveness

            // Log periodic stats
            LogPeriodicStats();
        }

        if (g_signal_received.load()) {
            spdlog::info("🔔 Signal received, initiating shutdown...");
        }
    }

    void Shutdown() {
        if (shutdown_requested_) return;

        spdlog::info("🛑 Shutting down OriginDB Server...");
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

        if (tick_scheduler_) {
            tick_scheduler_->Stop();
            spdlog::info("✅ Tick scheduler stopped");
        }

        if (wasm_subscription_manager_) {
            wasm_subscription_manager_->Stop();
            spdlog::info("✅ WASM subscription manager stopped");
        }

        if (wasm_engine_) {
            wasm_engine_->RequestShutdown();
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
        auto logger = std::make_shared<spdlog::logger>("origindb", sinks.begin(), sinks.end());
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

        // Tick scheduler heartbeat (faster cadence for load tuning)
        if (tick_scheduler_) {
            static auto last_tick_log = std::chrono::steady_clock::now();
            auto tick_dt = std::chrono::duration_cast<std::chrono::seconds>(now - last_tick_log).count();
            if (tick_dt >= 5) {
                auto ts = tick_scheduler_->SnapshotAndReset();
                spdlog::info("⏱️  Tick: {} entries, {:.0f} ticks/s, {} skipped, {} errs, maxExec {} us",
                             ts.entries, static_cast<double>(ts.executed) / tick_dt,
                             ts.skipped, ts.errors, ts.max_exec_us);
                last_tick_log = now;
            }
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
    std::shared_ptr<ModuleStore> module_store_;
    std::shared_ptr<AuthManager> auth_;
    std::shared_ptr<WasmSubscriptionManager> wasm_subscription_manager_;
    std::unique_ptr<TickScheduler> tick_scheduler_;
#ifdef GRPC_AVAILABLE
    std::shared_ptr<GrpcServer> grpc_server_;
#endif
};

// Global server instance for signal handling
static OriginDBServer* g_server = nullptr;

// Timeout mechanism for forced shutdown
void forced_shutdown_thread() {
    std::this_thread::sleep_for(std::chrono::seconds(10)); // 10-second timeout

    if (!g_shutdown_requested.load()) {
        spdlog::error("⏰ FORCED SHUTDOWN: Server did not respond to signals within timeout");
        std::exit(1);
    }
}

// Enhanced signal handler with immediate response and forced termination
void signal_handler(int signal) {
    int signal_count = g_signal_count.fetch_add(1) + 1;

    spdlog::info("🔔 Signal {} received (count: {}), requesting immediate shutdown...", signal, signal_count);

    // Set signal received flag immediately
    g_signal_received.store(true);
    g_shutdown_requested.store(true);

    // Start forced shutdown timer on first signal
    if (signal_count == 1) {
        spdlog::info("⏱️  Starting 10-second shutdown timeout...");
        std::thread timeout_thread(forced_shutdown_thread);
        timeout_thread.detach();
    }

    // Force immediate exit on second signal
    if (signal_count >= 2) {
        spdlog::error("⚡ IMMEDIATE EXIT: Second signal received - forcing termination");
        std::exit(1);
    }

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
    config.storage.data_dir = "./origindb_data";
    config.websocket.listen_address = "0.0.0.0:8080";
    config.grpc.listen_address = "0.0.0.0:50051";
    config.logging.level = spdlog::level::info;
    config.logging.log_file = "./logs/origindb.log";

    // TODO: Parse actual config file (JSON/YAML)
    // For now, use environment variables as overrides
    const char* data_dir = getenv("ORIGINDB_DATA_DIR");
    if (data_dir) {
        config.storage.data_dir = data_dir;
    }

    const char* ws_port = getenv("ORIGINDB_WS_PORT");
    if (ws_port) {
        config.websocket.listen_address = "0.0.0.0:" + std::string(ws_port);
    }

    const char* grpc_port = getenv("ORIGINDB_GRPC_PORT");
    if (grpc_port) {
        config.grpc.listen_address = "0.0.0.0:" + std::string(grpc_port);
    }

    const char* log_level = getenv("ORIGINDB_LOG_LEVEL");
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
    std::cout << "  -d, --data-dir DIR       Data directory (default: ./origindb_data)\n";
    std::cout << "  --tls-cert FILE          TLS certificate (PEM) for the gRPC endpoint\n";
    std::cout << "  --tls-key FILE           TLS private key (PEM) for the gRPC endpoint\n";
    std::cout << "  --sync-mode MODE         WAL durability: none|flush|fsync (default: fsync)\n";
    std::cout << "  --checkpoint-mb N        Checkpoint (snapshot+truncate WAL) at N MB (default: 256)\n";
    std::cout << "  --checkpoint-interval S  Also checkpoint every S seconds if WAL grew (default: 300)\n";
    std::cout << "  --no-checkpoint          Disable runtime WAL checkpointing (WAL grows unbounded)\n";
    std::cout << "  --no-auth                Disable token auth (dev only; insecure)\n";
    std::cout << "  --tick-modules PREFIX    Native tick: drive modules <PREFIX>0..<PREFIX>N-1 (see --tick-count)\n";
    std::cout << "  --tick-count N           Number of sharded modules to tick\n";
    std::cout << "  --tick-hz HZ             Tick rate (default: 60)\n";
    std::cout << "  --tick-substeps N        Physics substeps per tick passed as arg (default: 1)\n";
    std::cout << "  --tick-threads N         Tick worker pool size (default: hardware concurrency)\n";
    std::cout << "  --tick-reducer NAME      Reducer to call per tick (default: tick)\n";
    std::cout << "  -l, --log-level LEVEL    Log level: trace,debug,info,warn,error (default: info)\n";
    std::cout << "  -c, --config FILE        Config file path (default: origindb.conf)\n";
    std::cout << "  -h, --help               Show this help message\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << "                    # Start with defaults\n";
    std::cout << "  " << program_name << " -p 9090            # WebSocket on port 9090\n";
    std::cout << "  " << program_name << " -g 50052           # gRPC on port 50052\n";
    std::cout << "  " << program_name << " 9090               # WebSocket port 9090 (short form)\n";
    std::cout << "\nEnvironment Variables:\n";
    std::cout << "  ORIGINDB_WS_PORT       WebSocket port\n";
    std::cout << "  ORIGINDB_GRPC_PORT     gRPC port\n";
    std::cout << "  ORIGINDB_DATA_DIR      Data directory\n";
    std::cout << "  ORIGINDB_LOG_LEVEL     Log level\n";
}

int main(int argc, char* argv[]) {
    try {
        // Parse command line arguments
        std::string config_file = "origindb.conf";
        std::string port_arg;
        std::string grpc_port_arg;
        std::string data_dir_arg;
        std::string log_level_arg;
        std::string tls_cert_arg;
        std::string tls_key_arg;
        std::string sync_mode_arg;
        bool no_auth = false;
        std::string checkpoint_mb_arg, checkpoint_interval_arg;
        bool no_checkpoint = false;
        std::string tick_modules_arg, tick_count_arg, tick_hz_arg,
                    tick_substeps_arg, tick_threads_arg, tick_reducer_arg;

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
            } else if (arg == "--tls-cert") {
                if (i + 1 < argc) tls_cert_arg = argv[++i];
                else { std::cerr << "Error: --tls-cert requires a path\n"; return 1; }
            } else if (arg == "--tls-key") {
                if (i + 1 < argc) tls_key_arg = argv[++i];
                else { std::cerr << "Error: --tls-key requires a path\n"; return 1; }
            } else if (arg == "--sync-mode") {
                if (i + 1 < argc) sync_mode_arg = argv[++i];
                else { std::cerr << "Error: --sync-mode requires none|flush|fsync\n"; return 1; }
            } else if (arg == "--no-auth") {
                no_auth = true;
            } else if (arg == "--checkpoint-mb") {
                if (i + 1 < argc) checkpoint_mb_arg = argv[++i];
                else { std::cerr << "Error: --checkpoint-mb requires a size in MB\n"; return 1; }
            } else if (arg == "--checkpoint-interval") {
                if (i + 1 < argc) checkpoint_interval_arg = argv[++i];
                else { std::cerr << "Error: --checkpoint-interval requires seconds\n"; return 1; }
            } else if (arg == "--no-checkpoint") {
                no_checkpoint = true;
            } else if (arg == "--tick-modules") {
                if (i + 1 < argc) tick_modules_arg = argv[++i];
                else { std::cerr << "Error: --tick-modules requires a module name prefix\n"; return 1; }
            } else if (arg == "--tick-count") {
                if (i + 1 < argc) tick_count_arg = argv[++i];
                else { std::cerr << "Error: --tick-count requires a number\n"; return 1; }
            } else if (arg == "--tick-hz") {
                if (i + 1 < argc) tick_hz_arg = argv[++i];
                else { std::cerr << "Error: --tick-hz requires a number\n"; return 1; }
            } else if (arg == "--tick-substeps") {
                if (i + 1 < argc) tick_substeps_arg = argv[++i];
                else { std::cerr << "Error: --tick-substeps requires a number\n"; return 1; }
            } else if (arg == "--tick-threads") {
                if (i + 1 < argc) tick_threads_arg = argv[++i];
                else { std::cerr << "Error: --tick-threads requires a number\n"; return 1; }
            } else if (arg == "--tick-reducer") {
                if (i + 1 < argc) tick_reducer_arg = argv[++i];
                else { std::cerr << "Error: --tick-reducer requires a name\n"; return 1; }
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
        if (!tls_cert_arg.empty()) config.grpc.tls_cert = tls_cert_arg;
        if (!tls_key_arg.empty()) config.grpc.tls_key = tls_key_arg;
        if (!sync_mode_arg.empty()) {
            if (sync_mode_arg == "none")  config.storage.sync_mode = SyncMode::None;
            else if (sync_mode_arg == "flush") config.storage.sync_mode = SyncMode::Flush;
            else if (sync_mode_arg == "fsync") config.storage.sync_mode = SyncMode::Fsync;
            else { std::cerr << "Error: --sync-mode must be none|flush|fsync\n"; return 1; }
        }
        if (const char* t = std::getenv("ORIGINDB_ADMIN_TOKEN"))
            config.auth.admin_token = t;
        if (const char* t = std::getenv("ORIGINDB_CLIENT_TOKEN"))
            config.auth.client_token = t;
        if (no_auth) config.auth.enabled = false;
        if (no_checkpoint) config.storage.checkpoint_enabled = false;
        if (!checkpoint_mb_arg.empty())
            config.storage.checkpoint_wal_bytes =
                static_cast<size_t>(std::stoull(checkpoint_mb_arg)) * 1024 * 1024;
        if (!checkpoint_interval_arg.empty())
            config.storage.checkpoint_interval_sec =
                static_cast<uint32_t>(std::stoul(checkpoint_interval_arg));
        if (!tick_modules_arg.empty()) config.tick.module_prefix = tick_modules_arg;
        if (!tick_count_arg.empty()) config.tick.count = static_cast<uint32_t>(std::stoul(tick_count_arg));
        if (!tick_hz_arg.empty()) config.tick.hz = static_cast<uint32_t>(std::stoul(tick_hz_arg));
        if (!tick_substeps_arg.empty()) config.tick.substeps = static_cast<uint32_t>(std::stoul(tick_substeps_arg));
        if (!tick_threads_arg.empty()) config.tick.threads = static_cast<uint32_t>(std::stoul(tick_threads_arg));
        if (!tick_reducer_arg.empty()) config.tick.reducer = tick_reducer_arg;
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
        OriginDBServer server(config);
        g_server = &server;

        // Setup signal handlers with verification
        spdlog::info("🛡️  Registering signal handlers...");
        if (signal(SIGINT, signal_handler) == SIG_ERR) {
            spdlog::error("❌ Failed to register SIGINT handler");
            return 1;
        }
        if (signal(SIGTERM, signal_handler) == SIG_ERR) {
            spdlog::error("❌ Failed to register SIGTERM handler");
            return 1;
        }
        spdlog::info("✅ Signal handlers registered successfully");

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