# InstantDB API Guide

## 🎯 SpacetimeDB-like API Overview

InstantDB provides a SpacetimeDB-compatible API with automatic change detection and real-time streaming. This guide covers all the API patterns and usage examples.

## 📊 Table Definition

### Defining Tables with Attributes

```csharp
using InstantDB;

[Table(Name = "users")]
public class User
{
    [PrimaryKey]
    [JsonPropertyName("id")]
    public string Id { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("email")]
    public string Email { get; set; } = "";

    [JsonPropertyName("premium")]
    public bool Premium { get; set; }

    [JsonPropertyName("created_at")]
    public ulong CreatedAt { get; set; }
}

[Table(Name = "user_activities")]
public class UserActivity
{
    [PrimaryKey]
    [JsonPropertyName("id")]
    public string Id { get; set; } = "";

    [JsonPropertyName("user_id")]
    public string UserId { get; set; } = "";

    [JsonPropertyName("action")]
    public string Action { get; set; } = "";

    [JsonPropertyName("timestamp")]
    public ulong Timestamp { get; set; }
}
```

### Table Attributes
- `[Table(Name = "table_name")]` - Defines the table name in the database
- `[PrimaryKey]` - Marks the primary key field
- `[JsonPropertyName("field")]` - Controls JSON serialization field names

## 🔄 Reducer Functions

### Basic Reducer Pattern

```csharp
public class UserModule : ModuleBase
{
    [Reducer]
    public static int CreateUser(ReducerContext ctx, string name, string email, bool premium)
    {
        try
        {
            // Input validation
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
                Premium = premium,
                CreatedAt = Utils.Now()
            };

            // Insert into database (automatically triggers changefeed)
            ctx.Db.GetTable<User>().Insert(user);

            Utils.LogInfo($"Created user: {user.Id} ({name})");
            return (int)user.Id; // Return success
        }
        catch (Exception ex)
        {
            Utils.LogError($"Exception in CreateUser: {ex.Message}");
            return -1;
        }
    }

    [Reducer]
    public static int UpdateUserPremium(ReducerContext ctx, string userId, bool premium)
    {
        try
        {
            // Find existing user
            var user = ctx.Db.GetTable<User>().Find(userId);
            if (user == null)
            {
                Utils.LogError($"User not found: {userId}");
                return -2; // Not found
            }

            // Update premium status
            user.Premium = premium;

            // Update in database (automatically triggers changefeed)
            ctx.Db.GetTable<User>().Update(user);

            Utils.LogInfo($"Updated user {userId} premium status to {premium}");
            return 1; // Success
        }
        catch (Exception ex)
        {
            Utils.LogError($"Exception in UpdateUserPremium: {ex.Message}");
            return -1;
        }
    }

    [Reducer]
    public static int DeleteUser(ReducerContext ctx, string userId)
    {
        try
        {
            // Check if user exists
            var user = ctx.Db.GetTable<User>().Find(userId);
            if (user == null)
            {
                return 0; // Already deleted or never existed
            }

            // Delete user (automatically triggers changefeed)
            bool wasDeleted = ctx.Db.GetTable<User>().Delete(userId);

            if (wasDeleted)
            {
                Utils.LogInfo($"Deleted user: {userId}");
            }

            return wasDeleted ? 1 : 0;
        }
        catch (Exception ex)
        {
            Utils.LogError($"Exception in DeleteUser: {ex.Message}");
            return -1;
        }
    }
}
```

### Reducer Context API

The `ReducerContext` provides access to the database and execution context:

```csharp
public class ReducerContext
{
    public string Sender { get; internal set; } = "";      // Identity of caller
    public Random Random { get; internal set; } = new Random(); // Deterministic RNG
    public DbContext Db { get; internal set; } = new DbContext(); // Database access
}
```

## 🗄️ Database Operations

### Type-Safe Table Access

