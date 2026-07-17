#pragma once

#include "../framework/performance_test.h"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/client.hpp>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace origindb {
namespace performance {

// WebSocket client configuration
struct WebSocketClientConfig {
    std::string server_host = "localhost";
    uint16_t server_port = 9090;
    std::string endpoint = "/";
    uint32_t connection_timeout_ms = 5000;
    uint32_t heartbeat_interval_ms = 30000;
    uint32_t max_reconnection_attempts = 5;
    uint32_t reconnection_delay_ms = 1000;
};

// Message types for testing
enum class MessageType {
    SUBSCRIPTION_REQUEST,
    SQL_QUERY_REQUEST,
    HEARTBEAT,
    CUSTOM_MESSAGE
};

// Test message structure
struct TestMessage {
    MessageType type;
    std::string payload;
    std::chrono::high_resolution_clock::time_point sent_time;
    uint64_t sequence_id;

    std::string ToJson() const;
    static TestMessage FromJson(const std::string& json);
};

// WebSocket client wrapper
class WebSocketClient {
public:
    using client = websocketpp::client<websocketpp::config::asio>;
    using message_ptr = websocketpp::config::asio::message_type::ptr;
    using connection_hdl = websocketpp::connection_hdl;

    WebSocketClient(const WebSocketClientConfig& config, uint32_t client_id);
    ~WebSocketClient();

    bool Connect();
    void Disconnect();
    bool IsConnected() const;

    bool SendMessage(const TestMessage& message);
    bool ReceiveMessage(TestMessage& message, uint32_t timeout_ms = 1000);

    // Statistics
    uint64_t GetMessagesSent() const { return messages_sent_.load(); }
    uint64_t GetMessagesReceived() const { return messages_received_.load(); }
    uint64_t GetConnectionErrors() const { return connection_errors_.load(); }
    uint64_t GetMessageErrors() const { return message_errors_.load(); }

private:
    WebSocketClientConfig config_;
    uint32_t client_id_;
    client ws_client_;
    connection_hdl connection_;
    std::thread io_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_stop_{false};

    // Message queues
    std::queue<TestMessage> incoming_messages_;
    std::mutex incoming_mutex_;
    std::condition_variable incoming_cv_;

    // Statistics
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> messages_received_{0};
    std::atomic<uint64_t> connection_errors_{0};
    std::atomic<uint64_t> message_errors_{0};

    void OnOpen(connection_hdl hdl);
    void OnClose(connection_hdl hdl);
    void OnMessage(connection_hdl hdl, message_ptr msg);
    void OnFail(connection_hdl hdl);

    void RunIoService();
};

// Base class for WebSocket performance tests
class WebSocketPerformanceTest : public PerformanceTest {
public:
    WebSocketPerformanceTest(const TestConfig& config, const WebSocketClientConfig& ws_config);
    virtual ~WebSocketPerformanceTest() = default;

    bool Setup() override;
    void Cleanup() override;

protected:
    WebSocketClientConfig ws_config_;
    std::vector<std::unique_ptr<WebSocketClient>> clients_;

    // Helper methods
    TestMessage CreateSubscriptionMessage(const std::string& table_filter = "*");
    TestMessage CreateSqlQueryMessage(const std::string& query);
    TestMessage CreateHeartbeatMessage();
    TestMessage CreateCustomMessage(const std::string& data);
};

// Connection scaling test
class ConnectionScalingTest : public WebSocketPerformanceTest {
public:
    ConnectionScalingTest(const TestConfig& config,
                         const WebSocketClientConfig& ws_config,
                         uint32_t max_connections = 1000,
                         uint32_t connection_rate = 10);  // connections per second

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t max_connections_;
    uint32_t connection_rate_;
    std::atomic<uint32_t> connections_established_{0};
    std::atomic<uint32_t> connections_failed_{0};

    void EstablishConnections(uint32_t thread_id);
};

// Message throughput test
class MessageThroughputTest : public WebSocketPerformanceTest {
public:
    MessageThroughputTest(const TestConfig& config,
                         const WebSocketClientConfig& ws_config,
                         MessageType message_type = MessageType::CUSTOM_MESSAGE,
                         uint32_t message_size = 1024);

    void RunWorker(uint32_t thread_id) override;

private:
    MessageType message_type_;
    uint32_t message_size_;
    std::atomic<uint64_t> global_sequence_{0};

