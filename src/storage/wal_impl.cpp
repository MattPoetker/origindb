#include "storage/wal_impl.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace origindb {

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
        WALEntry timestamped_entry = entry;
        timestamped_entry.sequence = next_sequence_++;
        timestamped_entry.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string json_entry = SerializeEntry(timestamped_entry);
        wal_file_ << json_entry << "\n";

        if (config_.sync_wal) {
            wal_file_.flush();
            // Note: std::ofstream doesn't have sync() on all platforms
            // In production, we'd use fsync() or platform-specific calls
        }
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
        // Discards all entries with sequence <= `sequence`. Only whole-file
        // truncation is supported (used after a snapshot has captured the
        // full state); a mid-file sequence keeps everything.
        if (sequence < next_sequence_ - 1) {
            spdlog::warn("WAL partial truncation not supported (seq {} < last {})",
                        sequence, next_sequence_ - 1);
            return false;
        }
        wal_file_.close();
        wal_file_.open(wal_file_path_, std::ios::trunc | std::ios::binary);
        wal_file_.close();
        wal_file_.open(wal_file_path_, std::ios::app | std::ios::binary);
        spdlog::info("WAL truncated after sequence {}", sequence);
        return wal_file_.is_open();
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
    }

    std::string SerializeEntry(const WALEntry& entry) {
        // Same JSON-lines format as always (recovery-compatible); built in a
        // single preallocated buffer with table-based hex encoding.
        static const char* kHex = "0123456789abcdef";
        std::string json;
        json.reserve(160 + entry.table_name.size() + entry.key.size() +
                     entry.data.size() * 2);
        json += "{\"sequence\":";
        json += std::to_string(entry.sequence);
        json += ",\"type\":";
        json += std::to_string(static_cast<int>(entry.type));
        json += ",\"transaction_id\":";
        json += std::to_string(entry.transaction_id);
        json += ",\"timestamp\":";
        json += std::to_string(entry.timestamp);
        json += ",\"table_name\":\"";
        json += entry.table_name;
        json += "\",\"key\":\"";
        json += entry.key;
        json += "\",\"data_size\":";
        json += std::to_string(entry.data.size());
        json += ",\"data\":\"";
        for (uint8_t byte : entry.data) {
            json += kHex[byte >> 4];
            json += kHex[byte & 0xF];
        }
        json += "\"}";
        return json;
    }

    WALEntry DeserializeEntry(const std::string& json) {
        // Single forward scan; fields are searched from the previous match
        // position (the writer emits them in a fixed order) with a
        // full-string fallback for entries written by older builds.
        WALEntry entry;
        if (json.empty() || json.front() != '{' || json.back() != '}') {
            throw std::runtime_error("Invalid JSON format: " + json.substr(0, 50));
        }

        size_t cursor = 0;
        auto find_field = [&](const char* marker, size_t marker_len) -> size_t {
            size_t pos = json.find(marker, cursor);
            if (pos == std::string::npos) pos = json.find(marker);  // fallback
            if (pos == std::string::npos) return std::string::npos;
            cursor = pos + marker_len;
            return cursor;
        };

        auto parse_u64 = [&](const char* marker, size_t marker_len) -> uint64_t {
            size_t start = find_field(marker, marker_len);
            if (start == std::string::npos) return 0;
            uint64_t value = 0;
            size_t i = start;
            while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
                value = value * 10 + (json[i] - '0');
                i++;
            }
            cursor = i;
            return value;
        };

        auto parse_string = [&](const char* marker, size_t marker_len) -> std::string {
            size_t start = find_field(marker, marker_len);
            if (start == std::string::npos) return "";
            size_t end = json.find('"', start);
            if (end == std::string::npos)
                throw std::runtime_error("Malformed string field");
            cursor = end + 1;
            return json.substr(start, end - start);
        };

        entry.sequence = parse_u64("\"sequence\":", 11);
        entry.type = static_cast<WALEntryType>(parse_u64("\"type\":", 7));
        entry.transaction_id = parse_u64("\"transaction_id\":", 17);
        entry.timestamp = parse_u64("\"timestamp\":", 12);
        entry.table_name = parse_string("\"table_name\":\"", 14);
        entry.key = parse_string("\"key\":\"", 7);

        size_t data_start = find_field("\"data\":\"", 8);
        if (data_start != std::string::npos) {
            size_t data_end = json.find('"', data_start);
            if (data_end == std::string::npos)
                throw std::runtime_error("Malformed data field");

            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            size_t len = data_end - data_start;
            entry.data.reserve(len / 2);
            for (size_t i = 0; i + 1 < len; i += 2) {
                int hi = nibble(json[data_start + i]);
                int lo = nibble(json[data_start + i + 1]);
                if (hi < 0 || lo < 0) throw std::runtime_error("Invalid hex in data");
                entry.data.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
        }

        return entry;
    }

private:
    std::string wal_dir_;
    std::string wal_file_path_;
    StorageConfig config_;
    std::ofstream wal_file_;
    std::atomic<uint64_t> next_sequence_;
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

} // namespace origindb