#include "changefeed/changefeed_engine.h"
#include "storage/storage_engine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace origindb {

class ChangefeedEngine::Impl {
public:
    Impl(std::shared_ptr<StorageEngine> storage, const ChangefeedConfig& config)
        : storage_(storage), config_(config), running_(false),
          next_offset_(1), next_subscription_id_(1) {}

    bool Initialize() {
        spdlog::info("Changefeed engine initializing");
        return true;
    }

    bool Start() {
        if (running_) {
            return true;
        }

        running_ = true;

        // Start delivery thread
        delivery_thread_ = std::thread([this]() {
            DeliveryLoop();
        });

        spdlog::info("Changefeed engine started");
        return true;
    }

    void Stop() {
        if (!running_) {
            return;
        }

        running_ = false;

        // Wake up delivery thread
        delivery_cv_.notify_all();

        if (delivery_thread_.joinable()) {
            delivery_thread_.join();
        }

        spdlog::info("Changefeed engine stopped");
    }

    void PublishEvent(const ChangefeedEvent& event) {
        std::lock_guard<std::mutex> lock(events_mutex_);

        ChangefeedEvent timestamped_event = event;
        timestamped_event.offset = next_offset_++;
        timestamped_event.timestamp = std::chrono::system_clock::now();

        events_.push(timestamped_event);
        all_events_.push_back(timestamped_event);

        // Bound the retained history so a high-write workload can't grow it
        // without limit. Trim in batches (drop the oldest 25% once over the
        // cap) so the amortized cost per event stays O(1). Retention-by-time
        // (CompactEvents) still applies on top of this hard ceiling.
        if (config_.max_lag_entries > 0 &&
            all_events_.size() > config_.max_lag_entries) {
            size_t drop = all_events_.size() - (config_.max_lag_entries * 3 / 4);
            all_events_.erase(all_events_.begin(), all_events_.begin() + drop);
        }

        spdlog::debug("Published changefeed event: offset={}, table={}, op={}",
                     timestamped_event.offset, timestamped_event.table,
                     timestamped_event.operation);

        // Notify delivery thread
        delivery_cv_.notify_one();
    }

    void PublishBatch(const std::vector<ChangefeedEvent>& events) {
        for (const auto& event : events) {
            PublishEvent(event);
        }
    }

    std::string CreateSubscription(const SubscriptionFilter& filter,
                                  uint64_t start_offset,
                                  DeliveryMode mode) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        std::string sub_id = "sub-" + std::to_string(next_subscription_id_++);

        Subscription subscription;
        subscription.id = sub_id;
        subscription.filter = filter;
        subscription.current_offset = start_offset;
        subscription.ack_offset = start_offset > 0 ? start_offset - 1 : 0;
        subscription.delivery_mode = mode;
        subscription.is_active = true;
        subscription.created_at = std::chrono::system_clock::now();
        subscription.last_activity = subscription.created_at;

        subscriptions_[sub_id] = subscription;

        spdlog::info("Created subscription {} with filter table={}",
                    sub_id, filter.table_pattern);

