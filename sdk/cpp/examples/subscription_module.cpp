#include "../instantdb.hpp"
#include <string>
#include <nlohmann/json.hpp>

using namespace instantdb;

/**
 * Subscription Module Example
 *
 * This example demonstrates:
 * - Implementing subscription filter functions
 * - Transforming subscription events
 * - Providing initial data for subscriptions
 * - Client-specific event emission
 */

// =============================================================================
// Data Structures
// =============================================================================

struct UserActivity {
    std::string user_id;
    std::string action;
    uint64_t timestamp;
    std::string details;

    static std::vector<uint8_t> serialize(const UserActivity& activity) {
        nlohmann::json j;
        j["user_id"] = activity.user_id;
        j["action"] = activity.action;
        j["timestamp"] = activity.timestamp;
        j["details"] = activity.details;

        std::string json_str = j.dump();
        return std::vector<uint8_t>(json_str.begin(), json_str.end());
    }

    static Result<UserActivity> deserialize(const std::vector<uint8_t>& data) {
        try {
            std::string json_str(data.begin(), data.end());
            nlohmann::json j = nlohmann::json::parse(json_str);

            UserActivity activity;
            activity.user_id = j["user_id"];
            activity.action = j["action"];
            activity.timestamp = j["timestamp"];
            activity.details = j["details"];

            return activity;
        } catch (const std::exception& e) {
            return Error("Failed to deserialize UserActivity: " + std::string(e.what()));
        }
    }
};

// Specialize serialization for UserActivity
namespace instantdb::serialization {

template<>
class Serializable<UserActivity> {
public:
    static std::vector<uint8_t> serialize(const UserActivity& value) {
        return UserActivity::serialize(value);
    }

    static Result<UserActivity> deserialize(const std::vector<uint8_t>& data) {
        return UserActivity::deserialize(data);
    }
};

} // namespace instantdb::serialization

// =============================================================================
// Subscription Functions
// =============================================================================

/**
 * Filter function for user activity subscriptions
 * Only include activities from the last hour
 */
INSTANTDB_EXPORT bool filter_activity_by_time(const std::vector<uint8_t>& event_data) {
    try {
        // Parse the changefeed event
        std::string json_str(event_data.begin(), event_data.end());
        nlohmann::json event = nlohmann::json::parse(json_str);

        // Extract timestamp from the event
        if (!event.contains("new_value")) {
            return false; // Skip DELETE events
        }

        // Parse the activity data
        std::string activity_json = event["new_value"];
        nlohmann::json activity = nlohmann::json::parse(activity_json);

        uint64_t activity_time = activity["timestamp"];
        uint64_t current_time = utils::now();
        uint64_t one_hour_ms = 60 * 60 * 1000;

        // Only include activities from the last hour
        return (current_time - activity_time) <= one_hour_ms;

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Filter error: " + std::string(e.what()));
        return false;
    }
}

/**
 * Filter function for specific user activities
 * Only include activities for users with premium accounts
 */
INSTANTDB_EXPORT bool filter_premium_users(const std::vector<uint8_t>& event_data) {
    try {
        std::string json_str(event_data.begin(), event_data.end());
        nlohmann::json event = nlohmann::json::parse(json_str);

        if (!event.contains("new_value")) {
            return false;
        }

        std::string activity_json = event["new_value"];
        nlohmann::json activity = nlohmann::json::parse(activity_json);

        std::string user_id = activity["user_id"];

        // Check if user has premium account
        auto user_result = db::read<nlohmann::json>("users", user_id);
        if (user_result.is_err() || !user_result.unwrap().has_value()) {
            return false;
        }

        auto user = user_result.unwrap().value();
        return user.contains("premium") && user["premium"].get<bool>();

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Premium filter error: " + std::string(e.what()));
        return false;
    }
}

/**
 * Transform function to add computed fields
 */
INSTANTDB_EXPORT std::vector<uint8_t> transform_add_metadata(const std::vector<uint8_t>& event_data) {
    try {
        std::string json_str(event_data.begin(), event_data.end());
        nlohmann::json event = nlohmann::json::parse(json_str);

        if (event.contains("new_value")) {
            std::string activity_json = event["new_value"];
            nlohmann::json activity = nlohmann::json::parse(activity_json);

            // Add computed metadata
            activity["processed_at"] = utils::now();
            activity["server_version"] = "1.0.0";

            // Add user reputation score
            std::string user_id = activity["user_id"];
            auto user_result = db::read<nlohmann::json>("users", user_id);
            if (user_result.is_ok() && user_result.unwrap().has_value()) {
                auto user = user_result.unwrap().value();
                if (user.contains("reputation")) {
                    activity["user_reputation"] = user["reputation"];
                }
            }

            // Update the event with transformed data
            event["new_value"] = activity.dump();
        }

        std::string result_json = event.dump();
        return std::vector<uint8_t>(result_json.begin(), result_json.end());

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Transform error: " + std::string(e.what()));
        return event_data; // Return original on error
    }
}

