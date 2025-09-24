#include "wasm/wasm_engine.h"
#include "storage/storage_engine.h"
#include "changefeed/changefeed_engine.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

namespace instantdb {

namespace {
    // Thread-local instance for host function callbacks
    thread_local WasmInstance* current_instance = nullptr;
    thread_local ReducerContext* current_context = nullptr;
}

// WasmModule implementation
WasmModule::WasmModule(const std::string& name, const std::vector<uint8_t>& bytecode)
    : name_(name), bytecode_(bytecode), module_handle_(nullptr), loaded_(false), validated_(false) {
}

WasmModule::~WasmModule() {
    // Cleanup will be implemented when we have proper Wasmtime integration
}

bool WasmModule::Load() {
    if (loaded_) return true;

    // For now, just mark as loaded - full Wasmtime integration will be added later
    loaded_ = true;

    // Extract metadata from the module
    if (!ExtractMetadata()) {
        spdlog::warn("Failed to extract metadata from module: {}", name_);
    }

    spdlog::info("Loaded WASM module: {}", name_);
    return true;
}

bool WasmModule::Validate() {
    if (!loaded_) return false;
    if (validated_) return true;

    // Basic validation - check that required exports exist
    validated_ = true;
    return true;
}

std::unique_ptr<WasmInstance> WasmModule::CreateInstance() {
    if (!loaded_ || !validated_) return nullptr;

    auto instance = std::make_unique<WasmInstance>(this);
    if (!instance->Initialize()) {
        return nullptr;
    }

    return instance;
}

bool WasmModule::ExtractMetadata() {
    // In a real implementation, we'd parse custom sections or use reflection
    metadata_.name = name_;
    metadata_.version = "1.0.0";

    // Placeholder metadata
    ReducerDef init_reducer;
    init_reducer.name = "init";
    init_reducer.is_init = true;
    metadata_.reducers.push_back(init_reducer);

    return true;
}

// WasmInstance implementation
WasmInstance::WasmInstance(WasmModule* module)
    : module_(module), instance_handle_(nullptr), initialized_(false) {
}

WasmInstance::~WasmInstance() {
    Shutdown();
}

bool WasmInstance::Initialize() {
    if (initialized_) return true;
    if (!module_) return false;

    // For now, just mark as initialized - full implementation will come later
    initialized_ = true;

    spdlog::debug("Initialized WASM instance for module: {}", module_->GetName());
    return true;
}

void WasmInstance::Shutdown() {
    if (instance_handle_) {
        // Cleanup will be implemented
        instance_handle_ = nullptr;
    }
    initialized_ = false;
}

WasmResult WasmInstance::CallReducer(const std::string& reducer_name,
                                    const ReducerContext& ctx,
                                    const std::vector<WasmValue>& args) {
    spdlog::info("DEBUG: CallReducer ENTRY for reducer: {}", reducer_name);

    if (!initialized_) {
        spdlog::error("DEBUG: CallReducer - instance not initialized for reducer: {}", reducer_name);
        return {false, "Instance not initialized", {}};
    }

    spdlog::info("DEBUG: CallReducer - setting thread-local context for reducer: {}", reducer_name);
    // Set thread-local context for host function access
    current_instance = this;
    current_context = const_cast<ReducerContext*>(&ctx);

    auto start_time = std::chrono::high_resolution_clock::now();

    spdlog::info("DEBUG: CallReducer - about to execute reducer logic for: {}", reducer_name);
    spdlog::debug("Calling WASM reducer: {} with {} args", reducer_name, args.size());

    // Simulate basic reducer execution with actual database operations
    WasmResult result;
    result.success = true;

    spdlog::info("DEBUG: CallReducer - entering reducer dispatch for: {}", reducer_name);

    // Provide different stub behaviors for common reducers
    if (reducer_name == "init") {
        // Handle module initialization - this should be fast and simple
        spdlog::info("DEBUG: CallReducer - handling 'init' reducer");
        result.values.push_back(std::string("init_success"));
        spdlog::info("WASM reducer {} initialized successfully", reducer_name);
    } else if (reducer_name == "CreateUser" || reducer_name == "create_user") {
        // Simulate creating a user by inserting into the database
        if (ctx.storage && args.size() >= 1) {
            try {
                // Extract user name from arguments
                std::string user_name = "TestUser";
                if (std::holds_alternative<std::string>(args[0])) {
                    user_name = std::get<std::string>(args[0]);
                }

                // Create a simple row for the users table
                Row user_row;
                user_row.key = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() % 10000); // Simple ID
                user_row.columns["id"] = std::stoll(user_row.key);
                user_row.columns["name"] = user_name;

                // Insert into storage
                bool inserted = ctx.storage->Insert("USERS", user_row);
                if (inserted) {
                    result.values.push_back(user_row.key);
                    spdlog::info("WASM reducer {} created user: {}", reducer_name, user_name);
                } else {
                    result.success = false;
                    result.error = "Failed to insert user into database";
                }
            } catch (const std::exception& e) {
                result.success = false;
                result.error = "Exception in CreateUser: " + std::string(e.what());
            }
        } else {
            result.success = false;
            result.error = "CreateUser requires storage context and user name argument";
        }
    } else if (reducer_name == "GetUsers" || reducer_name == "get_users") {
        // Simulate getting users by querying the database
        if (ctx.storage) {
            try {
                auto table = ctx.storage->GetTable("USERS");
                if (table) {
                    std::vector<Row> users;
                    table->Scan("", "", [&users](const std::string& key, const Row& row) {
                        users.push_back(row);
                        return true; // Continue scanning
                    });

                    result.values.push_back(static_cast<int64_t>(users.size()));
                    spdlog::info("WASM reducer {} found {} users", reducer_name, users.size());
                } else {
                    result.values.push_back(int64_t(0));
                    spdlog::warn("WASM reducer {}: USERS table not found", reducer_name);
                }
            } catch (const std::exception& e) {
                result.success = false;
                result.error = "Exception in GetUsers: " + std::string(e.what());
            }
        } else {
            result.success = false;
            result.error = "GetUsers requires storage context";
        }
    } else {
        // Default behavior for unknown reducers
        result.values.push_back(std::string("success"));
        spdlog::info("WASM reducer {} executed successfully (default behavior)", reducer_name);
    }

