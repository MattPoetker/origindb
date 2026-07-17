// ============================================================================
// Smoke test for the AssemblyScript SDK + todo example.
//
// Emulates the OriginDB host side of the WASM ABI v1 (docs/WASM_ABI.md):
// writes invoke name/args into guest memory via the module's own
// origindb_alloc, provides the "env" imports, and checks describe/invoke
// semantics (status 0, filter 1/0, -404 for unknown names, results via
// host_set_result).
//
// Run:  npm run test    (builds build/module.wasm first)
// ============================================================================

import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const wasmPath = new URL("../build/module.wasm", import.meta.url);

let exp; // module exports
const dec = new TextDecoder();
const enc = new TextEncoder();

const tables = new Map(); // "table\0key" -> value JSON string
const logs = [];
const events = [];
let lastResult = null;
let idCounter = 0n;
const NOW = 1721000000000n;

const mem = () => new Uint8Array(exp.memory.buffer);
const dv = () => new DataView(exp.memory.buffer);

function readStr(ptr, len) {
  return dec.decode(mem().subarray(ptr, ptr + len));
}

function readCStr(ptr) {
  const bytes = mem();
  let end = ptr;
  while (bytes[end] !== 0) end++;
  return dec.decode(bytes.subarray(ptr, end));
}

// Host -> guest buffer, allocated with the guest's own origindb_alloc.
function writeGuestBuf(str) {
  const bytes = enc.encode(str);
  const ptr = exp.origindb_alloc(bytes.length);
  assert.ok(ptr !== 0, "origindb_alloc returned 0");
  mem().set(bytes, ptr);
  return [ptr, bytes.length];
}

const env = {
  host_table_read(table, key, keyLen, outPtr, outLen) {
    const k = `${readCStr(table)}\0${readStr(key, keyLen)}`;
    if (!tables.has(k)) return 0;
    const [p, l] = writeGuestBuf(tables.get(k));
    dv().setUint32(outPtr, p, true);
    dv().setUint32(outLen, l, true);
    return 1;
  },
  host_table_write(table, key, keyLen, value, valueLen) {
    tables.set(`${readCStr(table)}\0${readStr(key, keyLen)}`, readStr(value, valueLen));
    return 0;
  },
  host_table_delete(table, key, keyLen) {
    tables.delete(`${readCStr(table)}\0${readStr(key, keyLen)}`);
    return 0;
  },
  host_table_scan(table, prefix, prefixLen, limit, outPtr, outLen) {
    const t = readCStr(table);
    const pfx = readStr(prefix, prefixLen);
    const max = limit <= 0 ? 1000 : limit;
    const rows = [];
    for (const [k, v] of [...tables.entries()].sort()) {
      const [tn, key] = k.split("\0");
      if (tn === t && key.startsWith(pfx)) {
        rows.push({ key, value: JSON.parse(v) });
        if (rows.length >= max) break;
      }
    }
    const [p, l] = writeGuestBuf(JSON.stringify(rows));
    dv().setUint32(outPtr, p, true);
    dv().setUint32(outLen, l, true);
    return rows.length;
  },
  host_emit_event(topic, key, keyLen, payload, payloadLen) {
    events.push({
      topic: readCStr(topic),
      key: readStr(key, keyLen),
      payload: readStr(payload, payloadLen),
    });
    return 0;
  },
  host_now_ms: () => NOW,
  host_generate_id: () => (NOW << 20n) | ++idCounter,
  host_log(level, msg) {
    logs.push([level, readCStr(msg)]);
  },
  host_abort(msg) {
    throw new Error(`host_abort: ${readCStr(msg)}`);
  },
  host_alloc: (size) => exp.origindb_alloc(size),
  host_free: () => {},
  host_set_result(ptr, len) {
    lastResult = readStr(ptr, len);
  },
};

const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), { env });
exp = instance.exports;

for (const name of ["memory", "origindb_alloc", "origindb_invoke", "origindb_free", "origindb_describe"]) {
  assert.ok(name in exp, `missing export: ${name}`);
}

function invoke(name, args = []) {
  const [np, nl] = writeGuestBuf(name);
  const [ap, al] = writeGuestBuf(JSON.stringify(args));
  lastResult = null;
  return exp.origindb_invoke(np, nl, ap, al);
}

// ---- describe ----------------------------------------------------------------
const packed = exp.origindb_describe();
const dPtr = Number(packed >> 32n);
const dLen = Number(packed & 0xffffffffn);
const meta = JSON.parse(readStr(dPtr, dLen));
assert.equal(meta.name, "todo");
assert.equal(meta.version, "1.0.0");
assert.deepEqual(meta.tables, ["todos"]);
const reducerNames = meta.reducers.map((r) => r.name);
for (const n of ["addTodo", "listTodos", "completeTodo", "__init", "onlyPending"]) {
  assert.ok(reducerNames.includes(n), `describe missing reducer ${n}`);
}
assert.deepEqual(meta.reducers.find((r) => r.name === "addTodo").params, ["text"]);

// ---- lifecycle + unknown names -------------------------------------------------
assert.equal(invoke("__init"), 0);
assert.equal(invoke("__client_connected", ["conn-1"]), -404, "unregistered lifecycle -> -404");
assert.equal(invoke("noSuchReducer", [1, 2]), -404, "unknown reducer -> -404");

// ---- addTodo -------------------------------------------------------------------
assert.equal(invoke("addTodo", ["Buy milk"]), 0);
const created = JSON.parse(lastResult);
assert.ok(typeof created.id === "string" && created.id.length > 0);
const row = JSON.parse(tables.get(`todos\0${created.id}`));
assert.equal(row.text, "Buy milk");
assert.equal(row.done, false);
assert.equal(BigInt(row.created_at), NOW);

assert.equal(invoke("addTodo", ["Write ABI tests"]), 0);
const second = JSON.parse(lastResult).id;

// ---- validation failure traps via host_abort ------------------------------------
assert.throws(() => invoke("addTodo", [""]), /host_abort: addTodo/);

// ---- listTodos -------------------------------------------------------------------
assert.equal(invoke("listTodos"), 0);
const todos = JSON.parse(lastResult);
assert.equal(todos.length, 2);
assert.ok(todos.some((t) => t.text === "Buy milk"));

// ---- completeTodo ----------------------------------------------------------------
assert.equal(invoke("completeTodo", [created.id]), 0);
assert.deepEqual(JSON.parse(lastResult), { id: created.id, done: true });
assert.equal(JSON.parse(tables.get(`todos\0${created.id}`)).done, true);

// ---- filter: 1 = include, 0 = exclude ---------------------------------------------
// The host passes the event as a JSON *string* element in the args array.
const pendingEvent = JSON.stringify({
  table: "todos",
  operation: "UPDATE",
  offset: 7,
  transaction_id: 42,
  key: second,
  new_value: { id: second, text: "Write ABI tests", done: false },
  old_value: null,
});
const doneEvent = JSON.stringify({
  table: "todos",
  operation: "UPDATE",
  offset: 8,
  transaction_id: 43,
  key: created.id,
  new_value: { id: created.id, text: "Buy milk", done: true },
  old_value: null,
});
assert.equal(invoke("onlyPending", [pendingEvent]), 1);
assert.equal(invoke("onlyPending", [doneEvent]), 0);

console.log("smoke test passed:");
console.log(`  describe: ${dLen} bytes, ${meta.reducers.length} reducers`);
console.log(`  tables:   ${tables.size} rows staged`);
console.log(`  logs:     ${logs.length} host_log calls`);
