#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace InstantDB.Client
{
    /// <summary>
    /// Interface for InstantDB connections that integrates with Unity's lifecycle.
    /// </summary>
    public interface IInstantDBConnection
    {
        /// <summary>
        /// The server URL this connection is configured for.
        /// </summary>
        string ServerUrl { get; }

        /// <summary>
        /// The module name this connection is configured for.
        /// </summary>
        string ModuleName { get; }

        /// <summary>
        /// Current connection state.
        /// </summary>
        ConnectionState State { get; }

        /// <summary>
        /// Whether the connection is currently active.
        /// </summary>
        bool IsConnected { get; }

        /// <summary>
        /// Unique identifier for this connection instance.
        /// </summary>
        string ConnectionId { get; }

        // Connection events
        event Action OnConnected;
        event Action<Exception> OnDisconnected;
        event Action<Exception> OnError;
        event Action OnReconnecting;

        // Data events - these would be generated based on your schema
        event Action<Player> OnPlayerInsert;
        event Action<Player, Player> OnPlayerUpdate;
        event Action<Player> OnPlayerDelete;

        event Action<GameSession> OnGameSessionInsert;
        event Action<GameSession, GameSession> OnGameSessionUpdate;
        event Action<GameSession> OnGameSessionDelete;

        event Action<PlayerAction> OnPlayerActionInsert;
        event Action<PlayerAction, PlayerAction> OnPlayerActionUpdate;
        event Action<PlayerAction> OnPlayerActionDelete;

        /// <summary>
        /// Connect to the InstantDB server asynchronously.
        /// </summary>
        Task ConnectAsync();

        /// <summary>
        /// Disconnect from the server.
        /// </summary>
        void Disconnect();

        /// <summary>
        /// Subscribe to real-time updates for a specific table.
        /// </summary>
        /// <param name="tableName">The table to subscribe to</param>
        Task SubscribeToTable(string tableName);

        /// <summary>
        /// Subscribe to all tables for real-time updates.
        /// </summary>
        Task SubscribeToAllTables();

        /// <summary>
        /// Execute a reducer (WASM function) on the server.
        /// </summary>
        /// <param name="reducerName">Name of the reducer to execute</param>
        /// <param name="args">Arguments to pass to the reducer</param>
        Task<ReducerResult> ExecuteReducer(string reducerName, object args = null);

        /// <summary>
        /// Execute a SQL query on the server.
        /// </summary>
        /// <param name="sql">SQL query to execute</param>
        Task<QueryResult> ExecuteSQL(string sql);

        /// <summary>
        /// Query data from the server with a specific type.
        /// </summary>
        /// <typeparam name="T">Type to deserialize results to</typeparam>
        /// <param name="sql">SQL query to execute</param>
        Task<List<T>> Query<T>(string sql) where T : class;

        /// <summary>
        /// Get the current server status.
        /// </summary>
        Task<ServerStatus> GetServerStatus();

        /// <summary>
        /// Called every frame by Unity to process network events.
        /// </summary>
        void FrameTick();

        /// <summary>
        /// Pause the connection (e.g., when app loses focus).
        /// </summary>
        void Pause();

        /// <summary>
        /// Resume the connection (e.g., when app regains focus).
        /// </summary>
        void Resume();
    }

    /// <summary>
    /// Result of a reducer execution.
    /// </summary>
    public class ReducerResult
    {
        public bool Success { get; set; }
        public string ErrorMessage { get; set; }
        public Dictionary<string, object> Data { get; set; }

        public T GetValue<T>(string key)
        {
            if (Data != null && Data.TryGetValue(key, out var value))
            {
                if (value is T directValue)
                    return directValue;

                // Try to convert
                try
                {
                    return (T)Convert.ChangeType(value, typeof(T));
                }
                catch
                {
                    return default(T);
                }
            }
            return default(T);
        }
    }

    /// <summary>
    /// Result of a SQL query execution.
    /// </summary>
    public class QueryResult
    {
        public bool Success { get; set; }
        public string ErrorMessage { get; set; }
        public List<Dictionary<string, object>> Rows { get; set; }
        public int RowsAffected { get; set; }
        public TimeSpan ExecutionTime { get; set; }
    }

    /// <summary>
    /// Server status information.
    /// </summary>
    public class ServerStatus
    {
        public bool IsHealthy { get; set; }
        public string Version { get; set; }
        public int ConnectedClients { get; set; }
        public Dictionary<string, object> Statistics { get; set; }
    }
}

// Example data model classes that would be generated from your schema
namespace InstantDB.Client
{
    [System.Serializable]
    public class Player
    {
        public int Id { get; set; }
        public string Name { get; set; }
        public float PositionX { get; set; }
        public float PositionY { get; set; }
        public float PositionZ { get; set; }
        public int Health { get; set; }
        public int Score { get; set; }
        public string Team { get; set; }
        public DateTime LastSeen { get; set; }

        public UnityEngine.Vector3 Position
        {
            get => new UnityEngine.Vector3(PositionX, PositionY, PositionZ);
            set
            {
                PositionX = value.x;
                PositionY = value.y;
                PositionZ = value.z;
            }
        }
    }

    [System.Serializable]
    public class GameSession
    {
        public int Id { get; set; }
        public string Name { get; set; }
        public int MaxPlayers { get; set; }
        public int CurrentPlayers { get; set; }
        public string Status { get; set; }
        public DateTime CreatedAt { get; set; }
    }

    [System.Serializable]
    public class PlayerAction
    {
        public int Id { get; set; }
        public int PlayerId { get; set; }
        public int SessionId { get; set; }
        public string ActionType { get; set; }
        public string Data { get; set; }
        public DateTime Timestamp { get; set; }

        public T GetData<T>() where T : class
        {
            if (string.IsNullOrEmpty(Data))
                return null;

            try
            {
                return Newtonsoft.Json.JsonConvert.DeserializeObject<T>(Data);
            }
            catch
            {
                return null;
            }
        }
    }
}
#endif