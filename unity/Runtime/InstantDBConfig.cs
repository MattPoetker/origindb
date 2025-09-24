#if UNITY_5_3_OR_NEWER
using UnityEngine;
using System;

namespace InstantDB.Unity
{
    /// <summary>
    /// ScriptableObject configuration for InstantDB connections.
    /// Create instances of this asset to store connection settings.
    /// </summary>
    [CreateAssetMenu(fileName = "InstantDBConfig", menuName = "InstantDB/Configuration", order = 1)]
    public class InstantDBConfig : ScriptableObject
    {
        [Header("Server Settings")]
        [Tooltip("InstantDB server URL (supports HTTP/HTTPS, automatically converted to WebSocket)")]
        public string serverUrl = "http://localhost:8080";

        [Tooltip("Module name for this application")]
        public string moduleName = "default";

        [Header("Connection Settings")]
        [Tooltip("Automatically connect when the game starts")]
        public bool autoConnect = true;

        [Tooltip("Auto-reconnect on connection loss")]
        public bool autoReconnect = true;

        [Tooltip("Maximum number of reconnection attempts")]
        [Range(0, 20)]
        public int maxReconnectAttempts = 5;

        [Tooltip("Time between reconnection attempts (seconds)")]
        [Range(1f, 60f)]
        public float reconnectInterval = 5f;

        [Tooltip("Connection timeout (seconds)")]
        [Range(5f, 120f)]
        public float connectionTimeout = 30f;

        [Tooltip("Request timeout (seconds)")]
        [Range(1f, 60f)]
        public float requestTimeout = 15f;

        [Header("Authentication")]
        [Tooltip("Authentication token (leave empty if not required)")]
        public string authToken = "";

        [Header("Performance")]
        [Tooltip("Enable message compression")]
        public bool enableCompression = true;

        [Tooltip("Maximum messages to queue before dropping")]
        [Range(10, 1000)]
        public int maxMessageQueue = 100;

        [Header("Debug")]
        [Tooltip("Enable debug logging")]
        public bool enableDebugLogging = true;

        [Tooltip("Log connection events")]
        public bool logConnectionEvents = true;

        [Tooltip("Log all sent/received messages")]
        public bool logMessages = false;

        [Header("Environment Profiles")]
        [Tooltip("Configuration profiles for different environments")]
        public EnvironmentProfile[] environmentProfiles = new EnvironmentProfile[]
        {
            new EnvironmentProfile
            {
                name = "Local Development",
                serverUrl = "http://localhost:8080",
                moduleName = "dev"
            },
            new EnvironmentProfile
            {
                name = "Staging",
                serverUrl = "wss://staging.yourapp.com",
                moduleName = "staging"
            },
            new EnvironmentProfile
            {
                name = "Production",
                serverUrl = "wss://api.yourapp.com",
                moduleName = "production"
            }
        };

        [Header("Active Profile")]
        [Tooltip("Select which environment profile to use")]
        [Range(0, 2)]
        public int activeProfileIndex = 0;

        /// <summary>
        /// Gets the currently active environment profile.
        /// </summary>
        public EnvironmentProfile ActiveProfile
        {
            get
            {
                if (environmentProfiles != null &&
                    activeProfileIndex >= 0 &&
                    activeProfileIndex < environmentProfiles.Length)
                {
                    return environmentProfiles[activeProfileIndex];
                }
                return null;
            }
        }

        /// <summary>
        /// Creates connection options based on this configuration.
        /// </summary>
        /// <returns>Configured connection options</returns>
        public InstantDBConnectionOptions CreateConnectionOptions()
        {
            var profile = ActiveProfile;

            return new InstantDBConnectionOptions
            {
                ServerUrl = profile?.serverUrl ?? serverUrl,
                ModuleName = profile?.moduleName ?? moduleName,
                AuthToken = !string.IsNullOrEmpty(authToken) ? authToken : null,
                AutoReconnect = autoReconnect,
                MaxReconnectAttempts = maxReconnectAttempts,
                ReconnectInterval = TimeSpan.FromSeconds(reconnectInterval),
                ConnectionTimeout = TimeSpan.FromSeconds(connectionTimeout),
                RequestTimeout = TimeSpan.FromSeconds(requestTimeout),
                EnableLogging = enableDebugLogging,
                EnableCompression = enableCompression
            };
        }

