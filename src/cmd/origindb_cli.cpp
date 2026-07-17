#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <spdlog/spdlog.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

// ANSI color codes
const std::string RESET = "\033[0m";
const std::string BOLD = "\033[1m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";

// Version information
const std::string VERSION = "1.0.0";

// Command descriptions
struct Command {
    std::string binary;
    std::string description;
    std::string usage;
};

std::map<std::string, Command> commands = {
    {"server", {"origindb_server", "Start the OriginDB server", "origindb server [OPTIONS]"}},
    {"sql", {"origindb_sql", "SQL interactive shell", "origindb sql [OPTIONS]"}},
    {"client", {"origindb_client", "Connect as a client", "origindb client [OPTIONS]"}},
    {"demo", {"origindb_demo", "Run the demo application", "origindb demo"}},
    {"init", {"origindb_init", "Initialize a new OriginDB project", "origindb init [PROJECT_NAME]"}},
    {"publish", {"", "Build and deploy WASM module", "origindb publish [OPTIONS]"}},
    {"migrate", {"", "Run database migrations", "origindb migrate [OPTIONS]"}},
    {"backup", {"", "Backup the database", "origindb backup [OPTIONS]"}},
    {"restore", {"", "Restore from backup", "origindb restore [BACKUP_FILE]"}},
    {"logs", {"", "View server logs", "origindb logs [OPTIONS]"}},
    {"start", {"origindb_server", "Start the OriginDB server (alias for server)", "origindb start [OPTIONS]"}},
    {"stop", {"", "Stop the OriginDB server", "origindb stop"}},
};

void printHeader() {
    std::cout << CYAN << BOLD << R"(
╦┌┐┌┌─┐┌┬┐┌─┐┌┐┌┌┬┐╔╦╗╔╗
║│││└─┐ │ ├─┤│││ │  ║║╠╩╗
╩┘└┘└─┘ ┴ ┴ ┴┘└┘ ┴ ═╩╝╚═╝)" << RESET << " v" << VERSION << "\n\n";
}

void printUsage() {
    printHeader();

    std::cout << BOLD << "Usage:" << RESET << " origindb <command> [options]\n\n";

    std::cout << BOLD << "Commands:\n" << RESET;
    std::cout << "  " << GREEN << std::left << std::setw(12) << "init" << RESET
              << " Initialize a new OriginDB project\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "server" << RESET
              << " Start the OriginDB server\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "start" << RESET
              << " Start the OriginDB server (alias)\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "stop" << RESET
              << " Stop the OriginDB server\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "logs" << RESET
              << " View server logs\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "sql" << RESET
              << " SQL interactive shell\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "publish" << RESET
              << " Build and deploy WASM module\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "client" << RESET
              << " Connect as a client\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "demo" << RESET
              << " Run the demo application\n";

    std::cout << "\n" << BOLD << "Global Options:\n" << RESET;
    std::cout << "  " << YELLOW << "-h, --help" << RESET << "     Show help information\n";
    std::cout << "  " << YELLOW << "-v, --version" << RESET << "  Show version information\n";
    std::cout << "  " << YELLOW << "--verbose" << RESET << "      Enable verbose output\n";

    std::cout << "\n" << BOLD << "Examples:\n" << RESET;
    std::cout << "  " << BLUE << "origindb init myproject" << RESET << "      # Initialize new project\n";
    std::cout << "  " << BLUE << "origindb server -p 9090" << RESET << "      # Start server on port 9090\n";
    std::cout << "  " << BLUE << "origindb publish" << RESET << "             # Build and deploy WASM module\n";
    std::cout << "  " << BLUE << "origindb sql" << RESET << "                 # Open SQL shell\n";
    std::cout << "  " << BLUE << "origindb logs --follow" << RESET << "       # View logs with live updates\n";

    std::cout << "\n" << BOLD << "Documentation:\n" << RESET;
    std::cout << "  https://docs.origindb.com\n";
    std::cout << "  https://github.com/origindb/origindb\n\n";
}

