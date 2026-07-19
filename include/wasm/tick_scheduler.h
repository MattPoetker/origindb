#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "wasm/wasm_engine.h"

namespace origindb {

// Drives reducers at a fixed rate from a worker pool INSIDE the server, so no
// external process has to poll ticks over gRPC. Ticks for different modules run
// concurrently across the pool (up to `num_workers` cores); ticks to the same
// module serialize on that module's own call mutex. If a module's previous tick
// is still running when its next one is due, the new tick is COALESCED (skipped)
// so a slow shard degrades in rate instead of piling up unbounded work.
class TickScheduler {
public:
    struct Entry {
        std::string module;
        std::string reducer;
        std::vector<WasmValue> args;
        std::chrono::nanoseconds period;
    };

    TickScheduler(std::shared_ptr<WasmEngine> engine, unsigned num_workers);
    ~TickScheduler();

    // Add before Start(); not thread-safe against a running scheduler.
    void AddEntry(Entry entry);
    void Start();
    void Stop();

    struct Stats {
        uint64_t executed = 0;    // ticks completed since last snapshot
        uint64_t skipped = 0;     // coalesced (prior tick still in-flight)
        uint64_t errors = 0;      // reducer returned success == false
        uint64_t max_exec_us = 0; // slowest single tick
        size_t entries = 0;
    };
    Stats SnapshotAndReset();

private:
    struct Runtime {
        Entry entry;
        std::chrono::steady_clock::time_point next_fire;
        std::atomic<bool> in_flight{false};
    };

    void DispatchLoop();
    void WorkerLoop();

    std::shared_ptr<WasmEngine> engine_;
    unsigned num_workers_;
    std::vector<std::unique_ptr<Runtime>> entries_;

    std::thread dispatch_thread_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<Runtime*> queue_;

    std::atomic<uint64_t> stat_executed_{0};
    std::atomic<uint64_t> stat_skipped_{0};
    std::atomic<uint64_t> stat_errors_{0};
    std::atomic<uint64_t> stat_max_exec_us_{0};
};

}  // namespace origindb
