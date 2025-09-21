#include "websocket/websocket_server.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <regex>

namespace instantdb {

WebSocketServer::WebSocketServer(uint16_t port)
    : port_(port), running_(false), server_socket_(-1) {
    spdlog::info("WebSocket server created on port {}", port_);
}

WebSocketServer::~WebSocketServer() {
    Stop();
}

bool WebSocketServer::Start() {
    // Create socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        spdlog::error("Failed to create socket");
        return false;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        spdlog::error("Failed to set socket options");
        close(server_socket_);
        return false;
    }

    // Bind socket
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_socket_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        spdlog::error("Failed to bind socket to port {}", port_);
        close(server_socket_);
        return false;
    }

    // Listen
    if (listen(server_socket_, 10) < 0) {
        spdlog::error("Failed to listen on socket");
        close(server_socket_);
        return false;
    }

    running_ = true;
    server_thread_ = std::thread([this]() {
        RunServer();
    });

    spdlog::info("WebSocket server started on port {}", port_);
    return true;
}

void WebSocketServer::Stop() {
    if (running_) {
        running_ = false;

        if (server_socket_ >= 0) {
            close(server_socket_);
            server_socket_ = -1;
        }

        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        spdlog::info("WebSocket server stopped");
    }
}

void WebSocketServer::SetChangefeedEngine(std::shared_ptr<ChangefeedEngine> changefeed) {
    changefeed_ = changefeed;
    spdlog::info("WebSocket server connected to changefeed engine");

    // Create a global subscription to broadcast all table changes
    if (changefeed_) {
        SubscriptionFilter filter;
        filter.table_pattern = "*"; // Subscribe to all tables

        std::string global_subscription = changefeed_->CreateSubscription(filter, 0);

        // Register callback to broadcast all changefeed events to WebSocket clients
        changefeed_->Subscribe(global_subscription, [this](const std::string& sub_id, const ChangefeedEvent& event) {
            OnChangefeedEvent(sub_id, event);
        });

        spdlog::info("WebSocket server subscribed to all changefeed events (subscription: {})", global_subscription);
    }
}

size_t WebSocketServer::GetActiveConnections() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return active_clients_.size();
}

void WebSocketServer::RunServer() {
    spdlog::info("WebSocket server listening on port {}", port_);

    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running_) {
                spdlog::error("Failed to accept client connection");
            }
            continue;
        }

        // Handle client in a separate thread
        std::thread client_thread([this, client_socket]() {
            HandleClient(client_socket);
        });
        client_thread.detach();
    }
}

void WebSocketServer::HandleClient(int client_socket) {
    char buffer[4096];
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }

    buffer[bytes_read] = '\0';
    std::string request(buffer);

    // Check if it's a WebSocket upgrade request
    if (request.find("Upgrade: websocket") != std::string::npos) {
        if (HandleWebSocketHandshake(client_socket, request)) {
            // Add to active clients
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                active_clients_.push_back(client_socket);
            }

            spdlog::info("WebSocket client connected. Total connections: {}", GetActiveConnections());

            // Send welcome message
            nlohmann::json welcome;
            welcome["type"] = "welcome";
            welcome["message"] = "Connected to InstantDB changefeed";
            welcome["server_version"] = "0.1.0";
            SendWebSocketFrame(client_socket, welcome.dump());

            // Handle WebSocket messages
            while (running_) {
                bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
                if (bytes_read <= 0) {
                    spdlog::info("WebSocket client disconnected (recv returned {})", bytes_read);
                    break;
                }

                buffer[bytes_read] = '\0';
                std::string frame_data(buffer, bytes_read);

                // Check for close frame before parsing
                if (bytes_read >= 2) {
                    uint8_t opcode = frame_data[0] & 0x0F;
                    if (opcode == 8) { // Close frame
                        spdlog::info("Received WebSocket close frame, disconnecting client");
                        break;
                    }
                }

                std::string message = ParseWebSocketFrame(frame_data);

                if (!message.empty()) {
                    spdlog::debug("Received WebSocket message: {}", message);
                    HandleWebSocketMessage(client_socket, message);
                }
            }

            // Remove from active clients
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                active_clients_.erase(
                    std::remove(active_clients_.begin(), active_clients_.end(), client_socket),
                    active_clients_.end()
                );
            }

            spdlog::info("WebSocket client disconnected. Total connections: {}", GetActiveConnections());
        }
    } else {
        // Send HTTP response for non-WebSocket requests
        std::string response =
            "HTTP/1.1 426 Upgrade Required\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "\r\n"
            "WebSocket upgrade required";
        send(client_socket, response.c_str(), response.length(), 0);
    }

    close(client_socket);
}

bool WebSocketServer::HandleWebSocketHandshake(int client_socket, const std::string& request) {
    // Extract WebSocket key
    std::regex key_regex(R"(Sec-WebSocket-Key:\s*([^\r\n]+))");
    std::smatch match;

    if (!std::regex_search(request, match, key_regex)) {
        spdlog::error("WebSocket key not found in request");
        return false;
    }

    std::string websocket_key = match[1].str();
    std::string accept_key = GenerateWebSocketAccept(websocket_key);

    // Send WebSocket handshake response
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept_key + "\r\n"
        "\r\n";

    ssize_t sent = send(client_socket, response.c_str(), response.length(), 0);
    return sent == static_cast<ssize_t>(response.length());
}

std::string WebSocketServer::GenerateWebSocketAccept(const std::string& key) {
    std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;

    // SHA-1 hash
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.length(), hash);

    // Base64 encode
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    BIO_write(bio, hash, SHA_DIGEST_LENGTH);
    BIO_flush(bio);

    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);
    std::string result(buffer_ptr->data, buffer_ptr->length);

    BIO_free_all(bio);
    return result;
}

void WebSocketServer::SendWebSocketFrame(int client_socket, const std::string& message) {
    std::vector<uint8_t> frame;

    // FIN = 1, Opcode = 1 (text frame)
    frame.push_back(0x81);

    // Payload length
    if (message.length() < 126) {
        frame.push_back(static_cast<uint8_t>(message.length()));
    } else if (message.length() < 65536) {
        frame.push_back(126);
        frame.push_back((message.length() >> 8) & 0xFF);
        frame.push_back(message.length() & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((message.length() >> (i * 8)) & 0xFF);
        }
    }

    // Payload data
    frame.insert(frame.end(), message.begin(), message.end());

    ssize_t sent = send(client_socket, frame.data(), frame.size(), 0);
    if (sent < 0 || sent != static_cast<ssize_t>(frame.size())) {
        spdlog::warn("Failed to send WebSocket frame to client (sent {} of {} bytes)",
                    sent < 0 ? 0 : sent, frame.size());
        throw std::runtime_error("WebSocket send failed");
    }
}

std::string WebSocketServer::ParseWebSocketFrame(const std::string& data) {
    if (data.length() < 2) return "";

    uint8_t first_byte = data[0];
    uint8_t second_byte = data[1];

    bool fin = (first_byte & 0x80) != 0;
    uint8_t opcode = first_byte & 0x0F;
    bool masked = (second_byte & 0x80) != 0;
    uint64_t payload_length = second_byte & 0x7F;

    // Handle control frames (opcodes 8-15)
    if (opcode >= 8) {
        spdlog::debug("Received WebSocket control frame with opcode: {}", opcode);
        if (opcode == 8) { // Close frame
            spdlog::info("Received WebSocket close frame");
        } else if (opcode == 9) { // Ping frame
            spdlog::debug("Received WebSocket ping frame");
        } else if (opcode == 10) { // Pong frame
            spdlog::debug("Received WebSocket pong frame");
        }
        return ""; // Don't process control frames as messages
    }

    // Only process text frames (opcode 1)
    if (opcode != 1) {
        spdlog::debug("Ignoring non-text WebSocket frame with opcode: {}", opcode);
        return "";
    }

    size_t header_size = 2;

    // Extended payload length
    if (payload_length == 126) {
        if (data.length() < 4) return "";
        payload_length = (static_cast<uint8_t>(data[2]) << 8) | static_cast<uint8_t>(data[3]);
        header_size = 4;
    } else if (payload_length == 127) {
        if (data.length() < 10) return "";
        payload_length = 0;
        for (int i = 0; i < 8; i++) {
            payload_length = (payload_length << 8) | static_cast<uint8_t>(data[2 + i]);
        }
        header_size = 10;
    }

    // Masking key
    if (masked) {
        header_size += 4;
    }

    if (data.length() < header_size + payload_length) return "";

    std::string payload = data.substr(header_size, payload_length);

    // Unmask if needed
    if (masked) {
        uint8_t mask[4];
        for (int i = 0; i < 4; i++) {
            mask[i] = data[header_size - 4 + i];
        }

        for (size_t i = 0; i < payload.length(); i++) {
            payload[i] ^= mask[i % 4];
        }
    }

    return payload;
}

