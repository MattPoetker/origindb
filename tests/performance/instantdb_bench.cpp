// InstantDB benchmark suite — stable, repeatable baselines for regression
// tracking. Fixed operation counts (not wall-clock durations) so runs are
// comparable; JSON output feeds scripts/bench_compare.py.
//
// Usage: instantdb_bench [--out results.json] [--fixture tests/wasm/fixtures/test_module.wat]
//
// Coverage:
//   storage:    insert / get / scan throughput+latency
//   wasm:       reducer invoke (no-write), reducer with commit, multi-module scaling
//   changefeed: publish->delivery latency, subscription WHERE matching cost
//   wal:        recovery (full replay) time vs row count

#include <wasmtime.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "changefeed/changefeed_engine.h"
#include "changefeed/sql_subscription.h"
#include "common/config.h"
#include "storage/storage_engine.h"
#include "wasm/wasm_engine.h"

using namespace instantdb;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------------------

struct BenchResult {
    std::string name;
    uint64_t ops = 0;
    double seconds = 0;
    std::vector<double> latencies_us;  // optional
    std::string notes;

    double OpsPerSec() const { return seconds > 0 ? ops / seconds : 0; }
    double Pct(double p) const {
        if (latencies_us.empty()) return 0;
        auto sorted = latencies_us;
        std::sort(sorted.begin(), sorted.end());
        size_t i = std::min(sorted.size() - 1,
                            static_cast<size_t>(p / 100.0 * sorted.size()));
        return sorted[i];
    }
};

std::vector<BenchResult> g_results;

class Timer {
public:
    Timer() : start_(Clock::now()) {}
    double Seconds() const {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }
    double Us() const { return Seconds() * 1e6; }

private:
    Clock::time_point start_;
};

void Report(BenchResult r) {
    std::ostringstream line;
    line << "  " << r.name;
    for (size_t i = r.name.size(); i < 36; i++) line << ' ';
    line << std::fixed;
    line.precision(0);
    line << r.OpsPerSec() << " ops/s";
    if (!r.latencies_us.empty()) {
        line.precision(1);
        line << "   p50 " << r.Pct(50) << "us  p99 " << r.Pct(99) << "us";
    }
    if (!r.notes.empty()) line << "   [" << r.notes << "]";
    std::cout << line.str() << std::endl;
    g_results.push_back(std::move(r));
}

