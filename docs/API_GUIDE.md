# InstantDB API Guide

InstantDB exposes three surfaces:

1. **Module API** — reducers, filters, and lifecycle hooks running inside
   the server as WASM modules (SpacetimeDB-inspired programming model).
2. **WebSocket API** — real-time subscriptions (`ws://localhost:8080`).
3. **gRPC API** — SQL execution and module management
   (`localhost:50051`; see [GRPC_API.md](GRPC_API.md)).

The module-side contract is normatively defined in
[WASM_ABI.md](WASM_ABI.md). This guide covers usage patterns.

## Module API

Modules are written against an SDK that implements the ABI's registry
pattern: register named reducers/filters at initialization; the host
dispatches every call through `instantdb_invoke(name, json_args)`.

- **AssemblyScript** (`sdk/typescript/`) — recommended, verified
  end-to-end. See [../sdk/typescript/README.md](../sdk/typescript/README.md).
- **C#** (`sdk/csharp/`) — works only where the .NET 8 `wasi-experimental`
  workload supports `[UnmanagedCallersOnly]` exports; see
  [../sdk/csharp/README.md](../sdk/csharp/README.md).

There is no attribute-based `[Table]`/`[Reducer]`/`ctx.Db.GetTable<T>()`
API — earlier drafts of this document described a planned design that was
not built. The real API is an explicit registry:

### Reducers (C#)

```csharp
using System.Runtime.CompilerServices;
using System.Text.Json;
using InstantDB;

public static class Program
{
    public static void Main() { }

    [ModuleInitializer]
    internal static void Init()
    {
        Reducers.SetModuleInfo("user_module", "1.0.0");
        Reducers.DeclareTable("users");

        Reducers.Register("CreateUser", args =>
        {
            string name = args[0].GetString()!;
            string email = args[1].GetString()!;
            if (string.IsNullOrWhiteSpace(name))
                throw new ReducerException("name required", -3);

            long id = Host.GenerateId();
            Db.Write("users", id.ToString(), JsonSerializer.Serialize(new
            {
                id = id.ToString(),
                name,
                email,
                created_at = Host.NowMs(),
            }));
            return new { id = id.ToString() };
        }, "name", "email");
    }
}
```

### Reducers (AssemblyScript)

```ts
import { JsonValue, registerReducer, setModuleInfo, declareTable,
         writeTable, generateId, nowMs, abortCall } from "../../assembly/index";
export { instantdb_alloc, instantdb_free, instantdb_describe,
         instantdb_invoke, __instantdb_abort } from "../../assembly/index";

setModuleInfo("todo", "1.0.0");
declareTable("todos");

registerReducer("addTodo", (args: Array<JsonValue>): JsonValue | null => {
  const text = args.length > 0 ? args[0].asString() : "";
  if (text.length == 0) abortCall("addTodo: text required");
  const id = generateId().toString();
  writeTable("todos", id, JsonValue.newObject()
    .setString("id", id).setString("text", text)
    .setBool("done", false).setNumber("created_at", <f64>nowMs())
    .toString());
  return JsonValue.newObject().setString("id", id);
}, ["text"]);
```

### Database operations (per call, staged + atomic)

| Operation | Semantics | Changefeed event |
|---|---|---|
| write (`Db.Write` / `writeTable`) | Stages an upsert | INSERT/UPDATE on commit |
| delete (`Db.Delete` / `deleteTable`) | Stages a delete | DELETE on commit |
| read (`Db.Read` / `readTable`) | Sees own staged writes, then committed data | none |
| scan (`Db.Scan` / `scanTable`) | Prefix scan, committed merged with staged | none |
| emit event (`Host.EmitEvent` / `emitEvent`) | Custom `EVENT` payload | published after commit |

All staged writes commit atomically when the call returns a non-negative
status without trapping; on any failure nothing is applied. Changefeed
events for table writes are emitted automatically by the storage commit —
**no manual event emission is needed for row changes** (use
`emit event` only for custom, non-row payloads).

### Return status conventions (ABI)

