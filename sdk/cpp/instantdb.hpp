#pragma once

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <cstring>

/**
 * InstantDB C++ SDK for WebAssembly Modules
 *
 * This SDK provides a C++ interface for writing WASM modules that run inside
 * InstantDB.
 *
 * Key Features:
 * - Type-safe database operations
 * - Automatic serialization/deserialization
 * - Event emission for real-time subscriptions
 * - Reducer function decorators
 * - Table schema definitions
 */

namespace instantdb {

// Forward declarations
class Table;
class Reducer;
class Event;

// =============================================================================
// Core Types
// =============================================================================

/**
 * Primary key type - can be string or integer
 */
using Key = std::variant<std::string, int64_t, uint64_t>;

/**
 * Value types that can be stored in the database
 */
using Value = std::variant<
    std::monostate,    // null
    bool,
    int32_t,
    int64_t,
    uint64_t,
    float,
    double,
    std::string,
    std::vector<uint8_t>  // binary data
>;

/**
 * Error type for database operations
 */
class Error {
public:
    Error(const std::string& message) : message_(message) {}
    const std::string& message() const { return message_; }

private:
    std::string message_;
};

/**
 * Result type for operations that can fail
 */
template<typename T>
class Result {
public:
    // Success constructor
    Result(T&& value) : value_(std::move(value)), is_ok_(true) {}
    Result(const T& value) : value_(value), is_ok_(true) {}

    // Error constructor
    Result(Error&& error) : error_(std::move(error)), is_ok_(false) {}
    Result(const Error& error) : error_(error), is_ok_(false) {}

    bool is_ok() const { return is_ok_; }
    bool is_err() const { return !is_ok_; }

    const T& unwrap() const {
        if (!is_ok_) throw std::runtime_error("Called unwrap() on error result: " + error_.message());
        return value_;
    }

    T unwrap_or(T&& default_value) const {
        return is_ok_ ? value_ : std::move(default_value);
    }

    const Error& error() const {
        if (is_ok_) throw std::runtime_error("Called error() on ok result");
        return error_;
    }

private:
    union {
        T value_;
        Error error_;
    };
    bool is_ok_;
};

// =============================================================================
// Host API - Low-level interface to InstantDB
// =============================================================================

namespace host {

// External functions provided by InstantDB host (imported from WASM)
extern "C" {
    // Database operations
    int32_t host_table_read(const char* table, const uint8_t* key, uint32_t key_len, uint32_t* out_ptr, uint32_t* out_len);
    int32_t host_table_write(const char* table, const uint8_t* key, uint32_t key_len, const uint8_t* value, uint32_t value_len);
    int32_t host_table_delete(const char* table, const uint8_t* key, uint32_t key_len);
    int32_t host_table_scan(const char* table, const uint8_t* prefix, uint32_t prefix_len, uint32_t limit, uint32_t* out_ptr, uint32_t* out_len);

    // Event emission
    int32_t host_emit_event(const char* topic, const uint8_t* key, uint32_t key_len, const uint8_t* payload, uint32_t payload_len);

    // Utility functions
    uint64_t host_now_ms();
    uint64_t host_generate_id();
    void host_log(int32_t level, const char* message);
    void host_abort(const char* message);

    // Memory management
    uint32_t host_alloc(uint32_t size);
    void host_free(uint32_t ptr);
}

} // namespace host

// =============================================================================
// Serialization
// =============================================================================

namespace serialization {

/**
 * Trait for types that can be serialized to/from binary format
 */
template<typename T>
class Serializable {
public:
    static std::vector<uint8_t> serialize(const T& value);
    static Result<T> deserialize(const std::vector<uint8_t>& data);
};

// Specializations for built-in types
template<>
class Serializable<std::string> {
public:
    static std::vector<uint8_t> serialize(const std::string& value) {
        return std::vector<uint8_t>(value.begin(), value.end());
    }

    static Result<std::string> deserialize(const std::vector<uint8_t>& data) {
        return std::string(data.begin(), data.end());
    }
};

template<>
class Serializable<int64_t> {
public:
    static std::vector<uint8_t> serialize(const int64_t& value) {
        std::vector<uint8_t> result(8);
        std::memcpy(result.data(), &value, 8);
        return result;
    }

    static Result<int64_t> deserialize(const std::vector<uint8_t>& data) {
        if (data.size() != 8) {
            return Error("Invalid data size for int64_t");
        }
        int64_t value;
        std::memcpy(&value, data.data(), 8);
        return value;
    }
};

template<>
class Serializable<uint64_t> {
public:
    static std::vector<uint8_t> serialize(const uint64_t& value) {
        std::vector<uint8_t> result(8);
        std::memcpy(result.data(), &value, 8);
        return result;
    }

    static Result<uint64_t> deserialize(const std::vector<uint8_t>& data) {
        if (data.size() != 8) {
            return Error("Invalid data size for uint64_t");
        }
        uint64_t value;
        std::memcpy(&value, data.data(), 8);
        return value;
    }
};

} // namespace serialization

// =============================================================================
// Database Operations
// =============================================================================

/**
 * High-level database operations with type safety
 */
namespace db {

/**
 * Read a value from a table
 */
template<typename T>
Result<std::optional<T>> read(const std::string& table, const Key& key) {
    // Serialize key
    std::vector<uint8_t> key_data;
    if (std::holds_alternative<std::string>(key)) {
        auto& str_key = std::get<std::string>(key);
        key_data.assign(str_key.begin(), str_key.end());
    } else if (std::holds_alternative<int64_t>(key)) {
        auto int_key = std::get<int64_t>(key);
        key_data = serialization::Serializable<int64_t>::serialize(int_key);
    } else if (std::holds_alternative<uint64_t>(key)) {
        auto uint_key = std::get<uint64_t>(key);
        key_data = serialization::Serializable<uint64_t>::serialize(uint_key);
    }

    // Call host function
    uint32_t out_ptr, out_len;
    int32_t result = host::host_table_read(
        table.c_str(),
        key_data.data(),
        static_cast<uint32_t>(key_data.size()),
        &out_ptr,
        &out_len
    );

    if (result == 0) {
        // Not found
        return std::optional<T>{};
    } else if (result < 0) {
        // Error
        return Error("Database read failed");
    }

    // Deserialize result
    std::vector<uint8_t> value_data(out_len);
    std::memcpy(value_data.data(), reinterpret_cast<void*>(out_ptr), out_len);
    host::host_free(out_ptr);

    auto deserialize_result = serialization::Serializable<T>::deserialize(value_data);
    if (deserialize_result.is_err()) {
        return deserialize_result.error();
    }

    return std::optional<T>{deserialize_result.unwrap()};
}

/**
 * Write a value to a table
 */
template<typename T>
Result<void> write(const std::string& table, const Key& key, const T& value) {
    // Serialize key
    std::vector<uint8_t> key_data;
    if (std::holds_alternative<std::string>(key)) {
        auto& str_key = std::get<std::string>(key);
        key_data.assign(str_key.begin(), str_key.end());
    } else if (std::holds_alternative<int64_t>(key)) {
        auto int_key = std::get<int64_t>(key);
        key_data = serialization::Serializable<int64_t>::serialize(int_key);
    } else if (std::holds_alternative<uint64_t>(key)) {
        auto uint_key = std::get<uint64_t>(key);
        key_data = serialization::Serializable<uint64_t>::serialize(uint_key);
    }

    // Serialize value
    auto value_data = serialization::Serializable<T>::serialize(value);

    // Call host function
    int32_t result = host::host_table_write(
        table.c_str(),
        key_data.data(),
        static_cast<uint32_t>(key_data.size()),
        value_data.data(),
        static_cast<uint32_t>(value_data.size())
    );

    if (result < 0) {
        return Error("Database write failed");
    }

    return Result<void>{};
}

/**
 * Delete a value from a table
 */
Result<bool> remove(const std::string& table, const Key& key) {
    // Serialize key
    std::vector<uint8_t> key_data;
    if (std::holds_alternative<std::string>(key)) {
        auto& str_key = std::get<std::string>(key);
        key_data.assign(str_key.begin(), str_key.end());
    } else if (std::holds_alternative<int64_t>(key)) {
        auto int_key = std::get<int64_t>(key);
        key_data = serialization::Serializable<int64_t>::serialize(int_key);
    } else if (std::holds_alternative<uint64_t>(key)) {
        auto uint_key = std::get<uint64_t>(key);
        key_data = serialization::Serializable<uint64_t>::serialize(uint_key);
    }

    // Call host function
    int32_t result = host::host_table_delete(
        table.c_str(),
        key_data.data(),
        static_cast<uint32_t>(key_data.size())
    );

    if (result < 0) {
        return Error("Database delete failed");
    }

    return result > 0;
}

} // namespace db

// =============================================================================
// Event System
// =============================================================================

namespace events {

/**
 * Emit an event to the changefeed system
 */
template<typename T>
Result<void> emit(const std::string& topic, const std::string& key, const T& payload) {
    auto payload_data = serialization::Serializable<T>::serialize(payload);

    int32_t result = host::host_emit_event(
        topic.c_str(),
        reinterpret_cast<const uint8_t*>(key.c_str()),
        static_cast<uint32_t>(key.length()),
        payload_data.data(),
        static_cast<uint32_t>(payload_data.size())
    );

    if (result < 0) {
        return Error("Event emission failed");
    }

    return Result<void>{};
}

} // namespace events

// =============================================================================
// Utility Functions
// =============================================================================

namespace utils {

/**
 * Get current timestamp in milliseconds
 */
uint64_t now() {
    return host::host_now_ms();
}

/**
 * Generate a unique ID
 */
uint64_t generate_id() {
    return host::host_generate_id();
}

/**
 * Log a message with the specified level
 */
enum class LogLevel : int32_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4
};

void log(LogLevel level, const std::string& message) {
    host::host_log(static_cast<int32_t>(level), message.c_str());
}

/**
 * Abort the current transaction with an error message
 */
[[noreturn]] void abort(const std::string& message) {
    host::host_abort(message.c_str());
    std::unreachable();
}

} // namespace utils

