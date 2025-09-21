#include <iostream>
#include <memory>
#include <signal.h>
#include <thread>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "common/config.h"
#include "storage/storage_engine.h"
#include "raft/raft_node.h"
#include "sql/sql_engine.h"
#include "wasm/wasm_runtime.h"
#include "modules/module_store.h"
#include "net/grpc_server.h"
#include "net/websocket_server.h"
#include "changefeed/changefeed_engine.h"
#include "admin/admin_service.h"
#include "common/metrics.h"

namespace instantdb {

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
        spdlog::info("Initializing InstantDB server...");

        // Initialize storage engine
        storage_engine_ = std::make_shared<StorageEngine>(config_.storage);
        if (!storage_engine_->Initialize()) {
            spdlog::error("Failed to initialize storage engine");
            return false;
        }

        // Initialize WASM runtime
        wasm_runtime_ = std::make_shared<WasmRuntime>(config_.wasm);
        if (!wasm_runtime_->Initialize()) {
            spdlog::error("Failed to initialize WASM runtime");
            return false;
        }

        // Initialize module store
        module_store_ = std::make_shared<ModuleStore>(
            storage_engine_, wasm_runtime_, config_.modules);
        if (!module_store_->Initialize()) {
            spdlog::error("Failed to initialize module store");
            return false;
        }

        // Initialize Raft consensus
        raft_node_ = std::make_shared<RaftNode>(
            config_.raft, storage_engine_, module_store_);
        if (!raft_node_->Initialize()) {
            spdlog::error("Failed to initialize Raft node");
            return false;
        }

        // Initialize SQL engine
        sql_engine_ = std::make_shared<SqlEngine>(
            storage_engine_, module_store_, raft_node_);
        if (!sql_engine_->Initialize()) {
            spdlog::error("Failed to initialize SQL engine");
            return false;
        }

        // Initialize changefeed engine
        changefeed_engine_ = std::make_shared<ChangefeedEngine>(
            storage_engine_, config_.changefeed);
        if (!changefeed_engine_->Initialize()) {
            spdlog::error("Failed to initialize changefeed engine");
            return false;
        }

        // Initialize admin service
        admin_service_ = std::make_shared<AdminService>(
            module_store_, raft_node_, storage_engine_);

        // Initialize gRPC server
        grpc_server_ = std::make_shared<GrpcServer>(
            config_.grpc, sql_engine_, admin_service_);
        if (!grpc_server_->Initialize()) {
            spdlog::error("Failed to initialize gRPC server");
            return false;
        }

        // Initialize WebSocket server for changefeeds
        websocket_server_ = std::make_shared<WebSocketServer>(
            config_.websocket, changefeed_engine_);
        if (!websocket_server_->Initialize()) {
            spdlog::error("Failed to initialize WebSocket server");
            return false;
        }

        // Initialize metrics
        metrics_ = std::make_shared<MetricsExporter>(config_.metrics);
        if (!metrics_->Initialize()) {
            spdlog::error("Failed to initialize metrics exporter");
            return false;
        }

        RegisterMetrics();

        spdlog::info("InstantDB server initialized successfully");
        return true;
    }

    bool Start() {
        spdlog::info("Starting InstantDB server components...");

        // Start Raft node
        if (!raft_node_->Start()) {
            spdlog::error("Failed to start Raft node");
            return false;
        }

        // Start changefeed engine
        if (!changefeed_engine_->Start()) {
            spdlog::error("Failed to start changefeed engine");
            return false;
        }

        // Start gRPC server
        if (!grpc_server_->Start()) {
            spdlog::error("Failed to start gRPC server");
            return false;
        }

        // Start WebSocket server
        if (!websocket_server_->Start()) {
            spdlog::error("Failed to start WebSocket server");
            return false;
        }

        // Start metrics exporter
        if (!metrics_->Start()) {
            spdlog::error("Failed to start metrics exporter");
            return false;
        }

        spdlog::info("InstantDB server started successfully");
        spdlog::info("gRPC listening on {}", config_.grpc.listen_address);
        spdlog::info("WebSocket listening on {}", config_.websocket.listen_address);
        spdlog::info("Metrics available on {}", config_.metrics.listen_address);

        return true;
    }

    void WaitForShutdown() {
        std::unique_lock<std::mutex> lock(shutdown_mutex_);
        shutdown_cv_.wait(lock, [this] { return shutdown_requested_; });
    }

    void RequestShutdown() {
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        shutdown_requested_ = true;
        shutdown_cv_.notify_all();
    }

