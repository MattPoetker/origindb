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
#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include "changefeed/sql_subscription.h"
#include "changefeed/predicate_evaluator.h"
#include "wasm/wasm_engine.h"
#include <variant>

namespace origindb {

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

void WebSocketServer::SetWasmSubscriptionManager(std::shared_ptr<WasmSubscriptionManager> wasm_manager) {
    wasm_manager_ = wasm_manager;
    spdlog::info("WebSocket server connected to WASM subscription manager");

    // Set callback for WASM subscription events
    if (wasm_manager_) {
        wasm_manager_->SetSubscriptionCallback(
            [this](const std::string& subscription_id, const std::string& client_id, const WasmValue& data) {
                OnWasmSubscriptionEvent(subscription_id, client_id, data);
            }
        );
    }
}

void WebSocketServer::SetSqlSubscriptionManager(std::shared_ptr<SqlSubscriptionManager> sql_manager) {
    sql_manager_ = sql_manager;
    spdlog::info("WebSocket server connected to SQL subscription manager");
}

void WebSocketServer::SetSqlEngine(std::shared_ptr<SqlEngine> sql_engine) {
    sql_engine_ = sql_engine;
    spdlog::info("WebSocket server connected to SQL engine");
}

void WebSocketServer::SetStorageEngine(std::shared_ptr<StorageEngine> storage_engine) {
    storage_engine_ = storage_engine;
    spdlog::info("WebSocket server connected to storage engine");
}

void WebSocketServer::SetWasmEngine(std::shared_ptr<WasmEngine> wasm_engine) {
    wasm_engine_ = wasm_engine;
    spdlog::info("WebSocket server connected to WASM engine (bidirectional call_reducer enabled)");
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

    // Check if it's a WebSocket upgrade request. Header names/values are
    // case-insensitive per RFC 7230/6455 — browsers send "Upgrade: websocket"
    // but non-browser clients (undici, load tools) may lowercase them, so match
    // case-insensitively rather than rejecting them with a 426.
    std::string request_lc = request;
    std::transform(request_lc.begin(), request_lc.end(), request_lc.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (request_lc.find("upgrade: websocket") != std::string::npos) {
        // Auth (client scope). Browsers can't set WebSocket headers, so the
        // token rides in the GET target as ?token=... (falls back to the
        // Authorization header for non-browser clients).
        if (auth_) {
            std::string token = ExtractRequestToken(request);
            if (!auth_->Check(token, AuthScope::CLIENT)) {
                const char* resp =
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                send(client_socket, resp, strlen(resp), 0);
                close(client_socket);
                spdlog::warn("WebSocket connection rejected: invalid/missing token");
                return;
            }
        }
        if (HandleWebSocketHandshake(client_socket, request)) {
            // Generate client ID
            std::string client_id = "client_" + std::to_string(client_socket) + "_" +
                                   std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

            // Add to active clients
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                active_clients_.push_back(client_socket);
                client_ids_[client_socket] = client_id;
            }

            // Notify WASM subscription manager
            if (wasm_manager_) {
                wasm_manager_->OnClientConnected(client_id);
            }

            spdlog::info("WebSocket client connected: {} Total connections: {}", client_id, GetActiveConnections());

            // Send welcome message
            nlohmann::json welcome;
            welcome["type"] = "welcome";
            welcome["message"] = "Connected to OriginDB changefeed";
            welcome["server_version"] = "0.1.0";
            welcome["client_id"] = client_id;
            welcome["features"] = nlohmann::json::array({"changefeed", "wasm_subscriptions"});
            SendWebSocketFrame(client_socket, welcome.dump());

            // Handle WebSocket messages. TCP gives no frame alignment: one
            // recv may carry several websocket frames (coalesced sends) or a
            // partial one, so accumulate bytes and drain complete frames.
            std::string pending;
            bool client_closed = false;
            while (running_ && !client_closed) {
                bytes_read = recv(client_socket, buffer, sizeof(buffer), 0);
                if (bytes_read <= 0) {
                    spdlog::info("WebSocket client disconnected (recv returned {})", bytes_read);
                    break;
                }
                pending.append(buffer, static_cast<size_t>(bytes_read));

                while (true) {
                    size_t frame_size = CompleteFrameSize(pending);
                    if (frame_size == 0 || frame_size > pending.size()) break;

                    std::string frame_data = pending.substr(0, frame_size);
                    pending.erase(0, frame_size);

                    uint8_t opcode = frame_data[0] & 0x0F;
                    if (opcode == 8) { // Close frame
                        spdlog::info("Received WebSocket close frame, disconnecting client");
                        client_closed = true;
                        break;
                    }

                    std::string message = ParseWebSocketFrame(frame_data);
                    if (!message.empty()) {
                        HandleWebSocketMessage(client_socket, message);
                    }
                }

                // Backstop against malformed frames growing the buffer forever.
                if (pending.size() > 16 * 1024 * 1024) {
                    spdlog::warn("WebSocket client buffer exceeded 16MiB, disconnecting");
                    break;
                }
            }

            // Get client ID before removing
            std::string disconnected_client_id;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                auto it = client_ids_.find(client_socket);
                if (it != client_ids_.end()) {
                    disconnected_client_id = it->second;
                    client_ids_.erase(it);
                }

                // Remove from active clients
                active_clients_.erase(
                    std::remove(active_clients_.begin(), active_clients_.end(), client_socket),
                    active_clients_.end()
                );
            }

            // Notify WASM subscription manager
            if (wasm_manager_ && !disconnected_client_id.empty()) {
                wasm_manager_->OnClientDisconnected(disconnected_client_id);
            }

            // Notify SQL subscription manager
            if (sql_manager_ && !disconnected_client_id.empty()) {
                sql_manager_->UnsubscribeAll(disconnected_client_id);
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

std::string WebSocketServer::ExtractRequestToken(const std::string& request) {
    // Prefer the Authorization header (non-browser clients).
    std::regex auth_re(R"(Authorization:\s*([^\r\n]+))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(request, m, auth_re)) {
        return AuthManager::StripBearer(m[1].str());
    }
    // Otherwise ?token=... in the request target (browsers).
    std::regex token_re(R"([?&]token=([^&\s]+))");
    if (std::regex_search(request, m, token_re)) {
        return m[1].str();
    }
    return "";
}

size_t WebSocketServer::CompleteFrameSize(const std::string& data) {
    // Returns the total byte size of the first websocket frame in `data`,
    // or 0 if the header is not complete yet. The caller checks whether the
    // full frame has arrived.
    if (data.size() < 2) return 0;

    size_t header = 2;
    uint64_t payload_len = static_cast<uint8_t>(data[1]) & 0x7F;
    bool masked = (static_cast<uint8_t>(data[1]) & 0x80) != 0;

    if (payload_len == 126) {
        if (data.size() < 4) return 0;
        payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(data[2])) << 8) |
                      static_cast<uint8_t>(data[3]);
        header = 4;
    } else if (payload_len == 127) {
        if (data.size() < 10) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | static_cast<uint8_t>(data[2 + i]);
        }
        header = 10;
    }
    if (masked) header += 4;

    return header + payload_len;
}

bool WebSocketServer::HandleWebSocketHandshake(int client_socket, const std::string& request) {
    // Extract WebSocket key (header name is case-insensitive)
    std::regex key_regex(R"(Sec-WebSocket-Key:\s*([^\r\n]+))", std::regex::icase);
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

        } else if (type == "sql_subscribe") {
            HandleSqlSubscriptionRequest(client_socket, message);

        } else if (type == "sql_unsubscribe") {
            // Area-of-interest clients drop their old viewport subscription when
            // they pan, so subscriptions track the visible window instead of
            // leaking one per pan.
            std::string sub_id = request.value("subscription_id", "");
            if (sql_manager_ && !sub_id.empty()) sql_manager_->Unsubscribe(sub_id);
            nlohmann::json response;
            response["type"] = "sql_unsubscribed";
            response["subscription_id"] = sub_id;
            SendWebSocketFrame(client_socket, response.dump());

        } else if (type == "subscribe_to_all_tables") {
            HandleSubscribeToAllTablesRequest(client_socket, message);

        } else if (type == "wasm_subscribe") {
            HandleSubscriptionRequest(client_socket, message);

        } else if (type == "call_reducer") {
            HandleCallReducer(client_socket, message);

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

// Execute a reducer call that arrived over the websocket. This makes the
// changefeed socket bidirectional: a client subscribes for reads AND issues
// writes on the same connection, so the browser needs no separate HTTP write
// path (no per-request headers, no CORS preflight). The connection was already
// authenticated at CLIENT scope during the handshake, so no re-check here.
//
// Request:  {type:"call_reducer", module, reducer, args:[...], id?}
// Reply:    {type:"call_result", id, success, error?, result?}
// The reply is sent on error, or on success only when the caller supplied an
// `id` — so fire-and-forget writes (e.g. cursor moves) cost zero return traffic.
void WebSocketServer::HandleCallReducer(int client_socket, const std::string& message) {
    nlohmann::json response;
    response["type"] = "call_result";
    std::string id;
    bool success = false;
    try {
        nlohmann::json request = nlohmann::json::parse(message);
        id = request.value("id", "");
        response["id"] = id;

        if (!wasm_engine_) {
            response["error"] = "reducer calls over websocket are disabled";
            SendWebSocketFrame(client_socket, response.dump());
            return;
        }

        const std::string module = request.value("module", "");
        const std::string reducer = request.value("reducer", "");
        if (module.empty() || reducer.empty()) {
            response["success"] = false;
            response["error"] = "module and reducer are required";
            SendWebSocketFrame(client_socket, response.dump());
            return;
        }

        // JSON args → WasmValue (mirror of the gRPC path). Integral numbers map
        // to int64, other numbers to double, matching how reducers read args.
        std::vector<WasmValue> args;
        if (request.contains("args") && request["args"].is_array()) {
            for (const auto& a : request["args"]) {
                if (a.is_boolean()) args.push_back(a.get<bool>());
                else if (a.is_number_integer()) args.push_back(a.get<int64_t>());
                else if (a.is_number()) args.push_back(a.get<double>());
                else if (a.is_string()) args.push_back(a.get<std::string>());
                else args.push_back(std::monostate{});
            }
        }

        std::string sender;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = client_ids_.find(client_socket);
            if (it != client_ids_.end()) sender = it->second;
        }

        ReducerContext ctx = wasm_engine_->CreateReducerContext(sender, sender);
        ctx.module_identity = module;
        auto result = wasm_engine_->ExecuteReducer(module, reducer, ctx, args);

        success = result.success;
        response["success"] = success;
        if (!success) {
            response["error"] = result.error;
        } else if (!result.values.empty()) {
            std::visit([&response](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) { /* null */ }
                else if constexpr (std::is_same_v<T, bool>) response["result"] = v;
                else if constexpr (std::is_same_v<T, int32_t>) response["result"] = static_cast<int64_t>(v);
                else if constexpr (std::is_same_v<T, int64_t>) response["result"] = v;
                else if constexpr (std::is_same_v<T, float>) response["result"] = static_cast<double>(v);
                else if constexpr (std::is_same_v<T, double>) response["result"] = v;
                else if constexpr (std::is_same_v<T, std::string>) response["result"] = v;
                else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                    response["result"] = std::string(v.begin(), v.end());
            }, result.values[0]);
        }
    } catch (const std::exception& e) {
        response["success"] = false;
        response["error"] = std::string("call_reducer error: ") + e.what();
        SendWebSocketFrame(client_socket, response.dump());
        return;
    }

    // Ack on error, or on success only if the caller wants correlation (`id`).
    if (!success || !id.empty()) SendWebSocketFrame(client_socket, response.dump());
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
    // If SQL subscription manager is available, use it for selective filtering
    if (sql_manager_) {
        // Process event through SQL subscription manager to get filtered results per client
        auto filtered_results = sql_manager_->ProcessEvent(event);

        for (const auto& [client_id, filtered_event] : filtered_results) {
            // Find the client socket for this client ID
            int target_socket = -1;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (const auto& [socket, id] : client_ids_) {
                    if (id == client_id) {
                        target_socket = socket;
                        break;
                    }
                }
            }

            if (target_socket == -1) {
                spdlog::warn("SQL subscription event for unknown client: {}", client_id);
                continue;
            }

            // Convert filtered changefeed event to JSON for this specific client
            nlohmann::json message;
            message["type"] = "sql_changefeed_event";
            message["subscription_id"] = subscription_id;
            message["offset"] = filtered_event.offset;
            message["table"] = filtered_event.table;
            message["operation"] = filtered_event.operation;
            message["transaction_id"] = filtered_event.transaction_id;

            // Convert timestamp to milliseconds since epoch
            auto duration = filtered_event.timestamp.time_since_epoch();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            message["timestamp"] = millis;

            // Convert binary data to strings for JSON transport
            if (!filtered_event.key.empty()) {
                message["key"] = std::string(filtered_event.key.begin(), filtered_event.key.end());
            }

            if (!filtered_event.new_value.empty()) {
                message["new_value"] = std::string(filtered_event.new_value.begin(), filtered_event.new_value.end());
            }

            if (!filtered_event.old_value.empty()) {
                message["old_value"] = std::string(filtered_event.old_value.begin(), filtered_event.old_value.end());
            }

            // Send to the specific client only
            try {
                SendWebSocketFrame(target_socket, message.dump());
                spdlog::debug("Sent filtered changefeed event to client {}: {} {} key={}",
                            client_id, filtered_event.table, filtered_event.operation,
                            std::string(filtered_event.key.begin(), filtered_event.key.end()));
            } catch (const std::exception& e) {
                spdlog::warn("Failed to send filtered changefeed event to client {}: {}", client_id, e.what());
            }
        }

        // NOTE: one changefeed event per row-change — at 60 Hz physics this is
        // thousands/sec, so keep it at debug (info would flood the log + burn CPU).
        spdlog::debug("Processed changefeed event: {} {} key={} - sent to {} subscribers",
                    event.table, event.operation, std::string(event.key.begin(), event.key.end()),
                    filtered_results.size());
    } else {
        // Fallback: broadcast to all clients (legacy behavior)
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
}

void WebSocketServer::OnWasmSubscriptionEvent(const std::string& subscription_id,
                                             const std::string& client_id,
                                             const WasmValue& data) {
    // Find the client socket for this client ID
    int target_socket = -1;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (const auto& [socket, id] : client_ids_) {
            if (id == client_id) {
                target_socket = socket;
                break;
            }
        }
    }

    if (target_socket == -1) {
        spdlog::warn("WASM subscription event for unknown client: {}", client_id);
        return;
    }

    // Convert WasmValue to JSON
    nlohmann::json message;
    message["type"] = "wasm_subscription_event";
    message["subscription_id"] = subscription_id;
    message["client_id"] = client_id;

    // Convert WasmValue to JSON value
    std::visit([&message](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            message["data"] = nullptr;
        } else if constexpr (std::is_same_v<T, bool>) {
            message["data"] = value;
        } else if constexpr (std::is_arithmetic_v<T>) {
            message["data"] = value;
        } else if constexpr (std::is_same_v<T, std::string>) {
            message["data"] = value;
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            // Convert binary data to base64 string
            message["data"] = std::string(value.begin(), value.end()); // Simplified
            message["data_type"] = "binary";
        }
    }, data);

    try {
        SendWebSocketFrame(target_socket, message.dump());
        spdlog::debug("Sent WASM subscription event to client: {}", client_id);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to send WASM subscription event to client {}: {}", client_id, e.what());
    }
}

