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
    {"server", {"instantdb_server", "Start the InstantDB server", "instantdb server [OPTIONS]"}},
    {"sql", {"instantdb_sql", "SQL interactive shell", "instantdb sql [OPTIONS]"}},
    {"client", {"instantdb_client", "Connect as a client", "instantdb client [OPTIONS]"}},
    {"demo", {"instantdb_demo", "Run the demo application", "instantdb demo"}},
    {"init", {"instantdb_init", "Initialize a new InstantDB project", "instantdb init [PROJECT_NAME]"}},
    {"publish", {"", "Build and deploy WASM module", "instantdb publish [OPTIONS]"}},
    {"migrate", {"", "Run database migrations", "instantdb migrate [OPTIONS]"}},
    {"backup", {"", "Backup the database", "instantdb backup [OPTIONS]"}},
    {"restore", {"", "Restore from backup", "instantdb restore [BACKUP_FILE]"}},
    {"logs", {"", "View server logs", "instantdb logs [OPTIONS]"}},
    {"start", {"instantdb_server", "Start the InstantDB server (alias for server)", "instantdb start [OPTIONS]"}},
    {"stop", {"", "Stop the InstantDB server", "instantdb stop"}},
};

void printHeader() {
    std::cout << CYAN << BOLD << R"(
╦┌┐┌┌─┐┌┬┐┌─┐┌┐┌┌┬┐╔╦╗╔╗
║│││└─┐ │ ├─┤│││ │  ║║╠╩╗
╩┘└┘└─┘ ┴ ┴ ┴┘└┘ ┴ ═╩╝╚═╝)" << RESET << " v" << VERSION << "\n\n";
}

void printUsage() {
    printHeader();

    std::cout << BOLD << "Usage:" << RESET << " instantdb <command> [options]\n\n";

    std::cout << BOLD << "Commands:\n" << RESET;
    std::cout << "  " << GREEN << std::left << std::setw(12) << "init" << RESET
              << " Initialize a new InstantDB project\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "server" << RESET
              << " Start the InstantDB server\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "start" << RESET
              << " Start the InstantDB server (alias)\n";
    std::cout << "  " << GREEN << std::left << std::setw(12) << "stop" << RESET
              << " Stop the InstantDB server\n";
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
    std::cout << "  " << BLUE << "instantdb init myproject" << RESET << "      # Initialize new project\n";
    std::cout << "  " << BLUE << "instantdb server -p 9090" << RESET << "      # Start server on port 9090\n";
    std::cout << "  " << BLUE << "instantdb publish" << RESET << "             # Build and deploy WASM module\n";
    std::cout << "  " << BLUE << "instantdb sql" << RESET << "                 # Open SQL shell\n";
    std::cout << "  " << BLUE << "instantdb logs --follow" << RESET << "       # View logs with live updates\n";

    std::cout << "\n" << BOLD << "Documentation:\n" << RESET;
    std::cout << "  https://docs.instantdb.com\n";
    std::cout << "  https://github.com/instantdb/instantdb\n\n";
}

void printVersion() {
    printHeader();
    std::cout << "InstantDB CLI version " << VERSION << "\n";
    std::cout << "Copyright (c) 2024 InstantDB Team\n";
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
        std::cout << "  -d, --data-dir DIR     Data directory (default: ./instantdb_data)\n";
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
        std::cout << "  --server URL          InstantDB server URL (default: http://localhost:9090)\n";
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
        "/opt/instantdb/bin",
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
    std::string log_file = "./logs/instantdb.log";

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
            std::cout << BOLD << "Usage:" << RESET << " instantdb logs [OPTIONS]\n\n";
            std::cout << BOLD << "Options:\n" << RESET;
            std::cout << "  -f, --follow    Follow log output (like tail -f)\n";
            std::cout << "  -n, --lines N   Show last N lines (default: 50)\n";
            std::cout << "  --file PATH     Log file path (default: ./logs/instantdb.log)\n";
            std::cout << "  -h, --help      Show this help message\n\n";
            std::cout << BOLD << "Examples:\n" << RESET;
            std::cout << "  instantdb logs                # Show last 50 lines\n";
            std::cout << "  instantdb logs -n 100         # Show last 100 lines\n";
            std::cout << "  instantdb logs --follow       # Follow log output\n";
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
    std::cout << CYAN << "Stopping InstantDB server..." << RESET << "\n";

    // Try to find and kill the instantdb_server process
    int result = system("pkill -f instantdb_server");

    if (result == 0) {
        std::cout << GREEN << "✅ Server stopped successfully" << RESET << "\n";
        return 0;
    } else {
        std::cerr << YELLOW << "No running InstantDB server found" << RESET << "\n";
        return 1;
    }
}

