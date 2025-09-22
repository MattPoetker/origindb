# InstantDB Implementation Progress

## 🚀 Current Status (IMPLEMENTED)

InstantDB now has a **working SpacetimeDB-like API** with automatic change detection and real-time streaming! The system successfully implements:

✅ **SpacetimeDB-like WASM API**
- Reducers receive `ReducerContext ctx` parameter
- Database access via `ctx.Db.table.Insert()`, `ctx.Db.GetTable<T>().Find()`
- Automatic changefeed event emission (no manual `Events.Emit()` needed)
- Dynamic table access: `ctx.Db.config`, `ctx.Db.users`, etc.

✅ **Complete WASM Runtime Integration**
- Wasmtime-based WASM engine with host API
- C++ and C# SDKs with type-safe database operations
- Resource sandboxing and instance pooling
- Module deployment and lifecycle management

✅ **Automatic Change Detection**
- Storage engine automatically emits changefeed events on all writes
- Database operations trigger real-time events to WebSocket clients
- No manual event emission required in user code

✅ **Real-time Streaming Infrastructure**
- WebSocket server with changefeed integration
- WASM subscription manager with filtering and transformation
- Live event delivery to connected clients

✅ **SQL Integration**
- gRPC-based SQL service with changefeed support
- SQL queries trigger automatic event emission
- Transaction support with WAL persistence

## 🎯 API Examples (Working Implementation)

### SpacetimeDB-like Reducers
```csharp
[Reducer]
public static int CreateUser(ReducerContext ctx, string name, string email) {
    var user = new User {
        Id = Utils.GenerateId(),
        Name = name,
        Email = email
    };

    // Automatic changefeed event emission
    ctx.Db.GetTable<User>().Insert(user);
    return (int)user.Id;
}
```

### Dynamic Table Access
```csharp
// Both patterns work:
ctx.Db.users.Insert(user);           // Dynamic access
ctx.Db.GetTable<User>().Insert(user); // Type-safe access
```

### WebSocket Subscriptions with WASM Filtering
```javascript
const client = new InstantDBClient('ws://localhost:8080');
await client.subscribeToRecentActivities((data) => {
    console.log('Real-time update:', data);
});
```

## 🏗️ Implementation Details

### Architecture Components (Implemented)

```
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│   gRPC/SQL      │───▶│   Storage        │───▶│  Changefeed    │
│   Service       │    │   Engine         │    │  Engine        │
└─────────────────┘    └──────────────────┘    └────────────────┘
                                │                       │
                                ▼                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│   WASM Engine   │◄───│   Transaction    │    │  WebSocket     │
│   (Wasmtime)    │    │   Manager        │    │  Server        │
└─────────────────┘    └──────────────────┘    └────────────────┘
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│  WASM Modules   │    │      WAL         │    │   Clients      │
│  (C#, C++)      │    │   Persistence    │    │ (WebSocket)    │
└─────────────────┘    └──────────────────┘    └────────────────┘
```

### File Structure (Current)
```
/src
  /cmd                    # Server executables
    instantdb_server.cpp  # Main server with all components
    sql_demo.cpp         # SQL client demo
    grpc_client.cpp      # gRPC client
  /storage               # Storage engine with automatic changefeed
    storage_engine.cpp   # Main storage with changefeed integration
    mem_table.cpp        # In-memory tables
    transaction_impl.cpp # Transaction management
    wal_impl.cpp         # Write-ahead logging
  /changefeed            # Real-time streaming
    changefeed_engine.cpp # Event delivery system
  /wasm                  # SpacetimeDB-like WASM runtime
    wasm_engine.cpp      # Module execution engine
    wasm_subscription.cpp # WASM-powered subscriptions
  /sql                   # SQL integration
    sql_engine.cpp       # SQL parsing with changefeed support
  /websocket             # WebSocket server
    websocket_server.cpp # Real-time client connections
  /grpc                  # gRPC service
    grpc_server.cpp      # gRPC API implementation

/sdk                     # Developer SDKs
  /csharp
    InstantDB.cs         # Complete C# SDK with SpacetimeDB-like API
    /Examples
      CounterModule.cs   # Updated with ReducerContext
      SubscriptionModule.cs # WASM subscription examples
  /cpp                   # C++ SDK (header-only)

/docs                    # Documentation
  WASM_MODULES.md        # WASM module development guide
  WASM_SUBSCRIPTIONS.md  # Real-time subscription guide
  /examples              # Client examples
    websocket_subscription_client.js # WebSocket client demo
```

