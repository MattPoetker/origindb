# Development Guide

## Setting Up Development Environment

### Prerequisites

#### Required Tools
- **C++20 Compiler**: GCC 10+ or Clang 12+
- **CMake**: 3.20 or later
- **Git**: For version control
- **OpenSSL**: For WebSocket handshake (`brew install openssl` on macOS)

#### Platform-Specific Setup

**macOS:**
```bash
# Install Xcode command line tools
xcode-select --install

# Install dependencies via Homebrew
brew install cmake openssl

# Set OpenSSL paths (if needed)
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig"
```

**Ubuntu/Debian:**
```bash
# Install build tools
sudo apt update
sudo apt install build-essential cmake git

# Install OpenSSL
sudo apt install libssl-dev

# Install C++20 compiler (if needed)
sudo apt install gcc-10 g++-10
```

**CentOS/RHEL:**
```bash
# Install build tools
sudo yum groupinstall "Development Tools"
sudo yum install cmake3 git

# Install OpenSSL
sudo yum install openssl-devel

# Enable C++20 (may require devtoolset)
sudo yum install centos-release-scl
sudo yum install devtoolset-10
scl enable devtoolset-10 bash
```

### Building from Source

#### Quick Build
```bash
# Clone repository
git clone <repository-url>
cd instant_db

# Build with default settings
cmake -B build -S .
cmake --build build

# Run tests
./build/instantdb_demo
```

#### Debug Build
```bash
# Configure for debugging
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug

# Build with debug symbols
cmake --build build

# Run with debugger
gdb ./build/instantdb_demo
```

#### Release Build
```bash
# Configure for production
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build optimized version
cmake --build build

# Install system-wide (optional)
sudo cmake --install build
```

### Development Workflow

#### Code Organization

Follow the established project structure:

```
src/
├── cmd/           # Main executables (server, CLI, gRPC client, init)
├── storage/       # Storage engine implementation
├── sql/           # SQL parsing and execution
├── changefeed/    # Changefeed engine + WHERE predicate evaluator
├── websocket/     # WebSocket server
├── wasm/          # WASM engine (wasmtime), module store, subscriptions
├── grpc/          # gRPC services (SQLService, WasmService)
└── common/        # Shared utilities

include/           # Public headers (mirrors src/ layout)
sdk/               # Module SDKs (typescript/AssemblyScript, csharp, ...)
tests/             # gtest unit tests + WASM fixtures
scripts/           # e2e_verify.sh and helper scripts
```

#### Coding Standards

**C++ Style Guidelines:**
- Follow C++20 modern practices
- Use RAII for resource management
- Prefer `std::shared_ptr` for shared ownership
- Use `const` extensively
- Avoid raw pointers when possible

**Naming Conventions:**
```cpp
// Classes: PascalCase
class StorageEngine {};

// Functions: PascalCase
bool Initialize();

// Variables: snake_case
std::string table_name;
int row_count;

// Constants: UPPER_SNAKE_CASE
constexpr int MAX_CONNECTIONS = 1000;

// Private members: trailing underscore
class Example {
private:
    std::string name_;
    int value_;
};
```

**Error Handling:**
```cpp
// Use explicit error types
enum class StorageError {
    TABLE_NOT_FOUND,
    DUPLICATE_KEY,
    SCHEMA_MISMATCH
};

// Return results with error information
struct SqlResult {
    bool success;
    std::string error;
    std::vector<Row> rows;
};

// Use exceptions for unexpected errors only
if (critical_system_failure) {
    throw std::runtime_error("Critical system failure");
}
```

#### Adding New Features

**1. Planning Phase**
- Review existing architecture
- Design API interfaces
- Consider thread safety implications
- Plan testing strategy

**2. Implementation Phase**
- Create feature branch: `git checkout -b feature/feature-name`
- Implement core functionality
- Add comprehensive logging
- Handle error cases

**3. Testing Phase**
- Write unit tests (gtest; add sources to the `unit_tests` target in
  CMakeLists.txt, run with `ctest --test-dir build`)
- Run `scripts/e2e_verify.sh <build_dir>` for the full pipeline
- Verify WebSocket integration
- Test crash recovery scenarios

**4. Integration Phase**
- Update documentation
- Add configuration options
- Ensure backward compatibility
- Submit pull request

### Debugging

#### Logging Configuration

Use spdlog for structured logging:

```cpp
#include <spdlog/spdlog.h>

// Set log level for debugging
spdlog::set_level(spdlog::level::debug);

// Use structured logging
spdlog::info("Storage engine initialized with {} tables", table_count);
spdlog::debug("Processing WAL entry: seq={}, type={}", entry.sequence, entry.type);
spdlog::error("Failed to parse SQL: {}", sql_query);
```

**Log Levels:**
- `trace`: Very detailed debugging
- `debug`: Development debugging
- `info`: General information
- `warn`: Warning conditions
- `error`: Error conditions

#### GDB Debugging

```bash
# Build with debug symbols
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run with GDB
gdb ./build/instantdb_demo

# Common GDB commands
(gdb) break StorageEngine::Initialize
(gdb) run
(gdb) backtrace
(gdb) print variable_name
(gdb) continue
```

#### Memory Debugging

**Valgrind (Linux):**
```bash
# Install valgrind
sudo apt install valgrind

# Run with memory checking
valgrind --leak-check=full ./build/instantdb_demo
```

**AddressSanitizer:**
```bash
# Build with sanitizer
cmake -B build -S . -DCMAKE_CXX_FLAGS="-fsanitize=address -g"
cmake --build build

# Run (will detect memory errors automatically)
./build/instantdb_demo
```

### Testing

#### Automated Testing

```bash
# Unit tests (wasm engine, predicate evaluator, module store)
cmake --build build --target unit_tests
ctest --test-dir build --output-on-failure

# End-to-end verification: build -> server -> deploy WASM module ->
# reducer -> SQL verify -> filtered websocket delivery -> restart
# persistence -> undeploy
scripts/e2e_verify.sh build
```

#### Manual Testing

**Basic Functionality:**
```bash
# Test demo application
./build/instantdb_demo

# Test production server
./build/instantdb_server -p 9090

# Test WebSocket connection
# Use browser console or WebSocket client
const ws = new WebSocket('ws://localhost:9090');
ws.onmessage = console.log;
```

**Crash Recovery Testing:**
```bash
# Start server and insert data
./build/instantdb_demo

# Kill server during operation
kill -9 <pid>

# Restart and verify data recovery
./build/instantdb_demo
```

#### Performance Testing

**Load Testing Script:**
```cpp
// Simple performance test
auto start = std::chrono::high_resolution_clock::now();

for (int i = 0; i < 10000; ++i) {
    auto result = sql_engine->Execute(
        "INSERT INTO test VALUES (" + std::to_string(i) + ", 'test" + std::to_string(i) + "')"
    );
    if (!result.success) {
        std::cerr << "Insert failed: " << result.error << std::endl;
        break;
    }
}

auto end = std::chrono::high_resolution_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
std::cout << "10,000 inserts took: " << duration.count() << "ms" << std::endl;
```

### Adding New Components

#### Creating a New Engine

1. **Define Interface (header file):**
```cpp
// include/myengine/my_engine.h
#pragma once

#include <memory>
#include <string>

namespace instantdb {

class MyEngine {
public:
    MyEngine(const MyEngineConfig& config);
    ~MyEngine();

    bool Initialize();
    void Shutdown();

    // Engine-specific methods
    bool ProcessData(const std::string& data);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb
```

2. **Implement Engine (source file):**
```cpp
// src/myengine/my_engine.cpp
#include "myengine/my_engine.h"
#include <spdlog/spdlog.h>

namespace instantdb {

class MyEngine::Impl {
public:
    Impl(const MyEngineConfig& config) : config_(config) {}

    bool Initialize() {
        spdlog::info("Initializing MyEngine");
        // Implementation here
        return true;
    }

    void Shutdown() {
        spdlog::info("Shutting down MyEngine");
        // Cleanup here
    }

private:
    MyEngineConfig config_;
};

MyEngine::MyEngine(const MyEngineConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

MyEngine::~MyEngine() = default;

bool MyEngine::Initialize() {
    return impl_->Initialize();
}

void MyEngine::Shutdown() {
    impl_->Shutdown();
}

} // namespace instantdb
```

3. **Update CMakeLists.txt:**
```cmake
# Add new source files
file(GLOB_RECURSE MYENGINE_SRCS "src/myengine/*.cpp")

# Create library
add_library(my_engine STATIC ${MYENGINE_SRCS})
target_link_libraries(my_engine
    spdlog::spdlog
    # Other dependencies
)

# Link to main executable
target_link_libraries(instantdb_server
    # Existing libraries
    my_engine
)
```

#### Thread Safety Guidelines