| Status | Meaning |
|---|---|
| `0` | Success; staged writes commit |
| `1` / `0` (filter functions) | Include / exclude the event (writes still commit) |
| `< 0` | Error; everything staged is discarded |
| `-1` internal, `-2` permission denied, `-3` invalid argument, `-5` limit exceeded, `-404` no handler | Reserved codes |

Result payloads (query results, transformed events) are returned via
`host_set_result`, which the SDKs wire to your function's return value.

### Subscription filters and transforms

```csharp
Reducers.RegisterFilter("OnlyInserts",
    ev => ev.GetProperty("operation").GetString() == "INSERT");
```

```ts
registerFilter("onlyPending", (ev: JsonValue): bool =>
  ev.getString("operation") == "INSERT");
```

The event argument is
`{table, operation, offset, transaction_id, key, new_value, old_value}`.
Details: [WASM_SUBSCRIPTIONS.md](WASM_SUBSCRIPTIONS.md).

### Lifecycle hooks

Plain registrations under the reserved names `__init`,
`__client_connected`, `__client_disconnected`, `__get_initial_data`.

## WebSocket Client API

Connect to `ws://localhost:8080`. The server sends a `welcome` message
with your `client_id`. Subscription types:

### SQL subscription (server-evaluated WHERE)

```javascript
ws.send(JSON.stringify({
  type: 'sql_subscribe',
  sql: "SELECT * FROM users WHERE premium = true"
}));
// -> {"type":"sql_subscription_created", "subscription_id":"..."}
// -> {"type":"initial_state", "columns":[...], "rows":[...], ...}
//    (note: the initial snapshot is NOT where-filtered; events are)
// -> {"type":"sql_changefeed_event", "table":"users", "operation":"INSERT",
//     "key":"...", "new_value":"...", "old_value":"...", ...} per matching write
```

WHERE clauses support comparisons, `AND`/`OR`/`NOT`, parentheses, `LIKE`,
and `IS [NOT] NULL`, evaluated per event. Invalid queries are rejected
with an `error` frame. UPDATE events match if the old OR new row matches.

### All-tables subscription

```javascript
ws.send(JSON.stringify({ type: 'subscribe_to_all_tables' }));
// -> all_tables_subscription_created, initial_state_all_tables, then all events
```

### WASM subscription (module filter/transform)

```javascript
ws.send(JSON.stringify({
  type: 'wasm_subscribe',
  module_name: 'todo',
  filter_function: 'onlyPending',       // optional
  transform_function: 'addMetadata',    // optional
  tables: ['todos'],                    // optional
  include_initial_data: true            // optional (__get_initial_data)
}));
// -> wasm_subscription_created, then wasm_subscription_event messages
```

## gRPC API

Use the bundled client (`grpcurl` is not required):

```bash
# SQL
./build/instantdb_client exec "INSERT INTO users VALUES (1, 'Alice')"

# Modules
./build/instantdb_client deploy user_module ./UserModule.wasm 1.0.0
./build/instantdb_client call user_module CreateUser '["Alice", "alice@example.com"]'
./build/instantdb_client modules
./build/instantdb_client undeploy user_module
```

Services (`proto/instantdb.proto`): `SQLService`
(`Execute`, `ExecuteTransaction`, `GetStatus`) and `WasmService`
(`DeployModule`, `UndeployModule`, `ListModules`, `GetModule`,
`ExecuteReducer`). `DeployModuleRequest` carries optional
`ModuleCapabilities` (`allowed_tables`, `read_only`, `max_memory_mb`,
`timeout_ms`).

## Best Practices

1. **Validate inputs** in reducers; fail with a negative status
   (`ReducerException` / `abortCall`) so staged writes roll back.
2. **Keep durable state in tables** — module linear memory does not
   survive traps, timeouts, redeploys, or restarts.
3. **Use `host_now_ms` / `host_generate_id`** (via the SDK wrappers)
   instead of wall-clock or random APIs; time is fixed per call for
   deterministic replay.
4. **Keep filters lightweight** — they run on every matching event and
   calls into a module are serialized.
5. **Log via the SDK log functions** (`host_log`) — output lands in the
   server log; module stdout/stderr are discarded.
