// Implementations of the "env" host imports exposed to WASM modules, plus the
// staged-write commit path. See docs/WASM_ABI.md for the contract.

#include "wasm_runtime_internal.h"

#include "storage/storage_engine.h"
#include "changefeed/changefeed_engine.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstring>

namespace origindb {

namespace {

std::atomic<uint64_t> g_id_counter{0};

// ---------------------------------------------------------------------------
// Guest memory access. The data pointer is invalidated by any guest call or
// memory growth, so it is always re-fetched from the caller context.
// ---------------------------------------------------------------------------

struct GuestMem {
    wasmtime_context_t* ctx;
    WasmModuleImpl* mod;

    uint8_t* base() const { return wasmtime_memory_data(ctx, &mod->memory); }
    size_t size() const { return wasmtime_memory_data_size(ctx, &mod->memory); }

    bool InBounds(uint32_t ptr, uint32_t len) const {
        size_t sz = size();
        return static_cast<uint64_t>(ptr) + len <= sz;
    }

    // Copies out [ptr, ptr+len). Empty optional on OOB.
    std::optional<std::vector<uint8_t>> ReadBytes(uint32_t ptr, uint32_t len) const {
        if (!InBounds(ptr, len)) return std::nullopt;
        const uint8_t* b = base();
        return std::vector<uint8_t>(b + ptr, b + ptr + len);
    }

    // Reads a NUL-terminated string with a byte cap. Empty optional on OOB /
    // missing terminator within cap.
    std::optional<std::string> ReadCString(uint32_t ptr, size_t cap) const {
        size_t sz = size();
        if (ptr >= sz) return std::nullopt;
        const uint8_t* b = base();
        size_t limit = std::min(sz - ptr, cap);
        const void* nul = memchr(b + ptr, 0, limit);
        if (!nul) return std::nullopt;
        return std::string(reinterpret_cast<const char*>(b + ptr));
    }

    bool WriteBytes(uint32_t ptr, const uint8_t* data, size_t len) const {
        if (!InBounds(ptr, static_cast<uint32_t>(len))) return false;
        memcpy(base() + ptr, data, len);
        return true;
    }