### Key Implementation Features

**SpacetimeDB-like API Pattern:**
- `ReducerContext ctx` as first parameter to all reducers
- `ctx.Db.GetTable<T>()` for type-safe table access
- `ctx.Db.tablename` for dynamic table access
- Automatic change detection on all database writes

**Automatic Event Emission:**
- Storage engine linked to changefeed engine
- All `Insert()`, `Update()`, `Delete()` operations trigger events automatically
- No manual `Events.Emit()` calls required in user code

**Real-time Subscriptions:**
- WASM-powered filtering and transformation of events
- WebSocket delivery with client ID tracking
- Live event streaming from database changes

**Production-Ready Features:**
- Transaction support with WAL persistence
- gRPC API for SQL and module management
- Resource sandboxing for WASM modules
- Comprehensive error handling and logging

---

## 📋 Original Specification

Below is the complete, actionable scaffold plan for an in-memory SQL RDBMS that (1) runs user-supplied WASM modules inside the server, (2) is clustered (replicated) and durable, and (3) streams table changes to clients over WebSockets (changefeeds). This includes architecture, data formats, APIs (gRPC + proto examples), host API design for WASM, transaction/consensus design, subscription protocol, file layout, testing/CI, operational concerns (k8s, backups, upgrades), security, and a prioritized implementation checklist.

I assumed you want strong consistency for stateful writes (leader-based replication using Raft), deterministic behavior for modules, and low-latency subscriptions. Where there are tradeoffs I call them out and give alternatives.

1. Goals & assumptions

Primary goals: strong-consistency clustered in-memory SQL DB, persisted via WAL + snapshots, supports user-compiled WASM modules executed safely inside the DB server, streams committed row-level changes via WebSocket subscriptions.

Assumptions: leader-based replication (Raft), per-shard Raft groups for scale, modules compile to WASM/WASI, modules run on the leader within transactions and are sandboxed, changefeeds publish after commit.

Decisions you must confirm later (tradeoffs):

Strict serializability vs snapshot isolation.

Cross-shard transactions (2PC) or restrict modules to single-shard operations.

Module binary replication approach (raft log vs external object store).

Read scaling approach (leader-only vs read-index/lease).

2. High-level architecture (components)

Client API Layer

gRPC/HTTP(s) for SQL commands and administrative APIs (module upload, listing, module management).

WebSocket endpoint for subscriptions/changefeeds.

SQL Engine / Planner

Parse SQL; map statements to table operations, user-defined function/module calls, or control operations (CREATE MODULE, CREATE SUBSCRIPTION).

Storage Engine (in-memory)

In-memory tables keyed by primary key, indexes (hash or B-tree in memory), WAL, periodic snapshotter.

Consensus Layer

Raft per-shard (each shard = key range). A leader per shard serializes mutations and coordinates replication.

WASM Module Host

Wasmtime/WasmEdge embedded in C++ with a restricted host API exposing transactional DB operations (txn_read, txn_write, emit_event, etc.). Instance pool for fast reuse.

Module Store

Storage for module binaries & metadata (versioned). Metadata replicated via Raft; binaries either stored in Raft or external object storage with content digest in Raft.

Changefeed Engine

Aggregates emit_event() from committed transactions into a durable change log and delivers events to subscribers via WebSocket. Manages offsets/retention and backpressure.

Observability & Ops

Prometheus metrics, logs, traces, health endpoints, admin CLI.

3. Data model & on-disk formats
In-memory table model

Table = schema (columns, types, PK) + indexes.

Store rows as compact binary blobs (columnar optional). Keep a RowID and PK→RowID index.

Each write operation is represented as (table_id, op {INSERT/UPDATE/DELETE}, pk, row_blob).

WAL & Raft log entries

Log entry proto (conceptual):

