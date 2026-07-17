#if UNITY_5_3_OR_NEWER
using System;

namespace OriginDB.Unity
{
    /// <summary>
    /// Configuration options for OriginDB connections.
    /// </summary>
    [Serializable]
    public class OriginDBConnectionOptions
    {
        /// <summary>
        /// Server URL (supports http://, https://, ws://, wss://, or hostname)
        /// HTTP URLs are automatically converted to WebSocket URLs
        /// </summary>
        public string ServerUrl { get; set; } = "http://localhost:8080";

        /// <summary>
        /// Module name for this application
        /// </summary>
        public string ModuleName { get; set; } = "default";

        /// <summary>
        /// Authentication token (optional)
        /// </summary>
        public string AuthToken { get; set; }

        /// <summary>
        /// Enable automatic reconnection on connection loss
        /// </summary>
        public bool AutoReconnect { get; set; } = true;

        /// <summary>
        /// Maximum number of reconnection attempts
        /// </summary>
        public int MaxReconnectAttempts { get; set; } = 5;

        /// <summary>
        /// Time between reconnection attempts
        /// </summary>
        public TimeSpan ReconnectInterval { get; set; } = TimeSpan.FromSeconds(5);

        /// <summary>
        /// Connection timeout
        /// </summary>
        public TimeSpan ConnectionTimeout { get; set; } = TimeSpan.FromSeconds(30);

        /// <summary>
        /// Request timeout
        /// </summary>
        public TimeSpan RequestTimeout { get; set; } = TimeSpan.FromSeconds(15);

        /// <summary>
        /// Enable debug logging
        /// </summary>
        public bool EnableLogging { get; set; } = true;

        /// <summary>
        /// Enable message compression
        /// </summary>
        public bool EnableCompression { get; set; } = true;

        /// <summary>
        /// Maximum messages to queue before dropping
        /// </summary>
        public int MaxMessageQueue { get; set; } = 100;

        /// <summary>
        /// Creates a copy of these options with default values for any null properties.
        /// </summary>
        public OriginDBConnectionOptions WithDefaults()
        {
            return new OriginDBConnectionOptions
            {
                ServerUrl = !string.IsNullOrEmpty(ServerUrl) ? ServerUrl : "http://localhost:8080",
                ModuleName = !string.IsNullOrEmpty(ModuleName) ? ModuleName : "default",
                AuthToken = AuthToken,
                AutoReconnect = AutoReconnect,
                MaxReconnectAttempts = Math.Max(0, MaxReconnectAttempts),
                ReconnectInterval = ReconnectInterval.TotalSeconds > 0 ? ReconnectInterval : TimeSpan.FromSeconds(5),
                ConnectionTimeout = ConnectionTimeout.TotalSeconds > 0 ? ConnectionTimeout : TimeSpan.FromSeconds(30),
                RequestTimeout = RequestTimeout.TotalSeconds > 0 ? RequestTimeout : TimeSpan.FromSeconds(15),
                EnableLogging = EnableLogging,
                EnableCompression = EnableCompression,
                MaxMessageQueue = Math.Max(10, MaxMessageQueue)
            };
        }

        /// <summary>
        /// Validates the connection options.
        /// </summary>
        public bool IsValid(out string errorMessage)
        {
            errorMessage = null;

            if (string.IsNullOrEmpty(ServerUrl))
            {
                errorMessage = "ServerUrl cannot be empty";
                return false;
            }

            if (!ServerUrl.StartsWith("ws://") && !ServerUrl.StartsWith("wss://") &&
                !ServerUrl.StartsWith("http://") && !ServerUrl.StartsWith("https://") &&
                !ServerUrl.Contains("://"))
            {
                errorMessage = "ServerUrl must start with 'ws://', 'wss://', 'http://', 'https://', or be a hostname";
                return false;
            }

            if (string.IsNullOrEmpty(ModuleName))
            {
                errorMessage = "ModuleName cannot be empty";
                return false;
            }

            if (AutoReconnect && MaxReconnectAttempts <= 0)
            {
                errorMessage = "MaxReconnectAttempts must be greater than 0 when AutoReconnect is enabled";
                return false;
            }

            if (ConnectionTimeout.TotalSeconds <= 0)
            {
                errorMessage = "ConnectionTimeout must be greater than 0";
                return false;
            }

            if (RequestTimeout.TotalSeconds <= 0)
            {
                errorMessage = "RequestTimeout must be greater than 0";
                return false;
            }

            return true;
        }

        /// <summary>
        /// Creates connection options from environment variables.
        /// </summary>
        public static OriginDBConnectionOptions FromEnvironment()
        {
            return new OriginDBConnectionOptions
            {
                ServerUrl = Environment.GetEnvironmentVariable("ORIGINDB_SERVER_URL") ?? "http://localhost:8080",
                ModuleName = Environment.GetEnvironmentVariable("ORIGINDB_MODULE_NAME") ?? "default",
                AuthToken = Environment.GetEnvironmentVariable("ORIGINDB_AUTH_TOKEN"),
                EnableLogging = bool.Parse(Environment.GetEnvironmentVariable("ORIGINDB_ENABLE_LOGGING") ?? "true")
            };
        }

        public override string ToString()
        {
            return $"OriginDBConnectionOptions(ServerUrl='{ServerUrl}', ModuleName='{ModuleName}', AutoReconnect={AutoReconnect})";
        }
    }
}
#endif