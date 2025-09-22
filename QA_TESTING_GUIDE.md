# InstantDB Complete System - Manual QA Testing Guide

## Overview
This guide provides comprehensive manual testing procedures for InstantDB, including:
- **CLI Tool**: Command-line interface for project management and operations
- **SQL Subscription System**: WebSocket-based real-time subscriptions using SQL syntax
- **Initial State Broadcast**: Testing new functionality that sends current data on subscription
- **WASM Module System**: Module development, building, and deployment
- **Server Management**: Start/stop operations and monitoring
- **Database Operations**: CRUD operations and administration

## Prerequisites

### 1. Build the Complete System
```bash
cd /Users/a12042/Development/instant_db

# Clean and build everything
cmake -B build -S .
cmake --build build

# Verify CLI tool is built
ls -la ./build/instantdb
```

### 2. Verify CLI Tool Installation
```bash
# Test CLI tool basic functionality
./build/instantdb --help
./build/instantdb --version
```

Expected output:
- ✅ CLI help menu shows all commands
- ✅ Version information displays

### 3. Testing Tools Required
- **CLI Tool**: `./build/instantdb` (primary interface)
- **WebSocket client**: wscat, websocat, or browser dev tools
- **gRPC client**: grpcurl or `./build/instantdb_client`
- **Multiple terminals**: for parallel testing and monitoring

---

## Test Cases

## Part A: CLI Tool Testing

### Test Group A1: CLI Basic Functionality

#### Test Case A1.1: CLI Help and Version Information
**Objective**: Verify CLI tool basic information commands work correctly

**Steps**:
1. Test help command:
```bash
./build/instantdb --help
```

2. Test version command:
```bash
./build/instantdb --version
```

3. Test individual command help:
```bash
./build/instantdb init --help
./build/instantdb server --help
./build/instantdb module --help
```

**Expected Results**:
- ✅ Help displays all available commands with descriptions
- ✅ Version shows "InstantDB CLI v0.1.0"
- ✅ Command-specific help shows detailed usage information
- ✅ No error messages or crashes

#### Test Case A1.2: CLI Error Handling
**Objective**: Verify CLI handles invalid input gracefully

**Steps**:
1. Test invalid command:
```bash
./build/instantdb invalid_command
```

2. Test missing required arguments:
```bash
./build/instantdb init
./build/instantdb server
```

3. Test invalid options:
```bash
./build/instantdb init --invalid-option
```

**Expected Results**:
- ✅ Clear error messages for invalid commands
- ✅ Helpful usage information displayed
- ✅ Non-zero exit codes for errors
- ✅ No crashes or undefined behavior

### Test Group A2: Project Initialization

#### Test Case A2.1: Rust Project Initialization
**Objective**: Verify CLI can create complete Rust-based projects

**Steps**:
1. Create new project:
```bash
./build/instantdb init test_rust_project
```

2. Verify project structure:
```bash
ls -la test_rust_project/
ls -la test_rust_project/modules/
ls -la test_rust_project/config/
```

3. Check generated files:
```bash
cat test_rust_project/modules/main/Cargo.toml
cat test_rust_project/modules/main/src/lib.rs
cat test_rust_project/config/instantdb.toml
```

**Expected Results**:
- ✅ Project directory created with correct structure
- ✅ Rust module template with Cargo.toml and src/lib.rs
- ✅ Configuration file with default settings
- ✅ README.md with project instructions
- ✅ .gitignore with appropriate exclusions
- ✅ Build script with execute permissions

#### Test Case A2.2: Multi-Language Project Initialization
**Objective**: Verify CLI supports all programming languages

**Steps**:
1. Test each supported language:
```bash
./build/instantdb init --lang csharp test_csharp_project
./build/instantdb init --lang javascript test_js_project
./build/instantdb init --lang go test_go_project
./build/instantdb init --lang cpp test_cpp_project
```

2. Verify language-specific files exist:
```bash
# C# project
ls test_csharp_project/modules/main/main.csproj

# JavaScript project
ls test_js_project/modules/main/package.json

# Go project
ls test_go_project/modules/main/go.mod

# C++ project
ls test_cpp_project/modules/main/CMakeLists.txt
```

**Expected Results**:
- ✅ All languages create appropriate project structure
- ✅ Language-specific configuration files present
- ✅ Build scripts tailored to each language
- ✅ Template code follows language conventions

### Test Group A3: Server Management

#### Test Case A3.1: Server Start/Stop Operations
**Objective**: Verify CLI can manage server lifecycle

**Steps**:
1. Start server using CLI:
```bash
./build/instantdb server start --port 9090
```

2. In another terminal, check server status:
```bash
./build/instantdb server status
```

3. Stop the server:
```bash
./build/instantdb server stop
```

4. Verify server stopped:
```bash
./build/instantdb server status
```

**Expected Results**:
- ✅ Server starts successfully on specified port
- ✅ Status command shows "RUNNING" with PID
- ✅ Server stops gracefully
- ✅ Status command shows "STOPPED" after shutdown

#### Test Case A3.2: Daemon Mode Operations
**Objective**: Verify CLI can run server as background daemon

**Steps**:
1. Start server as daemon:
```bash
./build/instantdb server start --daemon --port 9091
```