```csharp
[Reducer]
public static int ProcessActivity(ReducerContext ctx, string userId, string action)
{
    // Type-safe table access
    var userTable = ctx.Db.GetTable<User>();
    var activityTable = ctx.Db.GetTable<UserActivity>();

    // Find user
    var user = userTable.Find(userId);
    if (user == null)
    {
        return -1; // User not found
    }

    // Create activity
    var activity = new UserActivity
    {
        Id = $"{userId}_{Utils.Now()}",
        UserId = userId,
        Action = action,
        Timestamp = Utils.Now()
    };

    // Insert activity
    activityTable.Insert(activity);

    return 1; // Success
}
```

### Dynamic Table Access

```csharp
[Reducer]
public static int UpdateConfig(ReducerContext ctx, string key, string value)
{
    // Dynamic table access
    ctx.Db.config.Insert(new { Key = key, Value = value });

    // Other dynamic tables
    ctx.Db.users.Insert(user);
    ctx.Db.server_state.Update(state);
    ctx.Db.user_activities.Delete(activityId);

    return 1;
}
```

### Available Operations

| Operation | Description | Automatic Changefeed |
|-----------|-------------|----------------------|
| `Insert(T row)` | Add new row to table | ✅ |
| `Update(T row)` | Update existing row | ✅ |
| `Delete(object key)` | Delete row by primary key | ✅ |
| `Find<T>(object key)` | Find row by primary key | ❌ (Read-only) |

## 🔄 Automatic Change Detection

### How It Works

All database write operations automatically trigger changefeed events:

```csharp
[Reducer]
public static int CreateOrder(ReducerContext ctx, string userId, string item, decimal amount)
{
    var order = new Order
    {
        Id = Utils.GenerateId(),
        UserId = userId,
        Item = item,
        Amount = amount,
        Status = "pending"
    };

    // This automatically emits a changefeed event:
    // {
    //   "table": "orders",
    //   "operation": "INSERT",
    //   "key": order.Id,
    //   "new_value": { ... order data ... }
    // }
    ctx.Db.GetTable<Order>().Insert(order);

    return 1;
}
```

### No Manual Events Needed

**❌ Old Manual Way (No Longer Needed):**
```csharp
// Don't do this anymore!
ctx.Db.GetTable<Order>().Insert(order);
Events.Emit("order_created", order); // Manual event emission
```

**✅ New Automatic Way:**
```csharp
// Just do this - events are automatic!
ctx.Db.GetTable<Order>().Insert(order);
```

## 📡 Real-time Subscriptions

### WASM Subscription Filters

```csharp
[SubscriptionFilter(Name = "filter_premium_users")]
public static bool FilterPremiumUsers(byte[] eventData)
{
    try
    {
        var eventJson = System.Text.Encoding.UTF8.GetString(eventData);
        var eventObj = JsonSerializer.Deserialize<JsonElement>(eventJson);

        if (!eventObj.TryGetProperty("new_value", out var newValueElement))
            return false;

        var activityJson = newValueElement.GetString();
        var activity = JsonSerializer.Deserialize<UserActivity>(activityJson);

        // Check if user has premium account
        var user = DB.Read<User>(new Key(activity.UserId));
        return user?.Premium == true;
    }
    catch (Exception ex)
    {
        Utils.LogError($"Filter error: {ex.Message}");
        return false;
    }
}
```

### WASM Subscription Transforms

