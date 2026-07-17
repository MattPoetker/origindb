// End-to-end tests for the wasmtime-backed WasmEngine using WAT fixtures.
// The fixture module implements the docs/WASM_ABI.md contract by hand:
// origindb_invoke dispatches on (name_len, first_byte).

#include <gtest/gtest.h>

#include <wasmtime.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "changefeed/changefeed_engine.h"
#include "common/config.h"
#include "storage/storage_engine.h"
#include "wasm/wasm_engine.h"

namespace origindb {
namespace {

std::vector<uint8_t> Wat2Wasm(const std::string& wat) {
    wasm_byte_vec_t bytes;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat.c_str(), wat.size(), &bytes);
    if (err) {
        wasm_name_t msg;
        wasmtime_error_message(err, &msg);
        std::string text(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(err);
        ADD_FAILURE() << "wat2wasm failed: " << text;
        return {};
    }
    std::vector<uint8_t> out(bytes.data, bytes.data + bytes.size);
    wasm_byte_vec_delete(&bytes);
    return out;
}

// Reducers (dispatch on name length + first byte):
//   "__init" (6,_)   -> 0
//   "w" (1,w)        -> host_table_write t/k1={"a":1}; returns write status
//   "rd" (2,r)       -> write, then host_table_read own write, host_set_result
//   "res" (3,r)      -> host_set_result("hello"), 0
//   "loop" (4,l)     -> infinite loop (timeout test)
//   "abort" (5,a)    -> write, then host_abort("boom") (rollback test)
//   "wsecret" (7,w)  -> write to table "secret" (capability test)
//   "growalot" (8,g) -> memory.grow(1024); -5 if denied (memory-limit test)
//   anything else    -> -404
const char* kTestModuleWat = R"WAT(
(module
  (import "env" "host_table_write"
    (func $write (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "host_table_read"
    (func $read (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "host_set_result" (func $set_result (param i32 i32)))
  (import "env" "host_abort" (func $abort (param i32)))

  (memory (export "memory") 8 1024)
  (global $heap (mut i32) (i32.const 65536))

  (data (i32.const 16) "t\00")
  (data (i32.const 32) "k1")
  (data (i32.const 48) "{\22a\22:1}")
  (data (i32.const 96) "hello")
  (data (i32.const 112) "secret\00")
  (data (i32.const 128) "boom\00")

  (func (export "origindb_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    global.get $heap
    local.set $ptr
    global.get $heap
    local.get $size
    i32.add
    i32.const 7
    i32.add
    i32.const -8
    i32.and
    global.set $heap
    local.get $ptr)

  (func (export "origindb_free") (param i32))

  (func $do_write (result i32)
    (call $write (i32.const 16) (i32.const 32) (i32.const 2)
                 (i32.const 48) (i32.const 7)))

  (func $do_read (result i32)
    (local $st i32)
    (drop (call $do_write))
    (local.set $st
      (call $read (i32.const 16) (i32.const 32) (i32.const 2)
                  (i32.const 200) (i32.const 204)))
    (if (i32.ne (local.get $st) (i32.const 1))
      (then (return (i32.const -1))))
    (call $set_result (i32.load (i32.const 200)) (i32.load (i32.const 204)))
    (i32.const 0))

  (func (export "origindb_invoke")
        (param $np i32) (param $nl i32) (param $ap i32) (param $al i32)
        (result i32)
    (local $fb i32)
    (local.set $fb (i32.load8_u (local.get $np)))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 6))
                 (i32.eq (local.get $fb) (i32.const 95)))   ;; '_' -> __init
      (then (return (i32.const 0))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 1))
                 (i32.eq (local.get $fb) (i32.const 119)))  ;; 'w'
      (then (return (call $do_write))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 2))
                 (i32.eq (local.get $fb) (i32.const 114)))  ;; 'r' -> rd
      (then (return (call $do_read))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 3))
                 (i32.eq (local.get $fb) (i32.const 114)))  ;; 'r' -> res
      (then
        (call $set_result (i32.const 96) (i32.const 5))
        (return (i32.const 0))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 4))
                 (i32.eq (local.get $fb) (i32.const 108)))  ;; 'l' -> loop
      (then (loop $spin (br $spin))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 5))
                 (i32.eq (local.get $fb) (i32.const 97)))   ;; 'a' -> abort
      (then
        (drop (call $do_write))
        (call $abort (i32.const 128))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 7))
                 (i32.eq (local.get $fb) (i32.const 119)))  ;; 'w' -> wsecret
      (then
        (return (call $write (i32.const 112) (i32.const 32) (i32.const 2)
                             (i32.const 48) (i32.const 7)))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 8))
                 (i32.eq (local.get $fb) (i32.const 103)))  ;; 'g' -> growalot
      (then
        (if (i32.eq (memory.grow (i32.const 1024)) (i32.const -1))
          (then (return (i32.const -5))))
        (return (i32.const 0))))

    (i32.const -404))
)
)WAT";

class WasmEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() /
                    ("wasm_engine_test_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
        StorageConfig config;
        config.data_dir = data_dir_.string();
        storage_ = std::make_shared<StorageEngine>(config);
        ASSERT_TRUE(storage_->Initialize());
        engine_ = std::make_shared<WasmEngine>(storage_, nullptr);
        ASSERT_TRUE(engine_->Initialize());
    }

    void TearDown() override {
        engine_->Shutdown();
        storage_->Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(data_dir_, ec);
    }

    WasmResult Run(const std::string& reducer) {
        return engine_->ExecuteReducer("m", reducer,
                                       engine_->CreateReducerContext("test"), {});
    }

    std::filesystem::path data_dir_;
    std::shared_ptr<StorageEngine> storage_;
    std::shared_ptr<WasmEngine> engine_;
};

TEST_F(WasmEngineTest, RejectsGarbageBytes) {
    std::vector<uint8_t> garbage = {'H', 'e', 'l', 'l', 'o'};
    EXPECT_FALSE(engine_->LoadModule("bad", garbage));
    EXPECT_NE(engine_->GetLastLoadError().find("invalid WASM module"),
              std::string::npos);
}

TEST_F(WasmEngineTest, RejectsMissingRequiredExports) {
    auto bytes = Wat2Wasm("(module (memory (export \"memory\") 1))");
    ASSERT_FALSE(bytes.empty());
    EXPECT_FALSE(engine_->LoadModule("noexports", bytes));
    EXPECT_NE(engine_->GetLastLoadError().find("origindb_invoke"),
              std::string::npos);
}

TEST_F(WasmEngineTest, RejectsUnknownImports) {
    auto bytes = Wat2Wasm(R"((module
        (import "env" "evil_syscall" (func (param i32)))
        (memory (export "memory") 1)
        (func (export "origindb_alloc") (param i32) (result i32) (i32.const 0))
        (func (export "origindb_invoke") (param i32 i32 i32 i32) (result i32)
          (i32.const 0))))");
    ASSERT_FALSE(bytes.empty());
    EXPECT_FALSE(engine_->LoadModule("evil", bytes));
    EXPECT_NE(engine_->GetLastLoadError().find("disallowed import"),
              std::string::npos);
}

TEST_F(WasmEngineTest, DeploysAndListsMetadata) {
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat), "2.0.0"));
    auto module = engine_->GetModule("m");
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(module->GetMetadata().version, "2.0.0");
    const auto& exports = module->GetMetadata().exports;
    EXPECT_NE(std::find(exports.begin(), exports.end(), "origindb_invoke"),
              exports.end());
}

TEST_F(WasmEngineTest, WriteCommitsToStorage) {
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));
    auto result = Run("w");
    ASSERT_TRUE(result.success) << result.error;

    auto row = storage_->Get("t", "k1");
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->columns.count("a"));
    EXPECT_EQ(std::get<int64_t>(row->columns.at("a")), 1);
}

TEST_F(WasmEngineTest, ReadsOwnStagedWrite) {
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));
    auto result = Run("rd");
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_FALSE(result.values.empty());
    auto payload = std::get<std::vector<uint8_t>>(result.values[0]);
    std::string json(payload.begin(), payload.end());
    EXPECT_NE(json.find("\"a\""), std::string::npos) << json;
}

TEST_F(WasmEngineTest, ResultPayloadRoundTrips) {
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));
    auto result = Run("res");
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_FALSE(result.values.empty());
    auto payload = std::get<std::vector<uint8_t>>(result.values[0]);
    EXPECT_EQ(std::string(payload.begin(), payload.end()), "hello");
}

TEST_F(WasmEngineTest, AbortRollsBackStagedWrites) {
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));
    auto result = Run("abort");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("boom"), std::string::npos) << result.error;
    // The write staged before host_abort must not have committed.
    EXPECT_FALSE(storage_->Get("t", "k1").has_value());

    // The instance was poisoned; the next call must still work.
    auto retry = Run("w");
    EXPECT_TRUE(retry.success) << retry.error;
    EXPECT_TRUE(storage_->Get("t", "k1").has_value());
}

TEST_F(WasmEngineTest, InfiniteLoopTimesOut) {
    engine_->SetTimeoutMs(200);
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));

    auto start = std::chrono::steady_clock::now();
    auto result = Run("loop");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_FALSE(result.success);
    EXPECT_LT(elapsed.count(), 5000) << "timeout did not fire promptly";
}

TEST_F(WasmEngineTest, MemoryLimitEnforced) {
    engine_->SetMemoryLimitMB(2);
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));
    auto result = Run("growalot");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("-5"), std::string::npos) << result.error;
}

TEST_F(WasmEngineTest, CapabilityDeniesDisallowedTable) {
    ModuleCapabilities caps;
    caps.allowed_tables = {"t"};
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat), "", caps));

    auto denied = Run("wsecret");
    EXPECT_FALSE(denied.success);
    EXPECT_NE(denied.error.find("-2"), std::string::npos) << denied.error;
    EXPECT_FALSE(storage_->Get("secret", "k1").has_value());

    auto allowed = Run("w");
    EXPECT_TRUE(allowed.success) << allowed.error;
}

TEST_F(WasmEngineTest, ReadOnlyCapabilityDeniesWrites) {
    ModuleCapabilities caps;
    caps.read_only = true;
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat), "", caps));
    auto result = Run("w");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(storage_->Get("t", "k1").has_value());
}

TEST_F(WasmEngineTest, UnknownReducerFailsButLifecycleNoops) {
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));

    auto unknown = Run("nope");
    EXPECT_FALSE(unknown.success);
    EXPECT_NE(unknown.error.find("unknown reducer"), std::string::npos);

    // Reserved lifecycle names without a handler are harmless no-ops.
    auto lifecycle = engine_->OnClientConnected(
        "m", engine_->CreateReducerContext("test", "conn-1"));
    EXPECT_TRUE(lifecycle.success) << lifecycle.error;
}

TEST_F(WasmEngineTest, ModulesExecuteConcurrently) {
    ASSERT_TRUE(engine_->LoadModule("m", Wat2Wasm(kTestModuleWat)));
    ASSERT_TRUE(engine_->LoadModule("m2", Wat2Wasm(kTestModuleWat)));

    std::atomic<int> failures{0};
    auto worker = [&](const std::string& module) {
        for (int i = 0; i < 20; i++) {
            auto result = engine_->ExecuteReducer(
                module, "w", engine_->CreateReducerContext("test"), {});
            if (!result.success) failures++;
        }
    };
    std::thread t1(worker, "m");
    std::thread t2(worker, "m2");
    t1.join();
    t2.join();
    EXPECT_EQ(failures.load(), 0);
}

}  // namespace
}  // namespace origindb