2. Verify daemon is running:
```bash
./build/instantdb server status
ps aux | grep instantdb_server
```

3. Check log files:
```bash
ls -la logs/
cat logs/server.out
cat logs/server.err
```

4. Stop daemon:
```bash
./build/instantdb server stop
```

**Expected Results**:
- ✅ Server starts in background and returns immediately
- ✅ PID file created in logs directory
- ✅ Log files contain server output
- ✅ Server process visible in system process list
- ✅ Daemon stops cleanly

### Test Group A4: Log Management

#### Test Case A4.1: Log Viewing and Following
**Objective**: Verify CLI log viewing functionality

**Prerequisites**: Server must be running (from previous test)

**Steps**:
1. Start server with logging:
```bash
./build/instantdb server start --daemon --port 9092
```

2. View recent logs:
```bash
./build/instantdb logs --lines 20
```

3. Test follow mode (in background):
```bash
./build/instantdb logs --follow &
LOG_PID=$!
```

4. Generate some activity:
```bash
./build/instantdb exec --sql "CREATE TABLE test_logs (id INT, data TEXT)"
./build/instantdb exec --sql "INSERT INTO test_logs VALUES (1, 'test')"
```

5. Stop follow mode:
```bash
kill $LOG_PID
```

**Expected Results**:
- ✅ Logs display recent server activity
- ✅ Follow mode shows real-time log updates
- ✅ Log entries appear when server performs operations
- ✅ No errors in log output functionality

### Test Group A5: Module Management

#### Test Case A5.1: Module Creation and Building
**Objective**: Verify CLI can create and build WASM modules

**Prerequisites**: Use one of the test projects from A2

**Steps**:
1. Navigate to test project:
```bash
cd test_rust_project
```

2. Create additional module:
```bash
../build/instantdb module init auth_service
```

3. List modules:
```bash
../build/instantdb module list
```

4. Build specific module:
```bash
../build/instantdb module build main
```

5. Build all modules:
```bash
../build/instantdb module build --all
```

6. Verify WASM files created:
```bash
ls -la modules/*.wasm
```

**Expected Results**:
- ✅ New modules created with proper structure
- ✅ Module list shows all available modules
- ✅ Build commands complete successfully
- ✅ WASM files generated in modules directory
- ✅ No compilation errors

#### Test Case A5.2: Module Deployment
**Objective**: Verify CLI can deploy modules to server

**Prerequisites**: Server running and modules built

**Steps**:
1. Deploy specific module:
```bash
../build/instantdb module deploy main
```

2. Deploy all modules:
```bash
../build/instantdb module deploy --all
```

3. Check deployment:
```bash
ls -la deployed_modules/
```

**Expected Results**:
- ✅ Modules deploy without errors
- ✅ WASM files appear in deployment directory
- ✅ Deployment status reported correctly

### Test Group A6: Database Operations

#### Test Case A6.1: Database Management
**Objective**: Verify CLI database administration commands

**Prerequisites**: Server running on known port

**Steps**:
1. Create database:
```bash
../build/instantdb database create test_db
```

2. List databases:
```bash
../build/instantdb database list
```

3. Create isolated database:
```bash
../build/instantdb database create --isolated isolated_test_db
```

4. Backup database:
```bash
../build/instantdb database backup test_db ./test_backup.db
```

5. Drop database:
```bash
../build/instantdb database drop test_db
```

**Expected Results**:
- ✅ Database creation succeeds
- ✅ Database list shows created databases
- ✅ Isolated database creates separately
- ✅ Backup operation completes
- ✅ Database drops successfully

### Test Group A7: Reducer Execution

#### Test Case A7.1: SQL Query Execution
**Objective**: Verify CLI can execute SQL commands

**Steps**:
1. Execute table creation:
```bash
../build/instantdb exec --sql "CREATE TABLE cli_test (id INT PRIMARY KEY, name TEXT)"
```

2. Insert data:
```bash
../build/instantdb exec --sql "INSERT INTO cli_test VALUES (1, 'Alice'), (2, 'Bob')"
```

3. Query data:
```bash
../build/instantdb exec --sql "SELECT * FROM cli_test"
```

**Expected Results**:
- ✅ Table creation succeeds
- ✅ Data insertion completes
- ✅ Query returns inserted data
- ✅ Results displayed in readable format

#### Test Case A7.2: Reducer Function Execution
**Objective**: Verify CLI can execute WASM reducer functions

**Prerequisites**: WASM module with reducer functions deployed

**Steps**:
1. Execute with JSON input:
```bash
../build/instantdb exec create_user --input '{"name":"Charlie","email":"charlie@example.com"}'
```

2. Execute with file input:
```bash
echo '{"id":1}' > test_input.json
../build/instantdb exec get_user --file test_input.json
```

**Expected Results**:
- ✅ Reducer execution completes successfully
- ✅ JSON input processed correctly
- ✅ File input read and processed
- ✅ Return values displayed appropriately

### Test Group A8: Build System Integration

#### Test Case A8.1: Project Building
**Objective**: Verify CLI build commands work correctly

**Steps**:
1. Clean build:
```bash
../build/instantdb build clean
```

2. Build server only:
```bash
../build/instantdb build server
```

3. Build client only:
```bash
../build/instantdb build client
```

4. Build everything:
```bash
../build/instantdb build
```

