using UnityEngine;
using OriginDB.Unity;
using OriginDB.Client;
using System.Collections.Generic;

/// <summary>
/// Example GameManager that demonstrates how to use OriginDB in Unity.
/// This script shows the recommended pattern for integrating OriginDB into your game.
/// </summary>
public class SampleGameManager : MonoBehaviour
{
    [Header("Configuration")]
    [SerializeField] private OriginDBConfig config;
    [SerializeField] private bool connectOnStart = true;

    [Header("UI References")]
    [SerializeField] private UnityEngine.UI.Text statusText;
    [SerializeField] private UnityEngine.UI.Button connectButton;
    [SerializeField] private UnityEngine.UI.Button disconnectButton;
    [SerializeField] private UnityEngine.UI.InputField playerNameInput;
    [SerializeField] private UnityEngine.UI.Button joinGameButton;

    // Connection management
    private IOriginDBConnection _connection;
    private bool _isConnected = false;

    // Game state
    private Dictionary<int, GameObject> _players = new Dictionary<int, GameObject>();
    private int _localPlayerId = -1;

    // Events for other scripts to listen to
    public static event System.Action<bool> OnConnectionStateChanged;
    public static event System.Action<Player> OnPlayerJoined;
    public static event System.Action<Player> OnPlayerLeft;

    private void Awake()
    {
        // Ensure we have a network manager in the scene
        if (OriginDBNetworkManager.Instance == null)
        {
            Debug.LogWarning("[Sample] No OriginDBNetworkManager found in scene. Creating one automatically.");
            var managerObj = new GameObject("OriginDB Network Manager");
            managerObj.AddComponent<OriginDBNetworkManager>();
        }

        // Set up UI button listeners
        if (connectButton != null)
            connectButton.onClick.AddListener(ConnectToServer);

        if (disconnectButton != null)
            disconnectButton.onClick.AddListener(DisconnectFromServer);

        if (joinGameButton != null)
            joinGameButton.onClick.AddListener(JoinGame);

        UpdateUI();
    }

    private void Start()
    {
        // Wait for network manager to initialize
        if (OriginDBNetworkManager.Instance != null && OriginDBNetworkManager.Instance.IsInitialized)
        {
            OnNetworkManagerReady();
        }
        else
        {
            OriginDBNetworkManager.OnManagerInitialized += OnNetworkManagerReady;
        }
    }

    private void OnNetworkManagerReady(OriginDBNetworkManager manager = null)
    {
        Debug.Log("[Sample] Network manager ready, setting up connection");

        if (connectOnStart)
        {
            ConnectToServer();
        }
    }

    public void ConnectToServer()
    {
        if (_isConnected)
        {
            Debug.LogWarning("[Sample] Already connected to server");
            return;
        }

        try
        {
            // Validate configuration
            if (config != null)
            {
                if (!config.ValidateConfiguration(out string errorMessage))
                {
                    Debug.LogError($"[Sample] Invalid configuration: {errorMessage}");
                    UpdateStatusText($"Configuration Error: {errorMessage}");
                    return;
                }

                _connection = OriginDBNetworkManager.Instance.CreateConnection(config.CreateConnectionOptions());
            }
            else
            {
                Debug.LogWarning("[Sample] No configuration set, using default connection");
                _connection = OriginDBNetworkManager.Instance.CreateDefaultConnection();
            }

            // Subscribe to connection events
            _connection.OnConnected += HandleConnected;
            _connection.OnDisconnected += HandleDisconnected;
            _connection.OnError += HandleConnectionError;

            // Subscribe to data events
            _connection.OnPlayerInsert += HandlePlayerJoined;
            _connection.OnPlayerUpdate += HandlePlayerUpdated;
            _connection.OnPlayerDelete += HandlePlayerLeft;

            UpdateStatusText("Connecting...");
            Debug.Log("[Sample] Connecting to OriginDB server...");
        }
        catch (System.Exception ex)
        {
            Debug.LogError($"[Sample] Failed to create connection: {ex.Message}");
            UpdateStatusText($"Connection Error: {ex.Message}");
        }
    }

    public void DisconnectFromServer()
    {
        if (!_isConnected)
        {
            Debug.LogWarning("[Sample] Not connected to server");
            return;
        }

        _connection?.Disconnect();
        Debug.Log("[Sample] Disconnecting from server...");
    }

    public async void JoinGame()
    {
        if (!_isConnected)
        {
            Debug.LogWarning("[Sample] Cannot join game - not connected to server");
            return;
        }

        string playerName = playerNameInput?.text ?? "Anonymous Player";

        try
        {
            UpdateStatusText("Joining game...");

            // Execute reducer to join the game
            var result = await _connection.ExecuteReducer("join_game", new
            {
                name = playerName,
                spawn_x = 0f,
                spawn_y = 0f,
                spawn_z = 0f
            });

            if (result.Success)
            {
                _localPlayerId = result.GetValue<int>("player_id");
                UpdateStatusText($"Joined as {playerName} (ID: {_localPlayerId})");
                Debug.Log($"[Sample] Successfully joined game as {playerName} with ID {_localPlayerId}");

                // Subscribe to real-time updates
                await _connection.SubscribeToTable("players");
                await _connection.SubscribeToTable("player_actions");
            }
            else
            {
                UpdateStatusText($"Failed to join: {result.ErrorMessage}");
                Debug.LogError($"[Sample] Failed to join game: {result.ErrorMessage}");
            }
        }
        catch (System.Exception ex)
        {
            UpdateStatusText($"Join Error: {ex.Message}");
            Debug.LogError($"[Sample] Error joining game: {ex.Message}");
        }
    }

    #region Connection Event Handlers

    private void HandleConnected()
    {
        _isConnected = true;
        UpdateStatusText("Connected to OriginDB");
        UpdateUI();
        OnConnectionStateChanged?.Invoke(true);
        Debug.Log("[Sample] Successfully connected to OriginDB");
    }

    private void HandleDisconnected(System.Exception ex)
    {
        _isConnected = false;
        UpdateStatusText(ex != null ? $"Disconnected: {ex.Message}" : "Disconnected");
        UpdateUI();
        OnConnectionStateChanged?.Invoke(false);

        // Clear all players
        foreach (var playerObj in _players.Values)
        {
            if (playerObj != null)
                Destroy(playerObj);
        }
        _players.Clear();
        _localPlayerId = -1;

        Debug.Log($"[Sample] Disconnected from OriginDB: {ex?.Message}");
    }

    private void HandleConnectionError(System.Exception ex)
    {
        UpdateStatusText($"Connection Error: {ex.Message}");
        Debug.LogError($"[Sample] Connection error: {ex.Message}");
    }

    #endregion

    #region Data Event Handlers

    private void HandlePlayerJoined(Player player)
    {
        Debug.Log($"[Sample] Player joined: {player.Name} (ID: {player.Id})");

        // Create player GameObject
        var playerObj = CreatePlayerObject(player);
        _players[player.Id] = playerObj;

        OnPlayerJoined?.Invoke(player);

        UpdateStatusText($"Player joined: {player.Name}");
    }

    private void HandlePlayerUpdated(Player oldPlayer, Player newPlayer)
    {
        Debug.Log($"[Sample] Player updated: {newPlayer.Name} (ID: {newPlayer.Id})");

        if (_players.TryGetValue(newPlayer.Id, out var playerObj))
        {
            // Update player position
            playerObj.transform.position = newPlayer.Position;

            // Update other player properties
            var nameDisplay = playerObj.GetComponentInChildren<UnityEngine.UI.Text>();
            if (nameDisplay != null)
            {
                nameDisplay.text = $"{newPlayer.Name}\nHP: {newPlayer.Health}";
            }
        }
    }

    private void HandlePlayerLeft(Player player)
    {
        Debug.Log($"[Sample] Player left: {player.Name} (ID: {player.Id})");

        if (_players.TryGetValue(player.Id, out var playerObj))
        {
            _players.Remove(player.Id);
            if (playerObj != null)
                Destroy(playerObj);
        }

        OnPlayerLeft?.Invoke(player);
        UpdateStatusText($"Player left: {player.Name}");
    }

    #endregion

    #region Helper Methods

    private GameObject CreatePlayerObject(Player player)
    {
        // Create a simple player representation
        var playerObj = GameObject.CreatePrimitive(PrimitiveType.Capsule);
        playerObj.name = $"Player_{player.Id}_{player.Name}";
        playerObj.transform.position = player.Position;

        // Add different color for local vs remote players
        var renderer = playerObj.GetComponent<Renderer>();
        if (player.Id == _localPlayerId)
        {
            renderer.material.color = Color.green; // Local player
        }
        else
        {
            renderer.material.color = Color.blue; // Remote player
        }

        // Add name tag
        var canvas = new GameObject("NameTag");
        canvas.transform.SetParent(playerObj.transform);
        canvas.transform.localPosition = Vector3.up * 2f;

        var canvasComponent = canvas.AddComponent<Canvas>();
        canvasComponent.renderMode = RenderMode.WorldSpace;
        canvasComponent.worldCamera = Camera.main;

        var text = canvas.AddComponent<UnityEngine.UI.Text>();
        text.text = $"{player.Name}\nHP: {player.Health}";
        text.fontSize = 14;
        text.color = Color.white;
        text.alignment = TextAnchor.MiddleCenter;

        // Scale the canvas appropriately
        canvas.transform.localScale = Vector3.one * 0.01f;

        return playerObj;
    }

    private void UpdateStatusText(string status)
    {
        if (statusText != null)
        {
            statusText.text = $"Status: {status}";
        }

        Debug.Log($"[Sample] Status: {status}");
    }

    private void UpdateUI()
    {
        if (connectButton != null)
            connectButton.interactable = !_isConnected;

        if (disconnectButton != null)
            disconnectButton.interactable = _isConnected;

        if (joinGameButton != null)
            joinGameButton.interactable = _isConnected;
    }

    #endregion

    #region Public API for other scripts

    public bool IsConnected => _isConnected;
    public int LocalPlayerId => _localPlayerId;
    public IOriginDBConnection Connection => _connection;

    public async void SendPlayerAction(string actionType, object data = null)
    {
        if (!_isConnected || _localPlayerId < 0)
        {
            Debug.LogWarning("[Sample] Cannot send action - not connected or not joined");
            return;
        }

        try
        {
            await _connection.ExecuteReducer("player_action", new
            {
                player_id = _localPlayerId,
                action_type = actionType,
                data = data
            });

            Debug.Log($"[Sample] Sent player action: {actionType}");
        }
        catch (System.Exception ex)
        {
            Debug.LogError($"[Sample] Failed to send action: {ex.Message}");
        }
    }

    #endregion

    private void OnDestroy()
    {
        // Clean up event subscriptions
        OriginDBNetworkManager.OnManagerInitialized -= OnNetworkManagerReady;

        if (_connection != null)
        {
            _connection.OnConnected -= HandleConnected;
            _connection.OnDisconnected -= HandleDisconnected;
            _connection.OnError -= HandleConnectionError;
            _connection.OnPlayerInsert -= HandlePlayerJoined;
            _connection.OnPlayerUpdate -= HandlePlayerUpdated;
            _connection.OnPlayerDelete -= HandlePlayerLeft;
        }
    }

    #region Inspector Helper

    [ContextMenu("Test Connection")]
    private void TestConnection()
    {
        if (Application.isPlaying)
        {
            ConnectToServer();
        }
        else
        {
            Debug.Log("[Sample] Test connection only works in play mode");
        }
    }

    #endregion
}