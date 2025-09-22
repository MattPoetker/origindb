# InstantDB Architecture

## Overview

InstantDB is designed as a high-performance, in-memory SQL database with real-time changefeed capabilities. The architecture prioritizes simplicity, performance, and real-time data streaming.

## Core Components

### 1. Storage Engine (`src/storage/`)

The storage engine is responsible for data management, persistence, and transaction handling.

#### Key Features
- **In-Memory Storage**: All data is kept in memory for fast access
- **MVCC (Multi-Version Concurrency Control)**: Enables concurrent reads and writes
- **Write-Ahead Logging (WAL)**: Ensures durability and crash recovery
- **Schema Management**: Type-safe table and column definitions

#### Implementation Details

```cpp
class StorageEngine {
private:
    std::unordered_map<std::string, Table> tables_;
    std::shared_mutex tables_mutex_;
    std::unique_ptr<WALImpl> wal_;
    std::atomic<uint64_t> next_transaction_id_;
    StorageConfig config_;
};
```

**Thread Safety:**
- Uses `std::shared_mutex` for reader-writer locks
- Multiple concurrent readers, single writer per table
- Atomic operations for transaction ID generation

**Data Structures:**
- `std::unordered_map` for O(1) table lookup
- `std::vector` for row storage within tables
- `std::variant` for type-safe column values

### 2. Write-Ahead Log (WAL) (`src/storage/wal_impl.cpp`)

The WAL provides crash recovery and data persistence.

#### Features
- **Sequence-based Ordering**: Each entry has a unique sequence number
- **JSON Serialization**: Human-readable log format for debugging
- **Binary Data Support**: Hex encoding for binary column data
- **Crash Recovery**: Automatic replay on startup

#### WAL Entry Format

```cpp
struct WALEntry {
    uint64_t sequence;
    WALEntryType type;           // INSERT, UPDATE, DELETE, CREATE_TABLE
    uint64_t transaction_id;
    uint64_t timestamp;
    std::string table_name;
    std::string key;
    std::vector<uint8_t> data;   // Serialized row data
};
```

**JSON Serialization Example:**
```json
{
    "sequence": 1,
    "type": 1,
    "transaction_id": 12345,
    "timestamp": 1705123456789,
    "table_name": "users",
    "key": "1",
    "data": "7b22696422203a20312c20226e616d6522203a2022416c696365227d"
}
```

#### Recovery Process
1. Read all WAL entries from disk
2. Sort by sequence number
3. Replay operations in order
4. Rebuild in-memory state
5. Continue from last sequence number

### 3. SQL Engine (`src/sql/`)

The SQL engine handles query parsing, planning, and execution.

#### Current Capabilities
- **DDL**: CREATE TABLE statements
- **DML**: INSERT and SELECT statements
- **Basic Parsing**: Simple SQL syntax support
- **Type Validation**: Ensures data type consistency

#### SQL Execution Flow
```
SQL Query → Parser → Planner → Executor → Storage Engine
```

**Supported Syntax:**
```sql
CREATE TABLE users (id INT64 PRIMARY KEY, name STRING);
INSERT INTO users VALUES (1, 'Alice');
SELECT * FROM users;
```

#### Future Enhancements
- Full SQL parser using ANTLR or similar
- Query optimization
- Index utilization
- Complex joins and aggregations

### 4. Changefeed Engine (`src/changefeed/`)

The changefeed engine tracks data changes and publishes events to subscribers.

#### Features
- **Subscription Management**: Multiple subscribers per table
- **Event Filtering**: Table-based filtering (more filters planned)
- **Event Publishing**: Asynchronous event delivery
- **Metrics Tracking**: Active subscriptions and event counts

#### Event Flow
```
Storage Write → WAL Entry → Changefeed Event → WebSocket Broadcast
```

**Event Structure:**
```cpp
struct ChangefeedEvent {
    uint64_t offset;
    std::string table;
    std::string operation;        // INSERT, UPDATE, DELETE
    std::string transaction_id;
    std::vector<uint8_t> key;
    std::vector<uint8_t> old_value;
    std::vector<uint8_t> new_value;
};
```

### 5. WebSocket Server (`src/websocket/`)

The WebSocket server provides real-time data streaming to connected clients.

#### Implementation
- **Raw Socket Implementation**: No external WebSocket library dependencies
- **OpenSSL Integration**: For secure WebSocket handshake
- **Protocol Compliance**: Full WebSocket RFC 6455 support
- **Connection Management**: Automatic cleanup of disconnected clients

#### WebSocket Handshake Process
1. Client sends HTTP Upgrade request
2. Server validates WebSocket headers
3. Generate Sec-WebSocket-Accept using SHA-1 + Base64
4. Send HTTP 101 Switching Protocols response
5. Establish WebSocket connection

#### Frame Processing
```cpp
struct WebSocketFrame {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_length;
    uint32_t masking_key;
    std::vector<uint8_t> payload;
};
```

**Supported Opcodes:**
- `0x1`: Text frame (JSON messages)
- `0x8`: Close frame
- `0x9`: Ping frame
- `0xA`: Pong frame

### 6. Configuration System (`src/common/config.h`)

Centralized configuration management for all components.

