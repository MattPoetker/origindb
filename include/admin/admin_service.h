#pragma once

#include <memory>
#include <string>
#include <vector>

#include "proto/db.pb.h"

namespace instantdb {

class ModuleStore;
class RaftNode;
class StorageEngine;

// Admin service implementation
class AdminService {
public:
    AdminService(std::shared_ptr<ModuleStore> module_store,
                 std::shared_ptr<RaftNode> raft_node,
                 std::shared_ptr<StorageEngine> storage);
    ~AdminService();

    // Module management
    UploadModuleResponse UploadModule(const UploadModuleRequest& request);
    InstallModuleResponse InstallModule(const InstallModuleRequest& request);
    ListModulesResponse ListModules(const ListModulesRequest& request);
    RevokeModuleResponse RevokeModule(const RevokeModuleRequest& request);

    // Cluster management
    GetClusterStatusResponse GetClusterStatus(const GetClusterStatusRequest& request);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb