# InstantDB C++ SDK

The **InstantDB C++ SDK** enables you to write WebAssembly modules in C++ that run inside the InstantDB database server. This provides functionality with type-safe database operations, event emission, and reducer functions.

## 🚀 Quick Start

### Prerequisites

- **C++20** compatible compiler (GCC 10+, Clang 12+)
- **Emscripten** for WebAssembly compilation
- **CMake** 3.20+

### Installation

```bash
# Install Emscripten
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh

# Clone InstantDB
git clone <repository-url>
cd instant_db/sdk/cpp
```

### Build Your First Module

```bash
# Configure for WebAssembly
emcmake cmake -B build -S .

# Build the example counter module
cmake --build build

# Output: build/counter_module.wasm
```

## 📚 Programming Guide

### Basic Module Structure

```cpp
#include "instantdb.hpp"

using namespace instantdb;

// Define your data structures
struct User {
    uint64_t id;
    std::string name;
    std::string email;
    uint64_t created_at;

    // Implement serialization (simplified example)
    static std::vector<uint8_t> serialize(const User& user) {
        // Your serialization logic here
    }

    static Result<User> deserialize(const std::vector<uint8_t>& data) {
        // Your deserialization logic here
    }
};

// Specialize the serializable trait
namespace instantdb::serialization {
template<>
class Serializable<User> {
public:
    static std::vector<uint8_t> serialize(const User& value) {
        return User::serialize(value);
    }

    static Result<User> deserialize(const std::vector<uint8_t>& data) {
        return User::deserialize(data);
    }
};
}

// Define reducer functions
INSTANTDB_EXPORT int32_t create_user(const char* name, const char* email) {
    try {
        User user{
            .id = utils::generate_id(),
            .name = std::string(name),
            .email = std::string(email),
            .created_at = utils::now()
        };

        auto result = db::write("users", user.id, user);
        if (result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to create user: " + result.error().message());
            return -1;
        }

        // Emit event for real-time subscribers
        events::emit("user_created", std::to_string(user.id), user);

        return 1; // Success
    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Exception: " + std::string(e.what()));
        return -1;
    }
}

// Module lifecycle hooks
INSTANTDB_INIT() {
    utils::log(utils::LogLevel::Info, "User module initialized");
    return 0;
}
```

## 🔧 API Reference

### Database Operations

#### Reading Data

```cpp
// Read a single value
auto result = db::read<User>("users", user_id);
if (result.is_ok()) {
    auto user_opt = result.unwrap();
    if (user_opt.has_value()) {
        User user = user_opt.value();
        // Use the user data
    }
}

// Handle errors
if (result.is_err()) {
    utils::log(utils::LogLevel::Error, result.error().message());
}
```

#### Writing Data

```cpp
User user{...};

auto result = db::write("users", user.id, user);
if (result.is_err()) {
    utils::log(utils::LogLevel::Error, "Write failed: " + result.error().message());
    return -1;
}
```

#### Deleting Data

```cpp
auto result = db::remove("users", user_id);
if (result.is_ok()) {
    bool was_deleted = result.unwrap();
    if (was_deleted) {
        utils::log(utils::LogLevel::Info, "User deleted");
    }
}
```

### Event Emission

```cpp
struct UserEvent {
    uint64_t user_id;
    std::string action;
    uint64_t timestamp;
};

// Emit an event
UserEvent event{user.id, "created", utils::now()};
auto result = events::emit("user_events", std::to_string(user.id), event);
```

### Utility Functions

```cpp
// Get current timestamp
uint64_t now = utils::now();

// Generate unique ID
uint64_t id = utils::generate_id();

// Logging
utils::log(utils::LogLevel::Info, "Operation completed");
utils::log(utils::LogLevel::Error, "Something went wrong");

// Abort transaction with error
utils::abort("Invalid operation");
```

### Error Handling

```cpp
// Result type for operations that can fail
Result<User> result = db::read<User>("users", user_id);

// Check for success
if (result.is_ok()) {
    auto user_opt = result.unwrap();
    // Handle success case
}

// Check for error
if (result.is_err()) {
    std::string error_msg = result.error().message();
    // Handle error case
}

// Unwrap with default value
User default_user{};
User user = result.unwrap_or(std::move(default_user));
```

## 📊 Type System

### Supported Key Types

```cpp
// String keys
db::read<User>("users", std::string("user123"));

// Integer keys
db::read<User>("users", int64_t(42));
db::read<User>("users", uint64_t(42));
```

### Supported Value Types

```cpp
// Built-in types
db::write("settings", "count", int64_t(42));
db::write("settings", "name", std::string("value"));
db::write("settings", "flag", bool(true));

// Custom types (must implement Serializable trait)
db::write("users", user_id, User{...});
```

## 🏗️ Advanced Features

### Custom Serialization

```cpp
struct ComplexStruct {
    std::vector<std::string> tags;
    std::unordered_map<std::string, int> scores;

    static std::vector<uint8_t> serialize(const ComplexStruct& obj) {
        // Implement efficient binary serialization
        // Consider using Protocol Buffers, MessagePack, or similar
        std::vector<uint8_t> result;

        // Serialize tags
        uint32_t tag_count = obj.tags.size();
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&tag_count),
                     reinterpret_cast<const uint8_t*>(&tag_count) + 4);

        for (const auto& tag : obj.tags) {
            uint32_t tag_len = tag.length();
            result.insert(result.end(), reinterpret_cast<const uint8_t*>(&tag_len),
                         reinterpret_cast<const uint8_t*>(&tag_len) + 4);
            result.insert(result.end(), tag.begin(), tag.end());
        }

        // Serialize scores...

        return result;
    }

    static Result<ComplexStruct> deserialize(const std::vector<uint8_t>& data) {
        // Implement corresponding deserialization
        // Return Error(...) on failure
        // Return ComplexStruct{...} on success
    }
};
```

### Lifecycle Management

```cpp
INSTANTDB_INIT() {
    // Called when module is first loaded
    // Initialize any module-global state
    utils::log(utils::LogLevel::Info, "Module starting up");

    // Return 0 for success, non-zero for failure
    return 0;
}

INSTANTDB_CLIENT_CONNECTED() {
    // Called when a WebSocket client connects
    utils::log(utils::LogLevel::Info, "Client connected");
    return 0;
}

INSTANTDB_CLIENT_DISCONNECTED() {
    // Called when a WebSocket client disconnects
    utils::log(utils::LogLevel::Info, "Client disconnected");
    return 0;
}
```

## 🛠️ Building for Production

### Optimized Build

```bash
# Release build with optimizations
emcmake cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Check module size
ls -lh build/*.wasm
```

### Memory Management

```cpp
// Configure memory limits in CMakeLists.txt
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s MAXIMUM_MEMORY=67108864") # 64MB

// Or use dynamic memory with growth
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s ALLOW_MEMORY_GROWTH=1")
```

### Performance Tips

1. **Minimize allocations**: Reuse objects where possible
2. **Efficient serialization**: Use binary formats, avoid JSON for hot paths
3. **Batch operations**: Group database operations when possible
4. **Error handling**: Use Result types instead of exceptions for performance-critical code

## 📝 Examples

### Counter Module

See `examples/counter_module.cpp` for a complete working example that demonstrates:

- Creating, incrementing, and deleting counters
- Error handling and validation
- Event emission for real-time updates
- Module lifecycle management

### Building the Example

```bash
# Build counter module
emcmake cmake -B build -S .
cmake --build build

# Deploy to InstantDB
grpcurl -plaintext -import-path ../../proto -proto instantdb.proto \
  -d '{
    "name": "counter_app",
    "version": "1.0.0",
    "bytecode": "'"$(base64 < build/counter_module.wasm)"'"
  }' \
  localhost:50051 instantdb.grpc.WasmService.DeployModule
```

### Testing the Module

```bash
# Execute reducers
grpcurl -plaintext -import-path ../../proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "create_counter",
    "args": [{"string_value": "global"}, {"int64_value": 0}]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer

grpcurl -plaintext -import-path ../../proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "increment_counter",
    "args": [{"string_value": "global"}, {"int64_value": 1}]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

## 🐛 Debugging

### Logging

```cpp
// Use different log levels
utils::log(utils::LogLevel::Trace, "Detailed debug info");
utils::log(utils::LogLevel::Debug, "Debug information");
utils::log(utils::LogLevel::Info, "General information");
utils::log(utils::LogLevel::Warn, "Warning message");
utils::log(utils::LogLevel::Error, "Error occurred");
```

### Error Handling Best Practices

```cpp
// Always check results
auto result = db::read<User>("users", user_id);
if (result.is_err()) {
    utils::log(utils::LogLevel::Error, "Database error: " + result.error().message());
    return -1; // Return error code from reducer
}

// Use RAII for cleanup
class Transaction {
public:
    Transaction() { /* begin */ }
    ~Transaction() { /* commit or rollback */ }
};

// Validate inputs
INSTANTDB_EXPORT int32_t create_user(const char* name, const char* email) {
    if (!name || strlen(name) == 0) {
        utils::log(utils::LogLevel::Error, "Name cannot be empty");
        return -1;
    }

    // Continue with implementation...
}
```

## 🔒 Security Considerations

1. **Input validation**: Always validate parameters passed to reducers
2. **Bounds checking**: Verify array/string lengths before operations
3. **Resource limits**: Be mindful of memory and CPU usage
4. **Error handling**: Don't leak sensitive information in error messages

## 📋 Roadmap

### Current Version (v1.0.0)
- ✅ Basic database operations (read, write, delete)
- ✅ Event emission for real-time updates
- ✅ Type-safe Result types for error handling
- ✅ Module lifecycle hooks
- ✅ Complete example with counter module

### Next Version (v1.1.0)
- 📋 Advanced serialization helpers (Protocol Buffers integration)
- 📋 Async operation support
- 📋 Batch operation APIs
- 📋 Query/scan operations with filtering

### Future Versions
- 📋 Cross-module communication
- 📋 Streaming data processing
- 📋 Advanced debugging tools
- 📋 Performance profiling integration

---

**InstantDB C++ SDK** - Write high-performance WASM modules in modern C++ 🚀