void printVersion() {
    printHeader();
    std::cout << "OriginDB CLI version " << VERSION << "\n";
    std::cout << "Copyright (c) 2024 OriginDB Team\n";
    std::cout << "MIT License\n\n";
}

void printCommandHelp(const std::string& command) {
    if (commands.find(command) == commands.end()) {
        std::cerr << RED << "Error: Unknown command '" << command << "'" << RESET << "\n";
        return;
    }

    const auto& cmd = commands[command];
    std::cout << BOLD << "Command: " << RESET << command << "\n";
    std::cout << BOLD << "Description: " << RESET << cmd.description << "\n";
    std::cout << BOLD << "Usage: " << RESET << cmd.usage << "\n\n";

    // Command-specific help
    if (command == "server") {
        std::cout << BOLD << "Server Options:\n" << RESET;
        std::cout << "  -p, --port PORT        WebSocket port (default: 8080)\n";
        std::cout << "  -g, --grpc-port PORT   gRPC port (default: 50051)\n";
        std::cout << "  -d, --data-dir DIR     Data directory (default: ./origindb_data)\n";
        std::cout << "  -l, --log-level LEVEL  Log level: trace,debug,info,warn,error\n";
        std::cout << "  -c, --config FILE      Config file path\n";
    } else if (command == "sql") {
        std::cout << BOLD << "SQL Options:\n" << RESET;
        std::cout << "  -h, --host HOST        Server host (default: localhost)\n";
        std::cout << "  -p, --port PORT        Server port (default: 8080)\n";
        std::cout << "  -f, --file FILE        Execute SQL from file\n";
        std::cout << "  -o, --output FILE      Output results to file\n";
        std::cout << "  --format FORMAT        Output format: table,json,csv\n";
    } else if (command == "init") {
        std::cout << BOLD << "Init Options:\n" << RESET;
        std::cout << "  --template TEMPLATE    Project template: csharp,unity,nodejs,python\n";
        std::cout << "  --no-git              Don't initialize git repository\n";
        std::cout << "  --force               Overwrite existing files\n";
    } else if (command == "publish") {
        std::cout << BOLD << "Publish Options:\n" << RESET;
        std::cout << "  --server URL          OriginDB server URL (default: http://localhost:9090)\n";
        std::cout << "  --path PATH           Project path (default: current directory)\n";
        std::cout << "\n" << BOLD << "Requirements:\n" << RESET;
        std::cout << "  - .NET 8+ SDK\n";
        std::cout << "  - WASM tools workload: dotnet workload install wasm-tools\n";
        std::cout << "  - grpcurl for deployment\n";
    }

    std::cout << "\n";
}

std::string findBinary(const std::string& name) {
    // First, check if binary exists in the same directory as this CLI
    char exe_path[1024];
    ssize_t len = -1;

#ifdef __APPLE__
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) == 0) {
        len = strlen(exe_path);
    }
#elif __linux__
    len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
#endif

    if (len != -1) {
        exe_path[len] = '\0';
        fs::path cli_dir = fs::path(exe_path).parent_path();
        fs::path binary_path = cli_dir / name;

        if (fs::exists(binary_path)) {
            return binary_path.string();
        }
    }

    // Check PATH
    const char* path_env = std::getenv("PATH");
    if (path_env) {
        std::stringstream ss(path_env);
        std::string dir;

        while (std::getline(ss, dir, ':')) {
            fs::path binary_path = fs::path(dir) / name;
            if (fs::exists(binary_path)) {
                return binary_path.string();
            }
        }
    }

    // Check common installation directories
    std::vector<std::string> common_dirs = {
        "/usr/local/bin",
        "/usr/bin",
        "/opt/origindb/bin",
        "./build",  // For development
        "."         // Current directory
    };

    for (const auto& dir : common_dirs) {
        fs::path binary_path = fs::path(dir) / name;
        if (fs::exists(binary_path)) {
            return binary_path.string();
        }
    }

    return "";
}

int handleLogsCommand(const std::vector<std::string>& args) {
    bool follow = false;
    int lines = 50;
    std::string log_file = "./logs/origindb.log";

    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        const auto& arg = args[i];
        if (arg == "--follow" || arg == "-f") {
            follow = true;
        } else if ((arg == "--lines" || arg == "-n") && i + 1 < args.size()) {
            lines = std::stoi(args[++i]);
        } else if (arg == "--file" && i + 1 < args.size()) {
            log_file = args[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << BOLD << "Usage:" << RESET << " origindb logs [OPTIONS]\n\n";
            std::cout << BOLD << "Options:\n" << RESET;
            std::cout << "  -f, --follow    Follow log output (like tail -f)\n";
            std::cout << "  -n, --lines N   Show last N lines (default: 50)\n";
            std::cout << "  --file PATH     Log file path (default: ./logs/origindb.log)\n";
            std::cout << "  -h, --help      Show this help message\n\n";
            std::cout << BOLD << "Examples:\n" << RESET;
            std::cout << "  origindb logs                # Show last 50 lines\n";
            std::cout << "  origindb logs -n 100         # Show last 100 lines\n";
            std::cout << "  origindb logs --follow       # Follow log output\n";
            return 0;
        }
    }

    // Check if log file exists
    if (!fs::exists(log_file)) {
        std::cerr << YELLOW << "Log file not found: " << log_file << RESET << "\n";
        std::cerr << "The server may not be running or logs may be in a different location.\n";
        return 1;
    }

    // Display logs
    if (follow) {
        std::string cmd = "tail -f " + log_file;
        return system(cmd.c_str());
    } else {
        std::string cmd = "tail -n " + std::to_string(lines) + " " + log_file;
        return system(cmd.c_str());
    }
}

int handleStopCommand(const std::vector<std::string>& args) {
    std::cout << CYAN << "Stopping OriginDB server..." << RESET << "\n";

    // Try to find and kill the origindb_server process
    int result = system("pkill -f origindb_server");

    if (result == 0) {
        std::cout << GREEN << "✅ Server stopped successfully" << RESET << "\n";
        return 0;
    } else {
        std::cerr << YELLOW << "No running OriginDB server found" << RESET << "\n";
        return 1;
    }
}

