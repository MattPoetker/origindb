#include "websocket_stress_test.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <random>

namespace origindb {
namespace performance {

// TestMessage Implementation
std::string TestMessage::ToJson() const {
    nlohmann::json j;
    j["type"] = static_cast<int>(type);
    j["payload"] = payload;
    j["sequence_id"] = sequence_id;
    j["sent_time"] = std::chrono::duration_cast<std::chrono::microseconds>(
        sent_time.time_since_epoch()).count();
    return j.dump();
}

TestMessage TestMessage::FromJson(const std::string& json) {
    TestMessage msg;
    try {
        auto j = nlohmann::json::parse(json);
        msg.type = static_cast<MessageType>(j["type"]);
        msg.payload = j["payload"];
        msg.sequence_id = j["sequence_id"];

        auto time_us = j["sent_time"].get<int64_t>();
        msg.sent_time = std::chrono::high_resolution_clock::time_point(
            std::chrono::microseconds(time_us));
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse message JSON: {}", e.what());
    }
    return msg;
}

// WebSocketClient Implementation
WebSocketClient::WebSocketClient(const WebSocketClientConfig& config, uint32_t client_id)
    : config_(config), client_id_(client_id) {

    ws_client_.set_access_channels(websocketpp::log::alevel::all);
    ws_client_.clear_access_channels(websocketpp::log::alevel::frame_payload);
    ws_client_.set_error_channels(websocketpp::log::elevel::all);

    ws_client_.init_asio();

    ws_client_.set_open_handler([this](connection_hdl hdl) { OnOpen(hdl); });
    ws_client_.set_close_handler([this](connection_hdl hdl) { OnClose(hdl); });
    ws_client_.set_message_handler([this](connection_hdl hdl, message_ptr msg) {
        OnMessage(hdl, msg);
    });
    ws_client_.set_fail_handler([this](connection_hdl hdl) { OnFail(hdl); });

    // Set timeouts
    ws_client_.set_pong_timeout(config_.connection_timeout_ms);
}

WebSocketClient::~WebSocketClient() {
    Disconnect();
}

bool WebSocketClient::Connect() {
    if (connected_.load()) {
        return true;
    }

    try {
        std::string uri = "ws://" + config_.server_host + ":" +
                         std::to_string(config_.server_port) + config_.endpoint;

        websocketpp::lib::error_code ec;
        auto con = ws_client_.get_connection(uri, ec);

        if (ec) {
            spdlog::error("Client {}: Connection creation failed: {}", client_id_, ec.message());
            connection_errors_.fetch_add(1);
            return false;
        }

        connection_ = con->get_handle();
        ws_client_.connect(con);

        // Start IO service in background thread
        io_thread_ = std::thread(&WebSocketClient::RunIoService, this);

        // Wait for connection to be established (with timeout)
        auto start = std::chrono::steady_clock::now();
        while (!connected_.load() &&
               std::chrono::steady_clock::now() - start < std::chrono::milliseconds(config_.connection_timeout_ms)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return connected_.load();
    } catch (const std::exception& e) {
        spdlog::error("Client {}: Connection exception: {}", client_id_, e.what());
        connection_errors_.fetch_add(1);
        return false;
    }
}

void WebSocketClient::Disconnect() {
    should_stop_.store(true);

    if (connected_.load()) {
        try {
            ws_client_.close(connection_, websocketpp::close::status::going_away, "Test complete");
        } catch (const std::exception& e) {
            spdlog::warn("Client {}: Disconnect exception: {}", client_id_, e.what());
        }
    }

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    connected_.store(false);
}

bool WebSocketClient::IsConnected() const {
    return connected_.load();
}

bool WebSocketClient::SendMessage(const TestMessage& message) {
    if (!connected_.load()) {
        message_errors_.fetch_add(1);
        return false;
    }

    try {
        std::string json_message = message.ToJson();
        websocketpp::lib::error_code ec;

        ws_client_.send(connection_, json_message, websocketpp::frame::opcode::text, ec);

        if (ec) {
            spdlog::warn("Client {}: Send error: {}", client_id_, ec.message());
            message_errors_.fetch_add(1);
            return false;
        }

        messages_sent_.fetch_add(1);
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("Client {}: Send exception: {}", client_id_, e.what());
        message_errors_.fetch_add(1);
        return false;
    }
}

bool WebSocketClient::ReceiveMessage(TestMessage& message, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(incoming_mutex_);

    if (incoming_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [this] { return !incoming_messages_.empty() || should_stop_.load(); })) {

        if (!incoming_messages_.empty()) {
            message = incoming_messages_.front();
            incoming_messages_.pop();
            return true;
        }
    }

    return false;
}

void WebSocketClient::OnOpen(connection_hdl hdl) {
    connected_.store(true);
    spdlog::debug("Client {}: Connected", client_id_);
}

void WebSocketClient::OnClose(connection_hdl hdl) {
    connected_.store(false);
    spdlog::debug("Client {}: Disconnected", client_id_);
}

void WebSocketClient::OnMessage(connection_hdl hdl, message_ptr msg) {
    try {
        TestMessage test_msg = TestMessage::FromJson(msg->get_payload());

        {
            std::lock_guard<std::mutex> lock(incoming_mutex_);
            incoming_messages_.push(test_msg);
        }
        incoming_cv_.notify_one();

        messages_received_.fetch_add(1);
    } catch (const std::exception& e) {
        spdlog::warn("Client {}: Message processing error: {}", client_id_, e.what());
        message_errors_.fetch_add(1);
    }
}

void WebSocketClient::OnFail(connection_hdl hdl) {
    connected_.store(false);
    connection_errors_.fetch_add(1);
    spdlog::warn("Client {}: Connection failed", client_id_);
}

void WebSocketClient::RunIoService() {
    try {
        ws_client_.run();
    } catch (const std::exception& e) {
        spdlog::error("Client {}: IO service exception: {}", client_id_, e.what());
    }
}

// WebSocketPerformanceTest Implementation
WebSocketPerformanceTest::WebSocketPerformanceTest(const TestConfig& config,
                                                   const WebSocketClientConfig& ws_config)
    : PerformanceTest(config), ws_config_(ws_config) {
}

bool WebSocketPerformanceTest::Setup() {
    clients_.clear();
    clients_.reserve(config_.num_threads);

    spdlog::info("Setting up {} WebSocket clients...", config_.num_threads);

    for (uint32_t i = 0; i < config_.num_threads; ++i) {
        auto client = std::make_unique<WebSocketClient>(ws_config_, i);
        if (!client->Connect()) {
            spdlog::error("Failed to connect client {}", i);
            return false;
        }
        clients_.push_back(std::move(client));

        // Small delay between connections to avoid overwhelming server
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    spdlog::info("All {} WebSocket clients connected successfully", config_.num_threads);
    return true;
}

void WebSocketPerformanceTest::Cleanup() {
    spdlog::info("Disconnecting {} WebSocket clients...", clients_.size());

    for (auto& client : clients_) {
        if (client) {
            client->Disconnect();
        }
    }

    clients_.clear();
    spdlog::info("WebSocket cleanup complete");
}

TestMessage WebSocketPerformanceTest::CreateSubscriptionMessage(const std::string& table_filter) {
    TestMessage msg;
    msg.type = MessageType::SUBSCRIPTION_REQUEST;
    msg.sequence_id = 0;
    msg.sent_time = std::chrono::high_resolution_clock::now();

    nlohmann::json payload;
    payload["action"] = "subscribe";
    payload["table_filter"] = table_filter;
    msg.payload = payload.dump();

    return msg;
}

TestMessage WebSocketPerformanceTest::CreateSqlQueryMessage(const std::string& query) {
    TestMessage msg;
    msg.type = MessageType::SQL_QUERY_REQUEST;
    msg.sequence_id = 0;
    msg.sent_time = std::chrono::high_resolution_clock::now();

    nlohmann::json payload;
    payload["action"] = "query";
    payload["sql"] = query;
    msg.payload = payload.dump();

    return msg;
}

TestMessage WebSocketPerformanceTest::CreateHeartbeatMessage() {
    TestMessage msg;
    msg.type = MessageType::HEARTBEAT;
    msg.sequence_id = 0;
    msg.sent_time = std::chrono::high_resolution_clock::now();
    msg.payload = R"({"action": "ping"})";

    return msg;
}

TestMessage WebSocketPerformanceTest::CreateCustomMessage(const std::string& data) {
    TestMessage msg;
    msg.type = MessageType::CUSTOM_MESSAGE;
    msg.sequence_id = 0;
    msg.sent_time = std::chrono::high_resolution_clock::now();
    msg.payload = data;

    return msg;
}

// ConnectionScalingTest Implementation
ConnectionScalingTest::ConnectionScalingTest(const TestConfig& config,
                                           const WebSocketClientConfig& ws_config,
                                           uint32_t max_connections,
                                           uint32_t connection_rate)
    : WebSocketPerformanceTest(config, ws_config),
      max_connections_(max_connections), connection_rate_(connection_rate) {
}

bool ConnectionScalingTest::Setup() {
    // Don't use base setup - we'll create connections dynamically
    return true;
}

void ConnectionScalingTest::RunWorker(uint32_t thread_id) {
    EstablishConnections(thread_id);
}

void ConnectionScalingTest::EstablishConnections(uint32_t thread_id) {
    uint32_t connections_per_thread = max_connections_ / config_.num_threads;
    uint32_t rate_per_thread = std::max(1u, connection_rate_ / config_.num_threads);

    std::vector<std::unique_ptr<WebSocketClient>> thread_clients;
    thread_clients.reserve(connections_per_thread);

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    uint32_t established = 0;
    auto last_connection_time = start_time;

    while (std::chrono::steady_clock::now() < end_time && established < connections_per_thread) {
        auto now = std::chrono::steady_clock::now();

        // Rate limiting
        auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_connection_time);
        uint32_t min_interval = 1000 / rate_per_thread; // ms between connections

        if (time_since_last.count() < min_interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(min_interval - time_since_last.count()));
        }

        uint32_t client_id = thread_id * connections_per_thread + established;
        auto client = std::make_unique<WebSocketClient>(ws_config_, client_id);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->Connect();
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);

        if (success) {
            connections_established_.fetch_add(1);
            thread_clients.push_back(std::move(client));
        } else {
            connections_failed_.fetch_add(1);
        }

        established++;
        last_connection_time = std::chrono::steady_clock::now();
    }

    // Keep connections alive for remaining duration
    while (std::chrono::steady_clock::now() < end_time) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup thread connections
    for (auto& client : thread_clients) {
        if (client) {
            client->Disconnect();
        }
    }
}

// MessageThroughputTest Implementation
MessageThroughputTest::MessageThroughputTest(const TestConfig& config,
                                           const WebSocketClientConfig& ws_config,
                                           MessageType message_type,
                                           uint32_t message_size)
    : WebSocketPerformanceTest(config, ws_config),
      message_type_(message_type), message_size_(message_size) {
}

void MessageThroughputTest::RunWorker(uint32_t thread_id) {
    SendMessages(thread_id);
}

void MessageThroughputTest::SendMessages(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        spdlog::error("Thread {} has no corresponding client", thread_id);
        return;
    }

    auto& client = clients_[thread_id];
    if (!client->IsConnected()) {
        spdlog::error("Client {} is not connected", thread_id);
        return;
    }

    // Generate test data of specified size
    std::string large_data(message_size_, 'A' + (thread_id % 26));

    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        uint64_t sequence = global_sequence_.fetch_add(1);

        TestMessage msg;
        msg.sequence_id = sequence;
        msg.sent_time = std::chrono::high_resolution_clock::now();

        switch (message_type_) {
            case MessageType::SUBSCRIPTION_REQUEST:
                msg = CreateSubscriptionMessage("test_table_" + std::to_string(sequence % 10));
                break;
            case MessageType::SQL_QUERY_REQUEST:
                msg = CreateSqlQueryMessage("SELECT * FROM test_table WHERE id = " + std::to_string(sequence));
                break;
            case MessageType::HEARTBEAT:
                msg = CreateHeartbeatMessage();
                break;
            case MessageType::CUSTOM_MESSAGE:
            default:
                msg = CreateCustomMessage(large_data);
                break;
        }

        msg.sequence_id = sequence;

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->SendMessage(msg);
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);
        operations++;

        if (config_.target_ops_per_second > 0) {
            ThrottleOperations(operations, start_time);
        }
    }
}

// BroadcastPerformanceTest Implementation
BroadcastPerformanceTest::BroadcastPerformanceTest(const TestConfig& config,
                                                 const WebSocketClientConfig& ws_config,
                                                 uint32_t receivers_count,
                                                 uint32_t broadcast_rate)
    : WebSocketPerformanceTest(config, ws_config),
      receivers_count_(receivers_count), broadcast_rate_(broadcast_rate) {
}

bool BroadcastPerformanceTest::Setup() {
    // Create extra clients for receivers
    clients_.clear();
    clients_.reserve(config_.num_threads + receivers_count_);

    // Setup sender clients
    for (uint32_t i = 0; i < config_.num_threads; ++i) {
        auto client = std::make_unique<WebSocketClient>(ws_config_, i);
        if (!client->Connect()) {
            spdlog::error("Failed to connect sender client {}", i);
            return false;
        }
        clients_.push_back(std::move(client));
    }

    // Setup receiver clients
    for (uint32_t i = 0; i < receivers_count_; ++i) {
        auto client = std::make_unique<WebSocketClient>(ws_config_, config_.num_threads + i);
        if (!client->Connect()) {
            spdlog::error("Failed to connect receiver client {}", i);
            return false;
        }
        clients_.push_back(std::move(client));

        // Subscribe to broadcasts
        auto sub_msg = CreateSubscriptionMessage("broadcast_channel");
        client->SendMessage(sub_msg);
    }

    spdlog::info("Setup complete: {} senders, {} receivers", config_.num_threads, receivers_count_);
    return true;
}

