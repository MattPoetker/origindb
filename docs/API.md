# InstantDB API Reference

## WebSocket API

### Connection

Connect to the WebSocket server to receive real-time changefeed events.

**Endpoint:** `ws://localhost:8080`

### Connection Flow

1. **Establish Connection**
   ```javascript
   const ws = new WebSocket('ws://localhost:8080');
   ```

2. **Handle Connection Events**
   ```javascript
   ws.onopen = function(event) {
       console.log('Connected to InstantDB WebSocket server');
   };

   ws.onclose = function(event) {
       console.log('Connection closed:', event.code, event.reason);
   };

   ws.onerror = function(error) {
       console.error('WebSocket error:', error);
   };
   ```

3. **Receive Changefeed Events**
   ```javascript
   ws.onmessage = function(event) {
       try {
           const data = JSON.parse(event.data);
           handleChangefeedEvent(data);
       } catch (e) {
           console.error('Failed to parse message:', e);
       }
   };
   ```

### Event Format

Changefeed events are sent as JSON messages with the following structure:

```json
{
    "type": "changefeed_event",
    "table": "users",
    "operation": "INSERT"
}
```

#### Fields

- **type**: Always `"changefeed_event"` for changefeed notifications
- **table**: Name of the table that was modified
- **operation**: Type of operation performed (`INSERT`, `UPDATE`, `DELETE`)

### Example Implementation

```javascript
class InstantDBClient {
    constructor(url = 'ws://localhost:8080') {
        this.url = url;
        this.ws = null;
        this.reconnectInterval = 5000;
        this.maxReconnectAttempts = 10;
        this.reconnectAttempts = 0;
    }

    connect() {
        this.ws = new WebSocket(this.url);

        this.ws.onopen = (event) => {
            console.log('Connected to InstantDB');
            this.reconnectAttempts = 0;
            this.onConnected?.(event);
        };

        this.ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                this.handleMessage(data);
            } catch (e) {
                console.error('Failed to parse message:', e);
            }
        };

        this.ws.onclose = (event) => {
            console.log('Connection closed:', event.code);
            this.onDisconnected?.(event);
            this.scheduleReconnect();
        };

        this.ws.onerror = (error) => {
            console.error('WebSocket error:', error);
            this.onError?.(error);
        };
    }

    handleMessage(data) {
        if (data.type === 'changefeed_event') {
            this.onChangefeedEvent?.(data);
        } else {
            console.log('Unknown message type:', data.type);
        }
    }

    scheduleReconnect() {
        if (this.reconnectAttempts < this.maxReconnectAttempts) {
            this.reconnectAttempts++;
            console.log(`Reconnecting in ${this.reconnectInterval}ms (attempt ${this.reconnectAttempts})`);
            setTimeout(() => this.connect(), this.reconnectInterval);
        } else {
            console.error('Max reconnection attempts reached');
        }
    }

    disconnect() {
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    // Event handlers (override these)
    onConnected(event) {}
    onDisconnected(event) {}
    onError(error) {}
    onChangefeedEvent(event) {}
}

// Usage
const client = new InstantDBClient();

client.onChangefeedEvent = (event) => {
    console.log('Table changed:', event.table, event.operation);

    // Handle different operations
    switch (event.operation) {
        case 'INSERT':
            console.log('New record added to', event.table);
            break;
        case 'UPDATE':
            console.log('Record updated in', event.table);
            break;
        case 'DELETE':
            console.log('Record deleted from', event.table);
            break;
    }
};

client.connect();
```

## SQL Interface (Demo Only)

The current version provides basic SQL operations through the demo interface.

### Supported Operations

#### CREATE TABLE

```sql
CREATE TABLE table_name (
    column_name data_type [PRIMARY KEY],
    ...
);
```

**Supported Data Types:**
- `INT64` - 64-bit integer
- `STRING` - Variable-length string

**Example:**
```sql
CREATE TABLE users (
    id INT64 PRIMARY KEY,
    name STRING
);
```

#### INSERT

```sql
INSERT INTO table_name VALUES (value1, value2, ...);
```

**Example:**
```sql
INSERT INTO users VALUES (1, 'Alice');
INSERT INTO users VALUES (2, 'Bob');
```

#### SELECT

```sql
SELECT * FROM table_name;
```

**Example:**
```sql
SELECT * FROM users;
```

### SQL Execution (Demo)

SQL commands are executed through the demo application:

```cpp
// C++ API (demo only)
auto result = sql_engine->Execute("SELECT * FROM users");

if (result.success) {
    for (const auto& row : result.rows) {
        // Process row data
    }
} else {
    std::cerr << "Query failed: " << result.error << std::endl;
}
```

## Error Handling

### WebSocket Errors

Common WebSocket connection issues:

1. **Connection Refused**
   - Ensure server is running
   - Check port availability
   - Verify firewall settings

2. **Handshake Failure**
   - Verify WebSocket protocol version
   - Check for proxy interference

3. **Unexpected Disconnection**
   - Implement reconnection logic
   - Handle network interruptions gracefully

### SQL Errors

Common SQL execution errors:

1. **Table Not Found**
   - Verify table exists
   - Check table name spelling

2. **Schema Mismatch**
   - Ensure column types match
   - Verify primary key constraints

3. **Parsing Error**
   - Check SQL syntax
   - Ensure proper quoting for strings

## Performance Considerations

### WebSocket

- **Connection Pooling**: Reuse connections when possible
- **Message Batching**: Consider batching small messages
- **Heartbeat**: Implement ping/pong for connection health

### SQL Operations

- **Prepared Statements**: Not yet implemented (future feature)
- **Batch Inserts**: Consider multiple INSERT statements
- **Indexing**: Currently only primary key indexing supported

## Limitations (v0.1.0)

### SQL
- No UPDATE or DELETE operations
- No JOIN operations
- No aggregate functions (COUNT, SUM, etc.)
- No complex WHERE clauses
- Limited data types

### WebSocket
- No authentication
- No message acknowledgment
- No subscription filtering

### Storage
- No data compression
- No backup/restore
- Single-node only (no clustering)

## Future API Extensions

### Planned for v0.2.0
- gRPC API for SQL operations
- Enhanced SQL operations (UPDATE, DELETE)
- WebSocket subscription filtering
- Configuration API

### Planned for Future Versions
- Authentication and authorization
- Cluster management API
- Metrics and monitoring API
- WASM module management API