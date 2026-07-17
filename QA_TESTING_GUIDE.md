# OriginDB Complete System - Manual QA Testing Guide

## Overview
This guide provides manual testing procedures for OriginDB:
- **CLI Tool**: project init, server management, publish, gRPC client
- **SQL Subscription System**: WebSocket real-time subscriptions with
  per-event WHERE evaluation
- **Initial State Broadcast**: current data sent on subscription
- **WASM Module System**: real wasmtime-backed modules — deploy, execute,
  persistence, subscription filters
- **Server Management** and **Database Operations**

## Prerequisites

### 1. Build the Complete System
```bash
cd /Users/a12042/Development/instant_db

cmake -B build -S .
cmake --build build

ls -la ./build/origindb ./build/origindb_server ./build/origindb_client
```

Unit tests build alongside (target `unit_tests`, gtest). The e2e script
defaults to a `build_new` build directory but accepts any:
`scripts/e2e_verify.sh [BUILD_DIR]`.

### 2. Verify CLI Tool
```bash
./build/origindb --help
./build/origindb --version
```

### 3. Testing Tools Required
- **CLI tools**: `./build/origindb`, `./build/origindb_client`
- **WebSocket client**: wscat, websocat, or browser dev tools
- **Multiple terminals** for parallel testing and monitoring

`grpcurl` is **not** required — all gRPC flows in this guide go through
`origindb_client`.

---

## Part A: CLI Tool Testing

### Test Case A1.1: CLI Help and Version
**Steps**:
```bash
./build/origindb --help
./build/origindb --version
./build/origindb logs --help
./build/origindb publish --help
```

**Expected Results**:
- Help lists the real commands: `init`, `server`, `start`, `stop`, `logs`,
  `sql`, `publish`, `client`, `demo`
- Version information displays
- No crashes

### Test Case A1.2: CLI Error Handling
**Steps**:
```bash
./build/origindb invalid_command
```

**Expected Results**:
- Clear "Unknown command" error, non-zero exit code, usage hint

### Test Case A2.1: Project Initialization
**Steps**:
```bash
./build/origindb init test_ts_module --template typescript
./build/origindb init test_cs_module            # csharp is the default
ls test_ts_module/ test_cs_module/
```

**Expected Results**:
- TypeScript project contains `asconfig.json` + `package.json`
- C# project contains a `.csproj` and `global.json` (pinned to .NET 8)
- `--force` / `--no-git` flags behave as documented

Templates are `csharp`, `typescript`, `unity`, `nodejs` — there are no
Rust/Go/C++ templates.

### Test Case A3.1: Server Start/Stop
**Steps**:
```bash
# Terminal 1
./build/origindb server -p 8080 -g 50051 -d ./qa_data

# Terminal 2
./build/origindb_client status
./build/origindb stop
```

**Expected Results**:
- Server starts; log shows WebSocket and gRPC ports ("gRPC Server ready")
- `status` reports version, uptime, and stats
- `stop` terminates the server process

Note: there is no `server status` subcommand or `--daemon` flag; use
`origindb_client status` for liveness and a shell/process manager for
backgrounding.

### Test Case A4.1: Log Viewing
**Steps** (server running, launched from repo root so logs land in
`./logs/origindb.log`):
```bash
./build/origindb logs -n 20
./build/origindb logs --follow &
LOG_PID=$!
./build/origindb_client exec "CREATE TABLE test_logs (id INT64 PRIMARY KEY, data STRING)"
./build/origindb_client exec "INSERT INTO test_logs VALUES (1, 'test')"
kill $LOG_PID
```

**Expected Results**:
- Recent log lines display; follow mode shows the new SQL activity live

### Test Case A5.1: SQL Execution via gRPC Client
**Steps**:
```bash
./build/origindb_client exec "CREATE TABLE cli_test (id INT64 PRIMARY KEY, name STRING)"
./build/origindb_client exec "INSERT INTO cli_test VALUES (1, 'Alice')"
./build/origindb_client exec "INSERT INTO cli_test VALUES (2, 'Bob')"
./build/origindb_client exec "SELECT * FROM cli_test"
./build/origindb_client interactive        # then: SELECT * FROM cli_test / exit
```

**Expected Results**:
- Statements succeed; SELECT returns both rows; interactive mode works

**Known SQL limitations (do NOT file as bugs, but confirm they fail
gracefully):**
- `SELECT ... WHERE ...` executes but the WHERE clause is ignored (full
  table returned); column projection is also ignored at the SQL layer
