#include "wasm/wasm_subscription.h"
#include "storage/storage_engine.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>
#include <thread>
#include <algorithm>

namespace origindb {

// =============================================================================
// WasmSubscriptionManager Implementation
// =============================================================================

class WasmSubscriptionManager::Impl {
public:
    Impl(std::shared_ptr<WasmEngine> wasm_engine,
         std::shared_ptr<ChangefeedEngine> changefeed_engine,
         std::shared_ptr<StorageEngine> storage_engine)
        : wasm_engine_(std::move(wasm_engine))
        , changefeed_engine_(std::move(changefeed_engine))
        , storage_engine_(std::move(storage_engine))
        , next_subscription_id_(1)
        , is_running_(false) {
    }

    bool Initialize() {
        spdlog::info("Initializing WASM subscription manager");

        // Subscribe to all changefeed events
        changefeed_subscription_id_ = changefeed_engine_->CreateSubscription(
            {
                .table_pattern = "*",  // Subscribe to all tables
                .sql_where_clause = "",
                .columns = {}
            },
            0,  // Start from beginning
            DeliveryMode::AT_LEAST_ONCE
        );

        // Set up changefeed callback
        changefeed_engine_->Subscribe(changefeed_subscription_id_,
            [this](const std::string& sub_id, const ChangefeedEvent& event) {
                this->ProcessChangefeedEvent(event);
            }
        );

        return true;
    }

    bool Start() {
        if (is_running_) return true;

        spdlog::info("Starting WASM subscription manager");
        is_running_ = true;

        // Start processing thread
        processing_thread_ = std::thread([this]() {
            ProcessingLoop();
        });

        return true;
    }

    void Stop() {
        if (!is_running_) return;

        spdlog::info("Stopping WASM subscription manager");
        is_running_ = false;

        // Unsubscribe from changefeed
        if (!changefeed_subscription_id_.empty()) {
            changefeed_engine_->Unsubscribe(changefeed_subscription_id_);
            changefeed_engine_->DeleteSubscription(changefeed_subscription_id_);
        }

        // Stop processing thread
        event_cv_.notify_all();
        if (processing_thread_.joinable()) {
            processing_thread_.join();
        }

        // Clear all subscriptions
        {
            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            subscriptions_.clear();
            client_subscriptions_.clear();
        }
    }

    std::string CreateSubscription(const WasmSubscriptionQuery& query) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        // Generate subscription ID
        std::string sub_id = "wasm_sub_" + std::to_string(next_subscription_id_++);

        // Store subscription
        auto stored_query = query;
        stored_query.id = sub_id;
        stored_query.created_at = std::chrono::system_clock::now();

        subscriptions_[sub_id] = stored_query;

        // Track by client
        if (!query.client_id.empty()) {
            client_subscriptions_[query.client_id].push_back(sub_id);
        }

        // Send initial data if requested
        if (query.include_initial_data) {
            SendInitialData(stored_query);
        }

        // Update metrics
        metrics_.total_subscriptions++;
        metrics_.active_subscriptions++;

        spdlog::info("Created WASM subscription: {} for client: {} module: {}",
                    sub_id, query.client_id, query.module_name);

        return sub_id;
    }

    bool DeleteSubscription(const std::string& subscription_id) {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        auto it = subscriptions_.find(subscription_id);
        if (it == subscriptions_.end()) {
            return false;
        }

        // Remove from client tracking
        const auto& client_id = it->second.client_id;
        if (!client_id.empty()) {
            auto& client_subs = client_subscriptions_[client_id];
            client_subs.erase(
                std::remove(client_subs.begin(), client_subs.end(), subscription_id),
                client_subs.end()
            );

            if (client_subs.empty()) {
                client_subscriptions_.erase(client_id);
            }
        }

        subscriptions_.erase(it);
        metrics_.active_subscriptions--;

        spdlog::info("Deleted WASM subscription: {}", subscription_id);
        return true;
    }