void BroadcastPerformanceTest::RunWorker(uint32_t thread_id) {
    if (thread_id < config_.num_threads / 2) {
        BroadcastMessages(thread_id);
    } else {
        ReceiveMessages(thread_id);
    }
}

void BroadcastPerformanceTest::BroadcastMessages(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        return;
    }

    auto& client = clients_[thread_id];
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    std::string broadcast_data = "Broadcast from thread " + std::to_string(thread_id);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        auto msg = CreateCustomMessage(broadcast_data);
        msg.sequence_id = broadcasts_sent_.fetch_add(1);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->SendMessage(msg);
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);
        operations++;

        // Rate limiting for broadcasts
        if (broadcast_rate_ > 0) {
            uint32_t interval_ms = 1000 / broadcast_rate_;
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }
}

void BroadcastPerformanceTest::ReceiveMessages(uint32_t thread_id) {
    uint32_t receiver_idx = thread_id + receivers_count_;
    if (receiver_idx >= clients_.size()) {
        return;
    }

    auto& client = clients_[receiver_idx];
    auto end_time = std::chrono::steady_clock::now() + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        TestMessage msg;
        if (client->ReceiveMessage(msg, 1000)) {
            total_messages_received_.fetch_add(1);
        }
    }
}

// LatencyMeasurementTest Implementation
LatencyMeasurementTest::LatencyMeasurementTest(const TestConfig& config,
                                             const WebSocketClientConfig& ws_config,
                                             uint32_t ping_interval_ms)
    : WebSocketPerformanceTest(config, ws_config), ping_interval_ms_(ping_interval_ms) {
}