```csharp
[SubscriptionTransform(Name = "transform_anonymize")]
public static byte[] TransformAnonymize(byte[] eventData)
{
    try
    {
        var eventJson = System.Text.Encoding.UTF8.GetString(eventData);
        var eventObj = JsonSerializer.Deserialize<JsonElement>(eventJson);

        if (eventObj.TryGetProperty("new_value", out var newValueElement))
        {
            var activityJson = newValueElement.GetString();
            var activity = JsonSerializer.Deserialize<UserActivity>(activityJson);

            // Anonymize user data
            activity.UserId = $"anon_{Math.Abs(activity.UserId.GetHashCode() % 100000)}";

            var transformedJson = JsonSerializer.Serialize(activity);
            var newEventObj = JsonSerializer.Deserialize<Dictionary<string, object>>(eventJson);
            newEventObj["new_value"] = transformedJson;

            var resultJson = JsonSerializer.Serialize(newEventObj);
            return System.Text.Encoding.UTF8.GetBytes(resultJson);
        }

        return eventData;
    }
    catch (Exception ex)
    {
        Utils.LogError($"Transform error: {ex.Message}");
        return eventData;
    }
}
```

## 🌐 WebSocket Client API

### JavaScript Client

```javascript
class InstantDBClient {
    constructor(url) {
        this.url = url;
        this.ws = null;
        this.clientId = null;
        this.subscriptions = new Map();
    }

    async connect() {
        return new Promise((resolve, reject) => {
            this.ws = new WebSocket(this.url);

            this.ws.onopen = () => {
                console.log('Connected to InstantDB');
            };

            this.ws.onmessage = (event) => {
                this.handleMessage(JSON.parse(event.data));
            };

            this.ws.onclose = () => {
                console.log('Disconnected from InstantDB');
            };

            // Wait for welcome message
            this.pendingRequests.set('welcome', { resolve, reject });
        });
    }

    async createWasmSubscription(options) {
        const request = {
            type: 'wasm_subscribe',
            module_name: options.moduleName,
            filter_function: options.filter_function,
            transform_function: options.transform_function,
            tables: options.tables,
            include_initial_data: options.include_initial_data || false
        };

        this.ws.send(JSON.stringify(request));
    }

    // Convenience methods
    async subscribeToRecentActivities(onEvent) {
        return this.createWasmSubscription({
            moduleName: 'subscription_demo',
            filter_function: 'filter_recent_activities',
            transform_function: 'transform_add_metadata',
            tables: ['user_activities'],
            include_initial_data: true,
            onEvent
        });
    }

    async subscribeToPremiumUsers(onEvent) {
        return this.createWasmSubscription({
            moduleName: 'subscription_demo',
            filter_function: 'filter_premium_users',
            transform_function: 'transform_anonymize',
            tables: ['user_activities'],
            onEvent
        });
    }
}

// Usage
const client = new InstantDBClient('ws://localhost:8080');
await client.connect();

// Real-time updates for recent activities
await client.subscribeToRecentActivities((data) => {
    console.log('📈 Recent Activity:', data);
    // Handle real-time activity updates
});

// Real-time updates for premium users (anonymized)
await client.subscribeToPremiumUsers((data) => {
    console.log('👑 Premium User Activity:', data);
    // Handle premium user activities
});
```

## 🔧 gRPC API

### SQL Operations

```bash
# Execute SQL with automatic changefeed events
grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{"sql": "INSERT INTO users VALUES (1, \"Alice\", \"alice@example.com\", true)"}' \
  localhost:50051 instantdb.grpc.SqlService.ExecuteSQL
```

### WASM Module Management

```bash
# Deploy WASM module
grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{
    "name": "user_module",
    "version": "1.0.0",
    "bytecode": "'$(base64 < UserModule.wasm)'"
  }' \
  localhost:50051 instantdb.grpc.WasmService.DeployModule

# Execute reducer
grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{
    "module_name": "user_module",
    "reducer_name": "CreateUser",
    "args": [
      {"string_value": "Alice"},
      {"string_value": "alice@example.com"},
      {"bool_value": true}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

## 🛡️ Error Handling

### Reducer Error Patterns

```csharp
[Reducer]
public static int SafeOperation(ReducerContext ctx, string input)
{
    try
    {
        // Input validation
        if (string.IsNullOrWhiteSpace(input))
        {
            Utils.LogError("Invalid input provided");
            return -1; // Error code
        }

        // Business logic
        var result = ProcessInput(input);

        // Database operation
        ctx.Db.results.Insert(result);

        return 1; // Success
    }
    catch (ArgumentException ex)
    {
        Utils.LogError($"Argument error: {ex.Message}");
        return -2; // Validation error
    }
    catch (Exception ex)
    {
        Utils.LogError($"Unexpected error: {ex.Message}");
        return -1; // General error
    }
}
```

### Return Code Conventions

| Return Code | Meaning |
|-------------|---------|
| `> 0` | Success (can return meaningful values) |
| `0` | No-op or not found (not an error) |
| `-1` | General error |
| `-2` | Validation error |
| `-3` | Permission denied |
| `-4` | Resource not found |

## 🎯 Best Practices

### 1. Reducer Design

```csharp
[Reducer]
public static int BestPracticeReducer(ReducerContext ctx, string param)
{
    // ✅ Always validate inputs
    if (string.IsNullOrWhiteSpace(param))
    {
        Utils.LogError("Parameter cannot be empty");
        return -1;
    }

    // ✅ Use try-catch for error handling
    try
    {
        // ✅ Keep operations atomic
        var entity = CreateEntity(param);
        ctx.Db.GetTable<Entity>().Insert(entity);

        // ✅ Log important operations
        Utils.LogInfo($"Created entity: {entity.Id}");

        // ✅ Return meaningful values
        return entity.Id;
    }
    catch (Exception ex)
    {
        // ✅ Log errors with context
        Utils.LogError($"Failed to create entity: {ex.Message}");
        return -1;
    }
}
```

### 2. Database Operations

```csharp
// ✅ Use type-safe access when possible
var userTable = ctx.Db.GetTable<User>();
var user = userTable.Find(userId);

// ✅ Check for null before operations
if (user != null)
{
    user.LastSeen = Utils.Now();
    userTable.Update(user);
}

// ✅ Use dynamic access for configuration
ctx.Db.config.Insert(new { Key = "setting", Value = "value" });
```

### 3. Subscription Filters

```csharp
[SubscriptionFilter(Name = "example_filter")]
public static bool ExampleFilter(byte[] eventData)
{
    try
    {
        // ✅ Always parse safely
        var eventJson = System.Text.Encoding.UTF8.GetString(eventData);
        var eventObj = JsonSerializer.Deserialize<JsonElement>(eventJson);

        // ✅ Check required fields exist
        if (!eventObj.TryGetProperty("new_value", out var newValueElement))
            return false;

        // ✅ Handle parsing errors
        var data = JsonSerializer.Deserialize<YourDataType>(newValueElement.GetString());

        // ✅ Apply filter logic
        return data.SomeField > SomeThreshold;
    }
    catch (Exception ex)
    {
        // ✅ Log errors and return false (don't crash)
        Utils.LogError($"Filter error: {ex.Message}");
        return false;
    }
}
```

## 📊 Performance Tips

### 1. Efficient Database Access

```csharp
// ✅ Batch operations when possible
var users = GetUsersToUpdate();
var userTable = ctx.Db.GetTable<User>();

foreach (var user in users)
{
    user.LastProcessed = Utils.Now();
    userTable.Update(user); // Each update triggers changefeed
}

// ✅ Use Find() for single lookups
var user = ctx.Db.GetTable<User>().Find(userId);

// ✅ Minimize database operations in loops
```

### 2. Memory Management

```csharp
// ✅ Avoid large object allocations
public static int ProcessLargeDataset(ReducerContext ctx, string[] items)
{
    // Process in chunks instead of all at once
    const int chunkSize = 100;

    for (int i = 0; i < items.Length; i += chunkSize)
    {
        var chunk = items.Skip(i).Take(chunkSize);
        ProcessChunk(ctx, chunk);
    }

    return 1;
}
```

---

**InstantDB API Guide** - Complete reference for the SpacetimeDB-like API with automatic change detection 🚀