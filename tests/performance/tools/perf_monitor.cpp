#include "../framework/performance_test.h"
// #include "../grpc/grpc_load_test.h"  // Disabled for now
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>

namespace origindb {
namespace performance {

struct SystemMetrics {
    double cpu_usage_percent = 0.0;
    double memory_usage_mb = 0.0;
    double memory_usage_percent = 0.0;
    uint64_t network_bytes_in = 0;
    uint64_t network_bytes_out = 0;
    uint64_t disk_reads = 0;
    uint64_t disk_writes = 0;
    std::chrono::system_clock::time_point timestamp;

    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["cpu_usage_percent"] = cpu_usage_percent;
        j["memory_usage_mb"] = memory_usage_mb;
        j["memory_usage_percent"] = memory_usage_percent;
        j["network_bytes_in"] = network_bytes_in;
        j["network_bytes_out"] = network_bytes_out;
        j["disk_reads"] = disk_reads;
        j["disk_writes"] = disk_writes;

        auto time_t_val = std::chrono::system_clock::to_time_t(timestamp);
        j["timestamp"] = std::put_time(std::localtime(&time_t_val), "%Y-%m-%d %H:%M:%S");

        return j;
    }
};

struct ServerStatus {
    bool is_responding = false;
    uint64_t connections_active = 0;
    uint64_t requests_per_second = 0;
    uint64_t total_requests = 0;
    double avg_response_time_ms = 0.0;
    std::string server_version;
    std::chrono::system_clock::time_point timestamp;

    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["is_responding"] = is_responding;
        j["connections_active"] = connections_active;
        j["requests_per_second"] = requests_per_second;
        j["total_requests"] = total_requests;
        j["avg_response_time_ms"] = avg_response_time_ms;
        j["server_version"] = server_version;

        auto time_t_val = std::chrono::system_clock::to_time_t(timestamp);
        j["timestamp"] = std::put_time(std::localtime(&time_t_val), "%Y-%m-%d %H:%M:%S");

        return j;
    }
};

class SystemMonitor {
public:
    SystemMonitor() = default;

    SystemMetrics GetCurrentMetrics() {
        SystemMetrics metrics;
        metrics.timestamp = std::chrono::system_clock::now();

#ifdef __APPLE__
        // macOS-specific system monitoring
        metrics.cpu_usage_percent = GetMacOSCpuUsage();
        metrics.memory_usage_mb = GetMacOSMemoryUsage();
        metrics.memory_usage_percent = GetMacOSMemoryPercent();
#elif __linux__
        // Linux-specific system monitoring
        metrics.cpu_usage_percent = GetLinuxCpuUsage();
        metrics.memory_usage_mb = GetLinuxMemoryUsage();
        metrics.memory_usage_percent = GetLinuxMemoryPercent();
#else
        // Generic fallback
        spdlog::warn("System monitoring not implemented for this platform");
#endif

        return metrics;
    }

private:
#ifdef __APPLE__
    double GetMacOSCpuUsage() {
        // Simplified CPU usage - in production this would use proper system APIs
        FILE* pipe = popen("ps -A -o %cpu | awk '{s+=$1} END {print s}'", "r");
        if (!pipe) return 0.0;

        char buffer[128];
        if (fgets(buffer, 128, pipe) != NULL) {
            pclose(pipe);
            return std::atof(buffer);
        }
        pclose(pipe);
        return 0.0;
    }

    double GetMacOSMemoryUsage() {
        FILE* pipe = popen("vm_stat | grep 'Pages active' | awk '{print $3}' | sed 's/\\.//'", "r");
        if (!pipe) return 0.0;

        char buffer[128];
        if (fgets(buffer, 128, pipe) != NULL) {
            pclose(pipe);
            uint64_t pages = std::atoll(buffer);
            return (pages * 4096) / (1024.0 * 1024.0); // Convert to MB
        }
        pclose(pipe);
        return 0.0;
    }

    double GetMacOSMemoryPercent() {
        // Simplified - would use proper system calls in production
        return std::min(95.0, GetMacOSMemoryUsage() / 80.0); // Assume 8GB system
    }
#endif

#ifdef __linux__
    double GetLinuxCpuUsage() {
        std::ifstream file("/proc/loadavg");
        if (!file.is_open()) return 0.0;

        double load;
        file >> load;
        return std::min(100.0, load * 25.0); // Rough conversion
    }

    double GetLinuxMemoryUsage() {
        std::ifstream file("/proc/meminfo");
        if (!file.is_open()) return 0.0;

        std::string line;
        uint64_t total_kb = 0, available_kb = 0;

        while (std::getline(file, line)) {
            if (line.substr(0, 9) == "MemTotal:") {
                std::istringstream iss(line);
                std::string label, unit;
                iss >> label >> total_kb >> unit;
            } else if (line.substr(0, 13) == "MemAvailable:") {
                std::istringstream iss(line);
                std::string label, unit;
                iss >> label >> available_kb >> unit;
                break;
            }
        }

        uint64_t used_kb = total_kb - available_kb;
        return used_kb / 1024.0; // Convert to MB
    }

    double GetLinuxMemoryPercent() {
        double used_mb = GetLinuxMemoryUsage();
        // Get total memory from /proc/meminfo
        std::ifstream file("/proc/meminfo");
        if (!file.is_open()) return 0.0;

        std::string line;
        while (std::getline(file, line)) {
            if (line.substr(0, 9) == "MemTotal:") {
                std::istringstream iss(line);
                std::string label, unit;
                uint64_t total_kb;
                iss >> label >> total_kb >> unit;
                double total_mb = total_kb / 1024.0;
                return (used_mb / total_mb) * 100.0;
            }
        }
        return 0.0;
    }
#endif
};

class ServerMonitor {
public:
    ServerMonitor(const std::string& grpc_address) : grpc_address_(grpc_address) {
        auto config = GrpcTestFactory::GetDefaultClientConfig();
        config.server_address = grpc_address;
        client_ = std::make_unique<GrpcClient>(config, 0);
    }

    ServerStatus GetServerStatus() {
        ServerStatus status;
        status.timestamp = std::chrono::system_clock::now();

        if (!client_->IsConnected()) {
            if (!client_->Connect()) {
                status.is_responding = false;
                return status;
            }
        }

        try {
            origindb::grpc::StatusResponse response;
            auto start = std::chrono::high_resolution_clock::now();
            bool success = client_->GetServerStatus(&response);
            auto end = std::chrono::high_resolution_clock::now();

            status.is_responding = success;
            if (success) {
                status.connections_active = response.connections_active();
                status.requests_per_second = response.requests_per_second();
                status.total_requests = response.total_requests();
                status.server_version = response.version();

                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                status.avg_response_time_ms = duration.count() / 1000.0;
            }
        } catch (const std::exception& e) {
            spdlog::warn("Error getting server status: {}", e.what());
            status.is_responding = false;
        }

        return status;
    }

private:
    std::string grpc_address_;
    std::unique_ptr<GrpcClient> client_;
};

class PerformanceMonitor {
public:
    PerformanceMonitor(const std::string& grpc_address, uint32_t monitoring_interval_ms = 5000)
        : grpc_address_(grpc_address),
          monitoring_interval_(std::chrono::milliseconds(monitoring_interval_ms)),
          system_monitor_(),
          server_monitor_(grpc_address),
          should_stop_(false) {
    }

    void Start() {
        spdlog::info("🔍 Starting performance monitoring...");
        spdlog::info("gRPC Server: {}", grpc_address_);
        spdlog::info("Monitoring Interval: {}ms", monitoring_interval_.count());

        monitoring_thread_ = std::thread(&PerformanceMonitor::MonitoringLoop, this);
        display_thread_ = std::thread(&PerformanceMonitor::DisplayLoop, this);
    }

    void Stop() {
        should_stop_.store(true);

        if (monitoring_thread_.joinable()) {
            monitoring_thread_.join();
        }
        if (display_thread_.joinable()) {
            display_thread_.join();
        }

        spdlog::info("Performance monitoring stopped");
    }

    void ExportResults(const std::string& filename) {
        std::ofstream file(filename);
        if (!file.is_open()) {
            spdlog::error("Could not open file for export: {}", filename);
            return;
        }

        nlohmann::json j;
        j["monitoring_session"] = {
            {"grpc_address", grpc_address_},
            {"monitoring_interval_ms", monitoring_interval_.count()},
            {"session_duration_s", GetSessionDurationSeconds()},
            {"data_points", system_metrics_.size()}
        };

        // Export system metrics
        nlohmann::json system_data = nlohmann::json::array();
        for (const auto& metric : system_metrics_) {
            system_data.push_back(metric.ToJson());
        }
        j["system_metrics"] = system_data;

        // Export server status data
        nlohmann::json server_data = nlohmann::json::array();
        for (const auto& status : server_status_) {
            server_data.push_back(status.ToJson());
        }
        j["server_status"] = server_data;

        file << j.dump(2);
        file.close();

        spdlog::info("Monitoring results exported to: {}", filename);
    }

    void PrintSummary() {
        if (system_metrics_.empty() || server_status_.empty()) {
            spdlog::warn("No monitoring data available for summary");
            return;
        }

        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "📊 PERFORMANCE MONITORING SUMMARY\n";
        std::cout << std::string(60, '=') << "\n";

        // Session info
        std::cout << "Session Duration: " << GetSessionDurationSeconds() << " seconds\n";
        std::cout << "Data Points: " << system_metrics_.size() << "\n";
        std::cout << "Server: " << grpc_address_ << "\n\n";

        // System metrics summary
        if (!system_metrics_.empty()) {
            double avg_cpu = 0, max_cpu = 0, avg_memory = 0, max_memory = 0;

            for (const auto& metric : system_metrics_) {
                avg_cpu += metric.cpu_usage_percent;
                avg_memory += metric.memory_usage_mb;
                max_cpu = std::max(max_cpu, metric.cpu_usage_percent);
                max_memory = std::max(max_memory, metric.memory_usage_mb);
            }

            avg_cpu /= system_metrics_.size();
            avg_memory /= system_metrics_.size();

            std::cout << "🖥️  SYSTEM METRICS:\n";
            std::cout << "   CPU Usage - Avg: " << std::fixed << std::setprecision(1) << avg_cpu
                      << "%, Peak: " << max_cpu << "%\n";
            std::cout << "   Memory Usage - Avg: " << std::fixed << std::setprecision(1) << avg_memory
                      << " MB, Peak: " << max_memory << " MB\n\n";
        }

        // Server status summary
        if (!server_status_.empty()) {
            uint64_t max_connections = 0, max_rps = 0;
            double avg_response_time = 0;
            int responding_count = 0;

            for (const auto& status : server_status_) {
                if (status.is_responding) {
                    responding_count++;
                    max_connections = std::max(max_connections, status.connections_active);
                    max_rps = std::max(max_rps, status.requests_per_second);
                    avg_response_time += status.avg_response_time_ms;
                }
            }

            double availability = (100.0 * responding_count) / server_status_.size();
            if (responding_count > 0) {
                avg_response_time /= responding_count;
            }

            std::cout << "🌐 SERVER METRICS:\n";
            std::cout << "   Availability: " << std::fixed << std::setprecision(1) << availability << "%\n";
            std::cout << "   Peak Connections: " << max_connections << "\n";
            std::cout << "   Peak RPS: " << max_rps << "\n";
            std::cout << "   Avg Response Time: " << std::fixed << std::setprecision(2)
                      << avg_response_time << " ms\n";

            if (!server_status_.empty() && !server_status_.back().server_version.empty()) {
                std::cout << "   Server Version: " << server_status_.back().server_version << "\n";
            }
        }

        std::cout << std::string(60, '=') << "\n";
    }

private:
    std::string grpc_address_;
    std::chrono::milliseconds monitoring_interval_;
    SystemMonitor system_monitor_;
    ServerMonitor server_monitor_;

    std::atomic<bool> should_stop_;
    std::thread monitoring_thread_;
    std::thread display_thread_;

    std::vector<SystemMetrics> system_metrics_;
    std::vector<ServerStatus> server_status_;
    std::chrono::system_clock::time_point session_start_;

    void MonitoringLoop() {
        session_start_ = std::chrono::system_clock::now();

        while (!should_stop_.load()) {
            try {
                // Collect system metrics
                auto system_metric = system_monitor_.GetCurrentMetrics();
                system_metrics_.push_back(system_metric);

                // Collect server status
                auto server_status = server_monitor_.GetServerStatus();
                server_status_.push_back(server_status);

                // Limit history size to prevent memory growth
                if (system_metrics_.size() > 1000) {
                    system_metrics_.erase(system_metrics_.begin(), system_metrics_.begin() + 500);
                }
                if (server_status_.size() > 1000) {
                    server_status_.erase(server_status_.begin(), server_status_.begin() + 500);
                }

            } catch (const std::exception& e) {
                spdlog::warn("Error during monitoring: {}", e.what());
            }

            std::this_thread::sleep_for(monitoring_interval_);
        }
    }

    void DisplayLoop() {
        while (!should_stop_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));

