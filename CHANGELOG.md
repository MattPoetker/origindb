# Changelog

All notable changes to OriginDB will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Real WASM execution** via the wasmtime C API (LTS line, prebuilt, platform-selected
  by CMake). Modules are compiled and structurally validated at deploy, imports are
  allow-listed (`env.host_*` + WASI preview 1), and required exports are enforced.
  The module ABI is documented in `docs/WASM_ABI.md` (single-dispatch
  `origindb_invoke`, guest allocator, `host_set_result` payloads).
- **Host API implementation**: `host_table_read/write/delete/scan` (staged-write
  overlay with read-your-writes), `host_emit_event` (published post-commit),
  `host_now_ms` (fixed per call), `host_generate_id`, `host_log`, `host_abort`,
  `host_alloc/free`, `host_set_result`.
- **Sandboxing**: epoch-based CPU deadlines honoring the configured timeout, store
  memory limiter, per-module deploy-time capabilities (`allowed_tables`, `read_only`,
  `max_memory_mb`, `timeout_ms` — new `ModuleCapabilities` proto message).
- **Module persistence**: deployed modules are stored under `<data_dir>/modules/`
  (bytecode + sha256-verified manifest) and restored on server boot.
- **Subscription WHERE evaluation**: `sql_subscribe` WHERE clauses are parsed into a
  predicate AST and evaluated per event (comparisons, `AND`/`OR`/`NOT`, parentheses,
  `LIKE`, `IS [NOT] NULL`). Invalid predicates reject the subscription with an error.
- **Changefeed `old_value`**: UPDATE/DELETE events now carry the previous row.
- `origindb_client` subcommands: `deploy`, `undeploy`, `modules`, `call`.
- Unit tests (googletest): WASM engine (WAT fixtures), predicate evaluator,
  module store. `scripts/e2e_verify.sh` covers deploy → execute → SQL → filtered
  websocket delivery → restart persistence → undeploy.

### Changed
- **BREAKING**: the hardcoded stub reducers (`CreateUser`/`GetUsers` simulated in C++)
  are gone; reducers now require a real WASM module. `test_module.wasm`/`.hex`
  (plain-text fakes) removed.
- **BREAKING**: changefeed events are emitted once per write (by the storage commit);
  the duplicate SQL-layer emission was removed. Event counts halve for SQL writes.
- `origindb publish` deploys through the bundled `origindb_client` (grpcurl no
  longer required) and supports AssemblyScript projects alongside C#.
- gRPC WASM service returns real status codes (INVALID_ARGUMENT/NOT_FOUND/INTERNAL)
  and honest module metadata (version, sha256, deployed_at).
- SQL identifiers preserve their original case (previously forced to uppercase).
- `StorageEngine::Get` reads committed data directly (previously always returned
  empty via the transaction read stub).

### Removed
- Dead parallel architecture: `src/net/`, `src/cmd/server_main.cpp`,
  `include/wasm/wasm_runtime.h`, `include/wasm/host_api.h`, `include/modules/`,
  `src/raft/`, `include/admin/`, `CMakeLists_full.txt`, `CMakeLists_minimal.txt`.

### Planned
- Configuration file support (JSON/YAML)
- Enhanced SQL operations (DELETE, JOINs, WHERE on SELECT)
- Basic authentication system

## [0.1.0] - 2024-01-15

### Added
- **Storage Engine**
  - In-memory table storage with MVCC support
  - Thread-safe operations using shared_mutex
  - Schema management with typed columns (INT64, STRING)
  - Primary key enforcement and validation
  - Automatic statistics tracking (tables, rows, bytes)

- **Write-Ahead Logging (WAL)**
  - Crash recovery and data persistence
  - JSON-based entry serialization with hex encoding for binary data
  - Sequence-based ordering for consistent replay
  - Automatic recovery on startup with WAL replay

- **SQL Engine**
  - Basic SQL parser and executor
  - CREATE TABLE support with schema validation
  - INSERT operations with type checking
  - SELECT operations with result set management
  - Error handling and descriptive error messages

- **Changefeed Engine**
  - Real-time change tracking for all table operations
  - Subscription management with filtering support
  - Event publishing to multiple subscribers
  - Metrics tracking (active subscriptions, events published)

