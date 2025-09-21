#include "storage/storage_engine.h"
#include "storage/mem_table.h"
#include "storage/transaction_impl.h"
#include "storage/wal_impl.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <atomic>

namespace instantdb {

class StorageEngine::Impl {
public:
    explicit Impl(const StorageConfig& config)
        : config_(config),
          next_txn_id_(1),
          next_timestamp_(1),
          stats_{} {
    }

    bool Initialize() {
        // Create data directory if needed
        std::filesystem::create_directories(config_.data_dir);
        std::filesystem::create_directories(config_.data_dir + "/wal");
        std::filesystem::create_directories(config_.data_dir + "/snapshots");

        // Initialize WAL
        wal_ = std::make_unique<WALImpl>(config_.data_dir + "/wal", config_);
        if (!wal_->Initialize()) {
            spdlog::error("Failed to initialize WAL");
            return false;
        }

        // TODO: Initialize snapshot manager
        // snapshot_manager_ = std::make_unique<SnapshotManager>(
        //     config_.data_dir + "/snapshots", config_);

        // Recover from WAL if exists
        if (!RecoverFromWAL()) {
            spdlog::error("Failed to recover from WAL");
            return false;
        }

        spdlog::info("Storage engine initialized with {} tables",
                    tables_.size());
        return true;
    }

    void Shutdown() {
        // Flush WAL
        if (wal_) {
            wal_->Flush();
        }

        // Clear tables
        std::unique_lock lock(tables_mutex_);
        tables_.clear();
    }

    bool CreateTable(const TableSchema& schema) {
        std::unique_lock lock(tables_mutex_);

        if (tables_.find(schema.name) != tables_.end()) {
            spdlog::warn("Table {} already exists", schema.name);
            return false;
        }

        auto table = std::make_shared<MemTable>(schema);
        tables_[schema.name] = table;

        // Log to WAL
        spdlog::debug("CreateTable: Preparing WAL entry for table '{}'", schema.name);
        WALEntry entry;
        entry.type = WALEntryType::CREATE_TABLE;
        entry.table_name = schema.name;
        entry.data = SerializeSchema(schema);
        spdlog::debug("CreateTable: WAL entry prepared with {} bytes of data", entry.data.size());
        wal_->Append(entry);
        spdlog::debug("CreateTable: WAL entry appended successfully");

        stats_.total_tables++;
        spdlog::info("Created table {}", schema.name);
        return true;
    }

    bool DropTable(const std::string& table_name) {
        std::unique_lock lock(tables_mutex_);

        auto it = tables_.find(table_name);
        if (it == tables_.end()) {
            spdlog::warn("Table {} does not exist", table_name);
            return false;
        }

        stats_.total_rows -= it->second->GetRowCount();
        stats_.total_bytes -= it->second->GetSizeBytes();
        tables_.erase(it);

        // Log to WAL
        WALEntry entry;
        entry.type = WALEntryType::DROP_TABLE;
        entry.table_name = table_name;
        wal_->Append(entry);

        stats_.total_tables--;
        spdlog::info("Dropped table {}", table_name);
        return true;
    }

    std::shared_ptr<Table> GetTable(const std::string& table_name) {
        std::shared_lock lock(tables_mutex_);
        auto it = tables_.find(table_name);
        return (it != tables_.end()) ? it->second : nullptr;
    }

    std::vector<std::string> ListTables() const {
        std::shared_lock lock(tables_mutex_);
        std::vector<std::string> result;
        result.reserve(tables_.size());
        for (const auto& [name, _] : tables_) {
            result.push_back(name);
        }
        return result;
    }

    std::shared_ptr<Transaction> BeginTransaction(IsolationLevel level) {
        uint64_t txn_id = next_txn_id_.fetch_add(1);
        uint64_t timestamp = next_timestamp_.fetch_add(1);

        auto txn = std::make_shared<TransactionImpl>(
            txn_id, timestamp, level, this);

        std::unique_lock lock(active_txns_mutex_);
        active_transactions_[timestamp] = txn;

        spdlog::debug("Started transaction {} with isolation level {}",
                     txn_id, static_cast<int>(level));
        return txn;
    }