**Use Appropriate Synchronization:**
```cpp
class ThreadSafeEngine {
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Data> data_;

public:
    // Read operations: shared lock
    Data GetData(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = data_.find(key);
        return (it != data_.end()) ? it->second : Data{};
    }

    // Write operations: exclusive lock
    void SetData(const std::string& key, const Data& value) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        data_[key] = value;
    }
};
```

**Atomic Operations for Counters:**
```cpp
class Metrics {
private:
    std::atomic<uint64_t> operations_count_{0};
    std::atomic<uint64_t> errors_count_{0};

public:
    void IncrementOperations() {
        operations_count_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t GetOperationsCount() const {
        return operations_count_.load(std::memory_order_relaxed);
    }
};
```

### Contributing Guidelines

#### Pull Request Process

1. **Fork and Branch**
   ```bash
   git checkout -b feature/my-new-feature
   ```

2. **Make Changes**
   - Follow coding standards
   - Add appropriate logging
   - Handle error cases
   - Update documentation

3. **Test Changes**
   - Verify functionality with demo
   - Test crash recovery
   - Check memory usage
   - Validate WebSocket integration

4. **Commit Guidelines**
   ```bash
   # Use descriptive commit messages
   git commit -m "Add: WebSocket connection pooling

   - Implement connection pool with configurable size
   - Add automatic connection cleanup
   - Improve error handling for network failures
   - Update documentation for new configuration options"
   ```

5. **Submit Pull Request**
   - Provide clear description
   - Reference related issues
   - Include testing steps
   - Request appropriate reviewers

#### Code Review Checklist

**Functionality:**
- [ ] Feature works as intended
- [ ] Error cases are handled
- [ ] Configuration is validated
- [ ] Logging is appropriate

**Code Quality:**
- [ ] Follows coding standards
- [ ] No memory leaks
- [ ] Thread safety considered
- [ ] Performance implications assessed

**Documentation:**
- [ ] API documentation updated
- [ ] Configuration documented
- [ ] Examples provided
- [ ] Architecture changes noted

**Testing:**
- [ ] Manual testing performed
- [ ] Crash recovery tested
- [ ] Memory usage validated
- [ ] Integration verified

### Troubleshooting Common Issues

#### Build Issues

**CMake Configuration Fails:**
```bash
# Clear build directory
rm -rf build/

# Reconfigure with verbose output
cmake -B build -S . --debug-output

# Check for missing dependencies
cmake -B build -S . -DCMAKE_VERBOSE_MAKEFILE=ON
```

**Compilation Errors:**
```bash
# Ensure C++20 support
g++ --version  # Should be 10+
clang++ --version  # Should be 12+

# Set explicit compiler
cmake -B build -S . -DCMAKE_CXX_COMPILER=g++-10
```

#### Runtime Issues

**Server Won't Start:**
- Check port availability: `netstat -an | grep 8080`
- Verify permissions: `ls -la ./instantdb_data/`
- Check logs for specific errors

**WebSocket Connection Fails:**
- Verify server is listening: `telnet localhost 8080`
- Check firewall settings
- Validate WebSocket headers

**Memory Issues:**
- Monitor memory usage: `top -p <pid>`
- Check for memory leaks with Valgrind
- Reduce data volume for testing

#### Performance Issues

**Slow Operations:**
- Enable debug logging to identify bottlenecks
- Profile with `perf` or similar tools
- Check for lock contention
- Monitor disk I/O for WAL writes

**High Memory Usage:**
- Review table sizes and row counts
- Check for memory leaks
- Consider data compression (future feature)
- Monitor WAL file growth

### Development Tools

#### Recommended IDEs

**Visual Studio Code:**
- C/C++ extension
- CMake Tools extension
- GitLens for Git integration

**CLion:**
- Full CMake integration
- Built-in debugger
- Memory profiling tools

**Vim/Neovim:**
- YouCompleteMe or coc.nvim for completion
- vim-cmake for build integration
- vim-fugitive for Git

#### Useful Utilities

**Code Formatting:**
```bash
# Install clang-format
sudo apt install clang-format

# Format code
clang-format -i src/**/*.cpp include/**/*.h
```

**Static Analysis:**
```bash
# Install cppcheck
sudo apt install cppcheck

# Run static analysis
cppcheck --enable=all src/
```

**Profiling:**
```bash
# Install perf
sudo apt install linux-perf

# Profile application
perf record ./build/instantdb_demo
perf report
```