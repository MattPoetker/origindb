#include "changefeed/sql_subscription.h"
#include "changefeed/predicate_evaluator.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <regex>
#include <nlohmann/json.hpp>

namespace origindb {

namespace {

// Parses a serialized row value (shaped {"key":...,"columns":{...}}) and
// extracts the flat column object. Falls back to the whole object if no
// "columns" field is present. Returns false on JSON parse failure.
bool ExtractRowColumns(const std::vector<uint8_t>& value, nlohmann::json& out) {
    try {
        std::string json_str(value.begin(), value.end());
        auto parsed = nlohmann::json::parse(json_str);
        if (parsed.is_object() && parsed.contains("columns") && parsed["columns"].is_object()) {
            out = parsed["columns"];
        } else {
            out = parsed;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

// ==================== SqlSubscription Methods ====================

bool SqlSubscription::MatchesEvent(const ChangefeedEvent& event) const {
    // Standalone entry point: parse the row payloads for this one call.
    // ProcessEvent uses the pre-parsed overload to share the parse across
    // every candidate subscription.
    nlohmann::json new_columns, old_columns;
    const nlohmann::json* new_ptr = nullptr;
    const nlohmann::json* old_ptr = nullptr;
    if (!event.new_value.empty() && ExtractRowColumns(event.new_value, new_columns))
        new_ptr = &new_columns;
    if (!event.old_value.empty() && ExtractRowColumns(event.old_value, old_columns))
        old_ptr = &old_columns;
    return MatchesEvent(event, new_ptr, old_ptr);
}

bool SqlSubscription::MatchesEvent(const ChangefeedEvent& event,
                                   const nlohmann::json* new_columns,
                                   const nlohmann::json* old_columns) const {
    // If subscription is not active, don't match
    if (!is_active) {
        return false;
    }

    // Handle ALL_TABLES subscription
    if (type == SubscriptionType::ALL_TABLES) {
        return true;  // Match all events
    }

    // Check table name
    if (!table_name.empty() && event.table != table_name) {
        return false;
    }

    // Evaluate WHERE clause predicate if present
    if (predicate) {
        if (event.operation == "DELETE") {
            // DELETE events carry only the old row
            if (!old_columns) {
                spdlog::debug("Subscription {}: DELETE event on table {} has no usable "
                              "old_value, cannot evaluate WHERE clause",
                              id, event.table);
                return false;
            }
            return predicate->Evaluate(*old_columns);
        }

        if (event.operation == "UPDATE") {
            // Match if either the new or the old row satisfies the predicate
            if (new_columns && predicate->Evaluate(*new_columns)) return true;
            if (old_columns && predicate->Evaluate(*old_columns)) return true;
            return false;
        }

        // INSERT: evaluate the new row
        if (!new_columns) {
            spdlog::warn("Subscription {}: unparseable new_value for {} event on table {}",
                         id, event.operation, event.table);
            return false;
        }
        return predicate->Evaluate(*new_columns);
    }

    return true;
}

ChangefeedEvent SqlSubscription::FilterEvent(const ChangefeedEvent& event) const {
    // If SELECT * or ALL_TABLES, return the full event
    if (type == SubscriptionType::SELECT_ALL || type == SubscriptionType::ALL_TABLES) {
        return event;
    }

    // For SELECT_COLUMNS, filter the event data
    // This requires parsing the event value and extracting only selected columns
    ChangefeedEvent filtered = event;

    // If columns are specified, we need to filter the new_value and old_value
    if (!columns.empty() && type == SubscriptionType::SELECT_COLUMNS) {
        // Parse the values as JSON and extract only specified columns
        try {
            // Filter new_value
            if (!event.new_value.empty()) {
                std::string json_str(event.new_value.begin(), event.new_value.end());
                auto json_obj = nlohmann::json::parse(json_str);

                nlohmann::json filtered_obj;
                for (const auto& col : columns) {
                    if (json_obj.contains(col)) {
                        filtered_obj[col] = json_obj[col];
                    }
                }

                std::string filtered_str = filtered_obj.dump();
                filtered.new_value = std::vector<uint8_t>(filtered_str.begin(), filtered_str.end());
            }

            // Filter old_value
            if (!event.old_value.empty()) {
                std::string json_str(event.old_value.begin(), event.old_value.end());
                auto json_obj = nlohmann::json::parse(json_str);

                nlohmann::json filtered_obj;
                for (const auto& col : columns) {
                    if (json_obj.contains(col)) {
                        filtered_obj[col] = json_obj[col];
                    }
                }

                std::string filtered_str = filtered_obj.dump();
                filtered.old_value = std::vector<uint8_t>(filtered_str.begin(), filtered_str.end());
            }
        } catch (const std::exception& e) {
            spdlog::warn("Failed to filter event columns: {}", e.what());
            // Return unfiltered event on error
            return event;
        }
    }

    return filtered;
}

// ==================== SqlSubscriptionParser Methods ====================

std::unique_ptr<SqlSubscription> SqlSubscriptionParser::Parse(
    const std::string& query,
    const std::string& subscription_id,
    const std::string& client_id) {

    auto subscription = std::make_unique<SqlSubscription>();
    subscription->id = subscription_id;
    subscription->client_id = client_id;
    subscription->is_active = true;

    // Trim and lowercase for easier parsing
    std::string normalized_query = Trim(query);
    std::string lower_query = ToLower(normalized_query);

    // Note: ALL_TABLES subscriptions should now use the SubscribeToAllTables() API method
    // instead of SQL syntax for cleaner code

    // Basic SQL parsing using regex
    // Pattern: SELECT (columns) FROM table_name [WHERE condition]
    std::regex select_pattern(R"(select\s+(.*?)\s+from\s+(\w+)(?:\s+where\s+(.*))?)",
                              std::regex_constants::icase);
    std::smatch matches;

    if (std::regex_match(normalized_query, matches, select_pattern)) {
        std::string select_clause = Trim(matches[1].str());
        std::string table_name = Trim(matches[2].str());
        std::string where_clause = matches[3].matched ? Trim(matches[3].str()) : "";

        subscription->table_name = table_name;

        // Parse columns
        if (select_clause == "*") {
            subscription->type = SubscriptionType::SELECT_ALL;
        } else {
            subscription->type = SubscriptionType::SELECT_COLUMNS;
            subscription->columns = ParseColumns(select_clause);
        }

        // Set WHERE clause if present and compile it into a predicate
        if (!where_clause.empty()) {
            subscription->where_clause = where_clause;

            std::string predicate_error;
            auto predicate = PredicateEvaluator::Parse(where_clause, predicate_error);
            if (!predicate) {
                spdlog::error("Failed to parse WHERE clause '{}' for subscription {}: {}",
                             where_clause, subscription_id, predicate_error);
                return nullptr;
            }
            subscription->predicate = std::move(predicate);
        }

        spdlog::info("Parsed SQL subscription: {} - SELECT {} FROM {} {}",
                    subscription_id,
                    select_clause,
                    table_name,
                    where_clause.empty() ? "" : "WHERE " + where_clause);

        return subscription;
    }

    // If parsing fails, return nullptr
    spdlog::error("Failed to parse SQL subscription query: {}", query);
    return nullptr;
}

std::vector<std::string> SqlSubscriptionParser::ParseColumns(const std::string& select_clause) {
    return Split(select_clause, ',');
}

std::string SqlSubscriptionParser::ExtractTableName(const std::string& from_clause) {
    // Simple extraction - assumes single table
    return Trim(from_clause);
}

std::optional<std::string> SqlSubscriptionParser::ExtractWhereClause(const std::string& query) {
    std::string lower = ToLower(query);
    size_t where_pos = lower.find(" where ");
    if (where_pos != std::string::npos) {
        return query.substr(where_pos + 7);  // Skip " where "
    }
    return std::nullopt;
}

std::string SqlSubscriptionParser::Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

std::string SqlSubscriptionParser::ToLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::vector<std::string> SqlSubscriptionParser::Split(const std::string& str, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;

    while (std::getline(ss, item, delimiter)) {
        std::string trimmed = Trim(item);
        if (!trimmed.empty()) {
            result.push_back(trimmed);
        }
    }

    return result;
}

// ==================== SqlSubscriptionManager Methods ====================

SqlSubscriptionManager::SqlSubscriptionManager()
    : next_subscription_id_(1) {
}

SqlSubscriptionManager::~SqlSubscriptionManager() {
}

std::string SqlSubscriptionManager::Subscribe(const std::string& client_id, const std::string& sql_query) {
    std::string subscription_id = GenerateSubscriptionId();

    auto subscription = SqlSubscriptionParser::Parse(sql_query, subscription_id, client_id);
    if (!subscription) {
        throw std::invalid_argument("Invalid SQL subscription query: " + sql_query);
    }

    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    IndexSubscription(*subscription);

    // Store subscription
    subscriptions_[subscription_id] = std::move(subscription);

    // Track client subscriptions
    client_subscriptions_[client_id].push_back(subscription_id);

    spdlog::info("Created SQL subscription {} for client {}: {}",
                subscription_id, client_id, sql_query);

    return subscription_id;
}

std::string SqlSubscriptionManager::SubscribeToAllTables(const std::string& client_id) {
    std::string subscription_id = GenerateSubscriptionId();

    auto subscription = std::make_unique<SqlSubscription>();
    subscription->id = subscription_id;
    subscription->client_id = client_id;
    subscription->type = SubscriptionType::ALL_TABLES;
    subscription->is_active = true;
    // table_name remains empty for ALL_TABLES
    // columns remains empty for ALL_TABLES

    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    IndexSubscription(*subscription);

    // Store subscription
    subscriptions_[subscription_id] = std::move(subscription);

    // Track client subscriptions
    client_subscriptions_[client_id].push_back(subscription_id);

    spdlog::info("Created ALL_TABLES subscription {} for client {}", subscription_id, client_id);

    return subscription_id;
}

void SqlSubscriptionManager::Unsubscribe(const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    auto it = subscriptions_.find(subscription_id);
    if (it != subscriptions_.end()) {
        std::string client_id = it->second->client_id;
        UnindexSubscription(*it->second);
        subscriptions_.erase(it);

        // Remove from client subscriptions
        auto& client_subs = client_subscriptions_[client_id];
        client_subs.erase(
            std::remove(client_subs.begin(), client_subs.end(), subscription_id),
            client_subs.end()
        );

        if (client_subs.empty()) {
            client_subscriptions_.erase(client_id);
        }

        spdlog::info("Removed SQL subscription: {}", subscription_id);
    }
}

void SqlSubscriptionManager::UnsubscribeAll(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    auto it = client_subscriptions_.find(client_id);
    if (it != client_subscriptions_.end()) {
        for (const auto& sub_id : it->second) {
            auto sub_it = subscriptions_.find(sub_id);
            if (sub_it != subscriptions_.end()) {
                UnindexSubscription(*sub_it->second);
                subscriptions_.erase(sub_it);
            }
        }
        client_subscriptions_.erase(it);

        spdlog::info("Removed all SQL subscriptions for client: {}", client_id);
    }
}

std::vector<std::pair<std::string, ChangefeedEvent>>
SqlSubscriptionManager::ProcessEvent(const ChangefeedEvent& event) {
    std::vector<std::pair<std::string, ChangefeedEvent>> results;

    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    // Parse the event's row payloads once; every candidate shares the result.
    nlohmann::json new_columns, old_columns;
    const nlohmann::json* new_ptr = nullptr;
    const nlohmann::json* old_ptr = nullptr;
    if (!event.new_value.empty() && ExtractRowColumns(event.new_value, new_columns))
        new_ptr = &new_columns;
    if (!event.old_value.empty() && ExtractRowColumns(event.old_value, old_columns))
        old_ptr = &old_columns;

    // Only subscriptions on this event's table (plus catch-alls) are
    // candidates; the per-table index avoids scanning every subscription.
    auto evaluate = [&](const std::string& sub_id) {
        auto it = subscriptions_.find(sub_id);
        if (it == subscriptions_.end()) return;
        const auto& subscription = it->second;
        if (subscription->MatchesEvent(event, new_ptr, old_ptr)) {
            results.emplace_back(subscription->client_id,
                                 subscription->FilterEvent(event));
        }
    };

    auto table_it = table_index_.find(event.table);
    if (table_it != table_index_.end()) {
        for (const auto& sub_id : table_it->second) evaluate(sub_id);
    }
    for (const auto& sub_id : all_tables_index_) evaluate(sub_id);

    return results;
}

void SqlSubscriptionManager::IndexSubscription(const SqlSubscription& subscription) {
    if (subscription.type == SubscriptionType::ALL_TABLES) {
        all_tables_index_.insert(subscription.id);
    } else {
        table_index_[subscription.table_name].insert(subscription.id);
    }
}

void SqlSubscriptionManager::UnindexSubscription(const SqlSubscription& subscription) {
    if (subscription.type == SubscriptionType::ALL_TABLES) {
        all_tables_index_.erase(subscription.id);
    } else {
        auto it = table_index_.find(subscription.table_name);
        if (it != table_index_.end()) {
            it->second.erase(subscription.id);
            if (it->second.empty()) table_index_.erase(it);
        }
    }
}

std::vector<SqlSubscription>
SqlSubscriptionManager::GetClientSubscriptions(const std::string& client_id) const {
    std::vector<SqlSubscription> results;

    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    auto it = client_subscriptions_.find(client_id);
    if (it != client_subscriptions_.end()) {
        for (const auto& sub_id : it->second) {
            auto sub_it = subscriptions_.find(sub_id);
            if (sub_it != subscriptions_.end()) {
                results.push_back(*sub_it->second);
            }
        }
    }

    return results;
}

SqlSubscriptionManager::Stats SqlSubscriptionManager::GetStats() const {
    Stats stats{};

    std::lock_guard<std::mutex> lock(subscriptions_mutex_);

    stats.total_subscriptions = subscriptions_.size();

    for (const auto& [id, sub] : subscriptions_) {
        if (sub->is_active) {
            stats.active_subscriptions++;
        }

        if (sub->type == SubscriptionType::ALL_TABLES) {
            stats.catch_all_subscriptions++;
        } else if (!sub->table_name.empty()) {
            stats.subscriptions_per_table[sub->table_name]++;
        }
    }

    return stats;
}

std::string SqlSubscriptionManager::GenerateSubscriptionId() {
    uint64_t id = next_subscription_id_.fetch_add(1);
    return "sql_sub_" + std::to_string(id);
}

} // namespace origindb