#include "net/websocket_server.h"
#include "changefeed/changefeed_engine.h"
#include <spdlog/spdlog.h>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace instantdb {

using json = nlohmann::json;
using server = websocketpp::server<websocketpp::config::asio>;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

// WebSocket connection implementation
class WebSocketConnectionImpl : public WebSocketConnection {
public:
    WebSocketConnectionImpl(const std::string& id,
                           websocketpp::connection_hdl hdl,
                           server* server_ptr)
        : id_(id), hdl_(hdl), server_(server_ptr), connected_(true) {}

    std::string GetId() const override {
        return id_;
    }

    std::string GetRemoteAddress() const override {
        try {
            auto con = server_->get_con_from_hdl(hdl_);
            return con->get_remote_endpoint();
        } catch (...) {
            return "unknown";
        }
    }

    bool IsConnected() const override {
        return connected_.load();
    }

    void Send(const std::string& message) override {
        if (!connected_) return;

        try {
            server_->send(hdl_, message, websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            spdlog::error("Error sending message to connection {}: {}", id_, e.what());
            connected_ = false;
        }
    }

    void SendBinary(const std::vector<uint8_t>& data) override {
        if (!connected_) return;

        try {
            std::string binary_data(data.begin(), data.end());
            server_->send(hdl_, binary_data, websocketpp::frame::opcode::binary);
        } catch (const std::exception& e) {
            spdlog::error("Error sending binary data to connection {}: {}", id_, e.what());
            connected_ = false;
        }
    }

    void Close(uint16_t code, const std::string& reason) override {
        if (!connected_) return;

        try {
            server_->close(hdl_, code, reason);
            connected_ = false;
        } catch (const std::exception& e) {
            spdlog::error("Error closing connection {}: {}", id_, e.what());
        }
    }

    void SetDisconnected() {
        connected_ = false;
    }

    websocketpp::connection_hdl GetHandle() const {
        return hdl_;
    }

private:
    std::string id_;
    websocketpp::connection_hdl hdl_;
    server* server_;
    std::atomic<bool> connected_;
};

class WebSocketServer::Impl {
public:
    Impl(const WebSocketConfig& config, std::shared_ptr<ChangefeedEngine> changefeed)
        : config_(config), changefeed_(changefeed), running_(false), next_conn_id_(1) {}

    bool Initialize() {
        try {
            // Set up server
            server_.set_access_channels(websocketpp::log::alevel::all);
            server_.clear_access_channels(websocketpp::log::alevel::frame_payload);
            server_.set_error_channels(websocketpp::log::elevel::all);

            server_.init_asio();
            server_.set_reuse_addr(true);

            // Set handlers
            server_.set_open_handler(bind(&Impl::OnOpen, this, _1));
            server_.set_close_handler(bind(&Impl::OnClose, this, _1));
            server_.set_message_handler(bind(&Impl::OnMessage, this, _1, _2));

            spdlog::info("WebSocket server initialized");
            return true;

        } catch (const std::exception& e) {
            spdlog::error("Failed to initialize WebSocket server: {}", e.what());
            return false;
        }
    }

    bool Start() {
        if (running_) {
            return true;
        }

        try {
            // Parse listen address
            auto pos = config_.listen_address.find(':');
            if (pos == std::string::npos) {
                spdlog::error("Invalid listen address format: {}", config_.listen_address);
                return false;
            }

            std::string host = config_.listen_address.substr(0, pos);
            uint16_t port = std::stoi(config_.listen_address.substr(pos + 1));

            server_.listen(host, port);
            server_.start_accept();

            running_ = true;

            // Start server thread
            server_thread_ = std::thread([this]() {
                try {
                    server_.run();
                } catch (const std::exception& e) {
                    spdlog::error("WebSocket server error: {}", e.what());
                }
            });

            spdlog::info("WebSocket server started on {}", config_.listen_address);
            return true;

        } catch (const std::exception& e) {
            spdlog::error("Failed to start WebSocket server: {}", e.what());
            return false;
        }
    }

    void Stop() {
        if (!running_) {
            return;
        }

        running_ = false;

        try {
            server_.stop();

            if (server_thread_.joinable()) {
                server_thread_.join();
            }

            // Clear connections
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.clear();

            spdlog::info("WebSocket server stopped");

        } catch (const std::exception& e) {
            spdlog::error("Error stopping WebSocket server: {}", e.what());
        }
    }

    size_t GetConnectionCount() const {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        return connections_.size();
    }

    std::vector<std::string> GetConnectionIds() const {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        std::vector<std::string> ids;
        for (const auto& [id, _] : connections_) {
            ids.push_back(id);
        }
        return ids;
    }

    std::shared_ptr<WebSocketConnection> GetConnection(const std::string& id) const {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(id);
        return (it != connections_.end()) ? it->second : nullptr;
    }

    void Broadcast(const std::string& message) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [id, conn] : connections_) {
            if (conn->IsConnected()) {
                conn->Send(message);
            }
        }
    }

    void BroadcastBinary(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [id, conn] : connections_) {
            if (conn->IsConnected()) {
                conn->SendBinary(data);
            }
        }
    }

private:
    void OnOpen(websocketpp::connection_hdl hdl) {
        std::string conn_id = "conn-" + std::to_string(next_conn_id_++);

        auto connection = std::make_shared<WebSocketConnectionImpl>(
            conn_id, hdl, &server_);

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[conn_id] = connection;
            hdl_to_id_[hdl] = conn_id;
        }

        spdlog::info("WebSocket connection opened: {} from {}",
                    conn_id, connection->GetRemoteAddress());
    }

    void OnClose(websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        auto hdl_it = hdl_to_id_.find(hdl);
        if (hdl_it != hdl_to_id_.end()) {
            std::string conn_id = hdl_it->second;

            auto conn_it = connections_.find(conn_id);
            if (conn_it != connections_.end()) {
                auto impl = std::static_pointer_cast<WebSocketConnectionImpl>(conn_it->second);
                impl->SetDisconnected();

                // Remove from changefeed subscriptions
                CleanupSubscriptions(conn_id);

                connections_.erase(conn_it);
            }

            hdl_to_id_.erase(hdl_it);

            spdlog::info("WebSocket connection closed: {}", conn_id);
        }
    }

    void OnMessage(websocketpp::connection_hdl hdl,
                   server::message_ptr msg) {
        try {
            std::string conn_id;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = hdl_to_id_.find(hdl);
                if (it == hdl_to_id_.end()) {
                    return;
                }
                conn_id = it->second;
            }

            std::string payload = msg->get_payload();
            spdlog::debug("Received message from {}: {}", conn_id, payload);

            // Parse JSON message
            json message = json::parse(payload);
            HandleMessage(conn_id, message);

        } catch (const std::exception& e) {
            spdlog::error("Error handling WebSocket message: {}", e.what());
        }
    }

    void HandleMessage(const std::string& conn_id, const json& message) {
        std::string action = message.value("action", "");

        if (action == "subscribe") {
            HandleSubscribe(conn_id, message);
        } else if (action == "unsubscribe") {
            HandleUnsubscribe(conn_id, message);
        } else if (action == "ack") {
            HandleAcknowledge(conn_id, message);
        } else if (action == "credits") {
            HandleFlowControl(conn_id, message);
        } else {
            spdlog::warn("Unknown action from {}: {}", conn_id, action);
        }
    }

    void HandleSubscribe(const std::string& conn_id, const json& message) {
        std::string subscription_id = message.value("subscription", "");
        std::string sql_filter = message.value("sql", "");
        uint64_t start_offset = message.value("start_offset", 0);
        std::string mode_str = message.value("mode", "at_least_once");

        DeliveryMode mode = (mode_str == "exactly_once") ?
            DeliveryMode::EXACTLY_ONCE : DeliveryMode::AT_LEAST_ONCE;

        // Create changefeed subscription
        SubscriptionFilter filter;
        filter.sql_where_clause = sql_filter;
        filter.table_pattern = "*"; // Accept all tables for prototype

        std::string cf_sub_id = changefeed_->CreateSubscription(filter, start_offset, mode);

        // Store mapping
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscriptions_[subscription_id] = {conn_id, cf_sub_id};
        }

        // Set up delivery callback
        changefeed_->Subscribe(cf_sub_id, [this, conn_id, subscription_id]
                              (const std::string& cf_sub_id, const ChangefeedEvent& event) {
            DeliverEvent(conn_id, subscription_id, event);
        });

        // Send acknowledgment
        json ack;
        ack["type"] = "sub_ack";
        ack["subscription"] = subscription_id;
        ack["start_offset"] = start_offset;

        SendToConnection(conn_id, ack.dump());

        spdlog::info("Created subscription {} for connection {}",
                    subscription_id, conn_id);
    }

    void HandleUnsubscribe(const std::string& conn_id, const json& message) {
        std::string subscription_id = message.value("subscription", "");

        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        auto it = subscriptions_.find(subscription_id);
        if (it != subscriptions_.end() && it->second.conn_id == conn_id) {
            changefeed_->Unsubscribe(it->second.cf_sub_id);
            changefeed_->DeleteSubscription(it->second.cf_sub_id);
            subscriptions_.erase(it);

            spdlog::info("Removed subscription {} for connection {}",
                        subscription_id, conn_id);
        }
    }

    void HandleAcknowledge(const std::string& conn_id, const json& message) {
        std::string subscription_id = message.value("subscription", "");
        uint64_t offset = message.value("offset", 0);

        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        auto it = subscriptions_.find(subscription_id);
        if (it != subscriptions_.end() && it->second.conn_id == conn_id) {
            changefeed_->AcknowledgeEvent(it->second.cf_sub_id, offset);
        }
    }

    void HandleFlowControl(const std::string& conn_id, const json& message) {
        std::string subscription_id = message.value("subscription", "");
        uint32_t credits = message.value("credits", 0);

        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        auto it = subscriptions_.find(subscription_id);
        if (it != subscriptions_.end() && it->second.conn_id == conn_id) {
            changefeed_->GrantCredits(it->second.cf_sub_id, credits);
        }
    }

    void DeliverEvent(const std::string& conn_id,
                     const std::string& subscription_id,
                     const ChangefeedEvent& event) {
        json event_msg;
        event_msg["type"] = "event";
        event_msg["subscription"] = subscription_id;
        event_msg["offset"] = event.offset;
        event_msg["tx_id"] = event.transaction_id;
        event_msg["table"] = event.table;
        event_msg["op"] = event.operation;
        event_msg["key"] = std::string(event.key.begin(), event.key.end());

        // Convert value to JSON (simplified for prototype)
        if (!event.new_value.empty()) {
            std::string value_str(event.new_value.begin(), event.new_value.end());
            event_msg["value"] = value_str;
        }

        SendToConnection(conn_id, event_msg.dump());
    }

    void SendToConnection(const std::string& conn_id, const std::string& message) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it != connections_.end() && it->second->IsConnected()) {
            it->second->Send(message);
        }
    }

    void CleanupSubscriptions(const std::string& conn_id) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        auto it = subscriptions_.begin();
        while (it != subscriptions_.end()) {
            if (it->second.conn_id == conn_id) {
                changefeed_->Unsubscribe(it->second.cf_sub_id);
                changefeed_->DeleteSubscription(it->second.cf_sub_id);
                it = subscriptions_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct SubscriptionMapping {
        std::string conn_id;
        std::string cf_sub_id;
    };

    WebSocketConfig config_;
    std::shared_ptr<ChangefeedEngine> changefeed_;

    server server_;
    std::atomic<bool> running_;
    std::thread server_thread_;

    mutable std::mutex connections_mutex_;
    std::unordered_map<std::string, std::shared_ptr<WebSocketConnection>> connections_;
    std::unordered_map<websocketpp::connection_hdl,
                      std::string,
                      std::owner_less<websocketpp::connection_hdl>> hdl_to_id_;
    std::atomic<uint64_t> next_conn_id_;

    std::mutex subscriptions_mutex_;
    std::unordered_map<std::string, SubscriptionMapping> subscriptions_;
};

WebSocketServer::WebSocketServer(const WebSocketConfig& config,
                                std::shared_ptr<ChangefeedEngine> changefeed)
    : impl_(std::make_unique<Impl>(config, changefeed)) {}

WebSocketServer::~WebSocketServer() = default;

bool WebSocketServer::Initialize() {
    return impl_->Initialize();
}

bool WebSocketServer::Start() {
    return impl_->Start();
}

void WebSocketServer::Stop() {
    impl_->Stop();
}

size_t WebSocketServer::GetConnectionCount() const {
    return impl_->GetConnectionCount();
}

std::vector<std::string> WebSocketServer::GetConnectionIds() const {
    return impl_->GetConnectionIds();
}

std::shared_ptr<WebSocketConnection> WebSocketServer::GetConnection(const std::string& id) const {
    return impl_->GetConnection(id);
}

void WebSocketServer::Broadcast(const std::string& message) {
    impl_->Broadcast(message);
}

void WebSocketServer::BroadcastBinary(const std::vector<uint8_t>& data) {
    impl_->BroadcastBinary(data);
}

} // namespace instantdb