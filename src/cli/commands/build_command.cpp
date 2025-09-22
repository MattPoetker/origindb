#include "cli/commands/build_command.h"
#include <iostream>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <cstdlib>
#include <thread>

namespace instantdb::cli {

int BuildCommand::Execute(const std::vector<std::string>& args) {
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }

    auto positional = GetPositionalArgs(args);
    std::string target = positional.empty() ? "all" : positional[0];

    if (target == "server") {
        return BuildServer(args);
    } else if (target == "client") {
        return BuildClient(args);
    } else if (target == "all") {
        return BuildAll(args);
    } else if (target == "clean") {
        return CleanBuild(args);
    } else {
        spdlog::error("Unknown build target: {}", target);
        PrintHelp();
        return 1;
    }
}

void BuildCommand::PrintHelp() const {
    std::cout << "Usage: instantdb build [target] [options]\n\n";
    std::cout << "Build InstantDB project components\n\n";
    std::cout << "Targets:\n";
    std::cout << "  all                       Build all components (default)\n";
    std::cout << "  server                    Build server only\n";
    std::cout << "  client                    Build client only\n";
    std::cout << "  clean                     Clean build directory\n\n";
    std::cout << "Options:\n";
    std::cout << "  --release, -r             Build in release mode\n";
    std::cout << "  --debug, -d               Build in debug mode (default)\n";
    std::cout << "  --jobs, -j <num>          Number of parallel jobs\n";
    std::cout << "  --help, -h                Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  instantdb build\n";
    std::cout << "  instantdb build server --release\n";
    std::cout << "  instantdb build --jobs 4\n";
    std::cout << "  instantdb build clean\n";
}

int BuildCommand::BuildServer(const std::vector<std::string>& args) {
    spdlog::info("Building InstantDB server...");

    std::string build_type = HasFlag(args, "--release") || HasFlag(args, "-r") ? "Release" : "Debug";

    if (!RunCMake(build_type)) {
        return 1;
    }

    if (!RunMake("instantdb_server")) {
        return 1;
    }

    spdlog::info("Server built successfully!");
    return 0;
}

int BuildCommand::BuildClient(const std::vector<std::string>& args) {
    spdlog::info("Building InstantDB client...");

    std::string build_type = HasFlag(args, "--release") || HasFlag(args, "-r") ? "Release" : "Debug";

    if (!RunCMake(build_type)) {
        return 1;
    }

    if (!RunMake("instantdb_client")) {
        return 1;
    }

    spdlog::info("Client built successfully!");
    return 0;
}

int BuildCommand::BuildAll(const std::vector<std::string>& args) {
    spdlog::info("Building all InstantDB components...");

    std::string build_type = HasFlag(args, "--release") || HasFlag(args, "-r") ? "Release" : "Debug";

    if (!RunCMake(build_type)) {
        return 1;
    }

    if (!RunMake("all")) {
        return 1;
    }

    spdlog::info("All components built successfully!");
    return 0;
}

int BuildCommand::CleanBuild(const std::vector<std::string>& args) {
    spdlog::info("Cleaning build directory...");

    try {
        if (std::filesystem::exists("build")) {
            std::filesystem::remove_all("build");
            spdlog::info("Build directory cleaned successfully");
        } else {
            spdlog::info("Build directory does not exist");
        }
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Failed to clean build directory: {}", e.what());
        return 1;
    }
}

bool BuildCommand::RunCMake(const std::string& build_type) {
    // Create build directory if it doesn't exist
    std::filesystem::create_directories("build");

    std::string cmake_command = "cmake -S . -B build -DCMAKE_BUILD_TYPE=" + build_type;
    spdlog::info("Running: {}", cmake_command);

    int result = system(cmake_command.c_str());
    if (result != 0) {
        spdlog::error("CMake configuration failed");
        return false;
    }

    return true;
}

bool BuildCommand::RunMake(const std::string& target) {
    std::string jobs_arg;

    // Try to detect number of CPU cores for parallel builds
    unsigned int num_cores = std::thread::hardware_concurrency();
    if (num_cores > 0) {
        jobs_arg = " -j" + std::to_string(num_cores);
    }

    std::string make_command = "cmake --build build --target " + target + jobs_arg;
    spdlog::info("Running: {}", make_command);

    int result = system(make_command.c_str());
    if (result != 0) {
        spdlog::error("Build failed for target: {}", target);
        return false;
    }

    return true;
}

} // namespace instantdb::cli