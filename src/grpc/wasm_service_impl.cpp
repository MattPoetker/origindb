#include "grpc/wasm_service_impl.h"
#include "wasm/wasm_engine.h"
#include "wasm/module_store.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

// Include protobuf headers
#include "instantdb.grpc.pb.h"

namespace instantdb {

namespace {

constexpr size_t kMaxModuleBytes = 64 * 1024 * 1024;

ModuleCapabilities CapabilitiesFromProto(
    const instantdb::grpc::ModuleCapabilities& proto) {
    ModuleCapabilities caps;
    for (const auto& t : proto.allowed_tables()) caps.allowed_tables.push_back(t);
    caps.read_only = proto.read_only();
    caps.max_memory_mb = proto.max_memory_mb();
    caps.timeout_ms = proto.timeout_ms();
    return caps;
}

}  // namespace

WasmServiceImpl::WasmServiceImpl(std::shared_ptr<WasmEngine> wasm_engine,
                                 std::shared_ptr<ModuleStore> module_store)
    : wasm_engine_(std::move(wasm_engine)), module_store_(std::move(module_store)) {}

void* WasmServiceImpl::DeployModule(void* context,
                                    const void* request,
                                    void* response) {
    (void)context;
    auto deploy_request = static_cast<const instantdb::grpc::DeployModuleRequest*>(request);
    auto deploy_response = static_cast<instantdb::grpc::DeployModuleResponse*>(response);

    static thread_local ::grpc::Status status;

    spdlog::info("DeployModule: {} ({} bytes)", deploy_request->name(),
                 deploy_request->bytecode().size());

    try {
        if (deploy_request->name().empty() || deploy_request->bytecode().empty()) {
            deploy_response->set_success(false);
            deploy_response->set_error("module name and bytecode are required");
            status = ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                    "module name and bytecode are required");
            return &status;
        }
        if (deploy_request->bytecode().size() > kMaxModuleBytes) {
            deploy_response->set_success(false);
            deploy_response->set_error("module exceeds 64MiB size limit");
            status = ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                    "module exceeds 64MiB size limit");
            return &status;
        }

        std::vector<uint8_t> bytecode(deploy_request->bytecode().begin(),
                                      deploy_request->bytecode().end());
        ModuleCapabilities caps = CapabilitiesFromProto(deploy_request->capabilities());

        bool loaded = wasm_engine_->LoadModule(deploy_request->name(), bytecode,
                                               deploy_request->version(), caps);
        if (!loaded) {
            std::string error = wasm_engine_->GetLastLoadError();
            deploy_response->set_success(false);
            deploy_response->set_error(error.empty() ? "failed to load WASM module"
                                                     : error);
            spdlog::error("Failed to deploy module {}: {}", deploy_request->name(),
                          error);
            status = ::grpc::Status::OK;  // application-level rejection
            return &status;
        }

        // Deploy is atomic: memory + disk, or neither.
        if (module_store_) {
            std::string save_error;
            if (!module_store_->Save(deploy_request->name(),
                                     deploy_request->version(), bytecode,
                                     save_error)) {
                wasm_engine_->UnloadModule(deploy_request->name());
                deploy_response->set_success(false);
                deploy_response->set_error("failed to persist module: " + save_error);
                spdlog::error("Failed to persist module {}: {}",
                              deploy_request->name(), save_error);
                status = ::grpc::Status(::grpc::StatusCode::INTERNAL, save_error);
                return &status;
            }
        }

        deploy_response->set_success(true);
        deploy_response->set_module_id(deploy_request->name());
        spdlog::info("Deployed module: {}", deploy_request->name());
        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        deploy_response->set_success(false);
        deploy_response->set_error("exception during module deployment: " +
                                   std::string(e.what()));
        spdlog::error("Exception in DeployModule: {}", e.what());
        status = ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }

    return &status;
}

void* WasmServiceImpl::UndeployModule(void* context,
                                      const void* request,
                                      void* response) {
    (void)context;
    auto undeploy_request = static_cast<const instantdb::grpc::UndeployModuleRequest*>(request);
    auto undeploy_response = static_cast<instantdb::grpc::UndeployModuleResponse*>(response);

    static thread_local ::grpc::Status status;

    try {
        bool success = wasm_engine_->UnloadModule(undeploy_request->name());
        if (module_store_) success |= module_store_->Remove(undeploy_request->name());

        undeploy_response->set_success(success);
        if (success) {
            spdlog::info("Undeployed module: {}", undeploy_request->name());
            status = ::grpc::Status::OK;
        } else {
            undeploy_response->set_error("module not found");
            status = ::grpc::Status(::grpc::StatusCode::NOT_FOUND,
                                    "module not found: " + undeploy_request->name());
        }

    } catch (const std::exception& e) {
        undeploy_response->set_success(false);
        undeploy_response->set_error("exception during module undeployment: " +
                                     std::string(e.what()));
        spdlog::error("Exception in UndeployModule: {}", e.what());
        status = ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }

    return &status;
}

void* WasmServiceImpl::ListModules(void* context,
                                   const void* request,
                                   void* response) {
    (void)context;
    (void)request;
    auto list_response = static_cast<instantdb::grpc::ListModulesResponse*>(response);

    static thread_local ::grpc::Status status;

    try {
        for (const auto& module_name : wasm_engine_->ListModules()) {
            auto* module_info = list_response->add_modules();
            module_info->set_name(module_name);

            auto module = wasm_engine_->GetModule(module_name);
            if (module) {
                module_info->set_version(module->GetMetadata().version);
                module_info->set_loaded(module->IsLoaded());
                module_info->set_validated(module->IsLoaded());
            }
            if (module_store_) {
                if (auto info = module_store_->GetInfo(module_name)) {
                    module_info->set_sha256(info->sha256);
                    module_info->set_deployed_at_ms(info->deployed_at_ms);
                    if (module_info->version().empty())
                        module_info->set_version(info->version);
                }
            }
        }
        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        spdlog::error("Exception in ListModules: {}", e.what());
        status = ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }

    return &status;
}