    bool Commit(std::shared_ptr<TransactionImpl> txn) {
        spdlog::info("StorageEngine::Commit: Starting commit for transaction");

        if (!txn->IsActive()) {
            spdlog::error("StorageEngine::Commit: Transaction not active");
            return false;
        }

        // Get write set
        auto write_set = txn->GetWriteSet();
        spdlog::info("StorageEngine::Commit: Write set has {} operations", write_set.size());

        if (!write_set.empty()) {
            // Validate and apply writes
            spdlog::info("StorageEngine::Commit: Acquiring tables lock");
            std::unique_lock lock(tables_mutex_);
            spdlog::info("StorageEngine::Commit: Tables lock acquired");

            for (const auto& write : write_set) {
                spdlog::info("StorageEngine::Commit: Processing write for table {}", write.table_name);

                spdlog::info("StorageEngine::Commit: Getting table {}", write.table_name);
                // Don't call GetTable() here since we already hold tables_mutex_ lock
                auto it = tables_.find(write.table_name);
                if (it == tables_.end()) {
                    spdlog::error("Table {} not found during commit",
                                 write.table_name);
                    return false;
                }
                auto table = it->second;

                spdlog::info("StorageEngine::Commit: Table found, applying write operation {}", static_cast<int>(write.op));
                // Apply write based on operation type
                bool success = false;
                switch (write.op) {
                    case WriteOp::INSERT:
                        spdlog::info("StorageEngine::Commit: Calling table->Insert for key {}", write.row.key);
                        success = table->Insert(write.row);
                        spdlog::info("StorageEngine::Commit: table->Insert returned {}", success);
                        if (success) stats_.total_rows++;
                        break;
                    case WriteOp::UPDATE:
                        success = table->Update(write.row.key, write.row);
                        break;
                    case WriteOp::DELETE:
                        success = table->Delete(write.row.key);
                        if (success) stats_.total_rows--;
                        break;
                }

                if (!success) {
                    spdlog::error("Failed to apply write to table {}",
                                 write.table_name);
                    return false;
                }

                spdlog::info("StorageEngine::Commit: Write applied successfully, logging to WAL");
                // Log to WAL
                WALEntry entry;

                // Map WriteOp to WALEntryType correctly
                switch (write.op) {
                    case WriteOp::INSERT:
                        entry.type = WALEntryType::INSERT;
                        break;
                    case WriteOp::UPDATE:
                        entry.type = WALEntryType::UPDATE;
                        break;
                    case WriteOp::DELETE:
                        entry.type = WALEntryType::DELETE;
                        break;
                }

                entry.table_name = write.table_name;
                entry.key = write.row.key;

                spdlog::info("StorageEngine::Commit: Serializing row for WAL");
                entry.data = SerializeRow(write.row);
                spdlog::info("StorageEngine::Commit: Row serialized, appending to WAL");

                entry.transaction_id = txn->GetTimestamp(); // Use timestamp as ID
                entry.timestamp = txn->GetTimestamp();
                wal_->Append(entry);
                spdlog::info("StorageEngine::Commit: WAL append completed");
            }
        }

        // Remove from active transactions
        {
            std::unique_lock lock(active_txns_mutex_);
            active_transactions_.erase(txn->GetTimestamp());
        }

        txn->SetCommitted();
        stats_.transactions_committed++;

        spdlog::debug("Committed transaction {}", txn->GetId());
        return true;
    }

    bool Rollback(std::shared_ptr<TransactionImpl> txn) {
        if (!txn->IsActive()) {
            return false;
        }

        // Remove from active transactions
        {
            std::unique_lock lock(active_txns_mutex_);
            active_transactions_.erase(txn->GetTimestamp());
        }

        txn->SetAborted();
        stats_.transactions_aborted++;

        spdlog::debug("Rolled back transaction {}", txn->GetId());
        return true;
    }

