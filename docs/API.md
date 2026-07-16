# InstantDB API Reference

## WebSocket API

### Connection

**Endpoint:** `ws://localhost:8080`

On connect the server sends a welcome message:

```json
{
  "type": "welcome",
  "client_id": "client_12345_67890",
  "server_version": "0.1.0",
  "features": ["changefeed", "wasm_subscriptions"]
}
```

Clients then create subscriptions; events are delivered per subscription
(there is no unsolicited broadcast).

### Subscription messages

#### SQL subscription

```json
{"type": "sql_subscribe", "sql": "SELECT * FROM users WHERE name = 'Alice'"}
```

Responses, in order:

1. Confirmation:
```json
{"type": "sql_subscription_created", "subscription_id": "sql_sub_1",
 "client_id": "...", "sql": "..."}
```
2. Initial snapshot (current table contents; **not** WHERE/column
   filtered — the SQL layer doesn't support that yet):
```json
{"type": "initial_state", "subscription_id": "sql_sub_1", "sql": "...",
 "columns": [...], "rows": [...], "rows_count": 2, "execution_time_ms": 0.4}
```
3. Change events, filtered per subscription:

```json
{
  "type": "sql_changefeed_event",
  "subscription_id": "sql_sub_1",
  "offset": 42,
  "table": "users",
  "operation": "INSERT",
  "transaction_id": "...",
  "timestamp": 1705123456789,
  "key": "1",
  "new_value": "{...row JSON...}",
  "old_value": "{...previous row JSON...}"
}
```

- `operation` is `INSERT`, `UPDATE`, `DELETE`, or `EVENT` (custom
  module-emitted events).
- `old_value` is present on UPDATE/DELETE.
- WHERE clauses are evaluated **per event**: comparisons (`=`, `!=`, `<>`,
  `<`, `<=`, `>`, `>=`), `AND`/`OR`/`NOT`, parentheses, `LIKE`,
  `IS [NOT] NULL`. UPDATE events match if the old OR new row matches.
- Column projection (`SELECT id, name FROM ...`) filters the columns
  inside event values.
- Invalid SQL or an unparsable WHERE clause rejects the subscription with
  an `error` frame.

#### All-tables subscription

```json
{"type": "subscribe_to_all_tables"}
```

Confirmed with `all_tables_subscription_created`, followed by
`initial_state_all_tables` (a `tables` object with columns/rows/rows_count
per table, plus `tables_count`), then every change event.

#### WASM subscription

```json
{"type": "wasm_subscribe", "module_name": "todo",
 "filter_function": "onlyPending", "transform_function": "addMetadata",
 "tables": ["todos"], "include_initial_data": true}
```

Confirmed with `wasm_subscription_created`; events arrive as
`wasm_subscription_event`. See
[WASM_SUBSCRIPTIONS.md](WASM_SUBSCRIPTIONS.md).

#### Errors

```json
{"type": "error", "message": "..."}
{"type": "initial_state_error", "subscription_id": "...", "message": "..."}
```

### Example client

```javascript
const ws = new WebSocket('ws://localhost:8080');

ws.onopen = () => {
    ws.send(JSON.stringify({ type: 'sql_subscribe',
                             sql: 'SELECT * FROM users' }));
};

ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    switch (msg.type) {
        case 'welcome':
            console.log('client id:', msg.client_id);
            break;
        case 'initial_state':
            console.log('current rows:', msg.rows);
            break;
        case 'sql_changefeed_event':
            console.log(msg.operation, 'on', msg.table, '->', msg.new_value);
            break;
        case 'error':
            console.error(msg.message);
            break;
    }
};
```

Reconnection is the client's responsibility — subscriptions do not survive
a dropped connection or a server restart (data does, via the WAL).

## SQL Interface

SQL executes over gRPC (`SQLService.Execute`, default port 50051) — use
`instantdb_client exec "SQL"` or `instantdb_client interactive`.

### Supported operations

```sql
CREATE TABLE users (id INT64 PRIMARY KEY, name STRING);
INSERT INTO users VALUES (1, 'Alice');
SELECT * FROM users;
UPDATE users SET name='Alicia' WHERE id=1;   -- simplified: WHERE key=value only
```

### Current SQL limitations (regex-based parser)

- `SELECT` ignores WHERE clauses and column projection (full table is
  returned). Use WebSocket subscriptions for server-side filtering.
- `DELETE` is unimplemented.
- `CREATE TABLE` currently ignores the column definitions you write.
- No JOINs, aggregates, or prepared statements.
- Identifiers preserve case (`Users` ≠ `users`).
- Data types: `INT64`, `STRING` (plus doubles/bools/bytes at the storage
  layer).

Tables are also created implicitly when a WASM module first commits a
write to a new table.

## WASM Module Management API

Modules are managed over gRPC (`WasmService`): `DeployModule`,
`UndeployModule`, `ListModules`, `GetModule`, `ExecuteReducer`. Use
`instantdb_client deploy/undeploy/modules/call` or call the service
directly. See [../WASM_MODULES.md](../WASM_MODULES.md) and
[GRPC_API.md](GRPC_API.md).

## Error Handling

### WebSocket

1. **Connection refused** — server not running / wrong port / firewall.
2. **Handshake failure** — proxy interference or protocol version.
3. **Unexpected disconnect** — implement reconnect + re-subscribe.

### SQL

1. **Table not found** — check spelling *and case* (identifiers are
   case-sensitive).
2. **Parsing error** — the parser is regex-based and strict about shape;
   check quoting.

## Limitations (current version)

### Security
- No authentication, authorization, or TLS on either API.

### Storage
- Single-node only; no clustering, no backup/restore tooling.

### WebSocket
- No message acknowledgment or replay from arbitrary offsets for SQL
  subscriptions.

## See also

- [GRPC_API.md](GRPC_API.md) — gRPC service reference
- [WASM_ABI.md](WASM_ABI.md) — module ABI
- [../QA_TESTING_GUIDE.md](../QA_TESTING_GUIDE.md) — testable behavior,
  message-by-message