int handlePublishCommand(const std::vector<std::string>& args) {
    std::cout << "PUBLISH_DEBUG: Starting handlePublishCommand" << std::endl;
    std::string server_url = "http://localhost:9090";
    std::string project_path = ".";

    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        const auto& arg = args[i];
        if (arg == "--server" && i + 1 < args.size()) {
            server_url = args[++i];
        } else if (arg.substr(0, 9) == "--server=") {
            server_url = arg.substr(9);
        } else if (arg == "--path" && i + 1 < args.size()) {
            project_path = args[++i];
        } else if (arg.substr(0, 7) == "--path=") {
            project_path = arg.substr(7);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << BOLD << "Usage:" << RESET << " instantdb publish [OPTIONS]\n\n";
            std::cout << BOLD << "Options:\n" << RESET;
            std::cout << "  --server URL    InstantDB server URL (default: http://localhost:9090)\n";
            std::cout << "  --path PATH     Project path (default: current directory)\n";
            std::cout << "  -h, --help      Show this help message\n\n";
            std::cout << BOLD << "Examples:\n" << RESET;
            std::cout << "  instantdb publish\n";
            std::cout << "  instantdb publish --server=http://localhost:9090\n";
            std::cout << "  instantdb publish --path=./my-module --server=http://prod.example.com:9090\n";
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

    // Step 1: Build the WASM module
    std::cout << "📦 Building WASM module...\n";
    int build_result = system("dotnet publish --configuration Release --verbosity quiet");
    if (build_result != 0) {
        std::cerr << RED << "Error: Build failed. Make sure you have .NET 8+ and wasm-tools workload installed." << RESET << "\n";
        std::cerr << "Try: dotnet workload install wasm-tools\n";
        chdir(original_dir);
        return 1;
    }

    // Step 2: Find the WASM file
    std::string wasm_file;
    std::string project_name;

    // Get project name from .csproj file
    fs::path current_path = fs::current_path();
    for (const auto& entry : fs::directory_iterator(current_path)) {
        if (entry.path().extension() == ".csproj") {
            project_name = entry.path().stem().string();
            // Try multiple possible WASM file locations
            std::vector<std::string> wasm_paths = {
                "bin/Release/net8.0/browser-wasm/AppBundle/_framework/" + project_name + ".wasm",
                "bin/Release/net8.0/browser-wasm/publish/" + project_name + ".wasm",
                "bin/Release/net8.0/wasm-wasi/publish/" + project_name + ".wasm"
            };

            for (const auto& path : wasm_paths) {
                if (std::filesystem::exists(path)) {
                    wasm_file = path;
                    break;
                }
            }

            if (wasm_file.empty()) {
                // Default to the first path for error reporting
                wasm_file = wasm_paths[0];
            }
            break;
        }
    }

    if (project_name.empty()) {
        std::cerr << RED << "Error: No .csproj file found in current directory" << RESET << "\n";
        chdir(original_dir);
        return 1;
    }

    if (!fs::exists(wasm_file)) {
        std::cerr << RED << "Error: WASM file not found: " << wasm_file << RESET << "\n";
        std::cerr << "Build may have failed or output path is incorrect.\n";
        chdir(original_dir);
        return 1;
    }

    std::cout << GREEN << "✅ Build successful: " << wasm_file << RESET << "\n";

    // Step 3: Deploy via gRPC
    std::cout << "🌐 Deploying to server: " << server_url << "\n";

    // Convert http to grpc port (http://localhost:9090 -> localhost:50051)
    std::string grpc_endpoint = "localhost:50051";
    if (server_url.find("localhost") != std::string::npos) {
        grpc_endpoint = "localhost:50051";
    } else {
        // For production servers, assume gRPC is on port 50051
        size_t protocol_pos = server_url.find("://");
        if (protocol_pos != std::string::npos) {
            std::string host_part = server_url.substr(protocol_pos + 3);
            size_t port_pos = host_part.find(":");
            if (port_pos != std::string::npos) {
                grpc_endpoint = host_part.substr(0, port_pos) + ":50051";
            } else {
                grpc_endpoint = host_part + ":50051";
            }
        }
    }

    // Create gRPC deployment command - use installed proto file if available
    std::string proto_args;
    if (access("/usr/local/share/instantdb/instantdb.proto", F_OK) == 0) {
        // Use installed proto file
        proto_args = "-import-path /usr/local/share/instantdb -proto instantdb.proto ";
    } else if (access("instantdb.proto", F_OK) == 0) {
        // Use local proto file if present
        proto_args = "-import-path . -proto instantdb.proto ";
    } else {
        // Try to find proto file in common locations
        const char* home = getenv("HOME");
        std::string home_proto = std::string(home ? home : "") + "/.instantdb/instantdb.proto";
        if (access(home_proto.c_str(), F_OK) == 0) {
            proto_args = "-import-path " + std::string(home ? home : "") + "/.instantdb -proto instantdb.proto ";
        } else {
            std::cerr << RED << "Error: Could not find instantdb.proto file. Make sure InstantDB is properly installed." << RESET << "\n";
            std::cerr << "Expected locations:\n";
            std::cerr << "  - /usr/local/share/instantdb/instantdb.proto (installed)\n";
            std::cerr << "  - ./instantdb.proto (current directory)\n";
            std::cerr << "  - ~/.instantdb/instantdb.proto (home directory)\n";
            return 1;
        }
    }

    // Create JSON with base64-encoded WASM file
    std::cout << YELLOW << "Debug: Starting base64 encoding..." << RESET << "\n";
    std::cout.flush();

    // First, base64 encode the file content
    std::string base64_cmd = "base64 < " + wasm_file;
    std::cout << YELLOW << "Debug: About to run: " << base64_cmd << RESET << "\n";
    std::cout.flush();

    FILE* pipe = popen(base64_cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << RED << "Error: Failed to encode WASM file" << RESET << "\n";
        chdir(original_dir);
        return 1;
    }

    std::cout << YELLOW << "Debug: popen() succeeded, reading base64 data..." << RESET << "\n";
    std::cout.flush();

    std::string base64_content;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        base64_content += buffer;
    }
    pclose(pipe);

    std::cout << YELLOW << "Debug: Base64 encoding completed, length: " << base64_content.length() << RESET << "\n";
    std::cout.flush();

    // Remove newlines from base64 content
    base64_content.erase(std::remove(base64_content.begin(), base64_content.end(), '\n'), base64_content.end());
    base64_content.erase(std::remove(base64_content.begin(), base64_content.end(), '\r'), base64_content.end());

    // Create the JSON payload
    std::string json_payload = "{\"name\": \"" + project_name + "\", "
                               "\"version\": \"1.0.0\", "
                               "\"bytecode\": \"" + base64_content + "\"}";

    // Write JSON to temporary file to avoid shell escaping issues
    std::string temp_json = "/tmp/instantdb_deploy_" + std::to_string(getpid()) + ".json";
    std::ofstream json_file(temp_json);
    if (!json_file) {
        std::cerr << RED << "Error: Failed to create temporary JSON file" << RESET << "\n";
        chdir(original_dir);
        return 1;
    }
    json_file << json_payload;
    json_file.close();

    // Create gRPC command using the JSON file
    // grpcurl expects @ alone for stdin, so we'll cat the file
    std::string grpc_cmd = "cat " + temp_json + " | grpcurl -plaintext " + proto_args +
                          "-d @ " +
                          grpc_endpoint + " instantdb.grpc.WasmService.DeployModule";

    // Debug: Print the command we're about to run
    std::cout << YELLOW << "Debug: NEW Running command: " << grpc_cmd << RESET << "\n";
    std::cout << YELLOW << "Debug: About to call system()..." << RESET << "\n";
    std::cout.flush(); // Ensure output is visible immediately

    int deploy_result = system(grpc_cmd.c_str());

    std::cout << YELLOW << "Debug: system() returned: " << deploy_result << RESET << "\n";

    // Clean up temporary JSON file
    std::remove(temp_json.c_str());

    // Restore original directory
    chdir(original_dir);

    if (deploy_result == 0) {
        std::cout << GREEN << BOLD << "🎉 Module published successfully!" << RESET << "\n\n";
        std::cout << "Module Name: " << CYAN << project_name << RESET << "\n";
        std::cout << "Server: " << CYAN << server_url << RESET << "\n\n";
        std::cout << BOLD << "Test your reducers:" << RESET << "\n";
        std::cout << "grpcurl -plaintext -d '{\n";
        std::cout << "  \"module_name\": \"" << project_name << "\",\n";
        std::cout << "  \"reducer_name\": \"CreateUser\",\n";
        std::cout << "  \"sender_identity\": \"user123\",\n";
        std::cout << "  \"args\": [{\"string_value\": \"Alice\"}, {\"string_value\": \"alice@example.com\"}]\n";
        std::cout << "}' " << grpc_endpoint << " instantdb.grpc.WasmService.ExecuteReducer\n";
        return 0;
    } else {
        std::cerr << RED << "Error: Deployment failed. Make sure the InstantDB server is running and grpcurl is installed." << RESET << "\n";
        return 1;
    }
}

int executeCommand(const std::string& command, const std::vector<std::string>& args) {
    if (commands.find(command) == commands.end()) {
        std::cerr << RED << "Error: Unknown command '" << command << "'" << RESET << "\n";
        std::cerr << "Run 'instantdb --help' for usage information.\n";
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
        std::cerr << "Please ensure InstantDB is properly installed.\n";
        std::cerr << "Run 'instantdb --help' for more information.\n";
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