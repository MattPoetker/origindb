#include "framework/performance_test.h"
#include "storage/storage_perf_test.h"
#include "websocket/simple_websocket_test.h"
// Disabled due to missing dependencies
// #include "grpc/grpc_load_test.h"

#include <spdlog/spdlog.h>
#include <iostream>
#include <string>
#include <memory>

using namespace instantdb::performance;

void PrintUsage() {
    std::cout << R"(
InstantDB Performance Test Runner

Usage: perf_test_runner [OPTIONS] --test <TEST_TYPE>

Test Types:
  storage-crud         - Storage engine CRUD operations test
  storage-batch        - Storage engine batch operations test
  storage-transaction  - Storage engine transaction test
  storage-scan         - Storage engine scan operations test
  storage-concurrency  - Storage engine concurrency stress test
  storage-memory       - Storage engine memory pressure test

  websocket-scaling    - WebSocket connection scaling test
  websocket-throughput - WebSocket message throughput test
  websocket-broadcast  - WebSocket broadcast performance test
  websocket-latency    - WebSocket latency measurement test
  websocket-stability  - WebSocket connection stability test

  grpc-sql             - gRPC SQL query performance test
  grpc-transaction     - gRPC transaction performance test
  grpc-wasm-deploy     - gRPC WASM module deployment test
  grpc-wasm-reducer    - gRPC WASM reducer execution test
  grpc-mixed           - gRPC mixed workload test
  grpc-connections     - gRPC concurrent connections test

  integration-full     - Full system integration test
  integration-realtime - Real-time event simulation test

Options:
  --test <TYPE>        - Test type to run (required)
  --threads <N>        - Number of threads (default: 4)
  --duration <N>       - Test duration in seconds (default: 60)
  --warmup <N>         - Warmup duration in seconds (default: 5)
  --target-ops <N>     - Target operations per second (default: unlimited)
  --output <FILE>      - Output file for results (default: test_results.json)
  --server <HOST:PORT> - Server address (default: localhost:9090)
  --grpc-server <ADDR> - gRPC server address (default: localhost:50051)
  --batch-size <N>     - Batch size for batch operations (default: 100)
  --connections <N>    - Max connections for connection tests (default: 1000)
  --message-size <N>   - Message size in bytes for throughput tests (default: 1024)
  --verbose            - Enable verbose logging
  --help               - Show this help message

Examples:
  # Basic storage CRUD test
  perf_test_runner --test storage-crud --threads 8 --duration 120

  # WebSocket scaling test with 5000 connections
  perf_test_runner --test websocket-scaling --connections 5000 --duration 300

  # gRPC mixed workload with custom server
  perf_test_runner --test grpc-mixed --grpc-server myserver:50051 --threads 16

  # Full integration test with comprehensive reporting
  perf_test_runner --test integration-full --duration 600 --output full_test.json

)";
}

struct CommandLineArgs {
    std::string test_type;
    uint32_t num_threads = 4;
    uint32_t duration_seconds = 60;
    uint32_t warmup_seconds = 5;
    uint32_t target_ops_per_second = 0;
    std::string output_file = "test_results.json";
    std::string server_address = "localhost:9090";
    std::string grpc_server_address = "localhost:50051";
    uint32_t batch_size = 100;
    uint32_t max_connections = 1000;
    uint32_t message_size = 1024;
    bool verbose = false;
    bool help = false;
};

bool ParseCommandLine(int argc, char* argv[], CommandLineArgs& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.help = true;
            return true;
        } else if (arg == "--test" && i + 1 < argc) {
            args.test_type = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            args.num_threads = std::stoul(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            args.duration_seconds = std::stoul(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            args.warmup_seconds = std::stoul(argv[++i]);
        } else if (arg == "--target-ops" && i + 1 < argc) {
            args.target_ops_per_second = std::stoul(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            args.output_file = argv[++i];
        } else if (arg == "--server" && i + 1 < argc) {
            args.server_address = argv[++i];
        } else if (arg == "--grpc-server" && i + 1 < argc) {
            args.grpc_server_address = argv[++i];
        } else if (arg == "--batch-size" && i + 1 < argc) {
            args.batch_size = std::stoul(argv[++i]);
        } else if (arg == "--connections" && i + 1 < argc) {
            args.max_connections = std::stoul(argv[++i]);
        } else if (arg == "--message-size" && i + 1 < argc) {
            args.message_size = std::stoul(argv[++i]);
        } else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return false;
        }
    }

    if (args.test_type.empty()) {
        std::cerr << "Error: --test argument is required" << std::endl;
        return false;
    }

    return true;
}

TestConfig CreateTestConfig(const CommandLineArgs& args, const std::string& test_name) {
    TestConfig config;
    config.test_name = test_name;
    config.num_threads = args.num_threads;
    config.duration_seconds = args.duration_seconds;
    config.warmup_seconds = args.warmup_seconds;
    config.target_ops_per_second = args.target_ops_per_second;
    config.batch_size = args.batch_size;
    config.output_file = args.output_file;

    // Parse server address
    auto colon_pos = args.server_address.find(':');
    if (colon_pos != std::string::npos) {
        config.server_host = args.server_address.substr(0, colon_pos);
        config.server_port = static_cast<uint16_t>(std::stoul(args.server_address.substr(colon_pos + 1)));
    }

    return config;
}

std::unique_ptr<PerformanceTest> CreateStorageTest(const CommandLineArgs& args) {
    auto test_config = CreateTestConfig(args, "Storage " + args.test_type);
    auto data_config = StorageTestFactory::GetMediumDataConfig();

    if (args.test_type == "storage-crud") {
        return StorageTestFactory::CreateCrudTest(test_config, data_config);
    } else if (args.test_type == "storage-batch") {
        return StorageTestFactory::CreateBatchTest(test_config, data_config, args.batch_size);
    } else if (args.test_type == "storage-transaction") {
        return StorageTestFactory::CreateTransactionTest(test_config, data_config);
    } else if (args.test_type == "storage-scan") {
        return StorageTestFactory::CreateScanTest(test_config, data_config, 1000);
    } else if (args.test_type == "storage-concurrency") {
        return StorageTestFactory::CreateConcurrencyTest(test_config, data_config);
    } else if (args.test_type == "storage-memory") {
        return StorageTestFactory::CreateMemoryPressureTest(test_config, data_config, 500);
    }

    return nullptr;
}

std::unique_ptr<PerformanceTest> CreateWebSocketTest(const CommandLineArgs& args) {
    auto test_config = CreateTestConfig(args, "WebSocket " + args.test_type);
    auto ws_config = SimpleWebSocketTestFactory::GetDefaultClientConfig();
    ws_config.server_host = test_config.server_host;
    ws_config.server_port = test_config.server_port;

    if (args.test_type == "websocket-scaling") {
        return SimpleWebSocketTestFactory::CreateConnectionScalingTest(test_config, ws_config, args.max_connections);
    } else if (args.test_type == "websocket-throughput") {
        return SimpleWebSocketTestFactory::CreateThroughputTest(test_config, ws_config, args.message_size);
    } else if (args.test_type == "websocket-latency") {
        return SimpleWebSocketTestFactory::CreateLatencyTest(test_config, ws_config);
    }

    return nullptr;
}

// gRPC tests disabled due to missing dependencies
std::unique_ptr<PerformanceTest> CreateGrpcTest(const CommandLineArgs& args) {
    spdlog::error("❌ gRPC tests are disabled due to missing dependencies");
    return nullptr;
}

void RunComprehensiveSuite(const CommandLineArgs& args) {
    spdlog::info("🚀 Running comprehensive performance test suite (Storage tests only)");

    TestSuite suite;

    // Add storage tests
    auto storage_config = CreateTestConfig(args, "Storage Suite");
    storage_config.duration_seconds = std::min(args.duration_seconds, 120u); // Limit duration for suite
    auto data_config = StorageTestFactory::GetMediumDataConfig();

    suite.AddTest(StorageTestFactory::CreateCrudTest(storage_config, data_config));
    suite.AddTest(StorageTestFactory::CreateBatchTest(storage_config, data_config, 100));
    suite.AddTest(StorageTestFactory::CreateConcurrencyTest(storage_config, data_config));

    // Add simple WebSocket tests
    auto ws_config = SimpleWebSocketTestFactory::GetDefaultClientConfig();
    ws_config.server_host = storage_config.server_host;
    ws_config.server_port = storage_config.server_port;

    suite.AddTest(SimpleWebSocketTestFactory::CreateConnectionScalingTest(storage_config, ws_config, 50));
    suite.AddTest(SimpleWebSocketTestFactory::CreateThroughputTest(storage_config, ws_config, 512));

    // gRPC tests disabled due to missing dependencies
    spdlog::warn("⚠️  gRPC tests are disabled in this build");

    // Run all tests
    suite.RunAll();

    // Save comprehensive results
    suite.SaveResults("./tests/performance/reports/comprehensive_suite");

    spdlog::info("✅ Comprehensive test suite completed!");
}

int main(int argc, char* argv[]) {
    CommandLineArgs args;

    if (!ParseCommandLine(argc, argv, args)) {
        PrintUsage();
        return 1;
    }

    if (args.help) {
        PrintUsage();
        return 0;
    }

    // Configure logging
    if (args.verbose) {
        spdlog::set_level(spdlog::level::debug);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    spdlog::info("🧪 InstantDB Performance Test Runner");
    spdlog::info("Test Type: {}", args.test_type);
    spdlog::info("Threads: {}, Duration: {}s, Warmup: {}s",
                 args.num_threads, args.duration_seconds, args.warmup_seconds);

    std::unique_ptr<PerformanceTest> test;

    // Create appropriate test based on type
    if (args.test_type.find("storage-") == 0) {
        test = CreateStorageTest(args);
    } else if (args.test_type.find("websocket-") == 0) {
        test = CreateWebSocketTest(args);
    } else if (args.test_type.find("grpc-") == 0) {
        test = CreateGrpcTest(args);
    } else if (args.test_type == "integration-full") {
        RunComprehensiveSuite(args);
        return 0;
    } else {
        spdlog::error("❌ Unknown test type: {}", args.test_type);
        PrintUsage();
        return 1;
    }

    if (!test) {
        spdlog::error("❌ Failed to create test for type: {}", args.test_type);
        return 1;
    }

    // Run the test
    bool success = test->Run();

    // Print results
    test->PrintSummary();

    // Save results
    test->SaveResults();

    if (success) {
        spdlog::info("✅ Test completed successfully!");
        return 0;
    } else {
        spdlog::error("❌ Test failed!");
        return 1;
    }
}