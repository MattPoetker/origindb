#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace instantdb {

// Table write operation
struct TableWrite {
    enum Operation {
        INSERT,
        UPDATE,
        DELETE,
        UPSERT
    };

    std::string table_name;
    Operation operation;
    std::vector<uint8_t> key;
    std::vector<uint8_t> old_value;  // For UPDATE
    std::vector<uint8_t> new_value;
};

// Emitted event for changefeed
struct EmittedEvent {
    std::string topic;
    uint64_t sequence_in_tx;
    std::string operation;
    std::vector<uint8_t> key;
    std::vector<uint8_t> payload;
};

// Host API interface exposed to WASM modules
class HostAPI {
public:
    virtual ~HostAPI() = default;

    // Transaction operations
    virtual int32_t host_txn_read(
        const char* table,
        const uint8_t* key, uint32_t key_len,
        uint8_t* value_out, uint32_t* value_len) = 0;

    virtual int32_t host_txn_scan(
        const char* table,
        const uint8_t* prefix, uint32_t prefix_len,
        int32_t limit) = 0;

    virtual int32_t host_txn_scan_next(
        int32_t iterator_id,
        uint8_t* key_out, uint32_t* key_len,
        uint8_t* value_out, uint32_t* value_len) = 0;

    virtual void host_txn_scan_close(int32_t iterator_id) = 0;

    virtual void host_txn_write(
        const char* table,
        const uint8_t* key, uint32_t key_len,
        const uint8_t* value, uint32_t value_len) = 0;

    virtual void host_txn_delete(
        const char* table,
        const uint8_t* key, uint32_t key_len) = 0;

    // Event emission
    virtual void host_emit_event(
        const char* topic,
        const uint8_t* key, uint32_t key_len,
        const uint8_t* payload, uint32_t payload_len) = 0;

    // Transaction control
    virtual void host_abort(const char* message) = 0;

    // Deterministic functions
    virtual uint64_t host_now_ms() = 0;
    virtual void host_random_bytes(uint8_t* out, uint32_t len) = 0;

    // Memory management helpers
    virtual void* host_alloc(uint32_t size) = 0;
    virtual void host_free(void* ptr) = 0;

    // Logging (for debugging)
    virtual void host_log(int32_t level, const char* message) = 0;
};

// Host API implementation
class HostAPIImpl : public HostAPI {
public:
    HostAPIImpl(
        std::shared_ptr<class Transaction> txn,
        std::shared_ptr<class ChangefeedEngine> changefeed,
        const struct ModuleContext& context);

    ~HostAPIImpl() override;

    // Get staged writes and events after execution
    std::vector<TableWrite> GetStagedWrites() const;
    std::vector<EmittedEvent> GetEmittedEvents() const;

    // Transaction operations
    int32_t host_txn_read(
        const char* table,
        const uint8_t* key, uint32_t key_len,
        uint8_t* value_out, uint32_t* value_len) override;

    int32_t host_txn_scan(
        const char* table,
        const uint8_t* prefix, uint32_t prefix_len,
        int32_t limit) override;

    int32_t host_txn_scan_next(
        int32_t iterator_id,
        uint8_t* key_out, uint32_t* key_len,
        uint8_t* value_out, uint32_t* value_len) override;

    void host_txn_scan_close(int32_t iterator_id) override;

    void host_txn_write(
        const char* table,
        const uint8_t* key, uint32_t key_len,
        const uint8_t* value, uint32_t value_len) override;

    void host_txn_delete(
        const char* table,
        const uint8_t* key, uint32_t key_len) override;

    void host_emit_event(
        const char* topic,
        const uint8_t* key, uint32_t key_len,
        const uint8_t* payload, uint32_t payload_len) override;

    void host_abort(const char* message) override;

    uint64_t host_now_ms() override;
    void host_random_bytes(uint8_t* out, uint32_t len) override;

    void* host_alloc(uint32_t size) override;
    void host_free(void* ptr) override;

    void host_log(int32_t level, const char* message) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb