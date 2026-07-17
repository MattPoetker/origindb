#include "../origindb.hpp"
#include <string>

using namespace origindb;

/**
 * Counter Module Example
 *
 * This example demonstrates:
 * - Defining reducers
 * - Reading and writing to database tables
 * - Emitting events for real-time subscriptions
 * - Error handling
 */

// =============================================================================
// Data Structures
// =============================================================================

struct Counter {
    std::string id;
    int64_t value;
    uint64_t last_updated;

    // Serialization support
    static std::vector<uint8_t> serialize(const Counter& counter) {
        // Simple binary serialization (in production, use protobuf or similar)
        std::vector<uint8_t> result;

        // Serialize id length and data
        uint32_t id_len = static_cast<uint32_t>(counter.id.length());
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&id_len),
                     reinterpret_cast<const uint8_t*>(&id_len) + 4);
        result.insert(result.end(), counter.id.begin(), counter.id.end());

        // Serialize value and timestamp
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&counter.value),
                     reinterpret_cast<const uint8_t*>(&counter.value) + 8);
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&counter.last_updated),
                     reinterpret_cast<const uint8_t*>(&counter.last_updated) + 8);

        return result;
    }

    static Result<Counter> deserialize(const std::vector<uint8_t>& data) {
        if (data.size() < 4) {
            return Error("Invalid counter data: too short");
        }

        size_t offset = 0;

        // Deserialize id
        uint32_t id_len;
        std::memcpy(&id_len, data.data() + offset, 4);
        offset += 4;

        if (offset + id_len + 16 > data.size()) {
            return Error("Invalid counter data: corrupted");
        }

        std::string id(reinterpret_cast<const char*>(data.data() + offset), id_len);
        offset += id_len;

        // Deserialize value and timestamp
        int64_t value;
        uint64_t last_updated;
        std::memcpy(&value, data.data() + offset, 8);
        offset += 8;
        std::memcpy(&last_updated, data.data() + offset, 8);

        return Counter{id, value, last_updated};
    }
};

struct CounterEvent {
    std::string counter_id;
    int64_t old_value;
    int64_t new_value;
    uint64_t timestamp;

    static std::vector<uint8_t> serialize(const CounterEvent& event) {
        std::vector<uint8_t> result;

        // Serialize counter_id
        uint32_t id_len = static_cast<uint32_t>(event.counter_id.length());
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&id_len),
                     reinterpret_cast<const uint8_t*>(&id_len) + 4);
        result.insert(result.end(), event.counter_id.begin(), event.counter_id.end());

        // Serialize values
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&event.old_value),
                     reinterpret_cast<const uint8_t*>(&event.old_value) + 8);
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&event.new_value),
                     reinterpret_cast<const uint8_t*>(&event.new_value) + 8);
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&event.timestamp),
                     reinterpret_cast<const uint8_t*>(&event.timestamp) + 8);

        return result;
    }
};

// Specialize serialization for our types
namespace origindb::serialization {

template<>
class Serializable<Counter> {
public:
    static std::vector<uint8_t> serialize(const Counter& value) {
        return Counter::serialize(value);
    }

    static Result<Counter> deserialize(const std::vector<uint8_t>& data) {
        return Counter::deserialize(data);
    }
};

template<>
class Serializable<CounterEvent> {
public:
    static std::vector<uint8_t> serialize(const CounterEvent& value) {
        return CounterEvent::serialize(value);
    }

    static Result<CounterEvent> deserialize(const std::vector<uint8_t>& data) {
        // Not implemented for events (write-only)
        return Error("CounterEvent deserialization not supported");
    }
};

} // namespace origindb::serialization

// =============================================================================
// Reducer Functions
// =============================================================================

/**
 * Create a new counter with initial value
 */
ORIGINDB_EXPORT int32_t create_counter(const char* counter_id, int64_t initial_value) {
    try {
        std::string id(counter_id);

        // Check if counter already exists
        auto existing = db::read<Counter>("counters", id);
        if (existing.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to check existing counter: " + existing.error().message());
            return -1;
        }

        if (existing.unwrap().has_value()) {
            utils::log(utils::LogLevel::Warn, "Counter already exists: " + id);
            return 0; // Already exists, not an error
        }

        // Create new counter
        Counter counter{
            .id = id,
            .value = initial_value,
            .last_updated = utils::now()
        };

        auto write_result = db::write("counters", id, counter);
        if (write_result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to create counter: " + write_result.error().message());
            return -1;
        }

        // Emit creation event
        CounterEvent event{
            .counter_id = id,
            .old_value = 0,
            .new_value = initial_value,
            .timestamp = utils::now()
        };

        auto emit_result = events::emit("counter_created", id, event);
        if (emit_result.is_err()) {
            utils::log(utils::LogLevel::Warn, "Failed to emit creation event: " + emit_result.error().message());
            // Continue anyway, creation succeeded
        }

        utils::log(utils::LogLevel::Info, "Created counter: " + id + " with value: " + std::to_string(initial_value));
        return 1; // Success

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Exception in create_counter: " + std::string(e.what()));
        return -1;
    }
}

