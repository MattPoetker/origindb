# WASM Subscriptions in OriginDB

WASM subscriptions let a WebSocket client route changefeed events through a
deployed module before delivery: a **filter** function decides per event
whether to deliver it, and a **transform** function can rewrite the payload.
Both are ordinary reducer registrations invoked through the module's single
`origindb_invoke` entry point — see [WASM_ABI.md](WASM_ABI.md) for the
contract and [../WASM_MODULES.md](../WASM_MODULES.md) for the module guide.

## Architecture Overview

```
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│   Database      │───▶│   Changefeed     │───▶│ WASM Sub Mgr   │
│   Operations    │    │   Engine         │    │                │
└─────────────────┘    └──────────────────┘    └────────────────┘
                                                       │
                                                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│  WebSocket      │◄───│   Filtered &     │◄───│  WASM Module   │
│  Clients        │    │   Transformed    │    │  (Filter/Xform)│
└─────────────────┘    │   Events         │    └────────────────┘
                       └──────────────────┘
```

For each event, the subscription manager:

1. Applies the subscription's table list (if any).
2. Invokes `filter_function` (if set) with the event JSON; ABI status `1`
   includes the event, `0` excludes it. A failed filter call drops the
   event and logs a warning.
3. Invokes `transform_function` (if set) with the event JSON; the value the
   module returns via `host_set_result` replaces the payload.
4. Sends the result to the subscribing client.

## WebSocket API

### Connection and Welcome

```javascript
const ws = new WebSocket('ws://localhost:8080');

// Welcome message includes client_id and available features
{
  "type": "welcome",
  "client_id": "client_12345_67890",
  "server_version": "0.1.0",
  "features": ["changefeed", "wasm_subscriptions"]
}
```

### Creating WASM Subscriptions

```javascript
ws.send(JSON.stringify({
  "type": "wasm_subscribe",
  "module_name": "todo",                 // required
  "filter_function": "onlyPending",      // optional
  "transform_function": "addMetadata",   // optional
  "tables": ["todos"],                   // optional table filter
  "include_initial_data": true,          // optional; invokes __get_initial_data
  "start_offset": 0                      // optional
}));

// Response
{
  "type": "wasm_subscription_created",
  "subscription_id": "wasm_sub_1",
  "client_id": "client_12345_67890",
  "module_name": "todo"
}
```

The request may also carry `where_clause`, `columns`, and `parameters`
fields; they are stored on the subscription and passed to the module
(`__get_initial_data` receives the `where_clause` as its argument) — the
server itself does not evaluate them for WASM subscriptions. For
server-side WHERE evaluation without a module, use `sql_subscribe`.

### Receiving Subscription Events

```javascript
{
  "type": "wasm_subscription_event",
  "subscription_id": "wasm_sub_1",
  "client_id": "client_12345_67890",
  "data": { /* the (possibly transformed) event payload */ }
}
```

### The event object

Filter and transform functions receive one argument: the changefeed event
serialized as a JSON object:

```json
{
  "table": "todos",
  "operation": "INSERT",        // INSERT | UPDATE | DELETE | EVENT
  "offset": 42,
  "transaction_id": "…",
  "key": "…",
  "new_value": { /* row as JSON, or null */ },
  "old_value": { /* previous row as JSON, or null */ }
}
```

## Writing Filter and Transform Functions

Use the SDKs — they register functions under the names the subscription
references and handle the ABI status codes for you.

### AssemblyScript (recommended — `sdk/typescript/`)

```ts
import { JsonValue, registerFilter, registerReducer } from "../../assembly/index";

// Filter: deliver only INSERTs of not-yet-done todos
registerFilter("onlyPending", (ev: JsonValue): bool => {
  if (ev.getString("operation") != "INSERT") return false;
  const row = ev.get("new_value");
  return row != null && !row.getBool("done");
});

// Transform: a reducer that returns the rewritten event
registerReducer("addMetadata", (args: Array<JsonValue>): JsonValue | null => {
  const ev = args[0];
  ev.setNumber("processed_at", <f64>nowMs());
  return ev;   // returned value → host_set_result → delivered payload
});
```

