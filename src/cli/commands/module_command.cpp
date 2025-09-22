#include "cli/commands/module_command.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace instantdb::cli {

int ModuleCommand::Execute(const std::vector<std::string>& args) {
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }

    auto positional = GetPositionalArgs(args);
    if (positional.empty()) {
        spdlog::error("Module action is required");
        PrintHelp();
        return 1;
    }

    std::string action = positional[0];

    if (action == "init") {
        return InitModule(args);
    } else if (action == "build") {
        return BuildModule(args);
    } else if (action == "deploy") {
        return DeployModule(args);
    } else if (action == "list" || action == "ls") {
        return ListModules(args);
    } else if (action == "remove" || action == "rm") {
        return RemoveModule(args);
    } else {
        spdlog::error("Unknown module action: {}", action);
        PrintHelp();
        return 1;
    }
}

void ModuleCommand::PrintHelp() const {
    std::cout << "Usage: instantdb module <action> [options]\n\n";
    std::cout << "Manage WASM modules\n\n";
    std::cout << "Actions:\n";
    std::cout << "  init <name>               Initialize a new WASM module\n";
    std::cout << "  build [module]            Build WASM module(s)\n";
    std::cout << "  deploy [module]           Deploy module(s) to server\n";
    std::cout << "  list, ls                  List available modules\n";
    std::cout << "  remove, rm <module>       Remove a module\n\n";
    std::cout << "Options:\n";
    std::cout << "  --lang, -l <language>     Programming language (rust/csharp/javascript/go/cpp)\n";
    std::cout << "  --server, -s <url>        Server URL for deployment\n";
    std::cout << "  --all, -a                 Build/deploy all modules\n";
    std::cout << "  --help, -h                Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  instantdb module init --lang rust auth_module\n";
    std::cout << "  instantdb module build\n";
    std::cout << "  instantdb module build auth_module\n";
    std::cout << "  instantdb module deploy --all\n";
    std::cout << "  instantdb module list\n";
}

int ModuleCommand::InitModule(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    if (positional.size() < 2) {
        spdlog::error("Module name is required for init");
        return 1;
    }

    std::string module_name = positional[1];
    std::string lang = GetOption(args, "--lang");
    if (lang.empty()) {
        lang = GetOption(args, "-l");
    }
    if (lang.empty()) {
        lang = "rust"; // Default
    }

    // Validate language
    if (lang != "rust" && lang != "csharp" && lang != "javascript" && lang != "go" && lang != "cpp") {
        spdlog::error("Unsupported language: {}. Supported: rust, csharp, javascript, go, cpp", lang);
        return 1;
    }

    std::string module_path = "./modules/" + module_name;

    // Check if module already exists
    if (std::filesystem::exists(module_path)) {
        spdlog::error("Module '{}' already exists", module_name);
        return 1;
    }

    try {
        spdlog::info("Creating {} module '{}'...", lang, module_name);
        std::filesystem::create_directories(module_path);

        // Use the same module creation logic from InitCommand
        // This is a simplified version - in real implementation, we'd refactor to avoid duplication
        if (lang == "rust") {
            // Create basic Rust WASM module structure
            std::filesystem::create_directories(module_path + "/src");

            std::ofstream cargo(module_path + "/Cargo.toml");
            cargo << "[package]\n";
            cargo << "name = \"" << module_name << "\"\n";
            cargo << "version = \"0.1.0\"\n";
            cargo << "edition = \"2021\"\n\n";
            cargo << "[lib]\n";
            cargo << "crate-type = [\"cdylib\"]\n\n";
            cargo << "[dependencies]\n";
            cargo << "wasm-bindgen = \"0.2\"\n";
            cargo.close();

            std::ofstream lib(module_path + "/src/lib.rs");
            lib << "use wasm_bindgen::prelude::*;\n\n";
            lib << "#[wasm_bindgen]\n";
            lib << "pub fn " << module_name << "_init() {\n";
            lib << "    // Module initialization\n";
            lib << "}\n\n";
            lib << "#[wasm_bindgen]\n";
            lib << "pub fn " << module_name << "_process(input: &str) -> String {\n";
            lib << "    // Process input and return result\n";
            lib << "    format!(\"Processed: {}\", input)\n";
            lib << "}\n";
            lib.close();

            std::ofstream build(module_path + "/build.sh");
            build << "#!/bin/bash\n";
            build << "cargo build --target wasm32-unknown-unknown --release\n";
            build << "cp target/wasm32-unknown-unknown/release/" << module_name << ".wasm ../" << module_name << ".wasm\n";
            build.close();
            std::filesystem::permissions(module_path + "/build.sh",
                                        std::filesystem::perms::owner_exec,
                                        std::filesystem::perm_options::add);
        }

        spdlog::info("Module '{}' created successfully!", module_name);
        spdlog::info("Next steps:");
        spdlog::info("  cd {}", module_path);
        spdlog::info("  Edit source files");
        spdlog::info("  instantdb module build {}", module_name);

        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Failed to create module: {}", e.what());
        return 1;
    }
}

