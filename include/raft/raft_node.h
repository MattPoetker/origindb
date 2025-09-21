#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "common/config.h"
#include "proto/raft_internal.pb.h"

namespace nuraft {
    class raft_server;
    class state_machine;
    class state_mgr;
}

namespace instantdb {

class StorageEngine;
class ModuleStore;

// Callback for Raft state changes
using RaftStateCallback = std::function<void(bool is_leader)>;

// Raft node wrapper
class RaftNode {
public:
    RaftNode(const RaftConfig& config,
             std::shared_ptr<StorageEngine> storage,
             std::shared_ptr<ModuleStore> modules);
    ~RaftNode();

    bool Initialize();
    bool Start();
    void Stop();

    // Raft operations
    bool AppendEntry(const internal::RaftEntry& entry);
    bool IsLeader() const;
    std::string GetLeaderId() const;
    uint64_t GetCurrentTerm() const;
    uint64_t GetCommitIndex() const;

    // Cluster management
    bool AddNode(const std::string& node_id, const std::string& address);
    bool RemoveNode(const std::string& node_id);
    std::vector<std::string> GetClusterNodes() const;

    // Callbacks
    void OnStateChange(RaftStateCallback callback);

    // Snapshot operations
    bool CreateSnapshot();
    uint64_t GetLastSnapshotIndex() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb