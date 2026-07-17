# OriginDB 🚀

A realtime WebSocket database with **user-programmable WASM modules**. Clients
subscribe to a *filtered subset* of authoritative server changes — no client
ever has to listen to the whole firehose — and application logic runs *inside*
the database as sandboxed WebAssembly reducers.

```
   browser / app                      OriginDB server                      disk
┌───────────────┐   gRPC call    ┌──────────────────────┐
│ call reducer ─┼───────────────▶│  WASM engine         │
│               │                │  (wasmtime sandbox)  │
│               │                │   your .wasm module  │
│               │                │        │ staged      │
│               │                │        ▼ writes      │
│               │                │  Storage engine ─────┼──▶ WAL (crash recovery)
│               │                │        │ commit      │
│               │                │        ▼             │
│               │                │  Changefeed engine   │
│               │   websocket    │        │             │
│ live events ◀─┼────────────────┼── per-client WHERE   │
└───────────────┘   (filtered)   │    filtering         │
                                 └──────────────────────┘
```

## ⚡ Try it: the NIGHTBOARD demo

A shared realtime wall — live cursors, pinned notes, and an activity tape for
every connected device. Open it on your laptop and your phone at once; every
cursor twitch travels through a WASM reducer, a transactional commit, the WAL,
and a filtered changefeed before landing on the other screen.

```bash
# 1. build (see Quick Start below), then:
build_new/origindb_server -d ./nightboard_data -p 8787 -g 50051 &
build_new/origindb_client deploy collab sdk/typescript/build/collab.wasm 1.0.0

# 2. start the web frontend
cd examples/collab && npm install && npm run build && node server.js --ws-port 8787 &

# 3. open http://<your-lan-ip>:9090 on every device
```

- **move** your pointer/finger → your cursor appears on every screen
- **double-click** (desktop) or **long-press** (mobile) → pin a note
- the **TAPE** panel streams everyone's activity live

Everything on the board is four database tables (`cursors`, `notes`,
`presence`, `activity`) written *only* by the reducers of one AssemblyScript
module ([`sdk/typescript/examples/collab/index.ts`](sdk/typescript/examples/collab/index.ts)).
The web page holds no state of its own — it is a pure subscriber. The tiny
node server ([`examples/collab/server.js`](examples/collab/server.js)) exists
only because browsers can't speak raw gRPC; realtime data flows straight from
OriginDB's websocket to the page.

## 🏗️ How the architecture works

### 1. Writes go through WASM reducers

Clients don't mutate tables directly. They call named **reducers** —
functions exported by a WASM module you deploy into the server:

```
origindb_client call collab addNote '["Ada", "#ffc24b", 0.31, 0.42, "hello"]'
```

The engine (wasmtime, LTS C API) runs the reducer in a sandbox:

- **Staged writes** — `host_table_write`/`host_table_delete` accumulate in an
  overlay; the module reads its own pending writes (`host_table_read`,
  `host_table_scan` merge overlay + committed state).
- **Atomic commit** — if the reducer returns success, the overlay is applied
  through one storage transaction and logged to the WAL. A trap, `host_abort`,
  timeout, or error status discards everything.
- **Determinism aids** — `host_now_ms` is frozen per call, `host_generate_id`
  is monotonic.
- **Limits** — epoch-based CPU deadlines (default 5 s), a memory limiter
  (default 256 MiB), and per-module deploy-time capabilities: allowed tables,
  read-only mode, custom memory/time budgets. WASI is available but sees no
  filesystem, environment, or network.

The full guest ABI (exports, imports, memory ownership, error codes) is
specified in [`docs/WASM_ABI.md`](docs/WASM_ABI.md). Modules persist under
`<data_dir>/modules/` (sha256-verified) and are restored on boot.

### 2. Commits feed the changefeed

Every committed write emits exactly one `ChangefeedEvent` — table, operation,
key, `new_value`, and `old_value` (for UPDATE/DELETE) — assigned a global
monotonic offset and delivered in order by the changefeed engine. Reducers can
also stage custom events (`host_emit_event`) that publish only after commit.

### 3. Subscribers receive a filtered subset

Clients subscribe over the websocket with SQL:

```json
{ "type": "sql_subscribe", "sql": "SELECT * FROM todos WHERE done = FALSE" }
```

- The server answers with an **initial state snapshot**, then streams
  `sql_changefeed_event` messages.
- **WHERE clauses are evaluated server-side per event** (comparisons,
  `AND`/`OR`/`NOT`, parentheses, `LIKE`, `IS [NOT] NULL`). UPDATE events match
  if the old *or* new row matches, so rows entering and leaving the filtered
  set both notify. Column projection (`SELECT a, b`) is applied per client.
- Invalid predicates reject the subscription with an error — nothing silently
  falls back to match-all.
- Modules can go further: `wasm_subscribe` routes events through a module's
  own filter/transform functions for fully programmable streams.

### 4. Storage: in-memory tables + write-ahead log

