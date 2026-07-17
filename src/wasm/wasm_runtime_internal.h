#pragma once

// Private wasmtime-backed runtime state shared between wasm_engine.cpp and
// host_functions.cpp. Never installed; wasmtime types must not leak into
// public headers.

#include <wasmtime.h>
#include <wasi.h>

#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "wasm/wasm_engine.h"

namespace origindb {

// ABI error codes (negative i32 returned by host functions / reducer status)
constexpr int32_t WASM_ERR_INTERNAL = -1;
constexpr int32_t WASM_ERR_PERMISSION = -2;
constexpr int32_t WASM_ERR_INVALID_ARG = -3;
constexpr int32_t WASM_ERR_LIMIT = -5;
// Returned by the SDK's origindb_invoke for an unregistered name; the engine
// treats it as a no-op for reserved "__" lifecycle names and an error otherwise.
constexpr int32_t WASM_ERR_NO_HANDLER = -404;

constexpr size_t WASM_MAX_TABLE_NAME = 256;
constexpr size_t WASM_MAX_LOG_MSG = 4096;
constexpr size_t WASM_MAX_RESULT = 16 * 1024 * 1024;
constexpr int32_t WASM_DEFAULT_SCAN_LIMIT = 1000;

// How often the engine epoch advances; timeout_ms is converted to ticks.
constexpr uint32_t WASM_EPOCH_TICK_MS = 10;

struct WasmModuleImpl;

// Per-ExecuteReducer call state: staged writes/events + result buffer.
// Lives on the caller's stack; reachable from host callbacks through
// WasmModuleImpl::current_call (calls are serialized per module).
struct HostCallContext {
    const ReducerContext* rctx = nullptr;
    WasmModuleImpl* mod = nullptr;

    // table -> key -> value (nullopt = staged delete)
    std::map<std::string, std::map<std::string, std::optional<nlohmann::json>>> overlay;

    struct StagedEvent {
        std::string topic;
        std::string key;
        std::vector<uint8_t> payload;
    };
    std::vector<StagedEvent> events;

    std::vector<uint8_t> result;
    bool has_result = false;
    std::string abort_msg;
};

// Long-lived per-module runtime state. The compiled module survives
// re-instantiation; the store/instance are recreated after a trap/timeout.
struct WasmModuleImpl {
    // Borrowed from WasmEngine::Impl; outlives all modules.
    wasm_engine_t* engine = nullptr;

    wasmtime_module_t* module = nullptr;

    // Instance state (valid when instantiated == true)
    wasmtime_store_t* store = nullptr;
    wasmtime_instance_t instance{};
    wasmtime_memory_t memory{};
    wasmtime_func_t fn_alloc{};
    wasmtime_func_t fn_free{};
    wasmtime_func_t fn_invoke{};
    wasmtime_func_t fn_describe{};
    wasmtime_func_t fn_initialize{};
    bool has_free = false;
    bool has_describe = false;
    bool has_initialize = false;
    bool instantiated = false;

    // Serializes all execution against this module.
    std::mutex call_mutex;

    // Effective limits (capabilities override engine defaults; resolved at load)
    uint64_t memory_limit_bytes = 0;
    uint32_t timeout_ms = 0;
    bool debug_stdio = false;

    const ModuleCapabilities* caps = nullptr;  // points into owning WasmModule
    std::string module_name;

    // Valid only while call_mutex is held during a call.
    HostCallContext* current_call = nullptr;

    ~WasmModuleImpl();
    void DestroyInstance();
};

// host_functions.cpp
// Registers all "env" imports on the linker with env = impl.
bool RegisterHostFunctions(wasmtime_linker_t* linker, WasmModuleImpl* impl,
                           std::string& error);
// Applies staged writes/events. Returns false and sets error on failure.
bool CommitHostCall(HostCallContext& call, std::string& error);

// Shared JSON <-> storage conversions (also used for metadata/results).
nlohmann::json RowToJson(const Row& row);
Row JsonToRow(const std::string& key, const nlohmann::json& columns);
std::string Base64Encode(const uint8_t* data, size_t len);
std::optional<std::vector<uint8_t>> Base64Decode(const std::string& in);

// Extracts an owned message from a wasmtime error/trap (deletes the input).
std::string ConsumeError(wasmtime_error_t* err);
std::string ConsumeTrap(wasm_trap_t* trap);

} // namespace origindb
