// Tests for the changefeed parallel fan-out worker pool.
//
// The pool delivers a single committed event to many subscribers concurrently,
// while still guaranteeing that each subscriber sees events in commit order
// (RunBatch barriers per event).
#include <gtest/gtest.h>

#include "changefeed/changefeed_engine.h"
#include "common/config.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace origindb;
using namespace std::chrono_literals;

namespace {

ChangefeedEvent MakeEvent(const std::string& table) {
    ChangefeedEvent e;
    e.table = table;
    e.operation = "INSERT";
    return e;
}

// Poll the delivered-count metric until it reaches `expected` or we time out.
// Delivery is asynchronous (a background loop), and Stop() does not drain the
// queue, so tests must wait on the metric before asserting.
bool WaitForDeliveries(ChangefeedEngine& cf, uint64_t expected,
                       std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cf.GetMetrics().total_events_delivered >= expected) return true;
        std::this_thread::sleep_for(2ms);
    }
    return cf.GetMetrics().total_events_delivered >= expected;
}

std::string AddSub(ChangefeedEngine& cf, DeliveryCallback cb) {
    SubscriptionFilter f;
    f.table_pattern = "*";
    std::string id = cf.CreateSubscription(f, 0);
    cf.Subscribe(id, std::move(cb));
    return id;
}

// Records the peak number of callbacks running at once — a lock-free max.
void RecordPeak(std::atomic<int>& inflight, std::atomic<int>& peak) {
    int cur = inflight.fetch_add(1) + 1;
    int prev = peak.load();
    while (cur > prev && !peak.compare_exchange_weak(prev, cur)) { /* retry */ }
}

}  // namespace

// Every subscriber receives every event, in strictly increasing offset order,
// even though each event is fanned out across the pool.
TEST(ChangefeedFanout, OrderingPreservedAcrossParallelDelivery) {
    ChangefeedConfig cfg;
    cfg.delivery_threads = 4;
    ChangefeedEngine cf(nullptr, cfg);
    ASSERT_TRUE(cf.Start());

    const int kSubs = 6;
    const int kEvents = 200;
    std::mutex m;
    std::map<std::string, std::vector<uint64_t>> got;
    std::vector<std::string> ids;
    for (int s = 0; s < kSubs; ++s) {
        std::string id = AddSub(cf, [&](const std::string& sid, const ChangefeedEvent& e) {
            std::lock_guard<std::mutex> lk(m);
            got[sid].push_back(e.offset);
        });
        ids.push_back(id);
    }

    for (int i = 0; i < kEvents; ++i) cf.PublishEvent(MakeEvent("t"));

    ASSERT_TRUE(WaitForDeliveries(cf, uint64_t(kSubs) * kEvents));

    for (const auto& id : ids) {
        auto& v = got[id];
        ASSERT_EQ(v.size(), size_t(kEvents)) << "sub " << id;
        for (size_t i = 1; i < v.size(); ++i) {
            EXPECT_LT(v[i - 1], v[i]) << "out-of-order at index " << i << " for " << id;
        }
    }
    EXPECT_EQ(cf.GetMetrics().total_events_delivered, uint64_t(kSubs) * kEvents);
    cf.Stop();
}

// With a pool, the subscribers of a single event run concurrently.
TEST(ChangefeedFanout, DeliversConcurrentlyWithPool) {
    ChangefeedConfig cfg;
    cfg.delivery_threads = 4;
    ChangefeedEngine cf(nullptr, cfg);
    ASSERT_TRUE(cf.Start());

    std::atomic<int> inflight{0};
    std::atomic<int> peak{0};
    const int kSubs = 4;
    for (int s = 0; s < kSubs; ++s) {
        AddSub(cf, [&](const std::string&, const ChangefeedEvent&) {
            RecordPeak(inflight, peak);
            std::this_thread::sleep_for(30ms);  // hold long enough to overlap
            inflight.fetch_sub(1);
        });
    }

    cf.PublishEvent(MakeEvent("t"));
    ASSERT_TRUE(WaitForDeliveries(cf, kSubs));
    EXPECT_GE(peak.load(), 2) << "expected concurrent delivery when a pool is configured";
    cf.Stop();
}

// delivery_threads == 1 disables the pool: strictly sequential, no overlap.
TEST(ChangefeedFanout, SequentialModeNeverOverlaps) {
    ChangefeedConfig cfg;
    cfg.delivery_threads = 1;
    ChangefeedEngine cf(nullptr, cfg);
    ASSERT_TRUE(cf.Start());

    std::atomic<int> inflight{0};
    std::atomic<int> peak{0};
    const int kSubs = 4;
    for (int s = 0; s < kSubs; ++s) {
        AddSub(cf, [&](const std::string&, const ChangefeedEvent&) {
            RecordPeak(inflight, peak);
            std::this_thread::sleep_for(5ms);
            inflight.fetch_sub(1);
        });
    }

    cf.PublishEvent(MakeEvent("t"));
    ASSERT_TRUE(WaitForDeliveries(cf, kSubs));
    EXPECT_EQ(peak.load(), 1) << "sequential mode must deliver one subscriber at a time";
    cf.Stop();
}

// A throwing subscriber must not take down delivery to the others, and the
// delivered-count still advances (it counts attempts).
TEST(ChangefeedFanout, SubscriberExceptionIsIsolated) {
    ChangefeedConfig cfg;
    cfg.delivery_threads = 4;
    ChangefeedEngine cf(nullptr, cfg);
    ASSERT_TRUE(cf.Start());

    std::atomic<int> good{0};
    AddSub(cf, [&](const std::string&, const ChangefeedEvent&) {
        throw std::runtime_error("boom");
    });
    for (int s = 0; s < 3; ++s) {
        AddSub(cf, [&](const std::string&, const ChangefeedEvent&) { good.fetch_add(1); });
    }

    const int kEvents = 20;
    for (int i = 0; i < kEvents; ++i) cf.PublishEvent(MakeEvent("t"));

    // 4 subscribers × 20 events attempted (delivered_ counts the thrower too).
    ASSERT_TRUE(WaitForDeliveries(cf, uint64_t(4) * kEvents));
    EXPECT_EQ(good.load(), 3 * kEvents);
    cf.Stop();
}
