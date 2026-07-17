#if UNITY_5_3_OR_NEWER
using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using UnityEngine;
using System.Net.WebSockets;
using System.Text;
using System.Threading;

namespace OriginDB.Client
{
    /// <summary>
    /// Unity implementation of OriginDB connection using WebSocket.
    /// </summary>
    public class OriginDBConnection : IOriginDBConnection
    {
        private readonly OriginDBConnectionOptions _options;
        private ClientWebSocket _webSocket;
        private CancellationTokenSource _cancellationTokenSource;
        private readonly Queue<string> _messageQueue = new Queue<string>();
        private bool _isPaused = false;

        public string ServerUrl => _options.ServerUrl;
        public string ModuleName => _options.ModuleName;
        public ConnectionState State { get; private set; } = ConnectionState.Disconnected;
        public bool IsConnected => State == ConnectionState.Connected;
        public string ConnectionId { get; private set; }

        // Events
        public event Action OnConnected;
        public event Action<Exception> OnDisconnected;
        public event Action<Exception> OnError;
        public event Action OnReconnecting;

        // Data events
        public event Action<Player> OnPlayerInsert;
        public event Action<Player, Player> OnPlayerUpdate;
        public event Action<Player> OnPlayerDelete;

        public event Action<GameSession> OnGameSessionInsert;
        public event Action<GameSession, GameSession> OnGameSessionUpdate;
        public event Action<GameSession> OnGameSessionDelete;

        public event Action<PlayerAction> OnPlayerActionInsert;
        public event Action<PlayerAction, PlayerAction> OnPlayerActionUpdate;
        public event Action<PlayerAction> OnPlayerActionDelete;

        public OriginDBConnection(OriginDBConnectionOptions options)
        {
            _options = options ?? throw new ArgumentNullException(nameof(options));

            // Automatically convert HTTP URLs to WebSocket URLs
            _options.ServerUrl = ConvertToWebSocketUrl(_options.ServerUrl);
            ConnectionId = Guid.NewGuid().ToString();

            if (_options.EnableLogging)
            {
                Debug.Log($"[OriginDB] Connection created for {_options.ServerUrl}");
            }
        }

        public async Task ConnectAsync()
        {
            try
            {
                State = ConnectionState.Connecting;

                _cancellationTokenSource = new CancellationTokenSource();
                _webSocket = new ClientWebSocket();

                if (_options.EnableLogging)
                {
                    Debug.Log($"[OriginDB] Connecting to {_options.ServerUrl}...");
                }

                var uri = new Uri(_options.ServerUrl);
                await _webSocket.ConnectAsync(uri, _cancellationTokenSource.Token);

                State = ConnectionState.Connected;
                OnConnected?.Invoke();

                if (_options.EnableLogging)
                {
                    Debug.Log($"[OriginDB] Connected successfully to {_options.ServerUrl}");
                }

                // Start receiving messages
                _ = ReceiveLoop();
            }
            catch (Exception ex)
            {
                State = ConnectionState.Error;
                OnError?.Invoke(ex);

                if (_options.EnableLogging)
                {
                    Debug.LogError($"[OriginDB] Connection failed: {ex.Message}");
                }

                throw;
            }
        }

        public void Disconnect()
        {
            try
            {
                State = ConnectionState.Disconnected;

                _cancellationTokenSource?.Cancel();

                if (_webSocket?.State == WebSocketState.Open)
                {
                    _ = _webSocket.CloseAsync(WebSocketCloseStatus.NormalClosure, "Client disconnect", CancellationToken.None);
                }

                _webSocket?.Dispose();
                _cancellationTokenSource?.Dispose();

                if (_options.EnableLogging)
                {
                    Debug.Log($"[OriginDB] Disconnected from {_options.ServerUrl}");
                }

                OnDisconnected?.Invoke(null);
            }
            catch (Exception ex)
            {
                OnError?.Invoke(ex);
            }
        }

        public async Task SubscribeToTable(string tableName)
        {
            var message = new
            {
                type = "subscribe_table",
                table = tableName
            };

            await SendMessage(message);

            if (_options.EnableLogging)
            {
                Debug.Log($"[OriginDB] Subscribed to table: {tableName}");
            }
        }

        public async Task SubscribeToAllTables()
        {
            var message = new
            {
                type = "subscribe_to_all_tables"
            };

            await SendMessage(message);

            if (_options.EnableLogging)
            {
                Debug.Log("[OriginDB] Subscribed to all tables");
            }
        }

        public async Task<ReducerResult> ExecuteReducer(string reducerName, object args = null)
        {
            var message = new
            {
                type = "execute_reducer",
                module_name = _options.ModuleName,
                reducer_name = reducerName,
                args = args ?? new object()
            };

            await SendMessage(message);

            // In a real implementation, you would wait for a response
            // For now, return a mock successful result
            return new ReducerResult
            {
                Success = true,
                Data = new Dictionary<string, object>()
            };
        }

        public async Task<QueryResult> ExecuteSQL(string sql)
        {
            var message = new
            {
                type = "execute_sql",
                sql = sql
            };

            await SendMessage(message);

            // In a real implementation, you would wait for a response
            return new QueryResult
            {
                Success = true,
                Rows = new List<Dictionary<string, object>>(),
                RowsAffected = 0,
                ExecutionTime = TimeSpan.Zero
            };
        }

        public async Task<List<T>> Query<T>(string sql) where T : class
        {
            var result = await ExecuteSQL(sql);

            if (!result.Success)
            {
                throw new Exception(result.ErrorMessage);
            }

            var items = new List<T>();

            // In a real implementation, you would deserialize the results
            // For now, return empty list
            return items;
        }

        public async Task<ServerStatus> GetServerStatus()
        {
            var message = new
            {
                type = "get_status"
            };

            await SendMessage(message);

            // Mock response
            return new ServerStatus
            {
                IsHealthy = true,
                Version = "1.0.0",
                ConnectedClients = 1,
                Statistics = new Dictionary<string, object>()
            };
        }

        public void FrameTick()
        {
            if (_isPaused) return;

            // Process queued messages
            ProcessMessageQueue();
        }

        public void Pause()
        {
            _isPaused = true;
            if (_options.EnableLogging)
            {
                Debug.Log("[OriginDB] Connection paused");
            }
        }

        public void Resume()
        {
            _isPaused = false;
            if (_options.EnableLogging)
            {
                Debug.Log("[OriginDB] Connection resumed");
            }
        }

        private async Task SendMessage(object message)
        {
            if (_webSocket?.State != WebSocketState.Open)
            {
                throw new InvalidOperationException("WebSocket is not connected");
            }

            try
            {
                var json = Newtonsoft.Json.JsonConvert.SerializeObject(message);
                var bytes = Encoding.UTF8.GetBytes(json);
                var segment = new ArraySegment<byte>(bytes);

                await _webSocket.SendAsync(segment, WebSocketMessageType.Text, true, _cancellationTokenSource.Token);

                if (_options.EnableLogging)
                {
                    Debug.Log($"[OriginDB] Sent: {json}");
                }
            }
            catch (Exception ex)
            {
                OnError?.Invoke(ex);
                throw;
            }
        }

        private async Task ReceiveLoop()
        {
            var buffer = new byte[4096];

            try
            {
                while (_webSocket.State == WebSocketState.Open && !_cancellationTokenSource.Token.IsCancellationRequested)
                {
                    var result = await _webSocket.ReceiveAsync(new ArraySegment<byte>(buffer), _cancellationTokenSource.Token);

                    if (result.MessageType == WebSocketMessageType.Text)
                    {
                        var message = Encoding.UTF8.GetString(buffer, 0, result.Count);

                        // Queue message for processing on main thread
                        lock (_messageQueue)
                        {
                            _messageQueue.Enqueue(message);
                        }
                    }
                    else if (result.MessageType == WebSocketMessageType.Close)
                    {
                        break;
                    }
                }
            }
            catch (OperationCanceledException)
            {
                // Expected when cancelling
            }
            catch (Exception ex)
            {
                OnError?.Invoke(ex);
            }
            finally
            {
                if (State == ConnectionState.Connected)
                {
                    State = ConnectionState.Disconnected;
                    OnDisconnected?.Invoke(null);
                }
            }
        }

        private void ProcessMessageQueue()
        {
            lock (_messageQueue)
            {
                while (_messageQueue.Count > 0)
                {
                    var message = _messageQueue.Dequeue();
                    ProcessMessage(message);
                }
            }
        }

        private void ProcessMessage(string message)
        {
            try
            {
                if (_options.EnableLogging)
                {
                    Debug.Log($"[OriginDB] Received: {message}");
                }

                var messageObj = Newtonsoft.Json.JsonConvert.DeserializeObject<Dictionary<string, object>>(message);

                if (!messageObj.TryGetValue("type", out var typeObj) || !(typeObj is string messageType))
                {
                    return;
                }

                switch (messageType)
                {
                    case "changefeed_event":
                        ProcessChangefeedEvent(messageObj);
                        break;

                    case "initial_state":
                        ProcessInitialState(messageObj);
                        break;

                    case "error":
                        ProcessError(messageObj);
                        break;

                    default:
                        if (_options.EnableLogging)
                        {
                            Debug.LogWarning($"[OriginDB] Unknown message type: {messageType}");
                        }
                        break;
                }
            }
            catch (Exception ex)
            {
                OnError?.Invoke(ex);
            }
        }

