#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include <map>
#include <mutex>
#include <fstream>

namespace instantdb {
namespace performance {

// Performance metrics structure
struct Metrics {
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point end_time;

    std::atomic<uint64_t> operations_completed{0};
    std::atomic<uint64_t> operations_failed{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};

    std::vector<double> latencies;  // In microseconds
    mutable std::mutex latencies_mutex;

    // Resource usage
    double peak_memory_mb = 0.0;
    double avg_cpu_percent = 0.0;
    uint32_t peak_connections = 0;
    uint32_t peak_threads = 0;

    // Computed metrics
    double GetDurationSeconds() const {
        return std::chrono::duration<double>(end_time - start_time).count();
    }

    double GetThroughputOpsPerSec() const {
        double duration = GetDurationSeconds();
        return duration > 0 ? operations_completed.load() / duration : 0.0;
    }

    double GetAverageLatencyUs() const {
        std::lock_guard<std::mutex> lock(latencies_mutex);
        if (latencies.empty()) return 0.0;

        double sum = 0.0;
        for (double lat : latencies) sum += lat;
        return sum / latencies.size();
    }

    double GetPercentileLatencyUs(double percentile) const {
        std::lock_guard<std::mutex> lock(latencies_mutex);
        if (latencies.empty()) return 0.0;

        auto sorted = latencies;
        std::sort(sorted.begin(), sorted.end());

        size_t index = static_cast<size_t>((percentile / 100.0) * sorted.size());
        if (index >= sorted.size()) index = sorted.size() - 1;

        return sorted[index];
    }

    void RecordLatency(double latency_us) {
        std::lock_guard<std::mutex> lock(latencies_mutex);
        latencies.push_back(latency_us);
    }
};

// Test configuration
struct TestConfig {
    std::string test_name;
    uint32_t num_threads = 1;
    uint32_t duration_seconds = 60;
    uint32_t warmup_seconds = 5;
    uint32_t target_ops_per_second = 0;  // 0 = unlimited
    uint32_t batch_size = 1;
    bool enable_latency_tracking = true;
    bool enable_resource_monitoring = true;
    std::string output_file;

    // Server connection settings
    std::string server_host = "localhost";
    uint16_t server_port = 9090;
    uint16_t grpc_port = 50051;
    std::string connection_string;
};

// Base class for performance tests
class PerformanceTest {
public:
    PerformanceTest(const TestConfig& config) : config_(config), metrics_(std::make_shared<Metrics>()) {}
    virtual ~PerformanceTest() = default;

    // Main test execution
    virtual bool Run();

    // Test-specific implementation
    virtual bool Setup() = 0;
    virtual void RunWorker(uint32_t thread_id) = 0;
    virtual void Cleanup() = 0;

    // Results
    std::shared_ptr<Metrics> GetMetrics() const { return metrics_; }
    void SaveResults(const std::string& filename = "");
    void PrintSummary() const;

protected:
    TestConfig config_;
    std::shared_ptr<Metrics> metrics_;
    std::atomic<bool> stop_requested_{false};

    // Helper functions
    void RecordOperation(bool success, uint64_t bytes_sent = 0, uint64_t bytes_received = 0);
    void RecordLatency(std::chrono::high_resolution_clock::time_point start);

private:
    void MonitorResources();
    void RateLimitWorker(uint32_t target_ops_per_sec, uint32_t thread_id);

    std::thread resource_monitor_thread_;
    std::vector<std::thread> worker_threads_;
};

// Resource monitoring utilities
class ResourceMonitor {
public:
    static double GetMemoryUsageMB();
    static double GetCpuUsagePercent();
    static uint32_t GetThreadCount();
    static uint32_t GetOpenFileDescriptors();
    static void GetNetworkStats(uint64_t& bytes_sent, uint64_t& bytes_received);
};

// Test result reporter
class TestReporter {
public:
    static void SaveToJson(const std::string& filename, const std::vector<std::shared_ptr<Metrics>>& results);
    static void SaveToCsv(const std::string& filename, const std::vector<std::shared_ptr<Metrics>>& results);
    static void GenerateHtmlReport(const std::string& filename, const std::vector<std::shared_ptr<Metrics>>& results);
    static void PrintComparison(const std::vector<std::shared_ptr<Metrics>>& results);
};

// Test suite runner
class TestSuite {
public:
    void AddTest(std::unique_ptr<PerformanceTest> test);
    void RunAll();
    void SaveResults(const std::string& directory);

private:
    std::vector<std::unique_ptr<PerformanceTest>> tests_;
    std::vector<std::shared_ptr<Metrics>> results_;
};

} // namespace performance
} // namespace instantdb