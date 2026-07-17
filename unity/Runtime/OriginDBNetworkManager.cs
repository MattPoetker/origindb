#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using OriginDB.Client;
using UnityEngine;

namespace OriginDB.Unity
{
    /// <summary>
    /// Unity component that manages OriginDB connections and integrates with Unity's update loop.
    /// Attach this to a GameObject in your scene to use OriginDB.
    /// This is a singleton component - only one should exist per scene.
    /// </summary>
    public class OriginDBNetworkManager : MonoBehaviour
    {
        [Header("Server Configuration")]
        [SerializeField, Tooltip("OriginDB server WebSocket URL")]
        private string serverUrl = "ws://localhost:8080";

        [SerializeField, Tooltip("Module name for this application")]
        private string moduleName = "default";

        [SerializeField, Tooltip("Automatically connect on Start")]
        private bool autoConnect = true;

        [SerializeField, Tooltip("Auto-reconnect on connection loss")]
        private bool autoReconnect = true;

        [SerializeField, Tooltip("Maximum reconnection attempts")]
        private int maxReconnectAttempts = 5;

        [SerializeField, Tooltip("Reconnection interval in seconds")]
        private float reconnectInterval = 5f;

        [Header("Authentication")]
        [SerializeField, Tooltip("Authentication token (optional)")]
        private string authToken = "";

        [Header("Debug")]
        [SerializeField, Tooltip("Enable debug logging")]
        private bool enableDebugLogging = true;

        [SerializeField, Tooltip("Log connection events")]
        private bool logConnectionEvents = true;

        // Singleton instance
        internal static OriginDBNetworkManager _instance;

        // Active connections management
        private readonly List<IOriginDBConnection> activeConnections = new();

        // Connection state
        private bool _isInitialized = false;

        // Public properties
        public static OriginDBNetworkManager Instance => _instance;
        public string ServerUrl => serverUrl;
        public string ModuleName => moduleName;
        public bool IsInitialized => _isInitialized;
        public int ActiveConnectionCount => activeConnections.Count;

        // Events
        public static event Action<OriginDBNetworkManager> OnManagerInitialized;
        public static event Action<IOriginDBConnection> OnConnectionAdded;
        public static event Action<IOriginDBConnection> OnConnectionRemoved;

        private void Awake()
        {
            // Ensure singleton pattern
            if (_instance != null && _instance != this)
            {
                if (enableDebugLogging)
                {
                    Debug.LogError("[OriginDB] OriginDBNetworkManager is a singleton and should only be attached once. Destroying duplicate instance.");
                }
                Destroy(gameObject);
                return;
            }

            _instance = this;

            // Don't destroy on load if this is the main instance
            DontDestroyOnLoad(gameObject);

            if (enableDebugLogging)
            {
                Debug.Log("[OriginDB] Network Manager initialized");
            }
        }

        private void Start()
        {
            _isInitialized = true;
            OnManagerInitialized?.Invoke(this);

            if (autoConnect)
            {
                CreateDefaultConnection();
            }
        }

        /// <summary>
        /// Creates a default connection using the configured server URL and module name.
        /// </summary>
        /// <returns>The created connection instance</returns>
        public IOriginDBConnection CreateDefaultConnection()
        {
            var options = new OriginDBConnectionOptions
            {
                ServerUrl = serverUrl,
                ModuleName = moduleName,
                AuthToken = !string.IsNullOrEmpty(authToken) ? authToken : null,
                AutoReconnect = autoReconnect,
                MaxReconnectAttempts = maxReconnectAttempts,
                ReconnectInterval = TimeSpan.FromSeconds(reconnectInterval),
                EnableLogging = enableDebugLogging
            };

            var connection = new OriginDBConnection(options);

            if (AddConnection(connection))
            {
                // Auto-connect if enabled
                _ = ConnectAsync(connection);
            }

            return connection;
        }

        /// <summary>
        /// Creates a connection with custom options.
        /// </summary>
        /// <param name="options">Connection configuration options</param>
        /// <returns>The created connection instance</returns>
        public IOriginDBConnection CreateConnection(OriginDBConnectionOptions options)
        {
            var connection = new OriginDBConnection(options);
            AddConnection(connection);
            return connection;
        }

        /// <summary>
        /// Adds a connection to be managed by this network manager.
        /// </summary>
        /// <param name="connection">The connection to add</param>
        /// <returns>True if added successfully, false if already exists</returns>
        public bool AddConnection(IOriginDBConnection connection)
        {
            if (connection == null)
            {
                Debug.LogWarning("[OriginDB] Attempted to add null connection");
                return false;
            }

            if (activeConnections.Contains(connection))
            {
                if (enableDebugLogging)
                {
                    Debug.LogWarning("[OriginDB] Connection already exists in manager");
                }
                return false;
            }

            activeConnections.Add(connection);

            // Subscribe to connection events
            connection.OnConnected += () => HandleConnectionEvent(connection, "Connected");
            connection.OnDisconnected += (ex) => HandleConnectionEvent(connection, $"Disconnected: {ex?.Message}");
            connection.OnError += (ex) => HandleConnectionEvent(connection, $"Error: {ex.Message}");

            OnConnectionAdded?.Invoke(connection);

            if (logConnectionEvents)
            {
                Debug.Log($"[OriginDB] Added connection to {connection.ServerUrl}. Total connections: {activeConnections.Count}");
            }

            return true;
        }

        /// <summary>
        /// Removes a connection from management.
        /// </summary>
        /// <param name="connection">The connection to remove</param>
        /// <returns>True if removed successfully</returns>
        public bool RemoveConnection(IOriginDBConnection connection)
        {
            if (connection == null) return false;

            bool removed = activeConnections.Remove(connection);

            if (removed)
            {
                OnConnectionRemoved?.Invoke(connection);

                if (logConnectionEvents)
                {
                    Debug.Log($"[OriginDB] Removed connection. Total connections: {activeConnections.Count}");
                }
            }

            return removed;
        }

        /// <summary>
        /// Gets all active connections.
        /// </summary>
        /// <returns>Read-only list of active connections</returns>
        public IReadOnlyList<IOriginDBConnection> GetActiveConnections()
        {
            return activeConnections.AsReadOnly();
        }

        /// <summary>
        /// Connects the specified connection asynchronously.
        /// </summary>
        /// <param name="connection">The connection to connect</param>
        public async System.Threading.Tasks.Task ConnectAsync(IOriginDBConnection connection)
        {
            try
            {
                if (logConnectionEvents)
                {
                    Debug.Log($"[OriginDB] Attempting to connect to {connection.ServerUrl}...");
                }

                await connection.ConnectAsync();
            }
            catch (Exception ex)
            {
                Debug.LogError($"[OriginDB] Failed to connect: {ex.Message}");
                throw;
            }
        }

        /// <summary>
        /// Disconnects all active connections.
        /// </summary>
        public void DisconnectAll()
        {
            ForEachConnection(connection => connection.Disconnect());
        }

        /// <summary>
        /// Gets connection statistics for debugging.
        /// </summary>
        /// <returns>Dictionary of connection statistics</returns>
        public Dictionary<string, object> GetConnectionStats()
        {
            var stats = new Dictionary<string, object>
            {
                ["TotalConnections"] = activeConnections.Count,
                ["ConnectedCount"] = 0,
                ["DisconnectedCount"] = 0,
                ["ErrorCount"] = 0
            };

            foreach (var connection in activeConnections)
            {
                switch (connection.State)
                {
                    case ConnectionState.Connected:
                        stats["ConnectedCount"] = (int)stats["ConnectedCount"] + 1;
                        break;
                    case ConnectionState.Disconnected:
                        stats["DisconnectedCount"] = (int)stats["DisconnectedCount"] + 1;
                        break;
                    case ConnectionState.Error:
                        stats["ErrorCount"] = (int)stats["ErrorCount"] + 1;
                        break;
                }
            }

            return stats;
        }

        private void ForEachConnection(Action<IOriginDBConnection> action)
        {
            // Reverse-iterate to handle modifications during iteration
            // (e.g., disconnect calls that remove connections)
            for (var i = activeConnections.Count - 1; i >= 0; i--)
            {
                if (i < activeConnections.Count) // Double-check bounds
                {
                    action(activeConnections[i]);
                }
            }
        }

        private void HandleConnectionEvent(IOriginDBConnection connection, string eventMessage)
        {
            if (logConnectionEvents)
            {
                Debug.Log($"[OriginDB] Connection Event: {eventMessage}");
            }
        }

        // Unity lifecycle methods
        private void Update()
        {
            // Process frame tick for all connections
            ForEachConnection(connection => connection.FrameTick());
        }

        private void OnApplicationPause(bool pauseStatus)
        {
            if (pauseStatus)
            {
                // Pause connections
                ForEachConnection(connection => connection.Pause());
            }
            else
            {
                // Resume connections
                ForEachConnection(connection => connection.Resume());
            }
        }

        private void OnApplicationFocus(bool hasFocus)
        {
            if (!hasFocus)
            {
                ForEachConnection(connection => connection.Pause());
            }
            else
            {
                ForEachConnection(connection => connection.Resume());
            }
        }

        private void OnDestroy()
        {
            if (logConnectionEvents)
            {
                Debug.Log("[OriginDB] Network Manager destroying, disconnecting all connections");
            }

            // Disconnect all connections
            ForEachConnection(connection => connection.Disconnect());

            // Clear the instance if this is the main instance
            if (_instance == this)
            {
                _instance = null;
            }
        }

        private void OnValidate()
        {
            // Validate server URL format
            if (!string.IsNullOrEmpty(serverUrl) && !serverUrl.StartsWith("ws://") && !serverUrl.StartsWith("wss://"))
            {
                Debug.LogWarning("[OriginDB] Server URL should start with 'ws://' or 'wss://'");
            }

            // Validate reconnection settings
            if (reconnectInterval < 1f)
            {
                reconnectInterval = 1f;
            }

            if (maxReconnectAttempts < 0)
            {
                maxReconnectAttempts = 0;
            }
        }
    }

    /// <summary>
    /// Configuration options for OriginDB connections.
    /// </summary>
    [System.Serializable]
    public class OriginDBConnectionOptions
    {
        public string ServerUrl { get; set; } = "ws://localhost:8080";
        public string ModuleName { get; set; } = "default";
        public string AuthToken { get; set; }
        public bool AutoReconnect { get; set; } = true;
        public int MaxReconnectAttempts { get; set; } = 5;
        public TimeSpan ReconnectInterval { get; set; } = TimeSpan.FromSeconds(5);
        public bool EnableLogging { get; set; } = true;
        public TimeSpan ConnectionTimeout { get; set; } = TimeSpan.FromSeconds(30);
        public TimeSpan RequestTimeout { get; set; } = TimeSpan.FromSeconds(15);
        public bool EnableCompression { get; set; } = true;
    }

    /// <summary>
    /// Represents the state of an OriginDB connection.
    /// </summary>
    public enum ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
        Error
    }
}
#endif