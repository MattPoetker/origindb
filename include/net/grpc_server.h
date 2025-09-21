#pragma once

#include <memory>
#include <string>

#include "common/config.h"

namespace instantdb {

class SqlEngine;
class AdminService;

// gRPC server implementation
class GrpcServer {
public:
    GrpcServer(const GrpcConfig& config,
               std::shared_ptr<SqlEngine> sql_engine,
               std::shared_ptr<AdminService> admin_service);
    ~GrpcServer();

    bool Initialize();
    bool Start();
    void Stop();

    std::string GetListenAddress() const;
    bool IsRunning() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb