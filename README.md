# InstantDB

A high-performance, in-memory SQL database with real-time changefeeds and WebSocket streaming capabilities.

## Current Features (v0.1.0)

- **In-Memory Storage Engine** with MVCC (Multi-Version Concurrency Control)
- **Write-Ahead Logging (WAL)** for persistence and crash recovery
- **SQL Query Engine** for standard SQL operations (CREATE TABLE, INSERT, SELECT)
- **Real-Time Changefeed Engine** for live data updates
- **WebSocket Server** for streaming changes to connected clients
- **Production Server** with CLI configuration and graceful shutdown
- **Thread-Safe Operations** with concurrent read/write access

## Planned Features (Future Versions)

- **Distributed Consensus**: Raft-based replication for high availability
- **WASM Modules**: Execute user-defined WASM modules within transactions
- **Complete SQL Interface**: Full SQL:2016 compliance with extensions
- **Strong Consistency**: Linearizable reads and writes across cluster

## Architecture

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   SQL Engine    │───▶│  Storage Engine  │───▶│  WAL (Persist)  │
└─────────────────┘    └──────────────────┘    └─────────────────┘
         │                       │                       │
         │                       ▼                       │
         │              ┌──────────────────┐             │
         │              │ Changefeed Engine│◀────────────┘
         │              └──────────────────┘
         │                       │
         ▼                       ▼
┌─────────────────┐    ┌──────────────────┐
│  Client Query   │    │ WebSocket Server │
│    Response     │    │   (Port 8080)    │
└─────────────────┘    └──────────────────┘
                                │
                                ▼
                       ┌──────────────────┐
                       │  Connected       │
                       │  WebSocket       │
                       │  Clients         │
                       └──────────────────┘
```

## Quick Start

### Prerequisites

- **CMake 3.20+**
- **C++20 compiler** (GCC 10+ or Clang 12+)
- **OpenSSL** (for WebSocket handshake)

The following dependencies are automatically fetched during build:
- **spdlog** (logging)
- **nlohmann/json** (JSON handling)
- **fmt** (formatting)

### Building

```bash
# Generate build files
cmake -B build -S .

# Build the project
cmake --build build

# Or use make if available
make
```

### Running the Demo

```bash
# Run interactive demo (default port 8080)
./build/instantdb_demo

# Run demo on custom port
./build/instantdb_demo 9090
```

### Running the Production Server

```bash
# Start server with defaults
./build/instantdb_server

# Start on custom port
./build/instantdb_server -p 9090
./build/instantdb_server --port 9090
./build/instantdb_server 9090

# Show help
./build/instantdb_server --help
```

## Configuration

### Command Line Options

```bash
Usage: instantdb_server [OPTIONS]

Options:
  -p, --port PORT          WebSocket port (default: 8080)
  -d, --data-dir DIR       Data directory (default: ./instantdb_data)
  -l, --log-level LEVEL    Log level: trace,debug,info,warn,error (default: info)
  -c, --config FILE        Config file path (default: instantdb.conf)
  -h, --help               Show help message

Examples:
  instantdb_server                    # Start with defaults
  instantdb_server -p 9090            # Start on port 9090
  instantdb_server --port 9090        # Start on port 9090
  instantdb_server 9090               # Start on port 9090 (short form)
```

### Environment Variables

```bash
export INSTANTDB_WS_PORT=9090        # WebSocket port
export INSTANTDB_DATA_DIR=/data/db   # Data directory
export INSTANTDB_LOG_LEVEL=debug     # Log level
```

## SQL Examples

### Creating Tables

```sql
CREATE TABLE users (
    id INT64 PRIMARY KEY,
    name STRING
);
```

### Data Operations

```sql
-- Insert data
INSERT INTO users VALUES (1, 'Alice');
INSERT INTO users VALUES (2, 'Bob');
INSERT INTO users VALUES (3, 'Charlie');

-- Query data
SELECT * FROM users;
```

## WebSocket API

### Connecting to Changefeeds

Connect to the WebSocket endpoint to receive real-time changefeed events:

```javascript
// Connect to server
const ws = new WebSocket('ws://localhost:8080');

// Receive changefeed events
ws.onmessage = function(event) {
    const data = JSON.parse(event.data);
    console.log('Changefeed event:', data);

    // Example event structure:
    // {
    //   "type": "changefeed_event",
    //   "table": "users",
    //   "operation": "INSERT"
    // }
};

ws.onopen = function() {
    console.log('Connected to InstantDB WebSocket');
};

ws.onclose = function() {
    console.log('Disconnected from InstantDB WebSocket');
};
```

### Testing with curl

```bash
# Test WebSocket connection
curl --include \
     --no-buffer \
     --header "Connection: Upgrade" \
     --header "Upgrade: websocket" \
     --header "Sec-WebSocket-Key: SGVsbG8sIHdvcmxkIQ==" \
     --header "Sec-WebSocket-Version: 13" \
     http://localhost:8080/
