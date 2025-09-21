# InstantDB Prototype Demo

This demo showcases the minimal end-to-end flow from the PROJECT.md implementation plan:

**INSERT → WAL → Raft → Apply → Changefeed → WebSocket Client**

## What the Demo Shows

The prototype demonstrates:

1. **SQL Parsing**: Simple INSERT statement parsing
2. **Storage Engine**: In-memory table with basic CRUD operations
3. **WAL**: Write-ahead logging for durability
4. **Raft Stub**: Local application of transactions (leader-only mode)
5. **Changefeed**: Event streaming to subscribers
6. **WebSocket Server**: Real-time delivery to connected clients

## Running the Demo

### Build

```bash
./build.sh --debug
```

### Run Prototype Demo

```bash
cd build
./instantdb_demo
```

### Expected Output

```
🚀 InstantDB Prototype Demo
==============================

📦 Initializing Storage Engine...
✅ Storage Engine ready

🗂️  Creating demo table 'users'...
✅ Table 'users' created

🔍 Initializing SQL Engine...
✅ SQL Engine ready

📡 Initializing Changefeed Engine...
✅ Changefeed Engine ready

🌐 Initializing WebSocket Server...
✅ WebSocket Server listening on 127.0.0.1:8080

🎯 Starting End-to-End Demo Flow
=================================

💻 Executing SQL: INSERT INTO users VALUES (1, 'Alice')
✅ SQL executed successfully
📡 Change event published
📤 Changefeed Event Received:
   Subscription: sub-1
   Offset: 1
   Table: users
   Operation: INSERT
   Key: 1

---
💻 Executing SQL: INSERT INTO users VALUES (2, 'Bob')
✅ SQL executed successfully
📡 Change event published
📤 Changefeed Event Received:
   Subscription: sub-1
   Offset: 2
   Table: users
   Operation: INSERT
   Key: 2

---
💻 Executing SQL: INSERT INTO users VALUES (3, 'Charlie')
✅ SQL executed successfully
📡 Change event published
📤 Changefeed Event Received:
   Subscription: sub-1
   Offset: 3
   Table: users
   Operation: INSERT
   Key: 3

---

📊 Current table contents:
Users table:
  Key: 1, id: 1, name: Alice
  Key: 2, id: 2, name: Bob
  Key: 3, id: 3, name: Charlie

📈 System Statistics:
  Tables: 1
  Rows: 3
  Storage bytes: 156
  Events published: 3
  Active subscriptions: 1
  WebSocket connections: 0

🎉 Demo completed successfully!

To connect a WebSocket client, use:
ws://127.0.0.1:8080

Example subscription message:
{"action":"subscribe","subscription":"demo","sql":"SELECT * FROM users","start_offset":0}

Press Enter to shutdown...
```

## Testing WebSocket Client

While the demo is running, you can connect a WebSocket client:

### Using `wscat` (Node.js)

```bash
npm install -g wscat
wscat -c ws://127.0.0.1:8080
```

Then send a subscription message:

```json
{"action":"subscribe","subscription":"test","sql":"SELECT * FROM users","start_offset":0}
```

### Using JavaScript in Browser

```javascript
const ws = new WebSocket('ws://127.0.0.1:8080');

ws.onopen = function() {
    console.log('Connected');

    // Subscribe to user changes
    ws.send(JSON.stringify({
        action: 'subscribe',
        subscription: 'user_updates',
        sql: 'SELECT * FROM users',
        start_offset: 0
    }));
};

ws.onmessage = function(event) {
    const data = JSON.parse(event.data);
    console.log('Received:', data);

    if (data.type === 'event') {
        console.log(`Change: ${data.op} on ${data.table} key ${data.key}`);
    }
};
```

## Architecture Flow

The demo shows this flow:

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│   SQL       │───▶│   Storage   │───▶│    WAL      │
│   Parser    │    │   Engine    │    │   Logger    │
└─────────────┘    └─────────────┘    └─────────────┘
                            │
                            ▼
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│ WebSocket   │◀───│ Changefeed  │◀───│  Raft Stub  │
│   Client    │    │   Engine    │    │   (Apply)   │
└─────────────┘    └─────────────┘    └─────────────┘
```

1. **SQL Parser** → Parses `INSERT INTO users VALUES (1, 'Alice')`
2. **Storage Engine** → Stores row in in-memory table
3. **WAL Logger** → Persists transaction to disk
4. **Raft Stub** → Applies transaction (leader-only mode)
5. **Changefeed Engine** → Generates change event
6. **WebSocket Client** → Receives real-time notification

## Prototype Limitations

This is a minimal prototype demonstrating the core flow. Production limitations:

- **Single Node**: No real Raft clustering (leader-only mode)
- **Simple SQL**: Only basic INSERT/SELECT parsing
- **No WASM**: Module execution not yet implemented
- **No Security**: No authentication or authorization
- **Memory Only**: Limited persistence (WAL only)
- **No Sharding**: Single-node storage only

## Next Steps

To move from prototype to production:

1. **Integrate NuRaft**: Replace stub with real Raft consensus
2. **Full SQL Parser**: Use ANTLR or similar for complete SQL support
3. **WASM Runtime**: Add Wasmtime integration for modules
4. **Authentication**: Add user management and security
5. **Persistence**: Implement proper snapshotting and recovery
6. **Clustering**: Multi-node deployment and sharding
7. **Performance**: Optimizations for production workloads

## Troubleshooting

### Build Issues

```bash
# Clean build
rm -rf build
./build.sh --clean --debug

# Check dependencies
cmake --version  # Requires 3.20+
protoc --version # Protocol buffers
```

### Runtime Issues

```bash
# Check data directory permissions
ls -la /tmp/instantdb_demo

# Check WebSocket port availability
netstat -an | grep 8080

# Enable debug logging
export SPDLOG_LEVEL=debug
./instantdb_demo
```