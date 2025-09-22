#include "cli/commands/server_command.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <spdlog/spdlog.h>

namespace instantdb::cli {

int ServerCommand::Execute(const std::vector<std::string>& args) {
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }

    auto positional = GetPositionalArgs(args);
    std::string action = positional.empty() ? "start" : positional[0];

    if (action == "start") {
        return StartServer(args);
    } else if (action == "stop") {
        return StopServer(args);
    } else if (action == "restart") {
        return RestartServer(args);
    } else if (action == "status") {
        return ServerStatus(args);
    } else {
        spdlog::error("Unknown server action: {}", action);
        PrintHelp();
        return 1;
    }
}

void ServerCommand::PrintHelp() const {
    std::cout << "Usage: instantdb server [action] [options]\n\n";
    std::cout << "Manage the InstantDB server\n\n";
    std::cout << "Actions:\n";
    std::cout << "  start                     Start the server (default)\n";
    std::cout << "  stop                      Stop the server\n";
    std::cout << "  restart                   Restart the server\n";
    std::cout << "  status                    Show server status\n\n";
    std::cout << "Options:\n";
    std::cout << "  --port, -p <port>         Server port (default: 8080)\n";
    std::cout << "  --websocket-port <port>   WebSocket port (default: 8085)\n";
    std::cout << "  --grpc-port <port>        gRPC port (default: 50051)\n";
    std::cout << "  --config, -c <file>       Config file path\n";
    std::cout << "  --data-dir <dir>          Data directory path\n";
    std::cout << "  --log-level, -l <level>   Log level (debug/info/warn/error)\n";
    std::cout << "  --daemon, -d              Run as daemon\n";
    std::cout << "  --help, -h                Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  instantdb server start\n";
    std::cout << "  instantdb server start --port 9090 --daemon\n";
    std::cout << "  instantdb server stop\n";
    std::cout << "  instantdb server status\n";
}

int ServerCommand::StartServer(const std::vector<std::string>& args) {
    if (IsServerRunning()) {
        spdlog::info("Server is already running (PID: {})", GetServerPid());
        return 0;
    }

    // Find the server binary
    std::string server_binary;
    if (std::filesystem::exists("./instantdb_server")) {
        server_binary = "./instantdb_server";
    } else if (std::filesystem::exists("./build/instantdb_server")) {
        server_binary = "./build/instantdb_server";
    } else {
        spdlog::error("InstantDB server binary not found. Please build the project first.");
        spdlog::info("Run: cmake --build build --target instantdb_server");
        return 1;
    }

    std::vector<std::string> server_args = BuildServerArgs(args);
    bool daemon = HasFlag(args, "--daemon") || HasFlag(args, "-d");

    spdlog::info("Starting InstantDB server...");

    if (daemon) {
        // Fork to run as daemon
        pid_t pid = fork();
        if (pid < 0) {
            spdlog::error("Failed to fork daemon process");
            return 1;
        } else if (pid == 0) {
            // Child process - become daemon
            setsid();

            // Redirect stdout/stderr to log file
            std::string log_dir = "./logs";
            std::filesystem::create_directories(log_dir);

            freopen((log_dir + "/server.out").c_str(), "w", stdout);
            freopen((log_dir + "/server.err").c_str(), "w", stderr);

            // Build exec args
            std::vector<char*> exec_args;
            exec_args.push_back(const_cast<char*>(server_binary.c_str()));
            for (const auto& arg : server_args) {
                exec_args.push_back(const_cast<char*>(arg.c_str()));
            }
            exec_args.push_back(nullptr);

            execv(server_binary.c_str(), exec_args.data());
            spdlog::error("Failed to exec server binary");
            exit(1);
        } else {
            // Parent process - save PID and return
            SaveServerPid(pid);
            spdlog::info("Server started as daemon (PID: {})", pid);
            spdlog::info("Logs: ./logs/server.out, ./logs/server.err");
            return 0;
        }
    } else {
        // Run in foreground
        std::vector<char*> exec_args;
        exec_args.push_back(const_cast<char*>(server_binary.c_str()));
        for (const auto& arg : server_args) {
            exec_args.push_back(const_cast<char*>(arg.c_str()));
        }
        exec_args.push_back(nullptr);

        execv(server_binary.c_str(), exec_args.data());
        spdlog::error("Failed to exec server binary");
        return 1;
    }
}

int ServerCommand::StopServer(const std::vector<std::string>& args) {
    if (!IsServerRunning()) {
        spdlog::info("Server is not running");
        return 0;
    }

    int pid = GetServerPid();
    spdlog::info("Stopping server (PID: {})...", pid);

    // Send SIGTERM first
    if (kill(pid, SIGTERM) != 0) {
        spdlog::error("Failed to send SIGTERM to server process");
        return 1;
    }

    // Wait for graceful shutdown
    int attempts = 0;
    while (attempts < 30) { // Wait up to 30 seconds
        if (!IsServerRunning()) {
            spdlog::info("Server stopped gracefully");
            RemoveServerPid();
            return 0;
        }
        sleep(1);
        attempts++;
    }

    // Force kill if still running
    spdlog::warn("Server did not stop gracefully, forcing shutdown...");
    if (kill(pid, SIGKILL) != 0) {
        spdlog::error("Failed to force kill server process");
        return 1;
    }

    RemoveServerPid();
    spdlog::info("Server stopped forcefully");
    return 0;
}

int ServerCommand::RestartServer(const std::vector<std::string>& args) {
    spdlog::info("Restarting server...");
    StopServer(args);
    sleep(1); // Brief pause between stop and start
    return StartServer(args);
}

int ServerCommand::ServerStatus(const std::vector<std::string>& args) {
    if (IsServerRunning()) {
        int pid = GetServerPid();
        std::cout << "Server Status: RUNNING\n";
        std::cout << "PID: " << pid << "\n";

        // Try to get additional info from /proc if available
        std::string proc_path = "/proc/" + std::to_string(pid) + "/stat";
        if (std::filesystem::exists(proc_path)) {
            std::ifstream stat_file(proc_path);
            std::string line;
            if (std::getline(stat_file, line)) {
                // Parse basic info from /proc/pid/stat
                std::cout << "Process Info: Available in " << proc_path << "\n";
            }
        }

        return 0;
    } else {
        std::cout << "Server Status: STOPPED\n";
        return 1;
    }
}

bool ServerCommand::IsServerRunning() const {
    int pid = GetServerPid();
    if (pid <= 0) {
        return false;
    }

    // Check if process exists
    return kill(pid, 0) == 0;
}

int ServerCommand::GetServerPid() const {
    std::string pid_file = GetPidFilePath();
    std::ifstream file(pid_file);
    if (!file.is_open()) {
        return -1;
    }

    int pid;
    file >> pid;
    return pid;
}

void ServerCommand::SaveServerPid(int pid) const {
    std::string pid_file = GetPidFilePath();
    std::filesystem::create_directories(std::filesystem::path(pid_file).parent_path());

    std::ofstream file(pid_file);
    file << pid;
}

void ServerCommand::RemoveServerPid() const {
    std::string pid_file = GetPidFilePath();
    std::filesystem::remove(pid_file);
}

std::string ServerCommand::GetPidFilePath() const {
    return "./logs/instantdb_server.pid";
}

std::string ServerCommand::GetConfigPath(const std::vector<std::string>& args) const {
    std::string config = GetOption(args, "--config");
    if (config.empty()) {
        config = GetOption(args, "-c");
    }
    if (config.empty()) {
        // Try default locations
        if (std::filesystem::exists("./config/instantdb.toml")) {
            config = "./config/instantdb.toml";
        } else if (std::filesystem::exists("./instantdb.toml")) {
            config = "./instantdb.toml";
        }
    }
    return config;
}

std::vector<std::string> ServerCommand::BuildServerArgs(const std::vector<std::string>& args) const {
    std::vector<std::string> server_args;

    // Port options
    std::string port = GetOption(args, "--port");
    if (port.empty()) {
        port = GetOption(args, "-p");
    }
    if (!port.empty()) {
        server_args.push_back("--port");
        server_args.push_back(port);
    }

    std::string ws_port = GetOption(args, "--websocket-port");
    if (!ws_port.empty()) {
        server_args.push_back("--websocket-port");
        server_args.push_back(ws_port);
    }

    std::string grpc_port = GetOption(args, "--grpc-port");
    if (!grpc_port.empty()) {
        server_args.push_back("--grpc-port");
        server_args.push_back(grpc_port);
    }

    // Config file
    std::string config = GetConfigPath(args);
    if (!config.empty()) {
        server_args.push_back("--config");
        server_args.push_back(config);
    }

    // Data directory
    std::string data_dir = GetOption(args, "--data-dir");
    if (!data_dir.empty()) {
        server_args.push_back("--data-dir");
        server_args.push_back(data_dir);
    }

    // Log level
    std::string log_level = GetOption(args, "--log-level");
    if (log_level.empty()) {
        log_level = GetOption(args, "-l");
    }
    if (!log_level.empty()) {
        server_args.push_back("--log-level");
        server_args.push_back(log_level);
    }

    return server_args;
}

} // namespace instantdb::cli