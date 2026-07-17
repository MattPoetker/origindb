#include "storage_perf_test.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cassert>

namespace origindb {
namespace performance {

// TestDataGenerator Implementation
TestDataGenerator::TestDataGenerator(const DataGenConfig& config)
    : config_(config), generator_(std::random_device{}()),
      value_size_dist_(config_.value_size_min, config_.value_size_max),
      column_type_dist_(0.0, 1.0) {
}

std::string TestDataGenerator::GenerateRandomString(uint32_t length) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result;
    result.reserve(length);

    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
    for (uint32_t i = 0; i < length; ++i) {
        result += charset[dist(generator_)];
    }
    return result;
}

Value TestDataGenerator::GenerateRandomValue() {
    double type_selector = column_type_dist_(generator_);

    if (type_selector < config_.string_column_ratio) {
        uint32_t str_length = value_size_dist_(generator_) / 4; // Rough estimate
        return Value(GenerateRandomString(str_length));
    } else if (type_selector < config_.string_column_ratio + config_.int_column_ratio) {
        std::uniform_int_distribution<int64_t> int_dist(-1000000, 1000000);
        return Value(int_dist(generator_));
    } else {
        std::uniform_real_distribution<double> float_dist(-1000.0, 1000.0);
        return Value(float_dist(generator_));
    }
}

Row TestDataGenerator::GenerateRandomRow() {
    std::string key = config_.key_prefix + std::to_string(generator_());
    return GenerateRandomRow(key);
}

Row TestDataGenerator::GenerateRandomRow(const std::string& key) {
    Row row;
    row.key = key;

    for (uint32_t i = 0; i < config_.num_columns; ++i) {
        std::string column_name = "col_" + std::to_string(i);
        row.columns[column_name] = GenerateRandomValue();
    }

    return row;
}

std::vector<Row> TestDataGenerator::GenerateBatch(uint32_t count) {
    std::vector<Row> batch;
    batch.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        batch.push_back(GenerateRandomRow());
    }

    return batch;
}

TableSchema TestDataGenerator::GenerateSchema() {
    TableSchema schema;
    schema.name = config_.table_name;

    for (uint32_t i = 0; i < config_.num_columns; ++i) {
        Column col;
        col.name = "col_" + std::to_string(i);

        double type_selector = column_type_dist_(generator_);
        if (type_selector < config_.string_column_ratio) {
            col.type = DataType::STRING;
        } else if (type_selector < config_.string_column_ratio + config_.int_column_ratio) {
            col.type = DataType::INT64;
        } else {
            col.type = DataType::DOUBLE;
        }

        schema.columns.push_back(col);
    }

    return schema;
}

// StoragePerformanceTest Implementation
StoragePerformanceTest::StoragePerformanceTest(const TestConfig& config, const DataGenConfig& data_config)
    : PerformanceTest(config), data_config_(data_config), data_generator_(data_config) {
}

bool StoragePerformanceTest::Setup() {
    // Initialize storage engine
    StorageConfig storage_config;
    storage_config.data_dir = "./test_data/perf_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    storage_config.sync_wal = true;
    storage_config.max_memory_bytes = 128 * 1024 * 1024;

    std::filesystem::create_directories(storage_config.data_dir);

    storage_ = std::make_shared<StorageEngine>(storage_config);
    if (!storage_->Initialize()) {
        spdlog::error("Failed to initialize storage engine");
        return false;
    }

    if (!CreateTestTable()) {
        spdlog::error("Failed to create test table");
        return false;
    }

    return true;
}

void StoragePerformanceTest::Cleanup() {
    if (storage_) {
        storage_->Shutdown();
        storage_.reset();
    }
}

bool StoragePerformanceTest::CreateTestTable() {
    auto schema = data_generator_.GenerateSchema();
    return storage_->CreateTable(schema);
}

void StoragePerformanceTest::PrepopulateData(uint32_t num_records) {
    spdlog::info("Prepopulating {} records...", num_records);

    const uint32_t batch_size = 1000;
    for (uint32_t i = 0; i < num_records; i += batch_size) {
        uint32_t current_batch_size = std::min(batch_size, num_records - i);
        auto batch = data_generator_.GenerateBatch(current_batch_size);

        for (const auto& row : batch) {
            storage_->Insert(data_config_.table_name, row);
        }

        if (i % 10000 == 0) {
            spdlog::debug("Prepopulated {} records", i);
        }
    }

    spdlog::info("Prepopulation complete");
}

std::string StoragePerformanceTest::GenerateKey(uint32_t thread_id, uint32_t sequence) {
    return data_config_.key_prefix + std::to_string(thread_id) + "_" + std::to_string(sequence);
}

// CrudPerformanceTest Implementation
CrudPerformanceTest::CrudPerformanceTest(const TestConfig& config,
                                       const DataGenConfig& data_config,
                                       OperationType operation_type)
    : StoragePerformanceTest(config, data_config), operation_type_(operation_type) {
}

void CrudPerformanceTest::RunWorker(uint32_t thread_id) {
    switch (operation_type_) {
        case OperationType::INSERT_ONLY:
            ExecuteInsert(thread_id);
            break;
        case OperationType::SELECT_ONLY:
            ExecuteSelect(thread_id);
            break;
        case OperationType::UPDATE_ONLY:
            ExecuteUpdate(thread_id);
            break;
        case OperationType::DELETE_ONLY:
            ExecuteDelete(thread_id);
            break;
        case OperationType::MIXED_CRUD:
            ExecuteMixedCrud(thread_id);
            break;
    }
}

void CrudPerformanceTest::ExecuteInsert(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string key = GenerateKey(thread_id, sequence);
        Row row = data_generator_.GenerateRandomRow(key);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = storage_->Insert(data_config_.table_name, row);
        auto op_end = std::chrono::high_resolution_clock::now();

        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

void CrudPerformanceTest::ExecuteSelect(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string key = GenerateKey(thread_id % config_.num_threads, sequence % 1000);

        auto op_start = std::chrono::high_resolution_clock::now();
        Row result;
        auto result_opt = storage_->Get(data_config_.table_name, key);
        bool success = result_opt.has_value();
        auto op_end = std::chrono::high_resolution_clock::now();

        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

void CrudPerformanceTest::ExecuteUpdate(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string key = GenerateKey(thread_id % config_.num_threads, sequence % 1000);
        Row updated_row = data_generator_.GenerateRandomRow(key);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = storage_->Update(data_config_.table_name, key, updated_row);
        auto op_end = std::chrono::high_resolution_clock::now();

        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

void CrudPerformanceTest::ExecuteDelete(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string key = GenerateKey(thread_id % config_.num_threads, sequence % 1000);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = storage_->Delete(data_config_.table_name, key);
        auto op_end = std::chrono::high_resolution_clock::now();

        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

void CrudPerformanceTest::ExecuteMixedCrud(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    std::mt19937 rng(thread_id);
    std::uniform_real_distribution<double> operation_dist(0.0, 1.0);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string key = GenerateKey(thread_id, sequence);

        double op_type = operation_dist(rng);
        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = false;

        if (op_type < 0.4) { // 40% SELECT
            Row result;
            auto result_opt = storage_->Get(data_config_.table_name, key);
            success = result_opt.has_value();
        } else if (op_type < 0.7) { // 30% INSERT
            Row row = data_generator_.GenerateRandomRow(key);
            success = storage_->Insert(data_config_.table_name, row);
        } else if (op_type < 0.9) { // 20% UPDATE
            Row updated_row = data_generator_.GenerateRandomRow(key);
            success = storage_->Update(data_config_.table_name, key, updated_row);
        } else { // 10% DELETE
            success = storage_->Delete(data_config_.table_name, key);
        }

        auto op_end = std::chrono::high_resolution_clock::now();
        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

// BatchPerformanceTest Implementation
BatchPerformanceTest::BatchPerformanceTest(const TestConfig& config,
                                         const DataGenConfig& data_config,
                                         uint32_t batch_size)
    : StoragePerformanceTest(config, data_config), batch_size_(batch_size) {
}

void BatchPerformanceTest::RunWorker(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        std::vector<Row> batch;
        batch.reserve(batch_size_);

        for (uint32_t i = 0; i < batch_size_; ++i) {
            uint32_t sequence = global_sequence_.fetch_add(1);
            std::string key = GenerateKey(thread_id, sequence);
            batch.push_back(data_generator_.GenerateRandomRow(key));
        }

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = true;
        for (const auto& row : batch) {
            if (!storage_->Insert(data_config_.table_name, row)) {
                success = false;
                break;
            }
        }
        auto op_end = std::chrono::high_resolution_clock::now();

        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations += batch_size_;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

// TransactionPerformanceTest Implementation
TransactionPerformanceTest::TransactionPerformanceTest(const TestConfig& config,
                                                     const DataGenConfig& data_config,
                                                     IsolationLevel isolation_level)
    : StoragePerformanceTest(config, data_config), isolation_level_(isolation_level) {
}

void TransactionPerformanceTest::RunWorker(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        ExecuteTransaction(thread_id, 5); // 5 operations per transaction
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

void TransactionPerformanceTest::ExecuteTransaction(uint32_t thread_id, uint32_t ops_per_txn) {
    auto op_start = std::chrono::high_resolution_clock::now();

    auto transaction = storage_->BeginTransaction(isolation_level_);
    bool success = true;

    try {
        for (uint32_t i = 0; i < ops_per_txn && success; ++i) {
            uint32_t sequence = global_sequence_.fetch_add(1);
            std::string key = GenerateKey(thread_id, sequence);
            Row row = data_generator_.GenerateRandomRow(key);

            success = transaction->Insert(data_config_.table_name, row);
        }

        if (success) {
            success = storage_->Commit(transaction);
        } else {
            storage_->Rollback(transaction);
        }
    } catch (const std::exception& e) {
        storage_->Rollback(transaction);
        success = false;
    }

    auto op_end = std::chrono::high_resolution_clock::now();
    if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
}

// ScanPerformanceTest Implementation
ScanPerformanceTest::ScanPerformanceTest(const TestConfig& config,
                                       const DataGenConfig& data_config,
                                       uint32_t scan_range_size)
    : StoragePerformanceTest(config, data_config),
      scan_range_size_(scan_range_size), total_records_(10000) {
}

bool ScanPerformanceTest::Setup() {
    if (!StoragePerformanceTest::Setup()) {
        return false;
    }

    // Prepopulate data for scanning
    PrepopulateData(total_records_);
    return true;
}

void ScanPerformanceTest::RunWorker(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    std::mt19937 rng(thread_id);
    std::uniform_int_distribution<uint32_t> start_dist(0, total_records_ - scan_range_size_);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t start_idx = start_dist(rng);
        std::string start_key = GenerateKey(0, start_idx);
        std::string end_key = GenerateKey(0, start_idx + scan_range_size_);

        auto op_start = std::chrono::high_resolution_clock::now();
        std::vector<Row> results;
        uint32_t count = 0;
        bool success = true;
        storage_->Scan(data_config_.table_name, start_key, end_key,
            [&count](const std::string&, const Row&) -> bool {
                count++;
                return true;
            });
        auto op_end = std::chrono::high_resolution_clock::now();

        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

void ScanPerformanceTest::ExecuteScan(uint32_t thread_id) {
    // Implementation moved to RunWorker
}

// ConcurrencyStressTest Implementation
ConcurrencyStressTest::ConcurrencyStressTest(const TestConfig& config,
                                           const DataGenConfig& data_config,
                                           double read_write_ratio)
    : StoragePerformanceTest(config, data_config),
      read_write_ratio_(read_write_ratio), rng_(std::random_device{}()) {
}

void ConcurrencyStressTest::RunWorker(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    std::mt19937 local_rng(thread_id);
    std::uniform_real_distribution<double> operation_dist(0.0, 1.0);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string key = GenerateKey(thread_id, sequence % 1000); // Reuse keys for conflicts

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = false;

        if (operation_dist(local_rng) < read_write_ratio_) {
            // Read operation
            Row result;
            auto result_opt = storage_->Get(data_config_.table_name, key);
            success = result_opt.has_value();
        } else {
            // Write operation
            Row row = data_generator_.GenerateRandomRow(key);
            success = storage_->Insert(data_config_.table_name, row);
        }

        auto op_end = std::chrono::high_resolution_clock::now();
        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

// MemoryPressureTest Implementation
MemoryPressureTest::MemoryPressureTest(const TestConfig& config,
                                     const DataGenConfig& data_config,
                                     uint64_t target_memory_mb)
    : StoragePerformanceTest(config, data_config), target_memory_mb_(target_memory_mb) {
}

void MemoryPressureTest::RunWorker(uint32_t thread_id) {
    ExecuteMemoryIntensiveOperations(thread_id);
}

void MemoryPressureTest::ExecuteMemoryIntensiveOperations(uint32_t thread_id) {
    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    // Create large data records to consume memory
    DataGenConfig large_config = data_config_;
    large_config.value_size_min = 10000;  // 10KB min
    large_config.value_size_max = 50000;  // 50KB max
    TestDataGenerator large_generator(large_config);

    while (std::chrono::steady_clock::now() < end_time && !stop_requested_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string key = GenerateKey(thread_id, sequence);
        Row large_row = large_generator.GenerateRandomRow(key);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = storage_->Insert(data_config_.table_name, large_row);
        auto op_end = std::chrono::high_resolution_clock::now();

        if (success) {
            metrics_->operations_completed.fetch_add(1);
        } else {
            metrics_->operations_failed.fetch_add(1);
        }

        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(op_end - op_start).count();
        std::lock_guard<std::mutex> lock(metrics_->latencies_mutex);
        metrics_->latencies.push_back(static_cast<double>(latency));
        operations++;

        if (config_.target_ops_per_second > 0) {
            // Rate limiting - simple throttle
            if (config_.target_ops_per_second > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count();
                auto expected_ops = (elapsed / 1000000.0) * config_.target_ops_per_second;
                if (operations > expected_ops) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                }
            }
        }
    }
}

// StorageTestFactory Implementation
std::unique_ptr<StoragePerformanceTest> StorageTestFactory::CreateCrudTest(
    const TestConfig& test_config,
    const DataGenConfig& data_config,
    CrudPerformanceTest::OperationType operation_type) {

    return std::make_unique<CrudPerformanceTest>(test_config, data_config, operation_type);
}

std::unique_ptr<StoragePerformanceTest> StorageTestFactory::CreateBatchTest(
    const TestConfig& test_config,
    const DataGenConfig& data_config,
    uint32_t batch_size) {

    return std::make_unique<BatchPerformanceTest>(test_config, data_config, batch_size);
}

std::unique_ptr<StoragePerformanceTest> StorageTestFactory::CreateTransactionTest(
    const TestConfig& test_config,
    const DataGenConfig& data_config,
    IsolationLevel isolation_level) {

    return std::make_unique<TransactionPerformanceTest>(test_config, data_config, isolation_level);
}

std::unique_ptr<StoragePerformanceTest> StorageTestFactory::CreateScanTest(
    const TestConfig& test_config,
    const DataGenConfig& data_config,
    uint32_t scan_range_size) {

    return std::make_unique<ScanPerformanceTest>(test_config, data_config, scan_range_size);
}

std::unique_ptr<StoragePerformanceTest> StorageTestFactory::CreateConcurrencyTest(
    const TestConfig& test_config,
    const DataGenConfig& data_config,
    double read_write_ratio) {

    return std::make_unique<ConcurrencyStressTest>(test_config, data_config, read_write_ratio);
}

std::unique_ptr<StoragePerformanceTest> StorageTestFactory::CreateMemoryPressureTest(
    const TestConfig& test_config,
    const DataGenConfig& data_config,
    uint64_t target_memory_mb) {

    return std::make_unique<MemoryPressureTest>(test_config, data_config, target_memory_mb);
}

// Predefined configurations
TestConfig StorageTestFactory::GetLightLoadConfig() {
    TestConfig config;
    config.test_name = "Light Storage Load";
    config.num_threads = 2;
    config.duration_seconds = 30;
    config.warmup_seconds = 5;
    return config;
}

TestConfig StorageTestFactory::GetMediumLoadConfig() {
    TestConfig config;
    config.test_name = "Medium Storage Load";
    config.num_threads = 8;
    config.duration_seconds = 120;
    config.warmup_seconds = 10;
    return config;
}

TestConfig StorageTestFactory::GetHeavyLoadConfig() {
    TestConfig config;
    config.test_name = "Heavy Storage Load";
    config.num_threads = 16;
    config.duration_seconds = 300;
    config.warmup_seconds = 15;
    return config;
}

TestConfig StorageTestFactory::GetStressTestConfig() {
    TestConfig config;
    config.test_name = "Storage Stress Test";
    config.num_threads = 32;
    config.duration_seconds = 600;
    config.warmup_seconds = 30;
    return config;
}

DataGenConfig StorageTestFactory::GetSmallDataConfig() {
    DataGenConfig config;
    config.key_length = 16;
    config.value_size_min = 50;
    config.value_size_max = 200;
    config.num_columns = 3;
    return config;
}

DataGenConfig StorageTestFactory::GetMediumDataConfig() {
    DataGenConfig config;
    config.key_length = 32;
    config.value_size_min = 100;
    config.value_size_max = 1000;
    config.num_columns = 5;
    return config;
}

DataGenConfig StorageTestFactory::GetLargeDataConfig() {
    DataGenConfig config;
    config.key_length = 64;
    config.value_size_min = 1000;
    config.value_size_max = 10000;
    config.num_columns = 10;
    return config;
}

} // namespace performance
} // namespace origindb