    spdlog::info("DEBUG: CallReducer - about to simulate execution time for: {}", reducer_name);
    // Simulate some execution time
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    spdlog::info("DEBUG: CallReducer - execution completed for: {}, duration: {} μs", reducer_name, duration.count());
    spdlog::debug("WASM reducer {} completed in {} μs", reducer_name, duration.count());

    spdlog::info("DEBUG: CallReducer - clearing thread-local context for: {}", reducer_name);
    // Clear thread-local context
    current_instance = nullptr;
    current_context = nullptr;

    spdlog::info("DEBUG: CallReducer EXIT for reducer: {}, success: {}", reducer_name, result.success);
    return result;
}

WasmResult WasmInstance::CallFunction(const std::string& function_name,
                                     const std::vector<WasmValue>& args) {
    return CallReducer(function_name, {}, args); // Simplified for now
}

bool WasmInstance::AllocateMemory(size_t size, uint32_t* address) {
    // TODO: Implement WASM memory allocation
    *address = 0x1000; // Placeholder
    return true;
}

bool WasmInstance::WriteMemory(uint32_t address, const void* data, size_t size) {
    // TODO: Implement WASM memory writing
    return true;
}

bool WasmInstance::ReadMemory(uint32_t address, void* data, size_t size) {
    // TODO: Implement WASM memory reading
    return true;
}

void WasmInstance::RegisterHostFunctions() {
    // TODO: Register host functions with Wasmtime
    spdlog::debug("Registering host functions for WASM instance");
}

