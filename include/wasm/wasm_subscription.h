#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <variant>
#include <optional>
#include <unordered_map>

#include "wasm/wasm_engine.h"
#include "changefeed/changefeed_engine.h"

namespace origindb {

// Forward declarations
class StorageEngine;

// =============================================================================
// Subscription Query Types
// =============================================================================

/**
 * Represents a subscription query that can be processed by WASM modules
 */
struct WasmSubscriptionQuery {
    std::string id;
    std::string module_name;        // WASM module that processes this subscription
    std::string filter_function;    // WASM function to filter events
    std::string transform_function; // WASM function to transform events (optional)

    // Base SQL-like query (optional)
    struct QuerySpec {
        std::vector<std::string> tables;     // Tables to subscribe to
        std::string where_clause;             // SQL WHERE clause
        std::vector<std::string> columns;     // Column projection
        std::unordered_map<std::string, WasmValue> parameters; // Query parameters
    };
    std::optional<QuerySpec> query_spec;

    // Subscription metadata
    std::string client_id;              // WebSocket client ID
    uint64_t start_offset;              // Starting offset for replay
    bool include_initial_data;         // Send current state on subscribe
    std::chrono::system_clock::time_point created_at;
};

/**
 * Result of WASM subscription processing
 */
struct WasmSubscriptionResult {
    bool should_emit;                  // Whether to send this event to the client
    std::optional<WasmValue> transformed_data; // Transformed event data (if any)
    std::optional<std::string> error;  // Error message if processing failed
};

// =============================================================================
// WASM Subscription Manager
// =============================================================================

/**
 * Manages subscriptions that are processed by WASM modules
 */
class WasmSubscriptionManager {
public:
    WasmSubscriptionManager(
        std::shared_ptr<WasmEngine> wasm_engine,
        std::shared_ptr<ChangefeedEngine> changefeed_engine,
        std::shared_ptr<StorageEngine> storage_engine
    );
    ~WasmSubscriptionManager();

    // Initialization
    bool Initialize();
    bool Start();
    void Stop();

    // Subscription management
    std::string CreateSubscription(const WasmSubscriptionQuery& query);
    bool DeleteSubscription(const std::string& subscription_id);
    std::optional<WasmSubscriptionQuery> GetSubscription(const std::string& subscription_id) const;
    std::vector<WasmSubscriptionQuery> ListSubscriptions(const std::string& client_id = "") const;

    // Event processing callback for WebSocket delivery
    using SubscriptionCallback = std::function<void(
        const std::string& subscription_id,
        const std::string& client_id,
        const WasmValue& data
    )>;

    void SetSubscriptionCallback(SubscriptionCallback callback);

    // Process changefeed events through WASM filters
    void ProcessChangefeedEvent(const ChangefeedEvent& event);

    // Client management
    void OnClientConnected(const std::string& client_id);
    void OnClientDisconnected(const std::string& client_id);

    // Subscription queries from WASM modules
    struct WasmQueryResult {
        std::vector<WasmValue> results;
        std::optional<std::string> error;
    };

    WasmQueryResult ExecuteQuery(
        const std::string& module_name,
        const std::string& query_function,
        const std::unordered_map<std::string, WasmValue>& parameters
    );

    // Metrics
    struct Metrics {
        uint64_t total_subscriptions;
        uint64_t active_subscriptions;
        uint64_t events_processed;
        uint64_t events_filtered;
        uint64_t events_transformed;
        uint64_t events_emitted;
        uint64_t wasm_errors;
        std::chrono::milliseconds avg_processing_time;
    };
    Metrics GetMetrics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// =============================================================================
// WASM Subscription API (exposed to WASM modules)
// =============================================================================

/**
 * Host functions exposed to WASM modules for subscription operations
 */
class WasmSubscriptionApi {
public:
    // Filter function signature: (event_data: bytes) -> bool
    static bool FilterEvent(
        const std::vector<uint8_t>& event_data,
        const std::string& filter_expression
    );

    // Transform function signature: (event_data: bytes) -> bytes
    static std::vector<uint8_t> TransformEvent(
        const std::vector<uint8_t>& event_data,
        const std::string& transform_expression
    );

    // Query function signature: (query: string, params: map) -> array
    static std::vector<WasmValue> ExecuteSubscriptionQuery(
        const std::string& query,
        const std::unordered_map<std::string, WasmValue>& parameters
    );

    // Emit subscription event to specific client
    static void EmitToClient(
        const std::string& client_id,
        const std::string& event_type,
        const WasmValue& data
    );

    // Batch query for initial data
    static std::vector<WasmValue> GetInitialData(
        const std::string& table,
        const std::string& where_clause,
        const std::vector<std::string>& columns
    );
};

// =============================================================================
// Subscription Query Builder (Helper)
// =============================================================================

/**
 * Fluent API for building subscription queries
 */
class SubscriptionQueryBuilder {
public:
    SubscriptionQueryBuilder& WithModule(const std::string& module_name);
    SubscriptionQueryBuilder& WithFilter(const std::string& filter_function);
    SubscriptionQueryBuilder& WithTransform(const std::string& transform_function);
    SubscriptionQueryBuilder& FromTable(const std::string& table);
    SubscriptionQueryBuilder& FromTables(const std::vector<std::string>& tables);
    SubscriptionQueryBuilder& Where(const std::string& where_clause);
    SubscriptionQueryBuilder& Select(const std::vector<std::string>& columns);
    SubscriptionQueryBuilder& WithParameter(const std::string& name, const WasmValue& value);
    SubscriptionQueryBuilder& ForClient(const std::string& client_id);
    SubscriptionQueryBuilder& StartingAt(uint64_t offset);
    SubscriptionQueryBuilder& IncludeInitialData(bool include = true);

    WasmSubscriptionQuery Build() const;

private:
    WasmSubscriptionQuery query_;
};

} // namespace origindb