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
cd examples/collab && npm install && node server.js --ws-port 8787 &

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
| C++ | [`sdk/cpp/`](sdk/cpp/) | header available; not yet aligned to ABI v1 |

Scaffold a project with `origindb init` (templates: `typescript`, `csharp`,
`unity`, `nodejs`) and ship it with `origindb publish`. Guides:
[`WASM_MODULES.md`](WASM_MODULES.md), [`CLI_GUIDE.md`](CLI_GUIDE.md),
[`docs/WASM_ABI.md`](docs/WASM_ABI.md).

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
