#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <memory>
#include <optional>
#include <nlohmann/json_fwd.hpp>
#include "changefeed/changefeed_engine.h"

namespace origindb {

class PredicateEvaluator;

// SQL subscription query types
enum class SubscriptionType {
    SELECT_ALL,      // SELECT * FROM table
    SELECT_COLUMNS,  // SELECT col1, col2 FROM table
    ALL_TABLES      // Special: subscribe to all tables
};

// Parsed SQL subscription
struct SqlSubscription {
    std::string id;
    std::string client_id;
    SubscriptionType type;
    std::string table_name;                    // Empty for ALL_TABLES
    std::vector<std::string> columns;          // Empty for SELECT_ALL
    std::optional<std::string> where_clause;   // Optional WHERE clause
    std::shared_ptr<PredicateEvaluator> predicate;  // Compiled WHERE clause (null if none)
    bool is_active;

    // Check if this subscription matches an event
    bool MatchesEvent(const ChangefeedEvent& event) const;

    // Fast path: caller supplies pre-parsed row column objects (nullptr when
    // absent/unparseable) so one event parse is shared across subscriptions.
    bool MatchesEvent(const ChangefeedEvent& event,
                      const nlohmann::json* new_columns,
                      const nlohmann::json* old_columns) const;

    // Filter event data based on column selection
    ChangefeedEvent FilterEvent(const ChangefeedEvent& event) const;
};

// SQL subscription parser
class SqlSubscriptionParser {
public:
    // Parse SQL subscription query
    // Supports:
    //   SELECT * FROM table_name
    //   SELECT col1, col2, col3 FROM table_name
    //   SELECT * FROM table_name WHERE condition
    //   SELECT * FROM ALL_TABLES (special syntax for catch-all)
    static std::unique_ptr<SqlSubscription> Parse(
        const std::string& query,
        const std::string& subscription_id,
        const std::string& client_id);

private:
    // Helper methods for parsing
    static std::vector<std::string> ParseColumns(const std::string& select_clause);
    static std::string ExtractTableName(const std::string& from_clause);
    static std::optional<std::string> ExtractWhereClause(const std::string& query);
    static std::string Trim(const std::string& str);
    static std::string ToLower(const std::string& str);
    static std::vector<std::string> Split(const std::string& str, char delimiter);
};

// Enhanced subscription manager with SQL support
class SqlSubscriptionManager {
public:
    SqlSubscriptionManager();
    ~SqlSubscriptionManager();

    // Subscribe with SQL query
    std::string Subscribe(const std::string& client_id, const std::string& sql_query);

    // Subscribe to all tables (cleaner API for catch-all subscriptions)
    std::string SubscribeToAllTables(const std::string& client_id);

    // Unsubscribe
    void Unsubscribe(const std::string& subscription_id);

    // Unsubscribe all for a client
    void UnsubscribeAll(const std::string& client_id);

    // Process changefeed event and return filtered events per client
    std::vector<std::pair<std::string, ChangefeedEvent>> ProcessEvent(const ChangefeedEvent& event);

    // Get all subscriptions for a client
    std::vector<SqlSubscription> GetClientSubscriptions(const std::string& client_id) const;

    // Get subscription statistics
    struct Stats {
        size_t total_subscriptions;
        size_t active_subscriptions;
        size_t catch_all_subscriptions;
        std::unordered_map<std::string, size_t> subscriptions_per_table;
    };
    Stats GetStats() const;

private:
    mutable std::mutex subscriptions_mutex_;
    std::unordered_map<std::string, std::unique_ptr<SqlSubscription>> subscriptions_;
    std::unordered_map<std::string, std::vector<std::string>> client_subscriptions_;  // client_id -> subscription_ids
    // Routing index: table -> subscription_ids, plus catch-all subscriptions.
    // ProcessEvent only evaluates candidates for the event's table.
    std::unordered_map<std::string, std::unordered_set<std::string>> table_index_;
    std::unordered_set<std::string> all_tables_index_;
    std::atomic<uint64_t> next_subscription_id_;

    std::string GenerateSubscriptionId();
    void IndexSubscription(const SqlSubscription& subscription);
    void UnindexSubscription(const SqlSubscription& subscription);
};

} // namespace origindb