    bool RecoverFromWAL() {
        auto entries = wal_->ReadAll();
        spdlog::info("Recovering {} WAL entries", entries.size());

        for (const auto& entry : entries) {
            try {
                spdlog::debug("Recovering WAL entry: type={}, table={}, key={}, data_size={}",
                            static_cast<int>(entry.type), entry.table_name, entry.key, entry.data.size());

                switch (entry.type) {
                    case WALEntryType::CREATE_TABLE: {
                        try {
                            TableSchema schema = DeserializeSchema(entry.data);

                            // Check if table already exists (avoid duplicates during recovery)
                            std::unique_lock lock(tables_mutex_);
                            if (tables_.find(schema.name) == tables_.end()) {
                                // Create table without logging to WAL (recovery mode)
                                auto table = std::make_shared<MemTable>(schema);
                                tables_[schema.name] = table;
                                stats_.total_tables++;
                                lock.unlock();
                                spdlog::info("Recovered table '{}' from WAL", schema.name);
                            } else {
                                spdlog::debug("Table '{}' already exists, skipping CREATE_TABLE recovery", schema.name);
                            }
                        } catch (const std::exception& e) {
                            spdlog::error("Failed to recover CREATE_TABLE for '{}': {}", entry.table_name, e.what());
                            // Continue recovery but note the failure
                        }
                        break;
                    }
                    case WALEntryType::DROP_TABLE:
                        DropTable(entry.table_name);
                        break;
                    case WALEntryType::INSERT: {
                        auto table = GetTable(entry.table_name);
                        if (table) {
                            Row row = DeserializeRow(entry.data);
                            table->Insert(row);
                            stats_.total_rows++;
                        } else {
                            spdlog::warn("Table '{}' not found during INSERT recovery", entry.table_name);
                        }
                        break;
                    }
                    case WALEntryType::UPDATE: {
                        auto table = GetTable(entry.table_name);
                        if (table) {
                            Row row = DeserializeRow(entry.data);
                            table->Update(entry.key, row);
                        } else {
                            spdlog::warn("Table '{}' not found during UPDATE recovery", entry.table_name);
                        }
                        break;
                    }
                    case WALEntryType::DELETE: {
                        auto table = GetTable(entry.table_name);
                        if (table) {
                            table->Delete(entry.key);
                            stats_.total_rows--;
                        } else {
                            spdlog::warn("Table '{}' not found during DELETE recovery", entry.table_name);
                        }
                        break;
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("Failed to recover WAL entry: type={}, table={}, key={}, error={}",
                            static_cast<int>(entry.type), entry.table_name, entry.key, e.what());
                // Continue with next entry instead of failing completely
                continue;
            }
        }

        spdlog::info("WAL recovery complete");
        return true;
    }

    StorageEngine::Stats GetStats() const {
        StorageEngine::Stats result = stats_;

        // Update dynamic stats
        std::shared_lock lock(tables_mutex_);
        result.total_bytes = 0;
        for (const auto& [_, table] : tables_) {
            result.total_bytes += table->GetSizeBytes();
        }

        if (wal_) {
            result.wal_size = wal_->GetSize();
        }

        return result;
    }

private:
    // Helper functions for serialization - implemented inline below

private:
    StorageConfig config_;

    mutable std::shared_mutex tables_mutex_;
    std::unordered_map<std::string, std::shared_ptr<MemTable>> tables_;

    mutable std::shared_mutex active_txns_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<TransactionImpl>> active_transactions_;

    std::unique_ptr<WALImpl> wal_;
    // std::unique_ptr<SnapshotManager> snapshot_manager_;

    std::atomic<uint64_t> next_txn_id_;
    std::atomic<uint64_t> next_timestamp_;

    mutable StorageEngine::Stats stats_;

    // Helper function implementations
    std::vector<uint8_t> SerializeSchema(const TableSchema& schema) {
        // Simple serialization - in production would use protobuf
        spdlog::debug("SerializeSchema: Starting serialization for table '{}'", schema.name);

        nlohmann::json j;
        j["name"] = schema.name;
        j["columns"] = nlohmann::json::array();

        spdlog::debug("SerializeSchema: Serializing {} columns", schema.columns.size());
        for (const auto& col : schema.columns) {
            nlohmann::json col_json;
            col_json["name"] = col.name;
            col_json["type"] = static_cast<int>(col.type);
            col_json["nullable"] = col.nullable;
            col_json["is_primary_key"] = col.is_primary_key;
            j["columns"].push_back(col_json);
            spdlog::debug("SerializeSchema: Added column '{}' type={}", col.name, static_cast<int>(col.type));
        }

        j["primary_key"] = schema.primary_key;

        std::string serialized = j.dump();
        spdlog::debug("SerializeSchema: Serialized schema to {} bytes: {}", serialized.size(), serialized);

        auto result = std::vector<uint8_t>(serialized.begin(), serialized.end());
        spdlog::debug("SerializeSchema: Returning vector of {} bytes", result.size());
        return result;
    }

    TableSchema DeserializeSchema(const std::vector<uint8_t>& data) {
        if (data.empty()) {
            spdlog::error("DeserializeSchema: Empty data provided");
            throw std::runtime_error("Cannot deserialize empty schema data");
        }

        std::string json_str(data.begin(), data.end());
        spdlog::debug("DeserializeSchema: Attempting to parse JSON: {}", json_str);

        if (json_str.empty()) {
            spdlog::error("DeserializeSchema: JSON string is empty");
            throw std::runtime_error("Cannot deserialize empty JSON string");
        }

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(json_str);
        } catch (const nlohmann::json::parse_error& e) {
            spdlog::error("DeserializeSchema: JSON parse error: {}", e.what());
            spdlog::error("DeserializeSchema: Failed JSON string: '{}'", json_str);
            throw std::runtime_error("Failed to parse schema JSON: " + std::string(e.what()));
        }

        TableSchema schema;
        schema.name = j["name"];
        schema.primary_key = j["primary_key"];

        for (const auto& col_json : j["columns"]) {
            Column col;
            col.name = col_json["name"];
            col.type = static_cast<DataType>(col_json["type"]);
            col.nullable = col_json["nullable"];
            col.is_primary_key = col_json["is_primary_key"];
            schema.columns.push_back(col);
        }

        return schema;
    }

    std::vector<uint8_t> SerializeRow(const Row& row) {
        // Simple serialization - in production would use protobuf
        spdlog::debug("SerializeRow: Starting serialization for key={}", row.key);

        nlohmann::json j;
        j["key"] = row.key;
        j["columns"] = nlohmann::json::object();

        spdlog::debug("SerializeRow: Processing {} columns", row.columns.size());

        for (const auto& [col_name, value] : row.columns) {
            spdlog::debug("SerializeRow: Processing column {}", col_name);
            nlohmann::json val_json;
            std::visit([&val_json, &col_name](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    val_json = nullptr;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    val_json = v;
                } else if constexpr (std::is_same_v<T, int32_t>) {
                    val_json = v;
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    val_json = v;
                } else if constexpr (std::is_same_v<T, float>) {
                    val_json = v;
                } else if constexpr (std::is_same_v<T, double>) {
                    val_json = v;
                } else if constexpr (std::is_same_v<T, bool>) {
                    val_json = v;
                } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                    // Store binary data as base64
                    val_json = nlohmann::json::binary(v);
                } else {
                    val_json = nullptr;
                }
            }, value);
            j["columns"][col_name] = val_json;
            spdlog::debug("SerializeRow: Completed column {}", col_name);
        }