void WebSocketServer::HandleSubscriptionRequest(int client_socket, const std::string& message) {
    if (!wasm_manager_) {
        nlohmann::json error;
        error["type"] = "error";
        error["message"] = "WASM subscription manager not available";
        SendWebSocketFrame(client_socket, error.dump());
        return;
    }

    try {
        nlohmann::json request = nlohmann::json::parse(message);

        // Get client ID for this socket
        std::string client_id;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = client_ids_.find(client_socket);
            if (it != client_ids_.end()) {
                client_id = it->second;
            }
        }

        if (client_id.empty()) {
            nlohmann::json error;
            error["type"] = "error";
            error["message"] = "Client ID not found";
            SendWebSocketFrame(client_socket, error.dump());
            return;
        }

        // Build WASM subscription query using the builder
        SubscriptionQueryBuilder builder;
        builder.ForClient(client_id);

        // Required fields
        if (request.contains("module_name")) {
            builder.WithModule(request["module_name"]);
        } else {
            nlohmann::json error;
            error["type"] = "error";
            error["message"] = "module_name is required";
            SendWebSocketFrame(client_socket, error.dump());
            return;
        }

        // Optional fields
        if (request.contains("filter_function")) {
            builder.WithFilter(request["filter_function"]);
        }

        if (request.contains("transform_function")) {
            builder.WithTransform(request["transform_function"]);
        }

        if (request.contains("tables")) {
            std::vector<std::string> tables = request["tables"];
            builder.FromTables(tables);
        }

        if (request.contains("where_clause")) {
            builder.Where(request["where_clause"]);
        }

        if (request.contains("columns")) {
            std::vector<std::string> columns = request["columns"];
            builder.Select(columns);
        }

        if (request.contains("parameters")) {
            nlohmann::json params = request["parameters"];
            for (const auto& [key, value] : params.items()) {
                // Convert JSON values to WasmValue
                WasmValue wasm_val;
                if (value.is_null()) {
                    wasm_val = std::monostate{};
                } else if (value.is_boolean()) {
                    wasm_val = value.get<bool>();
                } else if (value.is_number_integer()) {
                    wasm_val = value.get<int64_t>();
                } else if (value.is_number_float()) {
                    wasm_val = value.get<double>();
                } else if (value.is_string()) {
                    wasm_val = value.get<std::string>();
                }
                builder.WithParameter(key, wasm_val);
            }
        }

        if (request.contains("include_initial_data")) {
            builder.IncludeInitialData(request["include_initial_data"]);
        }

        if (request.contains("start_offset")) {
            builder.StartingAt(request["start_offset"]);
        }

        // Create the subscription
        auto query = builder.Build();
        std::string subscription_id = wasm_manager_->CreateSubscription(query);

        // Send response
        nlohmann::json response;
        response["type"] = "wasm_subscription_created";
        response["subscription_id"] = subscription_id;
        response["client_id"] = client_id;
        response["module_name"] = query.module_name;

        SendWebSocketFrame(client_socket, response.dump());
        spdlog::info("Created WASM subscription {} for client {}", subscription_id, client_id);

    } catch (const std::exception& e) {
        spdlog::error("Failed to handle WASM subscription request: {}", e.what());

        nlohmann::json error;
        error["type"] = "error";
        error["message"] = "Failed to create WASM subscription: " + std::string(e.what());
        SendWebSocketFrame(client_socket, error.dump());
    }
}

void WebSocketServer::HandleSqlSubscriptionRequest(int client_socket, const std::string& message) {
    if (!sql_manager_) {
        nlohmann::json error;
        error["type"] = "error";
        error["message"] = "SQL subscription manager not available";
        SendWebSocketFrame(client_socket, error.dump());
        return;
    }

    try {
        nlohmann::json request = nlohmann::json::parse(message);

        // Get client ID for this socket
        std::string client_id;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = client_ids_.find(client_socket);
            if (it != client_ids_.end()) {
                client_id = it->second;
            }
        }

        if (client_id.empty()) {
            nlohmann::json error;
            error["type"] = "error";
            error["message"] = "Client ID not found";
            SendWebSocketFrame(client_socket, error.dump());
            return;
        }

        // Extract SQL query
        if (!request.contains("sql")) {
            nlohmann::json error;
            error["type"] = "error";
            error["message"] = "sql field is required";
            SendWebSocketFrame(client_socket, error.dump());
            return;
        }

        std::string sql_query = request["sql"];

        // Create the SQL subscription
        std::string subscription_id = sql_manager_->Subscribe(client_id, sql_query);

        // Send response
        nlohmann::json response;
        response["type"] = "sql_subscription_created";
        response["subscription_id"] = subscription_id;
        response["client_id"] = client_id;
        response["sql"] = sql_query;

        SendWebSocketFrame(client_socket, response.dump());
        spdlog::info("Created SQL subscription {} for client {}: {}", subscription_id, client_id, sql_query);

        // Send initial state to the client
        SendInitialState(client_socket, sql_query, subscription_id);

    } catch (const std::exception& e) {
        spdlog::error("Failed to handle SQL subscription request: {}", e.what());

        nlohmann::json error;
        error["type"] = "error";
        error["message"] = "Failed to create SQL subscription: " + std::string(e.what());
        SendWebSocketFrame(client_socket, error.dump());
    }
}

