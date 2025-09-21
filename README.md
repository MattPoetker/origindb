# InstantDB

A high-performance, in-memory SQL RDBMS with WASM module execution and real-time changefeeds.

## Features

- **In-Memory Storage**: Fast, in-memory table storage with WAL persistence
- **Distributed Consensus**: Raft-based replication for high availability
- **WASM Modules**: Execute user-defined WASM modules within transactions
- **SQL Interface**: Standard SQL support with extensions for modules
- **Real-time Changefeeds**: WebSocket-based change streaming
- **Strong Consistency**: Linearizable reads and writes

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                    Client Layer                       │
│  ┌─────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │  gRPC   │  │WebSocket │  │  Admin CLI       │   │
│  └─────────┘  └──────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────┐
│                    SQL Engine                         │
│  ┌─────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ Parser  │  │ Planner  │  │    Executor      │   │
│  └─────────┘  └──────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────┐
│              Transaction & Storage Layer              │
│  ┌─────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │MemTable │  │   WAL    │  │   Snapshots      │   │
│  └─────────┘  └──────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────┐
│                 Consensus Layer (Raft)                │
│  ┌─────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ Leader  │  │ Follower │  │  Log Replication │   │
│  └─────────┘  └──────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────┐
│                   WASM Runtime                        │
│  ┌─────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │Wasmtime │  │ Host API │  │  Module Store    │   │
│  └─────────┘  └──────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────┘
```

## Building

### Prerequisites

- CMake 3.20+
- C++20 compiler (GCC 10+, Clang 12+)
- Protobuf
- gRPC
- Boost
- spdlog

### Build Steps

```bash
# Create build directory
mkdir build && cd build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
make -j$(nproc)

# Run tests
make test

# Install
sudo make install
```

## Usage

### Starting a Single Node

```bash
./instantdb --data-dir ./data --grpc-addr 0.0.0.0:50051 --ws-addr 0.0.0.0:8080
```

### Starting a Cluster

```bash
# Node 1 (initial leader)
./instantdb --node-id node1 --data-dir ./data1 --grpc-addr 0.0.0.0:50051

# Node 2
./instantdb --node-id node2 --data-dir ./data2 --grpc-addr 0.0.0.0:50052 --join node1:9001

# Node 3
./instantdb --node-id node3 --data-dir ./data3 --grpc-addr 0.0.0.0:50053 --join node1:9001
```

## SQL Examples

### Creating Tables

```sql
CREATE TABLE users (
    id INT PRIMARY KEY,
    name VARCHAR(100),
    email VARCHAR(100),
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE INDEX idx_users_email ON users(email);
```

### WASM Module Operations

```sql
-- Upload and install a module
CREATE MODULE transfer_funds FROM 'path/to/module.wasm'
WITH (
    capabilities = 'read_write',
    max_memory = 268435456,
    allowed_tables = 'accounts,transactions'
);

-- Call a module
CALL transfer_funds('account1', 'account2', 100.00);
```

### Subscriptions

```sql
-- Create a subscription for user changes
CREATE SUBSCRIPTION user_updates
FOR SELECT * FROM users WHERE country = 'US';
```

## WebSocket Changefeed Protocol

```javascript
// Connect to WebSocket
const ws = new WebSocket('ws://localhost:8080/changefeed');

// Subscribe
ws.send(JSON.stringify({
    action: 'subscribe',
    subscription: 'user_updates',
    sql: 'SELECT * FROM users WHERE active = true',
    start_offset: 0
}));

// Receive events
ws.onmessage = (event) => {
    const data = JSON.parse(event.data);
    console.log('Change event:', data);

    // Acknowledge
    ws.send(JSON.stringify({
        action: 'ack',
        subscription: 'user_updates',
        offset: data.offset
    }));
};
```

## Configuration

See `config.yaml` for full configuration options:

```yaml
node:
  id: node1
  data_dir: ./data

storage:
  max_memory_bytes: 4294967296  # 4GB
  wal_buffer_size: 67108864     # 64MB
  snapshot_interval: 10000

raft:
  election_timeout_ms: 3000
  heartbeat_interval_ms: 1000

wasm:
  max_instances: 100
  instance_memory_limit: 268435456  # 256MB
  max_execution_time_ns: 5000000000 # 5 seconds

grpc:
  listen_address: 0.0.0.0:50051
  max_message_size: 104857600  # 100MB

websocket:
  listen_address: 0.0.0.0:8080
  max_connections: 10000
```

## Development

### Project Structure

```
instant_db/
├── include/        # Header files
├── src/           # Implementation files
│   ├── cmd/       # Main entry point
│   ├── storage/   # Storage engine
│   ├── raft/      # Consensus layer
│   ├── sql/       # SQL engine
│   ├── wasm/      # WASM runtime
│   ├── modules/   # Module management
│   ├── net/       # Network servers
│   ├── changefeed/# Changefeed engine
│   └── admin/     # Admin services
├── proto/         # Protocol buffers
├── tests/         # Unit tests
└── deploy/        # Deployment configs
```

### Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests
5. Submit a pull request

## License

MIT License - see LICENSE file for details