#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <variant>
#include "storage/table.h"

namespace instantdb {

class StorageEngine;
class ChangefeedEngine;

// Forward declarations
class WasmModule;
class WasmInstance;
class WasmRuntime;

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

// Function signature for WASM functions
using WasmFunction = std::function<std::vector<WasmValue>(const std::vector<WasmValue>&)>;

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

// Result of WASM operation
struct WasmResult {
    bool success;
    std::string error;
    std::vector<WasmValue> values;
};

// WASM Module representation
class WasmModule {
public:
    WasmModule(const std::string& name, const std::vector<uint8_t>& bytecode);
    ~WasmModule();

    bool Load();
    bool Validate();
    std::unique_ptr<WasmInstance> CreateInstance();

    const std::string& GetName() const { return name_; }
    const ModuleMetadata& GetMetadata() const { return metadata_; }
    const std::vector<uint8_t>& GetBytecode() const { return bytecode_; }

private:
    friend class WasmInstance;

    std::string name_;
    std::vector<uint8_t> bytecode_;
    ModuleMetadata metadata_;
    void* module_handle_; // Implementation-specific handle
    bool loaded_;
    bool validated_;

    bool ExtractMetadata();
};

// WASM Instance (runtime execution context)
class WasmInstance {
public:
    WasmInstance(WasmModule* module);
    ~WasmInstance();

    bool Initialize();
    void Shutdown();

    // Execute a reducer function
    WasmResult CallReducer(const std::string& reducer_name,
                          const ReducerContext& ctx,
                          const std::vector<WasmValue>& args);

    // Execute arbitrary exported function
    WasmResult CallFunction(const std::string& function_name,
                           const std::vector<WasmValue>& args);

    // Memory management
    bool AllocateMemory(size_t size, uint32_t* address);
    bool WriteMemory(uint32_t address, const void* data, size_t size);
    bool ReadMemory(uint32_t address, void* data, size_t size);

private:
    WasmModule* module_;
    void* instance_handle_; // Implementation-specific handle
    bool initialized_;

    // Host function implementations
    void RegisterHostFunctions();

    // Host functions callable from WASM
    static WasmValue HostLog(const std::vector<WasmValue>& args);
    static WasmValue HostTableInsert(const std::vector<WasmValue>& args);
    static WasmValue HostTableSelect(const std::vector<WasmValue>& args);
    static WasmValue HostTableUpdate(const std::vector<WasmValue>& args);
    static WasmValue HostTableDelete(const std::vector<WasmValue>& args);
    static WasmValue HostEmitEvent(const std::vector<WasmValue>& args);
};

// WASM Runtime Engine
class WasmEngine {
public:
    WasmEngine(std::shared_ptr<StorageEngine> storage,
               std::shared_ptr<ChangefeedEngine> changefeed);
    ~WasmEngine();

    bool Initialize();
    void Shutdown();

    // Module management
    bool LoadModule(const std::string& name, const std::vector<uint8_t>& bytecode);
    bool UnloadModule(const std::string& name);
    std::shared_ptr<WasmModule> GetModule(const std::string& name);
    std::vector<std::string> ListModules() const;
    std::vector<std::string> GetLoadedModules() const;

    // Reducer execution
    WasmResult ExecuteReducer(const std::string& module_name,
                             const std::string& reducer_name,
                             const ReducerContext& ctx,
                             const std::vector<WasmValue>& args);

    // Lifecycle events
    WasmResult OnModuleInit(const std::string& module_name, const ReducerContext& ctx);
    WasmResult OnClientConnected(const std::string& module_name, const ReducerContext& ctx);
    WasmResult OnClientDisconnected(const std::string& module_name, const ReducerContext& ctx);

    // Configuration
    void SetMaxInstances(size_t max_instances) { max_instances_ = max_instances; }
    void SetTimeoutMs(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }
    void SetMemoryLimitMB(uint32_t memory_limit_mb) { memory_limit_mb_ = memory_limit_mb; }

    // Accessor methods
    std::shared_ptr<StorageEngine> GetStorageEngine() const { return storage_; }
    std::shared_ptr<ChangefeedEngine> GetChangefeedEngine() const { return changefeed_; }

private:
    std::shared_ptr<StorageEngine> storage_;
    std::shared_ptr<ChangefeedEngine> changefeed_;

    // Module storage
    std::unordered_map<std::string, std::shared_ptr<WasmModule>> modules_;

    // Instance pool for performance
    std::unordered_map<std::string, std::vector<std::unique_ptr<WasmInstance>>> instance_pools_;

    // Configuration
    size_t max_instances_;
    uint32_t timeout_ms_;
    uint32_t memory_limit_mb_;

    bool initialized_;
    mutable std::mutex modules_mutex_;

    // Instance management
    std::unique_ptr<WasmInstance> GetOrCreateInstance(const std::string& module_name);
    void ReturnInstance(const std::string& module_name, std::unique_ptr<WasmInstance> instance);

    // Utility functions
    bool ValidateModule(const WasmModule& module);
    ReducerContext CreateReducerContext(const std::string& sender_identity,
                                      const std::string& connection_id = "");
};

// Configuration for WASM engine
struct WasmEngineConfig {
    size_t max_instances = 10;
    uint32_t timeout_ms = 5000;
    uint32_t memory_limit_mb = 64;
    bool enable_debug = false;
    std::string runtime_type = "wasmtime"; // wasmtime, wasmer, etc.
};

} // namespace instantdb