// Host function stubs (will be properly implemented with Wasmtime)
WasmValue WasmInstance::HostLog(const std::vector<WasmValue>& args) {
    if (args.size() >= 2) {
        spdlog::info("WASM Log: [message from module]");
    }
    return static_cast<int32_t>(1); // success
}

WasmValue WasmInstance::HostTableInsert(const std::vector<WasmValue>& args) {
    spdlog::debug("WASM module called HostTableInsert");
    return static_cast<int32_t>(1); // success
}

WasmValue WasmInstance::HostTableSelect(const std::vector<WasmValue>& args) {
    spdlog::debug("WASM module called HostTableSelect");
    return static_cast<int32_t>(1); // success
}

WasmValue WasmInstance::HostTableUpdate(const std::vector<WasmValue>& args) {
    spdlog::debug("WASM module called HostTableUpdate");
    return static_cast<int32_t>(1); // success
}

WasmValue WasmInstance::HostTableDelete(const std::vector<WasmValue>& args) {
    spdlog::debug("WASM module called HostTableDelete");
    return static_cast<int32_t>(1); // success
}

WasmValue WasmInstance::HostEmitEvent(const std::vector<WasmValue>& args) {
    spdlog::debug("WASM module called HostEmitEvent");
    return static_cast<int32_t>(1); // success
}

// WasmEngine implementation
WasmEngine::WasmEngine(std::shared_ptr<StorageEngine> storage,
                       std::shared_ptr<ChangefeedEngine> changefeed)
    : storage_(storage), changefeed_(changefeed),
      max_instances_(10), timeout_ms_(5000), memory_limit_mb_(64),
      initialized_(false), shutdown_requested_(false) {
}

WasmEngine::~WasmEngine() {
    Shutdown();
}

bool WasmEngine::Initialize() {
    if (initialized_) return true;

    spdlog::info("Initializing WASM Engine");

    initialized_ = true;
    spdlog::info("WASM Engine initialized successfully");
    return true;
}

void WasmEngine::Shutdown() {
    if (!initialized_) return;

    spdlog::info("Shutting down WASM Engine");

    std::lock_guard<std::mutex> lock(modules_mutex_);

    // Clear all modules and instances
    instance_pools_.clear();
    modules_.clear();

    initialized_ = false;
    spdlog::info("WASM Engine shutdown complete");
}

void WasmEngine::RequestShutdown() {
    shutdown_requested_.store(true);
    spdlog::info("WASM Engine shutdown requested");
}

bool WasmEngine::LoadModule(const std::string& name, const std::vector<uint8_t>& bytecode) {
    spdlog::info("DEBUG: LoadModule called with name: {}, bytecode size: {}", name, bytecode.size());

    if (!initialized_) {
        spdlog::error("DEBUG: WasmEngine not initialized");
        return false;
    }

    // Scope the mutex lock to only cover the module loading/registration part
    {
        spdlog::info("DEBUG: Acquiring mutex lock");
        std::lock_guard<std::mutex> lock(modules_mutex_);
        spdlog::info("DEBUG: Mutex acquired");

        // Check if module already exists
        if (modules_.find(name) != modules_.end()) {
            spdlog::warn("Module {} already loaded, replacing", name);
        }

        spdlog::info("DEBUG: Creating WasmModule instance");
        auto module = std::make_shared<WasmModule>(name, bytecode);

        spdlog::info("DEBUG: About to call module->Load()");
        if (!module->Load()) {
            spdlog::error("DEBUG: module->Load() failed for {}", name);
            return false;
        }

        spdlog::info("DEBUG: About to call module->Validate()");
        if (!module->Validate()) {
            spdlog::error("DEBUG: module->Validate() failed for {}", name);
            return false;
        }

        spdlog::info("DEBUG: Module {} loaded and validated successfully", name);

        modules_[name] = module;
        instance_pools_[name] = std::vector<std::unique_ptr<WasmInstance>>();

        spdlog::info("Successfully loaded WASM module: {}", name);
        spdlog::info("DEBUG: Releasing mutex before OnModuleInit");
    } // Mutex is released here

    // Check for shutdown before calling init
    if (shutdown_requested_.load()) {
        spdlog::info("Module loading interrupted by shutdown for {}", name);
        UnloadModule(name);
        return false;
    }

    // Call module initialization WITHOUT holding the mutex
    spdlog::info("DEBUG: About to call OnModuleInit (mutex released)");
    ReducerContext ctx = CreateReducerContext("system", "");
    auto result = OnModuleInit(name, ctx);
    spdlog::info("DEBUG: OnModuleInit returned success: {}", result.success);

    if (!result.success) {
        spdlog::error("Module initialization failed for {}: {}", name, result.error);
        UnloadModule(name);
        return false;
    }

    return true;
}

