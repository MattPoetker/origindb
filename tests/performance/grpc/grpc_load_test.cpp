#include "grpc_load_test.h"
#include <spdlog/spdlog.h>
#include <random>
#include <chrono>
#include <fstream>

namespace instantdb {
namespace performance {

// GrpcClient Implementation
GrpcClient::GrpcClient(const GrpcClientConfig& config, uint32_t client_id)
    : config_(config), client_id_(client_id) {
}

GrpcClient::~GrpcClient() {
    Disconnect();
}

bool GrpcClient::Connect() {
    if (connected_.load()) {
        return true;
    }

    try {
        grpc::ChannelArguments args;
        if (config_.enable_keepalive) {
            args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, config_.keepalive_time_ms);
            args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, config_.keepalive_timeout_ms);
            args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        }

        channel_ = grpc::CreateCustomChannel(config_.server_address, grpc::InsecureChannelCredentials(), args);

        if (!channel_) {
            spdlog::error("Client {}: Failed to create gRPC channel", client_id_);
            connection_errors_.fetch_add(1);
            return false;
        }

        // Test connection with deadline
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(config_.connection_timeout_ms);
        if (!channel_->WaitForConnected(deadline)) {
            spdlog::error("Client {}: Failed to connect to gRPC server within timeout", client_id_);
            connection_errors_.fetch_add(1);
            return false;
        }

        sql_stub_ = instantdb::grpc::SQLService::NewStub(channel_);
        wasm_stub_ = instantdb::grpc::WasmService::NewStub(channel_);

        if (!sql_stub_ || !wasm_stub_) {
            spdlog::error("Client {}: Failed to create gRPC stubs", client_id_);
            connection_errors_.fetch_add(1);
            return false;
        }

        connected_.store(true);
        spdlog::debug("Client {}: Connected to gRPC server", client_id_);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Client {}: Connection exception: {}", client_id_, e.what());
        connection_errors_.fetch_add(1);
        return false;
    }
}

void GrpcClient::Disconnect() {
    if (connected_.load()) {
        channel_.reset();
        sql_stub_.reset();
        wasm_stub_.reset();
        connected_.store(false);
        spdlog::debug("Client {}: Disconnected", client_id_);
    }
}

bool GrpcClient::IsConnected() const {
    return connected_.load() && channel_ && channel_->GetState(false) == GRPC_CHANNEL_READY;
}

bool GrpcClient::ExecuteSQL(const std::string& query, instantdb::grpc::SQLResponse* response) {
    if (!IsConnected()) {
        RecordRequest(false);
        return false;
    }

    try {
        instantdb::grpc::SQLRequest request;
        request.set_query(query);

        grpc::ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms);
        context.set_deadline(deadline);

        grpc::Status status = sql_stub_->Execute(&context, request, response);
        bool success = status.ok();

        if (!success) {
            spdlog::warn("Client {}: SQL execution failed: {}", client_id_, status.error_message());
        }

        RecordRequest(success);
        return success;

    } catch (const std::exception& e) {
        spdlog::warn("Client {}: SQL execution exception: {}", client_id_, e.what());
        RecordRequest(false);
        return false;
    }
}

bool GrpcClient::ExecuteTransaction(const std::vector<std::string>& queries,
                                  instantdb::grpc::SQLTransactionResponse* response) {
    if (!IsConnected()) {
        RecordRequest(false);
        return false;
    }

    try {
        instantdb::grpc::SQLTransactionRequest request;
        for (const auto& query : queries) {
            request.add_queries(query);
        }

        grpc::ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms);
        context.set_deadline(deadline);

        grpc::Status status = sql_stub_->ExecuteTransaction(&context, request, response);
        bool success = status.ok();

        if (!success) {
            spdlog::warn("Client {}: Transaction execution failed: {}", client_id_, status.error_message());
        }

        RecordRequest(success);
        return success;

    } catch (const std::exception& e) {
        spdlog::warn("Client {}: Transaction execution exception: {}", client_id_, e.what());
        RecordRequest(false);
        return false;
    }
}

