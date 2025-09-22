# WASM Subscriptions in InstantDB

InstantDB's WASM subscription system enables real-time, programmable data streams that can be filtered, transformed, and customized using WebAssembly modules. This provides advanced functionality with the ability to write custom subscription logic that runs inside the database.

## 🎯 Key Features

- **Real-time Filtering**: Filter changefeed events using custom WASM functions
- **Data Transformation**: Transform events before sending to clients
- **Custom Query Logic**: Execute complex subscription queries with WASM
- **Client-specific Streams**: Route different data to different clients
- **Initial Data Support**: Send current state when clients first subscribe
- **High Performance**: WASM execution with connection pooling and resource limits

## 🏗️ Architecture Overview

```
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│   Database      │───▶│   Changefeed     │───▶│ WASM Sub Mgr   │
│   Operations    │    │   Engine         │    │                │
└─────────────────┘    └──────────────────┘    └────────────────┘
                                                       │
                                                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│  WebSocket      │◄───│   Filtered &     │◄───│  WASM Module   │
│  Clients        │    │   Transformed    │    │  (Filter/Xform)│
└─────────────────┘    │   Events         │    └────────────────┘
                       └──────────────────┘
```

## 📡 WebSocket API

### Connection and Welcome

```javascript
// Connect to InstantDB WebSocket
const ws = new WebSocket('ws://localhost:8080');

// Welcome message includes client_id and available features
{
  "type": "welcome",
  "client_id": "client_12345_67890",
  "server_version": "0.1.0",
  "features": ["changefeed", "wasm_subscriptions"]
}
```

### Creating WASM Subscriptions

```javascript
// Request a WASM subscription
ws.send(JSON.stringify({
  "type": "wasm_subscribe",
  "module_name": "subscription_demo",
  "filter_function": "filter_recent_activities",
  "transform_function": "transform_add_metadata",
  "tables": ["user_activities"],
  "where_clause": "timestamp > :since",
  "columns": ["user_id", "action", "timestamp"],
  "parameters": {
    "since": 1704067200000,
    "min_reputation": 50
  },
  "include_initial_data": true,
  "start_offset": 0
}));

// Response
{
  "type": "wasm_subscription_created",
  "subscription_id": "wasm_sub_1",
  "client_id": "client_12345_67890",
  "module_name": "subscription_demo"
}
```

### Receiving Subscription Events

```javascript
// Filtered and transformed events
{
  "type": "wasm_subscription_event",
  "subscription_id": "wasm_sub_1",
  "client_id": "client_12345_67890",
  "data": {
    "user_id": "user_123",
    "action": "purchase",
    "timestamp": 1704067260000,
    "processed_at": 1704067261000,
    "server_version": "1.0.0",
    "user_reputation": 95
  }
}
```

## 🔧 WASM Module Development

### C++ Module Example

```cpp
#include "instantdb.hpp"
using namespace instantdb;

// Filter function - only recent activities
INSTANTDB_EXPORT bool filter_recent_activities(const std::vector<uint8_t>& event_data) {
    try {
        std::string json_str(event_data.begin(), event_data.end());
        nlohmann::json event = nlohmann::json::parse(json_str);

        if (!event.contains("new_value")) return false;

        std::string activity_json = event["new_value"];
        nlohmann::json activity = nlohmann::json::parse(activity_json);

        uint64_t activity_time = activity["timestamp"];
        uint64_t current_time = utils::now();
        uint64_t one_hour_ms = 60 * 60 * 1000;

        return (current_time - activity_time) <= one_hour_ms;
    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Filter error: " + std::string(e.what()));
        return false;
    }
}

// Transform function - add metadata
INSTANTDB_EXPORT std::vector<uint8_t> transform_add_metadata(const std::vector<uint8_t>& event_data) {
    try {
        std::string json_str(event_data.begin(), event_data.end());
        nlohmann::json event = nlohmann::json::parse(json_str);

        if (event.contains("new_value")) {
            std::string activity_json = event["new_value"];
            nlohmann::json activity = nlohmann::json::parse(activity_json);

            // Add computed metadata
            activity["processed_at"] = utils::now();
            activity["server_version"] = "1.0.0";

            // Add user data
            std::string user_id = activity["user_id"];
            auto user_result = db::read<nlohmann::json>("users", user_id);
            if (user_result.is_ok() && user_result.unwrap().has_value()) {
                auto user = user_result.unwrap().value();
                activity["user_reputation"] = user["reputation"];
            }

            event["new_value"] = activity.dump();
        }

        std::string result_json = event.dump();
        return std::vector<uint8_t>(result_json.begin(), result_json.end());
    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Transform error: " + std::string(e.what()));
        return event_data;
    }
}

// Initial data function
INSTANTDB_EXPORT std::vector<uint8_t> get_initial_activities(const std::string& where_clause) {
    // Return initial state for new subscribers
    nlohmann::json initial_data = nlohmann::json::array();
    // ... populate with current data based on where_clause
    std::string result_json = initial_data.dump();
    return std::vector<uint8_t>(result_json.begin(), result_json.end());
}
```

### C# Module Example

```csharp
using InstantDB;

public class SubscriptionModule : ModuleBase
{
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
            var userResult = DB.Read<User>(new Key(activity.UserId));
            if (userResult.IsErr || userResult.Unwrap() == null)
                return false;

            return userResult.Unwrap().Premium;
        }
        catch (Exception ex)
        {
            Utils.LogError($"Filter error: {ex.Message}");
            return false;
        }
    }

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
                activity.Details = "[REDACTED]";

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
}
```

## 🚀 Getting Started

### 1. Build and Deploy a WASM Module

```bash
# C++ Module
cd sdk/cpp/examples
emcmake cmake -B build -S .
cmake --build build
grpcurl -plaintext -import-path ../../../proto -proto instantdb.proto \
  -d '{
    "name": "subscription_demo",
    "version": "1.0.0",
    "bytecode": "'$(base64 < build/subscription_module.wasm)'"
  }' \
  localhost:50051 instantdb.grpc.WasmService.DeployModule

# C# Module
cd sdk/csharp/Examples
dotnet publish SubscriptionModule.cs -c Release
grpcurl -plaintext -import-path ../../../proto -proto instantdb.proto \
  -d '{
    "name": "subscription_demo",
    "version": "1.0.0",
    "bytecode": "'$(base64 < bin/Release/net8.0/wasi-wasm/publish/SubscriptionModule.wasm)'"
  }' \
  localhost:50051 instantdb.grpc.WasmService.DeployModule
```

### 2. Create Test Data

```bash
# Create users and activities
grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{
    "module_name": "subscription_demo",
    "reducer_name": "CreateTestUser",
    "args": [
      {"string_value": "user_123"},
      {"string_value": "Alice"},
      {"bool_value": true},
      {"int64_value": 95}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer

grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{
    "module_name": "subscription_demo",
    "reducer_name": "CreateActivity",
    "args": [
      {"string_value": "user_123"},
      {"string_value": "purchase"},
      {"string_value": "Bought premium subscription"}
    ]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

### 3. Connect WebSocket Client

```javascript
const client = new InstantDBClient('ws://localhost:8080');
await client.connect();

// Subscribe to recent activities with metadata
await client.subscribeToRecentActivities((data) => {
    console.log('Activity update:', data);
});

// Subscribe to premium user activities (anonymized)
await client.subscribeToPremiumUsers((data) => {
    console.log('Premium user activity:', data);
});
```

## 📊 Subscription Types

### 1. Real-time Event Filtering

```javascript
{
  "type": "wasm_subscribe",
  "module_name": "subscription_demo",
  "filter_function": "filter_recent_activities",
  "tables": ["user_activities"],
  "include_initial_data": false
}
```

### 2. Data Transformation

```javascript
{
  "type": "wasm_subscribe",
  "module_name": "subscription_demo",
  "transform_function": "transform_add_metadata",
  "tables": ["user_activities"],
  "include_initial_data": true
}
```

### 3. Combined Filter + Transform

```javascript
{
  "type": "wasm_subscribe",
  "module_name": "subscription_demo",
  "filter_function": "filter_premium_users",
  "transform_function": "transform_anonymize",
  "tables": ["user_activities"],
  "where_clause": "premium = true",
  "parameters": {
    "min_reputation": 80
  }
}
```

### 4. Custom Query with Initial Data

```javascript
{
  "type": "wasm_subscribe",
  "module_name": "subscription_demo",
  "tables": ["user_activities", "users"],
  "columns": ["user_id", "action", "timestamp"],
  "where_clause": "timestamp > :since AND reputation > :min_rep",
  "parameters": {
    "since": 1704067200000,
    "min_rep": 50
  },
  "include_initial_data": true,
  "start_offset": 0
}
```

## 🔍 Advanced Use Cases

### 1. User-specific Activity Feeds

Filter events to only show activities from users that the subscriber follows:

```cpp
INSTANTDB_EXPORT bool filter_followed_users(const std::vector<uint8_t>& event_data) {
    // Parse current client context from event
    // Check if event user_id is in subscriber's following list
    // Return true only for followed users
}
```

### 2. Aggregated Real-time Analytics

Transform events into aggregated metrics:

```cpp
INSTANTDB_EXPORT std::vector<uint8_t> transform_to_metrics(const std::vector<uint8_t>& event_data) {
    // Convert individual events into aggregated metrics
    // Return metrics like daily active users, revenue, etc.
}
```

### 3. Geographic Filtering

Filter events based on user location:

```cpp
INSTANTDB_EXPORT bool filter_by_region(const std::vector<uint8_t>& event_data) {
    // Extract user location from database
    // Filter based on geographic regions
    // Support proximity-based filtering
}
```

### 4. Compliance and Privacy

Automatically redact sensitive information:

```cpp
INSTANTDB_EXPORT std::vector<uint8_t> transform_gdpr_compliant(const std::vector<uint8_t>& event_data) {
    // Remove PII based on user consent settings
    // Redact sensitive fields
    // Add compliance metadata
}
```

## 📈 Performance Considerations

- **WASM Instance Pooling**: Reuse WASM instances across multiple subscriptions
- **Event Batching**: Process multiple events in batches for efficiency
- **Resource Limits**: Memory and CPU limits prevent runaway modules
- **Connection Management**: Automatic cleanup of disconnected clients
- **Subscription Limits**: Configurable limits per client/module

## 🔒 Security Features

- **Sandbox Isolation**: WASM modules run in sandboxed environment
- **Resource Constraints**: Memory, CPU, and execution time limits
- **Access Control**: Modules can only access permitted tables/operations
- **Audit Logging**: All subscription operations are logged
- **Rate Limiting**: Prevent abuse through rate limiting

## 🛠️ Development Tools

### Debugging WASM Subscriptions

```bash
# Check subscription metrics
curl -s http://localhost:8080/metrics | grep wasm_subscription

# List active subscriptions
grpcurl -plaintext localhost:50051 instantdb.grpc.WasmService.ListModules

# Test filter functions
grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{
    "module_name": "subscription_demo",
    "reducer_name": "test_filter",
    "args": [{"string_value": "sample_event_data"}]
  }' \
  localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
```

### Performance Monitoring

```javascript
// WebSocket client metrics
const metrics = await client.getSubscriptionMetrics();
console.log('Active subscriptions:', metrics.active_subscriptions);
console.log('Events processed:', metrics.events_processed);
console.log('Events filtered:', metrics.events_filtered);
console.log('Average processing time:', metrics.avg_processing_time);
```

## 🎯 Best Practices

1. **Keep Functions Lightweight**: Filter and transform functions should be fast
2. **Handle Errors Gracefully**: Always return fallback values on errors
3. **Use Appropriate Data Types**: Choose efficient serialization formats
4. **Test Thoroughly**: Test with realistic data volumes and patterns
5. **Monitor Performance**: Track metrics and optimize bottlenecks
6. **Security First**: Validate all inputs and sanitize outputs

## 🔗 API Reference

### WebSocket Message Types

- `wasm_subscribe` - Create WASM subscription
- `wasm_subscription_created` - Subscription created confirmation
- `wasm_subscription_event` - Filtered/transformed event data
- `error` - Error responses

### WASM Function Signatures

- **Filter**: `bool filter_function(const std::vector<uint8_t>& event_data)`
- **Transform**: `std::vector<uint8_t> transform_function(const std::vector<uint8_t>& event_data)`
- **Initial Data**: `std::vector<uint8_t> get_initial_data(const std::string& where_clause)`

---

**InstantDB WASM Subscriptions** - Real-time, programmable data streams with WebAssembly 🚀