        return sub_id;
    }

    bool DeleteSubscription(const std::string& subscription_id) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        auto it = subscriptions_.find(subscription_id);
        if (it != subscriptions_.end()) {
            // Also remove callback
            callbacks_.erase(subscription_id);
            subscriptions_.erase(it);

            spdlog::info("Deleted subscription {}", subscription_id);
            return true;
        }

        return false;
    }

    std::optional<Subscription> GetSubscription(const std::string& subscription_id) const {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        auto it = subscriptions_.find(subscription_id);
        if (it != subscriptions_.end()) {
            return it->second;
        }

        return std::nullopt;
    }

    std::vector<Subscription> ListSubscriptions(bool include_inactive) const {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        std::vector<Subscription> result;
        for (const auto& [id, sub] : subscriptions_) {
            if (include_inactive || sub.is_active) {
                result.push_back(sub);
            }
        }

        return result;
    }

    void Subscribe(const std::string& subscription_id, DeliveryCallback callback) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_[subscription_id] = callback;

        spdlog::debug("Registered callback for subscription {}", subscription_id);

        // Wake up delivery thread to process pending events
        delivery_cv_.notify_one();
    }

    void Unsubscribe(const std::string& subscription_id) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_.erase(subscription_id);

        spdlog::debug("Unregistered callback for subscription {}", subscription_id);
    }

    bool AcknowledgeEvent(const std::string& subscription_id, uint64_t offset) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        auto it = subscriptions_.find(subscription_id);
        if (it != subscriptions_.end()) {
            if (offset > it->second.ack_offset) {
                it->second.ack_offset = offset;
                it->second.last_activity = std::chrono::system_clock::now();

                spdlog::debug("Acknowledged offset {} for subscription {}",
                             offset, subscription_id);
                return true;
            }
        }

        return false;
    }

    void GrantCredits(const std::string& subscription_id, uint32_t credits) {
        // TODO: Implement flow control
        spdlog::debug("Granted {} credits to subscription {}",
                     credits, subscription_id);
    }

    std::vector<ChangefeedEvent> GetEvents(const std::string& subscription_id,
                                          uint64_t start_offset,
                                          size_t limit) const {
        std::lock_guard<std::mutex> lock(events_mutex_);

        std::vector<ChangefeedEvent> result;

        for (const auto& event : all_events_) {
            if (event.offset >= start_offset) {
                if (MatchesFilter(subscription_id, event)) {
                    result.push_back(event);
                    if (result.size() >= limit) {
                        break;
                    }
                }
            }
        }

        return result;
    }

    uint64_t GetLatestOffset() const {
        return next_offset_ - 1;
    }

    ChangefeedEngine::Metrics GetMetrics() const {
        std::lock_guard<std::mutex> events_lock(events_mutex_);
        std::lock_guard<std::mutex> subs_lock(subscriptions_mutex_);

        ChangefeedEngine::Metrics metrics;
        metrics.total_events_published = all_events_.size();
        metrics.active_subscriptions = 0;
        metrics.buffer_size = config_.buffer_size;
        metrics.buffer_used = events_.size();
        metrics.max_lag_ms = 0;

        for (const auto& [id, sub] : subscriptions_) {
            if (sub.is_active) {
                metrics.active_subscriptions++;

                // Calculate lag
                uint64_t lag = GetLatestOffset() - sub.current_offset;
                if (lag > metrics.max_lag_ms) {
                    metrics.max_lag_ms = lag;
                }
            }
        }

        return metrics;
    }

    void CompactEvents(std::chrono::milliseconds retention_period) {
        std::lock_guard<std::mutex> lock(events_mutex_);

        auto cutoff = std::chrono::system_clock::now() - retention_period;

        // Remove old events
        all_events_.erase(
            std::remove_if(all_events_.begin(), all_events_.end(),
                          [cutoff](const ChangefeedEvent& event) {
                              return event.timestamp < cutoff;
                          }),
            all_events_.end());

        spdlog::info("Compacted changefeed events, {} remaining", all_events_.size());
    }

    uint64_t GetOldestRetainedOffset() const {
        std::lock_guard<std::mutex> lock(events_mutex_);

        if (all_events_.empty()) {
            return 0;
        }

        return all_events_.front().offset;
    }

private:
    void DeliveryLoop() {
        spdlog::debug("Changefeed delivery loop started");

        while (running_) {
            std::unique_lock<std::mutex> lock(delivery_mutex_);

            // Wait for events or shutdown
            delivery_cv_.wait(lock, [this]() {
                return !running_ || !events_.empty();
            });

            if (!running_) {
                break;
            }

            // Process pending events
            while (!events_.empty() && running_) {
                ChangefeedEvent event;
                {
                    std::lock_guard<std::mutex> events_lock(events_mutex_);
                    if (events_.empty()) break;

                    event = events_.front();
                    events_.pop();
                }

                DeliverEvent(event);
            }
        }

        spdlog::debug("Changefeed delivery loop stopped");
    }

    void DeliverEvent(const ChangefeedEvent& event) {
        std::vector<std::string> active_subscriptions;

        // Get list of active subscriptions that match this event
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            for (const auto& [id, sub] : subscriptions_) {
                if (sub.is_active && MatchesFilter(id, event)) {
                    active_subscriptions.push_back(id);
                }
            }
        }

        // Deliver to each matching subscription
        for (const std::string& sub_id : active_subscriptions) {
            DeliveryCallback callback;

            {
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                auto it = callbacks_.find(sub_id);
                if (it != callbacks_.end()) {
                    callback = it->second;
                }
            }

            if (callback) {
                try {
                    callback(sub_id, event);

                    // Update subscription offset
                    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                    auto it = subscriptions_.find(sub_id);
                    if (it != subscriptions_.end()) {
                        it->second.current_offset = event.offset;
                        it->second.last_activity = std::chrono::system_clock::now();
                    }

                } catch (const std::exception& e) {
                    spdlog::error("Error delivering event to subscription {}: {}",
                                 sub_id, e.what());
                }
            }
        }
    }

    bool MatchesFilter(const std::string& subscription_id,
                       const ChangefeedEvent& event) const {
        auto it = subscriptions_.find(subscription_id);
        if (it == subscriptions_.end()) {
            return false;
        }

        const auto& filter = it->second.filter;

        // Simple table pattern matching for prototype
        if (!filter.table_pattern.empty() && filter.table_pattern != "*") {
            if (event.table != filter.table_pattern) {
                return false;
            }
        }

        // TODO: Implement SQL WHERE clause filtering

        return true;
    }

