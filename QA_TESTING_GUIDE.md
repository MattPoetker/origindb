# InstantDB SQL Subscription System - Manual QA Testing Guide

## Overview
This guide provides comprehensive manual testing procedures for the SQL subscription system implemented in InstantDB. The system allows WebSocket clients to subscribe to specific database tables and columns using SQL syntax, with real-time changefeed event filtering.

## Prerequisites

### 1. Build the System
```bash
cd /Users/a12042/Development/instant_db
make clean && make
```

### 2. Start the Server
```bash
./build/instantdb_server
```

Expected output should show:
- ✅ SQL Subscription Manager ready
- ✅ WebSocket Server ready on 0.0.0.0:8080
- ✅ gRPC Server ready on 0.0.0.0:50051

### 3. Testing Tools Required
- WebSocket client (wscat, websocat, or browser dev tools)
- gRPC client (grpcurl or custom client)
- Terminal for server monitoring

---

## Test Cases

### Test Group 1: Basic SQL Subscription Functionality

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

## Additional Notes

- Test with both `sql_subscribe` and `subscribe_to_all_tables` message types
- Verify the clean API separation is working as intended
- Monitor server logs throughout testing for any unexpected behavior
- Consider testing with multiple database tables if available
- Test WASM modules with various complexity levels
- Verify WASM security boundaries are maintained
- Check that WASM execution doesn't affect SQL subscription performance