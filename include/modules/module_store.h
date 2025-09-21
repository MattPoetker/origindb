#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

#include "common/config.h"
#include "wasm/wasm_runtime.h"

namespace instantdb {

class StorageEngine;
class WasmRuntime;

// Module metadata
struct ModuleMetadata {
    std::string id;
    std::string version;
    std::string digest;
    ModuleCapabilities capabilities;
    std::chrono::system_clock::time_point uploaded_at;
    std::chrono::system_clock::time_point installed_at;
    bool is_installed = false;
    bool is_revoked = false;
    std::string revocation_reason;
};

// Module store for managing WASM modules
class ModuleStore {
public:
    ModuleStore(std::shared_ptr<StorageEngine> storage,
                std::shared_ptr<WasmRuntime> runtime,
                const ModuleConfig& config);
    ~ModuleStore();

    bool Initialize();
    void Shutdown();

    // Module lifecycle
    std::string UploadModule(const std::string& module_id,
                            const std::string& version,
                            const std::vector<uint8_t>& wasm_bytes,
                            const ModuleCapabilities& capabilities);

    bool InstallModule(const std::string& module_id,
                      const std::string& version,
                      const std::string& expected_digest = "");

    bool RevokeModule(const std::string& module_id,
                     const std::string& version,
                     const std::string& reason);

    // Module retrieval
    std::shared_ptr<WasmModule> GetModule(const std::string& module_id,
                                          const std::string& version = "");

    std::optional<ModuleMetadata> GetModuleMetadata(const std::string& module_id,
                                                    const std::string& version = "") const;

    std::vector<ModuleMetadata> ListModules(bool include_revoked = false) const;

    // Module binary management
    std::optional<std::vector<uint8_t>> GetModuleBinary(const std::string& module_id,
                                                        const std::string& version) const;

    bool DeleteModuleBinary(const std::string& module_id,
                           const std::string& version);

    // Module validation
    bool ValidateModule(const std::vector<uint8_t>& wasm_bytes,
                       std::string& error) const;

    bool VerifySignature(const std::vector<uint8_t>& wasm_bytes,
                        const std::vector<uint8_t>& signature) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace instantdb