#pragma once

#include "storage/table.h"
#include <memory>

namespace instantdb {

// In-memory table implementation
class MemTable : public Table {
public:
    explicit MemTable(const TableSchema& schema);
    ~MemTable() override;

    // Table interface
    const TableSchema& GetSchema() const override;
    uint64_t GetRowCount() const override;
    uint64_t GetSizeBytes() const override;

    bool Insert(const Row& row) override;
    bool Update(const std::string& key, const Row& row) override;
    bool Delete(const std::string& key) override;
    std::optional<Row> Get(const std::string& key) const override;

    void Scan(const std::string& start_key, const std::string& end_key,
              ScanCallback callback) const override;

    bool CreateIndex(const IndexSchema& schema) override;
    bool DropIndex(const std::string& index_name) override;
    std::vector<std::string> GetIndexNames() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb