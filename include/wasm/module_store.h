#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace origindb {

struct StoredModuleInfo {
    std::string name;
    std::string version;
    std::string sha256;  // hex digest of the bytecode
    uint64_t deployed_at_ms = 0;
    uint64_t size_bytes = 0;
    std::filesystem::path wasm_path;
};

// Durable store for deployed WASM modules:
//   <data_dir>/modules/<name>/module.wasm
//   <data_dir>/modules/<name>/manifest.json
// One active version per name; redeploy overwrites. Table data durability is
// the WAL's job — this only persists module artifacts.
class ModuleStore {
public:
    explicit ModuleStore(std::filesystem::path modules_dir);

    // Creates the directory and indexes existing modules. Corrupt entries
    // (missing manifest, digest mismatch) are skipped with a warning.
    bool Initialize();

    bool Save(const std::string& name, const std::string& version,
              const std::vector<uint8_t>& bytecode, std::string& error);
    bool Remove(const std::string& name);

    std::optional<StoredModuleInfo> GetInfo(const std::string& name) const;
    std::vector<StoredModuleInfo> List() const;
    std::optional<std::vector<uint8_t>> LoadBytecode(const std::string& name,
                                                     std::string& error) const;

    static std::string Sha256Hex(const uint8_t* data, size_t len);

private:
    std::filesystem::path dir_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoredModuleInfo> index_;
};

} // namespace origindb