5. Release build:
```bash
../build/instantdb build --release
```

**Expected Results**:
- ✅ Clean removes build artifacts
- ✅ Individual component builds succeed
- ✅ Full build completes without errors
- ✅ Release build optimizes correctly

---

## Part B: SQL Subscription System Testing

### Test Group B1: Basic SQL Subscription Functionality

#### Test Case 1.1: Subscribe to All Columns from Single Table
**Objective**: Verify basic SQL subscription works for SELECT * queries

**Steps**:
1. Connect WebSocket client to `ws://localhost:8085`
2. Send subscription message:
```json
{
  "type": "sql_subscribe",
  "sql": "SELECT * FROM users"
}
```

**Expected Result**:
- Server responds with subscription confirmation
- Server logs show: "Created SQL subscription sql_sub_1 for client [client_id]: SELECT * FROM users"
- **🆕 Client immediately receives initial state with current table data**

#### Test Case 1.2: Subscribe to Specific Columns
**Objective**: Verify column filtering works correctly

**Steps**:
1. Connect WebSocket client to `ws://localhost:8085`
2. Send subscription message:
```json
{
  "type": "sql_subscribe",
  "sql": "SELECT id, name FROM users"
}
```

**Expected Result**:
- Server responds with subscription confirmation
- Server logs show parsed columns: id, name

#### Test Case 1.3: Invalid SQL Query Handling
**Objective**: Verify error handling for malformed SQL

**Steps**:
1. Connect WebSocket client
2. Send invalid query:
```json
{
  "type": "sql_subscribe",
  "sql": "INVALID SQL SYNTAX"
}
```

**Expected Result**:
- Server responds with error message
- Server logs show: "Failed to parse SQL subscription query"

### Test Group 2: Subscribe to All Tables API

#### Test Case 2.1: Subscribe to All Tables
**Objective**: Verify the new SubscribeToAllTables API works

**Steps**:
1. Connect WebSocket client to `ws://localhost:8085`
2. Send subscription message:
```json
{
  "type": "subscribe_to_all_tables"
}
```

**Expected Result**:
- Server responds with subscription confirmation
- Server logs show: "Created ALL_TABLES subscription sql_sub_X for client [client_id]"
- **🆕 Client immediately receives initial state for all tables with current data**

### Test Group 2.5: Initial State Broadcast Testing 🆕

#### Test Case 2.5.1: SQL Subscription Initial State Format
**Objective**: Verify initial state message format for SQL subscriptions

**Setup**:
1. Create test table with sample data:
```bash
./build/instantdb exec --sql "CREATE TABLE test_users (id INT, name TEXT, active BOOLEAN)"
./build/instantdb exec --sql "INSERT INTO test_users VALUES (1, 'Alice', true)"
./build/instantdb exec --sql "INSERT INTO test_users VALUES (2, 'Bob', false)"
```

**Steps**:
1. Connect WebSocket client to `ws://localhost:8085`
2. Send SQL subscription:
```json
{
  "type": "sql_subscribe",
  "sql": "SELECT * FROM test_users WHERE active = true"
}
```

**Expected Result**:
1. Subscription confirmation message received
2. Initial state message received with:
   - `"type": "initial_state"`
   - `"subscription_id": "sql_sub_X"`
   - `"sql": "SELECT * FROM test_users WHERE active = true"`
   - `"columns"` array with column metadata
   - `"rows"` array with current matching data
   - `"rows_count": 1` (only Alice matches)
   - `"execution_time_ms"` with query execution time

#### Test Case 2.5.2: All Tables Initial State Format
**Objective**: Verify initial state message format for all-tables subscriptions

**Setup**: Use existing tables from previous test

**Steps**:
1. Connect WebSocket client to `ws://localhost:8085`
2. Send all-tables subscription:
```json
{
  "type": "subscribe_to_all_tables"
}
```

**Expected Result**:
1. Subscription confirmation message received
2. Initial state message received with:
   - `"type": "initial_state_all_tables"`
   - `"subscription_id": "sql_sub_X"`
   - `"tables"` object containing all table data
   - `"tables_count"` with number of tables
   - Each table includes columns, rows, and rows_count

#### Test Case 2.5.3: Initial State with Empty Tables
**Objective**: Verify initial state handles empty tables correctly

**Setup**:
1. Create empty table:
```bash
./build/instantdb exec --sql "CREATE TABLE empty_table (id INT, data TEXT)"
```

**Steps**:
1. Subscribe to empty table:
```json
{
  "type": "sql_subscribe",
  "sql": "SELECT * FROM empty_table"
}
```

**Expected Result**:
- Initial state message with `"rows_count": 0`
- Empty `"rows"` array
- Valid `"columns"` metadata

#### Test Case 2.5.4: Initial State After Incremental Updates
**Objective**: Verify clients get initial state, then only incremental changes

**Setup**: Use test_users table from Test Case 2.5.1

**Steps**:
1. Subscribe to table (should receive initial state)
2. Insert new record:
```bash
./build/instantdb exec --sql "INSERT INTO test_users VALUES (3, 'Charlie', true)"
```
3. Update existing record:
```bash
./build/instantdb exec --sql "UPDATE test_users SET active = false WHERE id = 1"
```

