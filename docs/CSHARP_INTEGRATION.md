# InstantDB C# Integration Guide

This guide shows C# developers how to integrate InstantDB into their projects for real-time data synchronization, particularly useful for Unity games, WPF applications, and ASP.NET services.

## 🚀 Quick Start

### 1. Install InstantDB CLI

Choose your preferred installation method:

```bash
# macOS (Homebrew)
brew install instantdb

# Windows (Chocolatey)
choco install instantdb

# Windows (Winget)
winget install instantdb

# Or download from releases
curl -L https://github.com/your-org/instantdb/releases/latest/download/instantdb-windows-x64.zip
```

### 2. Initialize Your Project

```bash
# Create a new InstantDB project optimized for C#
mkdir MyGameBackend
cd MyGameBackend

# Initialize with C# template
instantdb init --lang csharp --template unity-game

# This creates:
# ├── instantdb.config.json     # Project configuration
# ├── modules/                  # WASM modules directory
# │   └── gamelogic/           # Sample C# game logic module
# ├── schema.sql               # Database schema
# ├── scripts/                 # Build and deployment scripts
# └── client/                  # C# client examples
```

### 3. Define Your Game Schema

```sql
-- schema.sql
CREATE TABLE players (
    id INT64 PRIMARY KEY,
    name STRING NOT NULL,
    position_x FLOAT DEFAULT 0,
    position_y FLOAT DEFAULT 0,
    position_z FLOAT DEFAULT 0,
    health INT64 DEFAULT 100,
    score INT64 DEFAULT 0,
    team STRING,
    last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE game_sessions (
    id INT64 PRIMARY KEY,
    name STRING NOT NULL,
    max_players INT64 DEFAULT 10,
    current_players INT64 DEFAULT 0,
    status STRING DEFAULT 'waiting',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE player_actions (
    id INT64 PRIMARY KEY,
    player_id INT64 NOT NULL,
    session_id INT64 NOT NULL,
    action_type STRING NOT NULL,
    data JSON,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

Apply the schema:
```bash
instantdb schema apply schema.sql
```

### 4. Start the InstantDB Server

```bash
# Start as daemon for development
instantdb server start --dev

# Or start in foreground to see logs
instantdb server start --log-level debug
```

## 🎮 Unity Integration

### Add InstantDB to Unity Project

1. **Install the NuGet Package** (Unity 2021.2+):
```csharp
// In Unity Package Manager, add package from git URL:
// https://github.com/your-org/instantdb-unity.git
```

2. **Or download the Unity Package**:
   - Download `InstantDB.Unity.unitypackage` from [releases](https://github.com/your-org/instantdb/releases)
   - Import into your Unity project

### Unity GameManager Script

```csharp
using UnityEngine;
using InstantDB.Client;
using System.Threading.Tasks;

public class GameManager : MonoBehaviour
{
    [Header("InstantDB Settings")]
    public string serverUrl = "ws://localhost:8080";
    public bool autoConnect = true;

    private IInstantDBConnection _connection;

    // Game state
    public static Dictionary<int, PlayerController> Players = new Dictionary<int, PlayerController>();
    public static int LocalPlayerId { get; private set; }

    async void Start()
    {
        if (autoConnect)
        {
            await ConnectToInstantDB();
        }
    }

    public async Task ConnectToInstantDB()
    {
        try
        {
            Debug.Log("Connecting to InstantDB...");

            _connection = new InstantDBConnection(serverUrl);
            await _connection.ConnectAsync();

            // Register event handlers
            RegisterEventHandlers();

            // Subscribe to real-time updates
            await _connection.SubscribeToTable("players");
            await _connection.SubscribeToTable("game_sessions");
            await _connection.SubscribeToTable("player_actions");

            Debug.Log("Connected to InstantDB successfully!");
        }
        catch (System.Exception ex)
        {
            Debug.LogError($"Failed to connect to InstantDB: {ex.Message}");
        }
    }

    private void RegisterEventHandlers()
    {
        // Player events
        _connection.OnPlayerInsert += HandlePlayerJoin;
        _connection.OnPlayerUpdate += HandlePlayerUpdate;
        _connection.OnPlayerDelete += HandlePlayerLeave;

        // Game session events
        _connection.OnGameSessionInsert += HandleSessionCreated;
        _connection.OnGameSessionUpdate += HandleSessionUpdated;

        // Player action events
        _connection.OnPlayerActionInsert += HandlePlayerAction;
    }

    #region Event Handlers

    private void HandlePlayerJoin(Player player)
    {
        Debug.Log($"Player joined: {player.Name} (ID: {player.Id})");

        // Spawn player GameObject
        var playerPrefab = Resources.Load<GameObject>("PlayerPrefab");
        var playerObj = Instantiate(playerPrefab);

        var controller = playerObj.GetComponent<PlayerController>();
        controller.Initialize(player);

        Players[player.Id] = controller;

        // Position player
        playerObj.transform.position = new Vector3(
            player.PositionX,
            player.PositionY,
            player.PositionZ
        );
    }

    private void HandlePlayerUpdate(Player oldPlayer, Player newPlayer)
    {
        if (Players.TryGetValue(newPlayer.Id, out var controller))
        {
            controller.UpdateFromServer(newPlayer);
        }
    }

    private void HandlePlayerLeave(Player player)
    {
        Debug.Log($"Player left: {player.Name}");

        if (Players.TryGetValue(player.Id, out var controller))
        {
            Players.Remove(player.Id);
            Destroy(controller.gameObject);
        }
    }

    private void HandleSessionCreated(GameSession session)
    {
        Debug.Log($"New game session: {session.Name}");
        // Update UI to show available sessions
    }

    private void HandleSessionUpdated(GameSession oldSession, GameSession newSession)
    {
        Debug.Log($"Session updated: {newSession.Name} - {newSession.Status}");
        // Update session UI
    }

    private void HandlePlayerAction(PlayerAction action)
    {
        Debug.Log($"Player action: {action.ActionType} by player {action.PlayerId}");

        // Handle different action types
        switch (action.ActionType)
        {
            case "shoot":
                HandleShootAction(action);
                break;
            case "pickup":
                HandlePickupAction(action);
                break;
            case "chat":
                HandleChatAction(action);
                break;
        }
    }

    #endregion

    #region Public API

    public async Task JoinGame(string playerName)
    {
        if (_connection == null) return;

        // Execute reducer to join game
        var result = await _connection.ExecuteReducer("join_game", new
        {
            name = playerName,
            spawn_x = 0f,
            spawn_y = 0f,
            spawn_z = 0f
        });

        if (result.Success)
        {
            LocalPlayerId = result.GetValue<int>("player_id");
            Debug.Log($"Joined game as player {LocalPlayerId}");
        }
    }

    public async Task UpdatePlayerPosition(Vector3 position)
    {
        if (_connection == null || LocalPlayerId == 0) return;

        await _connection.ExecuteReducer("update_position", new
        {
            player_id = LocalPlayerId,
            x = position.x,
            y = position.y,
            z = position.z
        });
    }

    public async Task SendPlayerAction(string actionType, object data = null)
    {
        if (_connection == null || LocalPlayerId == 0) return;

        await _connection.ExecuteReducer("player_action", new
        {
            player_id = LocalPlayerId,
            action_type = actionType,
            data = data
        });
    }

    #endregion

    private void HandleShootAction(PlayerAction action)
    {
        // Parse shooting data and create effects
        var shootData = action.GetData<ShootData>();
        // Create bullet trail, muzzle flash, etc.
    }

    private void HandlePickupAction(PlayerAction action)
    {
        // Handle item pickup
        var pickupData = action.GetData<PickupData>();
        // Remove item from world, update inventory
    }

    private void HandleChatAction(PlayerAction action)
    {
        // Show chat message
        var chatData = action.GetData<ChatData>();
        // Update chat UI
    }

    void OnDestroy()
    {
        _connection?.Disconnect();
    }
}

// Data classes for actions
[System.Serializable]
public class ShootData
{
    public float[] origin;
    public float[] direction;
    public string weaponType;
}

[System.Serializable]
public class PickupData
{
    public int itemId;
    public string itemType;
}

[System.Serializable]
public class ChatData
{
    public string message;
    public string channel;
}
```

### PlayerController Script

```csharp
using UnityEngine;
using InstantDB.Client;

public class PlayerController : MonoBehaviour
{
    [Header("Player Settings")]
    public float moveSpeed = 5f;
    public float smoothTime = 0.3f;

    private Player _playerData;
    private Vector3 _targetPosition;
    private Vector3 _velocity;
    private bool _isLocalPlayer;

    public void Initialize(Player player)
    {
        _playerData = player;
        _isLocalPlayer = (player.Id == GameManager.LocalPlayerId);

        // Set initial position
        _targetPosition = new Vector3(
            player.PositionX,
            player.PositionY,
            player.PositionZ
        );

        transform.position = _targetPosition;

        // Configure based on local/remote
        if (_isLocalPlayer)
        {
            // Enable local player controls
            GetComponent<PlayerInput>().enabled = true;

            // Add camera follow
            var camera = Camera.main;
            if (camera != null)
            {
                camera.GetComponent<CameraFollow>().target = transform;
            }
        }
        else
        {
            // Disable input for remote players
            GetComponent<PlayerInput>().enabled = false;
        }
    }