        spdlog::debug("SerializeRow: Creating JSON string");
        std::string serialized = j.dump();
        spdlog::debug("SerializeRow: Serialization complete, {} bytes", serialized.size());
        return std::vector<uint8_t>(serialized.begin(), serialized.end());
    }

    Row DeserializeRow(const std::vector<uint8_t>& data) {
        if (data.empty()) {
            spdlog::error("DeserializeRow: Empty data provided");
            throw std::runtime_error("Cannot deserialize empty row data");
        }

        std::string json_str(data.begin(), data.end());
        spdlog::debug("DeserializeRow: Attempting to parse JSON: {}", json_str);

        if (json_str.empty()) {
            spdlog::error("DeserializeRow: JSON string is empty");
            throw std::runtime_error("Cannot deserialize empty JSON string");
        }

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(json_str);
        } catch (const nlohmann::json::parse_error& e) {
            spdlog::error("DeserializeRow: JSON parse error: {}", e.what());
            spdlog::error("DeserializeRow: Failed JSON string: '{}'", json_str);
            throw std::runtime_error("Failed to parse JSON: " + std::string(e.what()));
        }

        Row row;
        row.key = j["key"];

        for (const auto& [col_name, val_json] : j["columns"].items()) {
            if (val_json.is_null()) {
                row.columns[col_name] = std::monostate{};
            } else if (val_json.is_string()) {
                row.columns[col_name] = val_json.get<std::string>();
            } else if (val_json.is_number_integer()) {
                row.columns[col_name] = val_json.get<int64_t>();
            } else if (val_json.is_number_float()) {
                row.columns[col_name] = val_json.get<double>();
            } else if (val_json.is_boolean()) {
                row.columns[col_name] = val_json.get<bool>();
            } else if (val_json.is_binary()) {
                row.columns[col_name] = val_json.get_binary();
            }
        }

