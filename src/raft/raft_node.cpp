#include "raft/raft_node.h"
#include "storage/storage_engine.h"
#include "modules/module_store.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <mutex>

namespace instantdb {

class RaftNode::Impl {
public:
    Impl(const RaftConfig& config,
         std::shared_ptr<StorageEngine> storage,
         std::shared_ptr<ModuleStore> modules)
        : config_(config), storage_(storage), modules_(modules),
          is_leader_(true), // Start as leader for prototype
          current_term_(1),
          commit_index_(0) {}

    bool Initialize() {
        spdlog::info("Raft node {} initializing", config_.node_id);

        // For prototype, we start as leader immediately
        // In real implementation, this would initialize NuRaft
        is_leader_ = true;
        current_term_ = 1;

        spdlog::info("Raft node {} initialized as leader (prototype mode)",
                    config_.node_id);
        return true;
    }

    bool Start() {
        spdlog::info("Starting Raft node {}", config_.node_id);

        // In prototype mode, we're always ready
        if (state_change_callback_) {
            state_change_callback_(is_leader_);
        }

        spdlog::info("Raft node {} started", config_.node_id);
        return true;
    }

    void Stop() {
        spdlog::info("Stopping Raft node {}", config_.node_id);
        // Cleanup would go here
    }

    bool AppendEntry(const internal::RaftEntry& entry) {
        std::lock_guard<std::mutex> lock(entries_mutex_);

        // For prototype, directly apply entry
        spdlog::debug("Raft appending entry of type {} for term {}",
                     static_cast<int>(entry.payload_case()),
                     entry.term());

        // Assign index
        internal::RaftEntry indexed_entry = entry;
        indexed_entry.set_index(commit_index_ + 1);
        indexed_entry.set_term(current_term_);

        // Store the entry
        log_entries_.push_back(indexed_entry);

        // In prototype, immediately apply
        ApplyEntry(indexed_entry);

        commit_index_++;

        spdlog::debug("Raft entry applied, commit_index now {}",
                     commit_index_);

        return true;
    }

    bool IsLeader() const {
        return is_leader_.load();
    }

    std::string GetLeaderId() const {
        return is_leader_ ? config_.node_id : "";
    }

    uint64_t GetCurrentTerm() const {
        return current_term_.load();
    }

    uint64_t GetCommitIndex() const {
        return commit_index_.load();
    }

    bool AddNode(const std::string& node_id, const std::string& address) {
        spdlog::info("Adding node {} at {}", node_id, address);
        // TODO: Implement cluster membership changes
        return true;
    }

    bool RemoveNode(const std::string& node_id) {
        spdlog::info("Removing node {}", node_id);
        // TODO: Implement cluster membership changes
        return true;
    }

    std::vector<std::string> GetClusterNodes() const {
        // For prototype, return just ourselves
        return {config_.node_id};
    }

    void OnStateChange(RaftStateCallback callback) {
        state_change_callback_ = callback;
    }

    bool CreateSnapshot() {
        spdlog::info("Creating snapshot at index {}", commit_index_);
        // TODO: Implement snapshot creation
        return true;
    }

    uint64_t GetLastSnapshotIndex() const {
        return 0; // No snapshots in prototype
    }

private:
    void ApplyEntry(const internal::RaftEntry& entry) {
        // Apply the entry to the storage engine
        switch (entry.payload_case()) {
            case internal::RaftEntry::kModuleInvocation:
                ApplyModuleInvocation(entry.module_invocation());
                break;

            case internal::RaftEntry::kDdl:
                ApplyDDL(entry.ddl());
                break;

            case internal::RaftEntry::kDml:
                ApplyDML(entry.dml());
                break;

            case internal::RaftEntry::kSnapshot:
                spdlog::debug("Snapshot marker applied");
                break;

            case internal::RaftEntry::kConfigChange:
                ApplyConfigChange(entry.config_change());
                break;

            default:
                spdlog::warn("Unknown entry type in Raft log");
                break;
        }
    }

    void ApplyModuleInvocation(const internal::ModuleInvocation& invocation) {
        spdlog::debug("Applying module invocation: {}:{}",
                     invocation.module_id(), invocation.module_version());

        // Apply staged writes
        for (const auto& write : invocation.staged_writes()) {
            ApplyTableWrite(write);
        }

        // Emit events
        for (const auto& event : invocation.emitted_events()) {
            EmitEvent(event);
        }
    }

    void ApplyDDL(const internal::DDLCommand& ddl) {
        switch (ddl.command_case()) {
            case internal::DDLCommand::kCreateTable:
                ApplyCreateTable(ddl.create_table());
                break;

            case internal::DDLCommand::kDropTable:
                ApplyDropTable(ddl.drop_table());
                break;

            default:
                spdlog::warn("Unsupported DDL command");
                break;
        }
    }

    void ApplyDML(const internal::DMLCommand& dml) {
        spdlog::debug("Applying DML on table {}", dml.table_name());
        // TODO: Apply DML commands
    }

