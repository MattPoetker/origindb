#include "storage/wal_impl.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace instantdb {

class WALImpl::Impl {
public:
    Impl(const std::string& wal_dir, const StorageConfig& config)
        : wal_dir_(wal_dir), config_(config), next_sequence_(1) {}

    bool Initialize() {
        std::filesystem::create_directories(wal_dir_);

        wal_file_path_ = wal_dir_ + "/wal.log";

        // Open WAL file for append
        wal_file_.open(wal_file_path_, std::ios::app | std::ios::binary);
        if (!wal_file_.is_open()) {
            spdlog::error("Failed to open WAL file: {}", wal_file_path_);
            return false;
        }

        // Recover sequence number from existing entries
        RecoverSequenceNumber();

        spdlog::info("WAL initialized at {} with next sequence {}",
                    wal_file_path_, next_sequence_);
        return true;
    }

    bool Append(const WALEntry& entry) {
        spdlog::debug("WALImpl::Append: type={}, table={}, key={}, data_size={}",
                     static_cast<int>(entry.type), entry.table_name, entry.key, entry.data.size());

        WALEntry timestamped_entry = entry;
        timestamped_entry.sequence = next_sequence_++;
        timestamped_entry.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        spdlog::debug("WALImpl::Append: timestamped entry - seq={}, data_size={}",
                     timestamped_entry.sequence, timestamped_entry.data.size());

        // Serialize entry to JSON (simplified for prototype)
        std::string json_entry = SerializeEntry(timestamped_entry);
        spdlog::debug("WALImpl::Append: serialized to {} bytes: {}", json_entry.size(),
                     json_entry.length() > 200 ? json_entry.substr(0, 200) + "..." : json_entry);

        // Write to file
        wal_file_ << json_entry << "\n";

        if (config_.sync_wal) {
            wal_file_.flush();
            // Note: std::ofstream doesn't have sync() on all platforms
            // In production, we'd use fsync() or platform-specific calls
        }

        entries_.push_back(timestamped_entry);

        spdlog::debug("WAL appended entry seq={}, type={}, table={}",
                     timestamped_entry.sequence,
                     static_cast<int>(timestamped_entry.type),
                     timestamped_entry.table_name);
        return true;
    }

    std::vector<WALEntry> ReadAll() {
        std::vector<WALEntry> result;

        std::ifstream file(wal_file_path_);
        if (!file.is_open()) {
            return result;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty()) {
                try {
                    auto entry = DeserializeEntry(line);
                    if (entry.sequence > 0) {
                        result.push_back(entry);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Skipping malformed WAL entry: {}", e.what());
                    // Continue reading other entries
                }
            }
        }

        spdlog::info("Read {} entries from WAL", result.size());
        return result;
    }

    bool Truncate(uint64_t sequence) {
        // For prototype, just mark where to truncate
        truncate_sequence_ = sequence;
        spdlog::info("WAL marked for truncation at sequence {}", sequence);
        return true;
    }

    void Flush() {
        if (wal_file_.is_open()) {
            wal_file_.flush();
        }
    }

    uint64_t GetLastSequence() const {
        return next_sequence_ - 1;
    }

    uint64_t GetSize() const {
        if (std::filesystem::exists(wal_file_path_)) {
            return std::filesystem::file_size(wal_file_path_);
        }
        return 0;
    }

private:
    void RecoverSequenceNumber() {
        auto entries = ReadAll();
        if (!entries.empty()) {
            next_sequence_ = entries.back().sequence + 1;
        }
        entries_ = entries;
    }

    std::string SerializeEntry(const WALEntry& entry) {
        // Simple JSON serialization for prototype
        std::string json = "{";
        json += "\"sequence\":" + std::to_string(entry.sequence) + ",";
        json += "\"type\":" + std::to_string(static_cast<int>(entry.type)) + ",";
        json += "\"transaction_id\":" + std::to_string(entry.transaction_id) + ",";
        json += "\"timestamp\":" + std::to_string(entry.timestamp) + ",";
        json += "\"table_name\":\"" + entry.table_name + "\",";
        json += "\"key\":\"" + entry.key + "\",";
        json += "\"data_size\":" + std::to_string(entry.data.size()) + ",";

        // Encode binary data as hex string
        json += "\"data\":\"";
        for (uint8_t byte : entry.data) {
            char hex[3];
            sprintf(hex, "%02x", byte);
            json += hex;
        }
        json += "\"";

        json += "}";
        return json;
    }

    WALEntry DeserializeEntry(const std::string& json) {
        WALEntry entry;
        // Simple JSON parsing for prototype
        // In production, use a proper JSON library

        // Validate JSON structure
        if (json.empty() || json.front() != '{' || json.back() != '}') {
            throw std::runtime_error("Invalid JSON format: " + json.substr(0, 50));
        }

        // Extract sequence
        auto seq_pos = json.find("\"sequence\":");
        if (seq_pos != std::string::npos) {
            auto start = seq_pos + 11;
            auto end = json.find(",", start);
            if (end == std::string::npos) {
                throw std::runtime_error("Malformed sequence field");
            }
            entry.sequence = std::stoull(json.substr(start, end - start));
        }

        // Extract type
        auto type_pos = json.find("\"type\":");
        if (type_pos != std::string::npos) {
            auto start = type_pos + 7;
            auto end = json.find(",", start);
            entry.type = static_cast<WALEntryType>(std::stoi(json.substr(start, end - start)));
        }

        // Extract transaction_id
        auto txn_pos = json.find("\"transaction_id\":");
        if (txn_pos != std::string::npos) {
            auto start = txn_pos + 17;
            auto end = json.find(",", start);
            entry.transaction_id = std::stoull(json.substr(start, end - start));
        }

        // Extract timestamp
        auto ts_pos = json.find("\"timestamp\":");
        if (ts_pos != std::string::npos) {
            auto start = ts_pos + 12;
            auto end = json.find(",", start);
            entry.timestamp = std::stoull(json.substr(start, end - start));
        }

        // Extract table_name
        auto table_pos = json.find("\"table_name\":\"");
        if (table_pos != std::string::npos) {
            auto start = table_pos + 14;
            auto end = json.find("\"", start);
            entry.table_name = json.substr(start, end - start);
        }

        // Extract key
        auto key_pos = json.find("\"key\":\"");
        if (key_pos != std::string::npos) {
            auto start = key_pos + 7;
            auto end = json.find("\"", start);
            entry.key = json.substr(start, end - start);
        }

        // Extract data (hex encoded)
        auto data_pos = json.find("\"data\":\"");
        if (data_pos != std::string::npos) {
            auto start = data_pos + 8;
            auto end = json.find("\"", start);
            std::string hex_data = json.substr(start, end - start);

            spdlog::debug("DeserializeEntry: found hex data field with {} chars", hex_data.length());

            // Decode hex string to binary data
            entry.data.clear();
            for (size_t i = 0; i < hex_data.length(); i += 2) {
                if (i + 1 < hex_data.length()) {
                    std::string hex_byte = hex_data.substr(i, 2);
                    uint8_t byte = static_cast<uint8_t>(std::stoi(hex_byte, nullptr, 16));
                    entry.data.push_back(byte);
                }
            }

            spdlog::debug("DeserializeEntry: decoded {} bytes of binary data", entry.data.size());
        } else {
            spdlog::debug("DeserializeEntry: no data field found in JSON");
        }

        return entry;
    }

private:
    std::string wal_dir_;
    std::string wal_file_path_;
    StorageConfig config_;
    std::ofstream wal_file_;
    std::atomic<uint64_t> next_sequence_;
    uint64_t truncate_sequence_ = 0;
    std::vector<WALEntry> entries_;
};

WALImpl::WALImpl(const std::string& wal_dir, const StorageConfig& config)
    : impl_(std::make_unique<Impl>(wal_dir, config)) {}

WALImpl::~WALImpl() = default;

bool WALImpl::Initialize() {
    return impl_->Initialize();
}

bool WALImpl::Append(const WALEntry& entry) {
    return impl_->Append(entry);
}

std::vector<WALEntry> WALImpl::ReadAll() {
    return impl_->ReadAll();
}

bool WALImpl::Truncate(uint64_t sequence) {
    return impl_->Truncate(sequence);
}

void WALImpl::Flush() {
    impl_->Flush();
}

uint64_t WALImpl::GetLastSequence() const {
    return impl_->GetLastSequence();
}

uint64_t WALImpl::GetSize() const {
    return impl_->GetSize();
}

} // namespace instantdb