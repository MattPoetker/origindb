#include "storage/mem_table.h"
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <shared_mutex>

namespace instantdb {

class MemTable::Impl {
public:
    explicit Impl(const TableSchema& schema) : schema_(schema) {}

    const TableSchema& GetSchema() const { return schema_; }

    uint64_t GetRowCount() const {
        std::shared_lock lock(mutex_);
        return rows_.size();
    }

    uint64_t GetSizeBytes() const {
        std::shared_lock lock(mutex_);
        uint64_t size = 0;
        for (const auto& [key, row] : rows_) {
            size += key.size() + sizeof(row.version) + sizeof(row.timestamp);
            for (const auto& [col_name, value] : row.columns) {
                size += col_name.size();
                std::visit([&size](const auto& v) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                        size += v.size();
                    } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::vector<uint8_t>>) {
                        size += v.size();
                    } else {
                        size += sizeof(v);
                    }
                }, value);
            }
        }
        return size;
    }

    bool Insert(const Row& row) {
        std::unique_lock lock(mutex_);

        if (rows_.find(row.key) != rows_.end()) {
            spdlog::warn("Row with key {} already exists", row.key);
            return false;
        }

        rows_[row.key] = row;
        spdlog::debug("Inserted row with key {} into table {}", row.key, schema_.name);
        return true;
    }

    bool Update(const std::string& key, const Row& row) {
        std::unique_lock lock(mutex_);

        auto it = rows_.find(key);
        if (it == rows_.end()) {
            spdlog::warn("Row with key {} not found for update", key);
            return false;
        }

        // Update the row, incrementing version
        Row updated_row = row;
        updated_row.version = it->second.version + 1;
        updated_row.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        rows_[key] = updated_row;
        spdlog::debug("Updated row with key {} in table {}", key, schema_.name);
        return true;
    }

    bool Delete(const std::string& key) {
        std::unique_lock lock(mutex_);

        auto it = rows_.find(key);
        if (it == rows_.end()) {
            spdlog::warn("Row with key {} not found for delete", key);
            return false;
        }

        rows_.erase(it);
        spdlog::debug("Deleted row with key {} from table {}", key, schema_.name);
        return true;
    }

    std::optional<Row> Get(const std::string& key) const {
        std::shared_lock lock(mutex_);

        auto it = rows_.find(key);
        if (it != rows_.end()) {
            return it->second;
        }

        return std::nullopt;
    }

    void Scan(const std::string& start_key, const std::string& end_key,
              ScanCallback callback) const {
        std::shared_lock lock(mutex_);

        for (const auto& [key, row] : rows_) {
            if (key >= start_key && (end_key.empty() || key <= end_key)) {
                if (!callback(key, row)) {
                    break; // Callback returned false, stop scanning
                }
            }
        }
    }

    bool CreateIndex(const IndexSchema& schema) {
        // TODO: Implement index creation
        indexes_[schema.name] = schema;
        spdlog::info("Created index {} on table {}", schema.name, schema_.name);
        return true;
    }

    bool DropIndex(const std::string& index_name) {
        auto it = indexes_.find(index_name);
        if (it != indexes_.end()) {
            indexes_.erase(it);
            spdlog::info("Dropped index {} on table {}", index_name, schema_.name);
            return true;
        }
        return false;
    }

    std::vector<std::string> GetIndexNames() const {
        std::vector<std::string> names;
        for (const auto& [name, _] : indexes_) {
            names.push_back(name);
        }
        return names;
    }

private:
    TableSchema schema_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Row> rows_;
    std::unordered_map<std::string, IndexSchema> indexes_;
};

MemTable::MemTable(const TableSchema& schema)
    : impl_(std::make_unique<Impl>(schema)) {}

MemTable::~MemTable() = default;

const TableSchema& MemTable::GetSchema() const {
    return impl_->GetSchema();
}

uint64_t MemTable::GetRowCount() const {
    return impl_->GetRowCount();
}

uint64_t MemTable::GetSizeBytes() const {
    return impl_->GetSizeBytes();
}

bool MemTable::Insert(const Row& row) {
    return impl_->Insert(row);
}

bool MemTable::Update(const std::string& key, const Row& row) {
    return impl_->Update(key, row);
}

bool MemTable::Delete(const std::string& key) {
    return impl_->Delete(key);
}

std::optional<Row> MemTable::Get(const std::string& key) const {
    return impl_->Get(key);
}

void MemTable::Scan(const std::string& start_key, const std::string& end_key,
                    ScanCallback callback) const {
    impl_->Scan(start_key, end_key, callback);
}

bool MemTable::CreateIndex(const IndexSchema& schema) {
    return impl_->CreateIndex(schema);
}

bool MemTable::DropIndex(const std::string& index_name) {
    return impl_->DropIndex(index_name);
}

std::vector<std::string> MemTable::GetIndexNames() const {
    return impl_->GetIndexNames();
}

} // namespace instantdb