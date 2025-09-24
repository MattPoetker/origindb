# Basic Connection Example

This sample demonstrates how to establish a basic connection to InstantDB and handle connection events.

## What This Sample Shows

- Setting up an InstantDBNetworkManager
- Creating and configuring connections
- Handling connection events (connected, disconnected, error)
- Basic UI integration with Unity
- Player join/leave functionality
- Real-time data synchronization

## Files

- **SampleGameManager.cs**: Main game manager that demonstrates InstantDB integration
- **BasicConnection.unity**: Sample scene with configured UI and network manager
- **InstantDBConfig_Sample.asset**: Sample configuration asset

## How to Use

1. Import this sample into your project from the Package Manager
2. Open the `BasicConnection.unity` scene
3. Configure the `InstantDBConfig_Sample` asset with your server settings
4. Play the scene to test the connection

## Key Features Demonstrated

### Connection Management
```csharp
// Create connection with configuration
_connection = InstantDBNetworkManager.Instance.CreateConnection(config.CreateConnectionOptions());

// Subscribe to events
_connection.OnConnected += HandleConnected;
_connection.OnDisconnected += HandleDisconnected;
_connection.OnError += HandleConnectionError;
```

### Player Events
```csharp
// Subscribe to player data events
_connection.OnPlayerInsert += HandlePlayerJoined;
_connection.OnPlayerUpdate += HandlePlayerUpdated;
_connection.OnPlayerDelete += HandlePlayerLeft;
```

### Joining Game
```csharp
// Execute reducer to join game
var result = await _connection.ExecuteReducer("join_game", new
{
    name = playerName,
    spawn_x = 0f,
    spawn_y = 0f,
    spawn_z = 0f
});
```

### Real-time Updates
```csharp
// Subscribe to table updates
await _connection.SubscribeToTable("players");
await _connection.SubscribeToTable("player_actions");
```

## Requirements

- InstantDB server running (default: ws://localhost:8080)
- Unity 2021.3 or later
- Newtonsoft JSON package

## Troubleshooting

**Connection Fails**
- Check that your InstantDB server is running
- Verify the server URL in the configuration
- Check firewall/network settings

**No Player Events**
- Ensure you've called `SubscribeToTable("players")`
- Check that the connection is established
- Verify your reducers are working on the server

**UI Not Updating**
- Check that UI elements are assigned in the inspector
- Verify event handlers are properly subscribed