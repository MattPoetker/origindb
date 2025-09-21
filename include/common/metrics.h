#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>

#include "common/config.h"

namespace instantdb {

// Metric types
enum class MetricType {
    COUNTER,
    GAUGE,
    HISTOGRAM
};

// Metric value
struct MetricValue {
    MetricType type;
    double value;
    std::vector<double> histogram_buckets;
    std::unordered_map<std::string, std::string> labels;
};

// Metrics exporter
class MetricsExporter {
public:
    explicit MetricsExporter(const MetricsConfig& config);
    ~MetricsExporter();

    bool Initialize();
    bool Start();
    void Stop();

    // Metric registration
    void RegisterCounter(const std::string& name, const std::string& help);
    void RegisterGauge(const std::string& name, const std::string& help);
    void RegisterHistogram(const std::string& name, const std::string& help,
                          const std::vector<double>& buckets = {});

    // Counter operations
    void IncrementCounter(const std::string& name, double value = 1.0,
                         const std::unordered_map<std::string, std::string>& labels = {});

    // Gauge operations
    void SetGauge(const std::string& name, double value,
                 const std::unordered_map<std::string, std::string>& labels = {});
    void IncrementGauge(const std::string& name, double value = 1.0,
                       const std::unordered_map<std::string, std::string>& labels = {});
    void DecrementGauge(const std::string& name, double value = 1.0,
                       const std::unordered_map<std::string, std::string>& labels = {});

    // Histogram operations
    void ObserveHistogram(const std::string& name, double value,
                         const std::unordered_map<std::string, std::string>& labels = {});

    // Get all metrics (for Prometheus endpoint)
    std::vector<MetricValue> GetMetrics() const;
    std::string GetPrometheusFormat() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenient metric helpers
class ScopedTimer {
public:
    ScopedTimer(MetricsExporter* exporter, const std::string& metric_name,
                const std::unordered_map<std::string, std::string>& labels = {});
    ~ScopedTimer();

private:
    MetricsExporter* exporter_;
    std::string metric_name_;
    std::unordered_map<std::string, std::string> labels_;
    std::chrono::steady_clock::time_point start_;
};

// Global metrics instance
extern std::shared_ptr<MetricsExporter> g_metrics;

} // namespace instantdb