- `DELETE` is unimplemented
- `CREATE TABLE` currently ignores your column definitions
- Identifiers preserve case (`Users` and `users` are different tables)

---

## Part B: SQL Subscription System Testing

### Test Case B1.1: Subscribe to a Table
**Steps**:
1. Connect WebSocket client to `ws://localhost:8080`
2. Send:
```json
{"type": "sql_subscribe", "sql": "SELECT * FROM users"}
```

**Expected Result**:
- `sql_subscription_created` response with a `subscription_id`
- An `initial_state` message immediately follows with the table's current
  rows (`columns`, `rows`, `rows_count`, `execution_time_ms`)

### Test Case B1.2: WHERE-Filtered Subscription (per-event evaluation)
**Setup**:
```bash
./build/origindb_client exec "CREATE TABLE accounts (id INT64 PRIMARY KEY, name STRING, email STRING)"
```

**Steps**:
1. Subscribe:
```json
{"type": "sql_subscribe", "sql": "SELECT * FROM accounts WHERE name = 'Alice'"}
```
2. Insert matching and non-matching rows:
```bash
./build/origindb_client exec "INSERT INTO accounts VALUES (1, 'Alice', 'a@x.com')"
./build/origindb_client exec "INSERT INTO accounts VALUES (2, 'Bob', 'b@x.com')"
```

**Expected Result**:
- A `sql_changefeed_event` arrives for Alice's INSERT only; Bob's INSERT is
  filtered out
- **Note**: the `initial_state` snapshot is produced by the SQL engine,
  which ignores WHERE — the snapshot contains ALL current rows. WHERE
  filtering applies to subsequent change events only.

Supported WHERE syntax (evaluated per event by the predicate evaluator):
comparisons (`=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`), `AND`/`OR`/`NOT`,
parentheses, `LIKE`, `IS [NOT] NULL`.

### Test Case B1.3: Invalid Subscription Query Rejected
**Steps**:
```json
{"type": "sql_subscribe", "sql": "INVALID SQL SYNTAX"}
```