void WebSocketServer::HandleWebSocketMessage(int client_socket, const std::string& message) {
    try {
        nlohmann::json request = nlohmann::json::parse(message);
        std::string type = request.value("type", "");

        if (type == "subscribe") {
            // For now, clients are automatically subscribed to all changes
            // In a full implementation, you'd handle specific table subscriptions here

            nlohmann::json response;
            response["type"] = "subscription_created";
            response["subscription_id"] = "auto-all-tables";
            response["table_pattern"] = "*";
            response["message"] = "Subscribed to all table changes";

            SendWebSocketFrame(client_socket, response.dump());
            spdlog::info("WebSocket client subscribed to all table changes");

        } else if (type == "ping") {
            nlohmann::json response;
            response["type"] = "pong";
            SendWebSocketFrame(client_socket, response.dump());

        } else {
            nlohmann::json error;
            error["type"] = "error";
            error["message"] = "Unknown message type: " + type;
            SendWebSocketFrame(client_socket, error.dump());
        }

    } catch (const std::exception& e) {
        spdlog::error("Failed to parse WebSocket message: {}", e.what());

        nlohmann::json error;
        error["type"] = "error";
        error["message"] = "Invalid JSON: " + std::string(e.what());
        SendWebSocketFrame(client_socket, error.dump());
    }
}

void WebSocketServer::BroadcastToAllClients(const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    size_t sent = 0, failed = 0;
    std::vector<int> disconnected_clients;

    for (int client_socket : active_clients_) {
        try {
            SendWebSocketFrame(client_socket, message);
            sent++;
        } catch (const std::exception& e) {
            spdlog::warn("Failed to send message to WebSocket client {}: {}", client_socket, e.what());
            disconnected_clients.push_back(client_socket);
            failed++;
        }
    }

    // Remove disconnected clients
    for (int client_socket : disconnected_clients) {
        active_clients_.erase(
            std::remove(active_clients_.begin(), active_clients_.end(), client_socket),
            active_clients_.end()
        );
        close(client_socket);
        spdlog::info("Removed disconnected client {} from active clients list", client_socket);
    }

    if (sent > 0) {
        spdlog::debug("Broadcast message to {} WebSocket clients", sent);
    }

    if (failed > 0) {
        spdlog::warn("Failed to send to {} WebSocket clients, removed them from active list", failed);
    }
}

void WebSocketServer::OnChangefeedEvent(const std::string& subscription_id, const ChangefeedEvent& event) {
    // Convert changefeed event to JSON for WebSocket clients
    nlohmann::json message;
    message["type"] = "changefeed_event";
    message["subscription_id"] = subscription_id;
    message["offset"] = event.offset;
    message["table"] = event.table;
    message["operation"] = event.operation;
    message["transaction_id"] = event.transaction_id;
    // Convert timestamp to milliseconds since epoch
    auto duration = event.timestamp.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    message["timestamp"] = millis;

    // Convert binary data to strings for JSON transport
    if (!event.key.empty()) {
        message["key"] = std::string(event.key.begin(), event.key.end());
    }

    if (!event.new_value.empty()) {
        message["new_value"] = std::string(event.new_value.begin(), event.new_value.end());
    }

    if (!event.old_value.empty()) {
        message["old_value"] = std::string(event.old_value.begin(), event.old_value.end());
    }

    // Broadcast to all connected WebSocket clients
    BroadcastToAllClients(message.dump());

    spdlog::info("Broadcast changefeed event: {} {} key={} to {} clients",
                event.table, event.operation, std::string(event.key.begin(), event.key.end()),
                GetActiveConnections());
}

} // namespace instantdb