#include "websocket/binary_protocol.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace origindb {
namespace websocket {

// Magic bytes for binary protocol
constexpr uint8_t MAGIC_BYTE_1 = 0xDB;
constexpr uint8_t MAGIC_BYTE_2 = 0x01;

// BinarySerializer implementation
BinarySerializer::BinarySerializer() {
    buffer_.reserve(1024); // Pre-allocate for typical message size
}

void BinarySerializer::EnsureCapacity(size_t additional) {
    size_t required = buffer_.size() + additional;
    if (buffer_.capacity() < required) {
        buffer_.reserve(required * 2); // Double capacity for efficiency
    }
}

void BinarySerializer::WriteUint8(uint8_t value) {
    buffer_.push_back(value);
}

void BinarySerializer::WriteUint16(uint16_t value) {
    EnsureCapacity(2);
    buffer_.push_back((value >> 8) & 0xFF);
    buffer_.push_back(value & 0xFF);
}

void BinarySerializer::WriteUint32(uint32_t value) {
    EnsureCapacity(4);
    buffer_.push_back((value >> 24) & 0xFF);
    buffer_.push_back((value >> 16) & 0xFF);
    buffer_.push_back((value >> 8) & 0xFF);
    buffer_.push_back(value & 0xFF);
}

void BinarySerializer::WriteUint64(uint64_t value) {
    EnsureCapacity(8);
    for (int i = 7; i >= 0; i--) {
        buffer_.push_back((value >> (i * 8)) & 0xFF);
    }
}

void BinarySerializer::WriteInt8(int8_t value) {
    WriteUint8(static_cast<uint8_t>(value));
}

void BinarySerializer::WriteInt16(int16_t value) {
    WriteUint16(static_cast<uint16_t>(value));
}

void BinarySerializer::WriteInt32(int32_t value) {
    WriteUint32(static_cast<uint32_t>(value));
}

void BinarySerializer::WriteInt64(int64_t value) {
    WriteUint64(static_cast<uint64_t>(value));
}

void BinarySerializer::WriteFloat(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(float));
    WriteUint32(bits);
}

void BinarySerializer::WriteDouble(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));
    WriteUint64(bits);
}

void BinarySerializer::WriteString(const std::string& value) {
    WriteUint32(static_cast<uint32_t>(value.size()));
    WriteRaw(value.data(), value.size());
}

void BinarySerializer::WriteBinary(const std::vector<uint8_t>& data) {
    WriteUint32(static_cast<uint32_t>(data.size()));
    WriteRaw(data.data(), data.size());
}

