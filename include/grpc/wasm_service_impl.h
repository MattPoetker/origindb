#pragma once

#include <memory>

namespace origindb {

class WasmEngine;
class ModuleStore;
class AuthManager;

class WasmServiceImpl {
public:
    WasmServiceImpl(std::shared_ptr<WasmEngine> wasm_engine,
                    std::shared_ptr<ModuleStore> module_store = nullptr,
                    std::shared_ptr<AuthManager> auth = nullptr);

    // WASM service methods
    void* DeployModule(void* context, const void* request, void* response);
    void* UndeployModule(void* context, const void* request, void* response);
    void* ListModules(void* context, const void* request, void* response);
    void* GetModule(void* context, const void* request, void* response);
    void* ExecuteReducer(void* context, const void* request, void* response);

private:
    std::shared_ptr<WasmEngine> wasm_engine_;
    std::shared_ptr<ModuleStore> module_store_;
    std::shared_ptr<AuthManager> auth_;
};

} // namespace origindb