        private void ProcessChangefeedEvent(Dictionary<string, object> messageObj)
        {
            try
            {
                if (!messageObj.TryGetValue("table", out var tableObj) || !(tableObj is string table))
                    return;

                if (!messageObj.TryGetValue("operation", out var operationObj) || !(operationObj is string operation))
                    return;

                switch (table.ToLower())
                {
                    case "players":
                        ProcessPlayerEvent(operation, messageObj);
                        break;

                    case "game_sessions":
                        ProcessGameSessionEvent(operation, messageObj);
                        break;

                    case "player_actions":
                        ProcessPlayerActionEvent(operation, messageObj);
                        break;
                }
            }
            catch (Exception ex)
            {
                OnError?.Invoke(ex);
            }
        }

        private void ProcessPlayerEvent(string operation, Dictionary<string, object> messageObj)
        {
            // In a real implementation, you would deserialize the actual data
            var mockPlayer = new Player
            {
                Id = 1,
                Name = "TestPlayer",
                PositionX = 0,
                PositionY = 0,
                PositionZ = 0,
                Health = 100,
                Score = 0,
                Team = "TeamA",
                LastSeen = DateTime.Now
            };

            switch (operation.ToUpper())
            {
                case "INSERT":
                    OnPlayerInsert?.Invoke(mockPlayer);
                    break;

                case "UPDATE":
                    OnPlayerUpdate?.Invoke(mockPlayer, mockPlayer);
                    break;

                case "DELETE":
                    OnPlayerDelete?.Invoke(mockPlayer);
                    break;
            }
        }

        private void ProcessGameSessionEvent(string operation, Dictionary<string, object> messageObj)
        {
            var mockSession = new GameSession
            {
                Id = 1,
                Name = "Test Session",
                MaxPlayers = 10,
                CurrentPlayers = 1,
                Status = "active",
                CreatedAt = DateTime.Now
            };

            switch (operation.ToUpper())
            {
                case "INSERT":
                    OnGameSessionInsert?.Invoke(mockSession);
                    break;

                case "UPDATE":
                    OnGameSessionUpdate?.Invoke(mockSession, mockSession);
                    break;

                case "DELETE":
                    OnGameSessionDelete?.Invoke(mockSession);
                    break;
            }
        }

        private void ProcessPlayerActionEvent(string operation, Dictionary<string, object> messageObj)
        {
            var mockAction = new PlayerAction
            {
                Id = 1,
                PlayerId = 1,
                SessionId = 1,
                ActionType = "move",
                Data = "{}",
                Timestamp = DateTime.Now
            };

            switch (operation.ToUpper())
            {
                case "INSERT":
                    OnPlayerActionInsert?.Invoke(mockAction);
                    break;

                case "UPDATE":
                    OnPlayerActionUpdate?.Invoke(mockAction, mockAction);
                    break;

                case "DELETE":
                    OnPlayerActionDelete?.Invoke(mockAction);
                    break;
            }
        }

        private void ProcessInitialState(Dictionary<string, object> messageObj)
        {
            if (_options.EnableLogging)
            {
                Debug.Log("[OriginDB] Received initial state");
            }

            // Process initial state data
            // This would populate the initial game state
        }

        private void ProcessError(Dictionary<string, object> messageObj)
        {
            if (messageObj.TryGetValue("message", out var messageValue) && messageValue is string errorMessage)
            {
                OnError?.Invoke(new Exception(errorMessage));
            }
        }

        /// <summary>
        /// Converts HTTP/HTTPS URLs to WebSocket URLs automatically.
        /// This allows developers to use familiar HTTP URLs like "http://localhost:8080"
        /// which get automatically converted to "ws://localhost:8080".
        /// </summary>
        private string ConvertToWebSocketUrl(string url)
        {
            if (string.IsNullOrEmpty(url))
                return url;

            // Already a WebSocket URL
            if (url.StartsWith("ws://") || url.StartsWith("wss://"))
                return url;

            // Convert HTTP to WebSocket
            if (url.StartsWith("http://"))
            {
                return url.Replace("http://", "ws://");
            }

            // Convert HTTPS to WebSocket Secure
            if (url.StartsWith("https://"))
            {
                return url.Replace("https://", "wss://");
            }

            // No protocol specified, assume HTTP and convert to WebSocket
            if (!url.Contains("://"))
            {
                return $"ws://{url}";
            }

            // Unknown protocol, return as-is and let WebSocket handle the error
            return url;
        }
    }
}
#endif