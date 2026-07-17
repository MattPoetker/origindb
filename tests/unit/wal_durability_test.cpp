// Durability / group-commit tests for the WAL writer.
//
// Two properties are covered:
//   1. Concurrent committers going through the group-commit batch writer never
//      lose or corrupt entries — after a simulated crash (engine destroyed
//      without a clean shutdown, so no snapshot is written) every committed row
//      is recovered from the WAL.
//   2. A hard SIGKILL mid-write cannot lose a commit that was already
//      acknowledged: every key the (killed) child process recorded as durably
//      acked is present after recovery.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "common/config.h"
#include "storage/storage_engine.h"

namespace origindb {
namespace {

std::filesystem::path FreshDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("wal_durability_" + tag + "_" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

void CreateBenchTable(StorageEngine& storage) {
    TableSchema schema;
    schema.name = "t";
    schema.columns = {
        {.name = "id", .type = DataType::INT64, .is_primary_key = true},
        {.name = "name", .type = DataType::STRING},
    };
    schema.primary_key = {"id"};
    storage.CreateTable(schema);
}

Row MakeRow(uint64_t i) {
    Row row;
    row.key = "key_" + std::to_string(i);
    row.columns["id"] = static_cast<int64_t>(i);
    row.columns["name"] = "user_" + std::to_string(i);
    return row;
}

// Concurrent inserts through the group-commit writer, then a crash (destroy the
// engine WITHOUT Shutdown, so no snapshot). Every acked row must be recovered
// from the WAL.
TEST(WalDurability, ConcurrentCommitsAllRecovered) {
    auto dir = FreshDir("concurrent");
    constexpr int kThreads = 8;
    constexpr uint64_t kPerThread = 500;

    {
        StorageConfig cfg;
        cfg.data_dir = dir.string();
        cfg.sync_mode = SyncMode::Flush;  // mode-independent path; flush = fast
        auto storage = std::make_shared<StorageEngine>(cfg);
        ASSERT_TRUE(storage->Initialize());
        CreateBenchTable(*storage);

        std::atomic<uint64_t> next{0};
        std::atomic<uint64_t> acked{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < kThreads; t++) {
            ths.emplace_back([&] {
                for (uint64_t i = 0; i < kPerThread; i++) {
                    uint64_t k = next.fetch_add(1);
                    if (storage->Insert("t", MakeRow(k))) acked.fetch_add(1);
                }
            });
        }
        for (auto& t : ths) t.join();
        EXPECT_EQ(acked.load(), kThreads * kPerThread);
        // Drop the engine WITHOUT Shutdown() -> no snapshot, WAL left intact.
        // The WALImpl destructor drains the writer and closes the file.
    }

    // Reopen: recovery replays the WAL. Every acked key must be present.
    StorageConfig cfg;
    cfg.data_dir = dir.string();
    auto storage = std::make_shared<StorageEngine>(cfg);
    ASSERT_TRUE(storage->Initialize());
    uint64_t missing = 0;
    for (uint64_t k = 0; k < kThreads * kPerThread; k++) {
        if (!storage->Get("t", "key_" + std::to_string(k))) missing++;
    }
    EXPECT_EQ(missing, 0u);

    std::filesystem::remove_all(dir);
}

// A child process commits at fsync durability, recording each acked key to a
// separate fsync'd file. The parent SIGKILLs it mid-stream. After recovery,
// every key the child managed to record as acked must survive — a hard kill
// cannot lose an already-acknowledged, durable commit.
TEST(WalDurability, AckedCommitsSurviveHardKill) {
    auto dir = FreshDir("kill");
    auto ack_path = (dir / "acks.txt").string();

    pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork failed";

    if (pid == 0) {
        // ---- child ----
        spdlog::set_level(spdlog::level::off);
        StorageConfig cfg;
        cfg.data_dir = dir.string();
        cfg.sync_mode = SyncMode::Fsync;
        auto storage = std::make_shared<StorageEngine>(cfg);
        if (!storage->Initialize()) _exit(3);
        CreateBenchTable(*storage);

        int ack_fd = ::open(ack_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (ack_fd < 0) _exit(4);

        for (uint64_t i = 0; i < 100000; i++) {
            if (storage->Insert("t", MakeRow(i))) {
                // Insert returned => the commit is durable in the WAL. Record
                // the key AND fsync the ack file, so the parent only ever
                // trusts keys that were provably durable before this line.
                std::string line = "key_" + std::to_string(i) + "\n";
                (void)::write(ack_fd, line.data(), line.size());
                ::fsync(ack_fd);
            }
        }
        _exit(0);  // hard exit, no clean Shutdown (== crash, no snapshot)
    }

    // ---- parent ----
    // Give the child time to durably commit a batch of rows, then hard-kill it
    // mid-write. 800ms comfortably exceeds one fsync even on slow storage.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    // Collect the keys the child recorded as durably acked.
    std::vector<std::string> acked;
    {
        std::ifstream in(ack_path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) acked.push_back(line);
        }
    }
    ASSERT_FALSE(acked.empty()) << "child acked nothing before kill";

    // Reopen and recover from the WAL; every acked key must be present.
    StorageConfig cfg;
    cfg.data_dir = dir.string();
    cfg.sync_mode = SyncMode::Fsync;
    auto storage = std::make_shared<StorageEngine>(cfg);
    ASSERT_TRUE(storage->Initialize());

    uint64_t lost = 0;
    for (const auto& key : acked) {
        if (!storage->Get("t", key)) lost++;
    }
    EXPECT_EQ(lost, 0u) << "lost " << lost << " of " << acked.size()
                        << " acknowledged commits after SIGKILL";

    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace origindb