```cpp
struct ServerConfig {
    StorageConfig storage;
    ChangefeedConfig changefeed;
    WebSocketConfig websocket;
    LoggingConfig logging;
};
```

## Data Flow

### Write Path
```
SQL INSERT → SQL Engine → Storage Engine → WAL → Changefeed → WebSocket
```

1. **SQL Parsing**: Parse INSERT statement
2. **Validation**: Check schema and constraints
3. **Storage Write**: Add to in-memory table
4. **WAL Write**: Persist to disk
5. **Changefeed**: Generate change event
6. **WebSocket Broadcast**: Send to connected clients

### Read Path
```
SQL SELECT → SQL Engine → Storage Engine → Result Set
```

1. **SQL Parsing**: Parse SELECT statement
2. **Table Lookup**: Find target table
3. **Data Retrieval**: Read from in-memory storage
4. **Result Formatting**: Convert to SQL result format

### Recovery Path
```
Startup → WAL Recovery → Table Reconstruction → Ready
```

1. **WAL Scan**: Read all WAL entries
2. **Sort by Sequence**: Ensure correct ordering
3. **Replay Operations**: Rebuild tables from WAL
4. **Resume Operations**: Continue from last sequence

## Threading Model

### Thread Safety Strategy
- **Reader-Writer Locks**: Allow concurrent reads
- **Atomic Operations**: For counters and sequence numbers
- **Lock-Free Structures**: Where possible for performance

### Thread Allocation
- **Main Thread**: Server lifecycle management
- **Storage Threads**: Concurrent table operations
- **WebSocket Threads**: One per connection
- **Changefeed Thread**: Event processing and publishing

### Synchronization Points
1. **Table Access**: Shared mutex per table
2. **WAL Writing**: Sequential writes with atomic sequence
3. **Changefeed Publishing**: Thread-safe event queue
4. **WebSocket Broadcasting**: Connection list protection

## Memory Management

### Memory Layout
```
┌─────────────────┐
│   Tables Map    │ ← std::unordered_map<string, Table>
├─────────────────┤
│   Table Data    │ ← std::vector<Row> per table
├─────────────────┤
│   WAL Buffer    │ ← In-memory WAL entries
├─────────────────┤
│ WebSocket Conns │ ← Active connection list
├─────────────────┤
│ Changefeed Subs │ ← Subscription registry
└─────────────────┘
```

### Memory Growth Patterns
- **Tables**: Linear growth with data volume
- **WAL**: Bounded by retention policy (future feature)
- **Connections**: Limited by system resources
- **Events**: Bounded by processing rate

### Memory Optimizations (Future)
- Data compression for large columns
- WAL compaction and archival
- Connection pooling and limits
- Event batching and compression

## Performance Characteristics

### Latency Targets
- **SQL Operations**: < 1ms for simple queries
- **WAL Writes**: < 100μs for small entries
- **Changefeed Events**: < 10ms end-to-end
- **WebSocket Delivery**: < 50ms to connected clients

### Throughput Expectations
- **Inserts**: 10,000+ ops/sec single threaded
- **Selects**: 100,000+ ops/sec with concurrent reads
- **WebSocket Connections**: 1,000+ concurrent connections
- **Event Publishing**: 10,000+ events/sec

### Scalability Limits (Current)
- **Data Size**: Limited by available RAM
- **Concurrency**: Limited by CPU cores
- **Connections**: Limited by file descriptors
- **Geographic Distribution**: Single-node only

## Fault Tolerance

### Crash Recovery
1. **WAL Integrity**: Verify all entries are valid
2. **Sequence Gaps**: Detect and handle missing entries
3. **Corruption Detection**: Validate JSON parsing
4. **Graceful Degradation**: Skip corrupted entries with warnings

### Error Handling
- **SQL Errors**: Return descriptive error messages
- **Storage Errors**: Log and attempt recovery
- **Network Errors**: Automatic client reconnection
- **Memory Errors**: Graceful shutdown with data preservation

### Monitoring
- **Health Checks**: Regular system status validation
- **Metrics Collection**: Performance and error statistics
- **Logging**: Structured logging with configurable levels
- **Alerting**: Error threshold monitoring (future feature)

## Security Considerations

### Current Security
- **No Authentication**: All operations are unrestricted
- **No Encryption**: Data transmitted in plain text
- **No Authorization**: No access control mechanisms
- **Local Filesystem**: WAL stored with system permissions

### Planned Security Features
- **TLS/SSL**: Encrypted WebSocket connections
- **Authentication**: User-based access control
- **Authorization**: Table and operation-level permissions
- **Audit Logging**: Security event tracking

## Future Architecture Evolution

### Near-term (v0.2.0)
- **gRPC API**: High-performance SQL interface
- **Configuration Files**: YAML/JSON configuration
- **Enhanced SQL**: UPDATE, DELETE operations
- **Basic Authentication**: Simple user management

### Medium-term (v0.3.0)
- **Raft Consensus**: Multi-node clustering
- **Data Replication**: Automatic failover
- **Load Balancing**: Client connection distribution
- **Backup/Restore**: Point-in-time recovery

### Long-term (v1.0.0)
- **WASM Runtime**: User-defined functions
- **Complete SQL**: Full SQL:2016 compliance
- **Advanced Indexing**: B-tree and hash indexes
- **Query Optimization**: Cost-based query planning