    void SendMessages(uint32_t thread_id);
};

// Subscription performance test
class SubscriptionPerformanceTest : public WebSocketPerformanceTest {
public:
    SubscriptionPerformanceTest(const TestConfig& config,
                               const WebSocketClientConfig& ws_config,
                               uint32_t subscriptions_per_client = 5,
                               const std::vector<std::string>& table_filters = {"*"});

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t subscriptions_per_client_;
    std::vector<std::string> table_filters_;

    void ManageSubscriptions(uint32_t thread_id);
};

// Broadcast performance test
class BroadcastPerformanceTest : public WebSocketPerformanceTest {
public:
    BroadcastPerformanceTest(const TestConfig& config,
                            const WebSocketClientConfig& ws_config,
                            uint32_t receivers_count = 100,
                            uint32_t broadcast_rate = 10);  // broadcasts per second

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t receivers_count_;
    uint32_t broadcast_rate_;
    std::atomic<uint64_t> broadcasts_sent_{0};
    std::atomic<uint64_t> total_messages_received_{0};

    void BroadcastMessages(uint32_t thread_id);
    void ReceiveMessages(uint32_t thread_id);
};

// Latency measurement test
class LatencyMeasurementTest : public WebSocketPerformanceTest {
public:
    LatencyMeasurementTest(const TestConfig& config,
                          const WebSocketClientConfig& ws_config,
                          uint32_t ping_interval_ms = 1000);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t ping_interval_ms_;
    std::vector<double> round_trip_times_;
    std::mutex rtt_mutex_;

    void MeasureLatency(uint32_t thread_id);
};

// Connection stability test
class ConnectionStabilityTest : public WebSocketPerformanceTest {
public:
    ConnectionStabilityTest(const TestConfig& config,
                           const WebSocketClientConfig& ws_config,
                           uint32_t reconnection_interval_ms = 5000);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t reconnection_interval_ms_;
    std::atomic<uint32_t> reconnections_attempted_{0};
    std::atomic<uint32_t> reconnections_successful_{0};

    void TestConnectionStability(uint32_t thread_id);
};

// Real-time event simulation test
class RealTimeEventSimulationTest : public WebSocketPerformanceTest {
public:
    RealTimeEventSimulationTest(const TestConfig& config,
                                const WebSocketClientConfig& ws_config,
                                uint32_t event_rate_per_sec = 100,
                                uint32_t subscribers_count = 50);

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t event_rate_per_sec_;
    uint32_t subscribers_count_;
    std::atomic<uint64_t> events_published_{0};
    std::atomic<uint64_t> events_received_{0};

    void PublishEvents(uint32_t thread_id);
    void SubscribeToEvents(uint32_t thread_id);
};

// Memory usage test
class MemoryUsageTest : public WebSocketPerformanceTest {
public:
    MemoryUsageTest(const TestConfig& config,
                   const WebSocketClientConfig& ws_config,
                   uint32_t target_connections = 1000,
                   uint64_t target_memory_mb = 500);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t target_connections_;
    uint64_t target_memory_mb_;

    void EstablishAndMaintainConnections(uint32_t thread_id);
};

// WebSocket test factory
class WebSocketTestFactory {
public:
    static std::unique_ptr<WebSocketPerformanceTest> CreateConnectionScalingTest(
        const TestConfig& test_config,
        const WebSocketClientConfig& ws_config,
        uint32_t max_connections = 1000,
        uint32_t connection_rate = 10
    );

    static std::unique_ptr<WebSocketPerformanceTest> CreateMessageThroughputTest(
        const TestConfig& test_config,
        const WebSocketClientConfig& ws_config,
        MessageType message_type = MessageType::CUSTOM_MESSAGE,
        uint32_t message_size = 1024
    );

    static std::unique_ptr<WebSocketPerformanceTest> CreateBroadcastTest(
        const TestConfig& test_config,
        const WebSocketClientConfig& ws_config,
        uint32_t receivers_count = 100,
        uint32_t broadcast_rate = 10
    );

    static std::unique_ptr<WebSocketPerformanceTest> CreateLatencyTest(
        const TestConfig& test_config,
        const WebSocketClientConfig& ws_config,
        uint32_t ping_interval_ms = 1000
    );

    // Predefined configurations
    static WebSocketClientConfig GetDefaultClientConfig();
    static WebSocketClientConfig GetHighPerformanceConfig();
    static WebSocketClientConfig GetStabilityTestConfig();
};

} // namespace performance
} // namespace origindb