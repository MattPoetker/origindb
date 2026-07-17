#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>

namespace origindb {

class Table;

struct SnapshotMetadata {
    uint64_t snapshot_id;
    uint64_t raft_index;
    uint64_t raft_term;
    std::chrono::system_clock::time_point created_at;
    std::vector<std::string> table_names;
    uint64_t total_size_bytes;
};

class Snapshot {
public:
    virtual ~Snapshot() = default;

    virtual const SnapshotMetadata& GetMetadata() const = 0;
    virtual bool AddTable(const std::string& name, std::shared_ptr<Table> table) = 0;
    virtual std::shared_ptr<Table> GetTable(const std::string& name) const = 0;
    virtual bool Save(const std::string& path) = 0;
    virtual bool Load(const std::string& path) = 0;
};

class SnapshotManager {
public:
    SnapshotManager(const std::string& snapshot_dir,
                   const struct StorageConfig& config);
    ~SnapshotManager();

    std::shared_ptr<Snapshot> CreateSnapshot(uint64_t raft_index, uint64_t raft_term);
    std::shared_ptr<Snapshot> LoadSnapshot(const std::string& path);
    std::vector<SnapshotMetadata> ListSnapshots() const;
    bool DeleteSnapshot(uint64_t snapshot_id);
    bool CleanupOldSnapshots(size_t keep_count);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace origindb