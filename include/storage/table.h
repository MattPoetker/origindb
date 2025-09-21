#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <chrono>
#include <unordered_map>

namespace instantdb {

// Data types supported by the storage engine
enum class DataType {
    INT32,
    INT64,
    FLOAT,
    DOUBLE,
    STRING,
    BYTES,
    BOOLEAN,
    TIMESTAMP,
    JSON
};

// Column definition
struct Column {
    std::string name;
    DataType type;
    bool nullable = false;
    bool is_primary_key = false;
    std::variant<std::monostate, int64_t, double, std::string> default_value;
};

// Table schema
struct TableSchema {
    std::string name;
    std::vector<Column> columns;
    std::vector<std::string> primary_key;
    std::unordered_map<std::string, std::string> options;
};

// Index types
enum class IndexType {
    BTREE,
    HASH,
    FULLTEXT
};

// Index schema
struct IndexSchema {
    std::string name;
    std::vector<std::string> columns;
    IndexType type = IndexType::BTREE;
    bool unique = false;
};

// Value variant for row data
using Value = std::variant<
    std::monostate,  // NULL
    int32_t,
    int64_t,
    float,
    double,
    std::string,
    std::vector<uint8_t>,
    bool,
    std::chrono::system_clock::time_point
>;

// Row representation
struct Row {
    std::string key;  // Primary key
    std::unordered_map<std::string, Value> columns;
    uint64_t version = 0;  // MVCC version
    uint64_t timestamp = 0;  // Commit timestamp
};

// Table interface
class Table {
public:
    virtual ~Table() = default;

    virtual const TableSchema& GetSchema() const = 0;
    virtual uint64_t GetRowCount() const = 0;
    virtual uint64_t GetSizeBytes() const = 0;

    // Basic operations
    virtual bool Insert(const Row& row) = 0;
    virtual bool Update(const std::string& key, const Row& row) = 0;
    virtual bool Delete(const std::string& key) = 0;
    virtual std::optional<Row> Get(const std::string& key) const = 0;

    // Scan operations
    using ScanCallback = std::function<bool(const std::string&, const Row&)>;
    virtual void Scan(const std::string& start_key, const std::string& end_key,
                     ScanCallback callback) const = 0;

    // Index operations
    virtual bool CreateIndex(const IndexSchema& schema) = 0;
    virtual bool DropIndex(const std::string& index_name) = 0;
    virtual std::vector<std::string> GetIndexNames() const = 0;
};

} // namespace instantdb