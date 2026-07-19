#pragma once

#include <string>
#include <cstdint>
#include <spdlog/common.h>

namespace origindb {

// WAL durability level. Higher = safer, slower per lone commit (group commit
// amortizes the cost under concurrency).
enum class SyncMode {
    None,   // append only; OS decides when to flush (tests / throwaway data)
    Flush,  // flush userspace buffer to OS per group (survives process crash,
            // NOT power loss)
    Fsync   // fdatasync/F_FULLFSYNC per group before acking (survives power loss)
};

struct StorageConfig {
    std::string data_dir = "./data";
    size_t max_memory_bytes = 4ULL * 1024 * 1024 * 1024; // 4GB
    size_t wal_buffer_size = 64 * 1024 * 1024; // 64MB
    size_t snapshot_interval = 10000; // entries
    bool sync_wal = true;               // legacy: true->Fsync, false->Flush
    SyncMode sync_mode = SyncMode::Fsync;   // durability level (group commit)
    uint32_t group_commit_window_us = 0;    // 0 = opportunistic batching only
    size_t page_size = 4096;

    // Runtime WAL checkpointing. A background thread periodically snapshots all
    // tables and truncates the WAL, so the log can't grow without bound while
    // the server runs (previously the WAL was only truncated on clean shutdown,
    // letting a long-lived high-write workload fill the disk). A snapshot + a
    // short WAL tail replaces replaying every write ever made.
    bool checkpoint_enabled = true;
    // Size trigger: checkpoint once the WAL reaches this many bytes.
    size_t checkpoint_wal_bytes = 256ULL * 1024 * 1024; // 256MB
    // Time trigger: also checkpoint at least this often if the WAL has grown.
    // 0 disables the time trigger (size trigger still applies).
    uint32_t checkpoint_interval_sec = 300; // 5 min
    // How often the checkpoint thread wakes to evaluate the triggers.
    uint32_t checkpoint_poll_sec = 5;
};

struct RaftConfig {
    std::string node_id;
    std::string cluster_id = "origindb-cluster";
    std::string listen_address = "0.0.0.0:9001";
    std::vector<std::string> peer_addresses;
    uint32_t election_timeout_ms = 3000;
    uint32_t heartbeat_interval_ms = 1000;
    uint32_t snapshot_interval = 10000;
    size_t max_log_entries = 1000000;
    std::string log_dir;
};

struct WasmConfig {
    size_t max_instances = 100;
    size_t instance_memory_limit = 256 * 1024 * 1024; // 256MB per instance
    uint64_t max_execution_time_ns = 5000000000; // 5 seconds
    size_t instance_pool_size = 10;
    bool enable_cache = true;
    size_t cache_size = 100;
};

struct ModuleConfig {
    std::string module_dir;
    bool verify_signatures = false;
    size_t max_module_size = 50 * 1024 * 1024; // 50MB
    bool auto_install = false;
};

struct GrpcConfig {
    std::string listen_address = "0.0.0.0:50051";
    size_t max_message_size = 100 * 1024 * 1024; // 100MB
    uint32_t num_threads = 4;
    bool use_tls = false;
    std::string tls_cert;
    std::string tls_key;
};

struct WebSocketConfig {
    std::string listen_address = "0.0.0.0:8080";
    size_t max_connections = 10000;
    size_t max_message_size = 1024 * 1024; // 1MB
    uint32_t ping_interval_ms = 30000;
    uint32_t timeout_ms = 60000;
};

struct ChangefeedConfig {
    size_t buffer_size = 10000;
    size_t retention_ms = 86400000; // 24 hours
    size_t max_lag_entries = 100000;
    bool persist_offsets = true;
    size_t delivery_batch_size = 100;
};

struct MetricsConfig {
    std::string listen_address = "0.0.0.0:9090";
    bool enabled = true;
    uint32_t collection_interval_ms = 10000;
};

struct LoggingConfig {
    spdlog::level::level_enum level = spdlog::level::info;
    std::string log_file;
    size_t max_file_size = 100 * 1024 * 1024; // 100MB
    size_t max_files = 10;
};

struct AuthConfig {
    bool enabled = true;             // token auth on gRPC + websocket
    std::string admin_token;         // explicit override; else load-or-generate
    std::string client_token;        // explicit override; else load-or-generate
};

// Native in-server tick driver for sharded modules (e.g. game arenas). When
// `count > 0`, the server drives modules "<module_prefix>0".."<prefix>N-1" by
// calling `reducer` at `hz` from a worker pool, passing args [frameMs, substeps,
// shardIndex]. Replaces an external process polling ticks over gRPC.
struct TickConfig {
    std::string module_prefix;      // empty = disabled
    uint32_t count = 0;             // number of sharded modules
    std::string reducer = "tick";
    uint32_t hz = 60;
    uint32_t substeps = 1;
    uint32_t threads = 0;           // 0 = hardware_concurrency
};

struct ServerConfig {
    std::string node_id;
    StorageConfig storage;
    RaftConfig raft;
    WasmConfig wasm;
    ModuleConfig modules;
    GrpcConfig grpc;
    WebSocketConfig websocket;
    ChangefeedConfig changefeed;
    MetricsConfig metrics;
    LoggingConfig logging;
    AuthConfig auth;
    TickConfig tick;
};

bool ParseCommandLine(int argc, char* argv[], ServerConfig& config);
bool LoadConfigFile(const std::string& path, ServerConfig& config);

} // namespace origindb