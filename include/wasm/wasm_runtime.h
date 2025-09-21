#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <optional>

#include "common/config.h"
#include "wasm/host_api.h"

namespace instantdb {

class Transaction;
class ChangefeedEngine;

// Module capabilities
struct ModuleCapabilities {
    std::vector<std::string> allowed_tables;
    bool read_only = false;
    bool can_emit_events = true;
    size_t max_memory_bytes = 256 * 1024 * 1024; // 256MB
    uint64_t max_cpu_nanoseconds = 5000000000; // 5 seconds
    std::vector<std::string> allowed_host_functions;
};

// Module execution context
struct ModuleContext {
    std::shared_ptr<Transaction> transaction;
    std::shared_ptr<ChangefeedEngine> changefeed;
    std::string module_id;
    std::string module_version;
    uint64_t deterministic_timestamp;
    std::vector<uint8_t> deterministic_seed;
    ModuleCapabilities capabilities;
};

// Module execution result
struct ModuleResult {
    bool success = false;
    std::string error;
    std::vector<uint8_t> output;
    uint64_t execution_time_ns = 0;
    uint64_t memory_used_bytes = 0;
    uint64_t instructions_executed = 0;
    std::vector<TableWrite> staged_writes;
    std::vector<EmittedEvent> emitted_events;
};

// WASM module instance
class WasmModule {
public:
    virtual ~WasmModule() = default;

    virtual const std::string& GetId() const = 0;
    virtual const std::string& GetVersion() const = 0;
    virtual const ModuleCapabilities& GetCapabilities() const = 0;

    virtual ModuleResult Execute(
        const std::string& function_name,
        const std::vector<uint8_t>& input,
        const ModuleContext& context) = 0;
};

// WASM runtime manager
class WasmRuntime {
public:
    explicit WasmRuntime(const WasmConfig& config);
    ~WasmRuntime();

    bool Initialize();
    void Shutdown();

    // Module loading
    std::shared_ptr<WasmModule> LoadModule(
        const std::string& module_id,
        const std::string& version,
        const std::vector<uint8_t>& wasm_bytes,
        const ModuleCapabilities& capabilities);

    // Module cache management
    std::shared_ptr<WasmModule> GetCachedModule(
        const std::string& module_id,
        const std::string& version);

    void EvictModule(
        const std::string& module_id,
        const std::string& version);

    void ClearCache();

    // Instance pool management
    size_t GetActiveInstances() const;
    size_t GetPooledInstances() const;

    // Resource monitoring
    struct ResourceUsage {
        uint64_t total_memory_bytes;
        uint64_t active_instances;
        uint64_t pooled_instances;
        uint64_t total_executions;
        uint64_t total_execution_time_ns;
    };
    ResourceUsage GetResourceUsage() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb