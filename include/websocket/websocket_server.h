#pragma once

#include <thread>
#include <memory>
#include <functional>
#include <string>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>

#include "changefeed/changefeed_engine.h"
#include "changefeed/sql_subscription.h"
#include "wasm/wasm_subscription.h"
#include "sql/sql_engine.h"
#include "storage/storage_engine.h"
#include "common/auth.h"

namespace origindb {

class ChangefeedEngine;
class WasmSubscriptionManager;
class SqlSubscriptionManager;
class SqlEngine;
class StorageEngine;
class AuthManager;
class WasmEngine;

// Simple WebSocket server using raw sockets
class WebSocketServer {
public:
    WebSocketServer(uint16_t port);
    ~WebSocketServer();

    bool Start();
    void Stop();

    // Set changefeed engine to get events from
    void SetChangefeedEngine(std::shared_ptr<ChangefeedEngine> changefeed);

    // Set WASM subscription manager for custom subscriptions
    void SetWasmSubscriptionManager(std::shared_ptr<WasmSubscriptionManager> wasm_manager);

    // Set SQL subscription manager for SQL-based subscriptions
    void SetSqlSubscriptionManager(std::shared_ptr<SqlSubscriptionManager> sql_manager);

    // Set SQL engine for executing queries to get current state
    void SetSqlEngine(std::shared_ptr<SqlEngine> sql_engine);

    // Set storage engine for getting table list
    void SetStorageEngine(std::shared_ptr<StorageEngine> storage_engine);

    // Set WASM engine so clients can call reducers over the same websocket they
    // subscribe on (bidirectional: reads via changefeed, writes via call_reducer).
    // Null = reducer calls over ws disabled (writes must go through gRPC).
    void SetWasmEngine(std::shared_ptr<WasmEngine> wasm_engine);

    // Set the auth manager. Null = auth disabled (dev mode).
    void SetAuthManager(std::shared_ptr<AuthManager> auth) { auth_ = std::move(auth); }

    // Get connection statistics
    size_t GetActiveConnections() const;

    // Enable/disable binary protocol for a client
    void SetClientProtocol(int client_socket, bool use_binary);

    // Check if client is using binary protocol
    bool IsClientBinary(int client_socket) const;

private:
    void RunServer();
    void HandleClient(int client_socket);
    bool HandleWebSocketHandshake(int client_socket, const std::string& request);
    void HandleWebSocketMessage(int client_socket, const std::string& message);
    void SendWebSocketFrame(int client_socket, const std::string& message);
    void SendBinaryWebSocketFrame(int client_socket, const std::vector<uint8_t>& data);
    void BroadcastToAllClients(const std::string& message);
    void BroadcastBinaryToAllClients(const std::vector<uint8_t>& data);
    std::string ParseWebSocketFrame(const std::string& data);
    static size_t CompleteFrameSize(const std::string& data);
    static std::string ExtractRequestToken(const std::string& request);
    std::string GenerateWebSocketAccept(const std::string& key);

    // Changefeed event handler
    void OnChangefeedEvent(const std::string& subscription_id, const ChangefeedEvent& event);

    // WASM subscription event handler
    void OnWasmSubscriptionEvent(const std::string& subscription_id,
                                const std::string& client_id,
                                const WasmValue& data);

    // Handle subscription requests from clients
    void HandleSubscriptionRequest(int client_socket, const std::string& message);
    void HandleSqlSubscriptionRequest(int client_socket, const std::string& message);
    void HandleSubscribeToAllTablesRequest(int client_socket, const std::string& message);

    // Execute a reducer call arriving over the websocket (bidirectional writes).
    void HandleCallReducer(int client_socket, const std::string& message);

    // Helper method to execute SQL query and send initial state
    void SendInitialState(int client_socket, const std::string& sql_query, const std::string& subscription_id);

    // Helper method to send initial state for all tables
    void SendInitialStateAllTables(int client_socket, const std::string& subscription_id);

private:
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    int server_socket_;
    std::shared_ptr<ChangefeedEngine> changefeed_;
    std::shared_ptr<WasmSubscriptionManager> wasm_manager_;
    std::shared_ptr<SqlSubscriptionManager> sql_manager_;
    std::shared_ptr<SqlEngine> sql_engine_;
    std::shared_ptr<StorageEngine> storage_engine_;
    std::shared_ptr<AuthManager> auth_;
    std::shared_ptr<WasmEngine> wasm_engine_;

    mutable std::mutex clients_mutex_;
    std::vector<int> active_clients_;
    std::unordered_map<int, std::string> client_subscriptions_;
    std::unordered_map<int, std::string> client_ids_;  // Map socket to client ID
    std::unordered_map<int, bool> client_use_binary_;  // Map socket to binary protocol preference
};

} // namespace origindb