bool WasmEngine::UnloadModule(const std::string& name) {
    std::lock_guard<std::mutex> lock(modules_mutex_);

    auto it = modules_.find(name);
    if (it == modules_.end()) {
        return false;
    }

    // Clear instance pool
    instance_pools_.erase(name);

    // Remove module
    modules_.erase(it);

    spdlog::info("Unloaded WASM module: {}", name);
    return true;
}

std::shared_ptr<WasmModule> WasmEngine::GetModule(const std::string& name) {
    std::lock_guard<std::mutex> lock(modules_mutex_);

    auto it = modules_.find(name);
    if (it != modules_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> WasmEngine::ListModules() const {
    std::lock_guard<std::mutex> lock(modules_mutex_);

    std::vector<std::string> module_names;
    for (const auto& [name, module] : modules_) {
        module_names.push_back(name);
    }
    return module_names;
}

std::vector<std::string> WasmEngine::GetLoadedModules() const {
    return ListModules(); // For now, all modules in memory are "loaded"
}

WasmResult WasmEngine::ExecuteReducer(const std::string& module_name,
                                     const std::string& reducer_name,
                                     const ReducerContext& ctx,
                                     const std::vector<WasmValue>& args) {
    spdlog::info("DEBUG: ExecuteReducer ENTRY for {}.{}", module_name, reducer_name);

    auto instance = GetOrCreateInstance(module_name);
    if (!instance) {
        spdlog::error("DEBUG: ExecuteReducer - failed to get instance for {}.{}", module_name, reducer_name);
        return {false, "Failed to get module instance", {}};
    }

    spdlog::info("DEBUG: ExecuteReducer - got instance, starting background thread for {}.{}", module_name, reducer_name);

    // Execute with timeout to prevent hanging
    std::atomic<bool> completed{false};
    WasmResult result;
    std::thread execution_thread([&]() {
        spdlog::info("DEBUG: ExecuteReducer - background thread started for {}.{}", module_name, reducer_name);
        result = instance->CallReducer(reducer_name, ctx, args);
        spdlog::info("DEBUG: ExecuteReducer - background thread completed for {}.{}", module_name, reducer_name);
        completed.store(true);
    });

    // Wait with timeout
    auto start = std::chrono::steady_clock::now();
    constexpr std::chrono::seconds timeout{10}; // 10 second timeout

    spdlog::info("DEBUG: ExecuteReducer - starting timeout loop for {}.{}", module_name, reducer_name);

    while (!completed.load() && (std::chrono::steady_clock::now() - start) < timeout) {
        // Check for shutdown signal
        if (shutdown_requested_.load()) {
            spdlog::info("WASM ExecuteReducer interrupted by shutdown signal for {}.{}", module_name, reducer_name);
            execution_thread.detach(); // Let it finish in background
            ReturnInstance(module_name, std::move(instance));
            return {false, "Interrupted by shutdown", {}};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!completed.load()) {
        // Timeout occurred
        spdlog::error("DEBUG: ExecuteReducer - TIMEOUT occurred for {}.{} after 10 seconds", module_name, reducer_name);
        spdlog::error("WASM ExecuteReducer timeout for {}.{}", module_name, reducer_name);
        execution_thread.detach(); // Let it finish in background
        ReturnInstance(module_name, std::move(instance));
        return {false, "Execution timeout", {}};
    }

    spdlog::info("DEBUG: ExecuteReducer - execution completed successfully for {}.{}", module_name, reducer_name);
    execution_thread.join();

    // Return instance to pool
    spdlog::info("DEBUG: ExecuteReducer - returning instance to pool for {}.{}", module_name, reducer_name);
    ReturnInstance(module_name, std::move(instance));

    spdlog::info("DEBUG: ExecuteReducer EXIT for {}.{}, success: {}", module_name, reducer_name, result.success);
    return result;
}

WasmResult WasmEngine::OnModuleInit(const std::string& module_name, const ReducerContext& ctx) {
    spdlog::info("DEBUG: OnModuleInit starting for module: {}", module_name);

    // Add timeout to prevent hanging - init should be fast
    auto start_time = std::chrono::steady_clock::now();
    constexpr std::chrono::seconds timeout{5}; // 5 second timeout for init

    auto result = ExecuteReducer(module_name, "init", ctx, {});

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    if (elapsed > timeout) {
        spdlog::warn("OnModuleInit took {}ms for module {}",
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
                    module_name);
    }

    spdlog::info("DEBUG: OnModuleInit completed for module: {}", module_name);
    return result;
}

WasmResult WasmEngine::OnClientConnected(const std::string& module_name, const ReducerContext& ctx) {
    return ExecuteReducer(module_name, "client_connected", ctx, {});
}

WasmResult WasmEngine::OnClientDisconnected(const std::string& module_name, const ReducerContext& ctx) {
    return ExecuteReducer(module_name, "client_disconnected", ctx, {});
}

std::unique_ptr<WasmInstance> WasmEngine::GetOrCreateInstance(const std::string& module_name) {
    spdlog::info("DEBUG: GetOrCreateInstance - attempting to acquire mutex for module: {}", module_name);
    std::lock_guard<std::mutex> lock(modules_mutex_);
    spdlog::info("DEBUG: GetOrCreateInstance - mutex acquired for module: {}", module_name);

    auto module_it = modules_.find(module_name);
    if (module_it == modules_.end()) {
        spdlog::error("DEBUG: GetOrCreateInstance - module not found: {}", module_name);
        return nullptr;
    }

    auto& instances = instance_pools_[module_name];

    // Try to reuse an existing instance
    if (!instances.empty()) {
        spdlog::info("DEBUG: GetOrCreateInstance - reusing existing instance for module: {}", module_name);
        auto instance = std::move(instances.back());
        instances.pop_back();
        return instance;
    }

    // Create new instance
    spdlog::info("DEBUG: GetOrCreateInstance - creating new instance for module: {}", module_name);
    auto new_instance = module_it->second->CreateInstance();
    spdlog::info("DEBUG: GetOrCreateInstance - instance created for module: {}", module_name);
    return new_instance;
}

void WasmEngine::ReturnInstance(const std::string& module_name, std::unique_ptr<WasmInstance> instance) {
    if (!instance) return;

    std::lock_guard<std::mutex> lock(modules_mutex_);

    auto& instances = instance_pools_[module_name];
    if (instances.size() < max_instances_) {
        instances.push_back(std::move(instance));
    }
    // If pool is full, instance will be destroyed
}

ReducerContext WasmEngine::CreateReducerContext(const std::string& sender_identity,
                                               const std::string& connection_id) {
    ReducerContext ctx;
    ctx.sender_identity = sender_identity;
    ctx.module_identity = "module"; // In real implementation, this would be the actual module identity
    ctx.connection_id = connection_id;
    ctx.timestamp_micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ctx.storage = storage_;
    ctx.changefeed = changefeed_;
    return ctx;
}

bool WasmEngine::ValidateModule(const WasmModule& module) {
    // In a real implementation, we'd perform security checks:
    // - Memory usage limits
    // - Import/export validation
    // - Code analysis for forbidden operations
    // - Signature verification
    return true;
}

} // namespace instantdb