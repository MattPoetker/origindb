#pragma once

#include <memory>
#include <string>
#include <functional>

#include "common/config.h"

namespace instantdb {

class ChangefeedEngine;

// WebSocket connection handler
class WebSocketConnection {
public:
    virtual ~WebSocketConnection() = default;

    virtual std::string GetId() const = 0;
    virtual std::string GetRemoteAddress() const = 0;
    virtual bool IsConnected() const = 0;

    virtual void Send(const std::string& message) = 0;
    virtual void SendBinary(const std::vector<uint8_t>& data) = 0;
    virtual void Close(uint16_t code = 1000, const std::string& reason = "") = 0;
};

// WebSocket server for changefeed subscriptions
class WebSocketServer {
public:
    WebSocketServer(const WebSocketConfig& config,
                   std::shared_ptr<ChangefeedEngine> changefeed);
    ~WebSocketServer();

    bool Initialize();
    bool Start();
    void Stop();

    // Connection management
    size_t GetConnectionCount() const;
    std::vector<std::string> GetConnectionIds() const;
    std::shared_ptr<WebSocketConnection> GetConnection(const std::string& id) const;

    // Broadcast to all connections
    void Broadcast(const std::string& message);
    void BroadcastBinary(const std::vector<uint8_t>& data);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb