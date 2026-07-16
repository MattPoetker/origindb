# WASM Module System

InstantDB runs user-supplied WebAssembly modules inside the database server
on **wasmtime** (LTS C API). Modules implement *reducers* — atomic,
transactional functions that read and write tables, emit changefeed events,
and can filter or transform real-time subscriptions.

The normative contract between the server and a module is
[docs/WASM_ABI.md](docs/WASM_ABI.md) (ABI v1). This document is the
practical guide: how to build, deploy, call, and troubleshoot modules.

## Overview

- **Single dispatch entry point**: every reducer and lifecycle hook goes
  through one export, `instantdb_invoke(name, args)`. Args are a UTF-8 JSON
  array; results come back through `host_set_result`.
- **Real sandboxing**: epoch-based CPU deadlines (default 5000 ms/call) and
  a per-module memory limiter (default 256 MiB), both configurable per
  module at deploy time.
- **Deploy-time capabilities**: `allowed_tables`, `read_only`,
  `max_memory_mb`, `timeout_ms` (`ModuleCapabilities` in
  `proto/instantdb.proto`). Capability violations return error `-2` from
  table operations.
- **Transactional writes**: each call runs against a staged-write overlay
  that commits atomically only when the call returns a non-negative status
  without trapping. Changefeed events are emitted by the storage commit.
- **Persistence**: deployed modules survive server restarts.

## Writing a module

Don't implement the ABI by hand — use an SDK:

| SDK | Status | Docs |
|---|---|---|
| **AssemblyScript** (`sdk/typescript/`) | Verified end-to-end; recommended | [sdk/typescript/README.md](sdk/typescript/README.md) |
| **C#** (`sdk/csharp/`) | Works only with a .NET 8 `wasi-experimental` workload build that supports `[UnmanagedCallersOnly]` exports — support is incomplete upstream; see the SDK README | [sdk/csharp/README.md](sdk/csharp/README.md) |

### AssemblyScript example

```ts
import {
  JsonValue, registerReducer, registerFilter, setModuleInfo, declareTable,
  writeTable, generateId, nowMs,
} from "../../assembly/index";

export {
  instantdb_alloc, instantdb_free, instantdb_describe, instantdb_invoke,
  __instantdb_abort,
} from "../../assembly/index";

setModuleInfo("todo", "1.0.0");
declareTable("todos");

registerReducer("addTodo", (args: Array<JsonValue>): JsonValue | null => {
  const text = args.length > 0 ? args[0].asString() : "";
  const id = generateId().toString();
  writeTable("todos", id, JsonValue.newObject()
    .setString("id", id)
    .setString("text", text)
    .setBool("done", false)
    .setNumber("created_at", <f64>nowMs())
    .toString());
  return JsonValue.newObject().setString("id", id);
}, ["text"]);

registerFilter("onlyPending", (ev: JsonValue): bool =>
  ev.getString("operation") == "INSERT");
```

The complete buildable example is `sdk/typescript/examples/todo/`.

### C# example

```csharp
using System.Runtime.CompilerServices;
using System.Text.Json;
using InstantDB;

public static class Program
{
    public static void Main() { }   // WASI entry point; may stay empty

    [ModuleInitializer]
    internal static void Init()
    {
        Reducers.SetModuleInfo("user_service", "1.0.0");
        Reducers.DeclareTable("users");

        Reducers.Register("CreateUser", args =>
        {
            string name = args[0].GetString()!;
            long id = Host.GenerateId();
            Db.Write("users", id.ToString(), JsonSerializer.Serialize(new
            {
                id = id.ToString(),
                name,
                created_at = Host.NowMs(),
            }));
            return new { id = id.ToString() };
        }, "name");

        Reducers.RegisterFilter("OnlyInserts",
            ev => ev.GetProperty("operation").GetString() == "INSERT");
    }
}
```

The complete buildable example is `examples/csharp/UserService/`.

### Module requirements (summary — see the ABI doc)

- Core WebAssembly module. WASI preview 1 imports are allowed; WASI
  preview 2 *components* are rejected at deploy, as is any import outside
  `env` / `wasi_snapshot_preview1`.
- No filesystem, environment, args, or sockets inside modules.
- Determinism: use `host_now_ms` (fixed per call) and `host_generate_id`
  instead of wall-clock/random APIs.
