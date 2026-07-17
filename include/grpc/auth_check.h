#pragma once

#include <memory>
#include <string>

#include <grpcpp/server_context.h>

#include "common/auth.h"

namespace origindb {

// Validates the "authorization" metadata (Bearer token) against the required
// scope. A null auth manager means auth is disabled (dev mode). On failure
// fills `status` with UNAUTHENTICATED/PERMISSION_DENIED and returns false.
inline bool CheckAuth(::grpc::ServerContext* context,
                      const std::shared_ptr<AuthManager>& auth,
                      AuthScope scope,
                      ::grpc::Status& status) {
    if (!auth) return true;

    const auto& metadata = context->client_metadata();
    auto it = metadata.find("authorization");
    if (it == metadata.end()) {
        status = ::grpc::Status(::grpc::StatusCode::UNAUTHENTICATED,
                                "missing authorization metadata (Bearer token)");
        return false;
    }
    std::string token = AuthManager::StripBearer(
        std::string(it->second.data(), it->second.size()));
    if (!auth->Check(token, scope)) {
        status = ::grpc::Status(
            ::grpc::StatusCode::PERMISSION_DENIED,
            scope == AuthScope::ADMIN
                ? "admin token required for this operation"
                : "invalid token");
        return false;
    }
    return true;
}

} // namespace origindb
