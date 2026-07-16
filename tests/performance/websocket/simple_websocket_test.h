#pragma once

#include "../framework/performance_test.h"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace instantdb {
namespace performance {

// Simple WebSocket client configuration
struct SimpleWebSocketConfig {
    std::string server_host = "localhost";
    uint16_t server_port = 9090;
    uint32_t connection_timeout_ms = 5000;
    uint32_t max_connections = 100;
};

// Simple WebSocket client that uses basic socket connections
class SimpleWebSocketClient {
public:
    SimpleWebSocketClient(const SimpleWebSocketConfig& config, uint32_t client_id);
    ~SimpleWebSocketClient();

    bool Connect();
    void Disconnect();
    bool IsConnected() const { return connected_; }

    bool SendTextMessage(const std::string& message);
    bool SendBinaryMessage(const std::vector<uint8_t>& data);

    // Statistics
    uint64_t GetMessagesSent() const { return messages_sent_.load(); }
    uint64_t GetBytesReceived() const { return bytes_received_.load(); }
    uint64_t GetConnectionErrors() const { return connection_errors_.load(); }

private:
    SimpleWebSocketConfig config_;
    uint32_t client_id_;
    int socket_fd_;
    std::atomic<bool> connected_{false};

    // Statistics
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> connection_errors_{0};

    bool PerformHandshake();
    std::string CreateHandshakeRequest();
};

// Base class for simple WebSocket performance tests
class SimpleWebSocketTest : public PerformanceTest {
public:
    SimpleWebSocketTest(const TestConfig& config, const SimpleWebSocketConfig& ws_config);
    virtual ~SimpleWebSocketTest() = default;

    bool Setup() override;
    void Cleanup() override;

protected:
    SimpleWebSocketConfig ws_config_;
    std::vector<std::unique_ptr<SimpleWebSocketClient>> clients_;
    std::atomic<uint32_t> active_connections_{0};
};

// Simple connection scaling test
class SimpleConnectionScalingTest : public SimpleWebSocketTest {
public:
    SimpleConnectionScalingTest(const TestConfig& config,
                               const SimpleWebSocketConfig& ws_config,
                               uint32_t max_connections);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t max_connections_;
    std::atomic<uint32_t> connections_established_{0};
    std::atomic<uint32_t> connections_failed_{0};

    void EstablishConnections(uint32_t thread_id);
};

// Simple throughput test
class SimpleThroughputTest : public SimpleWebSocketTest {
public:
    SimpleThroughputTest(const TestConfig& config,
                        const SimpleWebSocketConfig& ws_config,
                        uint32_t message_size);

    void RunWorker(uint32_t thread_id) override;

private:
    std::string test_message_;
    std::atomic<uint64_t> total_messages_sent_{0};

    void SendMessages(uint32_t thread_id);
};

// Simple latency test using ping-pong messages
class SimpleLatencyTest : public SimpleWebSocketTest {
public:
    SimpleLatencyTest(const TestConfig& config,
                     const SimpleWebSocketConfig& ws_config);

    void RunWorker(uint32_t thread_id) override;

private:
    std::vector<double> round_trip_times_;
    std::mutex rtt_mutex_;

    void MeasureLatency(uint32_t thread_id);
};

// WebSocket test factory for simple tests
class SimpleWebSocketTestFactory {
public:
    static std::unique_ptr<SimpleWebSocketTest> CreateConnectionScalingTest(
        const TestConfig& test_config,
        const SimpleWebSocketConfig& ws_config,
        uint32_t max_connections
    );

    static std::unique_ptr<SimpleWebSocketTest> CreateThroughputTest(
        const TestConfig& test_config,
        const SimpleWebSocketConfig& ws_config,
        uint32_t message_size
    );

    static std::unique_ptr<SimpleWebSocketTest> CreateLatencyTest(
        const TestConfig& test_config,
        const SimpleWebSocketConfig& ws_config
    );

    // Predefined configurations
    static SimpleWebSocketConfig GetDefaultClientConfig();
};

} // namespace performance
} // namespace instantdb