        /// <summary>
        /// Validates the configuration settings.
        /// </summary>
        /// <returns>True if configuration is valid</returns>
        public bool ValidateConfiguration(out string errorMessage)
        {
            errorMessage = null;

            // Validate server URL
            var urlToCheck = ActiveProfile?.serverUrl ?? serverUrl;
            if (string.IsNullOrEmpty(urlToCheck))
            {
                errorMessage = "Server URL cannot be empty";
                return false;
            }

            if (!urlToCheck.StartsWith("ws://") && !urlToCheck.StartsWith("wss://") &&
                !urlToCheck.StartsWith("http://") && !urlToCheck.StartsWith("https://") &&
                !urlToCheck.Contains("://"))
            {
                errorMessage = "Server URL must start with 'ws://', 'wss://', 'http://', 'https://', or be a hostname";
                return false;
            }

            // Validate module name
            var moduleToCheck = ActiveProfile?.moduleName ?? moduleName;
            if (string.IsNullOrEmpty(moduleToCheck))
            {
                errorMessage = "Module name cannot be empty";
                return false;
            }

            // Validate reconnection settings
            if (autoReconnect && maxReconnectAttempts <= 0)
            {
                errorMessage = "Max reconnect attempts must be greater than 0 when auto-reconnect is enabled";
                return false;
            }

            return true;
        }

        private void OnValidate()
        {
            // Clamp values to valid ranges
            maxReconnectAttempts = Mathf.Max(0, maxReconnectAttempts);
            reconnectInterval = Mathf.Max(1f, reconnectInterval);
            connectionTimeout = Mathf.Max(5f, connectionTimeout);
            requestTimeout = Mathf.Max(1f, requestTimeout);
            maxMessageQueue = Mathf.Max(10, maxMessageQueue);

            if (environmentProfiles != null)
            {
                activeProfileIndex = Mathf.Clamp(activeProfileIndex, 0, environmentProfiles.Length - 1);
            }
        }

        /// <summary>
        /// Creates a default configuration asset.
        /// </summary>
        [ContextMenu("Reset to Defaults")]
        public void ResetToDefaults()
        {
            serverUrl = "http://localhost:8080";
            moduleName = "default";
            autoConnect = true;
            autoReconnect = true;
            maxReconnectAttempts = 5;
            reconnectInterval = 5f;
            connectionTimeout = 30f;
            requestTimeout = 15f;
            authToken = "";
            enableCompression = true;
            maxMessageQueue = 100;
            enableDebugLogging = true;
            logConnectionEvents = true;
            logMessages = false;
            activeProfileIndex = 0;
        }
    }

    /// <summary>
    /// Environment-specific configuration profile.
    /// </summary>
    [System.Serializable]
    public class EnvironmentProfile
    {
        [Tooltip("Display name for this environment")]
        public string name;

        [Tooltip("Server URL for this environment")]
        public string serverUrl;

        [Tooltip("Module name for this environment")]
        public string moduleName;

        [Tooltip("Environment-specific auth token")]
        public string authToken;

        [Tooltip("Enable debug logging for this environment")]
        public bool enableDebugLogging = true;
    }

    /// <summary>
    /// Editor utility for creating configuration presets.
    /// </summary>
    public static class InstantDBConfigUtility
    {
        /// <summary>
        /// Creates a local development configuration.
        /// </summary>
        public static InstantDBConfig CreateLocalConfig()
        {
            var config = ScriptableObject.CreateInstance<InstantDBConfig>();
            config.serverUrl = "http://localhost:8080";
            config.moduleName = "dev";
            config.enableDebugLogging = true;
            config.logMessages = true;
            return config;
        }

        /// <summary>
        /// Creates a production configuration template.
        /// </summary>
        public static InstantDBConfig CreateProductionConfig()
        {
            var config = ScriptableObject.CreateInstance<InstantDBConfig>();
            config.serverUrl = "wss://api.yourapp.com";
            config.moduleName = "production";
            config.enableDebugLogging = false;
            config.logMessages = false;
            return config;
        }

        /// <summary>
        /// Creates a configuration for Unity Cloud Build.
        /// </summary>
        public static InstantDBConfig CreateCloudBuildConfig()
        {
            var config = ScriptableObject.CreateInstance<InstantDBConfig>();

            // Use environment variables if available
            config.serverUrl = Environment.GetEnvironmentVariable("INSTANTDB_SERVER_URL") ?? "http://localhost:8080";
            config.moduleName = Environment.GetEnvironmentVariable("INSTANTDB_MODULE_NAME") ?? "cloudbuild";
            config.authToken = Environment.GetEnvironmentVariable("INSTANTDB_AUTH_TOKEN") ?? "";

            return config;
        }
    }
}
#endif