    bool WriteU32(uint32_t ptr, uint32_t value) const {
        return WriteBytes(ptr, reinterpret_cast<const uint8_t*>(&value), 4);
    }
};

// Calls the guest's exported origindb_alloc. Returns 0 on failure.
uint32_t GuestAlloc(wasmtime_context_t* ctx, WasmModuleImpl* mod, uint32_t size) {
    wasmtime_val_t arg;
    arg.kind = WASMTIME_I32;
    arg.of.i32 = static_cast<int32_t>(size);
    wasmtime_val_t result;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_func_call(ctx, &mod->fn_alloc, &arg, 1, &result, 1, &trap);
    if (err) {
        spdlog::warn("[wasm:{}] origindb_alloc failed: {}", mod->module_name,
                     ConsumeError(err));
        return 0;
    }
    if (trap) {
        spdlog::warn("[wasm:{}] origindb_alloc trapped: {}", mod->module_name,
                     ConsumeTrap(trap));
        return 0;
    }
    return static_cast<uint32_t>(result.of.i32);
}

// Allocates guest memory via origindb_alloc, writes `data` into it, and
// stores the pointer/length into *out_ptr / *out_len. Returns false on any
// failure (allocation, bounds).
bool WriteOut(wasmtime_context_t* ctx, WasmModuleImpl* mod,
              const std::string& data, uint32_t out_ptr, uint32_t out_len) {
    uint32_t buf = GuestAlloc(ctx, mod, static_cast<uint32_t>(data.size()));
    if (buf == 0 && !data.empty()) return false;
    GuestMem mem{ctx, mod};
    if (!data.empty() &&
        !mem.WriteBytes(buf, reinterpret_cast<const uint8_t*>(data.data()), data.size()))
        return false;
    return mem.WriteU32(out_ptr, buf) && mem.WriteU32(out_len, static_cast<uint32_t>(data.size()));
}

bool TableAllowed(const WasmModuleImpl* mod, const std::string& table) {
    if (!mod->caps || mod->caps->allowed_tables.empty()) return true;
    for (const auto& t : mod->caps->allowed_tables)
        if (t == table) return true;
    return false;
}

wasm_trap_t* Trap(const std::string& msg) {
    return wasmtime_trap_new(msg.c_str(), msg.size());
}

// Shared prologue for host callbacks: resolves the module, active call and
// context. Returns nullptr trap on success.
struct HostFrame {
    WasmModuleImpl* mod;
    HostCallContext* call;
    wasmtime_context_t* ctx;
};

wasm_trap_t* OpenFrame(void* env, wasmtime_caller_t* caller, HostFrame& out) {
    out.mod = static_cast<WasmModuleImpl*>(env);
    out.call = out.mod->current_call;
    out.ctx = wasmtime_caller_context(caller);
    if (!out.call) return Trap("host call outside reducer execution");
    return nullptr;
}

void SetI32(wasmtime_val_t* results, int32_t v) {
    results[0].kind = WASMTIME_I32;
    results[0].of.i32 = v;
}

void SetI64(wasmtime_val_t* results, int64_t v) {
    results[0].kind = WASMTIME_I64;
    results[0].of.i64 = v;
}

// ---------------------------------------------------------------------------
// env.* host functions
// ---------------------------------------------------------------------------

// (table, key, key_len, out_ptr, out_len) -> i32   1 hit / 0 miss / <0 error
wasm_trap_t* HostTableRead(void* env, wasmtime_caller_t* caller,
                           const wasmtime_val_t* args, size_t,
                           wasmtime_val_t* results, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    GuestMem mem{f.ctx, f.mod};

    auto table = mem.ReadCString(args[0].of.i32, WASM_MAX_TABLE_NAME);
    auto key_bytes = mem.ReadBytes(args[1].of.i32, args[2].of.i32);
    if (!table || !key_bytes) return Trap("host_table_read: out-of-bounds argument");
    std::string key(key_bytes->begin(), key_bytes->end());

    if (!TableAllowed(f.mod, *table)) {
        SetI32(results, WASM_ERR_PERMISSION);
        return nullptr;
    }

    nlohmann::json value;
    // Overlay first: read-your-writes including tombstones.
    auto tit = f.call->overlay.find(*table);
    if (tit != f.call->overlay.end()) {
        auto kit = tit->second.find(key);
        if (kit != tit->second.end()) {
            if (!kit->second.has_value()) {  // staged delete
                SetI32(results, 0);
                return nullptr;
            }
            value = *kit->second;
            if (!WriteOut(f.ctx, f.mod, value.dump(), args[3].of.i32, args[4].of.i32))
                return Trap("host_table_read: result write failed");
            SetI32(results, 1);
            return nullptr;
        }
    }

    auto tbl = f.call->rctx->storage->GetTable(*table);
    auto vj = tbl ? tbl->GetJsonCached(key, RowJsonSerializer())
                  : std::optional<std::string>{};
    if (!vj) {
        SetI32(results, 0);
        return nullptr;
    }
    if (!WriteOut(f.ctx, f.mod, *vj, args[3].of.i32, args[4].of.i32))
        return Trap("host_table_read: result write failed");
    SetI32(results, 1);
    return nullptr;
}

// (table, key, key_len, value, value_len) -> i32   0 ok / <0 error
wasm_trap_t* HostTableWrite(void* env, wasmtime_caller_t* caller,
                            const wasmtime_val_t* args, size_t,
                            wasmtime_val_t* results, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    GuestMem mem{f.ctx, f.mod};

    auto table = mem.ReadCString(args[0].of.i32, WASM_MAX_TABLE_NAME);
    auto key_bytes = mem.ReadBytes(args[1].of.i32, args[2].of.i32);
    auto value_bytes = mem.ReadBytes(args[3].of.i32, args[4].of.i32);
    if (!table || !key_bytes || !value_bytes)
        return Trap("host_table_write: out-of-bounds argument");

    if ((f.mod->caps && f.mod->caps->read_only) || !TableAllowed(f.mod, *table)) {
        SetI32(results, WASM_ERR_PERMISSION);
        return nullptr;
    }

    nlohmann::json value = nlohmann::json::parse(
        value_bytes->begin(), value_bytes->end(), nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        SetI32(results, WASM_ERR_INVALID_ARG);
        return nullptr;
    }

    std::string key(key_bytes->begin(), key_bytes->end());
    f.call->overlay[*table][key] = std::move(value);
    SetI32(results, 0);
    return nullptr;
}

// (table, key, key_len) -> i32   0 ok / <0 error
wasm_trap_t* HostTableDelete(void* env, wasmtime_caller_t* caller,
                             const wasmtime_val_t* args, size_t,
                             wasmtime_val_t* results, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    GuestMem mem{f.ctx, f.mod};

    auto table = mem.ReadCString(args[0].of.i32, WASM_MAX_TABLE_NAME);
    auto key_bytes = mem.ReadBytes(args[1].of.i32, args[2].of.i32);
    if (!table || !key_bytes) return Trap("host_table_delete: out-of-bounds argument");

    if ((f.mod->caps && f.mod->caps->read_only) || !TableAllowed(f.mod, *table)) {
        SetI32(results, WASM_ERR_PERMISSION);
        return nullptr;
    }

    std::string key(key_bytes->begin(), key_bytes->end());
    f.call->overlay[*table][key] = std::nullopt;
    SetI32(results, 0);
    return nullptr;
}

// (table, prefix, prefix_len, limit, out_ptr, out_len) -> i32  count / <0 error
wasm_trap_t* HostTableScan(void* env, wasmtime_caller_t* caller,
                           const wasmtime_val_t* args, size_t,
                           wasmtime_val_t* results, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    GuestMem mem{f.ctx, f.mod};

    auto table = mem.ReadCString(args[0].of.i32, WASM_MAX_TABLE_NAME);
    auto prefix_bytes = mem.ReadBytes(args[1].of.i32, args[2].of.i32);
    if (!table || !prefix_bytes) return Trap("host_table_scan: out-of-bounds argument");
    std::string prefix(prefix_bytes->begin(), prefix_bytes->end());

    if (!TableAllowed(f.mod, *table)) {
        SetI32(results, WASM_ERR_PERMISSION);
        return nullptr;
    }

    int32_t limit = args[3].of.i32;
    if (limit <= 0) limit = WASM_DEFAULT_SCAN_LIMIT;

    // Compute the exclusive upper bound for the prefix range.
    std::string end = prefix;
    while (!end.empty()) {
        if (static_cast<unsigned char>(end.back()) < 0xFF) {
            end.back() = static_cast<char>(end.back() + 1);
            break;
        }
        end.pop_back();
    }

    // Committed rows come back already-serialized (and memoized) via the
    // cached-JSON path — unchanged rows skip re-serialization entirely. Merge
    // into a sorted map (stable output; overlay overrides committed).
    std::map<std::string, std::string> rows;  // key -> value JSON
    auto tbl = f.call->rctx->storage->GetTable(*table);
    if (tbl) {
        tbl->ScanJsonCached(
            prefix, end, RowJsonSerializer(),
            [&](const std::string& key, const std::string& value_json) {
                if (key.compare(0, prefix.size(), prefix) == 0)
                    rows[key] = value_json;
                return true;
            });
    }

    // Apply this call's staged overlay (read-your-writes).
    auto tit = f.call->overlay.find(*table);
    if (tit != f.call->overlay.end()) {
        for (const auto& [key, val] : tit->second) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            if (val.has_value())
                rows[key] = val->dump();
            else
                rows.erase(key);
        }
    }

    // Build the JSON array in a single pass, splicing pre-serialized values.
    std::string out = "[";
    int32_t count = 0;
    for (const auto& [key, value_json] : rows) {
        if (count >= limit) break;
        if (count) out += ',';
        out += "{\"key\":";
        out += nlohmann::json(key).dump();
        out += ",\"value\":";
        out += value_json;
        out += '}';
        count++;
    }
    out += ']';

    if (!WriteOut(f.ctx, f.mod, out, args[4].of.i32, args[5].of.i32))
        return Trap("host_table_scan: result write failed");
    SetI32(results, count);
    return nullptr;
}

// (topic, key, key_len, payload, payload_len) -> i32
wasm_trap_t* HostEmitEvent(void* env, wasmtime_caller_t* caller,
                           const wasmtime_val_t* args, size_t,
                           wasmtime_val_t* results, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    GuestMem mem{f.ctx, f.mod};

    auto topic = mem.ReadCString(args[0].of.i32, WASM_MAX_TABLE_NAME);
    auto key_bytes = mem.ReadBytes(args[1].of.i32, args[2].of.i32);
    auto payload = mem.ReadBytes(args[3].of.i32, args[4].of.i32);
    if (!topic || !key_bytes || !payload)
        return Trap("host_emit_event: out-of-bounds argument");

    f.call->events.push_back({*topic,
                              std::string(key_bytes->begin(), key_bytes->end()),
                              std::move(*payload)});
    SetI32(results, 0);
    return nullptr;
}

// () -> i64
wasm_trap_t* HostNowMs(void* env, wasmtime_caller_t* caller,
                       const wasmtime_val_t*, size_t,
                       wasmtime_val_t* results, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    SetI64(results, static_cast<int64_t>(f.call->rctx->timestamp_micros / 1000));
    return nullptr;
}

// () -> i64
wasm_trap_t* HostGenerateId(void* env, wasmtime_caller_t* caller,
                            const wasmtime_val_t*, size_t,
                            wasmtime_val_t* results, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    uint64_t ms = f.call->rctx->timestamp_micros / 1000;
    uint64_t id = (ms << 20) | (g_id_counter.fetch_add(1) & 0xFFFFF);
    SetI64(results, static_cast<int64_t>(id));
    return nullptr;
}

// (level, msg) -> ()
wasm_trap_t* HostLog(void* env, wasmtime_caller_t* caller,
                     const wasmtime_val_t* args, size_t,
                     wasmtime_val_t*, size_t) {
    auto* mod = static_cast<WasmModuleImpl*>(env);
    GuestMem mem{wasmtime_caller_context(caller), mod};
    auto msg = mem.ReadCString(args[1].of.i32, WASM_MAX_LOG_MSG);
    if (!msg) return Trap("host_log: out-of-bounds message");
    switch (args[0].of.i32) {
        case 0: spdlog::trace("[wasm:{}] {}", mod->module_name, *msg); break;
        case 1: spdlog::debug("[wasm:{}] {}", mod->module_name, *msg); break;
        case 2: spdlog::info("[wasm:{}] {}", mod->module_name, *msg); break;
        case 3: spdlog::warn("[wasm:{}] {}", mod->module_name, *msg); break;
        default: spdlog::error("[wasm:{}] {}", mod->module_name, *msg); break;
    }
    return nullptr;
}

// (msg) -> ()  — records the message and traps (fails the call, rolls back)
wasm_trap_t* HostAbort(void* env, wasmtime_caller_t* caller,
                       const wasmtime_val_t* args, size_t,
                       wasmtime_val_t*, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    GuestMem mem{f.ctx, f.mod};
    auto msg = mem.ReadCString(args[0].of.i32, WASM_MAX_LOG_MSG);
    f.call->abort_msg = msg.value_or("(unreadable abort message)");
    return Trap("aborted: " + f.call->abort_msg);
}

// (size) -> i32 — host-side scratch inside guest memory, grown on demand.
// Kept for SDK compatibility; guests may also use their own allocator.
wasm_trap_t* HostAlloc(void* env, wasmtime_caller_t* caller,
                       const wasmtime_val_t* args, size_t,
                       wasmtime_val_t* results, size_t) {
    auto* mod = static_cast<WasmModuleImpl*>(env);
    wasmtime_context_t* ctx = wasmtime_caller_context(caller);
    uint32_t size = static_cast<uint32_t>(args[0].of.i32);
    if (size == 0) {
        SetI32(results, 0);
        return nullptr;
    }
    uint64_t pages = (static_cast<uint64_t>(size) + 65535) / 65536;
    uint64_t prev = 0;
    wasmtime_error_t* err = wasmtime_memory_grow(ctx, &mod->memory, pages, &prev);
    if (err) {
        spdlog::warn("[wasm:{}] host_alloc grow failed: {}", mod->module_name,
                     ConsumeError(err));
        SetI32(results, 0);
        return nullptr;
    }
    SetI32(results, static_cast<int32_t>(prev * 65536));
    return nullptr;
}

// (ptr) -> () — arena freed with the store; no-op.
wasm_trap_t* HostFree(void*, wasmtime_caller_t*,
                      const wasmtime_val_t*, size_t,
                      wasmtime_val_t*, size_t) {
    return nullptr;
}

// (ptr, len) -> () — copies the reducer's result payload out of guest memory.
wasm_trap_t* HostSetResult(void* env, wasmtime_caller_t* caller,
                           const wasmtime_val_t* args, size_t,
                           wasmtime_val_t*, size_t) {
    HostFrame f{};
    if (auto* t = OpenFrame(env, caller, f)) return t;
    uint32_t len = static_cast<uint32_t>(args[1].of.i32);
    if (len > WASM_MAX_RESULT) return Trap("host_set_result: payload exceeds 16MiB cap");
    GuestMem mem{f.ctx, f.mod};
    auto bytes = mem.ReadBytes(args[0].of.i32, len);
    if (!bytes) return Trap("host_set_result: out-of-bounds payload");
    f.call->result = std::move(*bytes);
    f.call->has_result = true;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

enum class VT { I32, I64 };

wasm_functype_t* MakeFuncType(std::initializer_list<VT> params,
                              std::initializer_list<VT> rets) {
    wasm_valtype_vec_t pvec, rvec;
    std::vector<wasm_valtype_t*> ps, rs;
    for (VT v : params) ps.push_back(wasm_valtype_new(v == VT::I32 ? WASM_I32 : WASM_I64));
    for (VT v : rets) rs.push_back(wasm_valtype_new(v == VT::I32 ? WASM_I32 : WASM_I64));
    wasm_valtype_vec_new(&pvec, ps.size(), ps.data());
    wasm_valtype_vec_new(&rvec, rs.size(), rs.data());
    return wasm_functype_new(&pvec, &rvec);
}

}  // namespace

bool RegisterHostFunctions(wasmtime_linker_t* linker, WasmModuleImpl* impl,
                           std::string& error) {
    struct Def {
        const char* name;
        std::initializer_list<VT> params;
        std::initializer_list<VT> rets;
        wasmtime_func_callback_t cb;
    };
    const VT i32 = VT::I32;
    const Def defs[] = {
        {"host_table_read", {i32, i32, i32, i32, i32}, {i32}, HostTableRead},
        {"host_table_write", {i32, i32, i32, i32, i32}, {i32}, HostTableWrite},
        {"host_table_delete", {i32, i32, i32}, {i32}, HostTableDelete},
        {"host_table_scan", {i32, i32, i32, i32, i32, i32}, {i32}, HostTableScan},
        {"host_emit_event", {i32, i32, i32, i32, i32}, {i32}, HostEmitEvent},
        {"host_now_ms", {}, {VT::I64}, HostNowMs},
        {"host_generate_id", {}, {VT::I64}, HostGenerateId},
        {"host_log", {i32, i32}, {}, HostLog},
        {"host_abort", {i32}, {}, HostAbort},
        {"host_alloc", {i32}, {i32}, HostAlloc},
        {"host_free", {i32}, {}, HostFree},
        {"host_set_result", {i32, i32}, {}, HostSetResult},
    };

    for (const auto& d : defs) {
        wasm_functype_t* ty = MakeFuncType(d.params, d.rets);
        wasmtime_error_t* err = wasmtime_linker_define_func(
            linker, "env", 3, d.name, strlen(d.name), ty, d.cb, impl, nullptr);
        wasm_functype_delete(ty);
        if (err) {
            error = std::string("failed to define env.") + d.name + ": " + ConsumeError(err);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Commit path
// ---------------------------------------------------------------------------

namespace {

// Best-effort column typing for auto-created tables written by modules.
TableSchema SchemaFromJson(const std::string& table, const nlohmann::json& columns) {
    TableSchema schema;
    schema.name = table;
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        Column col;
        col.name = it.key();
        if (it->is_boolean()) col.type = DataType::BOOLEAN;
        else if (it->is_number_integer()) col.type = DataType::INT64;
        else if (it->is_number_float()) col.type = DataType::DOUBLE;
        else col.type = DataType::STRING;
        col.nullable = true;
        schema.columns.push_back(col);
    }
    return schema;
}

}  // namespace

bool CommitHostCall(HostCallContext& call, std::string& error) {
    auto storage = call.rctx->storage;
    if (!storage) {
        error = "no storage engine in reducer context";
        return false;
    }

    if (!call.overlay.empty()) {
        // Auto-create missing tables so modules can write without SQL DDL.
        for (const auto& [table, writes] : call.overlay) {
            if (storage->GetTable(table)) continue;
            for (const auto& [key, val] : writes) {
                if (!val.has_value()) continue;
                // Another module may win the create race; existing is fine.
                if (!storage->CreateTable(SchemaFromJson(table, *val)) &&
                    !storage->GetTable(table)) {
                    error = "failed to auto-create table: " + table;
                    return false;
                }
                break;
            }
        }

        auto txn = storage->BeginTransaction(IsolationLevel::READ_COMMITTED);
        if (!txn) {
            error = "failed to begin transaction";
            return false;
        }
        for (const auto& [table, writes] : call.overlay) {
            for (const auto& [key, val] : writes) {
                bool ok;
                if (!val.has_value()) {
                    ok = txn->Delete(table, key);
                } else {
                    Row row = JsonToRow(key, *val);
                    ok = storage->Get(table, key).has_value()
                             ? txn->Update(table, key, row)
                             : txn->Insert(table, row);
                }
                if (!ok) {
                    storage->Rollback(txn);
                    error = "failed to stage write to " + table + "/" + key;
                    return false;
                }
            }
        }
        if (!storage->Commit(txn)) {
            error = "transaction commit failed";
            return false;
        }
    }

    // Custom events publish only after a successful commit.
    if (call.rctx->changefeed) {
        for (auto& ev : call.events) {
            ChangefeedEvent cf;
            cf.table = ev.topic;
            cf.operation = "EVENT";
            cf.key.assign(ev.key.begin(), ev.key.end());
            cf.new_value = std::move(ev.payload);
            cf.transaction_id = "wasm-" + call.mod->module_name;
            cf.timestamp = std::chrono::system_clock::now();
            call.rctx->changefeed->PublishEvent(cf);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON <-> storage conversions
// ---------------------------------------------------------------------------

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? kB64[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? kB64[n & 63] : '=');
    }
    return out;
}

std::optional<std::vector<uint8_t>> Base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (in.size() % 4 != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int a = val(in[i]), b = val(in[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (in[i + 2] != '=') {
            int c = val(in[i + 2]);
            if (c < 0) return std::nullopt;
            out.push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
            if (in[i + 3] != '=') {
                int d = val(in[i + 3]);
                if (d < 0) return std::nullopt;
                out.push_back(static_cast<uint8_t>((c << 6) | d));
            }
        }
    }
    return out;
}

const Table::JsonSerializer& RowJsonSerializer() {
    static const Table::JsonSerializer ser =
        [](const Row& r) { return RowToJson(r).dump(); };
    return ser;
}

nlohmann::json RowToJson(const Row& row) {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [name, value] : row.columns) {
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    obj[name] = nullptr;
                } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                    obj[name] = {{"$bytes", Base64Encode(v.data(), v.size())}};
                } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                    obj[name] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    v.time_since_epoch())
                                    .count();
                } else {
                    obj[name] = v;
                }
            },
            value);
    }
    return obj;
}