int ModuleCommand::BuildModule(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    bool build_all = HasFlag(args, "--all") || HasFlag(args, "-a");

    std::vector<std::string> modules_to_build;

    if (build_all || positional.size() < 2) {
        // Build all modules
        modules_to_build = FindModules();
        if (modules_to_build.empty()) {
            spdlog::info("No modules found to build");
            return 0;
        }
    } else {
        // Build specific module
        std::string module_name = positional[1];
        std::string module_path = "./modules/" + module_name;
        if (!std::filesystem::exists(module_path)) {
            spdlog::error("Module '{}' not found", module_name);
            return 1;
        }
        modules_to_build.push_back(module_name);
    }

    int failed_builds = 0;
    for (const auto& module : modules_to_build) {
        spdlog::info("Building module '{}'...", module);
        std::string module_path = "./modules/" + module;

        if (!RunBuildScript(module_path)) {
            spdlog::error("Failed to build module '{}'", module);
            failed_builds++;
        } else {
            spdlog::info("Module '{}' built successfully", module);
        }
    }

    if (failed_builds > 0) {
        spdlog::error("{} module(s) failed to build", failed_builds);
        return 1;
    }

    spdlog::info("All modules built successfully!");
    return 0;
}

int ModuleCommand::DeployModule(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    bool deploy_all = HasFlag(args, "--all") || HasFlag(args, "-a");
    std::string server_url = GetServerUrl(args);

    std::vector<std::string> modules_to_deploy;

    if (deploy_all || positional.size() < 2) {
        // Deploy all modules
        modules_to_deploy = FindModules();
        if (modules_to_deploy.empty()) {
            spdlog::info("No modules found to deploy");
            return 0;
        }
    } else {
        // Deploy specific module
        std::string module_name = positional[1];
        std::string wasm_path = "./modules/" + module_name + ".wasm";
        if (!std::filesystem::exists(wasm_path)) {
            spdlog::error("WASM file not found for module '{}'. Run 'instantdb module build {}' first", module_name, module_name);
            return 1;
        }
        modules_to_deploy.push_back(module_name);
    }

    int failed_deploys = 0;
    for (const auto& module : modules_to_deploy) {
        spdlog::info("Deploying module '{}'...", module);
        std::string wasm_path = "./modules/" + module + ".wasm";

        if (!std::filesystem::exists(wasm_path)) {
            spdlog::error("WASM file not found for module '{}'. Build it first.", module);
            failed_deploys++;
            continue;
        }

        if (!DeployToServer(module, wasm_path, server_url)) {
            spdlog::error("Failed to deploy module '{}'", module);
            failed_deploys++;
        } else {
            spdlog::info("Module '{}' deployed successfully", module);
        }
    }

    if (failed_deploys > 0) {
        spdlog::error("{} module(s) failed to deploy", failed_deploys);
        return 1;
    }

    spdlog::info("All modules deployed successfully!");
    return 0;
}