**Expected Result**:
1. Initial state received immediately upon subscription
2. Incremental changefeed events received for INSERT and UPDATE
3. No duplicate full table data sent after initial state

### Test Group 3: Event Filtering and Broadcasting

#### Test Case 3.1: Verify Column Filtering in Events
**Objective**: Ensure only subscribed columns are sent to clients

**Setup**:
1. Start server and connect WebSocket client
2. Subscribe to specific columns:
```json
{
  "type": "sql_subscribe",
  "sql": "SELECT id, name FROM users"
}
```

**Test Steps**:
1. Insert new user via gRPC:
```bash
grpcurl -plaintext -d '{"query": "INSERT INTO users (id, name, email, age) VALUES (999, \"Test User\", \"test@example.com\", 25)"}' localhost:50051 instantdb.grpc.SQLService.Execute
```

**Expected Result**:
- WebSocket client receives changefeed event
- Event contains only `id` and `name` fields
- `email` and `age` fields are filtered out

#### Test Case 3.2: Verify Table Filtering
**Objective**: Ensure clients only receive events for subscribed tables

**Setup**:
1. Connect two WebSocket clients
2. Client 1 subscribes to "users" table
3. Client 2 subscribes to "orders" table (if exists)

**Test Steps**:
1. Insert into users table
2. Insert into orders table (or another table)

**Expected Result**:
- Client 1 only receives users table events
- Client 2 only receives orders table events

#### Test Case 3.3: All Tables Subscription Broadcasting
**Objective**: Verify ALL_TABLES subscription receives all events

**Setup**:
1. Connect WebSocket client
2. Subscribe to all tables:
```json
{
  "type": "subscribe_to_all_tables"
}
```

**Test Steps**:
1. Insert into any table
2. Update any table
3. Delete from any table

**Expected Result**:
- Client receives all changefeed events regardless of table

### Test Group 4: Multiple Client Scenarios

#### Test Case 4.1: Multiple Clients, Same Subscription
**Objective**: Verify multiple clients can subscribe to same query

**Steps**:
1. Connect 3 WebSocket clients
2. All subscribe to "SELECT * FROM users"
3. Insert new user

**Expected Result**:
- All 3 clients receive the same changefeed event
- Server logs show 3 separate subscriptions created

#### Test Case 4.2: Multiple Clients, Different Subscriptions
**Objective**: Verify selective broadcasting works with mixed subscriptions

**Setup**:
1. Client A: "SELECT id, name FROM users"
2. Client B: "SELECT * FROM users"
3. Client C: Subscribe to all tables

**Test Steps**:
1. Insert new user with id, name, email, age

**Expected Result**:
- Client A receives event with only id, name
- Client B receives event with all user fields
- Client C receives event with all user fields

### Test Group 5: Connection Lifecycle Management

#### Test Case 5.1: Client Disconnection Cleanup
**Objective**: Verify subscriptions are cleaned up when client disconnects

**Steps**:
1. Connect WebSocket client
2. Create subscription
3. Check server stats (should show 1 subscription)
4. Disconnect client abruptly
5. Check server stats after cleanup period

**Expected Result**:
- Server stats show 0 subscriptions after cleanup
- Server logs show subscription removal

#### Test Case 5.2: Server Restart Persistence
**Objective**: Verify behavior when server restarts

**Steps**:
1. Connect clients and create subscriptions
2. Stop server (Ctrl+C)
3. Restart server
4. Reconnect clients