int handlePublishCommand(const std::vector<std::string>& args) {
    std::string grpc_endpoint = "localhost:50051";
    std::string project_path = ".";
    std::string version = "1.0.0";
    std::string token;
    if (const char* t = std::getenv("ORIGINDB_TOKEN")) token = t;

    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        const auto& arg = args[i];
        if (arg == "--server" && i + 1 < args.size()) {
            grpc_endpoint = args[++i];
        } else if (arg.substr(0, 9) == "--server=") {
            grpc_endpoint = arg.substr(9);
        } else if (arg == "--path" && i + 1 < args.size()) {
            project_path = args[++i];
        } else if (arg.substr(0, 7) == "--path=") {
            project_path = arg.substr(7);
        } else if (arg == "--version" && i + 1 < args.size()) {
            version = args[++i];
        } else if (arg.substr(0, 10) == "--version=") {
            version = arg.substr(10);
        } else if ((arg == "--token" || arg == "-t") && i + 1 < args.size()) {
            token = args[++i];
        } else if (arg.substr(0, 8) == "--token=") {
            token = arg.substr(8);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << BOLD << "Usage:" << RESET << " origindb publish [OPTIONS]\n\n";
            std::cout << BOLD << "Options:\n" << RESET;
            std::cout << "  --server HOST:PORT  OriginDB gRPC endpoint (default: localhost:50051)\n";
            std::cout << "  --path PATH         Project path (default: current directory)\n";
            std::cout << "  --version VERSION   Module version (default: 1.0.0)\n";
            std::cout << "  --token TOKEN       Admin token (or ORIGINDB_TOKEN env var)\n";
            std::cout << "  -h, --help          Show this help message\n\n";
            std::cout << BOLD << "Supported projects:\n" << RESET;
            std::cout << "  C# (.csproj):       .NET 8 + wasi-experimental workload\n";
            std::cout << "  TypeScript (asconfig.json): AssemblyScript via npm run asbuild\n\n";
            std::cout << BOLD << "Examples:\n" << RESET;
            std::cout << "  origindb publish\n";
            std::cout << "  origindb publish --path=./my-module --server=prod.example.com:50051\n";
            return 0;
        }
    }

    std::cout << CYAN << BOLD << "🚀 Publishing WASM module..." << RESET << "\n\n";

    // Change to project directory
    char original_dir[1024];
    if (getcwd(original_dir, sizeof(original_dir)) == nullptr) {
        std::cerr << RED << "Error: Could not get current directory" << RESET << "\n";
        return 1;
    }

    if (chdir(project_path.c_str()) != 0) {
        std::cerr << RED << "Error: Could not change to project directory: " << project_path << RESET << "\n";
        return 1;
    }

    // Detect project type and build.
    std::string wasm_file;
    std::string project_name;
    fs::path current_path = fs::current_path();

    if (fs::exists("asconfig.json")) {
        // AssemblyScript (TypeScript) project
        project_name = current_path.filename().string();
        std::cout << "📦 Building AssemblyScript module...\n";
        if (system("npm run asbuild --silent") != 0) {
            std::cerr << RED << "Error: Build failed. Run 'npm install' first." << RESET << "\n";
            chdir(original_dir);
            return 1;
        }
        if (fs::exists("build/module.wasm")) {
            wasm_file = "build/module.wasm";
        } else if (fs::exists("build/release.wasm")) {
            wasm_file = "build/release.wasm";
        }
    } else {
        // C# project
        for (const auto& entry : fs::directory_iterator(current_path)) {
            if (entry.path().extension() == ".csproj") {
                project_name = entry.path().stem().string();
                break;
            }
        }
        if (project_name.empty()) {
            std::cerr << RED << "Error: No .csproj or asconfig.json found in "
                      << project_path << RESET << "\n";
            chdir(original_dir);
            return 1;
        }

        std::cout << "📦 Building .NET WASM module...\n";
        if (system("dotnet publish --configuration Release --verbosity quiet") != 0) {
            std::cerr << RED << "Error: Build failed. Requires .NET 8 SDK with the wasi-experimental workload." << RESET << "\n";
            std::cerr << "Try: dotnet workload install wasi-experimental\n";
            chdir(original_dir);
            return 1;
        }

        std::vector<std::string> wasm_paths = {
            "bin/Release/net8.0/wasi-wasm/AppBundle/" + project_name + ".wasm",
            "bin/Release/net8.0/wasi-wasm/publish/" + project_name + ".wasm",
        };
        for (const auto& path : wasm_paths) {
            if (fs::exists(path)) {
                wasm_file = path;
                break;
            }
        }
    }

    if (wasm_file.empty() || !fs::exists(wasm_file)) {
        std::cerr << RED << "Error: Built WASM file not found" << RESET << "\n";
        std::cerr << "Build may have failed or the output path is unexpected.\n";
        chdir(original_dir);
        return 1;
    }

    std::cout << GREEN << "✅ Build successful: " << wasm_file << RESET << "\n";
    std::cout << "🌐 Deploying to " << grpc_endpoint << "\n";

    // Deploy through the bundled gRPC client (no grpcurl dependency).
    std::string client_binary = findBinary("origindb_client");
    if (client_binary.empty()) {
        std::cerr << RED << "Error: origindb_client binary not found (built without gRPC support?)" << RESET << "\n";
        chdir(original_dir);
        return 1;
    }

    std::string token_arg = token.empty() ? "" : " --token \"" + token + "\"";
    std::string deploy_cmd = "\"" + client_binary + "\"" + token_arg + " --server " + grpc_endpoint +
                             " deploy \"" + project_name + "\" \"" + wasm_file +
                             "\" \"" + version + "\"";
    int deploy_result = system(deploy_cmd.c_str());
    chdir(original_dir);

    if (deploy_result == 0) {
        std::cout << GREEN << BOLD << "\n🎉 Module published successfully!" << RESET << "\n\n";
        std::cout << "Module Name: " << CYAN << project_name << RESET << "\n";
        std::cout << "Server: " << CYAN << grpc_endpoint << RESET << "\n\n";
        std::cout << BOLD << "Test your reducers:" << RESET << "\n";
        std::cout << "  origindb client -s " << grpc_endpoint << " call "
                  << project_name << " <ReducerName> '[\"arg1\", 2]'\n";
        std::cout << "  origindb client -s " << grpc_endpoint << " modules\n";
        return 0;
    } else {
        std::cerr << RED << "Error: Deployment failed. Is the OriginDB server running?" << RESET << "\n";
        return 1;
    }
}

int executeCommand(const std::string& command, const std::vector<std::string>& args) {
    if (commands.find(command) == commands.end()) {
        std::cerr << RED << "Error: Unknown command '" << command << "'" << RESET << "\n";
        std::cerr << "Run 'origindb --help' for usage information.\n";
        return 1;
    }

    const auto& cmd = commands[command];

    // Handle built-in commands
    if (cmd.binary.empty()) {
        if (command == "publish") {
            return handlePublishCommand(args);
        } else if (command == "logs") {
            return handleLogsCommand(args);
        } else if (command == "stop") {
            return handleStopCommand(args);
        } else if (command == "migrate") {
            std::cout << GREEN << "Running database migrations..." << RESET << "\n";
            // TODO: Implement migrations
            std::cout << "Migrations not yet implemented.\n";
            return 0;
        } else if (command == "backup") {
            std::cout << GREEN << "Creating database backup..." << RESET << "\n";
            // TODO: Implement backup
            std::cout << "Backup not yet implemented.\n";
            return 0;
        } else if (command == "restore") {
            std::cout << GREEN << "Restoring from backup..." << RESET << "\n";
            // TODO: Implement restore
            std::cout << "Restore not yet implemented.\n";
            return 0;
        }
    }

    // Find and execute the binary
    std::string binary_path = findBinary(cmd.binary);

    if (binary_path.empty()) {
        std::cerr << RED << "Error: Could not find '" << cmd.binary << "' binary." << RESET << "\n";
        std::cerr << "Please ensure OriginDB is properly installed.\n";
        std::cerr << "Run 'origindb --help' for more information.\n";
        return 1;
    }

    // Build command line
    std::vector<char*> exec_args;
    exec_args.push_back(const_cast<char*>(binary_path.c_str()));

    for (const auto& arg : args) {
        exec_args.push_back(const_cast<char*>(arg.c_str()));
    }
    exec_args.push_back(nullptr);

    // Execute the command
    execvp(binary_path.c_str(), exec_args.data());

    // If we get here, execvp failed
    std::cerr << RED << "Error: Failed to execute '" << binary_path << "': "
              << strerror(errno) << RESET << "\n";
    return 1;
}

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

    if (argc < 2) {
        printUsage();
        return 0;
    }

    std::string first_arg(argv[1]);

    // Handle global options
    if (first_arg == "-h" || first_arg == "--help") {
        if (argc > 2) {
            printCommandHelp(argv[2]);
        } else {
            printUsage();
        }
        return 0;
    }

    if (first_arg == "-v" || first_arg == "--version") {
        printVersion();
        return 0;
    }

    // Check if it's a help request for a specific command
    if (argc > 2 && (std::string(argv[2]) == "-h" || std::string(argv[2]) == "--help")) {
        printCommandHelp(first_arg);
        return 0;
    }

    // Collect remaining arguments
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++) {
        args.push_back(argv[i]);
    }

    // Execute the command
    return executeCommand(first_arg, args);
}