#include "wasm/tick_scheduler.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace origindb {

TickScheduler::TickScheduler(std::shared_ptr<WasmEngine> engine, unsigned num_workers)
    : engine_(std::move(engine)), num_workers_(num_workers == 0 ? 1u : num_workers) {}

TickScheduler::~TickScheduler() { Stop(); }

void TickScheduler::AddEntry(Entry entry) {
    auto rt = std::make_unique<Runtime>();
    rt->entry = std::move(entry);
    rt->next_fire = std::chrono::steady_clock::now();
    entries_.push_back(std::move(rt));
}

void TickScheduler::Start() {
    if (running_.exchange(true)) return;
    workers_.reserve(num_workers_);
    for (unsigned i = 0; i < num_workers_; ++i) workers_.emplace_back([this] { WorkerLoop(); });
    dispatch_thread_ = std::thread([this] { DispatchLoop(); });
    spdlog::info("⏱️  TickScheduler started: {} entries, {} workers", entries_.size(), num_workers_);
}

void TickScheduler::Stop() {
    if (!running_.exchange(false)) return;
    queue_cv_.notify_all();
    if (dispatch_thread_.joinable()) dispatch_thread_.join();
    for (auto& w : workers_) if (w.joinable()) w.join();
    workers_.clear();
    spdlog::info("⏱️  TickScheduler stopped");
}

// One dispatcher thread: for each entry whose fire time has arrived, advance its
// schedule (dropping any missed frames) and enqueue it — unless its previous
// tick is still in flight, in which case it's coalesced. Sleeps until the next
// due entry, capped so it never oversleeps a frame.
void TickScheduler::DispatchLoop() {
    using namespace std::chrono;
    const auto max_slice = milliseconds(4);
    while (running_.load()) {
        auto now = steady_clock::now();
        auto next_wake = now + max_slice;
        for (auto& rt : entries_) {
            if (now >= rt->next_fire) {
                do { rt->next_fire += duration_cast<steady_clock::duration>(rt->entry.period); }
                while (rt->next_fire <= now);
                bool expected = false;
                if (rt->in_flight.compare_exchange_strong(expected, true)) {
                    { std::lock_guard<std::mutex> lk(queue_mutex_); queue_.push(rt.get()); }
                    queue_cv_.notify_one();
                } else {
                    stat_skipped_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (rt->next_fire < next_wake) next_wake = rt->next_fire;
        }
        std::this_thread::sleep_until(std::min(next_wake, now + max_slice));
    }
}

// Worker pool: pull a due tick, run its reducer (per-module mutex serializes
// against player-input calls automatically), record metrics, clear in-flight.
void TickScheduler::WorkerLoop() {
    using namespace std::chrono;
    while (true) {
        Runtime* rt = nullptr;
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            queue_cv_.wait(lk, [this] { return !queue_.empty() || !running_.load(); });
            if (!running_.load() && queue_.empty()) return;
            rt = queue_.front();
            queue_.pop();
        }
        auto ctx = engine_->CreateReducerContext("system", "");
        ctx.module_identity = rt->entry.module;
        auto t0 = steady_clock::now();
        auto res = engine_->ExecuteReducer(rt->entry.module, rt->entry.reducer, ctx, rt->entry.args);
        auto us = static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now() - t0).count());
        if (!res.success) stat_errors_.fetch_add(1, std::memory_order_relaxed);
        stat_executed_.fetch_add(1, std::memory_order_relaxed);
        uint64_t prev = stat_max_exec_us_.load(std::memory_order_relaxed);
        while (us > prev && !stat_max_exec_us_.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}
        rt->in_flight.store(false, std::memory_order_release);
    }
}

TickScheduler::Stats TickScheduler::SnapshotAndReset() {
    Stats s;
    s.executed = stat_executed_.exchange(0);
    s.skipped = stat_skipped_.exchange(0);
    s.errors = stat_errors_.exchange(0);
    s.max_exec_us = stat_max_exec_us_.exchange(0);
    s.entries = entries_.size();
    return s;
}

}  // namespace origindb