Row JsonToRow(const std::string& key, const nlohmann::json& columns) {
    Row row;
    row.key = key;
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        const auto& v = *it;
        if (v.is_null()) {
            row.columns[it.key()] = std::monostate{};
        } else if (v.is_boolean()) {
            row.columns[it.key()] = v.get<bool>();
        } else if (v.is_number_integer()) {
            row.columns[it.key()] = v.get<int64_t>();
        } else if (v.is_number_float()) {
            row.columns[it.key()] = v.get<double>();
        } else if (v.is_object() && v.contains("$bytes") && v["$bytes"].is_string()) {
            auto bytes = Base64Decode(v["$bytes"].get<std::string>());
            row.columns[it.key()] = bytes ? Value(std::move(*bytes)) : Value(std::monostate{});
        } else if (v.is_string()) {
            row.columns[it.key()] = v.get<std::string>();
        } else {
            // Nested arrays/objects stored as their JSON text.
            row.columns[it.key()] = v.dump();
        }
    }
    return row;
}

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

std::string ConsumeError(wasmtime_error_t* err) {
    wasm_name_t msg;
    wasmtime_error_message(err, &msg);
    std::string out(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return out;
}

std::string ConsumeTrap(wasm_trap_t* trap) {
    wasm_message_t msg;
    wasm_trap_message(trap, &msg);
    std::string out(msg.data, msg.size);
    // Trap messages are NUL-terminated wasm_message_t; strip trailing NUL.
    while (!out.empty() && out.back() == '\0') out.pop_back();
    wasm_byte_vec_delete(&msg);
    wasm_trap_delete(trap);
    return out;
}

} // namespace origindb