bool GrpcClient::GetServerStatus(instantdb::grpc::StatusResponse* response) {
    if (!IsConnected()) {
        RecordRequest(false);
        return false;
    }

    try {
        instantdb::grpc::StatusRequest request;

        grpc::ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms);
        context.set_deadline(deadline);

        grpc::Status status = sql_stub_->GetStatus(&context, request, response);
        bool success = status.ok();

        RecordRequest(success);
        return success;

    } catch (const std::exception& e) {
        spdlog::warn("Client {}: Status request exception: {}", client_id_, e.what());
        RecordRequest(false);
        return false;
    }
}

bool GrpcClient::DeployModule(const std::string& module_name,
                             const std::vector<uint8_t>& bytecode,
                             instantdb::grpc::DeployModuleResponse* response) {
    if (!IsConnected()) {
        RecordRequest(false);
        return false;
    }

    try {
        instantdb::grpc::DeployModuleRequest request;
        request.set_module_name(module_name);
        request.set_bytecode(bytecode.data(), bytecode.size());

        grpc::ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms * 2);
        context.set_deadline(deadline);

        grpc::Status status = wasm_stub_->DeployModule(&context, request, response);
        bool success = status.ok();

        if (!success) {
            spdlog::warn("Client {}: Module deployment failed: {}", client_id_, status.error_message());
        }

        RecordRequest(success);
        return success;

    } catch (const std::exception& e) {
        spdlog::warn("Client {}: Module deployment exception: {}", client_id_, e.what());
        RecordRequest(false);
        return false;
    }
}

bool GrpcClient::ExecuteReducer(const std::string& module_name,
                               const std::string& reducer_name,
                               const std::vector<instantdb::grpc::WasmValue>& args,
                               instantdb::grpc::ExecuteReducerResponse* response) {
    if (!IsConnected()) {
        RecordRequest(false);
        return false;
    }

    try {
        instantdb::grpc::ExecuteReducerRequest request;
        request.set_module_name(module_name);
        request.set_reducer_name(reducer_name);

        for (const auto& arg : args) {
            *request.add_args() = arg;
        }

        grpc::ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms);
        context.set_deadline(deadline);

        grpc::Status status = wasm_stub_->ExecuteReducer(&context, request, response);
        bool success = status.ok();

        if (!success) {
            spdlog::warn("Client {}: Reducer execution failed: {}", client_id_, status.error_message());
        }

        RecordRequest(success);
        return success;

    } catch (const std::exception& e) {
        spdlog::warn("Client {}: Reducer execution exception: {}", client_id_, e.what());
        RecordRequest(false);
        return false;
    }
}

void GrpcClient::RecordRequest(bool success) {
    requests_sent_.fetch_add(1);
    if (success) {
        requests_successful_.fetch_add(1);
    } else {
        requests_failed_.fetch_add(1);
    }
}

// GrpcPerformanceTest Implementation
GrpcPerformanceTest::GrpcPerformanceTest(const TestConfig& config, const GrpcClientConfig& grpc_config)
    : PerformanceTest(config), grpc_config_(grpc_config) {
}

