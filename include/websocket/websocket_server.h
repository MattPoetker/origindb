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

namespace instantdb {

class ChangefeedEngine;

// Simple WebSocket server using raw sockets
class WebSocketServer {
public:
    WebSocketServer(uint16_t port);
    ~WebSocketServer();

    bool Start();
    void Stop();

    // Set changefeed engine to get events from
    void SetChangefeedEngine(std::shared_ptr<ChangefeedEngine> changefeed);

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

private:
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    int server_socket_;
    std::shared_ptr<ChangefeedEngine> changefeed_;

    mutable std::mutex clients_mutex_;
    std::vector<int> active_clients_;
    std::unordered_map<int, std::string> client_subscriptions_;
};

} // namespace instantdb