private:
    std::shared_ptr<StorageEngine> storage_;
    ChangefeedConfig config_;

    std::atomic<bool> running_;
    std::thread delivery_thread_;

    mutable std::mutex events_mutex_;
    std::queue<ChangefeedEvent> events_;
    std::vector<ChangefeedEvent> all_events_;
    std::atomic<uint64_t> next_offset_;

    mutable std::mutex subscriptions_mutex_;
    std::unordered_map<std::string, Subscription> subscriptions_;
    std::atomic<uint64_t> next_subscription_id_;

    mutable std::mutex callbacks_mutex_;
    std::unordered_map<std::string, DeliveryCallback> callbacks_;

    std::mutex delivery_mutex_;
    std::condition_variable delivery_cv_;
};

ChangefeedEngine::ChangefeedEngine(std::shared_ptr<StorageEngine> storage,
                                   const ChangefeedConfig& config)
    : impl_(std::make_unique<Impl>(storage, config)) {}

ChangefeedEngine::~ChangefeedEngine() = default;

bool ChangefeedEngine::Initialize() {
    return impl_->Initialize();
}

bool ChangefeedEngine::Start() {
    return impl_->Start();
}

void ChangefeedEngine::Stop() {
    impl_->Stop();
}

void ChangefeedEngine::PublishEvent(const ChangefeedEvent& event) {
    impl_->PublishEvent(event);
}

void ChangefeedEngine::PublishBatch(const std::vector<ChangefeedEvent>& events) {
    impl_->PublishBatch(events);
}

std::string ChangefeedEngine::CreateSubscription(const SubscriptionFilter& filter,
                                                 uint64_t start_offset,
                                                 DeliveryMode mode) {
    return impl_->CreateSubscription(filter, start_offset, mode);
}

bool ChangefeedEngine::DeleteSubscription(const std::string& subscription_id) {
    return impl_->DeleteSubscription(subscription_id);
}

std::optional<Subscription> ChangefeedEngine::GetSubscription(
    const std::string& subscription_id) const {
    return impl_->GetSubscription(subscription_id);
}

std::vector<Subscription> ChangefeedEngine::ListSubscriptions(bool include_inactive) const {
    return impl_->ListSubscriptions(include_inactive);
}

void ChangefeedEngine::Subscribe(const std::string& subscription_id,
                                DeliveryCallback callback) {
    impl_->Subscribe(subscription_id, callback);
}

void ChangefeedEngine::Unsubscribe(const std::string& subscription_id) {
    impl_->Unsubscribe(subscription_id);
}

bool ChangefeedEngine::AcknowledgeEvent(const std::string& subscription_id,
                                        uint64_t offset) {
    return impl_->AcknowledgeEvent(subscription_id, offset);
}

void ChangefeedEngine::GrantCredits(const std::string& subscription_id,
                                   uint32_t credits) {
    impl_->GrantCredits(subscription_id, credits);
}

std::vector<ChangefeedEvent> ChangefeedEngine::GetEvents(
    const std::string& subscription_id,
    uint64_t start_offset,
    size_t limit) const {
    return impl_->GetEvents(subscription_id, start_offset, limit);
}

uint64_t ChangefeedEngine::GetLatestOffset() const {
    return impl_->GetLatestOffset();
}

ChangefeedEngine::Metrics ChangefeedEngine::GetMetrics() const {
    return impl_->GetMetrics();
}

void ChangefeedEngine::CompactEvents(std::chrono::milliseconds retention_period) {
    impl_->CompactEvents(retention_period);
}

uint64_t ChangefeedEngine::GetOldestRetainedOffset() const {
    return impl_->GetOldestRetainedOffset();
}

} // namespace origindb