    void ApplyConfigChange(const internal::ConfigChange& change) {
        spdlog::debug("Applying config change");
        // TODO: Apply cluster configuration changes
    }

    void ApplyTableWrite(const internal::TableWrite& write) {
        Row row;
        row.key = std::string(write.key().begin(), write.key().end());

        // For prototype, assume simple value deserialization
        if (!write.new_value().empty()) {
            // Simple deserialization - in production use proper serialization
            std::string value_str(write.new_value().begin(), write.new_value().end());
            // Parse as JSON or structured data
            row.columns["data"] = value_str;
        }

        switch (write.operation()) {
            case internal::TableOperation::TABLE_INSERT:
                storage_->Insert(write.table_name(), row);
                break;

            case internal::TableOperation::TABLE_UPDATE:
                storage_->Update(write.table_name(), row.key, row);
                break;

            case internal::TableOperation::TABLE_DELETE:
                storage_->Delete(write.table_name(), row.key);
                break;

            case internal::TableOperation::TABLE_UPSERT:
                // Try update first, then insert if not found
                if (!storage_->Update(write.table_name(), row.key, row)) {
                    storage_->Insert(write.table_name(), row);
                }
                break;
        }
    }

    void ApplyCreateTable(const internal::CreateTable& create_table) {
        TableSchema schema;
        schema.name = create_table.table_name();

        for (const auto& col_def : create_table.columns()) {
            Column col;
            col.name = col_def.name();
            col.type = ConvertDataType(col_def.type());
            col.nullable = col_def.nullable();
            schema.columns.push_back(col);
        }

        for (const auto& pk_col : create_table.primary_key()) {
            schema.primary_key.push_back(pk_col);
        }

        storage_->CreateTable(schema);
    }

    void ApplyDropTable(const internal::DropTable& drop_table) {
        storage_->DropTable(drop_table.table_name());
    }

    void EmitEvent(const internal::EmittedEvent& event) {
        spdlog::debug("Emitting event to topic {} with sequence {}",
                     event.topic(), event.sequence_in_tx());
        // TODO: Connect to changefeed engine
    }

    DataType ConvertDataType(internal::DataType proto_type) {
        switch (proto_type) {
            case internal::DataType::INT32: return DataType::INT32;
            case internal::DataType::INT64: return DataType::INT64;
            case internal::DataType::FLOAT: return DataType::FLOAT;
            case internal::DataType::DOUBLE: return DataType::DOUBLE;
            case internal::DataType::STRING: return DataType::STRING;
            case internal::DataType::BYTES: return DataType::BYTES;
            case internal::DataType::BOOLEAN: return DataType::BOOLEAN;
            case internal::DataType::TIMESTAMP: return DataType::TIMESTAMP;
            case internal::DataType::JSON: return DataType::JSON;
            default: return DataType::STRING;
        }
    }

private:
    RaftConfig config_;
    std::shared_ptr<StorageEngine> storage_;
    std::shared_ptr<ModuleStore> modules_;

    std::atomic<bool> is_leader_;
    std::atomic<uint64_t> current_term_;
    std::atomic<uint64_t> commit_index_;

    std::mutex entries_mutex_;
    std::vector<internal::RaftEntry> log_entries_;

    RaftStateCallback state_change_callback_;
};

RaftNode::RaftNode(const RaftConfig& config,
                   std::shared_ptr<StorageEngine> storage,
                   std::shared_ptr<ModuleStore> modules)
    : impl_(std::make_unique<Impl>(config, storage, modules)) {}

RaftNode::~RaftNode() = default;

bool RaftNode::Initialize() {
    return impl_->Initialize();
}

bool RaftNode::Start() {
    return impl_->Start();
}

void RaftNode::Stop() {
    impl_->Stop();
}

bool RaftNode::AppendEntry(const internal::RaftEntry& entry) {
    return impl_->AppendEntry(entry);
}

bool RaftNode::IsLeader() const {
    return impl_->IsLeader();
}

std::string RaftNode::GetLeaderId() const {
    return impl_->GetLeaderId();
}

uint64_t RaftNode::GetCurrentTerm() const {
    return impl_->GetCurrentTerm();
}

uint64_t RaftNode::GetCommitIndex() const {
    return impl_->GetCommitIndex();
}

bool RaftNode::AddNode(const std::string& node_id, const std::string& address) {
    return impl_->AddNode(node_id, address);
}

bool RaftNode::RemoveNode(const std::string& node_id) {
    return impl_->RemoveNode(node_id);
}

std::vector<std::string> RaftNode::GetClusterNodes() const {
    return impl_->GetClusterNodes();
}

void RaftNode::OnStateChange(RaftStateCallback callback) {
    impl_->OnStateChange(callback);
}

bool RaftNode::CreateSnapshot() {
    return impl_->CreateSnapshot();
}

uint64_t RaftNode::GetLastSnapshotIndex() const {
    return impl_->GetLastSnapshotIndex();
}

} // namespace instantdb