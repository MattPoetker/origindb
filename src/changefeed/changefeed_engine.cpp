#include "changefeed/changefeed_engine.h"
#include "storage/storage_engine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>

namespace origindb {

// Fixed-size worker pool used to deliver one changefeed event to many
// subscribers concurrently. RunBatch is a barrier: it enqueues `count` tasks
// and blocks until every one has finished, which preserves per-subscriber
// ordering (event K is fully delivered before the delivery loop starts K+1).
class DeliveryPool {
public:
    explicit DeliveryPool(size_t threads) {
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { Worker(); });
        }
    }

    ~DeliveryPool() { Shutdown(); }

    size_t size() const { return workers_.size(); }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
    }

    // Run fn(i) for i in [0, count) across the pool; return once all complete.
    void RunBatch(size_t count, const std::function<void(size_t)>& fn) {
        if (count == 0) return;

        std::atomic<size_t> remaining{count};
        std::mutex done_mutex;
        std::condition_variable done_cv;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (size_t i = 0; i < count; ++i) {
                tasks_.push([&, i] {
                    fn(i);  // fn itself is noexcept-guarded by the caller
                    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::lock_guard<std::mutex> dl(done_mutex);
                        done_cv.notify_one();
                    }
                });
            }
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> dl(done_mutex);
        done_cv.wait(dl, [&] { return remaining.load(std::memory_order_acquire) == 0; });
    }

private:
    void Worker() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

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

        // Spin up the fan-out worker pool. 0 = auto: leave a core for the
        // delivery loop + the rest of the server, cap at 8. 1 = sequential.
        size_t threads = config_.delivery_threads;
        if (threads == 0) {
            unsigned hw = std::thread::hardware_concurrency();
            threads = (hw > 2) ? std::min<unsigned>(hw - 1, 8u) : 1;
        }
        if (threads > 1) {
            pool_ = std::make_unique<DeliveryPool>(threads);
            spdlog::info("Changefeed fan-out pool: {} worker threads", threads);
        } else {
            spdlog::info("Changefeed fan-out: sequential (1 thread)");
        }

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

        // Delivery loop is done — no more RunBatch calls — so the pool is safe
        // to tear down now.
        if (pool_) {
            pool_->Shutdown();
            pool_.reset();
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
            // Also remove callback (same subscriptions_ -> callbacks_ lock order
            // the delivery path uses, so parallel fan-out can't race this).
            {
                std::lock_guard<std::mutex> cbs_lock(callbacks_mutex_);
                callbacks_.erase(subscription_id);
            }
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
        metrics.total_events_delivered = delivered_.load(std::memory_order_relaxed);
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
        // Phase 1 (locked): snapshot the matching subscribers and their
        // callbacks. Lock order is subscriptions_ -> callbacks_ everywhere.
        struct Target {
            std::string id;
            DeliveryCallback callback;
        };
        std::vector<Target> targets;
        {
            std::lock_guard<std::mutex> subs_lock(subscriptions_mutex_);
            std::lock_guard<std::mutex> cbs_lock(callbacks_mutex_);
            targets.reserve(subscriptions_.size());
            for (const auto& [id, sub] : subscriptions_) {
                if (sub.is_active && MatchesFilter(id, event)) {
                    auto it = callbacks_.find(id);
                    if (it != callbacks_.end()) {
                        targets.push_back({id, it->second});
                    }
                }
            }
        }

        if (targets.empty()) {
            return;
        }

        // Phase 2 (unlocked, parallelizable): the expensive part — predicate
        // eval + frame build + socket send happens inside each callback with no
        // engine lock held, so subscribers deliver independently.
        auto deliver_one = [this, &targets, &event](size_t i) {
            const Target& t = targets[i];
            try {
                t.callback(t.id, event);

                std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                auto it = subscriptions_.find(t.id);
                if (it != subscriptions_.end()) {
                    it->second.current_offset = event.offset;
                    it->second.last_activity = std::chrono::system_clock::now();
                }
            } catch (const std::exception& e) {
                spdlog::error("Error delivering event to subscription {}: {}",
                             t.id, e.what());
            }
            delivered_.fetch_add(1, std::memory_order_relaxed);
        };

        if (pool_ && targets.size() > 1) {
            // Fan out this event's subscribers across the pool; RunBatch is a
            // barrier so ordering across events is preserved.
            pool_->RunBatch(targets.size(), deliver_one);
        } else {
            for (size_t i = 0; i < targets.size(); ++i) {
                deliver_one(i);
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

    std::unique_ptr<DeliveryPool> pool_;
    std::atomic<uint64_t> delivered_{0};
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