- Module linear memory does **not** survive traps, timeouts, or redeploys —
  keep durable state in tables.

## Deploy lifecycle

### Option 1: `instantdb publish` (build + deploy)

From a module project directory, `instantdb publish` detects the project
type, builds it, and deploys the resulting `.wasm` over gRPC (via the
bundled `instantdb_client`; `grpcurl` is not needed):

```bash
instantdb publish
instantdb publish --path=./my-module --server=prod.example.com:50051 --version=1.2.0
```

| Flag | Meaning |
|---|---|
| `--server HOST:PORT` | InstantDB gRPC endpoint (default `localhost:50051`) |
| `--path PATH` | Project path (default: current directory) |
| `--version VERSION` | Module version (default `1.0.0`) |

Supported project types:

- **C#** — a `.csproj` in the project directory; built with
  `dotnet publish --configuration Release` (requires .NET 8 SDK + the
  `wasi-experimental` workload). Output picked up from
  `bin/Release/net8.0/wasi-wasm/AppBundle/` (or `publish/`).
- **AssemblyScript** — an `asconfig.json` in the project directory; built
  with `npm run asbuild`. Output picked up from `build/module.wasm` (or
  `build/release.wasm`).

The module name is the project name (`.csproj` stem, or the directory name
for AssemblyScript projects).

### Option 2: `instantdb_client` (deploy a prebuilt .wasm)

```bash
instantdb_client [-s HOST:PORT] deploy NAME FILE.wasm [VERSION]
instantdb_client modules                       # list deployed modules (name, version, sha256)
instantdb_client call MODULE REDUCER '[json, args]'
instantdb_client undeploy NAME
```

Example:

```bash
instantdb_client deploy user_service ./UserService.wasm 1.0.0
instantdb_client call user_service CreateUser '["Alice", "alice@example.com"]'
instantdb_client modules
instantdb_client undeploy user_service
```

`call` arguments are a JSON array; booleans, integers, floats, and strings
map to the corresponding reducer argument types.

Both paths use the gRPC `WasmService` (`DeployModule`, `UndeployModule`,
`ListModules`, `ExecuteReducer` in `proto/instantdb.proto`), which you can
also call directly from your own client.

### What happens at deploy

1. Bytecode is validated (core module; imports checked against the ABI).
2. `instantdb_describe` (if exported) is called once to collect module
   metadata (name, version, reducers, tables).
3. The module is instantiated; `_initialize` (or the start section) runs,
   and the `__init` lifecycle hook is invoked if registered.
4. Bytecode and a manifest are persisted (see below).

## Capabilities and limits

Capabilities are set at deploy time via the `capabilities` field of
`DeployModuleRequest`:

```protobuf
message ModuleCapabilities {
  repeated string allowed_tables = 1;  // empty = all tables
  bool read_only = 2;
  uint32 max_memory_mb = 3;            // 0 = default (256 MiB)
  uint32 timeout_ms = 4;               // 0 = default (5000 ms)
}
```

- **`allowed_tables`** — tables the module may read/write; empty means all.
- **`read_only`** — write and delete operations fail.
- Violations return error `-2` (permission denied) from the table
  operations; the SDKs surface this to your reducer.
- **CPU**: epoch-based deadline per call. A call that exceeds its timeout
  traps; nothing is committed.
- **Memory**: wasmtime store limiter caps linear memory growth.

After a trap or timeout the module instance is discarded and lazily
re-instantiated on the next call (fresh memory; `_initialize` runs again).
Calls are serialized per module; different modules execute concurrently.

## Persistence

Deployed modules are stored under `<data_dir>/modules/<name>/`:

- the raw bytecode
- `manifest.json` — `name`, `version`, `sha256`, `deployed_at_ms`

On boot the server restores every persisted module (log line:
`Restored persisted module: <name>`), verifying the bytecode against the
manifest's sha256; corrupt or mismatched entries are skipped with a
warning. `undeploy` removes the module's files. Table data written by
modules is recovered separately through the WAL like any other write.

## Transactions and events

Per the ABI transaction model:

- Reads (`host_table_read` / `host_table_scan`) see the call's own staged
  writes first, then committed data.
- On return status `>= 0` with no trap, staged writes are applied
  atomically through one storage transaction (WAL-logged). The storage
  commit emits one changefeed event per write — exactly once.
- Custom events staged with `host_emit_event` (operation `"EVENT"`) are
  published only after a successful commit.
- On trap, `host_abort`, timeout, memory-limit violation, or negative
  status, nothing is applied.
- Tables written by modules are auto-created on first commit (column types
  inferred from the first staged row).

## Lifecycle hooks

Lifecycle hooks are ordinary reducer registrations under reserved names
(all prefixed `__`). Unregistered reserved names return `-404`, which the
host treats as a harmless no-op:

| Name | When | Args |
|---|---|---|
| `__init` | after deploy/instantiation | — |
| `__client_connected` | WebSocket client connects | `[connection_id]` |
| `__client_disconnected` | WebSocket client disconnects | `[connection_id]` |
| `__get_initial_data` | WASM subscription created with `include_initial_data` | `[where_clause]` |

## Subscriptions (filter / transform)

WebSocket clients can create module-backed subscriptions. The module's
filter and transform functions are invoked through `instantdb_invoke`
under the names the subscription registered:

```javascript
const ws = new WebSocket('ws://localhost:8080');

ws.send(JSON.stringify({
  type: 'wasm_subscribe',
  module_name: 'todo',
  filter_function: 'onlyPending',      // optional
  transform_function: 'addMetadata',   // optional
  tables: ['todos'],                   // optional table filter
  include_initial_data: true,          // optional; calls __get_initial_data
  start_offset: 0                      // optional
}));

// Server replies:
// {"type": "wasm_subscription_created", "subscription_id": "...", ...}
// Matching events arrive as:
// {"type": "wasm_subscription_event", "subscription_id": "...", "data": ...}
```

- **Filter functions** receive one argument — the event as a JSON object
  `{table, operation, offset, transaction_id, key, new_value, old_value}` —
  and return ABI status `1` (include) or `0` (exclude). The SDKs wrap this
  as a boolean-returning function (`registerFilter` / `RegisterFilter`).
- **Transform functions** receive the same event object and return the
  transformed payload via `host_set_result`.
- Plain SQL subscriptions (`sql_subscribe`, with per-event WHERE
  evaluation) don't involve modules at all — see `QA_TESTING_GUIDE.md`.

## Debugging

- `host_log` output from modules goes to the server log
  (`instantdb server -l debug` for debug-level detail).
- `host_abort` messages are recorded and included in the reducer error.
- Module stdout/stderr are discarded unless the server runs with WASM
  debug enabled.
- `scripts/e2e_verify.sh` exercises the full pipeline (build → deploy →
  execute → SQL verify → filtered WebSocket delivery → restart persistence
  → undeploy) and is the quickest way to confirm a working setup.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Deploy rejected: unsupported import | Module imports something outside `env` / `wasi_snapshot_preview1` (e.g. AssemblyScript's default `env.abort` — use the SDK's abort handler wiring in `asconfig.json`). |
| Deploy rejected: not a core module | The toolchain emitted a WASI preview 2 *component* (e.g. .NET 9+ / componentize-dotnet). Pin .NET 8 + `wasi-experimental`, or use AssemblyScript. |
| Reducer returns `-404` | No handler registered under that name (check spelling; registration must happen in `_initialize` / start section / `[ModuleInitializer]`). C# only: your `wasi-experimental` workload build may not support `[UnmanagedCallersOnly]` exports at all — verify `instantdb_invoke` is exported (`wasm-tools print module.wasm | grep export`). |
| Reducer returns `-2` | Capability violation: table not in `allowed_tables`, or a write in a `read_only` module. |
| Reducer returns `-3` | Invalid argument (module-side validation). |
| Call fails with timeout | Exceeded the per-module `timeout_ms` (default 5000 ms). The instance is discarded and recreated on the next call. |
| Module state "disappears" | Linear memory does not survive traps/timeouts/redeploys/restarts. Keep durable state in tables. |
| Module missing after restart | Check the boot log for `ModuleStore: skipping ...` warnings (missing/corrupt manifest, sha256 mismatch). |

## Current limitations

- No cross-module calls, no async I/O, no network access from modules.
- No instance pooling: one long-lived instance per module, calls
  serialized per module.
- No hot reload; redeploying replaces the module (fresh instance).
- Reducer results are surfaced through `host_set_result` payloads; there
  is no typed result schema beyond JSON.
- No Rust/Go/C++ SDKs yet (the ABI is documented, so any language that can
  produce a conforming core module works in principle).
