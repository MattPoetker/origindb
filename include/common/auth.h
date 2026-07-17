#pragma once

#include <filesystem>
#include <string>

namespace origindb {

enum class AuthScope {
    CLIENT,  // call reducers, subscribe, read status
    ADMIN,   // deploy/undeploy modules, execute SQL
};

// Bearer-token authentication with two scopes. Tokens are loaded from
// <data_dir>/auth/tokens.json, or generated (and persisted, mode 0600) on
// first boot. Explicit tokens via config/env override the stored ones.
class AuthManager {
public:
    // dir: <data_dir>/auth. Empty admin/client override = load-or-generate.
    AuthManager(std::filesystem::path auth_dir,
                std::string admin_override = "",
                std::string client_override = "");

    bool Initialize();

    // Returns true if `token` grants `scope`. The admin token also grants
    // CLIENT. Constant-time comparison.
    bool Check(const std::string& token, AuthScope scope) const;

    // Accepts "Bearer <token>" or a bare token.
    static std::string StripBearer(const std::string& header_value);

    const std::string& AdminToken() const { return admin_token_; }
    const std::string& ClientToken() const { return client_token_; }
    bool GeneratedThisBoot() const { return generated_; }

private:
    std::filesystem::path dir_;
    std::string admin_token_;
    std::string client_token_;
    bool generated_ = false;
};

} // namespace origindb
