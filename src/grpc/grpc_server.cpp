#include "grpc/grpc_server.h"
#include "grpc/wasm_service_impl.h"
#include "wasm/module_store.h"
#include "sql/sql_engine.h"
#include "storage/storage_engine.h"
#include "changefeed/changefeed_engine.h"
#include "websocket/websocket_server.h"
#include "wasm/wasm_engine.h"
#include <spdlog/spdlog.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <thread>

// Include protobuf headers AFTER our headers to avoid conflicts
#include "origindb.grpc.pb.h"

namespace origindb {

// Internal gRPC service implementation that inherits from protobuf service
class GrpcServiceImpl : public origindb::grpc::SQLService::Service {
public:
    GrpcServiceImpl(SQLServiceImpl* wrapper) : wrapper_(wrapper) {}

    ::grpc::Status Execute(::grpc::ServerContext* context,
                        const origindb::grpc::SQLRequest* request,
                        origindb::grpc::SQLResponse* response) override {
        auto result = wrapper_->Execute(context, request, response);
        return *static_cast<::grpc::Status*>(result);
    }

    ::grpc::Status ExecuteTransaction(::grpc::ServerContext* context,
                                  const origindb::grpc::SQLTransactionRequest* request,
                                  origindb::grpc::SQLTransactionResponse* response) override {
        auto result = wrapper_->ExecuteTransaction(context, request, response);
        return *static_cast<::grpc::Status*>(result);
    }

    ::grpc::Status GetStatus(::grpc::ServerContext* context,
                          const origindb::grpc::StatusRequest* request,
                          origindb::grpc::StatusResponse* response) override {
        auto result = wrapper_->GetStatus(context, request, response);
        return *static_cast<::grpc::Status*>(result);
    }

private:
    SQLServiceImpl* wrapper_;
};

// Internal WASM gRPC service implementation
class WasmGrpcServiceImpl : public origindb::grpc::WasmService::Service {
public:
    WasmGrpcServiceImpl(WasmServiceImpl* wrapper) : wrapper_(wrapper) {}

    ::grpc::Status DeployModule(::grpc::ServerContext* context,
                               const origindb::grpc::DeployModuleRequest* request,
                               origindb::grpc::DeployModuleResponse* response) override {
        auto result = wrapper_->DeployModule(context, request, response);
        return *static_cast<::grpc::Status*>(result);
    }

    ::grpc::Status UndeployModule(::grpc::ServerContext* context,
                                 const origindb::grpc::UndeployModuleRequest* request,
                                 origindb::grpc::UndeployModuleResponse* response) override {
        auto result = wrapper_->UndeployModule(context, request, response);
        return *static_cast<::grpc::Status*>(result);
    }

    ::grpc::Status ListModules(::grpc::ServerContext* context,
                              const origindb::grpc::ListModulesRequest* request,
                              origindb::grpc::ListModulesResponse* response) override {
        try {
            auto result = wrapper_->ListModules(context, request, response);
            return *static_cast<::grpc::Status*>(result);
        } catch (const std::exception& e) {
            spdlog::error("WasmGrpcServiceImpl::ListModules caught exception: {}", e.what());
            return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Exception in ListModules");
        } catch (...) {
            spdlog::error("WasmGrpcServiceImpl::ListModules caught unknown exception");
            return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Unknown exception in ListModules");
        }
    }

    ::grpc::Status GetModule(::grpc::ServerContext* context,
                            const origindb::grpc::GetModuleRequest* request,
                            origindb::grpc::GetModuleResponse* response) override {
        auto result = wrapper_->GetModule(context, request, response);
        return *static_cast<::grpc::Status*>(result);
    }

    ::grpc::Status ExecuteReducer(::grpc::ServerContext* context,
                                 const origindb::grpc::ExecuteReducerRequest* request,
                                 origindb::grpc::ExecuteReducerResponse* response) override {
        auto result = wrapper_->ExecuteReducer(context, request, response);
        return *static_cast<::grpc::Status*>(result);
    }

private:
    WasmServiceImpl* wrapper_;
};

class GrpcServer::Impl {
public:
    Impl(const Config& config,
         std::shared_ptr<SqlEngine> sql_engine,
         std::shared_ptr<StorageEngine> storage_engine,
         std::shared_ptr<ChangefeedEngine> changefeed_engine,
         std::shared_ptr<WebSocketServer> websocket_server,
         std::shared_ptr<WasmEngine> wasm_engine,
         std::shared_ptr<ModuleStore> module_store)
        : config_(config)
        , sql_service_(sql_engine, storage_engine, changefeed_engine, websocket_server)
        , wasm_service_(wasm_engine, module_store)
        , grpc_service_(&sql_service_)
        , wasm_grpc_service_(&wasm_service_)
        , running_(false) {}

    bool Start() {
        if (running_) {
            spdlog::warn("gRPC server already running");
            return true;
        }

        try {
            ::grpc::ServerBuilder builder;

            // Configure server
            builder.AddListeningPort(config_.listen_address, ::grpc::InsecureServerCredentials());
            builder.SetMaxReceiveMessageSize(config_.max_message_size);
            builder.SetMaxSendMessageSize(config_.max_message_size);

            // Register services
            builder.RegisterService(&grpc_service_);
            builder.RegisterService(&wasm_grpc_service_);

            // Build and start server
            server_ = builder.BuildAndStart();
            if (!server_) {
                spdlog::error("Failed to start gRPC server");
                return false;
            }

            running_ = true;
            spdlog::info("gRPC server listening on {}", config_.listen_address);

            // Start server in background thread
            server_thread_ = std::thread([this]() {
                server_->Wait();
            });

            return true;

        } catch (const std::exception& e) {
            spdlog::error("Exception starting gRPC server: {}", e.what());
            return false;
        }
    }

    void Stop() {
        if (!running_) {
            return;
        }

        spdlog::info("Stopping gRPC server...");

        if (server_) {
            server_->Shutdown();
        }

        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        running_ = false;
        spdlog::info("gRPC server stopped");
    }

    bool IsRunning() const {
        return running_;
    }

    std::string GetListenAddress() const {
        return config_.listen_address;
    }

private:
    Config config_;
    SQLServiceImpl sql_service_;
    WasmServiceImpl wasm_service_;
    GrpcServiceImpl grpc_service_;
    WasmGrpcServiceImpl wasm_grpc_service_;
    std::unique_ptr<::grpc::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> running_;
};

GrpcServer::GrpcServer(const Config& config,
                       std::shared_ptr<SqlEngine> sql_engine,
                       std::shared_ptr<StorageEngine> storage_engine,
                       std::shared_ptr<ChangefeedEngine> changefeed_engine,
                       std::shared_ptr<WebSocketServer> websocket_server,
                       std::shared_ptr<WasmEngine> wasm_engine,
                       std::shared_ptr<ModuleStore> module_store)
    : impl_(std::make_unique<Impl>(config, sql_engine, storage_engine, changefeed_engine, websocket_server, wasm_engine, module_store)) {}

GrpcServer::~GrpcServer() {
    if (impl_) {
        impl_->Stop();
    }
}

bool GrpcServer::Start() {
    return impl_->Start();
}

void GrpcServer::Stop() {
    impl_->Stop();
}

bool GrpcServer::IsRunning() const {
    return impl_->IsRunning();
}

std::string GrpcServer::GetListenAddress() const {
    return impl_->GetListenAddress();
}

// SQLServiceImpl implementation

SQLServiceImpl::SQLServiceImpl(std::shared_ptr<SqlEngine> sql_engine,
                               std::shared_ptr<StorageEngine> storage_engine,
                               std::shared_ptr<ChangefeedEngine> changefeed_engine,
                               std::shared_ptr<WebSocketServer> websocket_server)
    : sql_engine_(sql_engine)
    , storage_engine_(storage_engine)
    , changefeed_engine_(changefeed_engine)
    , websocket_server_(websocket_server)
    , start_time_(std::chrono::steady_clock::now())
    , service_impl_(nullptr, [](void*){}) {}

void* SQLServiceImpl::Execute(void* context_ptr, const void* request_ptr, void* response_ptr) {
    auto* context = static_cast<::grpc::ServerContext*>(context_ptr);
    auto* request = static_cast<const origindb::grpc::SQLRequest*>(request_ptr);
    auto* response = static_cast<origindb::grpc::SQLResponse*>(response_ptr);

    static thread_local ::grpc::Status status;

    total_queries_.fetch_add(1, std::memory_order_relaxed);

    spdlog::debug("gRPC Execute request: {}", request->sql());

    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Execute SQL
        auto result = sql_engine_->Execute(request->sql());

        auto end_time = std::chrono::high_resolution_clock::now();
        auto execution_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

        // Convert result to protobuf
        ConvertSqlResultToProto(result, response, execution_time.count());

        if (result.success) {
            successful_queries_.fetch_add(1, std::memory_order_relaxed);
            spdlog::debug("gRPC Execute successful: {} rows returned", result.rows.size());
        } else {
            failed_queries_.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("gRPC Execute failed: {}", result.error);
        }

        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        failed_queries_.fetch_add(1, std::memory_order_relaxed);
        spdlog::error("gRPC Execute exception: {}", e.what());

        response->set_success(false);
        response->set_error("Internal server error: " + std::string(e.what()));
        status = ::grpc::Status::OK;
    }

    return &status;
}