    public void UpdateFromServer(Player newPlayerData)
    {
        _playerData = newPlayerData;

        if (!_isLocalPlayer)
        {
            // Update position for remote players
            _targetPosition = new Vector3(
                newPlayerData.PositionX,
                newPlayerData.PositionY,
                newPlayerData.PositionZ
            );
        }

        // Update UI elements
        UpdateHealthBar();
        UpdateNameTag();
    }

    void Update()
    {
        if (!_isLocalPlayer)
        {
            // Smooth movement for remote players
            transform.position = Vector3.SmoothDamp(
                transform.position,
                _targetPosition,
                ref _velocity,
                smoothTime
            );
        }
    }

    private void UpdateHealthBar()
    {
        var healthBar = GetComponentInChildren<HealthBar>();
        if (healthBar != null)
        {
            healthBar.SetHealth(_playerData.Health, 100);
        }
    }

    private void UpdateNameTag()
    {
        var nameTag = GetComponentInChildren<NameTag>();
        if (nameTag != null)
        {
            nameTag.SetName(_playerData.Name);
        }
    }
}
```

## 🌐 ASP.NET Core Integration

### Web API Backend

```csharp
using Microsoft.AspNetCore.Mvc;
using InstantDB.Client;

[ApiController]
[Route("api/[controller]")]
public class GameController : ControllerBase
{
    private readonly IInstantDBConnection _instantDB;

    public GameController(IInstantDBConnection instantDB)
    {
        _instantDB = instantDB;
    }

    [HttpPost("sessions")]
    public async Task<IActionResult> CreateSession([FromBody] CreateSessionRequest request)
    {
        var result = await _instantDB.ExecuteReducer("create_session", new
        {
            name = request.Name,
            max_players = request.MaxPlayers,
            game_mode = request.GameMode
        });

        if (result.Success)
        {
            return Ok(new { SessionId = result.GetValue<int>("session_id") });
        }

        return BadRequest(result.ErrorMessage);
    }

    [HttpGet("sessions")]
    public async Task<IActionResult> GetActiveSessions()
    {
        var sessions = await _instantDB.Query<GameSession>(
            "SELECT * FROM game_sessions WHERE status = 'active' ORDER BY created_at DESC"
        );

        return Ok(sessions);
    }

    [HttpPost("sessions/{sessionId}/join")]
    public async Task<IActionResult> JoinSession(int sessionId, [FromBody] JoinSessionRequest request)
    {
        var result = await _instantDB.ExecuteReducer("join_session", new
        {
            session_id = sessionId,
            player_name = request.PlayerName,
            team = request.Team
        });

        if (result.Success)
        {
            return Ok(new { PlayerId = result.GetValue<int>("player_id") });
        }

        return BadRequest(result.ErrorMessage);
    }
}

// Startup.cs
public void ConfigureServices(IServiceCollection services)
{
    services.AddControllers();

    // Add InstantDB
    services.AddInstantDB(options =>
    {
        options.ServerUrl = Configuration.GetConnectionString("InstantDB");
        options.AutoReconnect = true;
        options.ReconnectInterval = TimeSpan.FromSeconds(5);
    });
}
```

## 🔧 Advanced Configuration

### Custom Connection Options

```csharp
var options = new InstantDBOptions
{
    ServerUrl = "ws://localhost:8080",
    AutoReconnect = true,
    ReconnectInterval = TimeSpan.FromSeconds(5),
    MaxReconnectAttempts = 10,

    // Authentication
    AuthToken = "your-auth-token",

    // Compression
    EnableCompression = true,

    // Logging
    LogLevel = LogLevel.Information,

    // Timeouts
    ConnectionTimeout = TimeSpan.FromSeconds(30),
    RequestTimeout = TimeSpan.FromSeconds(15)
};

var connection = new InstantDBConnection(options);
```

### Dependency Injection Setup

```csharp
// Program.cs (ASP.NET Core 6+)
var builder = WebApplication.CreateBuilder(args);

builder.Services.AddInstantDB(builder.Configuration.GetSection("InstantDB"));

var app = builder.Build();
```

```json
// appsettings.json
{
  "InstantDB": {
    "ServerUrl": "ws://localhost:8080",
    "AutoReconnect": true,
    "ReconnectInterval": "00:00:05",
    "MaxReconnectAttempts": 10
  }
}
```

## 🔄 Real-time Patterns

### Event-Driven Architecture

```csharp
public class GameEventManager : MonoBehaviour
{
    public static event Action<Player> PlayerJoined;
    public static event Action<Player> PlayerLeft;
    public static event Action<PlayerAction> ActionPerformed;

    private void Start()
    {
        var connection = GetComponent<GameManager>().Connection;

        connection.OnPlayerInsert += (player) => PlayerJoined?.Invoke(player);
        connection.OnPlayerDelete += (player) => PlayerLeft?.Invoke(player);
        connection.OnPlayerActionInsert += (action) => ActionPerformed?.Invoke(action);
    }
}