// =============================================================================
// Subscription System
// =============================================================================

namespace subscriptions {

/**
 * Filter function for subscription events
 * Return true to include event, false to filter out
 */
INSTANTDB_EXPORT bool filter_event(const std::vector<uint8_t>& event_data) {
    // Default implementation - include all events
    return true;
}

/**
 * Transform function for subscription events
 * Return transformed event data
 */
INSTANTDB_EXPORT std::vector<uint8_t> transform_event(const std::vector<uint8_t>& event_data) {
    // Default implementation - pass through unchanged
    return event_data;
}

/**
 * Get initial data for subscription
 * Called when client subscribes with include_initial_data = true
 */
INSTANTDB_EXPORT std::vector<uint8_t> get_initial_data(const std::string& where_clause) {
    // Default implementation - return empty
    return {};
}

/**
 * Client-specific subscription event
 * Called when subscription events need to be sent to specific clients
 */
INSTANTDB_EXPORT void emit_to_client(const std::string& client_id,
                                   const std::string& event_type,
                                   const std::vector<uint8_t>& data) {
    // Implementation would use host API to emit to specific client
    // This is a placeholder for the host function call
}

} // namespace subscriptions

// =============================================================================
// Macros for Easy Development
// =============================================================================

/**
 * Declare a reducer function
 *
 * Usage:
 * INSTANTDB_REDUCER(my_function, int64_t user_id, std::string name) {
 *     // Reducer implementation
 *     return Result<std::string>{"success"};
 * }
 */
#define INSTANTDB_REDUCER(name, ...) \
    extern "C" int32_t name##_reducer(__VA_ARGS__); \
    extern "C" int32_t name##_reducer(__VA_ARGS__)

/**
 * Export a function for WebAssembly
 */
#define INSTANTDB_EXPORT extern "C" __attribute__((visibility("default")))

/**
 * Declare module initialization function
 */
#define INSTANTDB_INIT() \
    INSTANTDB_EXPORT int32_t module_init()

/**
 * Declare client connection handler
 */
#define INSTANTDB_CLIENT_CONNECTED() \
    INSTANTDB_EXPORT int32_t client_connected(const char* connection_id)

/**
 * Declare client disconnection handler
 */
#define INSTANTDB_CLIENT_DISCONNECTED() \
    INSTANTDB_EXPORT int32_t client_disconnected(const char* connection_id)

} // namespace instantdb

// =============================================================================
// Convenience Aliases
// =============================================================================

namespace idb = instantdb;