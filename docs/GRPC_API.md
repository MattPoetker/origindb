# gRPC API Documentation

## Overview

InstantDB provides a high-performance gRPC API for executing SQL statements and managing the database. This API is designed for programmatic access and integration with other systems.

## Service Definition

The gRPC services are defined in `proto/instantdb.proto`:

```protobuf
service SQLService {
  rpc Execute(SQLRequest) returns (SQLResponse);
  rpc ExecuteTransaction(SQLTransactionRequest) returns (SQLTransactionResponse);
  rpc GetStatus(StatusRequest) returns (StatusResponse);
}

service WasmService {
  rpc DeployModule(DeployModuleRequest) returns (DeployModuleResponse);
  rpc UndeployModule(UndeployModuleRequest) returns (UndeployModuleResponse);
  rpc ListModules(ListModulesRequest) returns (ListModulesResponse);
  rpc GetModule(GetModuleRequest) returns (GetModuleResponse);
  rpc ExecuteReducer(ExecuteReducerRequest) returns (ExecuteReducerResponse);
}
```

## Installation Requirements

To use the gRPC API, you need:

- **gRPC** (C++ implementation)
- **Protocol Buffers** (protobuf)

### Installing Dependencies

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
```

**macOS (Homebrew):**
```bash
brew install grpc protobuf
```

**From Source:**
```bash
# Install gRPC from source (if package not available)
git clone --recurse-submodules -b v1.50.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc
cd grpc
mkdir -p cmake/build
pushd cmake/build
cmake -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF ../..
make -j$(nproc)
sudo make install
popd
```

## Building with gRPC Support

```bash
# Configure with gRPC
cmake -B build -S .

# Build
cmake --build build

# The build will automatically detect gRPC and enable support
```

If gRPC is not found, the server will build without gRPC support and show:
```
gRPC not found - building without gRPC support
```

## Server Configuration

### Command Line Options

```bash
# Start server with custom gRPC port
./instantdb_server -g 50052

# Start with both WebSocket and gRPC custom ports
./instantdb_server -p 9090 -g 50052
```

### Environment Variables

```bash
export INSTANTDB_GRPC_PORT=50052
./instantdb_server
```

### Default Configuration

- **gRPC Port**: 50051
- **WebSocket Port**: 8080
- **Listen Address**: 0.0.0.0 (all interfaces)

## API Reference

### Execute SQL Statement

Execute a single SQL statement and return results.

**Request:**
```protobuf
message SQLRequest {
  string sql = 1;
}
```

**Response:**
```protobuf
message SQLResponse {
  bool success = 1;
  string error = 2;
  repeated Row rows = 3;
  int32 rows_affected = 4;
  int64 execution_time_micros = 5;
}
```

**Example:**
```cpp
SQLRequest request;
request.set_sql("SELECT * FROM users");

SQLResponse response;
Status status = stub->Execute(&context, request, &response);

if (status.ok() && response.success()) {
    for (const auto& row : response.rows()) {
        // Process row data
    }
}
```

### Execute Transaction

Execute multiple SQL statements as a transaction.

**Request:**
```protobuf
message SQLTransactionRequest {
  repeated string sql_statements = 1;
}
```

**Response:**
```protobuf
message SQLTransactionResponse {
  bool success = 1;
  string error = 2;
  repeated SQLResponse results = 3;
  int64 transaction_id = 4;
}
```

### Get Server Status

Get server statistics and health information.

**Request:**
```protobuf
message StatusRequest {
  // Empty for now
}
```

**Response:**
```protobuf
message StatusResponse {
  string version = 1;
  int64 uptime_seconds = 2;
  ServerStats stats = 3;
}
```

### WASM Module Management

`WasmService` deploys and executes WebAssembly modules (see
[../WASM_MODULES.md](../WASM_MODULES.md) and [WASM_ABI.md](WASM_ABI.md)).

**DeployModuleRequest** carries `name`, `version`, raw `bytecode`, and
optional deploy-time capabilities:

```protobuf
message ModuleCapabilities {
  repeated string allowed_tables = 1;  // empty = all tables
  bool read_only = 2;
  uint32 max_memory_mb = 3;            // 0 = default (256 MiB)
  uint32 timeout_ms = 4;               // 0 = default (5000 ms)
}
```

**ExecuteReducerRequest** carries `module_name`, `reducer_name`,
`sender_identity`, and typed `args` (`WasmValue`: int64/string/double/
bool/bytes). The response includes success, error, execution time, and
result values.

Modules persist under `<data_dir>/modules/` and are restored on server
restart; `UndeployModule` removes them.

## Using the gRPC Client

### Command Line Client

InstantDB includes a command-line gRPC client for testing and administration:

```bash
# Get server status
./instantdb_client status

# Execute SQL statement
./instantdb_client exec "CREATE TABLE users (id INT64 PRIMARY KEY, name STRING)"
./instantdb_client exec "INSERT INTO users VALUES (1, 'Alice')"
./instantdb_client exec "SELECT * FROM users"

# Interactive mode
./instantdb_client interactive

# WASM module management
./instantdb_client deploy user_service ./module.wasm 1.0.0
./instantdb_client modules
./instantdb_client call user_service CreateUser '["Alice", "alice@example.com"]'
./instantdb_client undeploy user_service
```

**Interactive Mode:**
```
InstantDB Interactive SQL Client
Connected to: localhost:50051
Type 'exit' or 'quit' to exit

instantdb> CREATE TABLE products (id INT64 PRIMARY KEY, name STRING)
✅ SQL executed successfully
Execution time: 1234 μs
Rows affected: 0

instantdb> INSERT INTO products VALUES (1, 'Widget')
✅ SQL executed successfully
Execution time: 567 μs
Rows affected: 1

instantdb> SELECT * FROM products
✅ SQL executed successfully
Execution time: 234 μs
Rows affected: 1

Results:
  Key: 1, id: 1, name: "Widget"

instantdb> exit
```

### Custom Server Address

```bash
# Connect to different server
./instantdb_client -s server.example.com:50051 status

# Connect to custom port
./instantdb_client -s localhost:50052 exec "SELECT * FROM users"
```

## Programming Examples

### C++ Client

```cpp
#include <grpcpp/grpcpp.h>
#include "instantdb.grpc.pb.h"

class InstantDBClient {
private:
    std::unique_ptr<instantdb::grpc::SQLService::Stub> stub_;

public:
    InstantDBClient(const std::string& server_address) {
        auto channel = grpc::CreateChannel(server_address,
                                         grpc::InsecureChannelCredentials());
        stub_ = instantdb::grpc::SQLService::NewStub(channel);
    }

    bool CreateTable(const std::string& table_name) {
        instantdb::grpc::SQLRequest request;
        request.set_sql("CREATE TABLE " + table_name +
                       " (id INT64 PRIMARY KEY, name STRING)");

        instantdb::grpc::SQLResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Execute(&context, request, &response);
        return status.ok() && response.success();
    }

    bool InsertUser(int64_t id, const std::string& name) {
        instantdb::grpc::SQLRequest request;
        request.set_sql("INSERT INTO users VALUES (" +
                       std::to_string(id) + ", '" + name + "')");

        instantdb::grpc::SQLResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Execute(&context, request, &response);
        return status.ok() && response.success();
    }

    std::vector<std::pair<int64_t, std::string>> GetAllUsers() {
        std::vector<std::pair<int64_t, std::string>> users;

        instantdb::grpc::SQLRequest request;
        request.set_sql("SELECT * FROM users");

        instantdb::grpc::SQLResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->Execute(&context, request, &response);

        if (status.ok() && response.success()) {
            for (const auto& row : response.rows()) {
                int64_t id = 0;
                std::string name;

                auto id_it = row.columns().find("id");
                if (id_it != row.columns().end() && id_it->second.has_int64_value()) {
                    id = id_it->second.int64_value();
                }

                auto name_it = row.columns().find("name");
                if (name_it != row.columns().end() && name_it->second.has_string_value()) {
                    name = name_it->second.string_value();
                }

                users.emplace_back(id, name);
            }
        }

        return users;
    }
};

