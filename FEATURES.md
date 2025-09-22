# InstantDB Feature Specification

## Overview

InstantDB is a real-time, programmable database with WebAssembly (WASM) support and automatic change detection. It combines the developer experience of SpacetimeDB with real-time streaming capabilities similar to Supabase.

## 🎯 Core Features (Implemented)

### 1. SpacetimeDB-like WASM API

**Reducer Context Pattern**
```csharp
[Reducer]
public static int CreateUser(ReducerContext ctx, string name, string email) {
    var user = new User { Name = name, Email = email };
    ctx.Db.GetTable<User>().Insert(user);
    return user.Id;
}
```

**Key Benefits:**
- ✅ Familiar SpacetimeDB-style API
- ✅ Type-safe database operations
- ✅ Automatic transaction management
- ✅ Built-in changefeed event emission

**ReducerContext API:**
- `ctx.Sender` - Identity of the caller
- `ctx.Random` - Deterministic random number generator
- `ctx.Db` - Database context for table operations

### 2. Automatic Change Detection

**Database Operations Trigger Events Automatically**
```csharp
// This automatically emits changefeed events
ctx.Db.users.Insert(user);

// No manual Events.Emit() needed!
// ❌ Events.Emit("user_created", user); // OLD WAY
```

**Implementation:**
- Storage engine linked to changefeed engine
- All write operations automatically emit events
- Events include table, operation type, key, and value
- Real-time delivery to WebSocket subscribers

### 3. Dynamic Table Access

**Multiple Access Patterns:**
```csharp
// Type-safe access
ctx.Db.GetTable<User>().Insert(user);
ctx.Db.GetTable<User>().Find(userId);

// Dynamic access
ctx.Db.users.Insert(user);
ctx.Db.config.Update(setting);
ctx.Db.server_state.Delete(key);
```

**Supported Operations:**
- `Insert(T row)` - Add new row
- `Update(T row)` - Update existing row
- `Delete(object key)` - Delete by primary key
- `Find<T>(object key)` - Find by primary key

### 4. WASM Subscription System

**Programmable Real-time Filters**
```csharp
[SubscriptionFilter(Name = "filter_premium_users")]
public static bool FilterPremiumUsers(byte[] eventData) {
    var activity = ParseActivity(eventData);
    var user = DB.Read<User>(new Key(activity.UserId));
    return user?.Premium == true;
}

[SubscriptionTransform(Name = "transform_anonymize")]
public static byte[] TransformAnonymize(byte[] eventData) {
    var activity = ParseActivity(eventData);
    activity.UserId = $"anon_{Math.Abs(activity.UserId.GetHashCode() % 100000)}";
    return SerializeActivity(activity);
}
```

**Features:**
- Server-side event filtering using WASM functions
- Real-time data transformation before delivery
- Client-specific event routing
- High-performance event processing

### 5. WebSocket Real-time Streaming

**Client API:**
```javascript
const client = new InstantDBClient('ws://localhost:8080');
await client.connect();

// Subscribe with WASM filtering
await client.subscribeToRecentActivities((data) => {
    console.log('Real-time update:', data);
});

// Subscribe with transformation
await client.subscribeToPremiumUsers((data) => {
    console.log('Premium user activity (anonymized):', data);
});
```

**Protocol Features:**
- JSON-over-WebSocket for easy debugging
- Client ID tracking and session management
- Subscription management with automatic cleanup
- Error handling and reconnection support

### 6. SQL Integration with Changefeeds

**gRPC SQL Service:**
```bash
# SQL operations automatically trigger changefeed events
grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{"sql": "INSERT INTO users VALUES (1, \"Alice\")"}' \
  localhost:50051 instantdb.grpc.SqlService.ExecuteSQL
```

**Features:**
- SQL queries integrated with changefeed system
- Transaction support with WAL persistence
- Automatic event emission for all SQL operations
- gRPC API with protocol buffer definitions

## 🏗️ Architecture

### Component Overview
```
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│   Client SDK    │───▶│   gRPC/SQL       │───▶│  Storage       │
│   (C#, C++)     │    │   Service        │    │  Engine        │
└─────────────────┘    └──────────────────┘    └────────────────┘
                                │                       │
                                ▼                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│  WebSocket      │◄───│   Changefeed     │◄───│  Auto Event    │
│  Clients        │    │   Engine         │    │  Emission      │
└─────────────────┘    └──────────────────┘    └────────────────┘
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐    ┌──────────────────┐    ┌────────────────┐
│  WASM Sub       │◄───│   WASM Engine    │◄───│  Module Store  │
│  Manager        │    │   (Wasmtime)     │    │  & Lifecycle   │
└─────────────────┘    └──────────────────┘    └────────────────┘
```

### Data Flow
1. **Client Request** → gRPC/SQL service or WASM reducer
2. **Transaction Processing** → Storage engine with automatic event emission
3. **Event Generation** → Changefeed engine captures all changes
4. **WASM Processing** → Optional filtering/transformation of events
5. **Real-time Delivery** → WebSocket server streams to subscribers

## 📚 SDK Support

### C# SDK (`InstantDB.cs`)
- Complete SpacetimeDB-like API
- Attributes: `[Table]`, `[PrimaryKey]`, `[Reducer]`, `[SubscriptionFilter]`
- Type-safe database operations
- JSON serialization support
- Error handling with `Result<T>` pattern

### C++ SDK (Header-only)
- Low-level host API bindings
- Memory-efficient operations
- Compatible with existing C++ codebases
- Direct WASM runtime integration

## 🔧 Development Workflow

### 1. Define Data Models
```csharp
[Table(Name = "users")]
public class User {
    [PrimaryKey]
    public string Id { get; set; }
    public string Name { get; set; }
    public bool Premium { get; set; }
}
```

### 2. Implement Reducers
```csharp
[Reducer]
public static int CreateUser(ReducerContext ctx, string name, bool premium) {
    var user = new User {
        Id = Utils.GenerateId(),
        Name = name,
        Premium = premium
    };

    ctx.Db.GetTable<User>().Insert(user);
    return 1; // Success
}
```

### 3. Build and Deploy
```bash
# Build WASM module
dotnet publish UserModule.cs -c Release

# Deploy to InstantDB
grpcurl -plaintext -import-path proto -proto instantdb.proto \
  -d '{"name": "user_module", "bytecode": "'$(base64 < UserModule.wasm)'"}' \
  localhost:50051 instantdb.grpc.WasmService.DeployModule
```

### 4. Connect Clients
```javascript
const client = new InstantDBClient('ws://localhost:8080');
await client.connect();

// Real-time updates
client.on('user_created', (user) => {
    console.log('New user:', user);
});
```

## 🚀 Performance Features

### WASM Runtime Optimization
- Instance pooling for fast module execution
- Resource limits (memory, CPU time)
- Sandboxed execution environment
- Efficient host API with minimal overhead

### Real-time Streaming
- Automatic changefeed event generation
- WebSocket connection pooling
- Client-specific event filtering
- Backpressure handling

### Storage Efficiency
- In-memory tables with WAL persistence
- Transaction support with ACID properties
- Efficient serialization formats
- Optimized key-value operations

## 🔒 Security Features

### WASM Sandboxing
- Memory isolation between modules
- CPU time limits and instruction metering
- Restricted host API surface
- No file system or network access by default

### Access Control
- Module deployment requires explicit approval
- Table-level access controls
- Client authentication and authorization
- Audit logging for all operations

## 📊 Monitoring & Observability

### Built-in Metrics
- WASM execution performance
- WebSocket connection counts
- Changefeed event rates
- Storage operation latency

### Logging
- Structured JSON logs
- Transaction tracing
- Error reporting with context
- Performance monitoring

## 🎯 Use Cases

### 1. Real-time Collaborative Applications
- Live document editing
- Multiplayer games
- Chat applications
- Collaborative design tools

### 2. IoT and Sensor Data
- Real-time sensor monitoring
- Device state synchronization
- Alert systems
- Data aggregation

### 3. Financial Applications
- Trading systems
- Risk monitoring
- Fraud detection
- Real-time analytics

### 4. Social Platforms
- Activity feeds
- Notification systems
- User interactions
- Content moderation

## 📈 Future Roadmap

### Short-term (Next 3 months)
- [ ] Raft consensus for clustering
- [ ] Enhanced query capabilities
- [ ] Performance optimizations
- [ ] More SDK language support

### Medium-term (3-6 months)
- [ ] Sharding for horizontal scaling
- [ ] Advanced subscription features
- [ ] Schema migration tools
- [ ] Backup and restore

### Long-term (6+ months)
- [ ] Geographic distribution
- [ ] Advanced analytics
- [ ] Plugin ecosystem
- [ ] Enterprise features

---

**InstantDB** - Real-time, programmable database with WebAssembly 🚀