#include "storage/mem_table.h"
#include <spdlog/spdlog.h>
#include <map>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>

namespace origindb {

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
        InvalidateJson(row.key);
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
        InvalidateJson(key);
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
        InvalidateJson(key);
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
        // Ordered container: seek to start_key and stop past end_key instead of
        // iterating the whole table (universal win for range/prefix scans).
        auto it = start_key.empty() ? rows_.begin() : rows_.lower_bound(start_key);
        for (; it != rows_.end(); ++it) {
            if (!end_key.empty() && it->first > end_key) break;
            if (!callback(it->first, it->second)) break;
        }
    }

    std::optional<std::string> GetJsonCached(const std::string& key,
                                             const JsonSerializer& ser) const {
        std::shared_lock lock(mutex_);
        auto it = rows_.find(key);
        if (it == rows_.end()) return std::nullopt;
        return CachedJson(it->first, it->second, ser);
    }

    void ScanJsonCached(const std::string& start_key, const std::string& end_key,
                        const JsonSerializer& ser, const JsonRowCallback& cb) const {
        std::shared_lock lock(mutex_);
        auto it = start_key.empty() ? rows_.begin() : rows_.lower_bound(start_key);
        for (; it != rows_.end(); ++it) {
            if (!end_key.empty() && it->first > end_key) break;
            if (!cb(it->first, CachedJson(it->first, it->second, ser))) break;
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
    // Return the row's cached value-JSON, computing + caching it on a miss.
    // Callers hold at least a shared lock on mutex_, so no write can invalidate
    // concurrently; the cache map itself is guarded by cache_mutex_ so parallel
    // readers filling the same entry don't race.
    const std::string& CachedJson(const std::string& key, const Row& row,
                                  const JsonSerializer& ser) const {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        auto it = json_cache_.find(key);
        if (it != json_cache_.end()) return it->second;
        return json_cache_.emplace(key, ser(row)).first->second;
    }

    void InvalidateJson(const std::string& key) {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        json_cache_.erase(key);
    }

    TableSchema schema_;
    mutable std::shared_mutex mutex_;
    std::map<std::string, Row> rows_;                 // ordered: efficient scans
    std::unordered_map<std::string, IndexSchema> indexes_;

    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<std::string, std::string> json_cache_;  // key -> value JSON
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

std::optional<std::string> MemTable::GetJsonCached(
    const std::string& key, const JsonSerializer& ser) const {
    return impl_->GetJsonCached(key, ser);
}

void MemTable::ScanJsonCached(const std::string& start_key, const std::string& end_key,
                              const JsonSerializer& ser,
                              const JsonRowCallback& cb) const {
    impl_->ScanJsonCached(start_key, end_key, ser, cb);
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

} // namespace origindb