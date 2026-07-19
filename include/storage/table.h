#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>

namespace origindb {

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

    // Cached value-JSON access. `ser` renders a row to its canonical JSON
    // "value" form; an implementation may memoize the result per row and
    // invalidate it on write, so repeated scans/reads of unchanged rows skip
    // re-serialization entirely. Universal: every WASM module scan and point
    // read flows through here. The default is a non-caching passthrough so
    // other Table implementations keep working unchanged. NOTE: the cache
    // assumes a single stable serializer format across calls.
    using JsonSerializer = std::function<std::string(const Row&)>;
    using JsonRowCallback =
        std::function<bool(const std::string& key, const std::string& value_json)>;

    virtual std::optional<std::string> GetJsonCached(
        const std::string& key, const JsonSerializer& ser) const {
        auto r = Get(key);
        if (!r) return std::nullopt;
        return ser(*r);
    }
    virtual void ScanJsonCached(const std::string& start_key,
                                const std::string& end_key,
                                const JsonSerializer& ser,
                                const JsonRowCallback& cb) const {
        Scan(start_key, end_key,
             [&](const std::string& k, const Row& row) { return cb(k, ser(row)); });
    }

    // Index operations
    virtual bool CreateIndex(const IndexSchema& schema) = 0;
    virtual bool DropIndex(const std::string& index_name) = 0;
    virtual std::vector<std::string> GetIndexNames() const = 0;
};

} // namespace origindb