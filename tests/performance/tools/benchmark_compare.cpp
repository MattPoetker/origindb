#include "../framework/performance_test.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace origindb {
namespace performance {

struct BenchmarkResult {
    std::string test_name;
    std::string timestamp;
    uint64_t operations_completed;
    uint64_t operations_failed;
    double duration_seconds;
    double throughput_ops_per_sec;
    double avg_latency_ms;
    double p50_latency_ms;
    double p95_latency_ms;
    double p99_latency_ms;
    double peak_memory_mb;
    double avg_cpu_percent;

    static BenchmarkResult FromJson(const nlohmann::json& j) {
        BenchmarkResult result;
        result.test_name = j.value("test_name", "Unknown");
        result.timestamp = j.value("timestamp", "");
        result.operations_completed = j.value("operations_completed", 0UL);
        result.operations_failed = j.value("operations_failed", 0UL);
        result.duration_seconds = j.value("duration_seconds", 0.0);
        result.throughput_ops_per_sec = j.value("throughput_ops_per_sec", 0.0);
        result.avg_latency_ms = j.value("avg_latency_ms", 0.0);
        result.p50_latency_ms = j.value("p50_latency_ms", 0.0);
        result.p95_latency_ms = j.value("p95_latency_ms", 0.0);
        result.p99_latency_ms = j.value("p99_latency_ms", 0.0);
        result.peak_memory_mb = j.value("peak_memory_mb", 0.0);
        result.avg_cpu_percent = j.value("avg_cpu_percent", 0.0);
        return result;
    }
};

class BenchmarkComparer {
public:
    BenchmarkComparer() = default;

    bool LoadResultsFromDirectory(const std::string& directory) {
        if (!std::filesystem::exists(directory)) {
            spdlog::error("Results directory does not exist: {}", directory);
            return false;
        }

        results_.clear();
        int loaded_count = 0;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.path().extension() == ".json") {
                if (LoadResultFile(entry.path().string())) {
                    loaded_count++;
                }
            }
        }