bool GrpcPerformanceTest::Setup() {
    clients_.clear();
    clients_.reserve(config_.num_threads);

    spdlog::info("Setting up {} gRPC clients...", config_.num_threads);

    for (uint32_t i = 0; i < config_.num_threads; ++i) {
        auto client = std::make_unique<GrpcClient>(grpc_config_, i);
        if (!client->Connect()) {
            spdlog::error("Failed to connect gRPC client {}", i);
            return false;
        }
        clients_.push_back(std::move(client));

        // Small delay between connections
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    spdlog::info("All {} gRPC clients connected successfully", config_.num_threads);
    return true;
}

void GrpcPerformanceTest::Cleanup() {
    spdlog::info("Disconnecting {} gRPC clients...", clients_.size());

    for (auto& client : clients_) {
        if (client) {
            client->Disconnect();
        }
    }

    clients_.clear();
    spdlog::info("gRPC cleanup complete");
}

std::string GrpcPerformanceTest::GenerateSelectQuery(const std::string& table_name) {
    return "SELECT * FROM " + table_name + " LIMIT 10";
}

std::string GrpcPerformanceTest::GenerateComplexSelectQuery(const std::string& table_name) {
    return "SELECT t1.*, t2.value FROM " + table_name + " t1 JOIN " + table_name +
           "_secondary t2 ON t1.id = t2.ref_id WHERE t1.created > '2023-01-01' ORDER BY t1.id LIMIT 100";
}

std::string GrpcPerformanceTest::GenerateInsertQuery(const std::string& table_name, uint32_t key_id) {
    return "INSERT INTO " + table_name + " (id, name, value, created) VALUES (" +
           std::to_string(key_id) + ", 'test_" + std::to_string(key_id) +
           "', 'value_" + std::to_string(key_id) + "', NOW())";
}

std::string GrpcPerformanceTest::GenerateUpdateQuery(const std::string& table_name, uint32_t key_id) {
    return "UPDATE " + table_name + " SET value = 'updated_" + std::to_string(key_id) +
           "', modified = NOW() WHERE id = " + std::to_string(key_id);
}

std::string GrpcPerformanceTest::GenerateDeleteQuery(const std::string& table_name, uint32_t key_id) {
    return "DELETE FROM " + table_name + " WHERE id = " + std::to_string(key_id);
}

std::string GrpcPerformanceTest::GenerateCreateTableQuery(const std::string& table_name) {
    return "CREATE TABLE " + table_name + " (id INTEGER PRIMARY KEY, name TEXT, value TEXT, created TIMESTAMP)";
}

std::vector<std::string> GrpcPerformanceTest::GenerateTransactionQueries(uint32_t num_queries) {
    std::vector<std::string> queries;
    queries.reserve(num_queries);

    for (uint32_t i = 0; i < num_queries; ++i) {
        queries.push_back(GenerateInsertQuery("transaction_test", i));
    }

    return queries;
}

std::vector<uint8_t> GrpcPerformanceTest::GenerateTestWasmBytecode(const std::string& module_name) {
    // Generate simple test WASM bytecode (dummy implementation for testing)
    std::vector<uint8_t> bytecode;

    // WASM magic number
    bytecode.insert(bytecode.end(), {0x00, 0x61, 0x73, 0x6d});
    // Version
    bytecode.insert(bytecode.end(), {0x01, 0x00, 0x00, 0x00});

    // Simple function that returns 42
    std::vector<uint8_t> simple_module = {
        // Type section
        0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
        // Function section
        0x03, 0x02, 0x01, 0x00,
        // Export section
        0x07, 0x0b, 0x01, 0x07, 0x61, 0x64, 0x64, 0x5f, 0x74, 0x77, 0x6f, 0x00, 0x00,
        // Code section
        0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b
    };

    bytecode.insert(bytecode.end(), simple_module.begin(), simple_module.end());
    return bytecode;
}

std::vector<instantdb::grpc::WasmValue> GrpcPerformanceTest::GenerateTestReducerArgs() {
    std::vector<instantdb::grpc::WasmValue> args;

    instantdb::grpc::WasmValue arg1, arg2;
    arg1.set_i32_value(10);
    arg2.set_i32_value(32);

    args.push_back(arg1);
    args.push_back(arg2);

    return args;
}

// SqlQueryPerformanceTest Implementation
SqlQueryPerformanceTest::SqlQueryPerformanceTest(const TestConfig& config,
                                                 const GrpcClientConfig& grpc_config,
                                                 SqlRequestType request_type)
    : GrpcPerformanceTest(config, grpc_config), request_type_(request_type) {
}

void SqlQueryPerformanceTest::RunWorker(uint32_t thread_id) {
    ExecuteQueries(thread_id);
}

void SqlQueryPerformanceTest::ExecuteQueries(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        spdlog::error("Thread {} has no corresponding gRPC client", thread_id);
        return;
    }

    auto& client = clients_[thread_id];
    if (!client->IsConnected()) {
        spdlog::error("gRPC Client {} is not connected", thread_id);
        return;
    }

    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string query;

        switch (request_type_) {
            case SqlRequestType::SELECT_SIMPLE:
                query = GenerateSelectQuery("perf_test");
                break;
            case SqlRequestType::SELECT_COMPLEX:
                query = GenerateComplexSelectQuery("perf_test");
                break;
            case SqlRequestType::INSERT:
                query = GenerateInsertQuery("perf_test", sequence);
                break;
            case SqlRequestType::UPDATE:
                query = GenerateUpdateQuery("perf_test", sequence % 1000);
                break;
            case SqlRequestType::DELETE:
                query = GenerateDeleteQuery("perf_test", sequence % 1000);
                break;
            default:
                query = GenerateSelectQuery("perf_test");
                break;
        }

        instantdb::grpc::SQLResponse response;

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->ExecuteSQL(query, &response);
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);
        operations++;

        if (config_.target_ops_per_second > 0) {
            ThrottleOperations(operations, start_time);
        }
    }
}

// TransactionPerformanceTest Implementation
TransactionPerformanceTest::TransactionPerformanceTest(const TestConfig& config,
                                                       const GrpcClientConfig& grpc_config,
                                                       uint32_t queries_per_transaction)
    : GrpcPerformanceTest(config, grpc_config), queries_per_transaction_(queries_per_transaction) {
}

void TransactionPerformanceTest::RunWorker(uint32_t thread_id) {
    ExecuteTransactions(thread_id);
}

void TransactionPerformanceTest::ExecuteTransactions(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        spdlog::error("Thread {} has no corresponding gRPC client", thread_id);
        return;
    }

    auto& client = clients_[thread_id];
    if (!client->IsConnected()) {
        spdlog::error("gRPC Client {} is not connected", thread_id);
        return;
    }

    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        uint32_t base_sequence = global_sequence_.fetch_add(queries_per_transaction_);

        std::vector<std::string> queries;
        for (uint32_t i = 0; i < queries_per_transaction_; ++i) {
            queries.push_back(GenerateInsertQuery("txn_test", base_sequence + i));
        }

        instantdb::grpc::SQLTransactionResponse response;

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->ExecuteTransaction(queries, &response);
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);
        operations++;

        if (config_.target_ops_per_second > 0) {
            ThrottleOperations(operations, start_time);
        }
    }
}

// WasmDeploymentPerformanceTest Implementation
WasmDeploymentPerformanceTest::WasmDeploymentPerformanceTest(const TestConfig& config,
                                                             const GrpcClientConfig& grpc_config,
                                                             uint32_t module_size_kb)
    : GrpcPerformanceTest(config, grpc_config), module_size_kb_(module_size_kb) {
}

void WasmDeploymentPerformanceTest::RunWorker(uint32_t thread_id) {
    DeployModules(thread_id);
}

void WasmDeploymentPerformanceTest::DeployModules(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        spdlog::error("Thread {} has no corresponding gRPC client", thread_id);
        return;
    }

    auto& client = clients_[thread_id];
    if (!client->IsConnected()) {
        spdlog::error("gRPC Client {} is not connected", thread_id);
        return;
    }

    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        std::string module_name = "test_module_" + std::to_string(thread_id) + "_" + std::to_string(sequence);

        auto bytecode = GenerateTestWasmBytecode(module_name);
        // Pad bytecode to reach target size
        size_t target_size = module_size_kb_ * 1024;
        if (bytecode.size() < target_size) {
            bytecode.resize(target_size, 0x00);
        }

        instantdb::grpc::DeployModuleResponse response;

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->DeployModule(module_name, bytecode, &response);
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);
        operations++;

        if (config_.target_ops_per_second > 0) {
            ThrottleOperations(operations, start_time);
        }
    }
}

// WasmReducerPerformanceTest Implementation
WasmReducerPerformanceTest::WasmReducerPerformanceTest(const TestConfig& config,
                                                       const GrpcClientConfig& grpc_config,
                                                       const std::string& test_module_name)
    : GrpcPerformanceTest(config, grpc_config), test_module_name_(test_module_name) {
}

bool WasmReducerPerformanceTest::Setup() {
    if (!GrpcPerformanceTest::Setup()) {
        return false;
    }

    // Deploy test module first
    if (!clients_.empty()) {
        auto bytecode = GenerateTestWasmBytecode(test_module_name_);
        instantdb::grpc::DeployModuleResponse response;

        bool success = clients_[0]->DeployModule(test_module_name_, bytecode, &response);
        if (!success) {
            spdlog::error("Failed to deploy test module for reducer testing");
            return false;
        }

        spdlog::info("Test module '{}' deployed successfully", test_module_name_);
    }

    return true;
}

void WasmReducerPerformanceTest::RunWorker(uint32_t thread_id) {
    ExecuteReducers(thread_id);
}