void WebSocketServer::HandleSubscribeToAllTablesRequest(int client_socket, const std::string& message) {
    if (!sql_manager_) {
        nlohmann::json error;
        error["type"] = "error";
        error["message"] = "SQL subscription manager not available";
        SendWebSocketFrame(client_socket, error.dump());
        return;
    }

    try {
        // Get client ID for this socket
        std::string client_id;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = client_ids_.find(client_socket);
            if (it != client_ids_.end()) {
                client_id = it->second;
            }
        }

        if (client_id.empty()) {
            nlohmann::json error;
            error["type"] = "error";
            error["message"] = "Client ID not found";
            SendWebSocketFrame(client_socket, error.dump());
            return;
        }

        // Create the ALL_TABLES subscription using the cleaner API
        std::string subscription_id = sql_manager_->SubscribeToAllTables(client_id);

        // Send response
        nlohmann::json response;
        response["type"] = "all_tables_subscription_created";
        response["subscription_id"] = subscription_id;
        response["client_id"] = client_id;
        response["scope"] = "all_tables";

        SendWebSocketFrame(client_socket, response.dump());
        spdlog::info("Created ALL_TABLES subscription {} for client {}", subscription_id, client_id);

        // Send initial state for all tables to the client
        SendInitialStateAllTables(client_socket, subscription_id);

    } catch (const std::exception& e) {
        spdlog::error("Failed to handle subscribe to all tables request: {}", e.what());

        nlohmann::json error;
        error["type"] = "error";
        error["message"] = "Failed to create ALL_TABLES subscription: " + std::string(e.what());
        SendWebSocketFrame(client_socket, error.dump());
    }
}

void WebSocketServer::SendInitialState(int client_socket, const std::string& sql_query, const std::string& subscription_id) {
    if (!sql_engine_) {
        spdlog::error("SQL engine not available for initial state query");
        return;
    }

    try {
        // Execute the SQL query to get current state
        auto result = sql_engine_->Execute(sql_query);

        // The SQL engine does not push WHERE down onto these dynamically-created
        // (schema-less, WASM-written) tables, so the snapshot comes back
        // unfiltered. Compile the query's WHERE clause and filter the rows here —
        // the SAME predicate the live changefeed uses — so an area-of-interest
        // subscription's initial_state matches its ongoing events.
        std::unique_ptr<PredicateEvaluator> aoi_pred;
        {
            std::smatch wm;
            std::regex where_re(R"(\bWHERE\b\s+(.+?)(?:\bORDER\s+BY\b|\bLIMIT\b|$))",
                                std::regex::icase);
            if (std::regex_search(sql_query, wm, where_re)) {
                std::string wc = wm[1].str();
                std::string perr;
                aoi_pred = PredicateEvaluator::Parse(wc, perr);
                if (!aoi_pred) spdlog::warn("initial_state: WHERE parse failed ('{}'): {}", wc, perr);
            }
        }

        if (!result.success) {
            spdlog::error("Failed to execute initial state query: {}", result.error);
            nlohmann::json error;
            error["type"] = "initial_state_error";
            error["subscription_id"] = subscription_id;
            error["message"] = "Failed to fetch initial state: " + result.error;
            SendWebSocketFrame(client_socket, error.dump());
            return;
        }

        // Convert SQL result to JSON format
        nlohmann::json initial_state;
        initial_state["type"] = "initial_state";
        initial_state["subscription_id"] = subscription_id;
        initial_state["sql"] = sql_query;

        // Add column information
        nlohmann::json columns = nlohmann::json::array();
        for (const auto& col : result.columns) {
            nlohmann::json column;
            column["name"] = col.name;
            column["type"] = col.type;
            column["nullable"] = col.nullable;
            columns.push_back(column);
        }
        initial_state["columns"] = columns;

        // Add row data
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& row : result.rows) {
            nlohmann::json json_row;
            json_row["key"] = row.key;
            json_row["version"] = row.version;
            json_row["timestamp"] = row.timestamp;

            nlohmann::json row_data;
            for (const auto& [col_name, value] : row.columns) {
                // Convert Value variant to JSON
                std::visit([&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        row_data[col_name] = nullptr;
                    } else if constexpr (std::is_same_v<T, bool>) {
                        row_data[col_name] = v;
                    } else if constexpr (std::is_arithmetic_v<T>) {
                        row_data[col_name] = v;
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        row_data[col_name] = v;
                    } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                        // Convert binary data to base64 or hex string
                        std::string hex;
                        for (uint8_t byte : v) {
                            hex += fmt::format("{:02x}", byte);
                        }
                        row_data[col_name] = hex;
                    } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                        auto time_t = std::chrono::system_clock::to_time_t(v);
                        row_data[col_name] = time_t;
                    }
                }, value);
            }
            // Area-of-interest: drop rows the WHERE clause excludes.
            if (aoi_pred && !aoi_pred->Evaluate(row_data)) continue;

            json_row["data"] = row_data;
            rows.push_back(json_row);
        }
        initial_state["rows"] = rows;
        initial_state["rows_count"] = rows.size();
        initial_state["execution_time_ms"] = result.execution_time_ms;

        // Send the initial state to the client
        SendWebSocketFrame(client_socket, initial_state.dump());
        spdlog::info("Sent initial state for subscription {} ({} rows)", subscription_id, result.rows.size());

    } catch (const std::exception& e) {
        spdlog::error("Exception while sending initial state: {}", e.what());
        nlohmann::json error;
        error["type"] = "initial_state_error";
        error["subscription_id"] = subscription_id;
        error["message"] = "Exception while fetching initial state: " + std::string(e.what());
        SendWebSocketFrame(client_socket, error.dump());
    }
}