        spdlog::info("Loaded {} benchmark results from {}", loaded_count, directory);
        return loaded_count > 0;
    }

    bool LoadResultFile(const std::string& filepath) {
        try {
            std::ifstream file(filepath);
            if (!file.is_open()) {
                spdlog::warn("Could not open result file: {}", filepath);
                return false;
            }

            nlohmann::json j;
            file >> j;

            BenchmarkResult result = BenchmarkResult::FromJson(j);
            if (result.test_name != "Unknown") {
                results_.push_back(result);
                return true;
            }

        } catch (const std::exception& e) {
            spdlog::warn("Error loading result file {}: {}", filepath, e.what());
        }

        return false;
    }

    void GenerateTextReport(const std::string& output_file) {
        std::ofstream out(output_file);
        if (!out.is_open()) {
            spdlog::error("Could not open output file: {}", output_file);
            return;
        }

        out << "OriginDB Performance Benchmark Report\n";
        out << "======================================\n\n";
        out << "Generated: " << GetCurrentTimestamp() << "\n";
        out << "Total Results: " << results_.size() << "\n\n";

        // Group results by test type
        std::map<std::string, std::vector<BenchmarkResult>> grouped_results;
        for (const auto& result : results_) {
            grouped_results[result.test_name].push_back(result);
        }

        // Generate summary for each test type
        for (const auto& [test_name, test_results] : grouped_results) {
            GenerateTestSummary(out, test_name, test_results);
            out << "\n";
        }

        // Generate comparison section
        GenerateComparisons(out, grouped_results);

        out.close();
        spdlog::info("Text report generated: {}", output_file);
    }

    void GenerateHTMLReport(const std::string& output_file) {
        std::ofstream out(output_file);
        if (!out.is_open()) {
            spdlog::error("Could not open output file: {}", output_file);
            return;
        }

        // HTML header with embedded CSS and JS
        out << R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>OriginDB Performance Report</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 40px; background: #f5f5f5; }
        .container { background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1, h2, h3 { color: #333; }
        h1 { border-bottom: 3px solid #007acc; padding-bottom: 10px; }
        h2 { color: #0066cc; margin-top: 30px; }
        table { width: 100%; border-collapse: collapse; margin: 20px 0; }
        th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }
        th { background-color: #f8f9fa; font-weight: 600; }
        tr:hover { background-color: #f5f5f5; }
        .metric { display: inline-block; margin: 10px 15px 10px 0; padding: 10px; background: #e3f2fd; border-radius: 4px; }
        .metric-value { font-weight: bold; font-size: 1.2em; color: #1976d2; }
        .metric-label { font-size: 0.9em; color: #666; }
        .good { color: #4caf50; }
        .warning { color: #ff9800; }
        .error { color: #f44336; }
        .chart-placeholder { width: 100%; height: 300px; background: #f0f0f0; border: 1px dashed #ccc;
                           display: flex; align-items: center; justify-content: center; color: #666; margin: 20px 0; }
        .comparison { background: #fff3e0; padding: 15px; border-radius: 4px; margin: 10px 0; }
    </style>
</head>
<body>
    <div class="container">
        <h1>📊 OriginDB Performance Report</h1>
        <p><strong>Generated:</strong> )" << GetCurrentTimestamp() << R"(</p>
        <p><strong>Total Results:</strong> )" << results_.size() << R"(</p>

)";

        // Group results by test type
        std::map<std::string, std::vector<BenchmarkResult>> grouped_results;
        for (const auto& result : results_) {
            grouped_results[result.test_name].push_back(result);
        }

        // Generate overview metrics
        GenerateOverviewHTML(out, grouped_results);

        // Generate detailed results for each test type
        for (const auto& [test_name, test_results] : grouped_results) {
            GenerateTestSummaryHTML(out, test_name, test_results);
        }

        // Generate comparison section
        GenerateComparisonsHTML(out, grouped_results);

        // HTML footer
        out << R"(
    </div>
</body>
</html>
)";

        out.close();
        spdlog::info("HTML report generated: {}", output_file);
    }

    void GenerateCSVExport(const std::string& output_file) {
        std::ofstream out(output_file);
        if (!out.is_open()) {
            spdlog::error("Could not open output file: {}", output_file);
            return;
        }

        // CSV header
        out << "test_name,timestamp,operations_completed,operations_failed,duration_seconds,";
        out << "throughput_ops_per_sec,avg_latency_ms,p50_latency_ms,p95_latency_ms,p99_latency_ms,";
        out << "peak_memory_mb,avg_cpu_percent\n";

        // Data rows
        for (const auto& result : results_) {
            out << result.test_name << ","
                << result.timestamp << ","
                << result.operations_completed << ","
                << result.operations_failed << ","
                << std::fixed << std::setprecision(2) << result.duration_seconds << ","
                << result.throughput_ops_per_sec << ","
                << result.avg_latency_ms << ","
                << result.p50_latency_ms << ","
                << result.p95_latency_ms << ","
                << result.p99_latency_ms << ","
                << result.peak_memory_mb << ","
                << result.avg_cpu_percent << "\n";
        }

        out.close();
        spdlog::info("CSV export generated: {}", output_file);
    }

    void GenerateTopPerformers(const std::string& output_file) {
        std::ofstream out(output_file);
        if (!out.is_open()) {
            spdlog::error("Could not open output file: {}", output_file);
            return;
        }

        out << "OriginDB Top Performers Report\n";
        out << "==============================\n\n";

        // Sort by different metrics and show top 10
        auto results_copy = results_;

        // Top throughput
        std::sort(results_copy.begin(), results_copy.end(),
                 [](const BenchmarkResult& a, const BenchmarkResult& b) {
                     return a.throughput_ops_per_sec > b.throughput_ops_per_sec;
                 });

        out << "🚀 TOP 10 THROUGHPUT (ops/sec):\n";
        for (size_t i = 0; i < std::min(10UL, results_copy.size()); ++i) {
            const auto& result = results_copy[i];
            out << std::setw(2) << (i + 1) << ". " << std::setw(25) << result.test_name
                << " - " << std::fixed << std::setprecision(2) << result.throughput_ops_per_sec << " ops/sec\n";
        }
        out << "\n";

        // Lowest latency
        std::sort(results_copy.begin(), results_copy.end(),
                 [](const BenchmarkResult& a, const BenchmarkResult& b) {
                     return a.avg_latency_ms < b.avg_latency_ms && a.avg_latency_ms > 0;
                 });

        out << "⚡ TOP 10 LOWEST LATENCY (avg ms):\n";
        for (size_t i = 0; i < std::min(10UL, results_copy.size()); ++i) {
            const auto& result = results_copy[i];
            if (result.avg_latency_ms > 0) {
                out << std::setw(2) << (i + 1) << ". " << std::setw(25) << result.test_name
                    << " - " << std::fixed << std::setprecision(3) << result.avg_latency_ms << " ms\n";
            }
        }

        out.close();
        spdlog::info("Top performers report generated: {}", output_file);
    }

private:
    std::vector<BenchmarkResult> results_;

    std::string GetCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    void GenerateTestSummary(std::ofstream& out, const std::string& test_name,
                           const std::vector<BenchmarkResult>& results) {
        out << "Test: " << test_name << "\n";
        out << std::string(test_name.length() + 6, '-') << "\n";

        if (results.empty()) return;

        // Calculate aggregates
        double avg_throughput = 0, avg_latency = 0, avg_memory = 0;
        uint64_t total_ops = 0, total_failed = 0;

        for (const auto& result : results) {
            avg_throughput += result.throughput_ops_per_sec;
            avg_latency += result.avg_latency_ms;
            avg_memory += result.peak_memory_mb;
            total_ops += result.operations_completed;
            total_failed += result.operations_failed;
        }

        avg_throughput /= results.size();
        avg_latency /= results.size();
        avg_memory /= results.size();

        out << "Runs: " << results.size() << "\n";
        out << "Total Operations: " << total_ops << "\n";
        out << "Total Failed: " << total_failed << " ("
            << std::fixed << std::setprecision(2)
            << (total_ops > 0 ? (100.0 * total_failed / total_ops) : 0) << "%)\n";
        out << "Avg Throughput: " << std::fixed << std::setprecision(2) << avg_throughput << " ops/sec\n";
        out << "Avg Latency: " << std::fixed << std::setprecision(3) << avg_latency << " ms\n";
        out << "Avg Memory: " << std::fixed << std::setprecision(1) << avg_memory << " MB\n";

        // Show recent runs
        if (results.size() > 1) {
            out << "\nRecent runs:\n";
            auto sorted_results = results;
            std::sort(sorted_results.begin(), sorted_results.end(),
                     [](const BenchmarkResult& a, const BenchmarkResult& b) {
                         return a.timestamp > b.timestamp;
                     });

            for (size_t i = 0; i < std::min(5UL, sorted_results.size()); ++i) {
                const auto& result = sorted_results[i];
                out << "  " << result.timestamp << " - "
                    << std::fixed << std::setprecision(1) << result.throughput_ops_per_sec << " ops/sec\n";
            }
        }
    }

    void GenerateComparisons(std::ofstream& out,
                           const std::map<std::string, std::vector<BenchmarkResult>>& grouped_results) {
        out << "\nPerformance Comparisons\n";
        out << "======================\n\n";

        // Find best performers in each category
        std::string best_throughput_test;
        double best_throughput = 0;
        std::string best_latency_test;
        double best_latency = std::numeric_limits<double>::max();

        for (const auto& [test_name, results] : grouped_results) {
            if (results.empty()) continue;

            double avg_throughput = 0, avg_latency = 0;
            for (const auto& result : results) {
                avg_throughput += result.throughput_ops_per_sec;
                avg_latency += result.avg_latency_ms;
            }
            avg_throughput /= results.size();
            avg_latency /= results.size();

            if (avg_throughput > best_throughput) {
                best_throughput = avg_throughput;
                best_throughput_test = test_name;
            }

            if (avg_latency < best_latency && avg_latency > 0) {
                best_latency = avg_latency;
                best_latency_test = test_name;
            }
        }

        out << "🏆 Best Throughput: " << best_throughput_test
            << " (" << std::fixed << std::setprecision(2) << best_throughput << " ops/sec)\n";
        out << "⚡ Best Latency: " << best_latency_test
            << " (" << std::fixed << std::setprecision(3) << best_latency << " ms)\n\n";

        // Performance matrix
        out << "Performance Matrix:\n";
        out << std::setw(25) << "Test Name" << std::setw(15) << "Avg Throughput"
            << std::setw(15) << "Avg Latency" << std::setw(12) << "Avg Memory" << "\n";
        out << std::string(67, '-') << "\n";

        for (const auto& [test_name, results] : grouped_results) {
            if (results.empty()) continue;

            double avg_throughput = 0, avg_latency = 0, avg_memory = 0;
            for (const auto& result : results) {
                avg_throughput += result.throughput_ops_per_sec;
                avg_latency += result.avg_latency_ms;
                avg_memory += result.peak_memory_mb;
            }
            avg_throughput /= results.size();
            avg_latency /= results.size();
            avg_memory /= results.size();

            out << std::setw(25) << test_name.substr(0, 24)
                << std::setw(15) << std::fixed << std::setprecision(2) << avg_throughput
                << std::setw(15) << std::fixed << std::setprecision(3) << avg_latency
                << std::setw(12) << std::fixed << std::setprecision(1) << avg_memory << "\n";
        }
    }

    void GenerateOverviewHTML(std::ofstream& out,
                            const std::map<std::string, std::vector<BenchmarkResult>>& grouped_results) {
        out << "<h2>📈 Performance Overview</h2>\n";

        // Calculate overall stats
        uint64_t total_operations = 0;
        uint64_t total_failures = 0;
        double total_throughput = 0;
        int test_count = 0;

        for (const auto& [test_name, results] : grouped_results) {
            for (const auto& result : results) {
                total_operations += result.operations_completed;
                total_failures += result.operations_failed;
                total_throughput += result.throughput_ops_per_sec;
                test_count++;
            }
        }

        double avg_throughput = test_count > 0 ? total_throughput / test_count : 0;
        double success_rate = total_operations > 0 ? (100.0 * (total_operations - total_failures) / total_operations) : 0;

        out << "<div style='display: flex; flex-wrap: wrap; margin: 20px 0;'>\n";
        out << "<div class='metric'><div class='metric-value'>" << grouped_results.size()
            << "</div><div class='metric-label'>Test Types</div></div>\n";
        out << "<div class='metric'><div class='metric-value'>" << test_count
            << "</div><div class='metric-label'>Total Runs</div></div>\n";
        out << "<div class='metric'><div class='metric-value'>" << total_operations
            << "</div><div class='metric-label'>Operations</div></div>\n";
        out << "<div class='metric'><div class='metric-value "
            << (success_rate > 95 ? "good" : success_rate > 90 ? "warning" : "error") << "'>"
            << std::fixed << std::setprecision(1) << success_rate << "%</div><div class='metric-label'>Success Rate</div></div>\n";
        out << "<div class='metric'><div class='metric-value'>"
            << std::fixed << std::setprecision(1) << avg_throughput
            << "</div><div class='metric-label'>Avg Throughput (ops/sec)</div></div>\n";
        out << "</div>\n";
    }

    void GenerateTestSummaryHTML(std::ofstream& out, const std::string& test_name,
                               const std::vector<BenchmarkResult>& results) {
        out << "<h2>" << test_name << "</h2>\n";

        if (results.empty()) {
            out << "<p>No results available for this test.</p>\n";
            return;
        }

        // Statistics table
        out << "<table>\n<thead>\n<tr>\n";
        out << "<th>Timestamp</th><th>Operations</th><th>Failed</th><th>Throughput (ops/sec)</th>";
        out << "<th>Avg Latency (ms)</th><th>P99 Latency (ms)</th><th>Memory (MB)</th>\n";
        out << "</tr>\n</thead>\n<tbody>\n";

        for (const auto& result : results) {
            double failure_rate = result.operations_completed > 0 ?
                (100.0 * result.operations_failed / result.operations_completed) : 0;

            out << "<tr>\n";
            out << "<td>" << result.timestamp << "</td>";
            out << "<td>" << result.operations_completed << "</td>";
            out << "<td class='" << (failure_rate > 5 ? "error" : failure_rate > 1 ? "warning" : "good")
                << "'>" << result.operations_failed << " (" << std::fixed << std::setprecision(1)
                << failure_rate << "%)</td>";
            out << "<td>" << std::fixed << std::setprecision(2) << result.throughput_ops_per_sec << "</td>";
            out << "<td>" << std::fixed << std::setprecision(3) << result.avg_latency_ms << "</td>";
            out << "<td>" << std::fixed << std::setprecision(3) << result.p99_latency_ms << "</td>";
            out << "<td>" << std::fixed << std::setprecision(1) << result.peak_memory_mb << "</td>";
            out << "\n</tr>\n";
        }

        out << "</tbody>\n</table>\n";

        // Chart placeholder
        out << "<div class='chart-placeholder'>Performance Chart (requires Chart.js integration)</div>\n";
    }

    void GenerateComparisonsHTML(std::ofstream& out,
                               const std::map<std::string, std::vector<BenchmarkResult>>& grouped_results) {
        out << "<h2>🏆 Performance Comparisons</h2>\n";

        // Find best performers
        std::vector<std::pair<std::string, double>> throughput_rankings;
        std::vector<std::pair<std::string, double>> latency_rankings;

        for (const auto& [test_name, results] : grouped_results) {
            if (results.empty()) continue;

            double avg_throughput = 0, avg_latency = 0;
            for (const auto& result : results) {
                avg_throughput += result.throughput_ops_per_sec;
                avg_latency += result.avg_latency_ms;
            }
            avg_throughput /= results.size();
            avg_latency /= results.size();

            throughput_rankings.emplace_back(test_name, avg_throughput);
            if (avg_latency > 0) {
                latency_rankings.emplace_back(test_name, avg_latency);
            }
        }

        // Sort rankings
        std::sort(throughput_rankings.begin(), throughput_rankings.end(),
                 [](const auto& a, const auto& b) { return a.second > b.second; });
        std::sort(latency_rankings.begin(), latency_rankings.end(),
                 [](const auto& a, const auto& b) { return a.second < b.second; });

        out << "<div style='display: flex; justify-content: space-between;'>\n";

        // Throughput rankings
        out << "<div style='flex: 1; margin-right: 20px;'>\n";
        out << "<h3>🚀 Throughput Leaders</h3>\n";
        for (size_t i = 0; i < std::min(5UL, throughput_rankings.size()); ++i) {
            const auto& [name, throughput] = throughput_rankings[i];
            out << "<div class='comparison'>";
            out << "<strong>" << (i + 1) << ". " << name << "</strong><br>";
            out << std::fixed << std::setprecision(2) << throughput << " ops/sec";
            out << "</div>\n";
        }
        out << "</div>\n";

        // Latency rankings
        out << "<div style='flex: 1;'>\n";
        out << "<h3>⚡ Latency Champions</h3>\n";
        for (size_t i = 0; i < std::min(5UL, latency_rankings.size()); ++i) {
            const auto& [name, latency] = latency_rankings[i];
            out << "<div class='comparison'>";
            out << "<strong>" << (i + 1) << ". " << name << "</strong><br>";
            out << std::fixed << std::setprecision(3) << latency << " ms avg";
            out << "</div>\n";
        }
        out << "</div>\n";

        out << "</div>\n";
    }
};

} // namespace performance
} // namespace origindb

void PrintUsage() {
    std::cout << R"(
OriginDB Benchmark Comparison Tool

Usage: benchmark_compare [OPTIONS]

Options:
  --input <DIR>          Directory containing JSON result files (required)
  --output <FILE>        Output file name (default: performance_report.html)
  --format <FORMAT>      Output format: html, text, csv, top (default: html)
  --help                 Show this help message

Formats:
  html    - Interactive HTML report with charts and styling
  text    - Plain text summary report
  csv     - CSV export for further analysis
  top     - Top performers summary

Examples:
  # Generate HTML report from results directory
  benchmark_compare --input ./reports --output performance.html

  # Generate text summary
  benchmark_compare --input ./reports --format text --output summary.txt

  # Export to CSV for analysis
  benchmark_compare --input ./reports --format csv --output results.csv

  # Show top performers
  benchmark_compare --input ./reports --format top --output top_performers.txt

)";
}

int main(int argc, char* argv[]) {
    std::string input_dir;
    std::string output_file = "performance_report.html";
    std::string format = "html";
    bool help = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            help = true;
        } else if (arg == "--input" && i + 1 < argc) {
            input_dir = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            format = argv[++i];
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

    if (input_dir.empty()) {
        std::cerr << "Error: --input directory is required" << std::endl;
        PrintUsage();
        return 1;
    }

    // Configure logging
    spdlog::set_level(spdlog::level::info);

    spdlog::info("📊 OriginDB Benchmark Comparison Tool");
    spdlog::info("Input Directory: {}", input_dir);
    spdlog::info("Output File: {}", output_file);
    spdlog::info("Format: {}", format);

    // Load and process results
    origindb::performance::BenchmarkComparer comparer;

    if (!comparer.LoadResultsFromDirectory(input_dir)) {
        spdlog::error("Failed to load results from directory: {}", input_dir);
        return 1;
    }

    // Generate report based on format
    try {
        if (format == "html") {
            comparer.GenerateHTMLReport(output_file);
        } else if (format == "text") {
            comparer.GenerateTextReport(output_file);
        } else if (format == "csv") {
            comparer.GenerateCSVExport(output_file);
        } else if (format == "top") {
            comparer.GenerateTopPerformers(output_file);
        } else {
            spdlog::error("Unknown format: {}", format);
            PrintUsage();
            return 1;
        }

        spdlog::info("✅ Report generation completed successfully!");
        return 0;

    } catch (const std::exception& e) {
        spdlog::error("Error generating report: {}", e.what());
        return 1;
    }
}