// Usage
int main() {
    InstantDBClient client("localhost:50051");

    // Create table
    if (!client.CreateTable("users")) {
        std::cerr << "Failed to create table" << std::endl;
        return 1;
    }

    // Insert data
    client.InsertUser(1, "Alice");
    client.InsertUser(2, "Bob");

    // Query data
    auto users = client.GetAllUsers();
    for (const auto& [id, name] : users) {
        std::cout << "User " << id << ": " << name << std::endl;
    }

    return 0;
}
```

### Python Client

```python
import grpc
import instantdb_pb2
import instantdb_pb2_grpc

class InstantDBClient:
    def __init__(self, server_address="localhost:50051"):
        self.channel = grpc.insecure_channel(server_address)
        self.stub = instantdb_pb2_grpc.SQLServiceStub(self.channel)

    def execute_sql(self, sql):
        request = instantdb_pb2.SQLRequest(sql=sql)
        try:
            response = self.stub.Execute(request)
            return response
        except grpc.RpcError as e:
            print(f"gRPC error: {e}")
            return None

    def get_status(self):
        request = instantdb_pb2.StatusRequest()
        try:
            response = self.stub.GetStatus(request)
            return response
        except grpc.RpcError as e:
            print(f"gRPC error: {e}")
            return None

# Usage
client = InstantDBClient()

# Create table
response = client.execute_sql("CREATE TABLE users (id INT64 PRIMARY KEY, name STRING)")
if response and response.success:
    print("Table created successfully")

# Insert data
response = client.execute_sql("INSERT INTO users VALUES (1, 'Alice')")
if response and response.success:
    print(f"Inserted {response.rows_affected} row(s)")

# Query data
response = client.execute_sql("SELECT * FROM users")
if response and response.success:
    for row in response.rows:
        print(f"Row {row.key}:")
        for col_name, col_value in row.columns.items():
            if col_value.HasField('int64_value'):
                print(f"  {col_name}: {col_value.int64_value}")
            elif col_value.HasField('string_value'):
                print(f"  {col_name}: {col_value.string_value}")
```

## Error Handling

### Common Error Scenarios

1. **Connection Errors**
   ```
   gRPC call failed: UNAVAILABLE: No connection could be made
   ```
   - Server is not running
   - Wrong port or address
   - Firewall blocking connection

2. **SQL Errors**
   ```
   SQL execution failed: Table not found: invalid_table
   ```
   - Invalid SQL syntax
   - Table doesn't exist
   - Schema validation errors

3. **Server Errors**
   ```
   Internal server error: Storage engine failure
   ```
   - Internal server issues
   - Storage engine problems
   - Memory or disk issues

### Best Practices

1. **Connection Management**
   ```cpp
   // Reuse connections
   auto channel = grpc::CreateChannel(server_address,
                                    grpc::InsecureChannelCredentials());
   auto stub = instantdb::grpc::SQLService::NewStub(channel);
   ```

2. **Error Checking**
   ```cpp
   grpc::Status status = stub->Execute(&context, request, &response);
   if (!status.ok()) {
       std::cerr << "gRPC Error: " << status.error_message() << std::endl;
       return false;
   }
   if (!response.success()) {
       std::cerr << "SQL Error: " << response.error() << std::endl;
       return false;
   }
   ```

3. **Timeout Configuration**
   ```cpp
   grpc::ClientContext context;
   auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
   context.set_deadline(deadline);
   ```

## Performance Considerations

### Throughput

- **Single Query**: ~10,000 ops/sec
- **Batch Queries**: Use ExecuteTransaction for better performance
- **Connection Pooling**: Reuse connections when possible

### Latency

- **Local Network**: < 1ms
- **Simple Queries**: < 5ms end-to-end
- **Complex Queries**: Varies based on data size

### Optimization Tips

1. **Batch Operations**
   ```cpp
   // Instead of multiple Execute calls
   instantdb::grpc::SQLTransactionRequest request;
   request.add_sql_statements("INSERT INTO users VALUES (1, 'Alice')");
   request.add_sql_statements("INSERT INTO users VALUES (2, 'Bob')");
   request.add_sql_statements("INSERT INTO users VALUES (3, 'Charlie')");

   instantdb::grpc::SQLTransactionResponse response;
   stub->ExecuteTransaction(&context, request, &response);
   ```

2. **Connection Reuse**
   ```cpp
   // Create once, use many times
   class DatabaseManager {
       std::unique_ptr<instantdb::grpc::SQLService::Stub> stub_;
   public:
       DatabaseManager() {
           auto channel = grpc::CreateChannel("localhost:50051",
                                            grpc::InsecureChannelCredentials());
           stub_ = instantdb::grpc::SQLService::NewStub(channel);
       }
   };
   ```

## Security Considerations

### Current Limitations

- **No Authentication**: All connections are unauthenticated
- **No Encryption**: Data transmitted in plain text
- **No Authorization**: All clients have full access

### Future Security Features

- **TLS/SSL**: Encrypted connections
- **Authentication**: Token-based auth
- **Authorization**: Role-based access control
- **Audit Logging**: Security event tracking

### Temporary Security

For production use, consider:

1. **Network Security**
   ```bash
   # Bind to localhost only
   ./instantdb_server -g 127.0.0.1:50051
   ```

2. **Firewall Rules**
   ```bash
   # Allow only specific IPs
   sudo ufw allow from 192.168.1.0/24 to any port 50051
   ```

3. **Reverse Proxy**
   ```nginx
   # nginx proxy with auth
   location /grpc/ {
       grpc_pass grpc://127.0.0.1:50051;
       auth_basic "Database Access";
       auth_basic_user_file /etc/nginx/.htpasswd;
   }
   ```

## Troubleshooting

### Build Issues

**gRPC Not Found:**
```
gRPC not found - building without gRPC support
```
- Install gRPC development packages
- Ensure protobuf is installed
- Check CMake can find gRPC

**Compilation Errors:**
```bash
# Verify gRPC installation
pkg-config --cflags --libs grpc++

# Check protobuf
protoc --version
```

### Runtime Issues

**Server Won't Start:**
```bash
# Check port availability
netstat -tuln | grep 50051

# Check server logs
./instantdb_server -l debug
```

**Connection Refused:**
```bash
# Test connectivity
telnet localhost 50051

# Check firewall
sudo ufw status
```

### Performance Issues

**Slow Queries:**
- Enable debug logging to identify bottlenecks
- Check system resources (CPU, memory)
- Consider query optimization

**High Latency:**
- Check network connectivity
- Monitor server load
- Verify client-side timeouts

## API Split: WebSocket vs gRPC

The WebSocket API does **not** execute SQL — it is subscription-only
(`sql_subscribe`, `subscribe_to_all_tables`, `wasm_subscribe`). All SQL
execution and module management goes over gRPC:

**WebSocket (JSON) — subscribe to changes:**
```javascript
ws.send(JSON.stringify({
    type: 'sql_subscribe',
    sql: 'SELECT * FROM users'
}));
```

**gRPC (Protobuf) — execute statements:**
```cpp
instantdb::grpc::SQLRequest request;
request.set_sql("SELECT * FROM users");

instantdb::grpc::SQLResponse response;
stub->Execute(&context, request, &response);
```

### Benefits of gRPC

1. **Type Safety**: Strongly typed protobuf messages
2. **Performance**: Binary serialization, HTTP/2
3. **Code Generation**: Automatic client libraries
4. **Streaming**: Bi-directional streaming support (future)
5. **Tooling**: Rich ecosystem and debugging tools