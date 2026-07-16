#pragma once

#include "../framework/performance_test.h"
#include <grpcpp/grpcpp.h>
#include "instantdb.grpc.pb.h"
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <queue>
#include <mutex>

namespace instantdb {
namespace performance {

// gRPC client configuration
struct GrpcClientConfig {
    std::string server_address = "localhost:50051";
    uint32_t connection_timeout_ms = 5000;
    uint32_t request_timeout_ms = 30000;
    uint32_t max_connections = 10;
    bool enable_keepalive = true;
    uint32_t keepalive_time_ms = 30000;
    uint32_t keepalive_timeout_ms = 5000;
};

// SQL request types for testing
enum class SqlRequestType {
    SELECT_SIMPLE,
    SELECT_COMPLEX,
    INSERT,
    UPDATE,
    DELETE,
    CREATE_TABLE,
    DROP_TABLE,
    TRANSACTION,
    BATCH
};

// gRPC client wrapper
class GrpcClient {
public:
    GrpcClient(const GrpcClientConfig& config, uint32_t client_id);
    ~GrpcClient();

    bool Connect();
    void Disconnect();
    bool IsConnected() const;

    // SQL service methods
    bool ExecuteSQL(const std::string& query, instantdb::grpc::SQLResponse* response);
    bool ExecuteTransaction(const std::vector<std::string>& queries,
                          instantdb::grpc::SQLTransactionResponse* response);
    bool GetServerStatus(instantdb::grpc::StatusResponse* response);

    // WASM service methods
    bool DeployModule(const std::string& module_name,
                     const std::vector<uint8_t>& bytecode,
                     instantdb::grpc::DeployModuleResponse* response);
    bool UndeployModule(const std::string& module_name,
                       instantdb::grpc::UndeployModuleResponse* response);
    bool ListModules(instantdb::grpc::ListModulesResponse* response);
    bool GetModule(const std::string& module_name,
                  instantdb::grpc::GetModuleResponse* response);
    bool ExecuteReducer(const std::string& module_name,
                       const std::string& reducer_name,
                       const std::vector<instantdb::grpc::WasmValue>& args,
                       instantdb::grpc::ExecuteReducerResponse* response);

    // Statistics
    uint64_t GetRequestsSent() const { return requests_sent_.load(); }
    uint64_t GetRequestsSuccessful() const { return requests_successful_.load(); }
    uint64_t GetRequestsFailed() const { return requests_failed_.load(); }
    uint64_t GetConnectionErrors() const { return connection_errors_.load(); }

private:
    GrpcClientConfig config_;
    uint32_t client_id_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<instantdb::grpc::SQLService::Stub> sql_stub_;
    std::unique_ptr<instantdb::grpc::WasmService::Stub> wasm_stub_;
    std::atomic<bool> connected_{false};

    // Statistics
    std::atomic<uint64_t> requests_sent_{0};
    std::atomic<uint64_t> requests_successful_{0};
    std::atomic<uint64_t> requests_failed_{0};
    std::atomic<uint64_t> connection_errors_{0};

    void RecordRequest(bool success);
};

// Base class for gRPC performance tests
class GrpcPerformanceTest : public PerformanceTest {
public:
    GrpcPerformanceTest(const TestConfig& config, const GrpcClientConfig& grpc_config);
    virtual ~GrpcPerformanceTest() = default;

    bool Setup() override;
    void Cleanup() override;

protected:
    GrpcClientConfig grpc_config_;
    std::vector<std::unique_ptr<GrpcClient>> clients_;

    // Helper methods for generating test queries
    std::string GenerateSelectQuery(const std::string& table_name = "test_table");
    std::string GenerateComplexSelectQuery(const std::string& table_name = "test_table");
    std::string GenerateInsertQuery(const std::string& table_name = "test_table", uint32_t key_id = 0);
    std::string GenerateUpdateQuery(const std::string& table_name = "test_table", uint32_t key_id = 0);
    std::string GenerateDeleteQuery(const std::string& table_name = "test_table", uint32_t key_id = 0);
    std::string GenerateCreateTableQuery(const std::string& table_name = "test_table");
    std::vector<std::string> GenerateTransactionQueries(uint32_t num_queries = 5);

    // WASM helper methods
    std::vector<uint8_t> GenerateTestWasmBytecode(const std::string& module_name);
    std::vector<instantdb::grpc::WasmValue> GenerateTestReducerArgs();
};

// SQL query performance test
class SqlQueryPerformanceTest : public GrpcPerformanceTest {
public:
    SqlQueryPerformanceTest(const TestConfig& config,
                           const GrpcClientConfig& grpc_config,
                           SqlRequestType request_type = SqlRequestType::SELECT_SIMPLE);

    void RunWorker(uint32_t thread_id) override;

private:
    SqlRequestType request_type_;
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteQueries(uint32_t thread_id);
};

// Transaction performance test
class TransactionPerformanceTest : public GrpcPerformanceTest {
public:
    TransactionPerformanceTest(const TestConfig& config,
                              const GrpcClientConfig& grpc_config,
                              uint32_t queries_per_transaction = 5);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t queries_per_transaction_;
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteTransactions(uint32_t thread_id);
};

// WASM module deployment test
class WasmDeploymentPerformanceTest : public GrpcPerformanceTest {
public:
    WasmDeploymentPerformanceTest(const TestConfig& config,
                                 const GrpcClientConfig& grpc_config,
                                 uint32_t module_size_kb = 100);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t module_size_kb_;
    std::atomic<uint32_t> global_sequence_{0};