    std::optional<WasmSubscriptionQuery> GetSubscription(const std::string& subscription_id) const {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        auto it = subscriptions_.find(subscription_id);
        if (it != subscriptions_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<WasmSubscriptionQuery> ListSubscriptions(const std::string& client_id) const {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        std::vector<WasmSubscriptionQuery> result;

        if (client_id.empty()) {
            // Return all subscriptions
            for (const auto& [id, query] : subscriptions_) {
                result.push_back(query);
            }
        } else {
            // Return subscriptions for specific client
            auto it = client_subscriptions_.find(client_id);
            if (it != client_subscriptions_.end()) {
                for (const auto& sub_id : it->second) {
                    auto sub_it = subscriptions_.find(sub_id);
                    if (sub_it != subscriptions_.end()) {
                        result.push_back(sub_it->second);
                    }
                }
            }
        }

        return result;
    }

    void SetSubscriptionCallback(SubscriptionCallback callback) {
        subscription_callback_ = std::move(callback);
    }

    void ProcessChangefeedEvent(const ChangefeedEvent& event) {
        // Queue event for processing
        {
            std::lock_guard<std::mutex> lock(event_queue_mutex_);
            event_queue_.push_back(event);
        }
        event_cv_.notify_one();

        metrics_.events_processed++;
    }

    void OnClientConnected(const std::string& client_id) {
        spdlog::info("WASM subscription client connected: {}", client_id);

        // Notify WASM modules about client connection
        // This could trigger module-specific initialization
        auto modules = wasm_engine_->ListModules();
        for (const auto& name : modules) {
            ReducerContext ctx;
            ctx.sender_identity = client_id;
            ctx.storage = storage_engine_;
            ctx.changefeed = changefeed_engine_;

            wasm_engine_->ExecuteReducer(
                name,
                "__client_connected",
                ctx,
                {WasmValue(client_id)}
            );
        }
    }

    void OnClientDisconnected(const std::string& client_id) {
        spdlog::info("WASM subscription client disconnected: {}", client_id);

        // Remove all subscriptions for this client
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        auto it = client_subscriptions_.find(client_id);
        if (it != client_subscriptions_.end()) {
            for (const auto& sub_id : it->second) {
                subscriptions_.erase(sub_id);
                metrics_.active_subscriptions--;
            }
            client_subscriptions_.erase(it);
        }

        // Notify WASM modules about client disconnection
        auto modules = wasm_engine_->ListModules();
        for (const auto& name : modules) {
            ReducerContext ctx;
            ctx.sender_identity = client_id;
            ctx.storage = storage_engine_;
            ctx.changefeed = changefeed_engine_;

            wasm_engine_->ExecuteReducer(
                name,
                "__client_disconnected",
                ctx,
                {WasmValue(client_id)}
            );
        }
    }

    WasmQueryResult ExecuteQuery(
        const std::string& module_name,
        const std::string& query_function,
        const std::unordered_map<std::string, WasmValue>& parameters) {

        WasmQueryResult result;

        // Prepare reducer context
        ReducerContext ctx;
        ctx.storage = storage_engine_;
        ctx.changefeed = changefeed_engine_;

        // Convert parameters to WasmValue vector
        std::vector<WasmValue> args;
        for (const auto& [key, value] : parameters) {
            args.push_back(value);
        }

        // Execute query function in WASM module
        auto wasm_result = wasm_engine_->ExecuteReducer(
            module_name,
            query_function,
            ctx,
            args
        );

        if (wasm_result.success) {
            // Parse result as array of values
            for (const auto& value : wasm_result.values) {
                result.results.push_back(value);
            }
        } else {
            result.error = wasm_result.error;
        }

        return result;
    }

    Metrics GetMetrics() const {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        return metrics_;
    }

private:
    void ProcessingLoop() {
        while (is_running_) {
            std::vector<ChangefeedEvent> events_to_process;

            // Wait for events
            {
                std::unique_lock<std::mutex> lock(event_queue_mutex_);
                event_cv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this] { return !event_queue_.empty() || !is_running_; });

                if (!is_running_) break;

                events_to_process = std::move(event_queue_);
                event_queue_.clear();
            }

            // Process each event
            for (const auto& event : events_to_process) {
                ProcessSingleEvent(event);
            }
        }
    }

