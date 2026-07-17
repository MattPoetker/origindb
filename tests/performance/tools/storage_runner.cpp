#include "../storage/storage_perf_test.h"
#include <spdlog/spdlog.h>
#include <iostream>

using namespace origindb::performance;

void PrintUsage() {
    std::cout << R"(
Storage Performance Test Runner

Usage: storage_perf_runner [OPTIONS]

Options:
  --test <TYPE>        Test type: crud, batch, transaction, scan, concurrency, memory
  --threads <N>        Number of threads (default: 4)
  --duration <N>       Test duration in seconds (default: 60)
  --output <FILE>      Output file for results (default: storage_results.json)
  --verbose            Enable verbose logging
  --help               Show this help message

Examples:
  storage_perf_runner --test crud --threads 8 --duration 120
  storage_perf_runner --test concurrency --threads 16 --duration 300

)";
}

int main(int argc, char* argv[]) {
    std::string test_type = "crud";
    uint32_t num_threads = 4;
    uint32_t duration_seconds = 60;
    std::string output_file = "storage_results.json";
    bool verbose = false;
    bool help = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            help = true;
        } else if (arg == "--test" && i + 1 < argc) {
            test_type = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            num_threads = std::stoul(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_seconds = std::stoul(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        }
    }

    if (help) {
        PrintUsage();
        return 0;
    }

    // Configure logging
    if (verbose) {
        spdlog::set_level(spdlog::level::debug);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    spdlog::info("🗄️  Storage Performance Test Runner");
    spdlog::info("Test Type: {}", test_type);
    spdlog::info("Threads: {}, Duration: {}s", num_threads, duration_seconds);

    // Create test configuration
    TestConfig config;
    config.test_name = "Storage " + test_type;
    config.num_threads = num_threads;
    config.duration_seconds = duration_seconds;
    config.warmup_seconds = 5;
    config.output_file = output_file;

    auto data_config = StorageTestFactory::GetMediumDataConfig();

    // Create appropriate test
    std::unique_ptr<StoragePerformanceTest> test;

    if (test_type == "crud") {
        test = StorageTestFactory::CreateCrudTest(config, data_config);
    } else if (test_type == "batch") {
        test = StorageTestFactory::CreateBatchTest(config, data_config, 100);
    } else if (test_type == "transaction") {
        test = StorageTestFactory::CreateTransactionTest(config, data_config);
    } else if (test_type == "scan") {
        test = StorageTestFactory::CreateScanTest(config, data_config, 1000);
    } else if (test_type == "concurrency") {
        test = StorageTestFactory::CreateConcurrencyTest(config, data_config);
    } else if (test_type == "memory") {
        test = StorageTestFactory::CreateMemoryPressureTest(config, data_config, 500);
    } else {
        spdlog::error("❌ Unknown test type: {}", test_type);
        PrintUsage();
        return 1;
    }

    if (!test) {
        spdlog::error("❌ Failed to create test");
        return 1;
    }

    // Run the test
    bool success = test->Run();

    // Print results
    test->PrintSummary();

    // Save results
    test->SaveResults();

    if (success) {
        spdlog::info("✅ Storage test completed successfully!");
        return 0;
    } else {
        spdlog::error("❌ Storage test failed!");
        return 1;
    }
}