- **WebSocket Server**
  - Native WebSocket protocol implementation (RFC 6455)
  - OpenSSL integration for secure handshake
  - Real-time changefeed broadcasting to connected clients
  - Connection management with automatic cleanup
  - Support for text frames and control frames (ping/pong/close)

- **Production Server**
  - Command line argument parsing with comprehensive options
  - Environment variable configuration support
  - Graceful shutdown with signal handling (SIGINT, SIGTERM)
  - Structured logging with configurable levels
  - Periodic statistics reporting every 30 seconds

- **Demo Application**
  - Interactive demo showing end-to-end data flow
  - Automatic data persistence detection and recovery
  - WebSocket connection testing capabilities
  - Comprehensive example usage

- **Build System**
  - CMake-based build configuration with C++20 support
  - Automatic dependency fetching (spdlog, nlohmann/json, fmt)
  - Support for Debug and Release builds
  - Cross-platform compatibility (Linux, macOS)

- **Documentation**
  - Comprehensive README with quick start guide
  - API reference documentation
  - Architecture documentation with detailed component descriptions
  - Development guide with coding standards and debugging tips
  - Deployment guide with Docker, Kubernetes, and systemd examples

### Technical Highlights
- **Thread Safety**: Extensive use of shared_mutex for concurrent read/write access
- **Memory Management**: RAII patterns and smart pointers throughout
- **Error Handling**: Comprehensive error handling with structured logging
- **Performance**: In-memory operations with efficient serialization
- **Reliability**: WAL-based crash recovery with data integrity validation

### Configuration Options
- WebSocket port configuration (CLI: `-p`, `--port`, env: `ORIGINDB_WS_PORT`)
- Data directory configuration (CLI: `-d`, `--data-dir`, env: `ORIGINDB_DATA_DIR`)
- Log level configuration (CLI: `-l`, `--log-level`, env: `ORIGINDB_LOG_LEVEL`)
- Help system (CLI: `-h`, `--help`)

### Known Limitations
- Single-node deployment only (no clustering)
- Basic SQL operations only (no UPDATE, DELETE, JOIN)
- No authentication or authorization
- No data compression or backup utilities
- No external API beyond WebSocket
- Limited data types (INT64, STRING only)

### Dependencies
- C++20 compatible compiler (GCC 10+, Clang 12+)
- CMake 3.20+
- OpenSSL (for WebSocket handshake)
- spdlog 1.12.0 (auto-fetched)
- nlohmann/json 3.11.2 (auto-fetched)
- fmt 10.1.1 (auto-fetched)

### File Structure
```
instant_db/
├── src/
│   ├── cmd/origindb_demo.cpp          # Interactive demo application
│   ├── cmd/origindb_server.cpp        # Production server
│   ├── storage/storage_engine.cpp      # Core storage implementation
│   ├── storage/wal_impl.cpp            # Write-ahead log
│   ├── sql/sql_engine.cpp              # SQL parser and executor
│   ├── changefeed/changefeed_engine.cpp # Real-time change tracking
│   ├── websocket/websocket_server.cpp  # WebSocket protocol implementation
│   └── common/config.h                 # Configuration structures
├── include/                            # Header files
├── docs/                               # Comprehensive documentation
├── CMakeLists.txt                      # Build configuration
├── PROJECT.md                          # Original specification
├── README.md                           # Main documentation
└── CHANGELOG.md                        # This file
```

## Version History Summary

### Development Timeline
- **Phase 1**: Core storage engine and WAL implementation
- **Phase 2**: SQL engine with basic operations
- **Phase 3**: WebSocket server and changefeed integration
- **Phase 4**: Production server with CLI configuration
- **Phase 5**: Comprehensive documentation and deployment guides

### Major Milestones
1. **Storage Foundation**: Thread-safe in-memory storage with persistence
2. **Real-time Capabilities**: WebSocket-based changefeed streaming
3. **Production Readiness**: CLI configuration and graceful shutdown
4. **Developer Experience**: Comprehensive documentation and examples

### Next Major Version Preview (v0.2.0)
- gRPC API for high-performance SQL operations
- Enhanced SQL with UPDATE, DELETE, and complex queries
- Configuration file support with YAML/JSON
- Basic authentication and user management
- Improved error handling and validation
- Performance optimizations and benchmarking

### Long-term Roadmap
- **v0.3.0**: Raft consensus and multi-node clustering
- **v0.4.0**: WASM module execution within transactions
- **v1.0.0**: Full SQL:2016 compliance and production hardening