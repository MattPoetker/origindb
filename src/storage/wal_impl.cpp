#include "storage/wal_impl.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace origindb {

// A durable WAL with a dedicated writer thread and opportunistic group commit.
//
// Committers call AppendBatchAsync while holding the engine commit lock: that
// assigns in-order sequence numbers, serializes the entries, and enqueues one
// PendingBatch. They then release the lock and wait on the returned future.
// The single writer thread drains everything queued, writes it in one pass,
// issues ONE fdatasync (Fsync mode) for the whole drain, and fulfils every
// waiter. Under load, many commits coalesce behind a single fsync — effective
// throughput is batch_size / fsync_latency instead of 1 / fsync_latency.
class WALImpl::Impl {
public:
    Impl(const std::string& wal_dir, const StorageConfig& config)
        : wal_dir_(wal_dir), config_(config), next_sequence_(1) {
        // Resolve durability level. sync_mode is the authority; the legacy
        // sync_wal bool only acts as an opt-out: sync_wal=false downgrades an
        // fsync default to flush (its historical "don't force the disk" intent).
        sync_mode_ = config.sync_mode;
        if (!config.sync_wal && sync_mode_ == SyncMode::Fsync) {
            sync_mode_ = SyncMode::Flush;
        }
    }

    ~Impl() {
        StopWriter();
        if (fd_ >= 0) ::close(fd_);
    }

    bool Initialize() {
        std::filesystem::create_directories(wal_dir_);
        wal_file_path_ = wal_dir_ + "/wal.log";

        fd_ = ::open(wal_file_path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd_ < 0) {
            spdlog::error("Failed to open WAL file {}: {}", wal_file_path_,
                          std::strerror(errno));
            return false;
        }

        RecoverSequenceNumber();

        stop_ = false;
        writer_thread_ = std::thread([this] { WriterLoop(); });

        spdlog::info("WAL initialized at {} (next seq {}, sync_mode={})",
                     wal_file_path_, next_sequence_.load(),
                     SyncModeName(sync_mode_));
        return true;
    }

    // Single-entry durable append (used for rare metadata ops: CREATE/DROP
    // table). Goes through the same batch machinery and blocks until durable.
    bool Append(const WALEntry& entry) {
        std::vector<WALEntry> one{entry};
        auto fut = AppendBatchAsync(one);
        return fut.get();
    }

    std::future<bool> AppendBatchAsync(std::vector<WALEntry>& entries) {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();

        // Assign sequence numbers + build the serialized blob, then enqueue,
        // all under enqueue_mutex_ so file order == sequence order regardless
        // of how many committers race here.
        std::lock_guard<std::mutex> lk(queue_mutex_);
        if (fatal_) {
            // Durability is compromised; refuse further writes rather than
            // silently accept data we can no longer persist safely.
            promise->set_value(false);
            return future;
        }

        std::string blob;
        blob.reserve(160 * entries.size());
        uint64_t ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        for (auto& e : entries) {
            e.sequence = next_sequence_++;
            e.timestamp = ts;
            blob += SerializeEntry(e);
            blob += '\n';
        }

        queue_.push_back(PendingBatch{std::move(blob), std::move(promise)});
        queue_cv_.notify_one();
        return future;
    }

    std::vector<WALEntry> ReadAll() {
        std::vector<WALEntry> result;
        std::ifstream file(wal_file_path_);
        if (!file.is_open()) return result;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            try {
                auto entry = DeserializeEntry(line);
                if (entry.sequence > 0) result.push_back(entry);
            } catch (const std::exception& e) {
                spdlog::warn("Skipping malformed WAL entry: {}", e.what());
            }
        }
        spdlog::info("Read {} entries from WAL", result.size());
        return result;
    }

    bool Truncate(uint64_t sequence) {
        // Whole-file truncation only (used after a snapshot captured the full
        // state). Drain the writer first so nothing is in flight, then reset
        // the file under file_mutex_.
        if (sequence < next_sequence_ - 1) {
            spdlog::warn("WAL partial truncation not supported (seq {} < last {})",
                         sequence, next_sequence_ - 1);
            return false;
        }
        Drain();
        std::lock_guard<std::mutex> flk(file_mutex_);
        if (fd_ >= 0) ::close(fd_);
        // Truncate by reopening with O_TRUNC, then reopen for append.
        int t = ::open(wal_file_path_.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644);
        if (t >= 0) ::close(t);
        fd_ = ::open(wal_file_path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd_ < 0) {
            spdlog::error("Failed to reopen WAL after truncate: {}",
                          std::strerror(errno));
            return false;
        }
        spdlog::info("WAL truncated after sequence {}", sequence);
        return true;
    }

    // Block until every queued batch is written and synced.
    void Flush() { Drain(); }

    uint64_t GetLastSequence() const { return next_sequence_ - 1; }

    uint64_t GetSize() const {
        if (std::filesystem::exists(wal_file_path_)) {
            return std::filesystem::file_size(wal_file_path_);
        }
        return 0;
    }

