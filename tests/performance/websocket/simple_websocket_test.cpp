#include "simple_websocket_test.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <thread>
#include <cstring>

namespace origindb {
namespace performance {

SimpleWebSocketClient::SimpleWebSocketClient(const SimpleWebSocketConfig& config, uint32_t client_id)
    : config_(config), client_id_(client_id), socket_fd_(-1) {
}

SimpleWebSocketClient::~SimpleWebSocketClient() {
    Disconnect();
}

bool SimpleWebSocketClient::Connect() {
    if (connected_) {
        return true;
    }

    // Create socket
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        connection_errors_++;
        return false;
    }

    // Set connection timeout
    struct timeval timeout;
    timeout.tv_sec = config_.connection_timeout_ms / 1000;
    timeout.tv_usec = (config_.connection_timeout_ms % 1000) * 1000;

    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        close(socket_fd_);
        connection_errors_++;
        return false;
    }

    // Connect to server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.server_port);

    if (inet_pton(AF_INET, config_.server_host.c_str(), &server_addr.sin_addr) <= 0) {
        close(socket_fd_);
        connection_errors_++;
        return false;
    }

    if (connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(socket_fd_);
        connection_errors_++;
        return false;
    }

    // Perform WebSocket handshake
    if (!PerformHandshake()) {
        close(socket_fd_);
        connection_errors_++;
        return false;
    }

    connected_ = true;
    return true;
}

void SimpleWebSocketClient::Disconnect() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    connected_ = false;
}

bool SimpleWebSocketClient::SendTextMessage(const std::string& message) {
    if (!connected_) {
        return false;
    }

    // Create WebSocket frame (simplified - text frame)
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + Text frame

    size_t payload_len = message.length();
    if (payload_len < 126) {
        frame.push_back(0x80 | payload_len); // MASK + payload length
    } else if (payload_len < 65536) {
        frame.push_back(0x80 | 126); // MASK + extended payload length
        frame.push_back((payload_len >> 8) & 0xFF);
        frame.push_back(payload_len & 0xFF);
    } else {
        frame.push_back(0x80 | 127); // MASK + extended payload length
        for (int i = 7; i >= 0; i--) {
            frame.push_back((payload_len >> (i * 8)) & 0xFF);
        }
    }

    // Add masking key (simple - use client_id based key)
    uint8_t mask[4] = {
        static_cast<uint8_t>((client_id_ >> 24) & 0xFF),
        static_cast<uint8_t>((client_id_ >> 16) & 0xFF),
        static_cast<uint8_t>((client_id_ >> 8) & 0xFF),
        static_cast<uint8_t>(client_id_ & 0xFF)
    };

    for (int i = 0; i < 4; i++) {
        frame.push_back(mask[i]);
    }

    // Add masked payload
    for (size_t i = 0; i < payload_len; i++) {
        frame.push_back(message[i] ^ mask[i % 4]);
    }

    // Send frame
    ssize_t sent = send(socket_fd_, frame.data(), frame.size(), 0);
    if (sent < 0 || static_cast<size_t>(sent) != frame.size()) {
        connection_errors_++;
        return false;
    }

    messages_sent_++;
    return true;
}

bool SimpleWebSocketClient::SendBinaryMessage(const std::vector<uint8_t>& data) {
    if (!connected_) {
        return false;
    }

    // Similar to text message but with binary frame type (0x82)
    std::vector<uint8_t> frame;
    frame.push_back(0x82); // FIN + Binary frame

    size_t payload_len = data.size();
    if (payload_len < 126) {
        frame.push_back(0x80 | payload_len);
    } else if (payload_len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((payload_len >> 8) & 0xFF);
        frame.push_back(payload_len & 0xFF);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((payload_len >> (i * 8)) & 0xFF);
        }
    }

    // Add masking key
    uint8_t mask[4] = {
        static_cast<uint8_t>((client_id_ >> 24) & 0xFF),
        static_cast<uint8_t>((client_id_ >> 16) & 0xFF),
        static_cast<uint8_t>((client_id_ >> 8) & 0xFF),
        static_cast<uint8_t>(client_id_ & 0xFF)
    };

    for (int i = 0; i < 4; i++) {
        frame.push_back(mask[i]);
    }

    // Add masked payload
    for (size_t i = 0; i < payload_len; i++) {
        frame.push_back(data[i] ^ mask[i % 4]);
    }

    ssize_t sent = send(socket_fd_, frame.data(), frame.size(), 0);
    if (sent < 0 || static_cast<size_t>(sent) != frame.size()) {
        connection_errors_++;
        return false;
    }

    messages_sent_++;
    return true;
}

bool SimpleWebSocketClient::PerformHandshake() {
    std::string handshake_request = CreateHandshakeRequest();

    ssize_t sent = send(socket_fd_, handshake_request.c_str(), handshake_request.length(), 0);
    if (sent < 0 || static_cast<size_t>(sent) != handshake_request.length()) {
        return false;
    }

    // Read handshake response (simplified - just check for 101 status)
    char buffer[1024];
    ssize_t received = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        return false;
    }

    buffer[received] = '\0';
    std::string response(buffer);

    bytes_received_ += received;

    // Check for successful handshake (HTTP 101 Switching Protocols)
    return response.find("101 Switching Protocols") != std::string::npos;
}

std::string SimpleWebSocketClient::CreateHandshakeRequest() {
    std::ostringstream request;
    request << "GET / HTTP/1.1\r\n";
    request << "Host: " << config_.server_host << ":" << config_.server_port << "\r\n";
    request << "Upgrade: websocket\r\n";
    request << "Connection: Upgrade\r\n";
    request << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"; // Fixed key for simplicity
    request << "Sec-WebSocket-Version: 13\r\n";
    request << "User-Agent: OriginDB-PerfTest-Client/" << client_id_ << "\r\n";
    request << "\r\n";

    return request.str();
}

// SimpleWebSocketTest implementation
SimpleWebSocketTest::SimpleWebSocketTest(const TestConfig& config, const SimpleWebSocketConfig& ws_config)
    : PerformanceTest(config), ws_config_(ws_config) {
}

bool SimpleWebSocketTest::Setup() {
    spdlog::info("Setting up simple WebSocket test with {} max connections", ws_config_.max_connections);
    return true;
}

void SimpleWebSocketTest::Cleanup() {
    clients_.clear();
}

// SimpleConnectionScalingTest implementation
SimpleConnectionScalingTest::SimpleConnectionScalingTest(const TestConfig& config,
                                                       const SimpleWebSocketConfig& ws_config,
                                                       uint32_t max_connections)
    : SimpleWebSocketTest(config, ws_config), max_connections_(max_connections) {
}

void SimpleConnectionScalingTest::RunWorker(uint32_t thread_id) {
    EstablishConnections(thread_id);
}

void SimpleConnectionScalingTest::EstablishConnections(uint32_t thread_id) {
    std::random_device rd;
    std::mt19937 gen(rd() + thread_id);
    std::uniform_int_distribution<> delay_dist(10, 100); // 10-100ms delay between connections

    uint32_t connections_per_thread = max_connections_ / config_.num_threads;
    uint32_t start_id = thread_id * connections_per_thread;
    uint32_t end_id = (thread_id == config_.num_threads - 1) ?
                      max_connections_ :
                      start_id + connections_per_thread;

    auto start_time = std::chrono::steady_clock::now();

    while (!stop_requested_ && connections_established_.load() < max_connections_) {
        for (uint32_t i = start_id; i < end_id && !stop_requested_; i++) {
            auto client = std::make_unique<SimpleWebSocketClient>(ws_config_, i);

            auto conn_start = std::chrono::high_resolution_clock::now();
            bool success = client->Connect();
            auto conn_end = std::chrono::high_resolution_clock::now();

            auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                conn_end - conn_start).count();

            if (success) {
                connections_established_++;
                active_connections_++;

                // Record latency for connection establishment
                {
                    std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
                    metrics_->latencies.push_back(static_cast<double>(latency_us));
                }

                // Keep connection alive for a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(gen)));

                metrics_->operations_completed++;
            } else {
                connections_failed_++;
                metrics_->operations_failed++;
            }

            // Check duration
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed >= std::chrono::seconds(config_.duration_seconds)) {
                stop_requested_ = true;
                break;
            }
        }
    }
}

// SimpleThroughputTest implementation
SimpleThroughputTest::SimpleThroughputTest(const TestConfig& config,
                                         const SimpleWebSocketConfig& ws_config,
                                         uint32_t message_size)
    : SimpleWebSocketTest(config, ws_config) {

    // Create test message of specified size
    test_message_.reserve(message_size);
    for (uint32_t i = 0; i < message_size; i++) {
        test_message_ += static_cast<char>('A' + (i % 26));
    }
}

void SimpleThroughputTest::RunWorker(uint32_t thread_id) {
    SendMessages(thread_id);
}

void SimpleThroughputTest::SendMessages(uint32_t thread_id) {
    // Create one client per thread
    auto client = std::make_unique<SimpleWebSocketClient>(ws_config_, thread_id);

    if (!client->Connect()) {
        spdlog::error("Thread {} failed to connect WebSocket client", thread_id);
        return;
    }

    active_connections_++;

    auto start_time = std::chrono::steady_clock::now();

    while (!stop_requested_) {
        auto send_start = std::chrono::high_resolution_clock::now();
        bool success = client->SendTextMessage(test_message_);
        auto send_end = std::chrono::high_resolution_clock::now();

        if (success) {
            auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                send_end - send_start).count();

            {
                std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
                metrics_->latencies.push_back(static_cast<double>(latency_us));
            }

            metrics_->operations_completed++;
            total_messages_sent_++;
        } else {
            metrics_->operations_failed++;
        }

        // Check duration
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= std::chrono::seconds(config_.duration_seconds)) {
            stop_requested_ = true;
            break;
        }

        // Small delay to prevent overwhelming
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// SimpleLatencyTest implementation
SimpleLatencyTest::SimpleLatencyTest(const TestConfig& config,
                                   const SimpleWebSocketConfig& ws_config)
    : SimpleWebSocketTest(config, ws_config) {
}

void SimpleLatencyTest::RunWorker(uint32_t thread_id) {
    MeasureLatency(thread_id);
}

void SimpleLatencyTest::MeasureLatency(uint32_t thread_id) {
    auto client = std::make_unique<SimpleWebSocketClient>(ws_config_, thread_id);

    if (!client->Connect()) {
        spdlog::error("Thread {} failed to connect WebSocket client for latency test", thread_id);
        return;
    }

    active_connections_++;

    auto start_time = std::chrono::steady_clock::now();

    while (!stop_requested_) {
        std::string ping_message = "ping_" + std::to_string(thread_id) + "_" +
                                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

        auto ping_start = std::chrono::high_resolution_clock::now();
        bool success = client->SendTextMessage(ping_message);

        if (success) {
            // For this simple test, we'll just measure send latency
            // In a real implementation, we'd wait for a pong response
            auto ping_end = std::chrono::high_resolution_clock::now();
            auto rtt_us = std::chrono::duration_cast<std::chrono::microseconds>(
                ping_end - ping_start).count();

            {
                std::lock_guard<std::mutex> lock(rtt_mutex_);
                round_trip_times_.push_back(static_cast<double>(rtt_us));
            }

            {
                std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
                metrics_->latencies.push_back(static_cast<double>(rtt_us));
            }

            metrics_->operations_completed++;
        } else {
            metrics_->operations_failed++;
        }

        // Wait before next ping
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Check duration
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= std::chrono::seconds(config_.duration_seconds)) {
            stop_requested_ = true;
            break;
        }
    }
}

// Factory implementations
std::unique_ptr<SimpleWebSocketTest> SimpleWebSocketTestFactory::CreateConnectionScalingTest(
    const TestConfig& test_config,
    const SimpleWebSocketConfig& ws_config,
    uint32_t max_connections) {

    return std::make_unique<SimpleConnectionScalingTest>(test_config, ws_config, max_connections);
}

std::unique_ptr<SimpleWebSocketTest> SimpleWebSocketTestFactory::CreateThroughputTest(
    const TestConfig& test_config,
    const SimpleWebSocketConfig& ws_config,
    uint32_t message_size) {

    return std::make_unique<SimpleThroughputTest>(test_config, ws_config, message_size);
}

std::unique_ptr<SimpleWebSocketTest> SimpleWebSocketTestFactory::CreateLatencyTest(
    const TestConfig& test_config,
    const SimpleWebSocketConfig& ws_config) {

    return std::make_unique<SimpleLatencyTest>(test_config, ws_config);
}

SimpleWebSocketConfig SimpleWebSocketTestFactory::GetDefaultClientConfig() {
    SimpleWebSocketConfig config;
    config.server_host = "localhost";
    config.server_port = 9090;
    config.connection_timeout_ms = 5000;
    config.max_connections = 100;
    return config;
}

} // namespace performance
} // namespace origindb