int ModuleCommand::ListModules(const std::vector<std::string>& args) {
    auto modules = FindModules();

    if (modules.empty()) {
        std::cout << "No modules found.\n";
        std::cout << "Use 'instantdb module init <name>' to create a new module.\n";
        return 0;
    }

    std::cout << "Available modules:\n\n";
    for (const auto& module : modules) {
        std::string module_path = "./modules/" + module;
        std::string wasm_path = "./modules/" + module + ".wasm";

        std::cout << "  " << module;

        // Check if built
        if (std::filesystem::exists(wasm_path)) {
            auto wasm_time = std::filesystem::last_write_time(wasm_path);
            std::cout << " (built)";
        } else {
            std::cout << " (not built)";
        }

        // Check language
        if (std::filesystem::exists(module_path + "/Cargo.toml")) {
            std::cout << " [rust]";
        } else if (std::filesystem::exists(module_path + "/package.json")) {
            std::cout << " [javascript]";
        } else if (std::filesystem::exists(module_path + "/go.mod")) {
            std::cout << " [go]";
        } else if (std::filesystem::exists(module_path + "/main.csproj")) {
            std::cout << " [csharp]";
        } else if (std::filesystem::exists(module_path + "/CMakeLists.txt")) {
            std::cout << " [cpp]";
        }

        std::cout << "\n";
    }

    return 0;
}

int ModuleCommand::RemoveModule(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    if (positional.size() < 2) {
        spdlog::error("Module name is required for remove");
        return 1;
    }

    std::string module_name = positional[1];
    std::string module_path = "./modules/" + module_name;

    if (!std::filesystem::exists(module_path)) {
        spdlog::error("Module '{}' not found", module_name);
        return 1;
    }

    try {
        std::filesystem::remove_all(module_path);

        // Also remove WASM file if it exists
        std::string wasm_path = "./modules/" + module_name + ".wasm";
        if (std::filesystem::exists(wasm_path)) {
            std::filesystem::remove(wasm_path);
        }

        spdlog::info("Module '{}' removed successfully", module_name);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Failed to remove module '{}': {}", module_name, e.what());
        return 1;
    }
}

bool ModuleCommand::RunBuildScript(const std::string& module_path) {
    std::string build_script = module_path + "/build.sh";
    if (!std::filesystem::exists(build_script)) {
        spdlog::error("Build script not found: {}", build_script);
        return false;
    }

    // Change to module directory and run build script
    std::string command = "cd " + module_path + " && ./build.sh";
    int result = system(command.c_str());

    return result == 0;
}

bool ModuleCommand::DeployToServer(const std::string& module_name, const std::string& wasm_path, const std::string& server_url) {
    // This is a simplified deployment - in real implementation, use proper HTTP client
    spdlog::info("Deploying {} to {}", module_name, server_url);

    // For now, just copy to a deployment directory
    std::string deploy_dir = "./deployed_modules";
    std::filesystem::create_directories(deploy_dir);

    try {
        std::filesystem::copy_file(wasm_path, deploy_dir + "/" + module_name + ".wasm",
                                  std::filesystem::copy_options::overwrite_existing);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to deploy module: {}", e.what());
        return false;
    }
}

std::vector<std::string> ModuleCommand::FindModules() const {
    std::vector<std::string> modules;

    if (!std::filesystem::exists("./modules")) {
        return modules;
    }

    for (const auto& entry : std::filesystem::directory_iterator("./modules")) {
        if (entry.is_directory()) {
            modules.push_back(entry.path().filename().string());
        }
    }

    return modules;
}

std::string ModuleCommand::GetServerUrl(const std::vector<std::string>& args) const {
    std::string server = GetOption(args, "--server");
    if (server.empty()) {
        server = GetOption(args, "-s");
    }
    if (server.empty()) {
        server = "http://localhost:50051"; // Default gRPC endpoint
    }
    return server;
}

} // namespace instantdb::cli