**Expected Result**:
- Clients need to re-subscribe (subscriptions don't persist)
- Server starts clean with 0 subscriptions

### Test Group 6: Performance and Load Testing

#### Test Case 6.1: High-Frequency Events
**Objective**: Verify system handles rapid database changes

**Steps**:
1. Connect WebSocket client with subscription
2. Rapidly insert multiple records (10-20 per second)
3. Monitor client receives all events

**Expected Result**:
- All events are received by client
- No events are lost or duplicated
- Server remains stable

#### Test Case 6.2: Many Concurrent Subscriptions
**Objective**: Test system with multiple active subscriptions

**Steps**:
1. Connect 10+ WebSocket clients
2. Each creates different subscription
3. Perform database operations
4. Monitor all clients receive appropriate events

**Expected Result**:
- All clients receive correct filtered events
- No cross-client event leakage
- Server performance remains acceptable

### Test Group 7: Edge Cases and Error Conditions

#### Test Case 7.1: Empty Query Results
**Objective**: Verify behavior with tables that have no data

**Steps**:
1. Subscribe to empty table
2. Insert first record
3. Verify event is received

#### Test Case 7.2: Special Characters in Data
**Objective**: Test with edge case data values

**Steps**:
1. Subscribe to table
2. Insert records with special characters, JSON, null values
3. Verify events are correctly formatted

#### Test Case 7.3: Large Payloads
**Objective**: Test with large data records

**Steps**:
1. Subscribe to table
2. Insert record with large text field (1MB+)
3. Verify event transmission

---

## Validation Checklist

### Server Logs Verification
- [ ] Subscription creation logs appear
- [ ] Event matching logs appear (debug level)
- [ ] No error messages during normal operation
- [ ] Clean subscription removal on disconnect

### Client Event Verification
- [ ] Events arrive in real-time (< 100ms delay)
- [ ] Event format matches expected schema
- [ ] Only subscribed columns are present
- [ ] Only subscribed tables generate events

### System Health Verification
- [ ] Memory usage remains stable
- [ ] CPU usage is reasonable
- [ ] No memory leaks with long-running tests
- [ ] Server gracefully handles client disconnections

---

## Troubleshooting Common Issues

### Issue: Client not receiving events
**Check**:
- WebSocket connection is established
- Subscription was successfully created (check server logs)
- Database operations are actually occurring
- Subscription query syntax is valid

### Issue: Wrong columns in events
**Check**:
- SQL query syntax is correct
- Column names match database schema
- JSON parsing is working (no parse errors in logs)

### Issue: Server memory growth
**Check**:
- Clients are properly disconnecting
- Subscriptions are being cleaned up
- No subscription ID leaks

---

## Test Group 8: WASM Module Testing

### Test Case 8.1: WASM Module Deployment
**Objective**: Verify WASM modules can be deployed successfully

**Prerequisites**:
- Counter module C++ source available at `sdk/cpp/examples/counter_module.cpp`
- WASM build tools (if available)

**Steps**:
1. Attempt to deploy a precompiled counter module (if available):
```bash
# Deploy counter module via gRPC
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "name": "counter_app",
    "version": "1.0.0",
    "bytecode": "'"$(base64 < path/to/counter_module.wasm)"'",
    "tables": [
      {
        "name": "counters",
        "columns": [
          {"name": "id", "type": "STRING", "is_primary_key": true},
          {"name": "value", "type": "INT64"},
          {"name": "last_updated", "type": "INT64"}
        ],
        "primary_key": ["id"]
      }
    ],
    "reducers": [
      {"name": "create_counter", "param_types": ["string", "int64"], "return_type": "int32"},
      {"name": "increment_counter", "param_types": ["string", "int64"], "return_type": "int32"},
      {"name": "get_counter_value", "param_types": ["string"], "return_type": "int32"},
      {"name": "delete_counter", "param_types": ["string"], "return_type": "int32"}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.DeployModule
```

**Expected Result**:
- Server responds with success and module_id
- Server logs show: "Loaded WASM module: counter_app"
- Server stats show: "1 loaded modules"

#### Test Case 8.2: List Deployed Modules
**Objective**: Verify module listing functionality

**Steps**:
1. List all deployed modules:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  localhost:50051 instantdb.grpc.WasmService.ListModules
```

**Expected Result**:
- Response includes deployed counter_app module
- Module info shows correct name, version, tables, and reducers

#### Test Case 8.3: Get Specific Module Information
**Objective**: Verify individual module details can be retrieved

**Steps**:
1. Get counter module details:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{"name": "counter_app"}' \
  localhost:50051 instantdb.grpc.WasmService.GetModule
```

**Expected Result**:
- Response contains detailed module information
- Tables and reducers match deployment specification

### Test Group 9: WASM Reducer Execution

#### Test Case 9.1: Create Counter Reducer
**Objective**: Verify counter creation via WASM reducer

**Steps**:
1. Execute create_counter reducer:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "create_counter",
    "sender_identity": "test_user",
    "args": [
      {"string_value": "test_counter"},
      {"int64_value": 10}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

**Expected Result**:
- Response shows success with return value 1
- Server logs show: "Created counter: test_counter with value: 10"
- Counter data is stored in database

#### Test Case 9.2: Increment Counter Reducer
**Objective**: Verify counter increment functionality

**Steps**:
1. Increment the test counter:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "increment_counter",
    "sender_identity": "test_user",
    "args": [
      {"string_value": "test_counter"},
      {"int64_value": 5}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

**Expected Result**:
- Response shows success with return value 1
- Server logs show increment operation with old/new values
- Counter value updated in database

#### Test Case 9.3: Read Counter Value
**Objective**: Verify counter value retrieval

**Steps**:
1. Get current counter value:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "get_counter_value",
    "sender_identity": "test_user",
    "args": [
      {"string_value": "test_counter"}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

**Expected Result**:
- Response shows success with current counter value
- Value matches expected result (10 + 5 = 15)

#### Test Case 9.4: Delete Counter
**Objective**: Verify counter deletion functionality

**Steps**:
1. Delete the test counter:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "delete_counter",
    "sender_identity": "test_user",
    "args": [
      {"string_value": "test_counter"}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

**Expected Result**:
- Response shows success with return value 1
- Server logs show: "Deleted counter: test_counter"
- Counter no longer exists in database

### Test Group 10: WASM Event Integration

#### Test Case 10.1: WASM Events to WebSocket Subscribers
**Objective**: Verify WASM module events reach WebSocket clients

**Setup**:
1. Connect WebSocket client to `ws://localhost:8085`
2. Subscribe to counter events (if event subscription is implemented):
```json
{
  "type": "sql_subscribe",
  "sql": "SELECT * FROM counter_events"
}
```

**Test Steps**:
1. Execute counter operations via WASM reducers
2. Monitor WebSocket for events

**Expected Result**:
- WebSocket receives counter_created, counter_updated, counter_deleted events
- Event payloads contain correct counter information
- Events arrive in real-time

#### Test Case 10.2: WASM-SQL Integration
**Objective**: Verify WASM modules work with SQL subscription filtering

**Setup**:
1. Deploy counter module
2. Connect WebSocket client
3. Subscribe to specific counter:
```json
{
  "type": "sql_subscribe",
  "sql": "SELECT id, value FROM counters WHERE id = 'global_counter'"
}
```

**Test Steps**:
1. Create counter via WASM: `create_counter("global_counter", 0)`
2. Create different counter: `create_counter("other_counter", 100)`
3. Increment global_counter: `increment_counter("global_counter", 1)`
4. Increment other_counter: `increment_counter("other_counter", 1)`

**Expected Result**:
- WebSocket only receives events for global_counter
- Events contain only id and value columns
- other_counter events are filtered out

### Test Group 11: WASM Error Handling

#### Test Case 11.1: Invalid Reducer Names
**Objective**: Verify error handling for non-existent reducers

**Steps**:
1. Try to execute non-existent reducer:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "nonexistent_reducer",
    "sender_identity": "test_user",
    "args": []
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

**Expected Result**:
- Response shows failure with descriptive error
- Server logs error appropriately

#### Test Case 11.2: Invalid Module Names
**Objective**: Verify error handling for non-existent modules

**Steps**:
1. Try to execute reducer on non-existent module:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "module_name": "nonexistent_module",
    "reducer_name": "create_counter",
    "sender_identity": "test_user",
    "args": []
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

**Expected Result**:
- Response shows failure with module not found error

#### Test Case 11.3: Wrong Argument Types
**Objective**: Verify error handling for type mismatches

**Steps**:
1. Pass wrong argument types to reducer:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "create_counter",
    "sender_identity": "test_user",
    "args": [
      {"int64_value": 123},
      {"string_value": "not_a_number"}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

**Expected Result**:
- Response shows failure with type error
- Server handles gracefully without crash

### Test Group 12: WASM Performance and Resource Management

#### Test Case 12.1: Multiple Concurrent Reducer Calls
**Objective**: Test concurrent WASM execution

**Steps**:
1. Execute multiple counter operations simultaneously (if scripting available):
```bash
# Run these in parallel
for i in {1..10}; do
  grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
    -d "{
      \"module_name\": \"counter_app\",
      \"reducer_name\": \"create_counter\",
      \"sender_identity\": \"user_$i\",
      \"args\": [
        {\"string_value\": \"counter_$i\"},
        {\"int64_value\": $i}
      ]
    }" \
    localhost:50051 instantdb.grpc.WasmService.ExecuteReducer &
done
wait
```

**Expected Result**:
- All operations complete successfully
- No race conditions or crashes
- All counters created with correct values

#### Test Case 12.2: Resource Cleanup
**Objective**: Verify WASM instances are properly cleaned up

**Steps**:
1. Execute many operations to create instances
2. Monitor server memory usage
3. Wait for cleanup period
4. Check memory usage again

**Expected Result**:
- Memory usage stabilizes
- No continuous memory growth
- Instance pool maintains reasonable size

### Test Group 13: WASM Module Lifecycle

#### Test Case 13.1: Module Undeployment
**Objective**: Verify modules can be removed properly

**Steps**:
1. Undeploy the counter module:
```bash
grpcurl -plaintext -import-path ./proto -proto instantdb.proto \
  -d '{"name": "counter_app"}' \
  localhost:50051 instantdb.grpc.WasmService.UndeployModule
```

**Expected Result**:
- Response shows success
- Module no longer appears in ListModules
- Subsequent reducer calls fail with module not found

#### Test Case 13.2: Module Redeployment
**Objective**: Verify modules can be redeployed after removal

**Steps**:
1. Redeploy the counter module (same as Test Case 8.1)
2. Execute a reducer to verify functionality

**Expected Result**:
- Deployment succeeds
- Module functions normally after redeployment

---

## WASM Testing Prerequisites

### Required Tools
- `grpcurl` for gRPC API testing
- WebSocket client (wscat, browser, etc.)
- Base64 encoding tool
- Optionally: WASM build tools for compiling modules

### Module Compilation (If Available)
If you have Rust/C++ toolchain with WASM target:

```bash
# For C++ modules (if toolchain supports)
clang++ --target=wasm32-wasi -O2 -nostdlib \
  -Wl,--no-entry -Wl,--export-all \
  sdk/cpp/examples/counter_module.cpp \
  -o counter_module.wasm

# For Rust modules
rustup target add wasm32-wasi
cargo build --target wasm32-wasi --release
```

### Test Data Setup
Before WASM testing:
1. Ensure server is running with WASM engine enabled
2. Verify gRPC WasmService is available:
```bash
grpcurl -plaintext localhost:50051 list instantdb.grpc.WasmService
```

### WASM Testing Notes
- WASM functionality may be partially implemented
- Some tests may fail if WASM engine integration is incomplete
- Focus on API contract testing even if full execution isn't available
- Monitor server logs for WASM-specific errors
- Test with simple modules first before complex ones

---

## Test Environment Cleanup

After testing:
1. Stop all WebSocket clients
2. Undeploy all WASM modules
3. Stop server (Ctrl+C)
4. Verify graceful shutdown in logs
5. Clean up any test data if needed

---

## Success Criteria

The complete InstantDB system (SQL subscriptions + WASM) passes QA if:

### SQL Subscription System
1. All basic subscription operations work correctly
2. Event filtering accurately reflects subscription queries
3. Multiple clients can coexist without interference
4. System handles edge cases gracefully

### WASM Module System
5. Modules can be deployed and undeployed successfully
6. Reducers execute correctly with proper argument handling
7. WASM events integrate with WebSocket subscriptions
8. Error conditions are handled appropriately
9. Resource management works without leaks

### Integration
10. WASM-generated events work with SQL subscription filtering
11. Performance is acceptable under load
12. No memory leaks or resource issues
13. Concurrent operations work correctly

---

## Part C: End-to-End Integration Testing

### Test Group C1: CLI + SQL Subscription Integration

#### Test Case C1.1: Complete Project Workflow
**Objective**: Verify complete development workflow using CLI tools

**Steps**:
1. Initialize project with CLI:
```bash
./build/instantdb init --lang rust realtime_chat
cd realtime_chat
```

2. Start server using CLI:
```bash
../build/instantdb server start --daemon --port 8080
```

3. Create and populate database via CLI:
```bash
../build/instantdb exec --sql "CREATE TABLE messages (id INT PRIMARY KEY, user TEXT, content TEXT, timestamp INT)"
../build/instantdb exec --sql "INSERT INTO messages VALUES (1, 'Alice', 'Hello World', 1234567890)"
```

4. Connect WebSocket client and subscribe:
```bash
wscat -c ws://localhost:8085
# Send: {"type":"sql_subscribe","sql":"SELECT * FROM messages"}
```

5. Add more data via CLI while monitoring subscription:
```bash
../build/instantdb exec --sql "INSERT INTO messages VALUES (2, 'Bob', 'Hello Alice!', 1234567891)"
```

6. View logs and server status:
```bash
../build/instantdb logs --lines 10
../build/instantdb server status
```

**Expected Results**:
- ✅ Project initializes successfully
- ✅ Server starts and runs as daemon
- ✅ Database operations complete via CLI
- ✅ WebSocket subscription receives real-time updates
- ✅ New data insertions trigger subscription events
- ✅ Log viewing shows all operations
- ✅ Server status reports correct information

#### Test Case C1.2: Multi-Client Real-Time Testing
**Objective**: Verify CLI can manage complex multi-client scenarios

**Steps**:
1. Start server with verbose logging:
```bash
../build/instantdb server start --daemon --port 8081 --log-level debug
```

2. Create test table:
```bash
../build/instantdb exec --sql "CREATE TABLE chat_rooms (id INT, room TEXT, user TEXT, message TEXT)"
```

3. Connect multiple WebSocket clients with different subscriptions:
```bash
# Terminal 1 - Subscribe to room 'general'
wscat -c ws://localhost:8085
# Send: {"type":"sql_subscribe","sql":"SELECT * FROM chat_rooms WHERE room = 'general'"}

# Terminal 2 - Subscribe to all rooms
wscat -c ws://localhost:8085
# Send: {"type":"sql_subscribe","sql":"SELECT * FROM chat_rooms"}

# Terminal 3 - Subscribe to specific user
wscat -c ws://localhost:8085
# Send: {"type":"sql_subscribe","sql":"SELECT * FROM chat_rooms WHERE user = 'Alice'"}
```

4. Insert data and verify filtering:
```bash
../build/instantdb exec --sql "INSERT INTO chat_rooms VALUES (1, 'general', 'Alice', 'Hello everyone')"
../build/instantdb exec --sql "INSERT INTO chat_rooms VALUES (2, 'tech', 'Bob', 'Technical discussion')"
../build/instantdb exec --sql "INSERT INTO chat_rooms VALUES (3, 'general', 'Charlie', 'General chat')"
```

5. Monitor logs for subscription activity:
```bash
../build/instantdb logs --follow
```

**Expected Results**:
- ✅ Multiple WebSocket clients connect successfully
- ✅ Each client receives only matching events based on their SQL subscription
- ✅ General room subscribers get general room messages only
- ✅ All-rooms subscriber gets all messages
- ✅ User-specific subscriber gets only Alice's messages
- ✅ Debug logs show proper event filtering and delivery

### Test Group C2: CLI + WASM Module Integration

#### Test Case C2.1: Complete Module Development Cycle
**Objective**: Verify full WASM module development workflow with CLI

**Steps**:
1. Create new module using CLI:
```bash
../build/instantdb module init --lang rust notification_service
```

2. Build the module:
```bash
../build/instantdb module build notification_service
```

3. Deploy the module:
```bash
../build/instantdb module deploy notification_service
```

4. Test module functionality via CLI:
```bash
../build/instantdb exec notification_init --input '{"service":"email"}'
../build/instantdb exec send_notification --input '{"to":"user@example.com","message":"Test notification"}'
```

5. Verify module integrates with subscriptions:
```bash
# Connect WebSocket client
wscat -c ws://localhost:8085
# Send: {"type":"subscribe_to_all_tables"}

# Execute module function that creates events
../build/instantdb exec process_user_action --input '{"user":"Alice","action":"login"}'
```

6. Check module status and logs:
```bash
../build/instantdb module list
../build/instantdb logs --follow
```

**Expected Results**:
- ✅ Module created with proper Rust template
- ✅ Module builds successfully to WASM
- ✅ Module deploys without errors
- ✅ Module functions execute via CLI
- ✅ Module-generated events appear in WebSocket subscriptions
- ✅ Module list shows deployed modules correctly

### Test Group C3: Performance and Stress Testing

#### Test Case C3.1: High-Volume Operations via CLI
**Objective**: Verify CLI handles high-volume operations correctly

**Steps**:
1. Start server with performance logging:
```bash
../build/instantdb server start --daemon --port 8082
```

2. Create test table for bulk operations:
```bash
../build/instantdb exec --sql "CREATE TABLE performance_test (id INT PRIMARY KEY, data TEXT, timestamp INT)"
```

3. Set up subscription monitoring:
```bash
wscat -c ws://localhost:8085 &
WSCAT_PID=$!
# Send subscription for all events
```

4. Execute bulk operations via CLI:
```bash
# Insert 100 records rapidly
for i in {1..100}; do
  ../build/instantdb exec --sql "INSERT INTO performance_test VALUES ($i, 'test_data_$i', $(date +%s))"
done
```

5. Monitor system performance:
```bash
../build/instantdb server status
../build/instantdb logs --lines 50
```

6. Clean up:
```bash
kill $WSCAT_PID
../build/instantdb server stop
```

**Expected Results**:
- ✅ Server handles 100+ rapid insertions without errors
- ✅ WebSocket subscriptions receive all events in order
- ✅ Server remains responsive during bulk operations
- ✅ No memory leaks or resource issues
- ✅ Performance metrics remain acceptable

### Test Group C4: Error Handling and Recovery

#### Test Case C4.1: Server Recovery Testing
**Objective**: Verify CLI handles server failures gracefully

**Steps**:
1. Start server normally:
```bash
../build/instantdb server start --daemon --port 8083
```

2. Verify server is running:
```bash
../build/instantdb server status
```

3. Simulate server crash (kill process directly):
```bash
SERVER_PID=$(../build/instantdb server status | grep -o 'PID: [0-9]*' | cut -d' ' -f2)
kill -9 $SERVER_PID
```

4. Check status after crash:
```bash
../build/instantdb server status
```

5. Restart server via CLI:
```bash
../build/instantdb server start --daemon --port 8083
```

6. Verify recovery:
```bash
../build/instantdb server status
../build/instantdb exec --sql "SELECT COUNT(*) FROM performance_test"
```

**Expected Results**:
- ✅ CLI detects server crash correctly
- ✅ Status command reports "STOPPED" after crash
- ✅ Server restarts successfully via CLI
- ✅ Data persists after restart (if WAL enabled)
- ✅ No corruption or data loss

#### Test Case C4.2: Network and Connection Error Handling
**Objective**: Verify CLI handles network issues gracefully

**Steps**:
1. Test CLI commands when server is down:
```bash
../build/instantdb server stop
../build/instantdb exec --sql "SELECT 1"
../build/instantdb database list
```

2. Test invalid server configurations:
```bash
../build/instantdb exec --server invalid:12345 --sql "SELECT 1"
../build/instantdb database create --server localhost:99999 test_db
```

3. Test with proper error messages:
```bash
../build/instantdb module deploy non_existent_module
../build/instantdb logs --file /non/existent/file.log
```

**Expected Results**:
- ✅ Clear error messages when server unavailable
- ✅ Appropriate error codes returned
- ✅ No crashes or hangs on network errors
- ✅ Helpful suggestions for resolving connection issues

## Final Integration Checklist

### CLI Tool Functionality ✅
- [ ] All commands execute without crashes
- [ ] Help and version information correct
- [ ] Error handling provides useful feedback
- [ ] Project initialization works for all languages
- [ ] Server management (start/stop/status) reliable
- [ ] Log viewing and following functional
- [ ] Module management complete lifecycle
- [ ] Database operations successful
- [ ] SQL and reducer execution working
- [ ] Build system integration functional

### SQL Subscription System ✅
- [ ] Basic subscriptions work correctly
- [ ] Column filtering operates properly
- [ ] WHERE clause support functional
- [ ] Multiple client support stable
- [ ] Event filtering accurate
- [ ] Performance acceptable under load

### WASM Module System ✅
- [ ] Modules load and execute correctly
- [ ] Reducers function properly
- [ ] Security boundaries maintained
- [ ] Performance impact minimal
- [ ] Error handling appropriate

### End-to-End Integration ✅
- [ ] CLI + SQL subscriptions work together
- [ ] CLI + WASM modules integrate properly
- [ ] Real-time events flow correctly
- [ ] Performance remains acceptable
- [ ] Error recovery functions properly
- [ ] Multiple concurrent operations stable

## Additional Notes

### CLI Tool Specific
- Test all CLI commands with `--help` flag for documentation accuracy
- Verify CLI respects environment variables when configured
- Test CLI behavior with various terminal environments
- Validate CLI exit codes for scripting compatibility

### Integration Testing
- Test with both `sql_subscribe` and `subscribe_to_all_tables` message types
- Verify the clean API separation is working as intended
- Monitor server logs throughout testing for any unexpected behavior
- Consider testing with multiple database tables if available
- Test WASM modules with various complexity levels
- Verify WASM security boundaries are maintained
- Check that WASM execution doesn't affect SQL subscription performance
- Validate CLI operations don't interfere with existing WebSocket connections