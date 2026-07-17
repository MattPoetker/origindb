# Changelog

All notable changes to OriginDB for Unity will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2024-12-22

### Added
- Initial release of OriginDB for Unity
- Real-time database integration with WebSocket connections
- Unity singleton pattern network manager (`OriginDBNetworkManager`)
- Comprehensive connection management with automatic reconnection
- ScriptableObject configuration system (`OriginDBConfig`) with environment profiles
- Data models for common game entities (`Player`, `GameSession`, `PlayerAction`)
- Unity lifecycle integration (Update loop, pause/resume handling)
- Event-driven architecture for real-time data synchronization
- Connection pooling and cleanup management
- Comprehensive error handling and logging
- Unity Package Manager integration
- Sample scenes and examples
- Editor tools and custom inspectors
- Support for Unity 2021.3+
- Dependency on Newtonsoft JSON 3.0.2

### Features
- **Connection Interface**: `IOriginDBConnection` with WebSocket implementation
- **Network Manager**: Singleton `OriginDBNetworkManager` with Unity lifecycle integration
- **Configuration**: `OriginDBConfig` ScriptableObject with environment profiles
- **Data Models**: Pre-built models for `Player`, `GameSession`, and `PlayerAction`
- **Editor Tools**: Custom inspectors and menu items for better development experience
- **Samples**: Basic connection example with UI integration
- **Events**: Comprehensive event system for connection and data events
- **Reconnection**: Automatic reconnection with configurable retry logic
- **Authentication**: Support for optional authentication tokens
- **Compression**: Optional message compression for better performance
- **Debug Tools**: Extensive logging and debugging capabilities

### Connection Events
- `OnConnected`: Fired when connection is established
- `OnDisconnected`: Fired when connection is lost
- `OnError`: Fired when connection errors occur
- `OnReconnecting`: Fired when attempting to reconnect

### Data Events
- `OnPlayerInsert`: New player joined
- `OnPlayerUpdate`: Player data updated
- `OnPlayerDelete`: Player left
- `OnGameSessionInsert`: New game session created
- `OnGameSessionUpdate`: Game session updated
- `OnPlayerActionInsert`: Player action performed

### API Methods
- `ConnectAsync()`: Establish connection to server
- `Disconnect()`: Close connection
- `SubscribeToTable(tableName)`: Subscribe to real-time table updates
- `SubscribeToAllTables()`: Subscribe to all table updates
- `ExecuteReducer(name, args)`: Execute server-side reducer function
- `ExecuteSQL(sql)`: Execute raw SQL query
- `Query<T>(sql)`: Execute typed SQL query

### Configuration Options
- Server URL (WebSocket endpoint)
- Module name (application identifier)
- Auto-connect and auto-reconnect settings
- Connection and request timeouts
- Authentication token support
- Compression and performance settings
- Debug logging options
- Environment profiles for different deployment targets

### Unity Integration
- Singleton pattern with validation
- Unity Update loop integration
- Pause/resume handling for mobile
- Editor menu items and utilities
- Custom inspectors for better UX
- Package Manager integration
- Sample scenes and documentation

### Requirements
- Unity 2021.3 or later
- Newtonsoft JSON 3.0.2
- OriginDB server
- .NET Framework 4.7.1+ or .NET Standard 2.1+

### Supported Platforms
- Windows (Standalone)
- macOS (Standalone)
- Linux (Standalone)
- iOS
- Android
- WebGL (with limitations)

### Known Limitations
- WebGL has limited WebSocket support
- iOS requires specific networking permissions
- Maximum message size depends on WebSocket implementation

## [Unreleased]

### Planned
- Unity multiplayer netcode integration
- Additional data model templates
- Performance monitoring tools
- Advanced debugging features
- More sample scenes (chat, multiplayer game)
- Unity Cloud Build integration
- Addressable asset system integration