void* SQLServiceImpl::ExecuteTransaction(void* context_ptr, const void* request_ptr, void* response_ptr) {
    auto* request = static_cast<const origindb::grpc::SQLTransactionRequest*>(request_ptr);
    auto* response = static_cast<origindb::grpc::SQLTransactionResponse*>(response_ptr);

    static thread_local ::grpc::Status status;

    spdlog::debug("gRPC ExecuteTransaction request with {} statements", request->sql_statements_size());

    // For now, execute statements sequentially without real transactions
    // TODO: Implement proper transaction support
    response->set_success(true);
    response->set_transaction_id(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    for (const auto& sql : request->sql_statements()) {
        auto* result_response = response->add_results();

        auto start_time = std::chrono::high_resolution_clock::now();
        auto result = sql_engine_->Execute(sql);
        auto end_time = std::chrono::high_resolution_clock::now();
        auto execution_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

        ConvertSqlResultToProto(result, result_response, execution_time.count());

        if (!result.success) {
            response->set_success(false);
            response->set_error("Statement failed: " + sql + " - " + result.error);
            break;
        }
    }

    status = ::grpc::Status::OK;
    return &status;
}

void* SQLServiceImpl::GetStatus(void* context_ptr, const void* request_ptr, void* response_ptr) {
    auto* response = static_cast<origindb::grpc::StatusResponse*>(response_ptr);

    static thread_local ::grpc::Status status;

    spdlog::debug("gRPC GetStatus request");

    try {
        response->set_version("0.1.0");

        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
        response->set_uptime_seconds(uptime.count());

        // Get server statistics
        auto* stats = response->mutable_stats();

        // Storage stats
        auto storage_stats = storage_engine_->GetStats();
        stats->set_total_tables(storage_stats.total_tables);
        stats->set_total_rows(storage_stats.total_rows);
        stats->set_storage_bytes(storage_stats.total_bytes);

        // Changefeed stats
        auto cf_metrics = changefeed_engine_->GetMetrics();
        stats->set_active_subscriptions(cf_metrics.active_subscriptions);
        stats->set_events_published(cf_metrics.total_events_published);

        // WebSocket stats
        stats->set_active_connections(websocket_server_->GetActiveConnections());

        // gRPC stats
        stats->set_total_queries(total_queries_.load(std::memory_order_relaxed));
        stats->set_successful_queries(successful_queries_.load(std::memory_order_relaxed));
        stats->set_failed_queries(failed_queries_.load(std::memory_order_relaxed));

        spdlog::debug("gRPC GetStatus successful");
        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        spdlog::error("gRPC GetStatus exception: {}", e.what());
        status = ::grpc::Status(::grpc::StatusCode::INTERNAL, "Failed to get server status");
    }

    return &status;
}

void* SQLServiceImpl::GetServiceImpl() {
    // This would return the actual gRPC service implementation
    return nullptr; // Not needed for this implementation
}

void SQLServiceImpl::ConvertSqlResultToProto(const SqlResult& sql_result,
                                            void* response_ptr,
                                            int64_t execution_time_micros) {
    auto* response = static_cast<origindb::grpc::SQLResponse*>(response_ptr);

    response->set_success(sql_result.success);
    response->set_error(sql_result.error);
    response->set_execution_time_micros(execution_time_micros);

    if (sql_result.success) {
        response->set_rows_affected(static_cast<int32_t>(sql_result.rows.size()));

        for (const auto& row : sql_result.rows) {
            auto* proto_row = response->add_rows();
            ConvertRowToProto(row, proto_row);
        }
    }
}

void SQLServiceImpl::ConvertRowToProto(const Row& storage_row, void* proto_row_ptr) {
    auto* proto_row = static_cast<origindb::grpc::Row*>(proto_row_ptr);

    proto_row->set_key(storage_row.key);

    for (const auto& [column_name, column_value] : storage_row.columns) {
        auto& proto_columns = *proto_row->mutable_columns();
        ConvertColumnValueToProto(column_value, &proto_columns[column_name]);
    }
}

void SQLServiceImpl::ConvertColumnValueToProto(const Value& value,
                                              void* proto_value_ptr) {
    auto* proto_value = static_cast<origindb::grpc::ColumnValue*>(proto_value_ptr);

    std::visit([proto_value](const auto& v) {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, std::monostate>) {
            // NULL value - no field set in oneof
        } else if constexpr (std::is_same_v<T, int32_t>) {
            proto_value->set_int64_value(static_cast<int64_t>(v));
        } else if constexpr (std::is_same_v<T, int64_t>) {
            proto_value->set_int64_value(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            proto_value->set_string_value(v);
        } else if constexpr (std::is_same_v<T, float>) {
            proto_value->set_double_value(static_cast<double>(v));
        } else if constexpr (std::is_same_v<T, double>) {
            proto_value->set_double_value(v);
        } else if constexpr (std::is_same_v<T, bool>) {
            proto_value->set_bool_value(v);
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            proto_value->set_bytes_value(std::string(v.begin(), v.end()));
        } else {
            // Fallback for unknown types
            proto_value->set_string_value("[unknown type]");
        }
    }, value);
}

} // namespace origindb