void WasmReducerPerformanceTest::ExecuteReducers(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        spdlog::error("Thread {} has no corresponding gRPC client", thread_id);
        return;
    }

    auto& client = clients_[thread_id];
    if (!client->IsConnected()) {
        spdlog::error("gRPC Client {} is not connected", thread_id);
        return;
    }

    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);

        auto args = GenerateTestReducerArgs();
        // Vary arguments based on sequence
        if (!args.empty()) {
            args[0].set_i32_value(sequence % 100);
        }

        instantdb::grpc::ExecuteReducerResponse response;

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->ExecuteReducer(test_module_name_, "add_two", args, &response);
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);
        operations++;

        if (config_.target_ops_per_second > 0) {
            ThrottleOperations(operations, start_time);
        }
    }
}

// MixedWorkloadTest Implementation
MixedWorkloadTest::MixedWorkloadTest(const TestConfig& config,
                                     const GrpcClientConfig& grpc_config,
                                     double sql_ratio,
                                     double wasm_ratio)
    : GrpcPerformanceTest(config, grpc_config), sql_ratio_(sql_ratio), wasm_ratio_(wasm_ratio) {
}

bool MixedWorkloadTest::Setup() {
    if (!GrpcPerformanceTest::Setup()) {
        return false;
    }

    // Deploy test module for WASM operations
    if (!clients_.empty()) {
        auto bytecode = GenerateTestWasmBytecode(test_module_name_);
        instantdb::grpc::DeployModuleResponse response;

        bool success = clients_[0]->DeployModule(test_module_name_, bytecode, &response);
        if (!success) {
            spdlog::warn("Failed to deploy test module for mixed workload test");
        } else {
            spdlog::info("Test module '{}' deployed for mixed workload", test_module_name_);
        }
    }

    return true;
}

void MixedWorkloadTest::RunWorker(uint32_t thread_id) {
    ExecuteMixedWorkload(thread_id);
}

void MixedWorkloadTest::ExecuteMixedWorkload(uint32_t thread_id) {
    if (thread_id >= clients_.size()) {
        spdlog::error("Thread {} has no corresponding gRPC client", thread_id);
        return;
    }

    auto& client = clients_[thread_id];
    if (!client->IsConnected()) {
        spdlog::error("gRPC Client {} is not connected", thread_id);
        return;
    }

    std::mt19937 rng(thread_id);
    std::uniform_real_distribution<double> operation_dist(0.0, 1.0);

    uint32_t operations = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    while (std::chrono::steady_clock::now() < end_time && !should_stop_.load()) {
        uint32_t sequence = global_sequence_.fetch_add(1);
        double op_type = operation_dist(rng);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = false;

        if (op_type < sql_ratio_) {
            // SQL operation
            std::string query = GenerateSelectQuery("mixed_test");
            instantdb::grpc::SQLResponse response;
            success = client->ExecuteSQL(query, &response);
        } else if (op_type < sql_ratio_ + wasm_ratio_) {
            // WASM operation
            auto args = GenerateTestReducerArgs();
            if (!args.empty()) {
                args[0].set_i32_value(sequence % 100);
            }
            instantdb::grpc::ExecuteReducerResponse response;
            success = client->ExecuteReducer(test_module_name_, "add_two", args, &response);
        } else {
            // Status check operation
            instantdb::grpc::StatusResponse response;
            success = client->GetServerStatus(&response);
        }

        auto op_end = std::chrono::high_resolution_clock::now();
        RecordOperation(success, op_start, op_end);
        operations++;

        if (config_.target_ops_per_second > 0) {
            ThrottleOperations(operations, start_time);
        }
    }
}

// ConcurrentConnectionTest Implementation
ConcurrentConnectionTest::ConcurrentConnectionTest(const TestConfig& config,
                                                   const GrpcClientConfig& grpc_config,
                                                   uint32_t max_concurrent_connections)
    : GrpcPerformanceTest(config, grpc_config), max_concurrent_connections_(max_concurrent_connections) {
}

void ConcurrentConnectionTest::RunWorker(uint32_t thread_id) {
    TestConcurrentConnections(thread_id);
}

