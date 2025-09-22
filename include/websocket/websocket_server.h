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

namespace instantdb {

class ChangefeedEngine;
class WasmSubscriptionManager;
class SqlSubscriptionManager;

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

    // Get connection statistics
    size_t GetActiveConnections() const;

private:
    void RunServer();
    void HandleClient(int client_socket);
    bool HandleWebSocketHandshake(int client_socket, const std::string& request);
    void HandleWebSocketMessage(int client_socket, const std::string& message);
    void SendWebSocketFrame(int client_socket, const std::string& message);
    void BroadcastToAllClients(const std::string& message);
    std::string ParseWebSocketFrame(const std::string& data);
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

private:
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    int server_socket_;
    std::shared_ptr<ChangefeedEngine> changefeed_;
    std::shared_ptr<WasmSubscriptionManager> wasm_manager_;
    std::shared_ptr<SqlSubscriptionManager> sql_manager_;

    mutable std::mutex clients_mutex_;
    std::vector<int> active_clients_;
    std::unordered_map<int, std::string> client_subscriptions_;
    std::unordered_map<int, std::string> client_ids_;  // Map socket to client ID
};

} // namespace instantdb