Tables live in memory (hash maps under shared mutexes); durability comes from
a JSON-lines WAL that is fully replayed on startup. Transactions are
write-buffered and applied atomically at commit under a global lock — simple,
correct for a single node. Raft-based replication is a future goal.

## 🚀 Quick Start

Prereqs: CMake ≥ 3.20, a C++20 compiler, OpenSSL, and (optional, for gRPC)
protobuf + grpc via Homebrew/pkg-config. The build fetches wasmtime, spdlog,
fmt, nlohmann-json, and googletest automatically.

```bash
cmake -B build_new -S .
cmake --build build_new -j8

build_new/unit_tests           # 53 tests: engine, predicate evaluator, module store
./scripts/e2e_verify.sh        # full pipeline: deploy → execute → SQL → filtered ws → restart persistence
```

Run a server and poke it:

```bash
build_new/origindb_server -d ./data -p 8787 -g 50051

build_new/origindb_client status
build_new/origindb_client deploy todo sdk/typescript/build/module.wasm 1.0.0
build_new/origindb_client call todo addTodo '["ship it"]'
build_new/origindb_client call todo listTodos
build_new/origindb_client exec "SELECT * FROM todos"
```

## 🛠️ Writing modules

| Language | SDK | Status |
|---|---|---|
| **TypeScript (AssemblyScript)** | [`sdk/typescript/`](sdk/typescript/) | ✅ verified end-to-end (todo + collab examples) |
| **C#** | [`sdk/csharp/`](sdk/csharp/) | ⚠️ compiles against the ABI; .NET 8 `wasi-experimental` export support is flaky upstream — see the SDK README |
| C++ / anything that emits WASM | [`docs/WASM_ABI.md`](docs/WASM_ABI.md) | the ABI is language-agnostic; `sdk/cpp/` is not yet aligned to ABI v1 |

A module is a WebAssembly file that exports `origindb_invoke` (a single
dispatch entry point), `origindb_alloc` (guest allocator), and `memory`.
Both SDKs implement that plumbing; you just register **reducers** — named
functions that receive JSON args, read/write tables through the host API,
and either commit (return success) or roll back (throw/abort). Reserved
names hook the module lifecycle:

| Hook | When it runs |
|---|---|
| `__init` | after every deploy/instantiation |
| `__migrate(old_version, new_version)` | on hot-swap over a live module — table writes commit like any reducer, so schema migrations are plain table ops; failure aborts the swap and the old version keeps serving |
| `__client_connected` / `__client_disconnected` | websocket client lifecycle |
| any other name | callable via `ExecuteReducer` / `origindb_client call` |

Deploys with a version lower than the running one are rejected. Per-module
sandbox limits (allowed tables, read-only, memory/CPU budgets) are set at
deploy time via `ModuleCapabilities`.

### TypeScript (AssemblyScript) — recommended

One entry file; top-level statements run at instantiation, so registration
lives there. From the todo example
([`sdk/typescript/examples/todo/index.ts`](sdk/typescript/examples/todo/index.ts)):

```ts
import {
  JsonValue, registerReducer, registerFilter, setModuleInfo, declareTable,
  readTable, writeTable, scanTable, generateId, nowMs, logInfo, abortCall,
} from "../../assembly/index";

// REQUIRED: re-export the ABI surface from your entry file.
export {
  origindb_alloc, origindb_free, origindb_describe, origindb_invoke,
  __origindb_abort,
} from "../../assembly/index";

setModuleInfo("todo", "1.0.0");
declareTable("todos");

registerReducer("addTodo", (args: Array<JsonValue>): JsonValue | null => {
  const text = args.length > 0 ? args[0].asString() : "";
  if (text.length == 0) abortCall("addTodo: text required");   // rolls back

  const id = generateId().toString();
  writeTable("todos", id, JsonValue.newObject()
    .setString("id", id)
    .setString("text", text)
    .setBool("done", false)
    .setNumber("created_at", <f64>nowMs())
    .toString());
  return JsonValue.newObject().setString("id", id);   // reducer result payload
}, ["text"]);

// Subscription filter: return true to forward the changefeed event.
// Storage events wrap the row as {"key": ..., "columns": {...}}.
registerFilter("onlyPending", (event: JsonValue): bool => {
  const nv = event.get("new_value").get("columns");
  return !nv.getBool("done", false);
});
```

Build and ship:

```bash
cd sdk/typescript && npm install
npm run asbuild                                   # → build/module.wasm (~20 KB)
origindb_client deploy todo build/module.wasm 1.0.0
origindb_client call todo addTodo '["ship it"]'
```

The collab module behind NIGHTBOARD
([`sdk/typescript/examples/collab/index.ts`](sdk/typescript/examples/collab/index.ts))
is the full-featured reference: read-modify-write reducers (`moveNote`,
`editNote`), a chat table, and a real `__migrate` hook that seeded the chat
table when v2 hot-swapped over v1 on a live server.

### C#

The SDK is one source file (`sdk/csharp/OriginDB.cs`) compiled into your
project. Registration goes in a `[ModuleInitializer]` — guaranteed to run
before any export executes. From
[`examples/csharp/UserService/Program.cs`](examples/csharp/UserService/Program.cs):

```csharp
using System.Runtime.CompilerServices;
using System.Text.Json;
using OriginDB;

public static class Program
{
    public static void Main() { }   // WASI entry point; leave empty

    [ModuleInitializer]
    internal static void Init()
    {
        Reducers.SetModuleInfo("user_service", "1.0.0");
        Reducers.DeclareTable("users");
        Reducers.Register("CreateUser", CreateUser, "name", "email");
        Reducers.Register("GetUsers", GetUsers);
        Reducers.RegisterFilter("OnlyUserInserts", OnlyUserInserts);
    }

    private static object? CreateUser(JsonElement[] args)
    {
        string name = args[0].GetString()!;
        string email = args[1].GetString()!;
        if (!email.Contains('@'))
            throw new ReducerException($"'{email}' is not a valid email", -3);

        long id = Host.GenerateId();
        Db.Write("users", id.ToString(), JsonSerializer.Serialize(new
        {
            id = id.ToString(), name, email, created_at = Host.NowMs(),
        }));
        return new { id = id.ToString() };
    }
}
```

Build and ship (**pin .NET 8** — the server runs core WASI preview 1
modules; .NET 9's componentize-dotnet emits preview 2 components it cannot
run):

```bash
dotnet workload install wasi-experimental          # once, .NET 8.0.4xx
dotnet publish -c Release
origindb publish --path examples/csharp/UserService
# or: origindb_client deploy user_service bin/Release/net8.0/wasi-wasm/AppBundle/UserService.wasm 1.0.0
```

Caveat: .NET 8's `wasi-experimental` has incomplete `[UnmanagedCallersOnly]`
export support upstream. If `wasm-tools print *.wasm | grep export` doesn't
show `origindb_invoke`, your workload build predates it — use AssemblyScript
until then. Details in [`sdk/csharp/README.md`](sdk/csharp/README.md).

### Any other language (C++, Rust, Zig, hand-written WAT…)

The ABI is deliberately small: export `memory`, `origindb_alloc`, and
`origindb_invoke(name_ptr, name_len, args_ptr, args_len) -> i32`; import
whatever you need from `env` (`host_table_read/write/delete/scan`,
`host_emit_event`, `host_set_result`, `host_log`, `host_abort`, …). Args
arrive as a UTF-8 JSON array; return `0` to commit, negative to roll back,
`-404` for "no such reducer"; payloads go out through `host_set_result`.
The complete contract — memory ownership, error codes, WASI limits — is in
[`docs/WASM_ABI.md`](docs/WASM_ABI.md), and
[`tests/wasm/fixtures/test_module.wat`](tests/wasm/fixtures/test_module.wat)
is a ~100-line hand-written module that exercises all of it (the e2e suite
deploys it). `sdk/cpp/` predates ABI v1 and needs realignment before use.

### Scaffolding and day-to-day flow

```bash
origindb init my-module          # templates: typescript, csharp, unity, nodejs
origindb publish                 # build + deploy (detects .csproj / asconfig.json)
origindb_client modules          # list deployed modules (version, sha256)
origindb_client call m Reducer '["arg1", 2]'
```

Redeploying an existing name hot-swaps it: in-flight calls finish on the
old version, new calls hit the new one, `__migrate` runs in between, and
nothing is lost because durable state lives in tables — module linear
memory is a cache that resets on swap. More depth:
[`WASM_MODULES.md`](WASM_MODULES.md), [`CLI_GUIDE.md`](CLI_GUIDE.md).

## 📦 Repository layout

```
src/storage/       in-memory tables, WAL, transactions
src/changefeed/    event stream, SQL subscriptions, WHERE predicate evaluator
src/wasm/          wasmtime engine, host ABI, module store (persistence)
src/websocket/     RFC6455 server, JSON subscription protocol
src/grpc/          SQLService + WasmService (deploy/execute)
src/sql/           regex-based SQL layer (prototype)
sdk/               module SDKs (typescript, csharp, cpp) + examples
examples/collab/   NIGHTBOARD realtime demo webapp
tests/             gtest suites + WAT fixtures; tests/performance harness
scripts/           e2e_verify.sh and friends
docs/              WASM_ABI.md (normative), architecture & API guides
```

## ⚠️ Honest limitations

- **SQL is a prototype**: regex parser; `SELECT` has no WHERE/projection,
  `DELETE` is unimplemented, `CREATE TABLE` ignores column definitions.
  (Subscription WHERE filtering is real and separate.)
- **No authentication** on the websocket or gRPC endpoints — anyone who can
  reach the gRPC port can deploy modules. LAN/dev use only.
- **Single node**: no replication yet; WAL flush is not fsync'd.
- Cursor-grade write volume runs the full commit+WAL pipeline (~1 ms per
  reducer call) — fine for demos and moderate loads; an ephemeral table class
  is on the roadmap.
- Subscription delivery is best-effort (at-most-once) with a single delivery
  thread; backpressure handling is planned.

## 📄 License

MIT — see [LICENSE](LICENSE).
