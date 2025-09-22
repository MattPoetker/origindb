# InstantDB CLI Tool Guide

The InstantDB CLI tool (`instantdb`) provides a comprehensive command-line interface for managing InstantDB servers, WASM modules, databases, and development workflows.

## Installation

Build the CLI tool:
```bash
cmake --build build --target instantdb
```

The CLI binary will be available at `./build/instantdb`.

## Overview

The CLI tool provides the following main commands:

- `init` - Initialize new InstantDB projects with WASM modules
- `server` - Manage the InstantDB server (start/stop/status)
- `logs` - View server logs with tail functionality
- `module` - Manage WASM modules (init/build/deploy/list)
- `database` - Manage databases (create/list/drop/backup/restore)
- `exec` - Execute reducer functions and SQL queries
- `build` - Build project components

## Command Reference

### Project Initialization

Initialize a new InstantDB project with WASM module support:

```bash
# Create a Rust-based project (default)
instantdb init my_project

# Create a C# project
instantdb init --lang csharp my_server_module

# Create a JavaScript/AssemblyScript project
instantdb init --lang javascript chat_app

# Supported languages: rust, csharp, javascript, go, cpp
```

**Features:**
- Creates complete project structure with config files
- Sets up WASM module templates for chosen language
- Generates build scripts and development environment
- Creates `.gitignore` and documentation

### Server Management

Control the InstantDB server:

```bash
# Start server (foreground)
instantdb server start

# Start server as daemon
instantdb server start --daemon

# Start with custom configuration
instantdb server start --port 9090 --websocket-port 8086 --config ./my-config.toml

# Stop server
instantdb server stop

# Restart server
instantdb server restart

# Check server status
instantdb server status
```

**Server Options:**
- `--port, -p` - HTTP server port (default: 8080)
- `--websocket-port` - WebSocket port (default: 8085)
- `--grpc-port` - gRPC port (default: 50051)
- `--config, -c` - Configuration file path
- `--data-dir` - Data directory path
- `--log-level, -l` - Log level (debug/info/warn/error)
- `--daemon, -d` - Run as background daemon

### Log Viewing

View and follow server logs:

```bash
# View last 50 lines (default)
instantdb logs

# View last 100 lines
instantdb logs --lines 100

# Follow logs in real-time
instantdb logs --follow

# View stderr logs
instantdb logs --stderr

# View specific log file
instantdb logs --file ./logs/custom.log
```

### WASM Module Management

Manage WASM modules for server-side logic:

```bash
# Initialize new module
instantdb module init auth_module
instantdb module init --lang rust user_service

# Build modules
instantdb module build                    # Build all modules
instantdb module build auth_module        # Build specific module

# Deploy modules
instantdb module deploy --all             # Deploy all modules
instantdb module deploy auth_module       # Deploy specific module

# List modules
instantdb module list

# Remove module
instantdb module remove auth_module
```

**Module Languages:**
- **Rust** - Full WASM support with `wasm-bindgen`
- **C#** - .NET 8 with AOT compilation to WASM
- **JavaScript** - AssemblyScript for WASM compilation
- **Go** - WASM compilation with `GOOS=wasip1`
- **C++** - Emscripten-based WASM compilation

### Database Operations

Manage databases and perform administrative tasks:

```bash
# Create database
instantdb database create myapp

# Create isolated database
instantdb database create --isolated testdb

# List databases
instantdb database list

# Drop database
instantdb database drop myapp

# Backup database
instantdb database backup myapp ./backups/myapp_backup.db

# Restore database
instantdb database restore myapp ./backups/myapp_backup.db
```

### Reducer Execution

Execute WASM reducer functions and SQL queries:

```bash
# Execute reducer with JSON input
instantdb exec create_user --input '{"name":"John","email":"john@example.com"}'

# Execute reducer with input from file
instantdb exec process_order --file order_data.json

# Execute SQL query
instantdb exec --sql "SELECT * FROM users LIMIT 10"

# Execute on specific server
instantdb exec get_user --server localhost:50052 --input '{"id":123}'
```

### Build System

Build project components:

```bash
# Build all components
instantdb build

# Build specific targets
instantdb build server               # Build server only
instantdb build client               # Build client only

# Release build
instantdb build --release

# Parallel build
instantdb build --jobs 8

# Clean build directory
instantdb build clean
```

## Configuration

### Environment Variables

The CLI tool respects these environment variables:

- `INSTANTDB_CONFIG_PATH` - Default config file path
- `INSTANTDB_DATA_DIR` - Default data directory
- `INSTANTDB_LOG_LEVEL` - Default log level

### Configuration Files

Projects can use TOML configuration files:

```toml
[server]
port = 8080
websocket_port = 8085
grpc_port = 50051
data_dir = "./data"
log_level = "info"
log_file = "./logs/instantdb.log"

[database]
name = "myapp"
wal_enabled = true
max_connections = 100

[wasm]
modules_dir = "./modules"
max_memory = "128MB"
timeout = "30s"
```

## Development Workflow

### Typical Development Flow

1. **Initialize Project:**
   ```bash
   instantdb init --lang rust my_realtime_app
   cd my_realtime_app
   ```

2. **Start Development Server:**
   ```bash
   instantdb server start --daemon
   ```

3. **Develop WASM Modules:**
   ```bash
   # Edit ./modules/main/src/lib.rs
   instantdb module build main
   instantdb module deploy main
   ```

4. **Monitor and Debug:**
   ```bash
   instantdb logs --follow
   instantdb server status
   ```

5. **Test Functionality:**
   ```bash
   instantdb exec my_reducer --input '{"test": "data"}'
   instantdb exec --sql "SELECT * FROM my_table"
   ```

### Multi-Language Projects

You can have modules in different languages within the same project:

```bash
# Create modules in different languages
instantdb module init --lang rust auth_service
instantdb module init --lang csharp payment_service
instantdb module init --lang javascript notification_service

# Build all modules
instantdb module build --all

# Deploy all modules
instantdb module deploy --all
```

## Troubleshooting

### Common Issues

1. **Server won't start:**
   ```bash
   instantdb server status           # Check if already running
   instantdb logs --stderr          # Check error logs
   ```

2. **Module build fails:**
   ```bash
   cd modules/module_name
   ./build.sh                       # Run build script directly
   ```

3. **CLI not found:**
   ```bash
   cmake --build build --target instantdb  # Rebuild CLI
   ```

### Debug Mode

Enable verbose logging for troubleshooting:

```bash
instantdb --verbose server start
instantdb --verbose module build
```

## Integration with Development Tools

### IDE Setup

The CLI generates `compile_commands.json` for IDE integration:

```bash
instantdb build                     # Generates compile_commands.json
```

### CI/CD Integration

Example CI workflow:

```bash
# In CI/CD pipeline
instantdb build --release
instantdb module build --all
instantdb database create test_db
# Run tests...
instantdb database drop test_db
```

## API Reference

For programmatic access, the CLI uses the same gRPC and WebSocket APIs as the server. You can also interact directly with:

- **gRPC API:** `localhost:50051` (default)
- **WebSocket API:** `localhost:8085` (default)
- **HTTP API:** `localhost:8080` (default)

## Examples

See the `examples/` directory for complete project examples using the CLI tool.

---

For more information, use `instantdb <command> --help` for detailed command documentation.