/**
 * Transform function to anonymize sensitive data
 */
INSTANTDB_EXPORT std::vector<uint8_t> transform_anonymize(const std::vector<uint8_t>& event_data) {
    try {
        std::string json_str(event_data.begin(), event_data.end());
        nlohmann::json event = nlohmann::json::parse(json_str);

        if (event.contains("new_value")) {
            std::string activity_json = event["new_value"];
            nlohmann::json activity = nlohmann::json::parse(activity_json);

            // Replace user_id with anonymous hash
            if (activity.contains("user_id")) {
                std::string user_id = activity["user_id"];
                activity["user_id"] = "anon_" + std::to_string(std::hash<std::string>{}(user_id) % 100000);
            }

            // Remove potentially sensitive details
            if (activity.contains("details")) {
                activity["details"] = "[REDACTED]";
            }

            event["new_value"] = activity.dump();
        }

        std::string result_json = event.dump();
        return std::vector<uint8_t>(result_json.begin(), result_json.end());

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Anonymize error: " + std::string(e.what()));
        return event_data;
    }
}

/**
 * Get initial data for activity subscriptions
 */
INSTANTDB_EXPORT std::vector<uint8_t> get_initial_activities(const std::string& where_clause) {
    try {
        nlohmann::json initial_data = nlohmann::json::array();

        // For demo purposes, create some sample activities
        // In a real implementation, you'd query the database based on the where_clause

        nlohmann::json activity1;
        activity1["user_id"] = "user_123";
        activity1["action"] = "login";
        activity1["timestamp"] = utils::now() - 30000; // 30 seconds ago
        activity1["details"] = "User logged in from mobile app";

        nlohmann::json activity2;
        activity2["user_id"] = "user_456";
        activity2["action"] = "purchase";
        activity2["timestamp"] = utils::now() - 60000; // 1 minute ago
        activity2["details"] = "Purchased premium subscription";

        initial_data.push_back(activity1);
        initial_data.push_back(activity2);

        std::string result_json = initial_data.dump();
        return std::vector<uint8_t>(result_json.begin(), result_json.end());

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Initial data error: " + std::string(e.what()));
        return {};
    }
}

// =============================================================================
// Reducer Functions
// =============================================================================

/**
 * Create a new user activity record
 */
INSTANTDB_EXPORT int32_t create_activity(const char* user_id, const char* action, const char* details) {
    try {
        UserActivity activity{
            .user_id = std::string(user_id),
            .action = std::string(action),
            .timestamp = utils::now(),
            .details = std::string(details)
        };

        std::string activity_id = std::string(user_id) + "_" + std::to_string(activity.timestamp);

        auto result = db::write("user_activities", activity_id, activity);
        if (result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to create activity: " + result.error().message());
            return -1;
        }

        // Emit real-time event
        auto emit_result = events::emit("activity_created", activity_id, activity);
        if (emit_result.is_err()) {
            utils::log(utils::LogLevel::Warn, "Failed to emit activity event: " + emit_result.error().message());
        }

        utils::log(utils::LogLevel::Info, "Created activity: " + activity_id + " (" + action + ")");
        return 1;

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Exception in create_activity: " + std::string(e.what()));
        return -1;
    }
}

/**
 * Get user activities with filtering
 */
INSTANTDB_EXPORT int32_t get_user_activities(const char* user_id, int64_t since_timestamp) {
    try {
        utils::log(utils::LogLevel::Info,
                  "Getting activities for user: " + std::string(user_id) +
                  " since: " + std::to_string(since_timestamp));

        // In a real implementation, you'd scan the activities table
        // and filter by user_id and timestamp
        // For now, just return success
        return 1;

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Exception in get_user_activities: " + std::string(e.what()));
        return -1;
    }
}

// =============================================================================
// Module Lifecycle
// =============================================================================

INSTANTDB_INIT() {
    utils::log(utils::LogLevel::Info, "Subscription module initialized");

    // Create some sample users for testing
    nlohmann::json user1;
    user1["id"] = "user_123";
    user1["name"] = "Alice";
    user1["premium"] = true;
    user1["reputation"] = 95;

    nlohmann::json user2;
    user2["id"] = "user_456";
    user2["name"] = "Bob";
    user2["premium"] = false;
    user2["reputation"] = 72;

    db::write("users", "user_123", user1);
    db::write("users", "user_456", user2);

    return 0;
}

INSTANTDB_CLIENT_CONNECTED() {
    utils::log(utils::LogLevel::Info, "Client connected to subscription module");
    return 0;
}

INSTANTDB_CLIENT_DISCONNECTED() {
    utils::log(utils::LogLevel::Info, "Client disconnected from subscription module");
    return 0;
}