```

## Monitoring

### Server Statistics

The server provides runtime statistics every 30 seconds:

```
📈 Server Stats: 1 tables, 3 rows, 156 bytes, 0 WS connections, 1 subscriptions, 3 events
```

### Logging

Structured logging with configurable levels:

```
[2024-01-15 10:30:45.123] [info] 🚀 Starting InstantDB Server
[2024-01-15 10:30:45.124] [info] 📦 Initializing Storage Engine...
[2024-01-15 10:30:45.125] [info] ✅ Storage Engine ready
[2024-01-15 10:30:45.126] [info] 🔍 Initializing SQL Engine...
[2024-01-15 10:30:45.127] [info] ✅ SQL Engine ready
[2024-01-15 10:30:45.128] [info] 📡 Initializing Changefeed Engine...
[2024-01-15 10:30:45.129] [info] ✅ Changefeed Engine ready
[2024-01-15 10:30:45.130] [info] 🌐 Initializing WebSocket Server...
[2024-01-15 10:30:45.131] [info] ✅ WebSocket Server ready on 0.0.0.0:8080
```

### Available Log Levels

- `trace` - Very detailed debug information
- `debug` - Debug information including WAL operations
- `info` - General information (default)
- `warn` - Warning messages
- `error` - Error messages only

## Development

### Project Structure

```
instant_db/
├── src/
│   ├── cmd/                    # Executables
│   │   ├── instantdb_demo.cpp  # Interactive demo
│   │   └── instantdb_server.cpp # Production server
│   ├── storage/                # Storage engine
│   │   ├── storage_engine.cpp  # Main storage implementation
│   │   └── wal_impl.cpp        # Write-ahead log
│   ├── sql/                    # SQL engine
│   │   └── sql_engine.cpp      # SQL parser and executor
│   ├── changefeed/             # Changefeed engine
│   │   └── changefeed_engine.cpp # Real-time change tracking
│   ├── websocket/              # WebSocket server
│   │   └── websocket_server.cpp # WebSocket protocol implementation
│   └── common/                 # Shared utilities
│       └── config.h            # Configuration structures
├── include/                    # Header files
├── docs/                       # Documentation
├── CMakeLists.txt             # Build configuration
├── PROJECT.md                 # Original project specification
└── README.md                  # This file
```

### Building from Source

```bash
# Clone repository
git clone <repository-url>
cd instant_db

# Create build directory
mkdir build && cd build

# Configure
cmake ..

# Build
make -j$(nproc)

# Run demo
./instantdb_demo

# Run production server
./instantdb_server --help
```

### Key Components

#### Storage Engine (`src/storage/storage_engine.cpp`)
- In-memory table storage with thread-safe operations
- MVCC implementation for concurrent access
- WAL integration for persistence
- Schema management and validation

#### WebSocket Server (`src/websocket/websocket_server.cpp`)
- Raw socket WebSocket implementation
- OpenSSL integration for handshake
- Real-time changefeed broadcasting
- Connection management and cleanup

#### Changefeed Engine (`src/changefeed/changefeed_engine.cpp`)
- Subscription management
- Event filtering and publishing
- Metrics tracking

#### WAL Implementation (`src/storage/wal_impl.cpp`)
- JSON-based entry serialization
- Hex encoding for binary data
- Crash recovery and replay
- Sequence number management

## Roadmap

### Current Version (v0.1.0)
- ✅ In-memory storage with WAL persistence
- ✅ Basic SQL operations (CREATE TABLE, INSERT, SELECT)
- ✅ Real-time changefeeds via WebSocket
- ✅ Production server with CLI configuration
- ✅ Thread-safe operations

### Next Version (v0.2.0)
- [ ] **gRPC API** - High-performance RPC interface for SQL operations
- [ ] **Configuration Files** - JSON/YAML configuration support
- [ ] **Enhanced SQL** - UPDATE, DELETE, complex queries
- [ ] **Authentication** - Basic user management

### Future Versions
- [ ] **Raft Consensus** - Distributed consensus for clustering
- [ ] **WASM Modules** - User-defined functions and triggers
- [ ] **Complete SQL Parser** - Full SQL:2016 compliance
- [ ] **Metrics API** - Prometheus-compatible metrics
- [ ] **Admin Interface** - Web-based administration

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make your changes
4. Add tests if applicable
5. Commit your changes (`git commit -m 'Add amazing feature'`)
6. Push to the branch (`git push origin feature/amazing-feature`)
7. Open a Pull Request

## License

[Add your license here]