std::filesystem::path FreshDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("instantdb_bench_" + tag + "_" +
                std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::shared_ptr<StorageEngine> MakeStorage(const std::filesystem::path& dir) {
    StorageConfig config;
    config.data_dir = dir.string();
    auto storage = std::make_shared<StorageEngine>(config);
    if (!storage->Initialize()) {
        std::cerr << "storage init failed" << std::endl;
        exit(1);
    }
    return storage;
}

void CreateBenchTable(StorageEngine& storage) {
    TableSchema schema;
    schema.name = "bench";
    schema.columns = {
        {.name = "id", .type = DataType::INT64, .is_primary_key = true},
        {.name = "name", .type = DataType::STRING},
        {.name = "score", .type = DataType::DOUBLE},
        {.name = "active", .type = DataType::BOOLEAN},
    };
    schema.primary_key = {"id"};
    storage.CreateTable(schema);
}

Row MakeRow(uint64_t i) {
    Row row;
    row.key = "key_" + std::to_string(i);
    row.columns["id"] = static_cast<int64_t>(i);
    row.columns["name"] = "user_" + std::to_string(i % 1000);
    row.columns["score"] = static_cast<double>(i % 100);
    row.columns["active"] = (i % 2) == 0;
    return row;
}

// ---------------------------------------------------------------------------
// storage
// ---------------------------------------------------------------------------

void BenchStorage() {
    std::cout << "\nstorage" << std::endl;
    auto dir = FreshDir("storage");
    auto storage = MakeStorage(dir);
    CreateBenchTable(*storage);

    constexpr uint64_t N = 50000;
    {
        BenchResult r{.name = "storage_insert_50k"};
        r.latencies_us.reserve(N);
        Timer total;
        for (uint64_t i = 0; i < N; i++) {
            Timer t;
            storage->Insert("bench", MakeRow(i));
            r.latencies_us.push_back(t.Us());
        }
        r.ops = N;
        r.seconds = total.Seconds();
        r.notes = "txn + WAL per op";
        Report(std::move(r));
    }
    {
        BenchResult r{.name = "storage_get_50k"};
        r.latencies_us.reserve(N);
        Timer total;
        for (uint64_t i = 0; i < N; i++) {
            Timer t;
            auto row = storage->Get("bench", "key_" + std::to_string(i));
            r.latencies_us.push_back(t.Us());
            if (!row) exit(2);
        }
        r.ops = N;
        r.seconds = total.Seconds();
        Report(std::move(r));
    }
    {
        constexpr int SCANS = 200;
        BenchResult r{.name = "storage_scan_full_50k_rows"};
        r.latencies_us.reserve(SCANS);
        Timer total;
        for (int i = 0; i < SCANS; i++) {
            Timer t;
            uint64_t count = 0;
            storage->Scan("bench", "", "", [&](const std::string&, const Row&) {
                count++;
                return true;
            });
            r.latencies_us.push_back(t.Us());
            if (count != N) exit(2);
        }
        r.ops = SCANS;
        r.seconds = total.Seconds();
        r.notes = "rows/s = " + std::to_string(
                      static_cast<uint64_t>(SCANS * N / total.Seconds()));
        Report(std::move(r));
    }

    storage->Shutdown();
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// WAL recovery
// ---------------------------------------------------------------------------

void BenchWalRecovery(uint64_t rows, bool clean_shutdown) {
    auto dir = FreshDir("wal");
    {
        auto storage = MakeStorage(dir);
        CreateBenchTable(*storage);
        for (uint64_t i = 0; i < rows; i++) storage->Insert("bench", MakeRow(i));
        // Clean shutdown writes a snapshot and truncates the WAL; skipping it
        // simulates a crash, forcing full WAL replay on the next boot.
        if (clean_shutdown) storage->Shutdown();
    }
    uint64_t disk_bytes = 0;
    std::error_code ec;
    for (auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file()) disk_bytes += e.file_size();

    std::string label = clean_shutdown ? "snapshot_recovery_" : "wal_replay_crash_";
    BenchResult r{.name = label + std::to_string(rows / 1000) + "k"};
    Timer t;
    auto storage = MakeStorage(dir);  // snapshot load and/or WAL replay
    r.seconds = t.Seconds();
    r.ops = rows;
    r.notes = "disk " + std::to_string(disk_bytes / 1024 / 1024) + "MB, boot " +
              std::to_string(r.seconds).substr(0, 5) + "s";
    if (!storage->Get("bench", "key_" + std::to_string(rows - 1))) exit(2);
    std::filesystem::remove_all(dir);
    Report(std::move(r));
}

// ---------------------------------------------------------------------------
// wasm
// ---------------------------------------------------------------------------

std::vector<uint8_t> LoadFixture(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "cannot open WAT fixture: " << path << std::endl;
        exit(1);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string wat = ss.str();
    wasm_byte_vec_t out;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat.c_str(), wat.size(), &out);
    if (err) {
        std::cerr << "wat2wasm failed" << std::endl;
        exit(1);
    }
    std::vector<uint8_t> bytes(out.data, out.data + out.size);
    wasm_byte_vec_delete(&out);
    return bytes;
}

void BenchWasm(const std::vector<uint8_t>& module_bytes) {
    std::cout << "\nwasm" << std::endl;
    auto dir = FreshDir("wasm");
    auto storage = MakeStorage(dir);
    auto engine = std::make_shared<WasmEngine>(storage, nullptr);
    engine->Initialize();
    if (!engine->LoadModule("bench", module_bytes)) {
        std::cerr << "module load failed: " << engine->GetLastLoadError() << std::endl;
        exit(1);
    }
    auto ctx = engine->CreateReducerContext("bench");

    constexpr uint64_t CALLS = 5000;
    {
        // "res": sets a result payload, no table writes, no commit.
        BenchResult r{.name = "wasm_invoke_nowrite_5k"};
        r.latencies_us.reserve(CALLS);
        Timer total;
        for (uint64_t i = 0; i < CALLS; i++) {
            Timer t;
            auto result = engine->ExecuteReducer("bench", "res", ctx, {});
            r.latencies_us.push_back(t.Us());
            if (!result.success) exit(2);
        }
        r.ops = CALLS;
        r.seconds = total.Seconds();
        r.notes = "sandbox + dispatch only";
        Report(std::move(r));
    }
    {
        // "w": one staged write -> transaction commit -> WAL.
        BenchResult r{.name = "wasm_invoke_write_commit_5k"};
        r.latencies_us.reserve(CALLS);
        Timer total;
        for (uint64_t i = 0; i < CALLS; i++) {
            Timer t;
            auto result = engine->ExecuteReducer("bench", "w", ctx, {});
            r.latencies_us.push_back(t.Us());
            if (!result.success) exit(2);
        }
        r.ops = CALLS;
        r.seconds = total.Seconds();
        r.notes = "full write pipeline";
        Report(std::move(r));
    }

    // Multi-module scaling: same workload spread across N independent modules,
    // one thread per module (per-module mutex serializes within a module).
    for (int modules : {1, 2, 4, 8}) {
        for (int m = 0; m < modules; m++) {
            std::string name = "bench_scale_" + std::to_string(m);
            if (!engine->GetModule(name) &&
                !engine->LoadModule(name, module_bytes)) {
                exit(1);
            }
        }
        constexpr uint64_t PER_THREAD = 2000;
        std::atomic<uint64_t> failures{0};
        Timer total;
        std::vector<std::thread> threads;
        for (int m = 0; m < modules; m++) {
            threads.emplace_back([&, m] {
                auto tctx = engine->CreateReducerContext("bench");
                std::string name = "bench_scale_" + std::to_string(m);
                for (uint64_t i = 0; i < PER_THREAD; i++) {
                    if (!engine->ExecuteReducer(name, "w", tctx, {}).success)
                        failures++;
                }
            });
        }
        for (auto& t : threads) t.join();
        BenchResult r{.name = "wasm_write_scaling_x" + std::to_string(modules)};
        r.ops = PER_THREAD * modules - failures.load();
        r.seconds = total.Seconds();
        r.notes = std::to_string(modules) + " modules, 1 thread each";
        Report(std::move(r));
    }

    engine->Shutdown();
    storage->Shutdown();
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// changefeed
// ---------------------------------------------------------------------------

ChangefeedEvent MakeEvent(uint64_t i, const std::string& table) {
    ChangefeedEvent ev;
    ev.table = table;
    ev.operation = "INSERT";
    std::string key = "k" + std::to_string(i);
    ev.key.assign(key.begin(), key.end());
    std::string value = "{\"key\":\"" + key + "\",\"columns\":{\"id\":" +
                        std::to_string(i) + ",\"score\":" + std::to_string(i % 100) +
                        ",\"name\":\"user_" + std::to_string(i % 1000) + "\"}}";
    ev.new_value.assign(value.begin(), value.end());
    ev.timestamp = std::chrono::system_clock::now();
    return ev;
}

void BenchChangefeedDelivery() {
    std::cout << "\nchangefeed" << std::endl;
    auto dir = FreshDir("cf");
    auto storage = MakeStorage(dir);
    ChangefeedConfig config;
    auto changefeed = std::make_shared<ChangefeedEngine>(storage, config);
    changefeed->Initialize();
    changefeed->Start();

    // publish -> single-subscriber delivery latency through the delivery thread
    constexpr uint64_t EVENTS = 10000;
    std::mutex mu;
    std::condition_variable cv;
    uint64_t delivered = 0;
    std::vector<double> latencies;
    latencies.reserve(EVENTS);
    std::vector<Clock::time_point> sent(EVENTS);

    auto sub = changefeed->CreateSubscription({.table_pattern = "*"}, 0,
                                              DeliveryMode::AT_LEAST_ONCE);
    changefeed->Subscribe(sub, [&](const std::string&, const ChangefeedEvent& ev) {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mu);
        if (delivered < EVENTS) {
            latencies.push_back(
                std::chrono::duration<double, std::micro>(now - sent[delivered]).count());
        }
        delivered++;
        if (delivered == EVENTS) cv.notify_one();
    });

    Timer total;
    for (uint64_t i = 0; i < EVENTS; i++) {
        sent[i] = Clock::now();
        changefeed->PublishEvent(MakeEvent(i, "bench"));
    }
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::seconds(30), [&] { return delivered >= EVENTS; });
    }
    BenchResult r{.name = "changefeed_publish_deliver_10k"};
    r.ops = delivered;
    r.seconds = total.Seconds();
    r.latencies_us = std::move(latencies);
    r.notes = "single subscriber, incl. queue wait";
    Report(std::move(r));

    changefeed->Stop();
    storage->Shutdown();
    std::filesystem::remove_all(dir);
}