void LatencyMeasurementTest::RunWorker(uint32_t thread_id) {
    MeasureLatency(thread_id);
}

void LatencyMeasurementTest::MeasureLatency(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        return;
    }

    auto& client = clients_[thread_id];
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        auto ping_msg = CreateHeartbeatMessage();
        ping_msg.sequence_id = thread_id;

        auto send_time = std::chrono::high_resolution_clock::now();
        bool success = client->SendMessage(ping_msg);

        if (success) {
            TestMessage response;
            if (client->ReceiveMessage(response, ping_interval_ms_)) {
                auto receive_time = std::chrono::high_resolution_clock::now();
                double rtt_ms = std::chrono::duration<double, std::milli>(receive_time - send_time).count();

                {
                    std::lock_guard<std::mutex> lock(rtt_mutex_);
                    round_trip_times_.push_back(rtt_ms);
                }

                RecordOperation(true, send_time, receive_time);
            } else {
                RecordOperation(false, send_time, std::chrono::high_resolution_clock::now());
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(ping_interval_ms_));
    }
}

// WebSocketTestFactory Implementation
std::unique_ptr<WebSocketPerformanceTest> WebSocketTestFactory::CreateConnectionScalingTest(
    const TestConfig& test_config,
    const WebSocketClientConfig& ws_config,
    uint32_t max_connections,
    uint32_t connection_rate) {

    return std::make_unique<ConnectionScalingTest>(test_config, ws_config, max_connections, connection_rate);
}

std::unique_ptr<WebSocketPerformanceTest> WebSocketTestFactory::CreateMessageThroughputTest(
    const TestConfig& test_config,
    const WebSocketClientConfig& ws_config,
    MessageType message_type,
    uint32_t message_size) {

    return std::make_unique<MessageThroughputTest>(test_config, ws_config, message_type, message_size);
}

std::unique_ptr<WebSocketPerformanceTest> WebSocketTestFactory::CreateBroadcastTest(
    const TestConfig& test_config,
    const WebSocketClientConfig& ws_config,
    uint32_t receivers_count,
    uint32_t broadcast_rate) {

    return std::make_unique<BroadcastPerformanceTest>(test_config, ws_config, receivers_count, broadcast_rate);
}

std::unique_ptr<WebSocketPerformanceTest> WebSocketTestFactory::CreateLatencyTest(
    const TestConfig& test_config,
    const WebSocketClientConfig& ws_config,
    uint32_t ping_interval_ms) {

    return std::make_unique<LatencyMeasurementTest>(test_config, ws_config, ping_interval_ms);
}

// Predefined configurations
WebSocketClientConfig WebSocketTestFactory::GetDefaultClientConfig() {
    WebSocketClientConfig config;
    config.server_host = "localhost";
    config.server_port = 9090;
    config.endpoint = "/";
    config.connection_timeout_ms = 5000;
    config.heartbeat_interval_ms = 30000;
    config.max_reconnection_attempts = 5;
    config.reconnection_delay_ms = 1000;
    return config;
}

WebSocketClientConfig WebSocketTestFactory::GetHighPerformanceConfig() {
    auto config = GetDefaultClientConfig();
    config.connection_timeout_ms = 2000;
    config.heartbeat_interval_ms = 10000;
    return config;
}

WebSocketClientConfig WebSocketTestFactory::GetStabilityTestConfig() {
    auto config = GetDefaultClientConfig();
    config.connection_timeout_ms = 10000;
    config.heartbeat_interval_ms = 60000;
    config.max_reconnection_attempts = 10;
    config.reconnection_delay_ms = 2000;
    return config;
}

} // namespace performance
} // namespace origindb