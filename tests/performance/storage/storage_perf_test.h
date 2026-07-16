#pragma once

#include "../framework/performance_test.h"
#include "storage/storage_engine.h"
#include "common/config.h"
#include <memory>
#include <string>
#include <vector>
#include <random>

namespace instantdb {
namespace performance {

// Data generation configuration
struct DataGenConfig {
    uint32_t key_length = 32;
    uint32_t value_size_min = 100;
    uint32_t value_size_max = 1000;
    uint32_t num_columns = 5;
    double string_column_ratio = 0.4;
    double int_column_ratio = 0.3;
    double float_column_ratio = 0.3;
    std::string key_prefix = "key_";
    std::string table_name = "perf_test_table";
};

// Test data generator utility
class TestDataGenerator {
public:
    explicit TestDataGenerator(const DataGenConfig& config);

    Row GenerateRandomRow();
    Row GenerateRandomRow(const std::string& key);
    std::vector<Row> GenerateBatch(uint32_t count);
    TableSchema GenerateSchema();

    void SetSeed(uint32_t seed) { generator_.seed(seed); }

private:
    DataGenConfig config_;
    mutable std::mt19937 generator_;
    std::uniform_int_distribution<uint32_t> value_size_dist_;
    std::uniform_real_distribution<double> column_type_dist_;

    std::string GenerateRandomString(uint32_t length);
    Value GenerateRandomValue();
};

// Base class for storage performance tests
class StoragePerformanceTest : public PerformanceTest {
public:
    StoragePerformanceTest(const TestConfig& config, const DataGenConfig& data_config);
    virtual ~StoragePerformanceTest() = default;

    bool Setup() override;
    void Cleanup() override;

protected:
    std::shared_ptr<StorageEngine> storage_;
    DataGenConfig data_config_;
    TestDataGenerator data_generator_;

    // Helper methods
    bool CreateTestTable();
    void PrepopulateData(uint32_t num_records);
    std::string GenerateKey(uint32_t thread_id, uint32_t sequence);
};

// CRUD Performance Test
class CrudPerformanceTest : public StoragePerformanceTest {
public:
    enum class OperationType {
        INSERT_ONLY,
        SELECT_ONLY,
        UPDATE_ONLY,
        DELETE_ONLY,
        MIXED_CRUD
    };

    CrudPerformanceTest(const TestConfig& config,
                       const DataGenConfig& data_config,
                       OperationType operation_type = OperationType::MIXED_CRUD);

    void RunWorker(uint32_t thread_id) override;

private:
    OperationType operation_type_;
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteInsert(uint32_t thread_id);
    void ExecuteSelect(uint32_t thread_id);
    void ExecuteUpdate(uint32_t thread_id);
    void ExecuteDelete(uint32_t thread_id);
    void ExecuteMixedCrud(uint32_t thread_id);
};

// Batch Operation Performance Test
class BatchPerformanceTest : public StoragePerformanceTest {
public:
    BatchPerformanceTest(const TestConfig& config,
                        const DataGenConfig& data_config,
                        uint32_t batch_size = 100);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t batch_size_;
    std::atomic<uint32_t> global_sequence_{0};
};

// Transaction Performance Test
class TransactionPerformanceTest : public StoragePerformanceTest {
public:
    TransactionPerformanceTest(const TestConfig& config,
                              const DataGenConfig& data_config,
                              IsolationLevel isolation_level = IsolationLevel::SNAPSHOT);

    void RunWorker(uint32_t thread_id) override;

private:
    IsolationLevel isolation_level_;
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteTransaction(uint32_t thread_id, uint32_t ops_per_txn);
};

// Scan Performance Test
class ScanPerformanceTest : public StoragePerformanceTest {
public:
    ScanPerformanceTest(const TestConfig& config,
                       const DataGenConfig& data_config,
                       uint32_t scan_range_size = 1000);

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t scan_range_size_;
    uint32_t total_records_;

    void ExecuteScan(uint32_t thread_id);
};

// Concurrency Stress Test
class ConcurrencyStressTest : public StoragePerformanceTest {
public:
    ConcurrencyStressTest(const TestConfig& config,
                         const DataGenConfig& data_config,
                         double read_write_ratio = 0.8);

    void RunWorker(uint32_t thread_id) override;

private:
    double read_write_ratio_;
    std::atomic<uint32_t> global_sequence_{0};
    std::uniform_real_distribution<double> operation_dist_{0.0, 1.0};
    mutable std::mt19937 rng_;
};

// Memory Pressure Test
class MemoryPressureTest : public StoragePerformanceTest {
public:
    MemoryPressureTest(const TestConfig& config,
                      const DataGenConfig& data_config,
                      uint64_t target_memory_mb = 1000);

    void RunWorker(uint32_t thread_id) override;

private:
    uint64_t target_memory_mb_;
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteMemoryIntensiveOperations(uint32_t thread_id);
};

// WAL Performance Test
class WalPerformanceTest : public StoragePerformanceTest {
public:
    WalPerformanceTest(const TestConfig& config,
                      const DataGenConfig& data_config,
                      bool enable_fsync = true);

    void RunWorker(uint32_t thread_id) override;

private:
    bool enable_fsync_;
    std::atomic<uint32_t> global_sequence_{0};
};

// Index Performance Test
class IndexPerformanceTest : public StoragePerformanceTest {
public:
    IndexPerformanceTest(const TestConfig& config,
                        const DataGenConfig& data_config,
                        const std::vector<std::string>& indexed_columns);

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    std::vector<std::string> indexed_columns_;
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteIndexedQueries(uint32_t thread_id);
};

// Storage performance test factory
class StorageTestFactory {
public:
    static std::unique_ptr<StoragePerformanceTest> CreateCrudTest(
        const TestConfig& test_config,
        const DataGenConfig& data_config,
        CrudPerformanceTest::OperationType operation_type = CrudPerformanceTest::OperationType::MIXED_CRUD
    );

    static std::unique_ptr<StoragePerformanceTest> CreateBatchTest(
        const TestConfig& test_config,
        const DataGenConfig& data_config,
        uint32_t batch_size = 100
    );

    static std::unique_ptr<StoragePerformanceTest> CreateTransactionTest(
        const TestConfig& test_config,
        const DataGenConfig& data_config,
        IsolationLevel isolation_level = IsolationLevel::SNAPSHOT
    );

    static std::unique_ptr<StoragePerformanceTest> CreateScanTest(
        const TestConfig& test_config,
        const DataGenConfig& data_config,
        uint32_t scan_range_size = 1000
    );

    static std::unique_ptr<StoragePerformanceTest> CreateConcurrencyTest(
        const TestConfig& test_config,
        const DataGenConfig& data_config,
        double read_write_ratio = 0.8
    );

    static std::unique_ptr<StoragePerformanceTest> CreateMemoryPressureTest(
        const TestConfig& test_config,
        const DataGenConfig& data_config,
        uint64_t target_memory_mb = 1000
    );

    // Predefined test configurations
    static TestConfig GetLightLoadConfig();
    static TestConfig GetMediumLoadConfig();
    static TestConfig GetHeavyLoadConfig();
    static TestConfig GetStressTestConfig();

    static DataGenConfig GetSmallDataConfig();
    static DataGenConfig GetMediumDataConfig();
    static DataGenConfig GetLargeDataConfig();
};

} // namespace performance
} // namespace instantdb