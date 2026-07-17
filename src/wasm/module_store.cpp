#include "wasm/module_store.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <chrono>
#include <fstream>

namespace origindb {

namespace {

// Module names become directory names; keep them filesystem-safe.
bool ValidModuleName(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    for (char c : name) {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
              c == '.'))
            return false;
    }
    return name != "." && name != "..";
}

std::optional<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

}  // namespace

ModuleStore::ModuleStore(std::filesystem::path modules_dir)
    : dir_(std::move(modules_dir)) {}

std::string ModuleStore::Sha256Hex(const uint8_t* data, size_t len) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_Digest(data, len, digest, &digest_len, EVP_sha256(), nullptr);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; i++) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0xF]);
    }
    return out;
}

bool ModuleStore::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        spdlog::error("ModuleStore: cannot create {}: {}", dir_.string(), ec.message());
        return false;
    }

    index_.clear();
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (!entry.is_directory()) continue;
        auto manifest_path = entry.path() / "manifest.json";
        auto wasm_path = entry.path() / "module.wasm";

        std::ifstream in(manifest_path);
        if (!in) {
            spdlog::warn("ModuleStore: skipping {} (no manifest)", entry.path().string());
            continue;
        }
        auto manifest = nlohmann::json::parse(in, nullptr, false);
        if (manifest.is_discarded() || !manifest.contains("name") ||
            !manifest.contains("sha256")) {
            spdlog::warn("ModuleStore: skipping {} (corrupt manifest)",
                         entry.path().string());
            continue;
        }

        auto bytes = ReadFileBytes(wasm_path);
        if (!bytes) {
            spdlog::warn("ModuleStore: skipping {} (missing module.wasm)",
                         entry.path().string());
            continue;
        }
        std::string expected = manifest["sha256"].get<std::string>();
        std::string actual = Sha256Hex(bytes->data(), bytes->size());
        if (actual != expected) {
            spdlog::warn("ModuleStore: skipping {} (sha256 mismatch)",
                         entry.path().string());
            continue;
        }

        StoredModuleInfo info;
        info.name = manifest["name"].get<std::string>();
        info.version = manifest.value("version", "");
        info.sha256 = expected;
        info.deployed_at_ms = manifest.value("deployed_at_ms", uint64_t{0});
        info.size_bytes = bytes->size();
        info.wasm_path = wasm_path;
        index_[info.name] = std::move(info);
    }

    spdlog::info("ModuleStore: indexed {} persisted module(s) in {}", index_.size(),
                 dir_.string());
    return true;
}

bool ModuleStore::Save(const std::string& name, const std::string& version,
                       const std::vector<uint8_t>& bytecode, std::string& error) {
    if (!ValidModuleName(name)) {
        error = "invalid module name";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto module_dir = dir_ / name;
    std::error_code ec;
    std::filesystem::create_directories(module_dir, ec);
    if (ec) {
        error = "cannot create module directory: " + ec.message();
        return false;
    }

    // Write bytecode to a temp file, then rename over the live one.
    auto wasm_path = module_dir / "module.wasm";
    auto tmp_path = module_dir / "module.wasm.tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out ||
            !out.write(reinterpret_cast<const char*>(bytecode.data()), bytecode.size())) {
            error = "failed to write module bytecode";
            return false;
        }
    }
    std::filesystem::rename(tmp_path, wasm_path, ec);
    if (ec) {
        error = "failed to move module into place: " + ec.message();
        return false;
    }

    StoredModuleInfo info;
    info.name = name;
    info.version = version;
    info.sha256 = Sha256Hex(bytecode.data(), bytecode.size());
    info.deployed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    info.size_bytes = bytecode.size();
    info.wasm_path = wasm_path;

    nlohmann::json manifest = {
        {"name", info.name},
        {"version", info.version},
        {"sha256", info.sha256},
        {"deployed_at_ms", info.deployed_at_ms},
        {"size_bytes", info.size_bytes},
    };
    {
        std::ofstream out(module_dir / "manifest.json", std::ios::trunc);
        if (!out || !(out << manifest.dump(2))) {
            error = "failed to write manifest";
            return false;
        }
    }

    index_[name] = std::move(info);
    return true;
}

bool ModuleStore::Remove(const std::string& name) {
    if (!ValidModuleName(name)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    bool existed = index_.erase(name) > 0;
    std::error_code ec;
    existed |= std::filesystem::remove_all(dir_ / name, ec) > 0;
    if (ec) spdlog::warn("ModuleStore: remove {} failed: {}", name, ec.message());
    return existed;
}

std::optional<StoredModuleInfo> ModuleStore::GetInfo(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(name);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

std::vector<StoredModuleInfo> ModuleStore::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StoredModuleInfo> out;
    out.reserve(index_.size());
    for (const auto& [_, info] : index_) out.push_back(info);
    return out;
}

std::optional<std::vector<uint8_t>> ModuleStore::LoadBytecode(
    const std::string& name, std::string& error) const {
    std::filesystem::path path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(name);
        if (it == index_.end()) {
            error = "module not in store";
            return std::nullopt;
        }
        path = it->second.wasm_path;
    }
    auto bytes = ReadFileBytes(path);
    if (!bytes) error = "cannot read " + path.string();
    return bytes;
}

} // namespace origindb
