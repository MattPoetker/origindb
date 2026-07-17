#include "performance_test.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <random>

#ifdef __APPLE__
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/host_info.h>
#elif __linux__
#include <sys/resource.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <fstream>
#endif

namespace origindb {
namespace performance {

// PerformanceTest implementation
bool PerformanceTest::Run() {
    spdlog::info("🚀 Starting performance test: {}", config_.test_name);
    spdlog::info("Configuration: {} threads, {} seconds, target {} ops/sec",
                 config_.num_threads, config_.duration_seconds, config_.target_ops_per_second);

    // Setup phase
    if (!Setup()) {
        spdlog::error("❌ Test setup failed");
        return false;
    }

    // Start resource monitoring
    if (config_.enable_resource_monitoring) {
        resource_monitor_thread_ = std::thread(&PerformanceTest::MonitorResources, this);
    }

    // Warmup phase
    if (config_.warmup_seconds > 0) {
        spdlog::info("🔥 Warming up for {} seconds...", config_.warmup_seconds);

        stop_requested_ = false;
        metrics_->start_time = std::chrono::high_resolution_clock::now();

        // Start warmup workers
        for (uint32_t i = 0; i < config_.num_threads; ++i) {
            worker_threads_.emplace_back(&PerformanceTest::RunWorker, this, i);
        }

        std::this_thread::sleep_for(std::chrono::seconds(config_.warmup_seconds));

        // Stop warmup
        stop_requested_ = true;
        for (auto& thread : worker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        worker_threads_.clear();

        // Reset metrics after warmup
        metrics_ = std::make_shared<Metrics>();
        spdlog::info("✅ Warmup complete, starting actual test...");
    }

    // Main test phase
    stop_requested_ = false;
    metrics_->start_time = std::chrono::high_resolution_clock::now();

    // Start worker threads
    for (uint32_t i = 0; i < config_.num_threads; ++i) {
        worker_threads_.emplace_back(&PerformanceTest::RunWorker, this, i);
    }

    // Run for specified duration
    std::this_thread::sleep_for(std::chrono::seconds(config_.duration_seconds));

    // Stop test
    stop_requested_ = true;
    metrics_->end_time = std::chrono::high_resolution_clock::now();

    // Wait for workers to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Stop resource monitoring
    if (resource_monitor_thread_.joinable()) {
        resource_monitor_thread_.join();
    }

    // Cleanup
    Cleanup();

    spdlog::info("✅ Test completed: {} ops in {:.2f}s ({:.2f} ops/sec)",
                 metrics_->operations_completed.load(),
                 metrics_->GetDurationSeconds(),
                 metrics_->GetThroughputOpsPerSec());

    return true;
}

void PerformanceTest::RecordOperation(bool success, uint64_t bytes_sent, uint64_t bytes_received) {
    if (success) {
        metrics_->operations_completed.fetch_add(1, std::memory_order_relaxed);
    } else {
        metrics_->operations_failed.fetch_add(1, std::memory_order_relaxed);
    }

    if (bytes_sent > 0) {
        metrics_->bytes_sent.fetch_add(bytes_sent, std::memory_order_relaxed);
    }

    if (bytes_received > 0) {
        metrics_->bytes_received.fetch_add(bytes_received, std::memory_order_relaxed);
    }
}

void PerformanceTest::RecordLatency(std::chrono::high_resolution_clock::time_point start) {
    if (!config_.enable_latency_tracking) return;

    auto end = std::chrono::high_resolution_clock::now();
    auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    metrics_->RecordLatency(static_cast<double>(latency_us));
}

void PerformanceTest::MonitorResources() {
    double peak_memory = 0.0;
    double total_cpu = 0.0;
    uint32_t cpu_samples = 0;

    while (!stop_requested_) {
        // Memory monitoring
        double current_memory = ResourceMonitor::GetMemoryUsageMB();
        if (current_memory > peak_memory) {
            peak_memory = current_memory;
        }

        // CPU monitoring
        double current_cpu = ResourceMonitor::GetCpuUsagePercent();
        if (current_cpu >= 0) {
            total_cpu += current_cpu;
            cpu_samples++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    metrics_->peak_memory_mb = peak_memory;
    if (cpu_samples > 0) {
        metrics_->avg_cpu_percent = total_cpu / cpu_samples;
    }
}

void PerformanceTest::PrintSummary() const {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "  PERFORMANCE TEST RESULTS: " << config_.test_name << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "Duration:              " << metrics_->GetDurationSeconds() << " seconds" << std::endl;
    std::cout << "Operations Completed:  " << metrics_->operations_completed.load() << std::endl;
    std::cout << "Operations Failed:     " << metrics_->operations_failed.load() << std::endl;
    std::cout << "Throughput:           " << metrics_->GetThroughputOpsPerSec() << " ops/sec" << std::endl;

    if (config_.enable_latency_tracking && !metrics_->latencies.empty()) {
        std::cout << "\nLatency Statistics (μs):" << std::endl;
        std::cout << "  Average:     " << std::setw(8) << metrics_->GetAverageLatencyUs() << std::endl;
        std::cout << "  50th %tile:  " << std::setw(8) << metrics_->GetPercentileLatencyUs(50) << std::endl;
        std::cout << "  90th %tile:  " << std::setw(8) << metrics_->GetPercentileLatencyUs(90) << std::endl;
        std::cout << "  95th %tile:  " << std::setw(8) << metrics_->GetPercentileLatencyUs(95) << std::endl;
        std::cout << "  99th %tile:  " << std::setw(8) << metrics_->GetPercentileLatencyUs(99) << std::endl;
    }

    if (config_.enable_resource_monitoring) {
        std::cout << "\nResource Usage:" << std::endl;
        std::cout << "  Peak Memory:   " << std::setw(8) << metrics_->peak_memory_mb << " MB" << std::endl;
        std::cout << "  Avg CPU:       " << std::setw(8) << metrics_->avg_cpu_percent << " %" << std::endl;
    }

    if (metrics_->bytes_sent.load() > 0 || metrics_->bytes_received.load() > 0) {
        std::cout << "\nNetwork I/O:" << std::endl;
        std::cout << "  Bytes Sent:     " << metrics_->bytes_sent.load() / 1024 << " KB" << std::endl;
        std::cout << "  Bytes Received: " << metrics_->bytes_received.load() / 1024 << " KB" << std::endl;
    }

    std::cout << std::string(60, '=') << std::endl << std::endl;
}

void PerformanceTest::SaveResults(const std::string& filename) {
    std::string output_file = filename.empty() ? config_.output_file : filename;
    if (output_file.empty()) {
        output_file = config_.test_name + "_results.json";
    }

    nlohmann::json results;
    results["test_name"] = config_.test_name;
    results["config"] = {
        {"num_threads", config_.num_threads},
        {"duration_seconds", config_.duration_seconds},
        {"warmup_seconds", config_.warmup_seconds},
        {"target_ops_per_second", config_.target_ops_per_second},
        {"batch_size", config_.batch_size}
    };

    results["metrics"] = {
        {"duration_seconds", metrics_->GetDurationSeconds()},
        {"operations_completed", metrics_->operations_completed.load()},
        {"operations_failed", metrics_->operations_failed.load()},
        {"throughput_ops_per_sec", metrics_->GetThroughputOpsPerSec()},
        {"peak_memory_mb", metrics_->peak_memory_mb},
        {"avg_cpu_percent", metrics_->avg_cpu_percent},
        {"bytes_sent", metrics_->bytes_sent.load()},
        {"bytes_received", metrics_->bytes_received.load()}
    };

    if (config_.enable_latency_tracking && !metrics_->latencies.empty()) {
        results["latency"] = {
            {"average_us", metrics_->GetAverageLatencyUs()},
            {"p50_us", metrics_->GetPercentileLatencyUs(50)},
            {"p90_us", metrics_->GetPercentileLatencyUs(90)},
            {"p95_us", metrics_->GetPercentileLatencyUs(95)},
            {"p99_us", metrics_->GetPercentileLatencyUs(99)}
        };
    }

    std::ofstream file(output_file);
    file << results.dump(2) << std::endl;

    spdlog::info("💾 Results saved to: {}", output_file);
}

// ResourceMonitor implementation
#ifdef __APPLE__
double ResourceMonitor::GetMemoryUsageMB() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<double>(usage.ru_maxrss) / 1024.0 / 1024.0;  // Convert to MB
    }
    return -1.0;
}

double ResourceMonitor::GetCpuUsagePercent() {
    // This is a simplified implementation
    // For accurate CPU monitoring on macOS, use more sophisticated methods
    return -1.0;  // Not implemented for this demo
}
#elif __linux__
double ResourceMonitor::GetMemoryUsageMB() {
    std::ifstream file("/proc/self/status");
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("VmRSS:") == 0) {
            size_t kb_pos = line.find("kB");
            if (kb_pos != std::string::npos) {
                std::string kb_str = line.substr(6, kb_pos - 6);
                double kb = std::stod(kb_str);
                return kb / 1024.0;  // Convert to MB
            }
        }
    }
    return -1.0;
}

double ResourceMonitor::GetCpuUsagePercent() {
    // Simplified implementation - could be improved with more accurate measurement
    return -1.0;  // Not implemented for this demo
}
#else
double ResourceMonitor::GetMemoryUsageMB() { return -1.0; }
double ResourceMonitor::GetCpuUsagePercent() { return -1.0; }
#endif

uint32_t ResourceMonitor::GetThreadCount() {
    // Platform-specific implementation would go here
    return 0;
}

uint32_t ResourceMonitor::GetOpenFileDescriptors() {
    // Platform-specific implementation would go here
    return 0;
}

void ResourceMonitor::GetNetworkStats(uint64_t& bytes_sent, uint64_t& bytes_received) {
    // Platform-specific implementation would go here
    bytes_sent = 0;
    bytes_received = 0;
}

// TestSuite implementation
void TestSuite::AddTest(std::unique_ptr<PerformanceTest> test) {
    tests_.push_back(std::move(test));
}

void TestSuite::RunAll() {
    results_.clear();

    for (auto& test : tests_) {
        if (test->Run()) {
            results_.push_back(test->GetMetrics());
            test->PrintSummary();
        }
    }

    TestReporter::PrintComparison(results_);
}

void TestSuite::SaveResults(const std::string& directory) {
    if (!results_.empty()) {
        TestReporter::SaveToJson(directory + "/test_results.json", results_);
        TestReporter::SaveToCsv(directory + "/test_results.csv", results_);
        TestReporter::GenerateHtmlReport(directory + "/test_results.html", results_);
    }
}

// TestReporter implementation
void TestReporter::SaveToJson(const std::string& filename, const std::vector<std::shared_ptr<Metrics>>& results) {
    nlohmann::json json_results = nlohmann::json::array();

    for (const auto& metrics : results) {
        nlohmann::json result;
        result["duration_seconds"] = metrics->GetDurationSeconds();
        result["operations_completed"] = metrics->operations_completed.load();
        result["operations_failed"] = metrics->operations_failed.load();
        result["throughput_ops_per_sec"] = metrics->GetThroughputOpsPerSec();
        result["peak_memory_mb"] = metrics->peak_memory_mb;
        result["avg_cpu_percent"] = metrics->avg_cpu_percent;

        if (!metrics->latencies.empty()) {
            result["latency_avg_us"] = metrics->GetAverageLatencyUs();
            result["latency_p95_us"] = metrics->GetPercentileLatencyUs(95);
            result["latency_p99_us"] = metrics->GetPercentileLatencyUs(99);
        }

        json_results.push_back(result);
    }

    std::ofstream file(filename);
    file << json_results.dump(2) << std::endl;

    spdlog::info("📊 Combined results saved to: {}", filename);
}

void TestReporter::SaveToCsv(const std::string& filename, const std::vector<std::shared_ptr<Metrics>>& results) {
    std::ofstream file(filename);
    file << "test_id,duration_seconds,operations_completed,operations_failed,throughput_ops_per_sec,peak_memory_mb,avg_cpu_percent,latency_avg_us,latency_p95_us,latency_p99_us\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& metrics = results[i];
        file << i << ","
             << metrics->GetDurationSeconds() << ","
             << metrics->operations_completed.load() << ","
             << metrics->operations_failed.load() << ","
             << metrics->GetThroughputOpsPerSec() << ","
             << metrics->peak_memory_mb << ","
             << metrics->avg_cpu_percent << ","
             << metrics->GetAverageLatencyUs() << ","
             << metrics->GetPercentileLatencyUs(95) << ","
             << metrics->GetPercentileLatencyUs(99) << "\n";
    }

    spdlog::info("📈 CSV results saved to: {}", filename);
}

void TestReporter::GenerateHtmlReport(const std::string& filename, const std::vector<std::shared_ptr<Metrics>>& results) {
    std::ofstream file(filename);
    file << R"(<!DOCTYPE html>
<html>
<head>
    <title>OriginDB Performance Test Results</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; }
        .chart-container { width: 800px; height: 400px; margin: 20px 0; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #ddd; padding: 8px; text-align: right; }
        th { background-color: #f2f2f2; }
    </style>
</head>
<body>
    <h1>OriginDB Performance Test Results</h1>
    <div class="chart-container">
        <canvas id="throughputChart"></canvas>
    </div>
    <div class="chart-container">
        <canvas id="latencyChart"></canvas>
    </div>
    <table>
        <tr>
            <th>Test</th>
            <th>Operations</th>
            <th>Throughput (ops/sec)</th>
            <th>Avg Latency (μs)</th>
            <th>P95 Latency (μs)</th>
            <th>Memory (MB)</th>
        </tr>)";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& metrics = results[i];
        file << "<tr>"
             << "<td>Test " << i + 1 << "</td>"
             << "<td>" << metrics->operations_completed.load() << "</td>"
             << "<td>" << std::fixed << std::setprecision(2) << metrics->GetThroughputOpsPerSec() << "</td>"
             << "<td>" << std::fixed << std::setprecision(1) << metrics->GetAverageLatencyUs() << "</td>"
             << "<td>" << std::fixed << std::setprecision(1) << metrics->GetPercentileLatencyUs(95) << "</td>"
             << "<td>" << std::fixed << std::setprecision(1) << metrics->peak_memory_mb << "</td>"
             << "</tr>";
    }

    file << R"(    </table>
</body>
</html>)";

    spdlog::info("📊 HTML report saved to: {}", filename);
}

void TestReporter::PrintComparison(const std::vector<std::shared_ptr<Metrics>>& results) {
    if (results.empty()) return;

    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "  PERFORMANCE TEST COMPARISON" << std::endl;
    std::cout << std::string(80, '=') << std::endl;

    std::cout << std::left << std::setw(10) << "Test"
              << std::right << std::setw(12) << "Ops"
              << std::setw(12) << "Ops/Sec"
              << std::setw(12) << "Avg Lat(μs)"
              << std::setw(12) << "P95 Lat(μs)"
              << std::setw(12) << "Memory(MB)" << std::endl;

    std::cout << std::string(80, '-') << std::endl;

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& metrics = results[i];
        std::cout << std::left << std::setw(10) << ("Test " + std::to_string(i + 1))
                  << std::right << std::fixed << std::setprecision(0)
                  << std::setw(12) << metrics->operations_completed.load()
                  << std::setw(12) << std::setprecision(1) << metrics->GetThroughputOpsPerSec()
                  << std::setw(12) << std::setprecision(1) << metrics->GetAverageLatencyUs()
                  << std::setw(12) << std::setprecision(1) << metrics->GetPercentileLatencyUs(95)
                  << std::setw(12) << std::setprecision(1) << metrics->peak_memory_mb << std::endl;
    }

    std::cout << std::string(80, '=') << std::endl << std::endl;
}

} // namespace performance
} // namespace origindb