    void ProcessSingleEvent(const ChangefeedEvent& event) {
        auto start_time = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(subscriptions_mutex_);

        for (const auto& [sub_id, query] : subscriptions_) {
            // Check if subscription matches table
            if (query.query_spec.has_value()) {
                const auto& tables = query.query_spec->tables;
                if (!tables.empty() &&
                    std::find(tables.begin(), tables.end(), event.table) == tables.end()) {
                    continue;
                }
            }

            // Prepare event data for WASM processing
            WasmValue event_data = ConvertEventToWasmValue(event);

            // Execute filter function
            bool should_emit = true;
            if (!query.filter_function.empty()) {
                ReducerContext ctx;
                ctx.storage = storage_engine_;
                ctx.changefeed = changefeed_engine_;

                auto filter_result = wasm_engine_->ExecuteReducer(
                    query.module_name,
                    query.filter_function,
                    ctx,
                    {event_data}
                );

                if (filter_result.success && !filter_result.values.empty() &&
                    std::holds_alternative<int32_t>(filter_result.values[0])) {
                    // ABI: origindb_invoke returns 1 = include, 0 = exclude.
                    should_emit = std::get<int32_t>(filter_result.values[0]) != 0;
                    if (!should_emit) {
                        metrics_.events_filtered++;
                    }
                } else if (filter_result.success && !filter_result.values.empty() &&
                           std::holds_alternative<bool>(filter_result.values[0])) {
                    should_emit = std::get<bool>(filter_result.values[0]);
                    if (!should_emit) {
                        metrics_.events_filtered++;
                    }
                } else {
                    spdlog::warn("Filter function failed for subscription {}: {}",
                                sub_id, filter_result.error);
                    metrics_.wasm_errors++;
                    continue;
                }
            }

            if (!should_emit) continue;

            // Execute transform function
            WasmValue final_data = event_data;
            if (!query.transform_function.empty()) {
                ReducerContext ctx;
                ctx.storage = storage_engine_;
                ctx.changefeed = changefeed_engine_;

                auto transform_result = wasm_engine_->ExecuteReducer(
                    query.module_name,
                    query.transform_function,
                    ctx,
                    {event_data}
                );

                if (transform_result.success && !transform_result.values.empty()) {
                    if (std::holds_alternative<std::vector<uint8_t>>(transform_result.values[0])) {
                        final_data = std::get<std::vector<uint8_t>>(transform_result.values[0]);
                    }
                    metrics_.events_transformed++;
                } else {
                    spdlog::warn("Transform function failed for subscription {}: {}",
                                sub_id, transform_result.error);
                    metrics_.wasm_errors++;
                    continue;
                }
            }

            // Emit to client
            if (subscription_callback_ && !query.client_id.empty()) {
                subscription_callback_(sub_id, query.client_id, final_data);
                metrics_.events_emitted++;
            }
        }

        // Update processing time metric
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Simple moving average for processing time
        if (metrics_.avg_processing_time.count() == 0) {
            metrics_.avg_processing_time = duration;
        } else {
            metrics_.avg_processing_time =
                (metrics_.avg_processing_time * 9 + duration) / 10;
        }
    }

    void SendInitialData(const WasmSubscriptionQuery& query) {
        if (!query.query_spec.has_value()) return;

        // Execute initial data query through WASM
        ReducerContext ctx;
        ctx.storage = storage_engine_;
        ctx.changefeed = changefeed_engine_;

        // Build initial data query
        std::vector<WasmValue> args;
        args.push_back(WasmValue(query.query_spec->where_clause));

        auto result = wasm_engine_->ExecuteReducer(
            query.module_name,
            "__get_initial_data",  // Special function for initial data
            ctx,
            args
        );

        if (result.success && subscription_callback_ && !query.client_id.empty() && !result.values.empty()) {
            if (std::holds_alternative<std::vector<uint8_t>>(result.values[0])) {
                subscription_callback_(query.id, query.client_id, std::get<std::vector<uint8_t>>(result.values[0]));
            }
        }
    }

    WasmValue ConvertEventToWasmValue(const ChangefeedEvent& event) {
        // Serialize the full event as a JSON string for the module's
        // filter/transform functions. Values are UTF-8 JSON where possible;
        // non-JSON bytes fall back to raw strings.
        nlohmann::json obj;
        obj["table"] = event.table;
        obj["operation"] = event.operation;
        obj["offset"] = event.offset;
        obj["transaction_id"] = event.transaction_id;
        obj["key"] = std::string(event.key.begin(), event.key.end());

        auto embed = [&obj](const char* field, const std::vector<uint8_t>& bytes) {
            if (bytes.empty()) {
                obj[field] = nullptr;
                return;
            }
            auto parsed = nlohmann::json::parse(bytes.begin(), bytes.end(),
                                                nullptr, false);
            if (!parsed.is_discarded())
                obj[field] = std::move(parsed);
            else
                obj[field] = std::string(bytes.begin(), bytes.end());
        };
        embed("new_value", event.new_value);
        embed("old_value", event.old_value);

        return WasmValue(obj.dump());
    }

private:
    std::shared_ptr<WasmEngine> wasm_engine_;
    std::shared_ptr<ChangefeedEngine> changefeed_engine_;
    std::shared_ptr<StorageEngine> storage_engine_;

    mutable std::mutex subscriptions_mutex_;
    std::unordered_map<std::string, WasmSubscriptionQuery> subscriptions_;
    std::unordered_map<std::string, std::vector<std::string>> client_subscriptions_;

    std::string changefeed_subscription_id_;
    uint64_t next_subscription_id_;

    std::mutex event_queue_mutex_;
    std::condition_variable event_cv_;
    std::vector<ChangefeedEvent> event_queue_;

    std::thread processing_thread_;
    std::atomic<bool> is_running_;

