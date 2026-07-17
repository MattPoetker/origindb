#include "common/auth.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <fstream>
#include <sys/stat.h>

namespace origindb {

namespace {

std::string GenerateToken(const char* prefix) {
    unsigned char bytes[24];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        spdlog::critical("OpenSSL RAND_bytes failed; cannot generate auth tokens");
        abort();
    }
    static const char hex[] = "0123456789abcdef";
    std::string out = prefix;
    for (unsigned char b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

// Constant-time equality; length leak is acceptable (tokens are fixed-size).
bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}  // namespace

AuthManager::AuthManager(std::filesystem::path auth_dir,
                         std::string admin_override,
                         std::string client_override)
    : dir_(std::move(auth_dir)),
      admin_token_(std::move(admin_override)),
      client_token_(std::move(client_override)) {}

bool AuthManager::Initialize() {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        spdlog::error("AuthManager: cannot create {}: {}", dir_.string(), ec.message());
        return false;
    }

    auto tokens_path = dir_ / "tokens.json";

    // Explicit overrides win outright; nothing is persisted for them.
    if (!admin_token_.empty() && !client_token_.empty()) {
        spdlog::info("Auth: using tokens from configuration");
        return true;
    }

    // Load persisted tokens.
    {
        std::ifstream in(tokens_path);
        if (in) {
            auto doc = nlohmann::json::parse(in, nullptr, false);
            if (!doc.is_discarded()) {
                if (admin_token_.empty())
                    admin_token_ = doc.value("admin_token", "");
                if (client_token_.empty())
                    client_token_ = doc.value("client_token", "");
            }
        }
    }

    // Generate whatever is still missing and persist.
    if (admin_token_.empty() || client_token_.empty()) {
        if (admin_token_.empty()) admin_token_ = GenerateToken("odb_admin_");
        if (client_token_.empty()) client_token_ = GenerateToken("odb_client_");
        generated_ = true;

        nlohmann::json doc = {
            {"admin_token", admin_token_},
            {"client_token", client_token_},
        };
        auto tmp = tokens_path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out || !(out << doc.dump(2))) {
                spdlog::error("AuthManager: failed to write {}", tmp);
                return false;
            }
        }
        ::chmod(tmp.c_str(), 0600);
        std::filesystem::rename(tmp, tokens_path, ec);
        if (ec) {
            spdlog::error("AuthManager: failed to persist tokens: {}", ec.message());
            return false;
        }
        spdlog::info("Auth: generated new tokens (stored in {})", tokens_path.string());
    } else {
        spdlog::info("Auth: loaded tokens from {}", tokens_path.string());
    }
    ::chmod(tokens_path.c_str(), 0600);
    return true;
}

bool AuthManager::Check(const std::string& token, AuthScope scope) const {
    if (token.empty()) return false;
    if (ConstantTimeEquals(token, admin_token_)) return true;  // admin ⊇ client
    if (scope == AuthScope::CLIENT && ConstantTimeEquals(token, client_token_))
        return true;
    return false;
}

std::string AuthManager::StripBearer(const std::string& header_value) {
    constexpr const char* kPrefix = "Bearer ";
    if (header_value.rfind(kPrefix, 0) == 0) return header_value.substr(7);
    return header_value;
}

} // namespace origindb