void BenchSubscriptionMatching(size_t num_subs) {
    SqlSubscriptionManager manager;
    // Half match the event's table with WHERE predicates of varying selectivity;
    // half point at other tables.
    for (size_t i = 0; i < num_subs; i++) {
        std::string client = "client_" + std::to_string(i);
        if (i % 2 == 0) {
            manager.Subscribe(client, "SELECT * FROM bench WHERE score > " +
                                          std::to_string(i % 100));
        } else {
            manager.Subscribe(client,
                              "SELECT * FROM other_" + std::to_string(i) );
        }
    }

    constexpr uint64_t EVENTS = 5000;
    uint64_t matched = 0;
    BenchResult r{.name = "subs_match_" + std::to_string(num_subs)};
    r.latencies_us.reserve(EVENTS);
    Timer total;
    for (uint64_t i = 0; i < EVENTS; i++) {
        auto ev = MakeEvent(i, "bench");
        Timer t;
        matched += manager.ProcessEvent(ev).size();
        r.latencies_us.push_back(t.Us());
    }
    r.ops = EVENTS;
    r.seconds = total.Seconds();
    r.notes = std::to_string(num_subs) + " subs, " +
              std::to_string(static_cast<double>(matched) / EVENTS).substr(0, 4) +
              " matches/event";
    Report(std::move(r));
}

}  // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::string out_path;
    std::string fixture = "tests/wasm/fixtures/test_module.wat";
    for (int i = 1; i < argc - 1; i++) {
        if (std::string(argv[i]) == "--out") out_path = argv[i + 1];
        if (std::string(argv[i]) == "--fixture") fixture = argv[i + 1];
    }

    spdlog::set_level(spdlog::level::err);
    std::cout << "InstantDB benchmark suite" << std::endl;

    auto module_bytes = LoadFixture(fixture);

    BenchStorage();
    std::cout << "\nrecovery" << std::endl;
    BenchWalRecovery(10000, false);
    BenchWalRecovery(100000, false);
    BenchWalRecovery(100000, true);
    BenchWasm(module_bytes);
    BenchChangefeedDelivery();
    std::cout << "\nsubscription matching" << std::endl;
    for (size_t n : {1, 10, 100, 1000}) BenchSubscriptionMatching(n);

    if (!out_path.empty()) {
        nlohmann::json doc;
        doc["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        doc["hardware_threads"] = std::thread::hardware_concurrency();
        nlohmann::json results = nlohmann::json::array();
        for (const auto& r : g_results) {
            results.push_back({
                {"name", r.name},
                {"ops", r.ops},
                {"seconds", r.seconds},
                {"ops_per_sec", r.OpsPerSec()},
                {"p50_us", r.Pct(50)},
                {"p99_us", r.Pct(99)},
                {"notes", r.notes},
            });
        }
        doc["results"] = results;
        std::ofstream out(out_path);
        out << doc.dump(2) << std::endl;
        std::cout << "\nwrote " << out_path << std::endl;
    }
    return 0;
}
