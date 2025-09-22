#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <filesystem>
#include <spdlog/spdlog.h>

#include "cli/commands/init_command.h"
#include "cli/commands/server_command.h"
#include "cli/commands/logs_command.h"
#include "cli/commands/module_command.h"
#include "cli/commands/database_command.h"
#include "cli/commands/exec_command.h"
#include "cli/commands/build_command.h"

namespace instantdb::cli {

class CLI {
public:
    CLI() {
        RegisterCommands();
    }

    int Run(int argc, char* argv[]) {
        if (argc < 2) {
            PrintUsage();
            return 1;
        }

        std::string command = argv[1];
        std::vector<std::string> args;
        for (int i = 2; i < argc; i++) {
            args.emplace_back(argv[i]);
        }

        if (command == "--help" || command == "-h") {
            PrintHelp();
            return 0;
        }

        if (command == "--version" || command == "-v") {
            PrintVersion();
            return 0;
        }

        auto it = commands_.find(command);
        if (it != commands_.end()) {
            try {
                return it->second->Execute(args);
            } catch (const std::exception& e) {
                spdlog::error("Command '{}' failed: {}", command, e.what());
                return 1;
            }
        }

        spdlog::error("Unknown command: {}", command);
        PrintUsage();
        return 1;
    }

private:
    void RegisterCommands() {
        commands_["init"] = std::make_unique<InitCommand>();
        commands_["server"] = std::make_unique<ServerCommand>();
        commands_["start"] = std::make_unique<ServerCommand>(); // Alias for server start
        commands_["stop"] = std::make_unique<ServerCommand>(); // Alias for server stop
        commands_["logs"] = std::make_unique<LogsCommand>();
        commands_["module"] = std::make_unique<ModuleCommand>();
        commands_["mod"] = std::make_unique<ModuleCommand>(); // Alias
        commands_["database"] = std::make_unique<DatabaseCommand>();
        commands_["db"] = std::make_unique<DatabaseCommand>(); // Alias
        commands_["exec"] = std::make_unique<ExecCommand>();
        commands_["build"] = std::make_unique<BuildCommand>();
    }

    void PrintUsage() {
        std::cout << "Usage: instantdb <command> [options]\n\n";
        std::cout << "Commands:\n";
        std::cout << "  init <name>               Initialize a new InstantDB project\n";
        std::cout << "  server <action>           Manage the InstantDB server (start/stop/status)\n";
        std::cout << "  logs [options]            View server logs\n";
        std::cout << "  module <action>           Manage WASM modules (init/build/deploy/list)\n";
        std::cout << "  database <action>         Manage databases (create/list/drop)\n";
        std::cout << "  exec <reducer>            Execute a reducer function\n";
        std::cout << "  build [target]            Build project components\n\n";
        std::cout << "Use 'instantdb <command> --help' for more information about a command.\n";
    }

    void PrintHelp() {
        PrintUsage();
        std::cout << "\nGlobal Options:\n";
        std::cout << "  --help, -h                Show help information\n";
        std::cout << "  --version, -v             Show version information\n";
        std::cout << "  --verbose                 Enable verbose output\n";
        std::cout << "  --config <file>           Use custom config file\n\n";
        std::cout << "Environment Variables:\n";
        std::cout << "  INSTANTDB_CONFIG_PATH     Path to config file\n";
        std::cout << "  INSTANTDB_DATA_DIR        Data directory path\n";
        std::cout << "  INSTANTDB_LOG_LEVEL       Log level (debug/info/warn/error)\n";
    }

    void PrintVersion() {
        std::cout << "InstantDB CLI v0.1.0\n";
    }

    std::map<std::string, std::unique_ptr<Command>> commands_;
};

} // namespace instantdb::cli

int main(int argc, char* argv[]) {
    // Set up logging
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    // Check for verbose flag
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--verbose") {
            spdlog::set_level(spdlog::level::debug);
            break;
        }
    }

    instantdb::cli::CLI cli;
    return cli.Run(argc, argv);
}