    SubscriptionCallback subscription_callback_;

    mutable Metrics metrics_{};
};

// =============================================================================
// WasmSubscriptionManager Public Interface
// =============================================================================

WasmSubscriptionManager::WasmSubscriptionManager(
    std::shared_ptr<WasmEngine> wasm_engine,
    std::shared_ptr<ChangefeedEngine> changefeed_engine,
    std::shared_ptr<StorageEngine> storage_engine)
    : impl_(std::make_unique<Impl>(
        std::move(wasm_engine),
        std::move(changefeed_engine),
        std::move(storage_engine))) {
}

WasmSubscriptionManager::~WasmSubscriptionManager() = default;

bool WasmSubscriptionManager::Initialize() {
    return impl_->Initialize();
}

bool WasmSubscriptionManager::Start() {
    return impl_->Start();
}

void WasmSubscriptionManager::Stop() {
    impl_->Stop();
}

std::string WasmSubscriptionManager::CreateSubscription(const WasmSubscriptionQuery& query) {
    return impl_->CreateSubscription(query);
}

bool WasmSubscriptionManager::DeleteSubscription(const std::string& subscription_id) {
    return impl_->DeleteSubscription(subscription_id);
}

std::optional<WasmSubscriptionQuery> WasmSubscriptionManager::GetSubscription(
    const std::string& subscription_id) const {
    return impl_->GetSubscription(subscription_id);
}

std::vector<WasmSubscriptionQuery> WasmSubscriptionManager::ListSubscriptions(
    const std::string& client_id) const {
    return impl_->ListSubscriptions(client_id);
}

void WasmSubscriptionManager::SetSubscriptionCallback(SubscriptionCallback callback) {
    impl_->SetSubscriptionCallback(std::move(callback));
}

void WasmSubscriptionManager::ProcessChangefeedEvent(const ChangefeedEvent& event) {
    impl_->ProcessChangefeedEvent(event);
}

void WasmSubscriptionManager::OnClientConnected(const std::string& client_id) {
    impl_->OnClientConnected(client_id);
}

void WasmSubscriptionManager::OnClientDisconnected(const std::string& client_id) {
    impl_->OnClientDisconnected(client_id);
}

WasmSubscriptionManager::WasmQueryResult WasmSubscriptionManager::ExecuteQuery(
    const std::string& module_name,
    const std::string& query_function,
    const std::unordered_map<std::string, WasmValue>& parameters) {
    return impl_->ExecuteQuery(module_name, query_function, parameters);
}

WasmSubscriptionManager::Metrics WasmSubscriptionManager::GetMetrics() const {
    return impl_->GetMetrics();
}

// =============================================================================
// SubscriptionQueryBuilder Implementation
// =============================================================================

SubscriptionQueryBuilder& SubscriptionQueryBuilder::WithModule(const std::string& module_name) {
    query_.module_name = module_name;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::WithFilter(const std::string& filter_function) {
    query_.filter_function = filter_function;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::WithTransform(const std::string& transform_function) {
    query_.transform_function = transform_function;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::FromTable(const std::string& table) {
    if (!query_.query_spec.has_value()) {
        query_.query_spec = WasmSubscriptionQuery::QuerySpec{};
    }
    query_.query_spec->tables = {table};
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::FromTables(const std::vector<std::string>& tables) {
    if (!query_.query_spec.has_value()) {
        query_.query_spec = WasmSubscriptionQuery::QuerySpec{};
    }
    query_.query_spec->tables = tables;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::Where(const std::string& where_clause) {
    if (!query_.query_spec.has_value()) {
        query_.query_spec = WasmSubscriptionQuery::QuerySpec{};
    }
    query_.query_spec->where_clause = where_clause;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::Select(const std::vector<std::string>& columns) {
    if (!query_.query_spec.has_value()) {
        query_.query_spec = WasmSubscriptionQuery::QuerySpec{};
    }
    query_.query_spec->columns = columns;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::WithParameter(
    const std::string& name, const WasmValue& value) {
    if (!query_.query_spec.has_value()) {
        query_.query_spec = WasmSubscriptionQuery::QuerySpec{};
    }
    query_.query_spec->parameters[name] = value;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::ForClient(const std::string& client_id) {
    query_.client_id = client_id;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::StartingAt(uint64_t offset) {
    query_.start_offset = offset;
    return *this;
}

SubscriptionQueryBuilder& SubscriptionQueryBuilder::IncludeInitialData(bool include) {
    query_.include_initial_data = include;
    return *this;
}

WasmSubscriptionQuery SubscriptionQueryBuilder::Build() const {
    return query_;
}

} // namespace origindb