#include "common/config.h"
#include <iostream>
#include <fstream>
#include <cstring>

namespace instantdb {

bool ParseCommandLine(int argc, char* argv[], ServerConfig& config) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--config" && i + 1 < argc) {
            return LoadConfigFile(argv[++i], config);
        } else if (arg == "--node-id" && i + 1 < argc) {
            config.node_id = argv[++i];
            config.raft.node_id = config.node_id;
        } else if (arg == "--data-dir" && i + 1 < argc) {
            config.storage.data_dir = argv[++i];
            config.raft.log_dir = config.storage.data_dir + "/raft";
        } else if (arg == "--grpc-addr" && i + 1 < argc) {
            config.grpc.listen_address = argv[++i];
        } else if (arg == "--ws-addr" && i + 1 < argc) {
            config.websocket.listen_address = argv[++i];
        } else if (arg == "--join" && i + 1 < argc) {
            config.raft.peer_addresses.push_back(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            return false;
        }
    }

    // Set defaults if not specified
    if (config.node_id.empty()) {
        config.node_id = "node-" + std::to_string(std::time(nullptr));
        config.raft.node_id = config.node_id;
    }

    if (config.raft.log_dir.empty()) {
        config.raft.log_dir = config.storage.data_dir + "/raft";
    }

    config.modules.module_dir = config.storage.data_dir + "/modules";

    return true;
}

bool LoadConfigFile(const std::string& path, ServerConfig& config) {
    // TODO: Implement YAML or JSON config file parsing
    // For now, just return true with defaults
    return true;
}

} // namespace instantdb