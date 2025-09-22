# InstantDB C# SDK

The **InstantDB C# SDK** enables you to write WebAssembly modules in C# that run inside the InstantDB database server. This provides functionality with type-safe database operations, automatic JSON serialization, and attribute-based schema definitions.

## 🚀 Quick Start

### Prerequisites

- **.NET 8.0** or later
- **WASI SDK** for WebAssembly compilation
- **wasm-tools** for module optimization

### Installation

```bash
# Install .NET WebAssembly tools
dotnet workload install wasi-experimental

# Install wasm-tools (optional, for optimization)
cargo install wasm-tools

# Clone InstantDB
git clone <repository-url>
cd instant_db/sdk/csharp
```

### Build Your First Module

```bash
# Create a new C# project
dotnet new console -n MyCounterModule
cd MyCounterModule

# Add InstantDB SDK reference
cp ../InstantDB.cs .

# Create your module (see examples below)
# Build for WebAssembly
dotnet build -c Release

# Output: bin/Release/net8.0/MyCounterModule.wasm
```

## 📚 Programming Guide

### Basic Module Structure

```csharp
using System;
using System.Text.Json.Serialization;
using InstantDB;

// Define your data structures with table attributes
[Table(Name = "users")]
public class User
{
    [PrimaryKey]
    [JsonPropertyName("id")]
    public ulong Id { get; set; }

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("email")]
    public string Email { get; set; } = "";

    [JsonPropertyName("created_at")]
    public ulong CreatedAt { get; set; }
}

// Define event schemas for real-time subscriptions
[Event(Name = "user_events")]
public class UserEvent
{
    [JsonPropertyName("user_id")]
    public ulong UserId { get; set; }

    [JsonPropertyName("action")]
    public string Action { get; set; } = "";

    [JsonPropertyName("timestamp")]
    public ulong Timestamp { get; set; }
}

// Create your module class
public class UserModule : ModuleBase
{
    // Define reducer functions
    [Reducer]
    public static int CreateUser(string name, string email)
    {
        try
        {
            // Validate inputs
            if (string.IsNullOrWhiteSpace(name))
            {
                Utils.LogError("Name cannot be empty");
                return -1;
            }

            // Create new user
            var user = new User
            {
                Id = Utils.GenerateId(),
                Name = name,
                Email = email,
                CreatedAt = Utils.Now()
            };

            // Write to database
            var result = DB.Write(new Key(user.Id), user);
            if (result.IsErr)
            {
                Utils.LogError($"Failed to create user: {result.Error()}");
                return -1;
            }

            // Emit real-time event
            var userEvent = new UserEvent
            {
                UserId = user.Id,
                Action = "created",
                Timestamp = user.CreatedAt
            };

            Events.Emit(user.Id.ToString(), userEvent);

            Utils.LogInfo($"Created user: {user.Id} ({name})");
            return (int)user.Id; // Return user ID

        }
        catch (Exception ex)
        {
            Utils.LogError($"Exception in CreateUser: {ex.Message}");
            return -1;
        }
    }

    // Module lifecycle override
    public override int Initialize()
    {
        Utils.LogInfo("User module initialized successfully");
        return 0;
    }
}
```

## 🔧 API Reference

### Database Operations

#### Reading Data

```csharp
// Read using explicit table name
var result = DB.Read<User>("users", new Key(userId));
if (result.IsOk)
{
    var user = result.Unwrap();
    if (user != null)
    {
        // Use the user data
        Utils.LogInfo($"Found user: {user.Name}");
    }
    else
    {
        Utils.LogInfo("User not found");
    }
}

// Read using table attribute (automatic table name)
var result2 = DB.Read<User>(new Key(userId));

// Handle errors
if (result.IsErr)
{
    Utils.LogError($"Database error: {result.Error()}");
}
```

#### Writing Data

```csharp
var user = new User
{
    Id = Utils.GenerateId(),
    Name = "John Doe",
    Email = "john@example.com",
    CreatedAt = Utils.Now()
};

// Write using explicit table name
var result = DB.Write("users", new Key(user.Id), user);

// Write using table attribute (automatic table name)
var result2 = DB.Write(new Key(user.Id), user);

if (result.IsErr)
{
    Utils.LogError($"Write failed: {result.Error()}");
    return -1;
}
```

#### Deleting Data

```csharp
// Delete using explicit table name
var result = DB.Delete("users", new Key(userId));

// Delete using table attribute (automatic table name)
var result2 = DB.Delete<User>(new Key(userId));

if (result.IsOk)
{
    bool wasDeleted = result.Unwrap();
    if (wasDeleted)
    {
        Utils.LogInfo("User deleted successfully");
    }
    else
    {
        Utils.LogInfo("User was not found");
    }
}
```

### Event Emission

```csharp
// Define event structure
[Event(Name = "user_events")]
public class UserEvent
{
    [JsonPropertyName("user_id")]
    public ulong UserId { get; set; }

    [JsonPropertyName("action")]
    public string Action { get; set; } = "";

    [JsonPropertyName("timestamp")]
    public ulong Timestamp { get; set; }
}

// Emit event with explicit topic
var userEvent = new UserEvent
{
    UserId = userId,
    Action = "updated",
    Timestamp = Utils.Now()
};

var result = Events.Emit("user_updates", userId.ToString(), userEvent);

// Emit event using event attribute (automatic topic name)
var result2 = Events.Emit(userId.ToString(), userEvent);
```

### Utility Functions

```csharp
// Get current timestamp in milliseconds
ulong now = Utils.Now();

// Generate unique ID
ulong id = Utils.GenerateId();

// Logging with different levels
Utils.LogTrace("Detailed debug information");
Utils.LogDebug("Debug information");
Utils.LogInfo("General information");
Utils.LogWarn("Warning message");
Utils.LogError("Error occurred");

// Abort transaction with error message
Utils.Abort("Invalid operation detected");
```

### Error Handling

```csharp
// Result type for operations that can fail
Result<User?> result = DB.Read<User>(new Key(userId));

// Check for success
if (result.IsOk)
{
    var user = result.Unwrap();
    // Handle success case
}

// Check for error
if (result.IsErr)
{
    string errorMsg = result.Error();
    Utils.LogError($"Operation failed: {errorMsg}");
}

// Unwrap with default value
var defaultUser = new User { Name = "Unknown" };
var user = result.UnwrapOr(defaultUser);
```

## 📊 Type System

### Supported Key Types

```csharp
// String keys
DB.Read<User>(new Key("user123"));

// Integer keys
DB.Read<User>(new Key(42L));        // long
DB.Read<User>(new Key(42UL));       // ulong
DB.Read<User>(new Key(42));         // int (converted to long)
DB.Read<User>(new Key(42U));        // uint (converted to ulong)
```

### Attributes for Schema Definition

```csharp
// Table definition
[Table(Name = "custom_table_name")]
public class MyTable
{
    // Primary key
    [PrimaryKey]
    [JsonPropertyName("id")]
    public string Id { get; set; } = "";

    // Foreign key reference
    [ForeignKey(typeof(User))]
    [JsonPropertyName("user_id")]
    public ulong UserId { get; set; }

    // Regular properties
    [JsonPropertyName("value")]
    public long Value { get; set; }
}

// Event definition
[Event(Name = "custom_events")]
public class MyEvent
{
    [JsonPropertyName("data")]
    public string Data { get; set; } = "";
}

// Reducer definition
[Reducer]
public static int MyReducer(string input)
{
    // Implementation
    return 1;
}
```

## 🏗️ Advanced Features

### Module Lifecycle Management

```csharp
public class MyModule : ModuleBase
{
    // Called when module is first loaded
    public override int Initialize()
    {
        Utils.LogInfo("Module starting up");

        // Initialize any module-global state
        // Create default data if needed
        var result = CreateDefaultUser();
        if (result < 0)
        {
            Utils.LogError("Failed to initialize default data");
        }

        return 0; // Success
    }

    // Called when a WebSocket client connects
    public override int OnClientConnected(string connectionId)
    {
        Utils.LogInfo($"Client connected: {connectionId}");

        // Optionally create per-client data
        // Subscribe client to relevant events

        return 0;
    }

    // Called when a WebSocket client disconnects
    public override int OnClientDisconnected(string connectionId)
    {
        Utils.LogInfo($"Client disconnected: {connectionId}");

        // Clean up client-specific data
        // Unsubscribe from events

        return 0;
    }
}
```

### Complex Data Types

```csharp
// Complex nested structures
public class OrderItem
{
    [JsonPropertyName("product_id")]
    public string ProductId { get; set; } = "";

    [JsonPropertyName("quantity")]
    public int Quantity { get; set; }

    [JsonPropertyName("price")]
    public decimal Price { get; set; }
}

[Table(Name = "orders")]
public class Order
{
    [PrimaryKey]
    [JsonPropertyName("id")]
    public string Id { get; set; } = "";

    [JsonPropertyName("customer_id")]
    public string CustomerId { get; set; } = "";

    [JsonPropertyName("items")]
    public List<OrderItem> Items { get; set; } = new();

    [JsonPropertyName("metadata")]
    public Dictionary<string, string> Metadata { get; set; } = new();

    [JsonPropertyName("created_at")]
    public ulong CreatedAt { get; set; }
}
```

### Error Handling Patterns

```csharp
[Reducer]
public static int ProcessOrder(string orderId, string customerId)
{
    try
    {
        // Input validation
        if (string.IsNullOrWhiteSpace(orderId))
        {
            Utils.LogError("Order ID cannot be empty");
            return -1; // Invalid input
        }

        // Read with error handling
        var orderResult = DB.Read<Order>(new Key(orderId));
        if (orderResult.IsErr)
        {
            Utils.LogError($"Failed to read order: {orderResult.Error()}");
            return -2; // Database error
        }

        var order = orderResult.Unwrap();
        if (order == null)
        {
            Utils.LogError($"Order not found: {orderId}");
            return -3; // Not found
        }

        // Business logic with validation
        if (order.CustomerId != customerId)
        {
            Utils.LogError("Customer ID mismatch");
            return -4; // Authorization error
        }

        // Update operation
        order.Status = "processed";
        var updateResult = DB.Write(new Key(orderId), order);
        if (updateResult.IsErr)
        {
            Utils.LogError($"Failed to update order: {updateResult.Error()}");
            return -5; // Update failed
        }

        Utils.LogInfo($"Successfully processed order: {orderId}");
        return 1; // Success

    }
    catch (Exception ex)
    {
        Utils.LogError($"Exception in ProcessOrder: {ex.Message}");
        return -1; // Unexpected error
    }
}
```

## 🛠️ Building for Production

### Project Configuration

Create a `.csproj` file for your module:

```xml
<Project Sdk="Microsoft.NET.Sdk">

  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <RuntimeIdentifier>wasi-wasm</RuntimeIdentifier>
    <UseAppHost>false</UseAppHost>
    <PublishSingleFile>false</PublishSingleFile>
    <InvariantGlobalization>true</InvariantGlobalization>
  </PropertyGroup>

  <ItemGroup>
    <PackageReference Include="Microsoft.DotNet.ILCompiler.LLVM" Version="8.0.0" />
  </ItemGroup>

</Project>
```

### Optimized Build

```bash
# Release build with optimizations
dotnet publish -c Release

# Check module size
ls -lh bin/Release/net8.0/wasi-wasm/publish/*.wasm

# Optimize with wasm-tools (optional)
wasm-tools strip bin/Release/net8.0/wasi-wasm/publish/MyModule.wasm \
  -o MyModule.optimized.wasm
```

### Performance Tips

1. **Minimize allocations**: Reuse objects and collections where possible
2. **Use struct for small data**: Prefer `struct` over `class` for small value types
3. **Efficient JSON**: Use `JsonPropertyName` attributes to control serialization
4. **Batch operations**: Group multiple database operations when feasible
5. **Error handling**: Use `Result<T>` pattern instead of exceptions for performance-critical paths

## 📝 Examples

### Complete Counter Module

See `Examples/CounterModule.cs` for a comprehensive working example that demonstrates:

