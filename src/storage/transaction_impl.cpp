#include "storage/transaction_impl.h"
#include "storage/storage_engine.h"
#include <spdlog/spdlog.h>

namespace instantdb {

TransactionImpl::TransactionImpl(uint64_t id, uint64_t timestamp,
                                IsolationLevel level, void* engine)
    : id_(id), timestamp_(timestamp), level_(level), engine_(engine),
      state_(State::ACTIVE), read_only_(false) {}

std::string TransactionImpl::GetId() const {
    return "txn-" + std::to_string(id_);
}

IsolationLevel TransactionImpl::GetIsolationLevel() const {
    return level_;
}

uint64_t TransactionImpl::GetTimestamp() const {
    return timestamp_;
}

bool TransactionImpl::Insert(const std::string& table, const Row& row) {
    if (state_ != State::ACTIVE) {
        return false;
    }

    WriteOperation op;
    op.op = WriteOp::INSERT;
    op.table_name = table;
    op.row = row;
    write_set_.push_back(op);

    spdlog::debug("Transaction {} staged INSERT into {}", GetId(), table);
    return true;
}

bool TransactionImpl::Update(const std::string& table, const std::string& key, const Row& row) {
    if (state_ != State::ACTIVE) {
        return false;
    }

    WriteOperation op;
    op.op = WriteOp::UPDATE;
    op.table_name = table;
    op.row = row;
    op.row.key = key;
    write_set_.push_back(op);

    spdlog::debug("Transaction {} staged UPDATE on {} key {}", GetId(), table, key);
    return true;
}

bool TransactionImpl::Delete(const std::string& table, const std::string& key) {
    if (state_ != State::ACTIVE) {
        return false;
    }

    WriteOperation op;
    op.op = WriteOp::DELETE;
    op.table_name = table;
    op.row.key = key;
    write_set_.push_back(op);

    spdlog::debug("Transaction {} staged DELETE from {} key {}", GetId(), table, key);
    return true;
}

std::optional<Row> TransactionImpl::Get(const std::string& table, const std::string& key) {
    if (state_ != State::ACTIVE) {
        return std::nullopt;
    }

    // Check write set first for read-your-writes consistency
    for (const auto& write : write_set_) {
        if (write.table_name == table && write.row.key == key) {
            if (write.op == WriteOp::DELETE) {
                return std::nullopt; // Row was deleted in this transaction
            } else {
                return write.row; // Return modified row
            }
        }
    }

    // For prototype, we'll need to access storage through another method
    // TODO: Implement proper storage access
    return std::nullopt;
}

void TransactionImpl::Scan(const std::string& table, const std::string& start_key,
                          const std::string& end_key, ScanCallback callback) {
    if (state_ != State::ACTIVE) {
        return;
    }

    // TODO: Implement scan for prototype
    spdlog::debug("Scan not fully implemented in prototype");
}

bool TransactionImpl::IsActive() const {
    return state_ == State::ACTIVE;
}

bool TransactionImpl::IsReadOnly() const {
    return read_only_;
}

void TransactionImpl::SetReadOnly(bool read_only) {
    read_only_ = read_only;
}

std::vector<WriteOperation> TransactionImpl::GetWriteSet() const {
    return write_set_;
}

void TransactionImpl::SetCommitted() {
    state_ = State::COMMITTED;
}

void TransactionImpl::SetAborted() {
    state_ = State::ABORTED;
}

} // namespace instantdb