#pragma once

#include "storage/wal.h"
#include "common/config.h"
#include <memory>

namespace instantdb {

// Simple WAL implementation for prototype
class WALImpl : public WAL {
public:
    WALImpl(const std::string& wal_dir, const StorageConfig& config);
    ~WALImpl() override;

    bool Initialize() override;
    bool Append(const WALEntry& entry) override;
    std::vector<WALEntry> ReadAll() override;
    bool Truncate(uint64_t sequence) override;
    void Flush() override;
    uint64_t GetLastSequence() const override;
    uint64_t GetSize() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb