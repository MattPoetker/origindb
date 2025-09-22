#pragma once

#include <memory>
#include <string>
#include <chrono>
#include "sql/sql_engine.h"
#include "storage/table.h"

namespace instantdb {

class SqlEngine;
class StorageEngine;
class ChangefeedEngine;
class WebSocketServer;

class GrpcServer {
public:
    struct Config {
        std::string listen_address = "0.0.0.0:50051";
        int max_concurrent_streams = 1000;
        int max_message_size = 64 * 1024 * 1024; // 64MB
    };

    GrpcServer(const Config& config,
               std::shared_ptr<SqlEngine> sql_engine,
               std::shared_ptr<StorageEngine> storage_engine,
               std::shared_ptr<ChangefeedEngine> changefeed_engine,
               std::shared_ptr<WebSocketServer> websocket_server);

    ~GrpcServer();

    bool Start();
    void Stop();
    bool IsRunning() const;

    std::string GetListenAddress() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class SQLServiceImpl {
public:
    SQLServiceImpl(std::shared_ptr<SqlEngine> sql_engine,
                   std::shared_ptr<StorageEngine> storage_engine,
                   std::shared_ptr<ChangefeedEngine> changefeed_engine,
                   std::shared_ptr<WebSocketServer> websocket_server);

    void* Execute(void* context, const void* request, void* response);

    void* ExecuteTransaction(void* context, const void* request, void* response);

    void* GetStatus(void* context, const void* request, void* response);

    // Get the actual gRPC service implementation (hidden from header)
    void* GetServiceImpl();

private:
    std::shared_ptr<SqlEngine> sql_engine_;
    std::shared_ptr<StorageEngine> storage_engine_;
    std::shared_ptr<ChangefeedEngine> changefeed_engine_;
    std::shared_ptr<WebSocketServer> websocket_server_;

    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> total_queries_{0};
    std::atomic<uint64_t> successful_queries_{0};
    std::atomic<uint64_t> failed_queries_{0};

    // Helper methods
    void ConvertSqlResultToProto(const SqlResult& sql_result,
                                void* response,
                                int64_t execution_time_micros);

    void ConvertRowToProto(const Row& storage_row, void* proto_row);
    void ConvertColumnValueToProto(const Value& value,
                                  void* proto_value);

    // Pimpl for gRPC service implementation
    std::unique_ptr<void, void(*)(void*)> service_impl_;
};

} // namespace instantdb