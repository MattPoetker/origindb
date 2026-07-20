// Wasmtime-backed WASM engine. Modules are compiled once at deploy, run as a
// long-lived instance serialized by a per-module mutex, and are re-instantiated
// after a trap/timeout. See docs/WASM_ABI.md for the guest contract.

#include "wasm/wasm_engine.h"
#include "wasm_runtime_internal.h"

#include "storage/storage_engine.h"
#include "changefeed/changefeed_engine.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace origindb {

namespace {

constexpr const char* kRequiredExports[] = {"memory", "origindb_invoke",
                                            "origindb_alloc"};

const std::unordered_map<std::string, int> kKnownEnvImports = {
    {"host_table_read", 0},  {"host_table_write", 0}, {"host_table_delete", 0},
    {"host_table_scan", 0},  {"host_emit_event", 0},  {"host_now_ms", 0},
    {"host_generate_id", 0}, {"host_log", 0},         {"host_abort", 0},
    {"host_alloc", 0},       {"host_free", 0},        {"host_set_result", 0},
    {"host_sender", 0},
};

bool IsReservedName(const std::string& name) {
    return name.rfind("__", 0) == 0;
}

std::string NameToString(const wasm_name_t* n) {
    return std::string(n->data, n->size);
}

}  // namespace

// ---------------------------------------------------------------------------
// WasmModuleImpl
// ---------------------------------------------------------------------------

void WasmModuleImpl::DestroyInstance() {
    if (store) {
        wasmtime_store_delete(store);
        store = nullptr;
    }
    instantiated = false;
    has_free = has_describe = has_initialize = false;
}

WasmModuleImpl::~WasmModuleImpl() {
    DestroyInstance();
    if (module) {
        wasmtime_module_delete(module);
        module = nullptr;
    }
}

// ---------------------------------------------------------------------------
// WasmModule
// ---------------------------------------------------------------------------

WasmModule::WasmModule(const std::string& name, const std::vector<uint8_t>& bytecode)
    : name_(name), bytecode_(bytecode) {
    metadata_.name = name;
}

WasmModule::~WasmModule() = default;

// ---------------------------------------------------------------------------
// WasmEngine::Impl
// ---------------------------------------------------------------------------

class WasmEngine::Impl {
public:
    Impl(std::shared_ptr<StorageEngine> storage,
         std::shared_ptr<ChangefeedEngine> changefeed)
        : storage_(std::move(storage)), changefeed_(std::move(changefeed)) {}

    ~Impl() { Shutdown(); }

    bool Initialize() {
        if (engine_) return true;

        wasm_config_t* config = wasm_config_new();
        wasmtime_config_epoch_interruption_set(config, true);
        engine_ = wasm_engine_new_with_config(config);
        if (!engine_) {
            spdlog::error("Failed to create wasmtime engine");
            return false;
        }

        ticker_running_ = true;
        epoch_thread_ = std::thread([this] {
            while (ticker_running_.load()) {
                wasmtime_engine_increment_epoch(engine_);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(WASM_EPOCH_TICK_MS));
            }
        });

        spdlog::info("WASM engine initialized (wasmtime, epoch tick {}ms)",
                     WASM_EPOCH_TICK_MS);
        return true;
    }

    void Shutdown() {
        if (!engine_) return;
        RequestShutdown();

        {
            std::lock_guard<std::mutex> lock(modules_mutex_);
            modules_.clear();  // destroys stores + modules
        }

        ticker_running_ = false;
        if (epoch_thread_.joinable()) epoch_thread_.join();
        wasm_engine_delete(engine_);
        engine_ = nullptr;
        spdlog::info("WASM engine shut down");
    }

    void RequestShutdown() {
        if (shutdown_requested_.exchange(true)) return;
        // Trip every in-flight epoch deadline (deadlines are timeout_ms/tick
        // ticks ahead; 100k increments covers timeouts up to ~1000s).
        if (engine_) {
            for (int i = 0; i < 100000; i++) wasmtime_engine_increment_epoch(engine_);
        }
    }

    bool LoadModule(const std::string& name, const std::vector<uint8_t>& bytecode,
                    const std::string& version, const ModuleCapabilities& caps) {
        if (!engine_) {
            SetLoadError(name, "engine not initialized");
            return false;
        }
        if (shutdown_requested_.load()) {
            SetLoadError(name, "engine shutting down");
            return false;
        }
        if (bytecode.empty()) {
            SetLoadError(name, "empty module bytecode");
            return false;
        }

        auto module = std::make_shared<WasmModule>(name, bytecode);
        module->capabilities_ = caps;
        module->metadata_.version = version;
        module->impl_ = std::make_unique<WasmModuleImpl>();
        auto* impl = module->impl_.get();
        impl->engine = engine_;
        impl->caps = &module->capabilities_;
        impl->module_name = name;
        impl->memory_limit_bytes =
            static_cast<uint64_t>(caps.max_memory_mb ? caps.max_memory_mb
                                                     : memory_limit_mb_.load()) *
            1024 * 1024;
        impl->timeout_ms = caps.timeout_ms ? caps.timeout_ms : timeout_ms_.load();
        impl->debug_stdio = false;

        std::string error;
        if (!Compile(*module, error) || !Instantiate(*impl, error)) {
            SetLoadError(name, error);
            spdlog::error("Failed to load WASM module {}: {}", name, error);
            return false;
        }
        module->loaded_ = true;

        ExtractMetadata(*module);

        // Hot-swap: the currently deployed module (if any) keeps serving until
        // the new one has passed the version gate, __init, and __migrate.
        std::shared_ptr<WasmModule> previous;
        {
            std::lock_guard<std::mutex> lock(modules_mutex_);
            auto it = modules_.find(name);
            if (it != modules_.end()) previous = it->second;
        }

        if (previous) {
            const std::string& old_v = previous->metadata_.version;
            const std::string& new_v = module->metadata_.version;
            if (!old_v.empty() && !new_v.empty() &&
                CompareVersions(new_v, old_v) < 0) {
                SetLoadError(name, "version " + new_v +
                                       " is older than deployed version " + old_v +
                                       "; bump the version to redeploy");
                spdlog::warn("Rejected downgrade of module {}: {} -> {}", name,
                             old_v, new_v);
                return false;
            }
        }

        ReducerContext ctx = CreateReducerContext("system", "");
        auto init = Execute(module, "__init", ctx, {});
        if (!init.success) {
            SetLoadError(name, "__init failed: " + init.error);
            spdlog::error("Module {} __init failed: {}", name, init.error);
            return false;  // previous module (if any) is untouched
        }

        if (previous) {
            // Reserved migration hook, invoked on the NEW module with
            // (old_version, new_version). Modules without a handler no-op
            // (-404). Failure aborts the swap; the old module keeps serving.
            auto migrate = Execute(module, "__migrate", ctx,
                                   {WasmValue(previous->metadata_.version),
                                    WasmValue(module->metadata_.version)});
            if (!migrate.success) {
                SetLoadError(name, "__migrate failed: " + migrate.error);
                spdlog::error("Module {} __migrate ({} -> {}) failed: {}", name,
                              previous->metadata_.version,
                              module->metadata_.version, migrate.error);
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(modules_mutex_);
            modules_[name] = module;
        }

        if (previous) {
            spdlog::info("Hot-swapped WASM module {} ({} -> {}, {} bytes)", name,
                         previous->metadata_.version.empty()
                             ? "?"
                             : previous->metadata_.version,
                         module->metadata_.version.empty()
                             ? "?"
                             : module->metadata_.version,
                         bytecode.size());
        } else {
            spdlog::info("Loaded WASM module {} (version '{}', {} bytes)", name,
                         module->metadata_.version, bytecode.size());
        }
        return true;
    }

    // Dotted numeric version compare ("1.2.10" > "1.2.9"); missing segments
    // are zero; non-numeric segments fall back to lexicographic comparison.
    static int CompareVersions(const std::string& a, const std::string& b) {
        size_t ia = 0, ib = 0;
        while (ia < a.size() || ib < b.size()) {
            std::string sa, sb;
            while (ia < a.size() && a[ia] != '.') sa += a[ia++];
            while (ib < b.size() && b[ib] != '.') sb += b[ib++];
            if (ia < a.size()) ia++;
            if (ib < b.size()) ib++;

            bool na = !sa.empty() && sa.find_first_not_of("0123456789") == std::string::npos;
            bool nb = !sb.empty() && sb.find_first_not_of("0123456789") == std::string::npos;
            if (na && nb) {
                long va = std::stol(sa), vb = std::stol(sb);
                if (va != vb) return va < vb ? -1 : 1;
            } else {
                if (sa != sb) return sa < sb ? -1 : 1;
            }
        }
        return 0;
    }

    bool UnloadModule(const std::string& name) {
        std::lock_guard<std::mutex> lock(modules_mutex_);
        auto it = modules_.find(name);
        if (it == modules_.end()) return false;
        modules_.erase(it);
        spdlog::info("Unloaded WASM module: {}", name);
        return true;
    }

    std::shared_ptr<WasmModule> GetModule(const std::string& name) const {
        std::lock_guard<std::mutex> lock(modules_mutex_);
        auto it = modules_.find(name);
        return it == modules_.end() ? nullptr : it->second;
    }

    std::vector<std::string> ListModules() const {
        std::lock_guard<std::mutex> lock(modules_mutex_);
        std::vector<std::string> names;
        names.reserve(modules_.size());
        for (const auto& [name, _] : modules_) names.push_back(name);
        return names;
    }

    WasmResult Execute(const std::shared_ptr<WasmModule>& module,
                       const std::string& reducer_name, const ReducerContext& ctx,
                       const std::vector<WasmValue>& args) {
        auto* impl = module->impl_.get();
        std::lock_guard<std::mutex> call_lock(impl->call_mutex);

        if (shutdown_requested_.load())
            return {false, "engine shutting down", {}};

        std::string error;
        if (!impl->instantiated && !Instantiate(*impl, error))
            return {false, "re-instantiation failed: " + error, {}};

        wasmtime_context_t* wctx = wasmtime_store_context(impl->store);

        HostCallContext call;
        call.rctx = &ctx;
        call.mod = impl;
        impl->current_call = &call;

        wasmtime_context_set_epoch_deadline(
            wctx, std::max<uint64_t>(1, impl->timeout_ms / WASM_EPOCH_TICK_MS));

        int32_t status = 0;
        bool trapped = false;
        std::string trap_msg;
        if (!CallInvoke(*impl, wctx, reducer_name, SerializeArgs(args), status,
                        trapped, trap_msg)) {
            impl->current_call = nullptr;
            impl->DestroyInstance();  // poisoned: trap, timeout or engine error
            std::string msg = !call.abort_msg.empty()
                                  ? "aborted: " + call.abort_msg
                                  : trap_msg.empty() ? "execution failed" : trap_msg;
            spdlog::warn("WASM {}.{} failed: {}", impl->module_name, reducer_name, msg);
            return {false, msg, {}};
        }
        impl->current_call = nullptr;

        // -404 from the SDK dispatcher = no handler registered for this name.
        if (status == WASM_ERR_NO_HANDLER) {
            if (IsReservedName(reducer_name)) return {true, "", {}};
            return {false, "unknown reducer: " + reducer_name, {}};
        }

        if (status < 0) {
            std::string detail(call.result.begin(), call.result.end());
            return {false,
                    "reducer returned error " + std::to_string(status) +
                        (detail.empty() ? "" : ": " + detail),
                    {}};
        }

        std::string commit_error;
        if (!CommitHostCall(call, commit_error)) {
            spdlog::error("WASM {}.{} commit failed: {}", impl->module_name,
                          reducer_name, commit_error);
            return {false, "commit failed: " + commit_error, {}};
        }

        WasmResult result;
        result.success = true;
        if (call.has_result)
            result.values.push_back(std::move(call.result));
        else
            result.values.push_back(status);
        return result;
    }

    ReducerContext CreateReducerContext(const std::string& sender_identity,
                                        const std::string& connection_id) {
        ReducerContext ctx;
        ctx.sender_identity = sender_identity;
        ctx.connection_id = connection_id;
        ctx.timestamp_micros =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        ctx.storage = storage_;
        ctx.changefeed = changefeed_;
        return ctx;
    }

    std::string GetLastLoadError() const {
        std::lock_guard<std::mutex> lock(load_error_mutex_);
        return last_load_error_;
    }

    std::shared_ptr<StorageEngine> storage_;
    std::shared_ptr<ChangefeedEngine> changefeed_;
    std::atomic<uint32_t> timeout_ms_{5000};
    std::atomic<uint32_t> memory_limit_mb_{256};

private:
    void SetLoadError(const std::string& name, const std::string& error) {
        std::lock_guard<std::mutex> lock(load_error_mutex_);
        last_load_error_ = name + ": " + error;
    }

    bool Compile(WasmModule& module, std::string& error) {
        auto* impl = module.impl_.get();
        wasmtime_error_t* err = wasmtime_module_new(
            engine_, module.bytecode_.data(), module.bytecode_.size(), &impl->module);
        if (err) {
            error = "invalid WASM module: " + ConsumeError(err);
            return false;
        }

        // Import allow-list: env.<known> and any wasi_snapshot_preview1.
        wasm_importtype_vec_t imports;
        wasmtime_module_imports(impl->module, &imports);
        bool ok = true;
        for (size_t i = 0; i < imports.size && ok; i++) {
            std::string mod = NameToString(wasm_importtype_module(imports.data[i]));
            std::string name = NameToString(wasm_importtype_name(imports.data[i]));
            module.metadata_.imports.push_back(mod + "." + name);
            if (mod == "wasi_snapshot_preview1") continue;
            if (mod == "env" && kKnownEnvImports.count(name)) continue;
            error = "disallowed import: " + mod + "." + name;
            ok = false;
        }
        wasm_importtype_vec_delete(&imports);
        if (!ok) return false;

        // Required exports.
        wasm_exporttype_vec_t exports;
        wasmtime_module_exports(impl->module, &exports);
        for (size_t i = 0; i < exports.size; i++)
            module.metadata_.exports.push_back(
                NameToString(wasm_exporttype_name(exports.data[i])));
        wasm_exporttype_vec_delete(&exports);

        for (const char* required : kRequiredExports) {
            bool found = false;
            for (const auto& e : module.metadata_.exports)
                if (e == required) { found = true; break; }
            if (!found) {
                error = std::string("missing required export: ") + required;
                return false;
            }
        }
        return true;
    }

    bool Instantiate(WasmModuleImpl& impl, std::string& error) {
        impl.DestroyInstance();

        impl.store = wasmtime_store_new(engine_, &impl, nullptr);
        wasmtime_context_t* ctx = wasmtime_store_context(impl.store);
        wasmtime_store_limiter(impl.store,
                               static_cast<int64_t>(impl.memory_limit_bytes),
                               100000 /*table elements*/, -1, -1, -1);

        wasi_config_t* wasi = wasi_config_new();
        if (impl.debug_stdio) {
            wasi_config_inherit_stdout(wasi);
            wasi_config_inherit_stderr(wasi);
        }
        if (wasmtime_error_t* err = wasmtime_context_set_wasi(ctx, wasi)) {
            error = "WASI setup failed: " + ConsumeError(err);
            impl.DestroyInstance();
            return false;
        }

        wasmtime_linker_t* linker = wasmtime_linker_new(engine_);
        if (wasmtime_error_t* err = wasmtime_linker_define_wasi(linker)) {
            error = "WASI linker setup failed: " + ConsumeError(err);
            wasmtime_linker_delete(linker);
            impl.DestroyInstance();
            return false;
        }
        if (!RegisterHostFunctions(linker, &impl, error)) {
            wasmtime_linker_delete(linker);
            impl.DestroyInstance();
            return false;
        }

        // Instantiation may run a start function; bound it like a call.
        wasmtime_context_set_epoch_deadline(
            ctx, std::max<uint64_t>(1, impl.timeout_ms / WASM_EPOCH_TICK_MS));

        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_linker_instantiate(
            linker, ctx, impl.module, &impl.instance, &trap);
        wasmtime_linker_delete(linker);
        if (err || trap) {
            error = "instantiation failed: " +
                    (err ? ConsumeError(err) : ConsumeTrap(trap));
            impl.DestroyInstance();
            return false;
        }

        if (!ResolveExports(impl, error)) {
            impl.DestroyInstance();
            return false;
        }

        // WASI reactor init (runs C#/clang static constructors, Main, etc.).
        if (impl.has_initialize) {
            wasm_trap_t* itrap = nullptr;
            wasmtime_error_t* ierr = wasmtime_func_call(
                ctx, &impl.fn_initialize, nullptr, 0, nullptr, 0, &itrap);
            if (ierr || itrap) {
                error = "_initialize failed: " +
                        (ierr ? ConsumeError(ierr) : ConsumeTrap(itrap));
                impl.DestroyInstance();
                return false;
            }
        }

        impl.instantiated = true;
        return true;
    }

    bool ResolveExports(WasmModuleImpl& impl, std::string& error) {
        wasmtime_context_t* ctx = wasmtime_store_context(impl.store);
        wasmtime_extern_t item;

        auto get_func = [&](const char* name, wasmtime_func_t* out) {
            if (!wasmtime_instance_export_get(ctx, &impl.instance, name,
                                              strlen(name), &item))
                return false;
            if (item.kind != WASMTIME_EXTERN_FUNC) return false;
            *out = item.of.func;
            return true;
        };

        if (!wasmtime_instance_export_get(ctx, &impl.instance, "memory", 6, &item) ||
            item.kind != WASMTIME_EXTERN_MEMORY) {
            error = "module does not export memory";
            return false;
        }
        impl.memory = item.of.memory;

        if (!get_func("origindb_invoke", &impl.fn_invoke)) {
            error = "module does not export origindb_invoke";
            return false;
        }
        if (!get_func("origindb_alloc", &impl.fn_alloc)) {
            error = "module does not export origindb_alloc";
            return false;
        }
        impl.has_free = get_func("origindb_free", &impl.fn_free);
        impl.has_describe = get_func("origindb_describe", &impl.fn_describe);
        impl.has_initialize = get_func("_initialize", &impl.fn_initialize);
        return true;
    }

    // Writes a buffer into guest memory via origindb_alloc. Returns false on
    // allocation failure.
    bool WriteGuestBuffer(WasmModuleImpl& impl, wasmtime_context_t* ctx,
                          const std::string& data, uint32_t& ptr_out) {
        wasmtime_val_t arg;
        arg.kind = WASMTIME_I32;
        arg.of.i32 = static_cast<int32_t>(data.size());
        wasmtime_val_t res;
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err =
            wasmtime_func_call(ctx, &impl.fn_alloc, &arg, 1, &res, 1, &trap);
        if (err) { ConsumeError(err); return false; }
        if (trap) { ConsumeTrap(trap); return false; }
        ptr_out = static_cast<uint32_t>(res.of.i32);
        if (ptr_out == 0 && !data.empty()) return false;

        uint8_t* base = wasmtime_memory_data(ctx, &impl.memory);
        size_t size = wasmtime_memory_data_size(ctx, &impl.memory);
        if (static_cast<uint64_t>(ptr_out) + data.size() > size) return false;
        memcpy(base + ptr_out, data.data(), data.size());
        return true;
    }

    bool CallInvoke(WasmModuleImpl& impl, wasmtime_context_t* ctx,
                    const std::string& name, const std::string& args_json,
                    int32_t& status_out, bool& trapped, std::string& trap_msg) {
        uint32_t name_ptr = 0, args_ptr = 0;
        if (!WriteGuestBuffer(impl, ctx, name, name_ptr) ||
            !WriteGuestBuffer(impl, ctx, args_json, args_ptr)) {
            trapped = true;
            trap_msg = "failed to write call arguments into guest memory";
            return false;
        }

        wasmtime_val_t call_args[4];
        for (auto& a : call_args) a.kind = WASMTIME_I32;
        call_args[0].of.i32 = static_cast<int32_t>(name_ptr);
        call_args[1].of.i32 = static_cast<int32_t>(name.size());
        call_args[2].of.i32 = static_cast<int32_t>(args_ptr);
        call_args[3].of.i32 = static_cast<int32_t>(args_json.size());

        wasmtime_val_t result;
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(ctx, &impl.fn_invoke, call_args,
                                                   4, &result, 1, &trap);
        if (err) {
            trapped = true;
            trap_msg = ConsumeError(err);
            return false;
        }
        if (trap) {
            trapped = true;
            trap_msg = ConsumeTrap(trap);
            return false;
        }
        status_out = result.of.i32;
        return true;
    }

    static std::string SerializeArgs(const std::vector<WasmValue>& args) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& value : args) {
            std::visit(
                [&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                        arr.push_back(nullptr);
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                        arr.push_back({{"$bytes", Base64Encode(v.data(), v.size())}});
                    else
                        arr.push_back(v);
                },
                value);
        }
        return arr.dump();
    }

    void ExtractMetadata(WasmModule& module) {
        auto* impl = module.impl_.get();
        if (!impl->has_describe) return;

        wasmtime_context_t* ctx = wasmtime_store_context(impl->store);
        wasmtime_val_t result;
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* err = wasmtime_func_call(ctx, &impl->fn_describe,
                                                   nullptr, 0, &result, 1, &trap);
        if (err) { ConsumeError(err); return; }
        if (trap) { ConsumeTrap(trap); return; }

        uint64_t packed = static_cast<uint64_t>(result.of.i64);
        uint32_t ptr = static_cast<uint32_t>(packed >> 32);
        uint32_t len = static_cast<uint32_t>(packed & 0xFFFFFFFF);

        uint8_t* base = wasmtime_memory_data(ctx, &impl->memory);
        size_t size = wasmtime_memory_data_size(ctx, &impl->memory);
        if (static_cast<uint64_t>(ptr) + len > size) {
            spdlog::warn("Module {} origindb_describe returned out-of-bounds blob",
                         module.name_);
            return;
        }

        const char* blob = reinterpret_cast<const char*>(base + ptr);
        auto meta = nlohmann::json::parse(blob, blob + len, nullptr, false);
        if (meta.is_discarded() || !meta.is_object()) {
            spdlog::warn("Module {} origindb_describe returned invalid JSON",
                         module.name_);
            return;
        }

        if (module.metadata_.version.empty() && meta.contains("version") &&
            meta["version"].is_string())
            module.metadata_.version = meta["version"].get<std::string>();
        if (meta.contains("reducers") && meta["reducers"].is_array()) {
            for (const auto& r : meta["reducers"]) {
                ReducerDef def;
                if (r.is_string()) {
                    def.name = r.get<std::string>();
                } else if (r.is_object() && r.contains("name")) {
                    def.name = r["name"].get<std::string>();
                    if (r.contains("params") && r["params"].is_array())
                        for (const auto& p : r["params"])
                            def.param_types.push_back(p.is_string() ? p.get<std::string>()
                                                                    : "any");
                } else {
                    continue;
                }
                def.is_init = def.name == "__init";
                def.is_client_connected = def.name == "__client_connected";
                def.is_client_disconnected = def.name == "__client_disconnected";
                module.metadata_.reducers.push_back(std::move(def));
            }
        }
    }

    wasm_engine_t* engine_ = nullptr;
    std::thread epoch_thread_;
    std::atomic<bool> ticker_running_{false};
    std::atomic<bool> shutdown_requested_{false};

    mutable std::mutex modules_mutex_;
    std::unordered_map<std::string, std::shared_ptr<WasmModule>> modules_;

    mutable std::mutex load_error_mutex_;
    std::string last_load_error_;
};

// ---------------------------------------------------------------------------
// WasmEngine public API
// ---------------------------------------------------------------------------

WasmEngine::WasmEngine(std::shared_ptr<StorageEngine> storage,
                       std::shared_ptr<ChangefeedEngine> changefeed)
    : impl_(std::make_unique<Impl>(std::move(storage), std::move(changefeed))) {}

WasmEngine::~WasmEngine() = default;

bool WasmEngine::Initialize() { return impl_->Initialize(); }
void WasmEngine::Shutdown() { impl_->Shutdown(); }
void WasmEngine::RequestShutdown() { impl_->RequestShutdown(); }

bool WasmEngine::LoadModule(const std::string& name,
                            const std::vector<uint8_t>& bytecode,
                            const std::string& version,
                            const ModuleCapabilities& capabilities) {
    return impl_->LoadModule(name, bytecode, version, capabilities);
}

bool WasmEngine::UnloadModule(const std::string& name) {
    return impl_->UnloadModule(name);
}

std::shared_ptr<WasmModule> WasmEngine::GetModule(const std::string& name) {
    return impl_->GetModule(name);
}

std::vector<std::string> WasmEngine::ListModules() const {
    return impl_->ListModules();
}

std::vector<std::string> WasmEngine::GetLoadedModules() const {
    return impl_->ListModules();
}

std::string WasmEngine::GetLastLoadError() const {
    return impl_->GetLastLoadError();
}

WasmResult WasmEngine::ExecuteReducer(const std::string& module_name,
                                      const std::string& reducer_name,
                                      const ReducerContext& ctx,
                                      const std::vector<WasmValue>& args) {
    auto module = impl_->GetModule(module_name);
    if (!module) return {false, "module not found: " + module_name, {}};
    return impl_->Execute(module, reducer_name, ctx, args);
}

WasmResult WasmEngine::OnModuleInit(const std::string& module_name,
                                    const ReducerContext& ctx) {
    return ExecuteReducer(module_name, "__init", ctx, {});
}

WasmResult WasmEngine::OnClientConnected(const std::string& module_name,
                                         const ReducerContext& ctx) {
    return ExecuteReducer(module_name, "__client_connected", ctx,
                          {WasmValue(ctx.connection_id)});
}

WasmResult WasmEngine::OnClientDisconnected(const std::string& module_name,
                                            const ReducerContext& ctx) {
    return ExecuteReducer(module_name, "__client_disconnected", ctx,
                          {WasmValue(ctx.connection_id)});
}

void WasmEngine::SetTimeoutMs(uint32_t timeout_ms) {
    impl_->timeout_ms_ = timeout_ms;
}

void WasmEngine::SetMemoryLimitMB(uint32_t memory_limit_mb) {
    impl_->memory_limit_mb_ = memory_limit_mb;
}

std::shared_ptr<StorageEngine> WasmEngine::GetStorageEngine() const {
    return impl_->storage_;
}

std::shared_ptr<ChangefeedEngine> WasmEngine::GetChangefeedEngine() const {
    return impl_->changefeed_;
}

ReducerContext WasmEngine::CreateReducerContext(const std::string& sender_identity,
                                                const std::string& connection_id) {
    return impl_->CreateReducerContext(sender_identity, connection_id);
}

} // namespace origindb
