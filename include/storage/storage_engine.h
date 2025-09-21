#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <optional>
#include <functional>

#include "common/config.h"
#include "storage/table.h"
#include "storage/wal.h"
#include "storage/snapshot.h"

namespace instantdb {

class Transaction;
class Index;

// Transaction isolation levels
enum class IsolationLevel {
    READ_UNCOMMITTED,
    READ_COMMITTED,
    REPEATABLE_READ,
    SNAPSHOT,
    SERIALIZABLE
};

class StorageEngine {
public:
    explicit StorageEngine(const StorageConfig& config);
    ~StorageEngine();

    bool Initialize();
    void Shutdown();

    // Table operations
    bool CreateTable(const TableSchema& schema);
    bool DropTable(const std::string& table_name);
    std::shared_ptr<Table> GetTable(const std::string& table_name);
    std::vector<std::string> ListTables() const;

    // Transaction management
    std::shared_ptr<Transaction> BeginTransaction(IsolationLevel level = IsolationLevel::SNAPSHOT);
    bool Commit(std::shared_ptr<Transaction> txn);
    bool Rollback(std::shared_ptr<Transaction> txn);

    // Direct operations (for non-transactional access)
    bool Insert(const std::string& table, const Row& row);
    bool Update(const std::string& table, const std::string& key, const Row& row);
    bool Delete(const std::string& table, const std::string& key);
    std::optional<Row> Get(const std::string& table, const std::string& key);

    // Scan operations
    using ScanCallback = std::function<bool(const std::string&, const Row&)>;
    void Scan(const std::string& table, const std::string& start_key,
              const std::string& end_key, ScanCallback callback);

    // Index operations
    bool CreateIndex(const std::string& table, const IndexSchema& schema);
    bool DropIndex(const std::string& table, const std::string& index_name);

    // WAL and recovery
    bool AppendToWAL(const WALEntry& entry);
    bool RecoverFromWAL();
    uint64_t GetLastWALSequence() const;

    // Snapshot operations
    bool CreateSnapshot(const std::string& path);
    bool LoadSnapshot(const std::string& path);
    uint64_t GetLastSnapshotIndex() const;

    // Metrics
    struct Stats {
        uint64_t total_tables;
        uint64_t total_rows;
        uint64_t total_bytes;
        uint64_t wal_size;
        uint64_t transactions_committed;
        uint64_t transactions_aborted;
    };
    Stats GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Transaction interface
class Transaction {
public:
    virtual ~Transaction() = default;

    virtual std::string GetId() const = 0;
    virtual IsolationLevel GetIsolationLevel() const = 0;
    virtual uint64_t GetTimestamp() const = 0;

    // Operations
    virtual bool Insert(const std::string& table, const Row& row) = 0;
    virtual bool Update(const std::string& table, const std::string& key, const Row& row) = 0;
    virtual bool Delete(const std::string& table, const std::string& key) = 0;
    virtual std::optional<Row> Get(const std::string& table, const std::string& key) = 0;

    // Scan with callback
    using ScanCallback = std::function<bool(const std::string&, const Row&)>;
    virtual void Scan(const std::string& table, const std::string& start_key,
                     const std::string& end_key, ScanCallback callback) = 0;

    // Transaction state
    virtual bool IsActive() const = 0;
    virtual bool IsReadOnly() const = 0;
    virtual void SetReadOnly(bool read_only) = 0;
};

} // namespace instantdb