// Usage in other scripts
public class UIManager : MonoBehaviour
{
    void Start()
    {
        GameEventManager.PlayerJoined += ShowPlayerJoinedNotification;
        GameEventManager.ActionPerformed += HandlePlayerAction;
    }

    void OnDestroy()
    {
        GameEventManager.PlayerJoined -= ShowPlayerJoinedNotification;
        GameEventManager.ActionPerformed -= HandlePlayerAction;
    }
}
```

### State Synchronization

```csharp
public class PlayerMovement : MonoBehaviour
{
    private Vector3 _lastSyncPosition;
    private float _syncInterval = 0.1f; // 10 times per second
    private float _lastSyncTime;

    void Update()
    {
        if (IsLocalPlayer && ShouldSync())
        {
            SyncPosition();
        }
    }

    private bool ShouldSync()
    {
        return Time.time - _lastSyncTime > _syncInterval &&
               Vector3.Distance(transform.position, _lastSyncPosition) > 0.01f;
    }

    private async void SyncPosition()
    {
        _lastSyncTime = Time.time;
        _lastSyncPosition = transform.position;

        await GameManager.Instance.UpdatePlayerPosition(transform.position);
    }
}
```

## 📊 Performance Optimization

### Efficient Updates

```csharp
public class BatchedUpdates : MonoBehaviour
{
    private Queue<PlayerUpdate> _pendingUpdates = new Queue<PlayerUpdate>();
    private float _batchInterval = 0.05f; // 20 times per second

    void Start()
    {
        InvokeRepeating(nameof(ProcessBatch), _batchInterval, _batchInterval);
    }

    public void QueueUpdate(PlayerUpdate update)
    {
        _pendingUpdates.Enqueue(update);
    }

    private async void ProcessBatch()
    {
        if (_pendingUpdates.Count == 0) return;

        var batch = new List<PlayerUpdate>();
        while (_pendingUpdates.Count > 0 && batch.Count < 10)
        {
            batch.Add(_pendingUpdates.Dequeue());
        }

        await GameManager.Instance.SendBatchedUpdates(batch);
    }
}
```

### Connection Management

```csharp
public class ConnectionManager : MonoBehaviour
{
    private IInstantDBConnection _connection;
    private bool _isReconnecting;

    async void Start()
    {
        await ConnectWithRetry();
    }

    private async Task ConnectWithRetry()
    {
        int attempts = 0;
        while (attempts < 5 && !_isConnected)
        {
            try
            {
                await _connection.ConnectAsync();
                _isReconnecting = false;
                break;
            }
            catch (Exception ex)
            {
                attempts++;
                Debug.LogWarning($"Connection attempt {attempts} failed: {ex.Message}");
                await Task.Delay(1000 * attempts); // Exponential backoff
            }
        }
    }

    private async void OnConnectionLost()
    {
        if (!_isReconnecting)
        {
            _isReconnecting = true;
            await ConnectWithRetry();
        }
    }
}
```

## 🚀 Deployment

### Production Configuration

```bash
# Production server configuration
instantdb server start \
  --port 8080 \
  --grpc-port 50051 \
  --data-dir /var/lib/instantdb \
  --log-level info \
  --daemon

# Monitor logs
instantdb logs --follow
```

### Health Checks

```csharp
public class HealthCheckService : MonoBehaviour
{
    public float checkInterval = 30f;

    void Start()
    {
        InvokeRepeating(nameof(CheckHealth), checkInterval, checkInterval);
    }

    private async void CheckHealth()
    {
        try
        {
            var status = await GameManager.Instance.Connection.GetServerStatus();

            if (!status.IsHealthy)
            {
                Debug.LogWarning("Server health check failed");
                // Trigger reconnection or fallback logic
            }
        }
        catch (Exception ex)
        {
            Debug.LogError($"Health check failed: {ex.Message}");
        }
    }
}
```

## 📚 Additional Resources

- **[Unity Package Documentation](https://github.com/your-org/instantdb-unity)**
- **[C# Client API Reference](https://docs.instantdb.com/csharp-client)**
- **[Example Unity Project](https://github.com/your-org/instantdb-unity-examples)**
- **[Performance Best Practices](https://docs.instantdb.com/performance)**
- **[Security Guidelines](https://docs.instantdb.com/security)**

## 💬 Community & Support

- **Discord**: [Join our community](https://discord.gg/instantdb)
- **GitHub Issues**: [Report bugs](https://github.com/your-org/instantdb/issues)
- **Stack Overflow**: Tag your questions with `instantdb`
- **Email Support**: [support@instantdb.com](mailto:support@instantdb.com)

---

**Ready to build real-time multiplayer games with C# and InstantDB! 🎮**