#pragma once

#include "storage/storage_engine.h"
#include <vector>

namespace origindb {

enum class WriteOp {
    INSERT,
    UPDATE,
    DELETE
};

struct WriteOperation {
    WriteOp op;
    std::string table_name;
    Row row;
};

class TransactionImpl : public Transaction {
public:
    enum class State {
        ACTIVE,
        COMMITTED,
        ABORTED
    };

    TransactionImpl(uint64_t id, uint64_t timestamp,
                   IsolationLevel level, void* engine);

    // Transaction interface
    std::string GetId() const override;
    IsolationLevel GetIsolationLevel() const override;
    uint64_t GetTimestamp() const override;

    bool Insert(const std::string& table, const Row& row) override;
    bool Update(const std::string& table, const std::string& key, const Row& row) override;
    bool Delete(const std::string& table, const std::string& key) override;
    std::optional<Row> Get(const std::string& table, const std::string& key) override;

    void Scan(const std::string& table, const std::string& start_key,
             const std::string& end_key, ScanCallback callback) override;

    bool IsActive() const override;
    bool IsReadOnly() const override;
    void SetReadOnly(bool read_only) override;

    // Internal methods
    std::vector<WriteOperation> GetWriteSet() const;
    void SetCommitted();
    void SetAborted();

private:
    uint64_t id_;
    uint64_t timestamp_;
    IsolationLevel level_;
    void* engine_;
    State state_;
    bool read_only_;
    std::vector<WriteOperation> write_set_;
};

} // namespace origindb