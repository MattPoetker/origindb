# InstantDB TypeScript (AssemblyScript) SDK

Write InstantDB server-side WASM modules in **AssemblyScript** — a strict,
TypeScript-syntax language that compiles directly to small core WASI-free
wasm modules (the todo example builds to ~20 KB). This is the recommended
module SDK today: it produces exactly the core preview-1-style modules the
server's wasmtime runtime expects.

The SDK implements the [InstantDB WASM ABI v1](../../docs/WASM_ABI.md):

- `assembly/env.ts` — typed `@external("env", ...)` declarations for every
  host import (`host_table_read`, `host_table_write`, `host_table_delete`,
  `host_table_scan`, `host_emit_event`, `host_now_ms`, `host_generate_id`,
  `host_log`, `host_abort`, `host_alloc`, `host_free`, `host_set_result`)
- `assembly/index.ts` — the SDK: high-level wrappers, a reducer/filter
  registry, a minimal hand-rolled JSON type, and the four ABI exports
  (`instantdb_alloc`, `instantdb_free`, `instantdb_describe`,
  `instantdb_invoke`)

> **JSON**: the SDK ships a small hand-rolled `JsonValue`
> (parse/build/stringify, ~400 lines) instead of depending on `json-as`.
> Rationale: zero dependencies, no decorator/transform integration to keep in
> sync with compiler releases, and predictable behavior under the `minimal`
> runtime. If you prefer `json-as` for your own module code you can still use
> it — the ABI surface only needs strings.

## Quick start

```bash
cd sdk/typescript
npm install
npm run asbuild          # → build/module.wasm  (todo example)
npm test                 # asbuild + node tests/smoke.mjs (host-side ABI harness)
```

Deploy `build/module.wasm` with `instantdb publish` (or the gRPC
`DeployModule` call).

## Writing a module

A module is one entry `.ts` file (see `examples/todo/index.ts`):

```ts
import {
  JsonValue, registerReducer, registerFilter, setModuleInfo, declareTable,
  readTable, writeTable, scanTable, generateId, nowMs, logInfo, abortCall,
} from "../../assembly/index";

// REQUIRED: re-export the ABI surface from your entry file.
export {
  instantdb_alloc, instantdb_free, instantdb_describe, instantdb_invoke,
  __instantdb_abort,
} from "../../assembly/index";

// Top-level statements run in the wasm start section (at instantiation),
// before the host calls anything — register everything here. Do not touch
// tables at top level; use an "__init" reducer.
setModuleInfo("todo", "1.0.0");
declareTable("todos");

registerReducer("addTodo", (args: Array<JsonValue>): JsonValue | null => {
  const text = args.length > 0 ? args[0].asString() : "";
  if (text.length == 0) abortCall("addTodo: text required");
  const id = generateId().toString();
  writeTable("todos", id, JsonValue.newObject()
    .setString("id", id)
    .setString("text", text)
    .setBool("done", false)
    .setNumber("created_at", <f64>nowMs())
    .toString());
  return JsonValue.newObject().setString("id", id);   // → host_set_result, status 0
}, ["text"]);
```

Point `asconfig.json`'s `entries` at your file and run `npm run asbuild`.

### Registry

| Call | Purpose |
|---|---|
| `registerReducer(name, fn, params?)` | `fn: (args: Array<JsonValue>) => JsonValue \| null`. Args arrive as the invocation's JSON array. A non-null return value is serialized and returned via `host_set_result`; the call returns status `0`. Unregistered names return `-404` per the ABI. |
| `registerFilter(name, fn)` | `fn: (event: JsonValue) => bool` — return value maps to ABI status `1` (include) / `0` (exclude). The event arrives parsed: `{table, operation, offset, transaction_id, key, new_value, old_value}`. |
| `setModuleInfo(name, version)` / `declareTable(table)` | Metadata reported by `instantdb_describe`. |

Lifecycle hooks are plain registrations under the reserved names `__init`,
`__client_connected`, `__client_disconnected`, `__get_initial_data`.

### Host API

| API | Notes |
|---|---|
| `readTable(table, key): string \| null` | row JSON, or null if not found |
| `writeTable(table, key, jsonObject): bool` | stages an upsert |
| `deleteTable(table, key): bool` | stages a delete |
| `scanTable(table, prefix = "", limit = 0): string` | `[{"key","value"}]` JSON, `limit <= 0` → 1000 |
| `emitEvent(topic, key, payloadJson): bool` | custom changefeed event, published on commit |
| `nowMs(): i64` / `generateId(): i64` | deterministic time (fixed per call) / unique id |
| `log(level, msg)` + `logTrace..logError` | server-side logging |
| `abortCall(msg)` | records the message via `host_abort` and traps — the call fails and staged writes are discarded |
| `setResult(json)` | advanced; the dispatcher normally does this for you |
| `base64Encode/Decode`, `bytesArg`, `bytesValue` | binary args per the ABI: `{"$bytes": "<base64>"}` |

All writes/events are staged and commit atomically only if the call returns
a non-negative status without trapping. Module memory does not survive traps
or redeploys — keep durable state in tables.

## AssemblyScript is not TypeScript

AssemblyScript compiles a strict *subset* of TS syntax to wasm. The big ones:

- **No `any`, no union types** (except `T | null` for reference types).
  Everything is statically typed (`i32`, `i64`, `f64`, `bool`, `string`, ...).
- **No closures over local variables** — callbacks can be top-level
  functions or arrow functions that don't capture locals. Keep shared state
  in module-level variables.
- **No `try`/`catch`** — `throw` and failed assertions trap. The SDK routes
  them to `host_abort` (via the custom abort handler wired in
  `asconfig.json`), so the call fails cleanly with the message in the server
  log.
- No JS standard library beyond what AS provides (`Map`, `Array`, `string`,
  `Math`, typed arrays). No `JSON` global — use the SDK's `JsonValue`.
- `i64` values (ids, timestamps) are real 64-bit integers, not `number`.
  Convert explicitly (`generateId().toString()`, `<f64>nowMs()`).

## Configuration notes (`asconfig.json`)

- `runtime: "minimal"` — TLSF allocator + lightweight GC; no collection
  happens mid-call, so pointers passed to host calls stay valid.
- `exportRuntime: false` — the ABI needs only `instantdb_alloc`/`free`.
- `importMemory: false` — the module exports its own `memory` (required).
- `use: ["abort=assembly/index/__instantdb_abort"]` — replaces
  AssemblyScript's default `env.abort(msg, file, line, col)` import, which
  the server would reject, with the SDK's `host_abort`-backed handler. If
  your module lives outside this directory tree, adjust the module path but
  keep the setting.

Buffer ownership per the ABI: results of `host_table_read`/`host_table_scan`
are written into buffers the host obtains from this module's
`instantdb_alloc` (`heap.alloc`); the SDK frees them with `heap.free` —
never `host_free`.

## Layout

```
sdk/typescript/
├── assembly/
│   ├── env.ts            host import declarations (ABI "env" module)
│   └── index.ts          SDK + ABI exports + JsonValue
├── examples/todo/index.ts  addTodo/listTodos/completeTodo + onlyPending filter
├── tests/smoke.mjs       node harness emulating the host side of the ABI
├── asconfig.json         entries + release/debug targets → build/module.wasm
└── package.json          npm run asbuild / npm test
```
