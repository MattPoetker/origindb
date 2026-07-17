#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <variant>
#include <atomic>
#include "storage/table.h"

namespace origindb {

class StorageEngine;
class ChangefeedEngine;

class WasmModule;
struct WasmModuleImpl;

// Value types that can be passed between host and WASM
using WasmValue = std::variant<
    std::monostate,  // null
    bool,
    int32_t,
    int64_t,
    float,
    double,
    std::string,
    std::vector<uint8_t>  // binary data
>;

// Reducer context passed to WASM modules
struct ReducerContext {
    std::string sender_identity;
    std::string module_identity;
    std::string connection_id;
    uint64_t timestamp_micros;
    std::shared_ptr<StorageEngine> storage;
    std::shared_ptr<ChangefeedEngine> changefeed;
};

// Reducer definition
struct ReducerDef {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    bool is_init = false;
    bool is_client_connected = false;
    bool is_client_disconnected = false;
};

// Module metadata
struct ModuleMetadata {
    std::string name;
    std::string version;
    std::vector<TableSchema> tables;
    std::vector<ReducerDef> reducers;
    std::vector<std::string> exports;
    std::vector<std::string> imports;
};

// Per-module sandbox restrictions. Zero/empty fields fall back to engine defaults.
struct ModuleCapabilities {
    std::vector<std::string> allowed_tables;  // empty = all tables
    bool read_only = false;
    uint32_t max_memory_mb = 0;
    uint32_t timeout_ms = 0;
};

// Result of WASM operation
struct WasmResult {
    bool success;
    std::string error;
    std::vector<WasmValue> values;
};

// A compiled WASM module plus its long-lived instance state.
class WasmModule {
public:
    WasmModule(const std::string& name, const std::vector<uint8_t>& bytecode);
    ~WasmModule();

    const std::string& GetName() const { return name_; }
    const ModuleMetadata& GetMetadata() const { return metadata_; }
    const std::vector<uint8_t>& GetBytecode() const { return bytecode_; }
    const ModuleCapabilities& GetCapabilities() const { return capabilities_; }
    const std::string& GetLoadError() const { return load_error_; }
    bool IsLoaded() const { return loaded_; }

private:
    friend class WasmEngine;

    std::string name_;
    std::vector<uint8_t> bytecode_;
    ModuleMetadata metadata_;
    ModuleCapabilities capabilities_;
    std::string load_error_;
    bool loaded_ = false;
    std::unique_ptr<WasmModuleImpl> impl_;
};

// WASM Runtime Engine (wasmtime-backed)
class WasmEngine {
public:
    WasmEngine(std::shared_ptr<StorageEngine> storage,
               std::shared_ptr<ChangefeedEngine> changefeed);
    ~WasmEngine();

    bool Initialize();
    void Shutdown();
    void RequestShutdown();

    // Module management
    bool LoadModule(const std::string& name, const std::vector<uint8_t>& bytecode,
                    const std::string& version = "",
                    const ModuleCapabilities& capabilities = {});
    bool UnloadModule(const std::string& name);
    std::shared_ptr<WasmModule> GetModule(const std::string& name);
    std::vector<std::string> ListModules() const;
    std::vector<std::string> GetLoadedModules() const;
    // Error text from the most recent failed LoadModule (best-effort).
    std::string GetLastLoadError() const;

    // Reducer execution
    WasmResult ExecuteReducer(const std::string& module_name,
                             const std::string& reducer_name,
                             const ReducerContext& ctx,
                             const std::vector<WasmValue>& args);

    // Lifecycle events (reserved invoke names __init/__client_connected/__client_disconnected)
    WasmResult OnModuleInit(const std::string& module_name, const ReducerContext& ctx);
    WasmResult OnClientConnected(const std::string& module_name, const ReducerContext& ctx);
    WasmResult OnClientDisconnected(const std::string& module_name, const ReducerContext& ctx);

    // Configuration (engine-wide defaults; per-module capabilities override)
    void SetTimeoutMs(uint32_t timeout_ms);
    void SetMemoryLimitMB(uint32_t memory_limit_mb);

    // Accessor methods
    std::shared_ptr<StorageEngine> GetStorageEngine() const;
    std::shared_ptr<ChangefeedEngine> GetChangefeedEngine() const;

    ReducerContext CreateReducerContext(const std::string& sender_identity,
                                        const std::string& connection_id = "");

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Configuration for WASM engine
struct WasmEngineConfig {
    uint32_t timeout_ms = 5000;
    uint32_t memory_limit_mb = 256;
    bool enable_debug = false;
};

} // namespace origindb
