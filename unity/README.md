# InstantDB for Unity

Real-time database integration for Unity with WebAssembly module support. Build multiplayer games with seamless data synchronization.

## ✨ Features

- **🔄 Real-time Data Sync**: Automatic synchronization of game state across all clients
- **🎮 Unity Integration**: Native Unity components with inspector configuration
- **⚡ WebSocket Connection**: High-performance, low-latency networking
- **🔧 Easy Setup**: Drop-in components with minimal configuration required
- **🛡️ Auto-Reconnection**: Robust connection management with automatic retry
- **📊 Connection Management**: Singleton pattern with lifecycle integration
- **🎯 Event-Driven**: Clean event system for reactive programming
- **🔍 Debug Support**: Comprehensive logging and debugging tools

## 🚀 Quick Start

### 1. Installation

**Option A: Unity Package Manager (Recommended)**
1. Open Unity Package Manager
2. Click "+" → "Add package from git URL"
3. Enter: `https://github.com/your-org/instantdb-unity.git`

**Option B: Download Package**
1. Download `InstantDB.Unity.unitypackage` from [releases](https://github.com/your-org/instantdb/releases)
2. Import into your Unity project

### 2. Setup Scene

1. **Add Network Manager**: Create an empty GameObject and add the `InstantDBNetworkManager` component
2. **Configure Connection**: Set your server URL and module name in the inspector
3. **Add Game Manager**: Create your game manager script (see example below)

### 3. Basic Usage

```csharp
using UnityEngine;
using InstantDB.Unity;
using InstantDB.Client;

public class GameManager : MonoBehaviour
{
    private IInstantDBConnection connection;

    async void Start()
    {
        // Get connection from network manager
        connection = InstantDBNetworkManager.Instance.CreateDefaultConnection();

        // Subscribe to events
        connection.OnPlayerInsert += HandlePlayerJoined;
        connection.OnPlayerUpdate += HandlePlayerUpdated;

        // Connect and subscribe to tables
        await connection.ConnectAsync();
        await connection.SubscribeToTable("players");
    }

    private void HandlePlayerJoined(Player player)
    {
        Debug.Log($"Player joined: {player.Name}");
        // Spawn player in game world
    }

    private void HandlePlayerUpdated(Player oldPlayer, Player newPlayer)
    {
        Debug.Log($"Player updated: {newPlayer.Name}");
        // Update player position/state
    }
}
```

## 📋 Components

### InstantDBNetworkManager

The core component that manages connections and integrates with Unity's lifecycle.

**Key Features:**
- Singleton pattern - only one per scene
- Automatic connection management
- Unity lifecycle integration (Update, OnDestroy, etc.)
- Connection pooling and cleanup

**Inspector Settings:**
- **Server URL**: Server endpoint (http://localhost:8080, automatically converts to WebSocket)
- **Module Name**: Application module identifier
- **Auto Connect**: Connect automatically on Start
- **Auto Reconnect**: Reconnect on connection loss
- **Debug Logging**: Enable debug output

### InstantDBConfig (ScriptableObject)

Configuration asset for storing connection settings across environments.

**Environment Profiles:**
- Local Development
- Staging
- Production

**Create Config Asset:**
Right-click in Project → Create → InstantDB → Configuration

## 🔌 Connection Management

### Creating Connections

```csharp
// Use default configuration
var connection = InstantDBNetworkManager.Instance.CreateDefaultConnection();

// Use custom options
var options = new InstantDBConnectionOptions
{
    ServerUrl = "http://localhost:8080",  // Automatically converted to ws://localhost:8080
    ModuleName = "mygame",
    AutoReconnect = true,
    MaxReconnectAttempts = 5
};
var connection = InstantDBNetworkManager.Instance.CreateConnection(options);

// Use ScriptableObject config
var connection = InstantDBNetworkManager.Instance.CreateConnection(config.CreateConnectionOptions());
```

### Connection Events

```csharp
connection.OnConnected += () => Debug.Log("Connected!");
connection.OnDisconnected += (ex) => Debug.Log($"Disconnected: {ex?.Message}");
connection.OnError += (ex) => Debug.LogError($"Error: {ex.Message}");
connection.OnReconnecting += () => Debug.Log("Reconnecting...");
```

## 📊 Data Synchronization

### Table Subscriptions

```csharp
// Subscribe to specific table
await connection.SubscribeToTable("players");

// Subscribe to all tables
await connection.SubscribeToAllTables();
```

### Event Handlers

```csharp
// Player events
connection.OnPlayerInsert += (player) => { /* Player joined */ };
connection.OnPlayerUpdate += (oldPlayer, newPlayer) => { /* Player updated */ };
connection.OnPlayerDelete += (player) => { /* Player left */ };

// Game session events
connection.OnGameSessionInsert += (session) => { /* Session created */ };
connection.OnGameSessionUpdate += (oldSession, newSession) => { /* Session updated */ };

// Custom action events
connection.OnPlayerActionInsert += (action) => { /* Action performed */ };
```

### Executing Reducers

```csharp
// Join game
var result = await connection.ExecuteReducer("join_game", new
{
    name = "PlayerName",
    spawn_x = 0f,
    spawn_y = 0f,
    spawn_z = 0f
});

// Send player action
await connection.ExecuteReducer("player_action", new
{
    player_id = localPlayerId,
    action_type = "shoot",
    data = new { target_x = 10f, target_y = 0f, target_z = 5f }
});
```

### SQL Queries

```csharp
// Execute raw SQL
var result = await connection.ExecuteSQL("SELECT * FROM players WHERE team = 'red'");

// Typed queries
var players = await connection.Query<Player>("SELECT * FROM players WHERE active = true");
```

## 🎮 Game Integration Patterns

### Player Management

```csharp
public class PlayerManager : MonoBehaviour
{
    private Dictionary<int, GameObject> players = new Dictionary<int, GameObject>();

    void Start()
    {
        var connection = InstantDBNetworkManager.Instance.CreateDefaultConnection();
        connection.OnPlayerInsert += SpawnPlayer;
        connection.OnPlayerUpdate += UpdatePlayer;
        connection.OnPlayerDelete += DespawnPlayer;
    }

    private void SpawnPlayer(Player player)
    {
        var playerObj = Instantiate(playerPrefab);
        playerObj.transform.position = player.Position;
        players[player.Id] = playerObj;
    }

    private void UpdatePlayer(Player oldPlayer, Player newPlayer)
    {
        if (players.TryGetValue(newPlayer.Id, out var playerObj))
        {
            playerObj.transform.position = newPlayer.Position;
        }
    }

    private void DespawnPlayer(Player player)
    {
        if (players.TryGetValue(player.Id, out var playerObj))
        {
            players.Remove(player.Id);
            Destroy(playerObj);
        }
    }
}
```

### Movement Synchronization

```csharp
public class PlayerMovement : MonoBehaviour
{
    private Vector3 lastSyncPosition;
    private float syncInterval = 0.1f; // 10 FPS
    private float lastSyncTime;

    void Update()
    {
        if (isLocalPlayer && ShouldSync())
        {
            SyncPosition();
        }
    }

    private bool ShouldSync()
    {
        return Time.time - lastSyncTime > syncInterval &&
               Vector3.Distance(transform.position, lastSyncPosition) > 0.01f;
    }

    private async void SyncPosition()
    {
        lastSyncTime = Time.time;
        lastSyncPosition = transform.position;

        await connection.ExecuteReducer("update_position", new
        {
            player_id = playerId,
            x = transform.position.x,
            y = transform.position.y,
            z = transform.position.z
        });
    }
}
```

## 🔧 Configuration

### Environment Setup

```csharp
// Development
config.serverUrl = "http://localhost:8080";  // Converts to ws://localhost:8080
config.moduleName = "dev";
config.enableDebugLogging = true;

// Production
config.serverUrl = "https://api.yourgame.com";  // Converts to wss://api.yourgame.com
config.moduleName = "production";
config.enableDebugLogging = false;
```

### Performance Tuning

```csharp
var options = new InstantDBConnectionOptions
{
    // Network settings
    ConnectionTimeout = TimeSpan.FromSeconds(30),
    RequestTimeout = TimeSpan.FromSeconds(15),

    // Reconnection
    AutoReconnect = true,
    MaxReconnectAttempts = 5,
    ReconnectInterval = TimeSpan.FromSeconds(5),

    // Performance
    EnableCompression = true,
    MaxMessageQueue = 100
};
```

## 🐛 Debugging

### Enable Debug Logging

```csharp
// In inspector or code
config.enableDebugLogging = true;
config.logConnectionEvents = true;
config.logMessages = true; // Log all WebSocket messages
```

### Connection Diagnostics

```csharp
// Check connection state
Debug.Log($"Connected: {connection.IsConnected}");
Debug.Log($"State: {connection.State}");

// Get connection statistics
var stats = InstantDBNetworkManager.Instance.GetConnectionStats();
Debug.Log($"Total connections: {stats["TotalConnections"]}");
Debug.Log($"Connected: {stats["ConnectedCount"]}");
```

### Common Issues

**Connection Fails**
- Check server URL format (must start with ws:// or wss://)
- Verify InstantDB server is running
- Check firewall/network settings

**Events Not Firing**
- Ensure you've subscribed to the table
- Check that connection is established
- Verify event handler registration

**Performance Issues**
- Reduce sync frequency for position updates
- Use batched updates for multiple changes
- Enable compression for large messages

## 📚 API Reference

### IInstantDBConnection

```csharp
interface IInstantDBConnection
{
    // Properties
    string ServerUrl { get; }
    ConnectionState State { get; }
    bool IsConnected { get; }

    // Events
    event Action OnConnected;
    event Action<Exception> OnDisconnected;
    event Action<Exception> OnError;

    // Methods
    Task ConnectAsync();
    void Disconnect();
    Task SubscribeToTable(string tableName);
    Task<ReducerResult> ExecuteReducer(string name, object args);
    Task<QueryResult> ExecuteSQL(string sql);
    Task<List<T>> Query<T>(string sql);
}
```

### Data Models

```csharp
public class Player
{
    public int Id { get; set; }
    public string Name { get; set; }
    public Vector3 Position { get; set; }
    public int Health { get; set; }
    public int Score { get; set; }
    public string Team { get; set; }
}

public class GameSession
{
    public int Id { get; set; }
    public string Name { get; set; }
    public int MaxPlayers { get; set; }
    public int CurrentPlayers { get; set; }
    public string Status { get; set; }
}
```

## 📖 Examples

Check the `Samples~` folder for complete examples:

- **Basic Connection**: Simple connection and event handling
- **Multiplayer Game**: Full multiplayer game with player sync
- **Chat System**: Real-time chat implementation

## 🆘 Support

- **Documentation**: [https://docs.instantdb.com/unity](https://docs.instantdb.com/unity)
- **GitHub Issues**: [Report bugs](https://github.com/your-org/instantdb/issues)
- **Discord**: [Join community](https://discord.gg/instantdb)
- **Email**: [support@instantdb.com](mailto:support@instantdb.com)

## 📄 License

MIT License - see [LICENSE](LICENSE) file for details.

---

**Build amazing real-time multiplayer games with InstantDB! 🚀🎮**