        return row;
    }

    friend class TransactionImpl;
};

// StorageEngine public interface implementation
StorageEngine::StorageEngine(const StorageConfig& config)
    : impl_(std::make_unique<Impl>(config)) {
}

StorageEngine::~StorageEngine() = default;

bool StorageEngine::Initialize() {
    return impl_->Initialize();
}

void StorageEngine::Shutdown() {
    impl_->Shutdown();
}

bool StorageEngine::CreateTable(const TableSchema& schema) {
    return impl_->CreateTable(schema);
}

bool StorageEngine::DropTable(const std::string& table_name) {
    return impl_->DropTable(table_name);
}

std::shared_ptr<Table> StorageEngine::GetTable(const std::string& table_name) {
    return impl_->GetTable(table_name);
}

std::vector<std::string> StorageEngine::ListTables() const {
    return impl_->ListTables();
}

std::shared_ptr<Transaction> StorageEngine::BeginTransaction(IsolationLevel level) {
    return impl_->BeginTransaction(level);
}

bool StorageEngine::Commit(std::shared_ptr<Transaction> txn) {
    auto txn_impl = std::dynamic_pointer_cast<TransactionImpl>(txn);
    return txn_impl ? impl_->Commit(txn_impl) : false;
}

bool StorageEngine::Rollback(std::shared_ptr<Transaction> txn) {
    auto txn_impl = std::dynamic_pointer_cast<TransactionImpl>(txn);
    return txn_impl ? impl_->Rollback(txn_impl) : false;
}

StorageEngine::Stats StorageEngine::GetStats() const {
    return impl_->GetStats();
}

// Direct operations
bool StorageEngine::Insert(const std::string& table, const Row& row) {
    spdlog::info("StorageEngine::Insert: Starting transaction for table={}, key={}", table, row.key);

    auto txn = BeginTransaction(IsolationLevel::READ_COMMITTED);
    spdlog::info("StorageEngine::Insert: Created transaction");

    if (!txn->Insert(table, row)) {
        spdlog::error("StorageEngine::Insert: Transaction insert failed");
        Rollback(txn);
        return false;
    }

    spdlog::info("StorageEngine::Insert: Transaction insert succeeded, committing");
    bool result = Commit(txn);
    spdlog::info("StorageEngine::Insert: Commit result={}", result);
    return result;
}

bool StorageEngine::Update(const std::string& table, const std::string& key, const Row& row) {
    auto txn = BeginTransaction(IsolationLevel::READ_COMMITTED);
    if (!txn->Update(table, key, row)) {
        Rollback(txn);
        return false;
    }
    return Commit(txn);
}

bool StorageEngine::Delete(const std::string& table, const std::string& key) {
    auto txn = BeginTransaction(IsolationLevel::READ_COMMITTED);
    if (!txn->Delete(table, key)) {
        Rollback(txn);
        return false;
    }
    return Commit(txn);
}

std::optional<Row> StorageEngine::Get(const std::string& table, const std::string& key) {
    auto txn = BeginTransaction(IsolationLevel::READ_COMMITTED);
    txn->SetReadOnly(true);
    return txn->Get(table, key);
}

void StorageEngine::Scan(const std::string& table, const std::string& start_key,
                         const std::string& end_key, ScanCallback callback) {
    auto txn = BeginTransaction(IsolationLevel::READ_COMMITTED);
    txn->SetReadOnly(true);
    txn->Scan(table, start_key, end_key, callback);
}

} // namespace instantdb