/**
 * Increment a counter by the specified amount
 */
ORIGINDB_EXPORT int32_t increment_counter(const char* counter_id, int64_t amount) {
    try {
        std::string id(counter_id);

        // Read current counter
        auto current_result = db::read<Counter>("counters", id);
        if (current_result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to read counter: " + current_result.error().message());
            return -1;
        }

        auto current_opt = current_result.unwrap();
        if (!current_opt.has_value()) {
            utils::log(utils::LogLevel::Error, "Counter not found: " + id);
            return -2; // Not found
        }

        Counter current = current_opt.value();
        int64_t old_value = current.value;

        // Update counter
        current.value += amount;
        current.last_updated = utils::now();

        auto write_result = db::write("counters", id, current);
        if (write_result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to update counter: " + write_result.error().message());
            return -1;
        }

        // Emit update event
        CounterEvent event{
            .counter_id = id,
            .old_value = old_value,
            .new_value = current.value,
            .timestamp = current.last_updated
        };

        auto emit_result = events::emit("counter_updated", id, event);
        if (emit_result.is_err()) {
            utils::log(utils::LogLevel::Warn, "Failed to emit update event: " + emit_result.error().message());
        }

        utils::log(utils::LogLevel::Info, "Incremented counter: " + id + " by " + std::to_string(amount) +
                  " (old: " + std::to_string(old_value) + ", new: " + std::to_string(current.value) + ")");

        return 1; // Success

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Exception in increment_counter: " + std::string(e.what()));
        return -1;
    }
}

/**
 * Get current counter value
 */
ORIGINDB_EXPORT int32_t get_counter_value(const char* counter_id, int64_t* out_value) {
    try {
        std::string id(counter_id);

        auto result = db::read<Counter>("counters", id);
        if (result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to read counter: " + result.error().message());
            return -1;
        }

        auto counter_opt = result.unwrap();
        if (!counter_opt.has_value()) {
            return 0; // Not found
        }

        if (out_value != nullptr) {
            *out_value = counter_opt.value().value;
        }

        return 1; // Found

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Exception in get_counter_value: " + std::string(e.what()));
        return -1;
    }
}

/**
 * Delete a counter
 */
ORIGINDB_EXPORT int32_t delete_counter(const char* counter_id) {
    try {
        std::string id(counter_id);

        // Read current value for event emission
        auto current_result = db::read<Counter>("counters", id);
        if (current_result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to read counter for deletion: " + current_result.error().message());
            return -1;
        }

        auto current_opt = current_result.unwrap();
        if (!current_opt.has_value()) {
            return 0; // Already deleted or never existed
        }

        Counter current = current_opt.value();

        // Delete the counter
        auto delete_result = db::remove("counters", id);
        if (delete_result.is_err()) {
            utils::log(utils::LogLevel::Error, "Failed to delete counter: " + delete_result.error().message());
            return -1;
        }

        bool was_deleted = delete_result.unwrap();
        if (was_deleted) {
            // Emit deletion event
            CounterEvent event{
                .counter_id = id,
                .old_value = current.value,
                .new_value = 0,
                .timestamp = utils::now()
            };

            auto emit_result = events::emit("counter_deleted", id, event);
            if (emit_result.is_err()) {
                utils::log(utils::LogLevel::Warn, "Failed to emit deletion event: " + emit_result.error().message());
            }

            utils::log(utils::LogLevel::Info, "Deleted counter: " + id);
        }

        return was_deleted ? 1 : 0;

    } catch (const std::exception& e) {
        utils::log(utils::LogLevel::Error, "Exception in delete_counter: " + std::string(e.what()));
        return -1;
    }
}

// =============================================================================
// Module Lifecycle
// =============================================================================

/**
 * Module initialization - called when module is loaded
 */
ORIGINDB_INIT() {
    utils::log(utils::LogLevel::Info, "Counter module initialized");
    return 0;
}

/**
 * Client connection handler
 */
ORIGINDB_CLIENT_CONNECTED() {
    utils::log(utils::LogLevel::Info, "Client connected to counter module");
    return 0;
}

/**
 * Client disconnection handler
 */
ORIGINDB_CLIENT_DISCONNECTED() {
    utils::log(utils::LogLevel::Info, "Client disconnected from counter module");
    return 0;
}