void BinarySerializer::WriteRaw(const void* data, size_t size) {
    if (size == 0) return;
    EnsureCapacity(size);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void BinarySerializer::WriteMessageHeader(MessageType type, uint32_t payload_size, uint8_t flags) {
    WriteUint8(MAGIC_BYTE_1);
    WriteUint8(MAGIC_BYTE_2);
    WriteUint8(static_cast<uint8_t>(type));
    WriteUint8(flags);
    WriteUint32(payload_size);
}

void BinarySerializer::WriteDataType(DataType type) {
    WriteUint8(static_cast<uint8_t>(type));
}

void BinarySerializer::WriteOperationType(OperationType type) {
    WriteUint8(static_cast<uint8_t>(type));
}

void BinarySerializer::WriteChangefeedEvent(
    const std::string& table,
    OperationType operation,
    const std::string& key,
    const std::map<std::string, std::variant<int64_t, double, std::string, bool>>& data
) {
    // Write table name
    WriteString(table);

    // Write operation type
    WriteOperationType(operation);

    // Write key
    WriteString(key);

    // Write field count
    WriteUint32(static_cast<uint32_t>(data.size()));

    // Write each field
    for (const auto& [field_name, field_value] : data) {
        WriteString(field_name);

        // Write type and value based on variant type
        if (std::holds_alternative<int64_t>(field_value)) {
            WriteDataType(DataType::INT64);
            WriteInt64(std::get<int64_t>(field_value));
        } else if (std::holds_alternative<double>(field_value)) {
            WriteDataType(DataType::FLOAT64);
            WriteDouble(std::get<double>(field_value));
        } else if (std::holds_alternative<std::string>(field_value)) {
            WriteDataType(DataType::STRING);
            WriteString(std::get<std::string>(field_value));
        } else if (std::holds_alternative<bool>(field_value)) {
            WriteDataType(DataType::BOOL);
            WriteUint8(std::get<bool>(field_value) ? 1 : 0);
        }
    }
}

// BinaryDeserializer implementation
BinaryDeserializer::BinaryDeserializer(const uint8_t* data, size_t size)
    : data_(data), size_(size), position_(0) {}

BinaryDeserializer::BinaryDeserializer(const std::vector<uint8_t>& data)
    : data_(data.data()), size_(data.size()), position_(0) {}

void BinaryDeserializer::EnsureAvailable(size_t bytes) {
    if (position_ + bytes > size_) {
        throw std::runtime_error("Insufficient data in buffer");
    }
}

uint8_t BinaryDeserializer::ReadUint8() {
    EnsureAvailable(1);
    return data_[position_++];
}

uint16_t BinaryDeserializer::ReadUint16() {
    EnsureAvailable(2);
    uint16_t value = (static_cast<uint16_t>(data_[position_]) << 8) |
                     static_cast<uint16_t>(data_[position_ + 1]);
    position_ += 2;
    return value;
}

uint32_t BinaryDeserializer::ReadUint32() {
    EnsureAvailable(4);
    uint32_t value = (static_cast<uint32_t>(data_[position_]) << 24) |
                     (static_cast<uint32_t>(data_[position_ + 1]) << 16) |
                     (static_cast<uint32_t>(data_[position_ + 2]) << 8) |
                     static_cast<uint32_t>(data_[position_ + 3]);
    position_ += 4;
    return value;
}

uint64_t BinaryDeserializer::ReadUint64() {
    EnsureAvailable(8);
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value = (value << 8) | data_[position_ + i];
    }
    position_ += 8;
    return value;
}

int8_t BinaryDeserializer::ReadInt8() {
    return static_cast<int8_t>(ReadUint8());
}

int16_t BinaryDeserializer::ReadInt16() {
    return static_cast<int16_t>(ReadUint16());
}

int32_t BinaryDeserializer::ReadInt32() {
    return static_cast<int32_t>(ReadUint32());
}

int64_t BinaryDeserializer::ReadInt64() {
    return static_cast<int64_t>(ReadUint64());
}

float BinaryDeserializer::ReadFloat() {
    uint32_t bits = ReadUint32();
    float value;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
}

double BinaryDeserializer::ReadDouble() {
    uint64_t bits = ReadUint64();
    double value;
    std::memcpy(&value, &bits, sizeof(double));
    return value;
}

std::string BinaryDeserializer::ReadString() {
    uint32_t length = ReadUint32();
    EnsureAvailable(length);
    std::string result(reinterpret_cast<const char*>(data_ + position_), length);
    position_ += length;
    return result;
}

std::vector<uint8_t> BinaryDeserializer::ReadBinary() {
    uint32_t length = ReadUint32();
    EnsureAvailable(length);
    std::vector<uint8_t> result(data_ + position_, data_ + position_ + length);
    position_ += length;
    return result;
}

void BinaryDeserializer::ReadRaw(void* dest, size_t size) {
    EnsureAvailable(size);
    std::memcpy(dest, data_ + position_, size);
    position_ += size;
}

BinaryMessageHeader BinaryDeserializer::ReadMessageHeader() {
    BinaryMessageHeader header;
    header.magic[0] = ReadUint8();
    header.magic[1] = ReadUint8();
    header.type = static_cast<MessageType>(ReadUint8());
    header.flags = ReadUint8();
    header.payload_size = ReadUint32();
    return header;
}

DataType BinaryDeserializer::ReadDataType() {
    return static_cast<DataType>(ReadUint8());
}

OperationType BinaryDeserializer::ReadOperationType() {
    return static_cast<OperationType>(ReadUint8());
}

BinaryDeserializer::ChangefeedEvent BinaryDeserializer::ReadChangefeedEvent() {
    ChangefeedEvent event;

    event.table = ReadString();
    event.operation = ReadOperationType();
    event.key = ReadString();

    uint32_t field_count = ReadUint32();
    for (uint32_t i = 0; i < field_count; i++) {
        std::string field_name = ReadString();
        DataType type = ReadDataType();

        switch (type) {
            case DataType::INT64:
                event.data[field_name] = ReadInt64();
                break;
            case DataType::FLOAT64:
                event.data[field_name] = ReadDouble();
                break;
            case DataType::STRING:
                event.data[field_name] = ReadString();
                break;
            case DataType::BOOL:
                event.data[field_name] = (ReadUint8() != 0);
                break;
            default:
                throw std::runtime_error("Unsupported data type in changefeed event");
        }
    }

    return event;
}

// ProtocolConverter implementation
std::vector<uint8_t> ProtocolConverter::ChangefeedEventToBinary(
    const std::string& subscription_id,
    const std::string& table,
    const std::string& operation,
    const std::string& key,
    const std::string& json_data
) {
    BinarySerializer serializer;

    // Parse JSON data
    nlohmann::json json = nlohmann::json::parse(json_data);

    // Convert operation string to enum
    OperationType op_type;
    if (operation == "insert") {
        op_type = OperationType::INSERT;
    } else if (operation == "update") {
        op_type = OperationType::UPDATE;
    } else if (operation == "delete") {
        op_type = OperationType::DELETE;
    } else {
        op_type = OperationType::TRUNCATE;
    }

    // Build data map
    std::map<std::string, std::variant<int64_t, double, std::string, bool>> data;
    for (auto& [key, value] : json.items()) {
        if (value.is_number_integer()) {
            data[key] = value.get<int64_t>();
        } else if (value.is_number_float()) {
            data[key] = value.get<double>();
        } else if (value.is_string()) {
            data[key] = value.get<std::string>();
        } else if (value.is_boolean()) {
            data[key] = value.get<bool>();
        }
    }

    // Create payload first to know its size
    BinarySerializer payload_serializer;
    payload_serializer.WriteString(subscription_id);
    payload_serializer.WriteChangefeedEvent(table, op_type, key, data);

    // Write header
    serializer.WriteMessageHeader(
        MessageType::CHANGEFEED_EVENT,
        static_cast<uint32_t>(payload_serializer.Size()),
        0
    );

    // Write payload
    serializer.WriteRaw(payload_serializer.GetData().data(), payload_serializer.Size());

    return serializer.TakeData();
}

std::string ProtocolConverter::BinaryToJsonChangefeedEvent(const uint8_t* data, size_t size) {
    BinaryDeserializer deserializer(data, size);

    // Read header
    BinaryMessageHeader header = deserializer.ReadMessageHeader();
    if (header.type != MessageType::CHANGEFEED_EVENT) {
        throw std::runtime_error("Not a changefeed event message");
    }

    // Read subscription ID
    std::string subscription_id = deserializer.ReadString();

    // Read event
    auto event = deserializer.ReadChangefeedEvent();

    // Convert to JSON
    nlohmann::json json;
    json["type"] = "changefeed_event";
    json["subscription_id"] = subscription_id;
    json["table"] = event.table;

    // Convert operation type
    switch (event.operation) {
        case OperationType::INSERT:
            json["operation"] = "insert";
            break;
        case OperationType::UPDATE:
            json["operation"] = "update";
            break;
        case OperationType::DELETE:
            json["operation"] = "delete";
            break;
        case OperationType::TRUNCATE:
            json["operation"] = "truncate";
            break;
    }

    json["key"] = event.key;

    // Convert data
    nlohmann::json data_json;
    for (const auto& [field, value] : event.data) {
        if (std::holds_alternative<int64_t>(value)) {
            data_json[field] = std::get<int64_t>(value);
        } else if (std::holds_alternative<double>(value)) {
            data_json[field] = std::get<double>(value);
        } else if (std::holds_alternative<std::string>(value)) {
            data_json[field] = std::get<std::string>(value);
        } else if (std::holds_alternative<bool>(value)) {
            data_json[field] = std::get<bool>(value);
        }
    }
    json["data"] = data_json;

    return json.dump();
}

std::vector<uint8_t> ProtocolConverter::CreateBinaryWelcomeMessage(
    const std::string& client_id,
    const std::string& server_version
) {
    BinarySerializer serializer;

    // Create payload
    BinarySerializer payload;
    payload.WriteString(client_id);
    payload.WriteString(server_version);
    payload.WriteUint32(3); // Number of features
    payload.WriteString("changefeed");
    payload.WriteString("wasm_subscriptions");
    payload.WriteString("binary_protocol");

    // Write header
    serializer.WriteMessageHeader(
        MessageType::WELCOME,
        static_cast<uint32_t>(payload.Size()),
        0
    );

    // Write payload
    serializer.WriteRaw(payload.GetData().data(), payload.Size());

    return serializer.TakeData();
}

std::vector<uint8_t> ProtocolConverter::CreateBinaryErrorMessage(
    uint32_t error_code,
    const std::string& error_message
) {
    BinarySerializer serializer;

    // Create payload
    BinarySerializer payload;
    payload.WriteUint32(error_code);
    payload.WriteString(error_message);

    // Write header
    serializer.WriteMessageHeader(
        MessageType::ERROR,
        static_cast<uint32_t>(payload.Size()),
        0
    );

    // Write payload
    serializer.WriteRaw(payload.GetData().data(), payload.Size());

    return serializer.TakeData();
}

std::vector<uint8_t> ProtocolConverter::CreateBinaryPingMessage(uint64_t timestamp) {
    BinarySerializer serializer;

    // Create payload
    BinarySerializer payload;
    payload.WriteUint64(timestamp);

    // Write header
    serializer.WriteMessageHeader(
        MessageType::PING,
        static_cast<uint32_t>(payload.Size()),
        0
    );

    // Write payload
    serializer.WriteRaw(payload.GetData().data(), payload.Size());

    return serializer.TakeData();
}

std::vector<uint8_t> ProtocolConverter::CreateBinaryPongMessage(uint64_t timestamp) {
    BinarySerializer serializer;

    // Create payload
    BinarySerializer payload;
    payload.WriteUint64(timestamp);

    // Write header
    serializer.WriteMessageHeader(
        MessageType::PONG,
        static_cast<uint32_t>(payload.Size()),
        0
    );

    // Write payload
    serializer.WriteRaw(payload.GetData().data(), payload.Size());

    return serializer.TakeData();
}

bool ProtocolConverter::IsBinaryProtocol(const uint8_t* data, size_t size) {
    return size >= 2 && data[0] == MAGIC_BYTE_1 && data[1] == MAGIC_BYTE_2;
}

// ProtocolBenchmark implementation
ProtocolBenchmark::BenchmarkResult ProtocolBenchmark::BenchmarkChangefeedEvent(
    const std::string& table,
    const std::string& operation,
    const std::string& key,
    const std::map<std::string, std::variant<int64_t, double, std::string, bool>>& data
) {
    BenchmarkResult result = {};

    // Create JSON version
    nlohmann::json json;
    json["type"] = "changefeed_event";
    json["subscription_id"] = "benchmark-sub";
    json["table"] = table;
    json["operation"] = operation;
    json["key"] = key;

    nlohmann::json data_json;
    for (const auto& [field, value] : data) {
        if (std::holds_alternative<int64_t>(value)) {
            data_json[field] = std::get<int64_t>(value);
        } else if (std::holds_alternative<double>(value)) {
            data_json[field] = std::get<double>(value);
        } else if (std::holds_alternative<std::string>(value)) {
            data_json[field] = std::get<std::string>(value);
        } else if (std::holds_alternative<bool>(value)) {
            data_json[field] = std::get<bool>(value);
        }
    }
    json["data"] = data_json;

    // Benchmark JSON serialization
    auto start = std::chrono::high_resolution_clock::now();
    std::string json_str = json.dump();
    auto end = std::chrono::high_resolution_clock::now();
    result.json_serialize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    result.json_size = json_str.size();

    // Benchmark binary serialization
    start = std::chrono::high_resolution_clock::now();

    BinarySerializer serializer;
    OperationType op_type = (operation == "insert") ? OperationType::INSERT :
                           (operation == "update") ? OperationType::UPDATE :
                           (operation == "delete") ? OperationType::DELETE :
                           OperationType::TRUNCATE;

    BinarySerializer payload;
    payload.WriteString("benchmark-sub");
    payload.WriteChangefeedEvent(table, op_type, key, data);

    serializer.WriteMessageHeader(
        MessageType::CHANGEFEED_EVENT,
        static_cast<uint32_t>(payload.Size()),
        0
    );
    serializer.WriteRaw(payload.GetData().data(), payload.Size());

    auto binary_data = serializer.TakeData();
    end = std::chrono::high_resolution_clock::now();
    result.binary_serialize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    result.binary_size = binary_data.size();

    // Benchmark JSON deserialization
    start = std::chrono::high_resolution_clock::now();
    auto parsed_json = nlohmann::json::parse(json_str);
    end = std::chrono::high_resolution_clock::now();
    result.json_deserialize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    // Benchmark binary deserialization
    start = std::chrono::high_resolution_clock::now();
    BinaryDeserializer deserializer(binary_data);
    auto header = deserializer.ReadMessageHeader();
    auto sub_id = deserializer.ReadString();
    auto event = deserializer.ReadChangefeedEvent();
    end = std::chrono::high_resolution_clock::now();
    result.binary_deserialize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    // Calculate compression ratio
    result.compression_ratio = static_cast<double>(result.json_size) / static_cast<double>(result.binary_size);

    return result;
}

ProtocolBenchmark::BenchmarkResult ProtocolBenchmark::BenchmarkBatchOperation(
    const std::vector<std::map<std::string, std::variant<int64_t, double, std::string, bool>>>& records
) {
    BenchmarkResult result = {};

    // Create JSON version
    nlohmann::json json;
    json["type"] = "batch_insert";
    json["table"] = "benchmark_table";
    json["records"] = nlohmann::json::array();

    for (const auto& record : records) {
        nlohmann::json record_json;
        for (const auto& [field, value] : record) {
            if (std::holds_alternative<int64_t>(value)) {
                record_json[field] = std::get<int64_t>(value);
            } else if (std::holds_alternative<double>(value)) {
                record_json[field] = std::get<double>(value);
            } else if (std::holds_alternative<std::string>(value)) {
                record_json[field] = std::get<std::string>(value);
            } else if (std::holds_alternative<bool>(value)) {
                record_json[field] = std::get<bool>(value);
            }
        }
        json["records"].push_back(record_json);
    }

    // Benchmark JSON serialization
    auto start = std::chrono::high_resolution_clock::now();
    std::string json_str = json.dump();
    auto end = std::chrono::high_resolution_clock::now();
    result.json_serialize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    result.json_size = json_str.size();

    // Benchmark binary serialization
    start = std::chrono::high_resolution_clock::now();

    BinarySerializer serializer;
    BinarySerializer payload;

    payload.WriteString("benchmark_table");
    payload.WriteUint32(static_cast<uint32_t>(records.size()));

    for (const auto& record : records) {
        payload.WriteUint32(static_cast<uint32_t>(record.size()));
        for (const auto& [field, value] : record) {
            payload.WriteString(field);
            if (std::holds_alternative<int64_t>(value)) {
                payload.WriteDataType(DataType::INT64);
                payload.WriteInt64(std::get<int64_t>(value));
            } else if (std::holds_alternative<double>(value)) {
                payload.WriteDataType(DataType::FLOAT64);
                payload.WriteDouble(std::get<double>(value));
            } else if (std::holds_alternative<std::string>(value)) {
                payload.WriteDataType(DataType::STRING);
                payload.WriteString(std::get<std::string>(value));
            } else if (std::holds_alternative<bool>(value)) {
                payload.WriteDataType(DataType::BOOL);
                payload.WriteUint8(std::get<bool>(value) ? 1 : 0);
            }
        }
    }

    serializer.WriteMessageHeader(
        MessageType::BATCH_INSERT,
        static_cast<uint32_t>(payload.Size()),
        0
    );
    serializer.WriteRaw(payload.GetData().data(), payload.Size());

    auto binary_data = serializer.TakeData();
    end = std::chrono::high_resolution_clock::now();
    result.binary_serialize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    result.binary_size = binary_data.size();

    // Calculate compression ratio
    result.compression_ratio = static_cast<double>(result.json_size) / static_cast<double>(result.binary_size);

    return result;
}

} // namespace websocket
} // namespace origindb