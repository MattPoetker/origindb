#pragma once

#include <memory>

namespace instantdb {

class WasmEngine;

class WasmServiceImpl {
public:
    explicit WasmServiceImpl(std::shared_ptr<WasmEngine> wasm_engine);

    // WASM service methods
    void* DeployModule(void* context, const void* request, void* response);
    void* UndeployModule(void* context, const void* request, void* response);
    void* ListModules(void* context, const void* request, void* response);
    void* GetModule(void* context, const void* request, void* response);
    void* ExecuteReducer(void* context, const void* request, void* response);

private:
    std::shared_ptr<WasmEngine> wasm_engine_;
};

} // namespace instantdb