private:
    void InitializeLogging() {
        std::vector<spdlog::sink_ptr> sinks;

        // Console sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);
        sinks.push_back(console_sink);

        // File sink
        if (!config_.logging.log_file.empty()) {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config_.logging.log_file,
                config_.logging.max_file_size,
                config_.logging.max_files);
            file_sink->set_level(spdlog::level::trace);
            sinks.push_back(file_sink);
        }

        // Create logger
        auto logger = std::make_shared<spdlog::logger>("instantdb",
            sinks.begin(), sinks.end());
        logger->set_level(config_.logging.level);
        logger->flush_on(spdlog::level::err);

        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    }

    void RegisterMetrics() {
        // Register server metrics
        metrics_->RegisterGauge("instantdb_up", "Server status");
        metrics_->RegisterCounter("instantdb_requests_total", "Total requests");
        metrics_->RegisterHistogram("instantdb_request_duration_seconds",
            "Request duration");

        // Register storage metrics
        metrics_->RegisterGauge("instantdb_storage_tables", "Number of tables");
        metrics_->RegisterGauge("instantdb_storage_rows", "Total rows");
        metrics_->RegisterGauge("instantdb_storage_bytes", "Storage size in bytes");

        // Register Raft metrics
        metrics_->RegisterGauge("instantdb_raft_term", "Current Raft term");
        metrics_->RegisterGauge("instantdb_raft_commit_index", "Raft commit index");
        metrics_->RegisterGauge("instantdb_raft_leader", "Is leader (1) or follower (0)");

        // Register WASM metrics
        metrics_->RegisterCounter("instantdb_wasm_invocations_total",
            "Total WASM invocations");
        metrics_->RegisterHistogram("instantdb_wasm_execution_duration_seconds",
            "WASM execution duration");
        metrics_->RegisterGauge("instantdb_wasm_instances", "Active WASM instances");

        // Register changefeed metrics
        metrics_->RegisterGauge("instantdb_changefeed_subscriptions",
            "Active subscriptions");
        metrics_->RegisterCounter("instantdb_changefeed_events_total",
            "Total events delivered");
        metrics_->RegisterGauge("instantdb_changefeed_lag_seconds",
            "Changefeed delivery lag");
    }

    void Shutdown() {
        if (shutdown_called_) return;
        shutdown_called_ = true;

        spdlog::info("Shutting down InstantDB server...");

        // Stop components in reverse order
        if (metrics_) metrics_->Stop();
        if (websocket_server_) websocket_server_->Stop();
        if (grpc_server_) grpc_server_->Stop();
        if (changefeed_engine_) changefeed_engine_->Stop();
        if (raft_node_) raft_node_->Stop();
        if (sql_engine_) sql_engine_->Shutdown();
        if (module_store_) module_store_->Shutdown();
        if (wasm_runtime_) wasm_runtime_->Shutdown();
        if (storage_engine_) storage_engine_->Shutdown();

        spdlog::info("InstantDB server shutdown complete");
    }

private:
    ServerConfig config_;

    std::shared_ptr<StorageEngine> storage_engine_;
    std::shared_ptr<WasmRuntime> wasm_runtime_;
    std::shared_ptr<ModuleStore> module_store_;
    std::shared_ptr<RaftNode> raft_node_;
    std::shared_ptr<SqlEngine> sql_engine_;
    std::shared_ptr<ChangefeedEngine> changefeed_engine_;
    std::shared_ptr<AdminService> admin_service_;
    std::shared_ptr<GrpcServer> grpc_server_;
    std::shared_ptr<WebSocketServer> websocket_server_;
    std::shared_ptr<MetricsExporter> metrics_;

    std::mutex shutdown_mutex_;
    std::condition_variable shutdown_cv_;
    bool shutdown_requested_;
    bool shutdown_called_ = false;
};

} // namespace instantdb

// Global server instance for signal handling
std::unique_ptr<instantdb::InstantDBServer> g_server;

void SignalHandler(int signal) {
    spdlog::info("Received signal {}, requesting shutdown...", signal);
    if (g_server) {
        g_server->RequestShutdown();
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    instantdb::ServerConfig config;
    if (!instantdb::ParseCommandLine(argc, argv, config)) {
        std::cerr << "Usage: " << argv[0] << " [options]" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << "  --config <file>     Configuration file path" << std::endl;
        std::cerr << "  --node-id <id>      Node ID for clustering" << std::endl;
        std::cerr << "  --data-dir <dir>    Data directory path" << std::endl;
        std::cerr << "  --grpc-addr <addr>  gRPC listen address" << std::endl;
        std::cerr << "  --ws-addr <addr>    WebSocket listen address" << std::endl;
        std::cerr << "  --join <addr>       Join existing cluster at address" << std::endl;
        return 1;
    }

    // Set up signal handlers
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    try {
        // Create and initialize server
        g_server = std::make_unique<instantdb::InstantDBServer>(config);

        if (!g_server->Initialize()) {
            spdlog::error("Server initialization failed");
            return 1;
        }

        if (!g_server->Start()) {
            spdlog::error("Server startup failed");
            return 1;
        }

        // Wait for shutdown signal
        g_server->WaitForShutdown();

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    }

    return 0;
}