private:
    struct PendingBatch {
        std::string bytes;
        std::shared_ptr<std::promise<bool>> done;
    };

    static const char* SyncModeName(SyncMode m) {
        switch (m) {
            case SyncMode::None:  return "none";
            case SyncMode::Flush: return "flush";
            case SyncMode::Fsync: return "fsync";
        }
        return "?";
    }

    void WriterLoop() {
        while (true) {
            std::deque<PendingBatch> drained;
            {
                std::unique_lock<std::mutex> lk(queue_mutex_);
                queue_cv_.wait(lk, [&] { return !queue_.empty() || stop_; });
                if (queue_.empty() && stop_) break;

                // Opportunistic enlargement: on very fast storage a tiny window
                // lets more commits pile in behind the in-flight fsync. Default
                // 0 = pure opportunistic (no artificial delay).
                if (config_.group_commit_window_us > 0) {
                    queue_cv_.wait_for(
                        lk, std::chrono::microseconds(config_.group_commit_window_us),
                        [&] { return stop_; });
                }
                drained.swap(queue_);
            }

            std::string blob;
            for (auto& p : drained) blob += p.bytes;

            bool ok = WriteAndSync(blob);
            for (auto& p : drained) p.done->set_value(ok);

            if (!ok) {
                // Durability integrity is gone; refuse further writes. Prefer a
                // hard stop over silent divergence on a single node.
                std::lock_guard<std::mutex> lk(queue_mutex_);
                fatal_ = true;
                spdlog::critical("WAL write/sync failed — refusing further writes. "
                                 "Restart required (recovery from last-good WAL).");
            }
        }
    }

    bool WriteAndSync(const std::string& blob) {
        std::lock_guard<std::mutex> flk(file_mutex_);
        if (fd_ < 0) return false;

        const char* p = blob.data();
        size_t remaining = blob.size();
        while (remaining > 0) {
            ssize_t n = ::write(fd_, p, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                spdlog::error("WAL write failed: {}", std::strerror(errno));
                return false;
            }
            p += n;
            remaining -= static_cast<size_t>(n);
        }

        // None/Flush: data is already in the OS page cache once write() returns
        // (raw fd, no userspace buffer), so both survive a process crash. Only
        // Fsync forces the drive and survives power loss.
        if (sync_mode_ == SyncMode::Fsync) {
            return SyncFd();
        }
        return true;
    }

    bool SyncFd() {
#if defined(__APPLE__)
        // On macOS plain fsync does NOT flush the drive's own cache; F_FULLFSYNC
        // does. Fall back to fsync if the device rejects F_FULLFSYNC.
        if (::fcntl(fd_, F_FULLFSYNC) == 0) return true;
        if (::fsync(fd_) == 0) return true;
        spdlog::error("WAL F_FULLFSYNC/fsync failed: {}", std::strerror(errno));
        return false;
#elif defined(__linux__)
        // fdatasync: WAL is append-only, so we need data + size persisted but
        // not unrelated inode metadata (mtime) on every commit.
        if (::fdatasync(fd_) == 0) return true;
        spdlog::error("WAL fdatasync failed: {}", std::strerror(errno));
        return false;
#else
        if (::fsync(fd_) == 0) return true;
        spdlog::error("WAL fsync failed: {}", std::strerror(errno));
        return false;
#endif
    }

    // Submit an empty barrier batch and wait for the writer to process it,
    // guaranteeing every previously-queued batch is durable.
    void Drain() {
        if (stop_) return;
        auto promise = std::make_shared<std::promise<bool>>();
        auto fut = promise->get_future();
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            if (fatal_) return;
            queue_.push_back(PendingBatch{std::string(), std::move(promise)});
            queue_cv_.notify_one();
        }
        fut.wait();
    }

    void StopWriter() {
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            if (stop_) return;
            stop_ = true;
        }
        queue_cv_.notify_all();
        if (writer_thread_.joinable()) writer_thread_.join();
    }

    void RecoverSequenceNumber() {
        auto entries = ReadAll();
        uint64_t max_seq = 0;
        for (const auto& e : entries) max_seq = std::max(max_seq, e.sequence);
        next_sequence_ = max_seq + 1;
    }

    std::string SerializeEntry(const WALEntry& entry) {
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
    SyncMode sync_mode_;

    int fd_ = -1;
    std::mutex file_mutex_;              // guards fd_ writes / truncate

    std::atomic<uint64_t> next_sequence_;

    // Writer thread + submission queue
    std::thread writer_thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<PendingBatch> queue_;
    bool stop_ = true;
    bool fatal_ = false;
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

std::future<bool> WALImpl::AppendBatchAsync(std::vector<WALEntry>& entries) {
    return impl_->AppendBatchAsync(entries);
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
