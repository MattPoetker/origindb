#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <map>
#include <variant>
#include <memory>

namespace origindb {
namespace websocket {

// Binary Protocol Format:
// - Much more compact than JSON
// - Fixed-size headers for fast parsing
// - Variable-length data with length prefixes
// - Native number formats (no string conversion)

enum class MessageType : uint8_t {
    // Control messages
    WELCOME = 0x01,
    PING = 0x02,
    PONG = 0x03,
    ERROR = 0x04,

    // Subscription messages
    SUBSCRIBE = 0x10,
    UNSUBSCRIBE = 0x11,
    SUBSCRIPTION_CREATED = 0x12,
    SUBSCRIPTION_REMOVED = 0x13,

    // Data messages
    CHANGEFEED_EVENT = 0x20,
    SQL_RESULT = 0x21,
    WASM_EVENT = 0x22,

    // Bulk operations
    BATCH_INSERT = 0x30,
    BATCH_UPDATE = 0x31,
    BATCH_DELETE = 0x32,

    // SQL operations
    SQL_QUERY = 0x40,
    SQL_EXECUTE = 0x41,

    // Reserved for future use
    RESERVED = 0xFF
};

enum class DataType : uint8_t {
    NULL_TYPE = 0x00,
    BOOL = 0x01,
    INT8 = 0x02,
    INT16 = 0x03,
    INT32 = 0x04,
    INT64 = 0x05,
    FLOAT32 = 0x06,
    FLOAT64 = 0x07,
    STRING = 0x08,
    BINARY = 0x09,
    ARRAY = 0x0A,
    MAP = 0x0B,
    TIMESTAMP = 0x0C
};

enum class OperationType : uint8_t {
    INSERT = 0x01,
    UPDATE = 0x02,
    DELETE = 0x03,
    TRUNCATE = 0x04
};

// Binary message header (fixed 8 bytes)
struct BinaryMessageHeader {
    uint8_t magic[2];       // 0xDB, 0x01 - identifies binary protocol
    MessageType type;       // Message type
    uint8_t flags;          // Bit flags (compression, encryption, etc.)
    uint32_t payload_size;  // Size of payload in bytes
};

// Flags for message header
constexpr uint8_t FLAG_COMPRESSED = 0x01;
constexpr uint8_t FLAG_ENCRYPTED = 0x02;
constexpr uint8_t FLAG_FRAGMENTED = 0x04;
constexpr uint8_t FLAG_FINAL_FRAGMENT = 0x08;

class BinarySerializer {
public:
    BinarySerializer();

    // Write methods
    void WriteUint8(uint8_t value);
    void WriteUint16(uint16_t value);
    void WriteUint32(uint32_t value);
    void WriteUint64(uint64_t value);
    void WriteInt8(int8_t value);
    void WriteInt16(int16_t value);
    void WriteInt32(int32_t value);
    void WriteInt64(int64_t value);
    void WriteFloat(float value);
    void WriteDouble(double value);
    void WriteString(const std::string& value);
    void WriteBinary(const std::vector<uint8_t>& data);
    void WriteRaw(const void* data, size_t size);

    // High-level write methods
    void WriteMessageHeader(MessageType type, uint32_t payload_size, uint8_t flags = 0);
    void WriteDataType(DataType type);
    void WriteOperationType(OperationType type);

    // Write changefeed event
    void WriteChangefeedEvent(const std::string& table,
                              OperationType operation,
                              const std::string& key,
                              const std::map<std::string, std::variant<int64_t, double, std::string, bool>>& data);

    // Get serialized data
    const std::vector<uint8_t>& GetData() const { return buffer_; }
    std::vector<uint8_t> TakeData() { return std::move(buffer_); }
    void Clear() { buffer_.clear(); }
    size_t Size() const { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;

    void EnsureCapacity(size_t additional);
};

class BinaryDeserializer {
public:
    BinaryDeserializer(const uint8_t* data, size_t size);
    BinaryDeserializer(const std::vector<uint8_t>& data);

    // Read methods
    uint8_t ReadUint8();
    uint16_t ReadUint16();
    uint32_t ReadUint32();
    uint64_t ReadUint64();
    int8_t ReadInt8();
    int16_t ReadInt16();
    int32_t ReadInt32();
    int64_t ReadInt64();
    float ReadFloat();
    double ReadDouble();
    std::string ReadString();
    std::vector<uint8_t> ReadBinary();
    void ReadRaw(void* dest, size_t size);

    // High-level read methods
    BinaryMessageHeader ReadMessageHeader();
    DataType ReadDataType();
    OperationType ReadOperationType();

    // Read changefeed event
    struct ChangefeedEvent {
        std::string table;
        OperationType operation;
        std::string key;
        std::map<std::string, std::variant<int64_t, double, std::string, bool>> data;
    };
    ChangefeedEvent ReadChangefeedEvent();

    // Check remaining data
    bool HasData() const { return position_ < size_; }
    size_t Remaining() const { return size_ - position_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_;

    void EnsureAvailable(size_t bytes);
};

// Utility functions for protocol conversion
class ProtocolConverter {
public:
    // Convert JSON changefeed event to binary
    static std::vector<uint8_t> ChangefeedEventToBinary(
        const std::string& subscription_id,
        const std::string& table,
        const std::string& operation,
        const std::string& key,
        const std::string& json_data
    );

    // Convert binary changefeed event to JSON
    static std::string BinaryToJsonChangefeedEvent(const uint8_t* data, size_t size);

    // Create a binary welcome message
    static std::vector<uint8_t> CreateBinaryWelcomeMessage(
        const std::string& client_id,
        const std::string& server_version
    );

    // Create a binary error message
    static std::vector<uint8_t> CreateBinaryErrorMessage(
        uint32_t error_code,
        const std::string& error_message
    );

    // Create a binary ping message
    static std::vector<uint8_t> CreateBinaryPingMessage(uint64_t timestamp);

    // Create a binary pong message
    static std::vector<uint8_t> CreateBinaryPongMessage(uint64_t timestamp);

    // Check if data is binary protocol
    static bool IsBinaryProtocol(const uint8_t* data, size_t size);
};

// Performance comparison utilities
class ProtocolBenchmark {
public:
    struct BenchmarkResult {
        size_t json_size;
        size_t binary_size;
        double compression_ratio;
        uint64_t json_serialize_ns;
        uint64_t binary_serialize_ns;
        uint64_t json_deserialize_ns;
        uint64_t binary_deserialize_ns;
    };

    // Benchmark a single changefeed event
    static BenchmarkResult BenchmarkChangefeedEvent(
        const std::string& table,
        const std::string& operation,
        const std::string& key,
        const std::map<std::string, std::variant<int64_t, double, std::string, bool>>& data
    );

    // Benchmark batch operations
    static BenchmarkResult BenchmarkBatchOperation(
        const std::vector<std::map<std::string, std::variant<int64_t, double, std::string, bool>>>& records
    );
};

} // namespace websocket
} // namespace origindb