- Creating, incrementing, and deleting counters
- Input validation and error handling
- Event emission for real-time updates
- Module lifecycle management with client connection handling

### Building the Example

```bash
# Navigate to the C# SDK directory
cd sdk/csharp

# Build counter module
dotnet publish Examples/CounterModule.cs -c Release

# Deploy to InstantDB
grpcurl -plaintext -import-path ../../proto -proto instantdb.proto \
  -d '{
    "name": "counter_app",
    "version": "1.0.0",
    "bytecode": "'$(base64 < bin/Release/net8.0/wasi-wasm/publish/CounterModule.wasm)'"
  }' \
  localhost:50051 instantdb.grpc.WasmService.DeployModule
```

### Testing the Module

```bash
# Create a counter
grpcurl -plaintext -import-path ../../proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "CreateCounter",
    "args": [
      {"string_value": "global"},
      {"int64_value": 0},
      {"string_value": "system"}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer

# Increment the counter
grpcurl -plaintext -import-path ../../proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "IncrementCounter",
    "args": [
      {"string_value": "global"},
      {"int64_value": 5},
      {"string_value": "user123"}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer

# Get counter value
grpcurl -plaintext -import-path ../../proto -proto instantdb.proto \
  -d '{
    "module_name": "counter_app",
    "reducer_name": "GetCounterValue",
    "args": [{"string_value": "global"}]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

## 🐛 Debugging

### Logging Best Practices

```csharp
// Use appropriate log levels
Utils.LogTrace("Detailed variable states"); // Development only
Utils.LogDebug("Function entry/exit");      // Debug builds
Utils.LogInfo("Important operations");      // Production
Utils.LogWarn("Recoverable issues");        // Always
Utils.LogError("Serious problems");         // Always

// Include context in log messages
Utils.LogError($"Failed to process user {userId}: {error.Message}");
Utils.LogInfo($"Counter '{counterId}' incremented from {oldValue} to {newValue}");
```

### Error Handling Best Practices

```csharp
// Always validate inputs
[Reducer]
public static int MyReducer(string input)
{
    if (string.IsNullOrWhiteSpace(input))
    {
        Utils.LogError("Input parameter cannot be null or empty");
        return -1;
    }

    if (input.Length > 1000)
    {
        Utils.LogError($"Input too long: {input.Length} characters");
        return -1;
    }

    // Continue with implementation...
}

// Check all database operations
var result = DB.Read<User>(new Key(userId));
if (result.IsErr)
{
    Utils.LogError($"Database read failed: {result.Error()}");
    return -1; // Return error code from reducer
}

// Handle null results appropriately
var user = result.Unwrap();
if (user == null)
{
    Utils.LogWarn($"User not found: {userId}");
    return 0; // Not an error, just not found
}
```

## 🔒 Security Considerations

1. **Input validation**: Always validate all parameters passed to reducers
2. **SQL injection prevention**: Use parameterized operations (built into the SDK)
3. **Resource limits**: Be mindful of memory usage and processing time
4. **Error information**: Don't leak sensitive data in error messages
5. **Access control**: Implement proper authorization in your business logic

## 📋 Roadmap

### Current Version (v1.0.0)
- ✅ Type-safe database operations with automatic JSON serialization
- ✅ Attribute-based schema definitions for tables and events
- ✅ Event emission for real-time WebSocket subscriptions
- ✅ Result types for comprehensive error handling
- ✅ Module lifecycle hooks with client connection management
- ✅ Complete counter module example with full functionality

### Next Version (v1.1.0)
- 📋 Advanced query operations with LINQ-style syntax
- 📋 Bulk operation APIs for batch processing
- 📋 Custom serialization attributes and converters
- 📋 Async/await support for non-blocking operations

### Future Versions
- 📋 Cross-module communication and dependency injection
- 📋 Advanced debugging tools and profiling integration
- 📋 Entity Framework-style migrations and schema management
- 📋 Streaming data processing with reactive extensions

---

**InstantDB C# SDK** - Write type-safe, high-performance WASM modules in modern C# 🚀