    void DeployModules(uint32_t thread_id);
};

// WASM reducer execution test
class WasmReducerPerformanceTest : public GrpcPerformanceTest {
public:
    WasmReducerPerformanceTest(const TestConfig& config,
                              const GrpcClientConfig& grpc_config,
                              const std::string& test_module_name = "test_module");

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    std::string test_module_name_;
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteReducers(uint32_t thread_id);
};

// Connection pooling performance test
class ConnectionPoolingTest : public GrpcPerformanceTest {
public:
    ConnectionPoolingTest(const TestConfig& config,
                         const GrpcClientConfig& grpc_config,
                         uint32_t pool_size = 10);

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t pool_size_;
    std::queue<std::unique_ptr<GrpcClient>> connection_pool_;
    std::mutex pool_mutex_;

    std::unique_ptr<GrpcClient> GetConnection();
    void ReturnConnection(std::unique_ptr<GrpcClient> client);
    void TestConnectionPooling(uint32_t thread_id);
};

// Mixed workload test
class MixedWorkloadTest : public GrpcPerformanceTest {
public:
    MixedWorkloadTest(const TestConfig& config,
                     const GrpcClientConfig& grpc_config,
                     double sql_ratio = 0.8,
                     double wasm_ratio = 0.2);

    bool Setup() override;
    void RunWorker(uint32_t thread_id) override;

private:
    double sql_ratio_;
    double wasm_ratio_;
    std::string test_module_name_ = "mixed_test_module";
    std::atomic<uint32_t> global_sequence_{0};

    void ExecuteMixedWorkload(uint32_t thread_id);
};

// Server status monitoring test
class StatusMonitoringTest : public GrpcPerformanceTest {
public:
    StatusMonitoringTest(const TestConfig& config,
                        const GrpcClientConfig& grpc_config,
                        uint32_t polling_interval_ms = 1000);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t polling_interval_ms_;
    std::vector<instantdb::grpc::StatusResponse> status_snapshots_;
    std::mutex snapshots_mutex_;

    void MonitorServerStatus(uint32_t thread_id);
};

// Concurrent connection test
class ConcurrentConnectionTest : public GrpcPerformanceTest {
public:
    ConcurrentConnectionTest(const TestConfig& config,
                            const GrpcClientConfig& grpc_config,
                            uint32_t max_concurrent_connections = 100);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t max_concurrent_connections_;
    std::atomic<uint32_t> active_connections_{0};
    std::atomic<uint32_t> connection_attempts_{0};

    void TestConcurrentConnections(uint32_t thread_id);
};

// Request timeout test
class RequestTimeoutTest : public GrpcPerformanceTest {
public:
    RequestTimeoutTest(const TestConfig& config,
                      const GrpcClientConfig& grpc_config,
                      uint32_t timeout_ms = 1000);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t timeout_ms_;
    std::atomic<uint32_t> timeouts_encountered_{0};

    void TestRequestTimeouts(uint32_t thread_id);
};

// Streaming performance test (if streaming is implemented)
class StreamingPerformanceTest : public GrpcPerformanceTest {
public:
    StreamingPerformanceTest(const TestConfig& config,
                            const GrpcClientConfig& grpc_config,
                            uint32_t stream_duration_ms = 10000);

    void RunWorker(uint32_t thread_id) override;

private:
    uint32_t stream_duration_ms_;
    std::atomic<uint64_t> stream_messages_sent_{0};
    std::atomic<uint64_t> stream_messages_received_{0};

    void TestStreaming(uint32_t thread_id);
};

// gRPC test factory
class GrpcTestFactory {
public:
    static std::unique_ptr<GrpcPerformanceTest> CreateSqlQueryTest(
        const TestConfig& test_config,
        const GrpcClientConfig& grpc_config,
        SqlRequestType request_type = SqlRequestType::SELECT_SIMPLE
    );

    static std::unique_ptr<GrpcPerformanceTest> CreateTransactionTest(
        const TestConfig& test_config,
        const GrpcClientConfig& grpc_config,
        uint32_t queries_per_transaction = 5
    );

    static std::unique_ptr<GrpcPerformanceTest> CreateWasmDeploymentTest(
        const TestConfig& test_config,
        const GrpcClientConfig& grpc_config,
        uint32_t module_size_kb = 100
    );

    static std::unique_ptr<GrpcPerformanceTest> CreateWasmReducerTest(
        const TestConfig& test_config,
        const GrpcClientConfig& grpc_config
    );

    static std::unique_ptr<GrpcPerformanceTest> CreateMixedWorkloadTest(
        const TestConfig& test_config,
        const GrpcClientConfig& grpc_config,
        double sql_ratio = 0.8,
        double wasm_ratio = 0.2
    );

    static std::unique_ptr<GrpcPerformanceTest> CreateConcurrentConnectionTest(
        const TestConfig& test_config,
        const GrpcClientConfig& grpc_config,
        uint32_t max_concurrent_connections = 100
    );

    // Predefined configurations
    static GrpcClientConfig GetDefaultClientConfig();
    static GrpcClientConfig GetHighThroughputConfig();
    static GrpcClientConfig GetLowLatencyConfig();
    static GrpcClientConfig GetStressTestConfig();
};

} // namespace performance
} // namespace instantdb