            if (!system_metrics_.empty() && !server_status_.empty()) {
                const auto& latest_system = system_metrics_.back();
                const auto& latest_server = server_status_.back();

                // Clear screen and show current status
                std::cout << "\033[2J\033[H"; // ANSI clear screen and move cursor to top

                std::cout << "🔍 OriginDB Performance Monitor - " << GetCurrentTimestamp() << "\n";
                std::cout << std::string(70, '=') << "\n";

                std::cout << "🖥️  SYSTEM STATUS:\n";
                std::cout << "   CPU: " << std::fixed << std::setprecision(1) << latest_system.cpu_usage_percent << "%";
                std::cout << "   Memory: " << std::fixed << std::setprecision(1) << latest_system.memory_usage_mb << " MB";
                std::cout << " (" << std::fixed << std::setprecision(1) << latest_system.memory_usage_percent << "%)\n";

                std::cout << "\n🌐 SERVER STATUS:\n";
                std::cout << "   Status: " << (latest_server.is_responding ? "🟢 ONLINE" : "🔴 OFFLINE") << "\n";
                if (latest_server.is_responding) {
                    std::cout << "   Connections: " << latest_server.connections_active << "\n";
                    std::cout << "   RPS: " << latest_server.requests_per_second << "\n";
                    std::cout << "   Avg Response: " << std::fixed << std::setprecision(2)
                              << latest_server.avg_response_time_ms << " ms\n";
                    std::cout << "   Total Requests: " << latest_server.total_requests << "\n";
                }

                std::cout << "\n📊 SESSION INFO:\n";
                std::cout << "   Duration: " << GetSessionDurationSeconds() << " seconds\n";
                std::cout << "   Data Points: " << system_metrics_.size() << "\n";
                std::cout << "   Server: " << grpc_address_ << "\n";

                std::cout << "\n" << std::string(70, '=') << "\n";
                std::cout << "Press Ctrl+C to stop monitoring...\n";
            }
        }
    }

    std::string GetCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    double GetSessionDurationSeconds() {
        if (session_start_.time_since_epoch().count() == 0) return 0.0;

        auto now = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - session_start_);
        return duration.count();
    }
};

} // namespace performance
} // namespace origindb

void PrintUsage() {
    std::cout << R"(
OriginDB Performance Monitor

Usage: perf_monitor [OPTIONS]

Options:
  --server <ADDRESS>     gRPC server address (default: localhost:50051)
  --interval <MS>        Monitoring interval in milliseconds (default: 5000)
  --duration <SECONDS>   Monitoring duration in seconds (default: unlimited)
  --output <FILE>        Export results to file (optional)
  --quiet               Suppress real-time display
  --help                Show this help message

Examples:
  # Monitor server with default settings
  perf_monitor

  # Monitor specific server with custom interval
  perf_monitor --server localhost:50052 --interval 2000

  # Monitor for 5 minutes and export results
  perf_monitor --duration 300 --output monitoring_results.json

  # Quiet monitoring with export only
  perf_monitor --quiet --duration 120 --output results.json

)";
}

int main(int argc, char* argv[]) {
    std::string server_address = "localhost:50051";
    uint32_t interval_ms = 5000;
    uint32_t duration_seconds = 0; // 0 means unlimited
    std::string output_file;
    bool quiet = false;
    bool help = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            help = true;
        } else if (arg == "--server" && i + 1 < argc) {
            server_address = argv[++i];
        } else if (arg == "--interval" && i + 1 < argc) {
            interval_ms = std::stoul(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_seconds = std::stoul(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            PrintUsage();
            return 1;
        }
    }

    if (help) {
        PrintUsage();
        return 0;
    }

    // Configure logging
    if (quiet) {
        spdlog::set_level(spdlog::level::warn);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    spdlog::info("🔍 OriginDB Performance Monitor");
    spdlog::info("Server: {}", server_address);
    spdlog::info("Interval: {}ms", interval_ms);
    if (duration_seconds > 0) {
        spdlog::info("Duration: {} seconds", duration_seconds);
    } else {
        spdlog::info("Duration: Unlimited (press Ctrl+C to stop)");
    }

    // Create and start monitor
    origindb::performance::PerformanceMonitor monitor(server_address, interval_ms);
    monitor.Start();

    // Handle duration limit
    if (duration_seconds > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
        monitor.Stop();
    } else {
        // Wait for Ctrl+C
        std::signal(SIGINT, [](int signal) {
            spdlog::info("Received interrupt signal, stopping monitor...");
            std::exit(0);
        });

        // Keep running until interrupted
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Print summary and export results
    monitor.PrintSummary();

    if (!output_file.empty()) {
        monitor.ExportResults(output_file);
    }

    spdlog::info("✅ Monitoring session completed");
    return 0;
}