#include "grpc/wasm_service_impl.h"
#include "wasm/wasm_engine.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

// Include protobuf headers
#include "instantdb.grpc.pb.h"

namespace instantdb {

WasmServiceImpl::WasmServiceImpl(std::shared_ptr<WasmEngine> wasm_engine)
    : wasm_engine_(wasm_engine) {
}

void* WasmServiceImpl::DeployModule(void* context,
                                   const void* request,
                                   void* response) {
    auto grpc_context = static_cast<::grpc::ServerContext*>(context);
    auto deploy_request = static_cast<const instantdb::grpc::DeployModuleRequest*>(request);
    auto deploy_response = static_cast<instantdb::grpc::DeployModuleResponse*>(response);

    static thread_local ::grpc::Status status;

    spdlog::info("DeployModule called for module: {}", deploy_request->name());

    try {
        // Convert bytecode to vector
        std::vector<uint8_t> bytecode(deploy_request->bytecode().begin(),
                                     deploy_request->bytecode().end());

        // Load the module into the WASM engine
        bool success = wasm_engine_->LoadModule(deploy_request->name(), bytecode);

        if (success) {
            deploy_response->set_success(true);
            deploy_response->set_module_id(deploy_request->name());
            spdlog::info("Successfully deployed module: {}", deploy_request->name());
        } else {
            deploy_response->set_success(false);
            deploy_response->set_error("Failed to load WASM module");
            spdlog::error("Failed to deploy module: {}", deploy_request->name());
        }

        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        deploy_response->set_success(false);
        deploy_response->set_error("Exception during module deployment: " + std::string(e.what()));
        spdlog::error("Exception in DeployModule: {}", e.what());
        status = ::grpc::Status::OK;
    }

    return &status;
}

void* WasmServiceImpl::UndeployModule(void* context,
                                     const void* request,
                                     void* response) {
    auto undeploy_request = static_cast<const instantdb::grpc::UndeployModuleRequest*>(request);
    auto undeploy_response = static_cast<instantdb::grpc::UndeployModuleResponse*>(response);

    static thread_local ::grpc::Status status;

    spdlog::info("UndeployModule called for module: {}", undeploy_request->name());

    try {
        bool success = wasm_engine_->UnloadModule(undeploy_request->name());
        undeploy_response->set_success(success);
        if (success) {
            spdlog::info("Successfully undeployed module: {}", undeploy_request->name());
        } else {
            undeploy_response->set_error("Module not found or failed to unload");
            spdlog::warn("Failed to undeploy module: {}", undeploy_request->name());
        }

        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        undeploy_response->set_success(false);
        undeploy_response->set_error("Exception during module undeployment: " + std::string(e.what()));
        spdlog::error("Exception in UndeployModule: {}", e.what());
        status = ::grpc::Status::OK;
    }

    return &status;
}

void* WasmServiceImpl::ListModules(void* context,
                                  const void* request,
                                  void* response) {
    auto list_response = static_cast<instantdb::grpc::ListModulesResponse*>(response);

    static thread_local ::grpc::Status status;

    spdlog::info("ListModules called - starting");

    try {
        auto module_names = wasm_engine_->ListModules();
        spdlog::info("Got {} module names", module_names.size());

        for (const auto& module_name : module_names) {
            auto* module_info = list_response->add_modules();
            module_info->set_name(module_name);
            module_info->set_version("1.0.0"); // Default version
            module_info->set_loaded(true);
            module_info->set_validated(true);
            // Note: Reducers will be populated when GetModule is called
        }

        spdlog::info("ListModules returning {} modules", module_names.size());
        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        spdlog::error("Exception in ListModules: {}", e.what());
        status = ::grpc::Status(::grpc::StatusCode::INTERNAL, "Failed to list modules");
    }

    return &status;
}

void* WasmServiceImpl::GetModule(void* context,
                                const void* request,
                                void* response) {
    auto get_request = static_cast<const instantdb::grpc::GetModuleRequest*>(request);
    auto get_response = static_cast<instantdb::grpc::GetModuleResponse*>(response);

    static thread_local ::grpc::Status status;

    spdlog::debug("GetModule called for: {}", get_request->name());

    try {
        auto module = wasm_engine_->GetModule(get_request->name());

        if (module) {
            get_response->set_success(true);
            auto* module_info = get_response->mutable_module();
            module_info->set_name(module->GetName());
            module_info->set_version("1.0.0"); // Default version
            module_info->set_loaded(true);
            module_info->set_validated(true);

            // Add reducers from metadata
            const auto& metadata = module->GetMetadata();
            for (const auto& reducer : metadata.reducers) {
                auto* reducer_def = module_info->add_reducers();
                reducer_def->set_name(reducer.name);
                reducer_def->set_is_init(reducer.is_init);
                reducer_def->set_is_client_connected(reducer.is_client_connected);
                reducer_def->set_is_client_disconnected(reducer.is_client_disconnected);
            }

            spdlog::debug("GetModule found module: {}", get_request->name());
        } else {
            get_response->set_success(false);
            get_response->set_error("Module not found");
            spdlog::warn("GetModule: module not found: {}", get_request->name());
        }

        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        get_response->set_success(false);
        get_response->set_error("Exception while getting module: " + std::string(e.what()));
        spdlog::error("Exception in GetModule: {}", e.what());
        status = ::grpc::Status::OK;
    }

    return &status;
}

void* WasmServiceImpl::ExecuteReducer(void* context,
                                     const void* request,
                                     void* response) {
    auto exec_request = static_cast<const instantdb::grpc::ExecuteReducerRequest*>(request);
    auto exec_response = static_cast<instantdb::grpc::ExecuteReducerResponse*>(response);

    static thread_local ::grpc::Status status;

    spdlog::info("ExecuteReducer called: {}.{}", exec_request->module_name(), exec_request->reducer_name());

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
                    args.push_back(std::vector<uint8_t>(arg.bytes_value().begin(), arg.bytes_value().end()));
                    break;
                default:
                    args.push_back(std::monostate{}); // null value
                    break;
            }
        }

        // Create reducer context
        ReducerContext ctx;
        ctx.sender_identity = exec_request->sender_identity();
        ctx.connection_id = exec_request->connection_id();
        ctx.module_identity = exec_request->module_name();
        ctx.timestamp_micros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ctx.storage = wasm_engine_->GetStorageEngine();
        ctx.changefeed = wasm_engine_->GetChangefeedEngine();

        auto start_time = std::chrono::high_resolution_clock::now();

        // Execute the reducer
        auto result = wasm_engine_->ExecuteReducer(
            exec_request->module_name(),
            exec_request->reducer_name(),
            ctx,
            args
        );

        auto end_time = std::chrono::high_resolution_clock::now();
        auto execution_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

        exec_response->set_success(result.success);
        exec_response->set_execution_time_micros(execution_time.count());

        if (result.success) {
            // Convert results back to protobuf
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

            spdlog::info("ExecuteReducer successful: {}.{}", exec_request->module_name(), exec_request->reducer_name());
        } else {
            exec_response->set_error(result.error);
            spdlog::error("ExecuteReducer failed: {}.{}: {}",
                         exec_request->module_name(), exec_request->reducer_name(), result.error);
        }

        status = ::grpc::Status::OK;

    } catch (const std::exception& e) {
        exec_response->set_success(false);
        exec_response->set_error("Exception during reducer execution: " + std::string(e.what()));
        spdlog::error("Exception in ExecuteReducer: {}", e.what());
        status = ::grpc::Status::OK;
    }

    return &status;
}

} // namespace instantdb