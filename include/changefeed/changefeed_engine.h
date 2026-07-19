#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <optional>

#include "common/config.h"

namespace origindb {

class StorageEngine;

// Delivery modes
enum class DeliveryMode {
    AT_LEAST_ONCE = 0,
    EXACTLY_ONCE = 1
};

// Changefeed event
struct ChangefeedEvent {
    uint64_t offset;
    std::string transaction_id;
    std::string table;
    std::string operation;  // INSERT, UPDATE, DELETE
    std::vector<uint8_t> key;
    std::vector<uint8_t> old_value;
    std::vector<uint8_t> new_value;
    std::chrono::system_clock::time_point timestamp;
};

// Subscription filter
struct SubscriptionFilter {
    std::string table_pattern;  // Regex or glob pattern
    std::string sql_where_clause;  // SQL WHERE clause filter
    std::vector<std::string> columns;  // Column projection
};

// Subscription state
struct Subscription {
    std::string id;
    SubscriptionFilter filter;
    uint64_t current_offset;
    uint64_t ack_offset;
    DeliveryMode delivery_mode;
    bool is_active;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_activity;
};

// Delivery callback
using DeliveryCallback = std::function<void(const std::string& subscription_id,
                                           const ChangefeedEvent& event)>;

// Changefeed engine for managing event streams
class ChangefeedEngine {
public:
    ChangefeedEngine(std::shared_ptr<StorageEngine> storage,
                     const ChangefeedConfig& config);
    ~ChangefeedEngine();

    bool Initialize();
    bool Start();
    void Stop();

    // Event publishing (called by transaction commit)
    void PublishEvent(const ChangefeedEvent& event);
    void PublishBatch(const std::vector<ChangefeedEvent>& events);

    // Subscription management
    std::string CreateSubscription(const SubscriptionFilter& filter,
                                  uint64_t start_offset = 0,
                                  DeliveryMode mode = DeliveryMode::AT_LEAST_ONCE);

    bool DeleteSubscription(const std::string& subscription_id);

    std::optional<Subscription> GetSubscription(const std::string& subscription_id) const;
    std::vector<Subscription> ListSubscriptions(bool include_inactive = false) const;

    // Event delivery
    void Subscribe(const std::string& subscription_id,
                  DeliveryCallback callback);

    void Unsubscribe(const std::string& subscription_id);

    // Acknowledgement and flow control
    bool AcknowledgeEvent(const std::string& subscription_id,
                         uint64_t offset);

    void GrantCredits(const std::string& subscription_id,
                     uint32_t credits);

    // Event retrieval
    std::vector<ChangefeedEvent> GetEvents(const std::string& subscription_id,
                                          uint64_t start_offset,
                                          size_t limit = 100) const;

    uint64_t GetLatestOffset() const;

    // Metrics
    struct Metrics {
        uint64_t total_events_published;
        uint64_t total_events_delivered;
        uint64_t active_subscriptions;
        uint64_t max_lag_ms;
        uint64_t buffer_size;
        uint64_t buffer_used;
    };
    Metrics GetMetrics() const;

    // Retention management
    void CompactEvents(std::chrono::milliseconds retention_period);
    uint64_t GetOldestRetainedOffset() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace origindb