// =============================================================================
// OriginDB host imports (WASM ABI v1, module "env").
// See docs/WASM_ABI.md for the normative contract.
//
// All pointers are byte offsets into this module's exported linear memory
// (usize == u32 on wasm32). `table`, `topic` and message pointers are
// NUL-terminated UTF-8; keys/values/payloads are pointer+length.
// =============================================================================

// ---- tables -----------------------------------------------------------------

// 1 = found (result written to a buffer allocated via our origindb_alloc;
// offset stored at *outPtr, length at *outLen), 0 = not found, <0 = error.
@external("env", "host_table_read")
export declare function host_table_read(
  table: usize, key: usize, keyLen: i32, outPtr: usize, outLen: usize): i32;

// Stages an upsert; value must be a UTF-8 JSON object. 0 = ok.
@external("env", "host_table_write")
export declare function host_table_write(
  table: usize, key: usize, keyLen: i32, value: usize, valueLen: i32): i32;

// Stages a delete. 0 = ok.
@external("env", "host_table_delete")
export declare function host_table_delete(
  table: usize, key: usize, keyLen: i32): i32;

// Prefix scan; result JSON array written via origindb_alloc. Returns row
// count or <0. limit <= 0 -> 1000.
@external("env", "host_table_scan")
export declare function host_table_scan(
  table: usize, prefix: usize, prefixLen: i32, limit: i32,
  outPtr: usize, outLen: usize): i32;

// ---- events -----------------------------------------------------------------

// Stages a custom changefeed event (operation "EVENT").
@external("env", "host_emit_event")
export declare function host_emit_event(
  topic: usize, key: usize, keyLen: i32, payload: usize, payloadLen: i32): i32;

// ---- utilities ----------------------------------------------------------------

// Wall clock (ms), fixed for the duration of the current call.
@external("env", "host_now_ms")
export declare function host_now_ms(): i64;

// Unique id: (now_ms << 20) | counter.
@external("env", "host_generate_id")
export declare function host_generate_id(): i64;

// level 0..4 = trace/debug/info/warn/error; msg NUL-terminated (max 4 KiB).
@external("env", "host_log")
export declare function host_log(level: i32, msg: usize): void;

// Records the message and traps; the call fails and staged work is discarded.
@external("env", "host_abort")
export declare function host_abort(msg: usize): void;

// ---- memory / results ---------------------------------------------------------

// Host-managed scratch arena inside guest memory; valid until the call returns.
@external("env", "host_alloc")
export declare function host_alloc(size: i32): usize;

// No-op (arena reclaimed with the instance).
@external("env", "host_free")
export declare function host_free(ptr: usize): void;

// Copies the call's result payload to the host (max 16 MiB, last call wins).
@external("env", "host_set_result")
export declare function host_set_result(ptr: usize, len: i32): void;