void* WasmServiceImpl::GetModule(void* context,
                                 const void* request,
                                 void* response) {
    (void)context;
    auto get_request = static_cast<const instantdb::grpc::GetModuleRequest*>(request);
    auto get_response = static_cast<instantdb::grpc::GetModuleResponse*>(response);

    static thread_local ::grpc::Status status;

    try {
        auto module = wasm_engine_->GetModule(get_request->name());
        if (!module) {
            get_response->set_success(false);
            get_response->set_error("module not found");
            status = ::grpc::Status(::grpc::StatusCode::NOT_FOUND,
                                    "module not found: " + get_request->name());
            return &status;
        }

        get_response->set_success(true);
        auto* module_info = get_response->mutable_module();
        module_info->set_name(module->GetName());
        module_info->set_version(module->GetMetadata().version);
        module_info->set_loaded(module->IsLoaded());
        module_info->set_validated(module->IsLoaded());

        for (const auto& reducer : module->GetMetadata().reducers) {
            auto* reducer_def = module_info->add_reducers();
            reducer_def->set_name(reducer.name);
            reducer_def->set_is_init(reducer.is_init);
            reducer_def->set_is_client_connected(reducer.is_client_connected);
            reducer_def->set_is_client_disconnected(reducer.is_client_disconnected);
        }
        if (module_store_) {
            if (auto info = module_store_->GetInfo(get_request->name())) {
                module_info->set_sha256(info->sha256);
                module_info->set_deployed_at_ms(info->deployed_at_ms);
            }
        }
        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        get_response->set_success(false);
        get_response->set_error("exception while getting module: " +
                                std::string(e.what()));
        spdlog::error("Exception in GetModule: {}", e.what());
        status = ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }

    return &status;
}

void* WasmServiceImpl::ExecuteReducer(void* context,
                                      const void* request,
                                      void* response) {
    (void)context;
    auto exec_request = static_cast<const instantdb::grpc::ExecuteReducerRequest*>(request);
    auto exec_response = static_cast<instantdb::grpc::ExecuteReducerResponse*>(response);

    static thread_local ::grpc::Status status;

    spdlog::debug("ExecuteReducer: {}.{}", exec_request->module_name(),
                  exec_request->reducer_name());

    try {
        // Convert arguments from protobuf to WasmValue
        std::vector<WasmValue> args;
        for (const auto& arg : exec_request->args()) {
            switch (arg.value_case()) {
                case instantdb::grpc::WasmValue::kBoolValue:
                    args.push_back(arg.bool_value());
                    break;
                case instantdb::grpc::WasmValue::kInt64Value:
                    args.push_back(arg.int64_value());
                    break;
                case instantdb::grpc::WasmValue::kStringValue:
                    args.push_back(arg.string_value());
                    break;
                case instantdb::grpc::WasmValue::kDoubleValue:
                    args.push_back(arg.double_value());
                    break;
                case instantdb::grpc::WasmValue::kBytesValue:
                    args.push_back(std::vector<uint8_t>(arg.bytes_value().begin(),
                                                        arg.bytes_value().end()));
                    break;
                default:
                    args.push_back(std::monostate{});  // null value
                    break;
            }
        }

        ReducerContext ctx = wasm_engine_->CreateReducerContext(
            exec_request->sender_identity(), exec_request->connection_id());
        ctx.module_identity = exec_request->module_name();

        auto start_time = std::chrono::high_resolution_clock::now();
        auto result = wasm_engine_->ExecuteReducer(exec_request->module_name(),
                                                   exec_request->reducer_name(),
                                                   ctx, args);
        auto end_time = std::chrono::high_resolution_clock::now();
        auto execution_time =
            std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

        exec_response->set_success(result.success);
        exec_response->set_execution_time_micros(execution_time.count());

        if (result.success) {
            for (const auto& value : result.values) {
                auto* result_value = exec_response->add_results();
                std::visit([result_value](const auto& v) {
                    using T = std::decay_t<decltype(v)>;

                    if constexpr (std::is_same_v<T, std::monostate>) {
                        // NULL value - no field set
                    } else if constexpr (std::is_same_v<T, bool>) {
                        result_value->set_bool_value(v);
                    } else if constexpr (std::is_same_v<T, int32_t>) {
                        result_value->set_int64_value(static_cast<int64_t>(v));
                    } else if constexpr (std::is_same_v<T, int64_t>) {
                        result_value->set_int64_value(v);
                    } else if constexpr (std::is_same_v<T, float>) {
                        result_value->set_double_value(static_cast<double>(v));
                    } else if constexpr (std::is_same_v<T, double>) {
                        result_value->set_double_value(v);
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        result_value->set_string_value(v);
                    } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                        result_value->set_bytes_value(std::string(v.begin(), v.end()));
                    }
                }, value);
            }
        } else {
            exec_response->set_error(result.error);
            spdlog::warn("ExecuteReducer failed: {}.{}: {}", exec_request->module_name(),
                         exec_request->reducer_name(), result.error);
        }

        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        exec_response->set_success(false);
        exec_response->set_error("exception during reducer execution: " +
                                 std::string(e.what()));
        spdlog::error("Exception in ExecuteReducer: {}", e.what());
        status = ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }

    return &status;
}

} // namespace instantdb