void WebSocketServer::SendInitialStateAllTables(int client_socket, const std::string& subscription_id) {
    if (!storage_engine_ || !sql_engine_) {
        spdlog::error("Storage engine or SQL engine not available for all tables initial state");
        return;
    }

    try {
        // Get list of all tables
        auto tables = storage_engine_->ListTables();

        nlohmann::json all_tables_state;
        all_tables_state["type"] = "initial_state_all_tables";
        all_tables_state["subscription_id"] = subscription_id;

        nlohmann::json tables_data = nlohmann::json::object();

        // Get current state for each table
        for (const auto& table_name : tables) {
            std::string sql_query = "SELECT * FROM " + table_name;
            auto result = sql_engine_->Execute(sql_query);

            if (result.success) {
                nlohmann::json table_state;

                // Add column information
                nlohmann::json columns = nlohmann::json::array();
                for (const auto& col : result.columns) {
                    nlohmann::json column;
                    column["name"] = col.name;
                    column["type"] = col.type;
                    column["nullable"] = col.nullable;
                    columns.push_back(column);
                }
                table_state["columns"] = columns;

                // Add row data
                nlohmann::json rows = nlohmann::json::array();
                for (const auto& row : result.rows) {
                    nlohmann::json json_row;
                    json_row["key"] = row.key;
                    json_row["version"] = row.version;
                    json_row["timestamp"] = row.timestamp;

                    nlohmann::json row_data;
                    for (const auto& [col_name, value] : row.columns) {
                        // Convert Value variant to JSON
                        std::visit([&](const auto& v) {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, std::monostate>) {
                                row_data[col_name] = nullptr;
                            } else if constexpr (std::is_same_v<T, bool>) {
                                row_data[col_name] = v;
                            } else if constexpr (std::is_arithmetic_v<T>) {
                                row_data[col_name] = v;
                            } else if constexpr (std::is_same_v<T, std::string>) {
                                row_data[col_name] = v;
                            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                                // Convert binary data to hex string
                                std::string hex;
                                for (uint8_t byte : v) {
                                    hex += fmt::format("{:02x}", byte);
                                }
                                row_data[col_name] = hex;
                            } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                                auto time_t = std::chrono::system_clock::to_time_t(v);
                                row_data[col_name] = time_t;
                            }
                        }, value);
                    }
                    json_row["data"] = row_data;
                    rows.push_back(json_row);
                }
                table_state["rows"] = rows;
                table_state["rows_count"] = result.rows.size();

                tables_data[table_name] = table_state;
            } else {
                spdlog::warn("Failed to get initial state for table '{}': {}", table_name, result.error);
                nlohmann::json error_state;
                error_state["error"] = result.error;
                error_state["rows_count"] = 0;
                tables_data[table_name] = error_state;
            }
        }

        all_tables_state["tables"] = tables_data;
        all_tables_state["tables_count"] = tables.size();

        // Send the initial state to the client
        SendWebSocketFrame(client_socket, all_tables_state.dump());
        spdlog::info("Sent initial state for all tables subscription {} ({} tables)", subscription_id, tables.size());

    } catch (const std::exception& e) {
        spdlog::error("Exception while sending initial state for all tables: {}", e.what());
        nlohmann::json error;
        error["type"] = "initial_state_error";
        error["subscription_id"] = subscription_id;
        error["message"] = "Exception while fetching initial state for all tables: " + std::string(e.what());
        SendWebSocketFrame(client_socket, error.dump());
    }
}

} // namespace origindb