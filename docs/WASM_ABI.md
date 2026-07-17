# OriginDB WASM Module ABI (v1)

The contract between the OriginDB server (wasmtime host) and user modules.
All SDKs (C#, TypeScript/AssemblyScript, C++) must conform to this document.

## Module requirements

- Core WebAssembly module (WASI preview 1 imports allowed). WASI *components*
  (preview 2) are not supported.
- All pointers are `i32` byte offsets into the module's exported linear
  memory. The host bounds-checks every pointer/length pair; violations trap
  and the call is rolled back.

### Required exports

| Export | Signature | Purpose |
|---|---|---|
| `memory` | memory | linear memory |
| `origindb_alloc` | `(i32 size) -> i32 ptr` | Guest allocator. The host uses it for every host→guest buffer (invoke name/args, read/scan results). Return 0 on failure. |
| `origindb_invoke` | `(i32 name_ptr, i32 name_len, i32 args_ptr, i32 args_len) -> i32 status` | Single dispatch entry point for all reducers and lifecycle hooks. |

### Optional exports

| Export | Signature | Purpose |
|---|---|---|
| `_initialize` | `() -> ()` | WASI reactor initialization; called once per instantiation, before anything else. Register reducers here (or in static ctors it runs). |
| `origindb_free` | `(i32 ptr) -> ()` | Frees an `origindb_alloc` buffer. The host currently does not call it (instance memory is reclaimed on re-instantiation), but SDKs should export it. |
| `origindb_describe` | `() -> i64` | Returns `(ptr << 32) \| len` of a UTF-8 JSON metadata blob: `{"name": str, "version": str, "reducers": [{"name": str, "params": [str]}], "tables": [str]}`. Called once at deploy. |

## invoke semantics

- `name` is the reducer name (UTF-8, exact match). Reserved lifecycle names
  begin with `__`:
  - `__init` — after deploy/instantiation
  - `__migrate` — args: `[old_version, new_version]`; invoked on the NEW
    module when it replaces an already-deployed module (hot-swap). Staged
    writes commit like any reducer, so schema/data migrations are ordinary
    table operations. A non-zero status aborts the swap and the old module
    keeps serving. Deploys with a version lower than the deployed one are
    rejected before `__migrate` runs.
  - `__client_connected` / `__client_disconnected` — args: `[connection_id]`
  - `__get_initial_data` — args: `[where_clause]`
  - filter/transform functions are invoked under whatever name the
    subscription registered (no `__` prefix required).
- `args` is a UTF-8 JSON array. Scalars map naturally; binary data is
  `{"$bytes": "<base64>"}`.
- Return `status`:
  - `0` — success. Staged writes commit.
  - `1` (filter functions) — include event. `0` = exclude. Any staged writes
    still commit on non-negative status.
  - `< 0` — error. Staged writes and events are discarded. `-404` is reserved
    for "no handler registered for this name" (the host treats `-404` on a
    reserved `__` name as a harmless no-op).
- Result payloads (query results, transformed events, initial data) are
  returned by calling `env.host_set_result` before returning; not via the
  status code.

## Host imports (module `"env"`)

`table`, `topic` and message pointers are NUL-terminated UTF-8 (table/topic
max 256 bytes, log/abort messages max 4 KiB). Keys and values are
pointer+length (not NUL-terminated).

| Import | Signature | Semantics |
|---|---|---|
| `host_table_read` | `(table, key, key_len, out_ptr, out_len) -> i32` | 1 = found (result JSON object written to a buffer allocated via `origindb_alloc`; its offset stored at `*out_ptr`, length at `*out_len`), 0 = not found, <0 = error. Sees the call's own staged writes, then committed data. |
| `host_table_write` | `(table, key, key_len, value, value_len) -> i32` | Stages an upsert. `value` must be a UTF-8 JSON object (column → value). 0 ok. Not visible to other calls until commit. |
| `host_table_delete` | `(table, key, key_len) -> i32` | Stages a delete. 0 ok. |
| `host_table_scan` | `(table, prefix, prefix_len, limit, out_ptr, out_len) -> i32` | Prefix scan over committed rows merged with staged writes. Result: JSON array `[{"key": str, "value": obj}]`, key-ordered, at most `limit` rows (`limit <= 0` → 1000). Returns row count or <0. |
| `host_emit_event` | `(topic, key, key_len, payload, payload_len) -> i32` | Stages a custom changefeed event (operation `"EVENT"`). Published only after a successful commit. |
| `host_now_ms` | `() -> i64` | Wall clock, **fixed for the duration of the call** (deterministic replay). |
| `host_generate_id` | `() -> i64` | Unique id: `(now_ms << 20) \| counter`. |
| `host_log` | `(i32 level, msg) -> ()` | level 0..4 = trace/debug/info/warn/error. |
| `host_abort` | `(msg) -> ()` | Records the message and traps. The call fails, everything staged is discarded. |
| `host_alloc` | `(i32 size) -> i32` | Host-managed scratch arena inside guest memory (grown via `memory.grow`). Valid until the call returns. 0 on failure. |
| `host_free` | `(i32 ptr) -> ()` | No-op (arena reclaimed with the instance). |
| `host_set_result` | `(ptr, len) -> ()` | Copies the call's result payload to the host (max 16 MiB, last call wins). |

Error codes: `-1` internal, `-2` permission denied (capability), `-3` invalid
argument, `-5` limit exceeded, `-404` no handler (SDK dispatcher only).

## Transaction model

Each `origindb_invoke` call runs against a staged-write overlay:
reads see the call's own writes first, then committed storage. On status
`>= 0` and no trap, the overlay is applied atomically through one storage
transaction (WAL-logged; changefeed events for each write are emitted by the
storage commit), then staged `host_emit_event`s are published. On trap,
`host_abort`, timeout, memory-limit or negative status, nothing is applied.

Tables written by modules are auto-created on first commit if they don't
exist (column types inferred from the first staged row).

## Execution model & limits

- One long-lived instance per module; calls are serialized per module.
  Different modules execute concurrently.
- After a trap/timeout the instance is discarded and lazily re-instantiated
  on the next call (fresh memory; `_initialize` runs again). Module state in
  linear memory does NOT survive traps — keep durable state in tables.
- CPU: epoch-based deadline, default 5000 ms per call (configurable per
  module via deploy capabilities).
- Memory: store limiter, default 256 MiB (configurable per module).
- Capabilities (deploy-time): `allowed_tables` (empty = all), `read_only`,
  `max_memory_mb`, `timeout_ms`. Violations return `-2` from table ops.
- WASI: modules may import `wasi_snapshot_preview1`. No filesystem
  preopens, no env, no args, no sockets; stdout/stderr are discarded unless
  the server runs with WASM debug enabled. All other imports are rejected at
  deploy.