**Expected Result**:
- Server replies with an `error` frame ("Failed to create SQL
  subscription: ..."); no subscription is created. An unparsable WHERE
  clause is rejected the same way.

### Test Case B1.4: UPDATE Event Matching Semantics
**Steps** (with the `WHERE name = 'Alice'` subscription from B1.2):
```bash
./build/origindb_client exec "UPDATE accounts SET name='Alicia' WHERE id=1"
```

**Expected Result**:
- The UPDATE event IS delivered: an UPDATE matches if the **old OR new**
  row satisfies the predicate (so subscribers see rows leaving the
  filtered set)
- The event carries both `new_value` and `old_value`

### Test Case B2.1: Subscribe to All Tables
**Steps**:
```json
{"type": "subscribe_to_all_tables"}
```

**Expected Result**:
- `all_tables_subscription_created` response
- `initial_state_all_tables` message with a `tables` object (each table:
  columns, rows, rows_count) and `tables_count`
- Subsequent events from every table are delivered

### Test Case B2.2: Initial State with Empty Table
**Setup**:
```bash
./build/origindb_client exec "CREATE TABLE empty_table (id INT64 PRIMARY KEY, data STRING)"
```
Subscribe with `SELECT * FROM empty_table`.

**Expected Result**:
- `initial_state` with `"rows_count": 0`, empty `rows`, valid `columns`

### Test Case B2.3: Exactly-Once Event Delivery
**Steps**:
1. Subscribe to a table
2. Perform one INSERT and one UPDATE via `origindb_client exec`

**Expected Result**:
- Exactly one event per write — no duplicates (the old duplicate SQL-layer
  emission was removed; events are emitted by the storage commit)

### Test Case B3.1: Column Filtering in Events
**Steps**:
1. Subscribe: `{"type":"sql_subscribe","sql":"SELECT id, name FROM accounts"}`
2. Insert a row with id, name, email

**Expected Result**:
- The delivered event's `new_value` contains only `id` and `name`
- Reminder: the `initial_state` snapshot is NOT column-filtered (SQL-layer
  projection limitation); only change events are

### Test Case B4.1: Multiple Clients, Mixed Subscriptions
**Setup**:
- Client A: `SELECT id, name FROM accounts`
- Client B: `SELECT * FROM accounts`
- Client C: `subscribe_to_all_tables`

**Steps**: insert one row into `accounts` and one into another table.

**Expected Result**:
- A receives the accounts event with only id, name
- B receives the accounts event with all fields
- C receives both events
- No cross-client leakage

### Test Case B5.1: Client Disconnection Cleanup
**Steps**:
1. Connect, subscribe, then disconnect abruptly
2. Watch server logs / `origindb_client status` subscription count

**Expected Result**:
- Subscription is removed; count returns to previous value

### Test Case B5.2: Server Restart
**Steps**: subscribe, stop the server (Ctrl+C), restart, reconnect.

**Expected Result**:
- WebSocket subscriptions do not persist — clients must re-subscribe
- Table data DOES persist (WAL recovery)

### Test Group B6: Load and Edge Cases
- Rapid inserts (10-20/sec): all events delivered, none lost or duplicated
- 10+ concurrent clients with different subscriptions: correct filtering
- Special characters / large values (1 MB+ text): events transmitted intact

---

## Part C: WASM Module System Testing

The WASM runtime is real (wasmtime): modules execute with epoch timeouts
(default 5 s/call), a memory limiter (default 256 MiB), deploy-time
capabilities, and atomic staged-write transactions. See
[docs/WASM_ABI.md](docs/WASM_ABI.md) for the contract and
`WASM_MODULES.md` for the practical guide.

### Test Case C0.1: Automated Coverage (run first)

```bash
# Unit tests: wasm engine, predicate evaluator, module store
cmake --build build --target unit_tests
ctest --test-dir build --output-on-failure

# Full end-to-end pipeline:
# build -> start server -> deploy -> execute reducer -> SQL verify ->
# WHERE-filtered websocket delivery -> persistence across restart -> undeploy
scripts/e2e_verify.sh build
```

**Expected Result**:
- All unit tests pass
- e2e script ends with "ALL E2E CHECKS PASSED"

If both pass, the manual cases below are confirmation/exploration.

### Test Case C1.1: Build a Real Module
Use the AssemblyScript todo example (the verified SDK path):

```bash
cd sdk/typescript
npm install
npm run asbuild          # -> build/module.wasm
npm test                 # host-side ABI smoke test
cd ../..
```

(Alternative: `examples/csharp/UserService` with .NET 8 +
`wasi-experimental` — note the C# export-support caveat in
`sdk/csharp/README.md`.)

### Test Case C1.2: Deploy and List
**Prerequisites**: server running.

```bash
./build/origindb_client deploy todo sdk/typescript/build/module.wasm 1.0.0
./build/origindb_client modules
```

**Expected Result**:
- Deploy reports success with the byte count
- `modules` lists `todo v1.0.0` with a sha256 prefix
- Server log shows the module loaded

### Test Case C1.3: Execute Reducers
```bash
./build/origindb_client call todo addTodo '["buy milk"]'
./build/origindb_client call todo listTodos
./build/origindb_client exec "SELECT * FROM todos"
```

**Expected Result**:
- `addTodo` succeeds and returns a result payload with the new id
- `listTodos` returns the rows
- The `todos` table is visible via SQL (auto-created on first module
  write) and contains the row — module writes and SQL reads see the same
  storage

### Test Case C1.4: Reducer Errors and Rollback
```bash
# Module-side validation failure (empty text) -> negative status
./build/origindb_client call todo addTodo '[""]'

# Unknown reducer -> -404 (no handler registered)
./build/origindb_client call todo noSuchReducer

# Unknown module
./build/origindb_client call no_such_module foo
```

**Expected Result**:
- Each call fails with a descriptive error; the server does not crash
- Failed calls commit nothing: `SELECT * FROM todos` shows no new rows

### Test Case C1.5: Module Events Reach SQL Subscriptions
**Steps**:
1. Connect a WebSocket client and subscribe:
```json
{"type": "sql_subscribe", "sql": "SELECT * FROM todos"}
```
2. Execute `./build/origindb_client call todo addTodo '["from wasm"]'`

**Expected Result**:
- The client receives exactly one `sql_changefeed_event` (INSERT on
  `todos`) generated by the module's committed write

### Test Case C1.6: WASM Subscription Filters
**Steps**:
1. Send:
```json
{
  "type": "wasm_subscribe",
  "module_name": "todo",
  "filter_function": "onlyPending",
  "tables": ["todos"]
}
```
2. Add a todo, then complete one
   (`call todo completeTodo '["<id>"]'`).

**Expected Result**:
- `wasm_subscription_created` response
- `wasm_subscription_event` messages arrive only for events the filter
  accepts (the filter returns ABI status 1 = include, 0 = exclude)
- Filter input is the event JSON: `{table, operation, offset,
  transaction_id, key, new_value, old_value}`

### Test Case C2.1: Module Persistence Across Restart
**Steps**:
1. With `todo` deployed and at least one row written, verify the on-disk
   layout: `ls <data_dir>/modules/todo/` (bytecode + `manifest.json`
   containing name/version/sha256)
2. Stop the server, restart it with the same `--data-dir`
3. Check the boot log and re-run:
```bash
grep "Restored persisted module: todo" <server log>
./build/origindb_client modules
./build/origindb_client call todo listTodos
./build/origindb_client exec "SELECT * FROM todos"
```

**Expected Result**:
- Boot log shows `Restored persisted module: todo`
- Module is listed and callable without redeploying
- Table data recovered from the WAL

### Test Case C2.2: Undeploy Removes Persisted Files
```bash
./build/origindb_client undeploy todo
./build/origindb_client modules
ls <data_dir>/modules/
```

**Expected Result**:
- Undeploy succeeds; module no longer listed; `<data_dir>/modules/todo/`
  is gone; subsequent `call` fails with module-not-found

### Test Case C2.3: Redeploy After Undeploy
Re-run C1.2/C1.3.

**Expected Result**: deployment and execution work normally. Note the
module instance starts fresh (linear memory is never persisted) — durable
state lives in tables only.

### Test Case C3.1: Timeout Enforcement (capability limits)
If you have a module with a long-running/looping reducer, deploy it with a
small `timeout_ms` in `DeployModuleRequest.capabilities` and call it.

**Expected Result**:
- The call fails after the deadline (epoch timeout); nothing is committed;
  the instance is discarded and transparently re-instantiated on the next
  call. Other capability checks behave similarly: writes from a
  `read_only` module or access outside `allowed_tables` fail with
  permission-denied (`-2`).

(`origindb_client deploy` currently sends default capabilities; testing
non-default capabilities requires a direct gRPC `DeployModule` call.)

### Test Case C3.2: Concurrent Reducer Calls
```bash
for i in {1..10}; do
  ./build/origindb_client call todo addTodo "[\"item $i\"]" &
done
wait
./build/origindb_client exec "SELECT * FROM todos"
```

**Expected Result**:
- All 10 calls succeed (calls to the same module are serialized
  internally; different modules run concurrently); 10 rows exist; no
  crashes or lost writes

---

## Validation Checklist

### Server Logs
- [ ] Subscription creation/removal logs appear
- [ ] `Restored persisted module: <name>` on reboot with deployed modules
- [ ] No unexpected errors during normal operation

### Client Events
- [ ] Events arrive in real time (< 100 ms locally)
- [ ] WHERE-filtered subscriptions deliver only matching events
- [ ] UPDATE events carry `old_value` and `new_value`
- [ ] Exactly one event per write

### System Health
- [ ] Memory usage stable under load
- [ ] Clean handling of client disconnects
- [ ] Data and modules survive restart

---

## Troubleshooting

### Client not receiving events
- WebSocket connected? Subscription confirmed (check for the `..._created`
  response and server logs)?
- Is the WHERE clause excluding the events? Remember initial_state is
  unfiltered but events are filtered.

### Reducer call fails
- `-404`: name not registered (or, for C#, exports missing — see
  `sdk/csharp/README.md`)
- `-2`: capability violation; `-3`: module-side validation
- Timeout: reducer exceeded its per-call deadline

### Deploy fails
- Is the file a core WASI p1 module (not a p2 component)?
- Does it import only `env` / `wasi_snapshot_preview1`?
- Server running and `-s` pointing at the gRPC port?

---

## Test Environment Cleanup

1. Stop WebSocket clients
2. `./build/origindb_client undeploy <module>` for test modules
3. Stop the server (Ctrl+C or `./build/origindb stop`)
4. Remove test data dirs (`./qa_data`, generated projects)

---

## Success Criteria

1. `ctest` unit tests and `scripts/e2e_verify.sh` pass
2. SQL subscriptions deliver correctly WHERE-filtered, column-filtered,
   exactly-once events; invalid queries are rejected with an error frame
3. Modules deploy/undeploy/redeploy cleanly and persist across restarts
4. Reducers execute transactionally: success commits (and emits exactly
   one event per write), failure commits nothing
5. WASM subscription filters include/exclude events per the module logic
6. Error conditions (bad reducer, bad module, capability violations,
   timeouts) fail gracefully without destabilizing the server
