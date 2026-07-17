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

namespace {

// 8-byte file magic identifying the binary WAL format. Lets us cleanly reject a
// legacy JSON-lines WAL (which starts with '{') instead of mis-parsing it.
constexpr char kWalMagic[8] = {'O', 'R', 'W', 'A', 'L', 0x01, 0, 0};

void PutU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 24));
}
void PutU64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; i++) out.push_back(static_cast<char>(v >> (8 * i)));
}
void PutLenPrefixed(std::string& out, const std::string& s) {
    PutU32(out, static_cast<uint32_t>(s.size()));
    out += s;
}
void PutLenPrefixed(std::string& out, const std::vector<uint8_t>& b) {
    PutU32(out, static_cast<uint32_t>(b.size()));
    out.append(reinterpret_cast<const char*>(b.data()), b.size());
}

}  // namespace

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
        WriteMagicIfEmpty();

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
        blob.reserve(64 * entries.size());
        uint64_t ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        for (auto& e : entries) {
            e.sequence = next_sequence_++;
            e.timestamp = ts;
            blob += SerializeRecord(e);
        }

        queue_.push_back(PendingBatch{std::move(blob), std::move(promise)});
        queue_cv_.notify_one();
        return future;
    }

    std::vector<WALEntry> ReadAll() {
        std::vector<WALEntry> result;
        std::ifstream file(wal_file_path_, std::ios::binary);
        if (!file.is_open()) return result;

        // Verify the file magic; a legacy JSON-lines WAL (or garbage) is
        // rejected wholesale rather than mis-parsed. Recovery then falls back
        // to the snapshot.
        char magic[sizeof(kWalMagic)];
        file.read(magic, sizeof(magic));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(magic)) ||
            std::memcmp(magic, kWalMagic, sizeof(magic)) != 0) {
            if (file.gcount() > 0) {
                spdlog::warn("WAL {} has no/unknown magic — ignoring (expected "
                             "binary WAL; legacy JSON WALs are not supported)",
                             wal_file_path_);
            }
            return result;
        }

        // Records: [u32 record_len][record_bytes]. A short read at the end is a
        // torn tail from a crash mid-write — stop cleanly, keeping prior records.
        while (true) {
            uint8_t lenbuf[4];
            file.read(reinterpret_cast<char*>(lenbuf), 4);
            if (file.gcount() != 4) break;
            uint32_t rlen = static_cast<uint32_t>(lenbuf[0]) |
                            (static_cast<uint32_t>(lenbuf[1]) << 8) |
                            (static_cast<uint32_t>(lenbuf[2]) << 16) |
                            (static_cast<uint32_t>(lenbuf[3]) << 24);
            std::string rec(rlen, '\0');
            file.read(rec.data(), rlen);
            if (file.gcount() != static_cast<std::streamsize>(rlen)) break;
            try {
                auto entry = DeserializeRecord(rec);
                if (entry.sequence > 0) result.push_back(std::move(entry));
            } catch (const std::exception& e) {
                spdlog::warn("Skipping malformed WAL record: {}", e.what());
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
        WriteMagicIfEmpty();  // freshly truncated file needs the magic header
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

    void WriteMagicIfEmpty() {
        std::error_code ec;
        auto size = std::filesystem::file_size(wal_file_path_, ec);
        if (!ec && size == 0 && fd_ >= 0) {
            ssize_t n = ::write(fd_, kWalMagic, sizeof(kWalMagic));
            if (n != static_cast<ssize_t>(sizeof(kWalMagic))) {
                spdlog::error("Failed to write WAL magic: {}", std::strerror(errno));
            }
        }
    }

    // Binary record: [u32 record_len][ record_bytes ] where record_bytes =
    //   [u8 type][u64 seq][u64 txn_id][u64 timestamp]
    //   [u32 table_len][table][u32 key_len][key][u32 data_len][data]
    // The `data` blob is an opaque, already-binary row payload (row_codec).
    std::string SerializeRecord(const WALEntry& entry) {
        std::string rec;
        rec.reserve(33 + entry.table_name.size() + entry.key.size() +
                    entry.data.size());
        rec.push_back(static_cast<char>(static_cast<int>(entry.type)));
        PutU64(rec, entry.sequence);
        PutU64(rec, entry.transaction_id);
        PutU64(rec, entry.timestamp);
        PutLenPrefixed(rec, entry.table_name);
        PutLenPrefixed(rec, entry.key);
        PutLenPrefixed(rec, entry.data);

        std::string framed;
        framed.reserve(4 + rec.size());
        PutU32(framed, static_cast<uint32_t>(rec.size()));
        framed += rec;
        return framed;
    }

    WALEntry DeserializeRecord(const std::string& rec) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(rec.data());
        const uint8_t* end = p + rec.size();
        auto need = [&](size_t n) {
            if (static_cast<size_t>(end - p) < n)
                throw std::runtime_error("truncated WAL record");
        };
        auto u32 = [&]() -> uint32_t {
            need(4);
            uint32_t v = static_cast<uint32_t>(p[0]) |
                         (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16) |
                         (static_cast<uint32_t>(p[3]) << 24);
            p += 4;
            return v;
        };
        auto u64 = [&]() -> uint64_t {
            need(8);
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
            p += 8;
            return v;
        };
        auto str = [&]() -> std::string {
            uint32_t n = u32();
            need(n);
            std::string s(reinterpret_cast<const char*>(p), n);
            p += n;
            return s;
        };
        auto bytes = [&]() -> std::vector<uint8_t> {
            uint32_t n = u32();
            need(n);
            std::vector<uint8_t> b(p, p + n);
            p += n;
            return b;
        };

        WALEntry entry;
        need(1);
        entry.type = static_cast<WALEntryType>(*p++);
        entry.sequence = u64();
        entry.transaction_id = u64();
        entry.timestamp = u64();
        entry.table_name = str();
        entry.key = str();
        entry.data = bytes();
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