message RaftEntry {
  oneof payload {
    ModuleInvocation module_invocation = 1;
    DDL ddl = 2;
    // etc.
  }
  uint64 term = 100;
  uint64 index = 101;
}
message ModuleInvocation {
  string module_id = 1;
  string module_version = 2;
  bytes input = 3; // serialized args
  repeated TableWrite staged_writes = 4; // produced by module execution on leader
  repeated Event emitted_events = 5; // changefeed events
  string tx_id = 6;
}
message TableWrite {
  string table = 1;
  enum Op { INSERT=0; UPDATE=1; DELETE=2; }
  Op op = 2;
  bytes key = 3;
  bytes value = 4; // empty for delete
}
message Event {
  string table = 1;
  uint64 op_index = 2; // event sequence within tx
  string op = 3; // "INSERT"/"UPDATE"/"DELETE"/"CUSTOM"
  bytes key = 4;
  bytes value = 5;
}


Important: staged_writes and emitted_events are the authoritative results that followers will apply during log application.

Snapshots

Periodic cluster snapshot contains:

All table data & indexes.

Current Raft index and term.

Module metadata and module binaries or references.

Subscriber offsets (if needed).

Snapshot format: protobuf container + chunked upload.

4. Consensus & transaction model
Leader-only execution (recommended)

All mutating requests (SQL that modifies data or invokes modules that can write) must be routed to the leader of the shard.

Execution flow:

Leader receives request → obtains execution lock for shard (serialize execution per-shard).

Leader executes module or SQL planner against current committed snapshot, producing a set of staged writes and emitted events.

Leader packages results into a ModuleInvocation log entry and calls Raft AppendEntries.

On commit, the leader applies the staged writes to in-memory state and broadcasts events to subscribers.

Followers apply the same log entry deterministically (no module execution on followers required).

This guarantees deterministic, linearizable writes as long as execution is serialized per shard.

Concurrency & isolation

Per-shard serialized execution provides serializability inside a shard.

For global serializability across shards, you have three choices:

Disallow cross-shard transactions. Simpler.

Implement 2PC across leaders with a distributed coordinator (more complicated).

Use a global sequencer to assign monotonic tx ids before execution (limits scalability).

Reads:

Leader reads are simple and linearizable.

Followers can serve stale reads or linearizable reads via Raft readIndex/lease mechanisms.

5. WASM runtime & host API
Why WASM

Sandboxing, portability, resource control, determinism when host API is controlled.

Runtime embedding (C++ integration)

Use Wasmtime or WasmEdge (embeddable C++ runtimes).

Provide a pool of pre-instantiated WASM instances per module/version to reduce startup latency.

Configure resource limits per instance (memory, CPU time via instruction metering).

Host API design (exports to WASM)

Expose a small, capability-scoped host API. All calls are synchronous and operate against the current transaction snapshot:

Example exported host calls (conceptual C ABI names — you'll map via the Wasm embed API):

// returns 0 for not found, 1 for found; value written into linear memory (pointer/len)
int host_txn_read(const char* table, const uint8_t* key, uint32_t key_len, uint32_t out_ptr);

// iterate: returns iterator id
int host_txn_scan(const char* table, const uint8_t* prefix, uint32_t prefix_len, int limit);

// write (staged for this txn)
void host_txn_write(const char* table, const uint8_t* key, uint32_t key_len, const uint8_t* value, uint32_t value_len);

// delete
void host_txn_delete(const char* table, const uint8_t* key, uint32_t key_len);

// emit a changefeed event (attached to this txn)
void host_emit_event(const char* topic, const uint8_t* key, uint32_t key_len, const uint8_t* payload, uint32_t payload_len);

// abort txn
void host_abort(const char* message);

// get deterministic "now" - leader provides value, but recorded in log for replay
uint64_t host_now_ms(); // use only if leader provides reproducible value

Host capabilities & sandboxing

Each module is given a capabilities descriptor at invocation: allowed tables, read-only vs read-write, can_emit_events, network access boolean, memory limit, max cpu nanoseconds.

The runtime enforces memory and CPU (instruction) limits; if the module exceeds limits, the host aborts the txn and returns error.

Deterministic behavior

Avoid exposing non-deterministic host calls (system time, RNG) unless the leader provides deterministic seeds / times and logs them as part of the invocation so followers can replay by using the same seeds/values (these are included in the ModuleInvocation entry).

6. Module lifecycle & storage

Upload: client uploads module.wasm via Admin API (gRPC/HTTP). Provide metadata: module_id, semantic version, manifest (capabilities, entry points).

Validate & sign: server optionally validates the module (size, WASI imports, disallowed syscalls) and verifies signature if required.

Install: leader processes InstallModule command, replicates module metadata in Raft. Binary storage options:

Small modules (< a few MB): store the binary as part of Raft log entry so followers always have it. Pros: simple. Cons: log bloat.

Large modules: upload binary to an object store (S3) and put a digest + URL in Raft. All nodes fetch and cache the binary by digest during apply.

Versioning & revocation: module versions are immutable. To revoke, submit a RevokeModule command replicated by Raft.

Invocation: leader loads module from cache or store and checks capabilities + runtime limits, executes with host API.

7. SQL integration & language
SQL primitives & module bindings

Extend SQL with module artifacts:

CREATE MODULE name FROM '...' WITH (capabilities...);

CALL module_name(arg1, arg2, ...) — module invocation as stored procedure.

CREATE FUNCTION name(...) RETURNS ... LANGUAGE WASM AS MODULE 'mod:1.0' — module as UDF (if allowed).

CREATE SUBSCRIPTION sub1 FOR SELECT * FROM table WHERE ... — changefeed registration.

For DML statements that trigger modules (e.g., triggers), run module in the transaction that performs the DML.

Planner

Use a SQL parser (existing opensource parser or embed SQLite/DuckDB as parser/engine) and translate to either:

Native operations on in-memory tables (no module).

Module invocations that become Raft log entries (mutative).

Example SQL flows

Store procedure call (mutating):

CALL transfer_funds(from_id, to_id, amount);


translates into module invocation module=transfer_funds, executed on leader, result writes are staged into the ModuleInvocation log entry.

8. Changefeeds & WebSocket subscription protocol
Event model

Modules call host_emit_event(...) as part of their transaction. These events are attached to the transaction and committed together.

Each emitted event gets a tuple {commit_index, tx_id, event_seq, table, op, key, payload} once the transaction is committed.

Event storage & retention

Keep a durable event log that stores emitted events (either embedded in Raft entries or appended to a separate append-only topic replicated across nodes).

Retention policy: keep events for N days or until compaction. Allow subscribers to resume from offsets while events are retained.

WebSocket protocol (logical)

Protocol is JSON-over-WebSocket for clarity (binary options possible). Example messages:

Client → Server (subscribe)

{ "action":"subscribe",
  "subscription":"sub_users",
  "sql":"SELECT * FROM users WHERE country='CA'",
  "start_offset": 0,
  "mode":"at_least_once"  // or "exactly_once" (if supported)
}


Server → Client (ack)

{ "type":"sub_ack", "subscription":"sub_users", "start_offset": 0 }


Server → Client (event)

{
  "type":"event",
  "subscription":"sub_users",
  "offset":12345,
  "tx_id":"tx-0001",
  "table":"users",
  "op":"INSERT",
  "key":"1",
  "value": {"id":1,"name":"Alice","country":"CA"}
}


Client → Server (ack / flow control)

{"action":"ack","subscription":"sub_users","offset":12345}
{"action":"credits","subscription":"sub_users","credits":100}

Delivery semantics & flow control

At-least-once: default; server replays events since last acknowledged offset on reconnect.

Exactly-once: requires idempotency on client side or server-side deduplication keyed by (tx_id, event_seq) and persistent per-subscriber delivered-offset; more complex.

Backpressure: client sends credits; server doesn't send beyond credits, else it buffers (bounded). Implement per-subscriber buffer cap; when full, either drop slow clients or temporarily pause delivery.

Reconnection: client reconnects; server uses start_offset to replay events.

Subscription filters & pushdown

Allow SQL predicate pushdown for basic filters (e.g., table + where clause). If filters are complex, evaluate filter on server while scanning committed event stream.

9. API contracts (proto examples)
Module admin & SQL service (gRPC)
syntax = "proto3";
package db;

service Admin {
  rpc UploadModule(UploadModuleReq) returns (UploadModuleResp);
  rpc InstallModule(InstallModuleReq) returns (InstallModuleResp);
  rpc ListModules(Empty) returns (ListModulesResp);
}
service SQL {
  rpc Execute(SQLRequest) returns (SQLResponse);
  rpc StreamExecute(SQLRequest) returns (stream SQLResponse); // for long polls
}
message UploadModuleReq {
  string module_id = 1;
  string version = 2;
  bytes wasm = 3;
}
message InstallModuleReq {
  string module_id = 1;
  string version = 2;
  // optional capabilities claimed
}
message SQLRequest { string sql = 1; repeated bytes params = 2; }
message SQLResponse { bool ok = 1; bytes result = 2; string leader_address = 3; }

Internal replication (log entry) – conceptual (showed earlier)

ModuleInvocation message holds results of leader execution (staged_writes + emitted_events) so followers can apply without executing the module.

10. Failure modes & recovery
Module binary missing during replay

If a follower needs to apply a log entry referencing a module binary not present locally:

Option A: store the binary in the log entry (guaranteed).

Option B: binary stored in object store; follower fetches by digest (make this fetch blocking in apply path).

If fetch fails, node should mark itself unhealthy and stop applying.

Node restart

On restart: load latest snapshot, download module binaries referenced by snapshot if needed, then replay WAL entries > snapshot_index.

Network partitions

Raft handles leader election; ensure clients can retry or redirect on leader change.

Slow subscribers

For backpressure, bound server memory and allow per-subscription retention window; drop or mark subscription slow if backlog exceeds threshold.

11. Monitoring & observability

Expose Prometheus metrics:

raft_term, raft_commit_index, raft_leader_up (gauge)

module_invocations_total, module_invocations_failed, module_invocation_duration_seconds (histogram)

wasm_instances_total, wasm_instance_pool_size

websocket_connections_total, ws_subscriptions_active

event_delivery_lag_seconds, event_queue_size

wal_bytes, snapshot_last_index

Logs:

Structured logs for module install/invoke, transactions, errors, revocations.

Tracing:

Trace each client request across SQL → module execution → raft append → commit → changefeed publish.

12. Security & compliance

Sandboxing: WASM runtime with memory and instruction metering.

Capabilities: modules have an explicit whitelist of host API capabilities (no default access).

Code signing: require signed module binaries or pre-approved publishers.

Resource quotas: per-tenant quotas (CPU, memory, concurrent invocations).

Auditing: all module executions, uploaded binaries, and admin actions logged and stored immutably.

Network controls: modules cannot open arbitrary network connections unless explicitly proxied and audited.

Least privilege for admin APIs: RBAC for module upload/install and subscription management.

13. Developer UX & SDKs

Provide SDKs for building modules: C/C++ header or Rust crate that wraps host API function calls.

Example: small C header to call host functions (compile with wasm32-wasi or similar).

CLI tool dbctl:

dbctl upload-module module.wasm --id transfer --version 1.0 --sign

dbctl install-module transfer:1.0

dbctl sql "CALL transfer(...)" --node leader:50051

dbctl subscribe --sql 'SELECT * FROM users'

14. File layout & scaffold (posix/CMake)
/repo
  /src
    /cmd
      server_main.cpp
    /raft        # consensus wrapper
    /sql         # parser, planner, executor
    /storage     # in-memory tables, WAL, snapshots
    /wasm        # wasm host, instance pool
    /modules     # module store, metadata
    /net         # gRPC server, websocket server
    /changefeed  # event storage and delivery
    /admin       # CLI, admin API
  /tests
  /deploy
    k8s-manifests/
  CMakeLists.txt
  third_party_deps.md

15. Dependencies (recommended)

C++ build: CMake

RPC: gRPC + Protobuf

WASM runtime: Wasmtime or WasmEdge (C++ embedding)

Consensus: NuRaft or other mature C++ raft implementation

Networking: Boost.Asio (gRPC uses its own), Boost.Beast or websocketpp for WebSocket server

Storage options: RocksDB/LevelDB for module store or durable objects; or native files for WAL/snapshots

Observability: prometheus-cpp

Logging: spdlog or similar

16. Testing plan (unit → integration → chaos)

Unit tests: Storage operations, in-memory indexes, serialization.

WASM host tests: run small WASM test modules that exercise txn_read/write/emit.

Integration: Single node: SQL → module → WAL → apply → snapshot.

Cluster tests: 3/5 node Raft cluster: leader failover, follower catch-up.

Changefeed tests: subscribe, inject transactions, verify delivery order and replay by offset.

Chaos tests: network partition (tc/netem), disk I/O faults, process kill and recovery.

Fuzz tests: fuzz host API inputs to WASM to find crashes or memory leaks.

17. Dev milestones (scaffold tasks, no durations)

Follow this implementation order (each stage produces a runnable prototype):

Minimal single-node, no replication

In-memory table + WAL + snapshot

Basic SQL parser & single-node executor (no modules yet)

WebSocket changefeed for local commits

Embed WASM runtime (single-node)

Host API skeleton, run simple wasm modules against local table

Implement txn_read, txn_write, emit_event in host API

Add Raft & replication

Replace single-node commit with leader + follower prototype

Ensure leader executes module and writes ModuleInvocation entries

Module Store & lifecycle

Module upload/install/admin APIs + module binary persistence strategy

Subscriptions & WebSocket changefeed

Durable event log, per-subscriber offsets, resume/replay logic

Sharding

Implement consistent hashing, per-shard Raft groups, shard rebalancing

Hardening

Resource metering, auditing, signing, performance tuning

Production tooling

Backup, restore, k8s manifests, rolling upgrade procedures, monitoring dashboards

18. Example flows (end-to-end)
A. Module upload + install

UploadModule(wasm, module_id, version) → server stores binary in module-store (or S3) and returns digest.

InstallModule(module_id, version, digest) → leader appends InstallModule Raft entry; on commit nodes cache binary.

B. Client invokes a module via SQL

Client submits CALL my_module(arg1, arg2) to any node.

Node forwards to shard leader (if not leader) or returns leader address for redirect.

Leader takes shard lock, creates transaction snapshot, loads module instance from pool, runs module with txn_* host functions.

Module produces staged writes and events; leader packages these into ModuleInvocation and calls Raft append.

On commit, leader applies writes to in-memory tables and pushes events to changefeed; acknowledgment returned to client.

C. Subscriber receives events

Client opens WebSocket, sends subscribe request with start_offset.

Server responds ack and starts streaming events (JSON) from event log.

Client acks events or grants credits; on reconnect it requests next offset and continues.

19. Operational concerns & k8s deployment

Use StatefulSets for nodes of each Raft group (shards) or deploy one process per node that participates in many shard groups (preferred).

PVCs for each node to persist WAL/snapshots.

Rolling upgrades: Raft membership change + graceful drain of leader roles.

Backup strategy: snapshot + WAL archiving. Offsite snapshot storage (S3).

Autoscaling: scale read-only replicas by adding followers, scale write capacity by adding shards and rebalancing.

20. Performance & tuning knobs

WAL fsync policy (sync per entry vs group commit).

Batch size for Raft replication.

Snapshot frequency and size.

WASM instance pool size and module cache size.

Subscription buffer size and retention window.

Shard keyspace partitioning granularity.

21. Example code snippets (conceptual)
Protobuf (ModuleInvocation excerpt)
message ModuleInvocation {
  string module_id = 1;
  string module_version = 2;
  string tx_id = 3;
  bytes leader_seed = 4; // deterministic inputs to module (optional)
  repeated TableWrite staged_writes = 5;
  repeated Event emitted_events = 6;
}

WASM host C++ API (pseudocode header)
// wasm_host_api.h
#pragma once
#include <string>
#include <vector>

struct HostTxn {
  // Return optional value
  bool read(const std::string& table, const std::string& key, std::string* out_value);
  // stage write
  void write(const std::string& table, const std::string& key, const std::string& value);
  void del(const std::string& table, const std::string& key);
  // emit changefeed event
  void emit_event(const std::string& topic, const std::string& key, const std::string& payload);
  void abort(const std::string& reason);
};

22. Common pitfalls & advice

Determinism: never let follower re-run module code — followers must apply staged writes from leader log entries.

Performance: copying blobs in/out of WASM memory is expensive — use zero-copy if possible or memory arenas to reduce overhead.

Security: don’t run third-party native code inside your process; prefer WASM. If native code is required, use separate processes with seccomp & cgroups.

Cross-shard transactions: complicated — design early if required.

Compaction: Raft logs + event logs will grow; plan snapshot and compaction carefully.

23. What I can produce next (pick one)

a) A C++ + CMake scaffold (file structure + minimal skeleton code for server, wasm host, storage, and a mocked Raft) to get started; or

b) A full protobuf API file (admin + SQL + internal replication messages + changefeed) ready for protoc; or

c) A concrete example of a small WASM module (Rust or C) and how it calls the host API + host integration code (Wasmtime embedding + API binding).


📜 Detailed Implementation Plan
1. Storage Engine (Foundation)

Goal: Achieve durable, transactional, in-memory storage.

Tasks

 MemTable & Indexing

Row-oriented storage (primary key + value blobs).

Hash table or B+Tree for lookups.

 MVCC Implementation

Versioned rows (txn_id, commit_ts).

Readers see snapshot at their start timestamp.

 Write-Ahead Log (WAL)

Append-only log per shard.

fsync policies (strict vs. batch).

 Snapshots

Periodic checkpointing (serialize MemTable).

Log truncation after snapshot.

Milestone

✅ A single-node engine where you can:

BEGIN;
INSERT INTO users VALUES (1, 'Alice');
COMMIT;


...and the data persists after restart.

2. Raft Consensus (Cluster Backbone)

Goal: Replicated, consistent state machine.

Tasks

 Integrate NuRaft

Define state machine callbacks: apply_log, create_snapshot, load_snapshot.

 Cluster Membership

API to join/leave nodes.

Heartbeat monitoring.

 State Machine Application

Log entries = Transaction {sql, wasm_invocation, writes, events}.

Deterministic apply on all nodes.

Milestone

✅ Start a 3-node cluster, elect leader, replicate inserts across nodes.

3. WASM Runtime (Programmable Logic)

Goal: Run user modules safely inside DB transactions.

Tasks

 Wasmtime Integration

Host functions: txn_read, txn_write, emit_event.

 Resource Sandboxing

Memory limits, instruction metering, timeouts.

 Module Lifecycle

Upload, versioning, revoke.

Store metadata in Raft state machine.

Milestone

✅ User uploads a WASM module that transforms row data:

CALL my_wasm_function('arg');


...executes deterministically on leader.

4. SQL Engine (User Interface)

Goal: Parse & execute SQL with module support.

Tasks

 Parser

Use ANTLR SQL grammar or embed SQLite parser.

 Planner & Executor

Translate SQL → transaction operations.

Recognize CALL → WASM invocation.

 Transaction Handling

BEGIN / COMMIT / ROLLBACK.

Integrate with MVCC and Raft log.

Milestone

✅ Can run queries:

INSERT INTO orders VALUES (1, 'Widget');
CALL discount_module(1);
SELECT * FROM orders;

5. Changefeed Engine (Realtime Streaming)

Goal: Notify clients of row-level changes over WebSocket.

Tasks

 Event Capture

Hook into commit path.

Generate ChangeEvent {table, op, key, value, ts}.

 Subscription Manager

SQL predicate filtering on server-side.

Track per-client offsets.

 WebSocket Delivery

At-least-once delivery.

Replay from offset on reconnect.

Milestone

✅ Client subscribes:

SUBSCRIBE users WHERE age > 30;


...receives push updates in real-time.

6. Observability & Ops

Goal: Production visibility.

Tasks

 Logging

Structured logs (JSON) with txn IDs.

 Metrics

Latency histograms, Raft stats, query throughput.

 Tracing

Transaction tracing with OpenTelemetry hooks.

Milestone

✅ Admin can curl /metrics to see Prometheus data.

🛠 Development Strategy
A. Phased Milestones

Single-node local DB (Storage + SQL)

Focus: correctness + persistence.

Cluster replication (Raft + WAL)

Focus: HA + consistency.

WASM integration

Focus: safe programmable logic.

Changefeeds/WebSocket streaming

Focus: reactive data pipelines.

Cluster-scale testing

Focus: sharding, rebalancing, failover.

B. Testing Approach

Unit tests: Storage, Raft log application, SQL parsing.

Integration tests: Multi-node cluster with failover.

Fuzzing: SQL parser and WASM sandbox host functions.

Chaos testing: Kill leader mid-transaction → verify consistency.

C. Example Dev Roadmap (3–6 months)

Month 1: Storage engine + basic SQL executor.

Month 2: Raft integration, single-table replication.

Month 3: WASM sandbox & CALL syntax.

Month 4: Changefeed & WebSocket streaming.

Month 5: Sharding, multi-table joins.

Month 6: Observability, security hardening, stress tests.

🌟 Endgame Vision

At the end of this roadmap, you’ll have:

A distributed in-memory RDBMS with persistence.

Programmable WASM modules for server-side logic.

SQL interface for ease of use.

Changefeeds over WebSocket for realtime apps.

Clustered replication with Raft for consistency.

A foundation to scale into sharding, indexing, and cross-shard transactions.

Would you like me to now draft a minimal end-to-end prototype flow (e.g., INSERT → WAL → Raft → Apply → Changefeed → WebSocket client), so you have a concrete “happy path” to target for your first integration milestone?

You said:
Yes, draft a minimal end-to-end prototype flow
ChatGPT said:

Perfect ✅ — let’s build a happy path prototype flow that covers the full journey:

INSERT → WAL → Raft → Apply → Changefeed → WebSocket Client

This will give you the first vertical slice of InstantDB, even if many parts are stubbed.

🔄 End-to-End Flow (Happy Path)
1. Client issues SQL
INSERT INTO users VALUES (1, 'Alice');

2. SQL Engine parses + plans

SQL parser extracts:

{
  "op": "INSERT",
  "table": "users",
  "values": [1, "Alice"]
}


Query planner wraps it in a transaction request object.

3. Transaction request → Raft log entry

The leader packages request into a Raft entry:

{
  "txn_id": 42,
  "ops": [
    { "op": "INSERT", "table": "users", "key": 1, "value": "Alice" }
  ]
}


Appends entry to WAL before sending to followers.

NuRaft replicates entry, waits for quorum.

4. Commit on leader

Once quorum ack:

Mark txn committed.

Apply writes to MemTable (users[1] = "Alice").

Generate ChangeEvent:

{
  "table": "users",
  "op": "INSERT",
  "key": 1,
  "value": "Alice",
  "ts": 1695234555
}

5. Changefeed Engine enqueues event

Subscriptions tracked per WebSocket connection.

Filtering is applied (e.g., SUBSCRIBE users WHERE id > 0).

Matching subscribers get event enqueued.

6. WebSocket Server delivers to client

Event serialized (protobuf or JSON):

{
  "type": "change_event",
  "table": "users",
  "op": "INSERT",
  "row": { "id": 1, "name": "Alice" }
}


Sent via WebSocket send().

🧩 Prototype Components (Minimal Stubs)

Here’s a simplified C++-like pseudocode for the flow:

// ---- SQL Engine ----
struct SqlRequest { std::string sql; };
struct TxnOp { std::string op, table; int key; std::string value; };
struct TxnEntry { int txn_id; std::vector<TxnOp> ops; };

TxnEntry parse_sql(const SqlRequest& req) {
    // naive parser: INSERT INTO users VALUES (1, 'Alice');
    return { 42, {{"INSERT", "users", 1, "Alice"}} };
}

// ---- Raft Layer ----
void raft_append(const TxnEntry& entry) {
    wal_append(entry);         // persist locally
    raft_replicate(entry);     // send to followers
    apply_entry(entry);        // leader apply on commit
}

// ---- Storage ----
std::unordered_map<int, std::string> users;

void apply_entry(const TxnEntry& e) {
    for (auto& op : e.ops) {
        if (op.op == "INSERT" && op.table == "users") {
            users[op.key] = op.value;
            emit_change(op);
        }
    }
}

// ---- Changefeed ----
struct ChangeEvent { std::string table, op; int key; std::string value; };
std::vector<WebSocket*> subscribers;

void emit_change(const TxnOp& op) {
    ChangeEvent ev{op.table, op.op, op.key, op.value};
    for (auto* sub : subscribers) {
        sub->send(serialize(ev));
    }
}

🏁 Prototype Milestone (Target Demo)
Minimal Workflow:

Start 1-node leader (Raft stub).

Connect WebSocket client with SUBSCRIBE users.

Send SQL:

INSERT INTO users VALUES (1, 'Alice');


Client receives:

{"table":"users","op":"INSERT","key":1,"value":"Alice"}

Stretch Goal:

Spin up 3 nodes, kill the leader, and show:

Insert still works.

WebSocket subscribers continue receiving updates from new leader.

📌 Implementation Order for Prototype

Storage stub: unordered_map table.

SQL stub: hardcoded parsing for INSERT.

Raft stub: local apply (later plug into NuRaft).

WAL stub: append JSON entries to file.

Changefeed stub: in-memory subscriber list, push JSON.

WebSocket server: Boost.Beast or websocketpp.

This prototype gets you client-visible success with:

SQL accepted,

Data stored,

Event streamed in real time.

From there, you can harden each component (real SQL parser, full Raft, WASM hooks, MVCC, snapshots, filtering).