### C# (`sdk/csharp/` — note the .NET 8 toolchain caveat in its README)

```csharp
Reducers.RegisterFilter("OnlyUserInserts", ev =>
    ev.GetProperty("operation").GetString() == "INSERT" &&
    ev.GetProperty("table").GetString() == "users");

Reducers.Register("Anonymize", args =>
{
    var ev = args[0];
    // rebuild the event with redacted fields and return it
    return RedactPii(ev);
});
```

A working filter example ships in `examples/csharp/UserService`
(`OnlyUserInserts`).

### Initial data

If the subscription sets `include_initial_data`, the server invokes the
module's `__get_initial_data` lifecycle reducer (args:
`[where_clause]`) once at subscribe time and forwards whatever payload the
module returns via `host_set_result`. If the module doesn't register
`__get_initial_data`, the reserved-name call is a harmless no-op (`-404`).

## Getting Started

### 1. Build and deploy a module

```bash
# AssemblyScript example (sdk/typescript/examples/todo)
cd sdk/typescript && npm install && npm run asbuild
cd ../..
./build/origindb_client deploy todo sdk/typescript/build/module.wasm 1.0.0

# Or from a project directory, in one step:
origindb publish --server=localhost:50051
```

### 2. Create test data

```bash
./build/origindb_client call todo addTodo '["buy milk"]'
./build/origindb_client call todo addTodo '["write docs"]'
```

### 3. Subscribe from a WebSocket client

```javascript
const ws = new WebSocket('ws://localhost:8080');
ws.onopen = () => ws.send(JSON.stringify({
  type: 'wasm_subscribe',
  module_name: 'todo',
  filter_function: 'onlyPending',
  tables: ['todos'],
}));
ws.onmessage = (m) => {
  const msg = JSON.parse(m.data);
  if (msg.type === 'wasm_subscription_event') console.log('event:', msg.data);
};
```

### 4. Trigger events

```bash
./build/origindb_client call todo addTodo '["from wasm"]'      # delivered
./build/origindb_client call todo completeTodo '["<id>"]'      # filtered out
```

## Execution Semantics and Limits

- Filter/transform calls run under the same sandbox as any reducer: the
  module's epoch timeout (default 5 s) and memory limit (default 256 MiB)
  apply, and capability checks (`allowed_tables`, `read_only`) govern any
  table reads the function performs.
- A filter/transform MAY read tables (e.g. to look up the affected user);
  writes staged during a filter call commit only on non-negative status,
  like any other call.
- Calls into one module are serialized; a slow filter delays that module's
  other work, so keep these functions lightweight.
- If a filter or transform call fails (trap, timeout, negative status),
  the event is dropped for that subscription and a warning is logged —
  fail-closed, not fail-open.
- Subscriptions are per-connection state: they are cleaned up on
  disconnect and do not survive server restarts (the module itself does).

## Debugging

```bash
# Watch module log output (host_log) and subscription activity
./build/origindb server -l debug

# Confirm the module and its functions are deployed
./build/origindb_client modules
```

`scripts/ws_filter_check.py` (used by `scripts/e2e_verify.sh`) is a
reference for scripted WebSocket subscription verification.

## API Reference

### WebSocket message types

- `wasm_subscribe` — create a WASM subscription
- `wasm_subscription_created` — confirmation (subscription_id, module_name)
- `wasm_subscription_event` — filtered/transformed event data
- `error` — error responses (unknown module, missing module_name, ...)

### Function semantics (ABI)

- **Filter**: invoked as `origindb_invoke(filter_name, [event])`; return
  status `1` to include, `0` to exclude, `< 0` on error.
- **Transform**: invoked as `origindb_invoke(transform_name, [event])`;
  return status `0` and set the new payload via `host_set_result`.
- **Initial data**: `origindb_invoke("__get_initial_data",
  [where_clause])`; payload via `host_set_result`.

---

**OriginDB WASM Subscriptions** — real-time, programmable data streams
running on the server's wasmtime runtime.