void ConcurrentConnectionTest::TestConcurrentConnections(uint32_t thread_id) {
    uint32_t connections_per_thread = max_concurrent_connections_ / config_.num_threads;
    std::vector<std::unique_ptr<GrpcClient>> thread_clients;

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(config_.duration_seconds);

    uint32_t established = 0;
    while (std::chrono::steady_clock::now() < end_time && established < connections_per_thread) {
        uint32_t client_id = thread_id * connections_per_thread + established;
        auto client = std::make_unique<GrpcClient>(grpc_config_, client_id);

        connection_attempts_.fetch_add(1);

        auto op_start = std::chrono::high_resolution_clock::now();
        bool success = client->Connect();
        auto op_end = std::chrono::high_resolution_clock::now();

        RecordOperation(success, op_start, op_end);

        if (success) {
            active_connections_.fetch_add(1);
            thread_clients.push_back(std::move(client));
        }

        established++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Keep connections alive and occasionally test them
    while (std::chrono::steady_clock::now() < end_time) {
        for (auto& client : thread_clients) {
            if (client && client->IsConnected()) {
                instantdb::grpc::StatusResponse response;
                client->GetServerStatus(&response);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Cleanup
    for (auto& client : thread_clients) {
        if (client) {
            client->Disconnect();
            active_connections_.fetch_sub(1);
        }
    }
}

// GrpcTestFactory Implementation
std::unique_ptr<GrpcPerformanceTest> GrpcTestFactory::CreateSqlQueryTest(
    const TestConfig& test_config,
    const GrpcClientConfig& grpc_config,
    SqlRequestType request_type) {

    return std::make_unique<SqlQueryPerformanceTest>(test_config, grpc_config, request_type);
}

std::unique_ptr<GrpcPerformanceTest> GrpcTestFactory::CreateTransactionTest(
    const TestConfig& test_config,
    const GrpcClientConfig& grpc_config,
    uint32_t queries_per_transaction) {

    return std::make_unique<TransactionPerformanceTest>(test_config, grpc_config, queries_per_transaction);
}

std::unique_ptr<GrpcPerformanceTest> GrpcTestFactory::CreateWasmDeploymentTest(
    const TestConfig& test_config,
    const GrpcClientConfig& grpc_config,
    uint32_t module_size_kb) {

    return std::make_unique<WasmDeploymentPerformanceTest>(test_config, grpc_config, module_size_kb);
}

std::unique_ptr<GrpcPerformanceTest> GrpcTestFactory::CreateWasmReducerTest(
    const TestConfig& test_config,
    const GrpcClientConfig& grpc_config) {

    return std::make_unique<WasmReducerPerformanceTest>(test_config, grpc_config);
}

std::unique_ptr<GrpcPerformanceTest> GrpcTestFactory::CreateMixedWorkloadTest(
    const TestConfig& test_config,
    const GrpcClientConfig& grpc_config,
    double sql_ratio,
    double wasm_ratio) {

    return std::make_unique<MixedWorkloadTest>(test_config, grpc_config, sql_ratio, wasm_ratio);
}

std::unique_ptr<GrpcPerformanceTest> GrpcTestFactory::CreateConcurrentConnectionTest(
    const TestConfig& test_config,
    const GrpcClientConfig& grpc_config,
    uint32_t max_concurrent_connections) {

    return std::make_unique<ConcurrentConnectionTest>(test_config, grpc_config, max_concurrent_connections);
}

// Predefined configurations
GrpcClientConfig GrpcTestFactory::GetDefaultClientConfig() {
    GrpcClientConfig config;
    config.server_address = "localhost:50051";
    config.connection_timeout_ms = 5000;
    config.request_timeout_ms = 30000;
    config.max_connections = 10;
    config.enable_keepalive = true;
    config.keepalive_time_ms = 30000;
    config.keepalive_timeout_ms = 5000;
    return config;
}

GrpcClientConfig GrpcTestFactory::GetHighThroughputConfig() {
    auto config = GetDefaultClientConfig();
    config.max_connections = 50;
    config.request_timeout_ms = 10000;
    config.keepalive_time_ms = 10000;
    return config;
}

GrpcClientConfig GrpcTestFactory::GetLowLatencyConfig() {
    auto config = GetDefaultClientConfig();
    config.connection_timeout_ms = 1000;
    config.request_timeout_ms = 5000;
    config.keepalive_time_ms = 5000;
    config.keepalive_timeout_ms = 1000;
    return config;
}

GrpcClientConfig GrpcTestFactory::GetStressTestConfig() {
    auto config = GetDefaultClientConfig();
    config.max_connections = 100;
    config.connection_timeout_ms = 10000;
    config.request_timeout_ms = 60000;
    config.keepalive_time_ms = 60000;
    return config;
}

} // namespace performance
} // namespace instantdb