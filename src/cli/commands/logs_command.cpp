#include "cli/commands/logs_command.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <deque>
#include <spdlog/spdlog.h>

namespace instantdb::cli {

int LogsCommand::Execute(const std::vector<std::string>& args) {
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }

    std::string log_file = GetLogFile(args);
    bool follow = HasFlag(args, "--follow") || HasFlag(args, "-f");

    int lines = 50; // Default
    std::string lines_str = GetOption(args, "--lines");
    if (lines_str.empty()) {
        lines_str = GetOption(args, "-n");
    }
    if (!lines_str.empty()) {
        try {
            lines = std::stoi(lines_str);
        } catch (const std::exception&) {
            spdlog::error("Invalid number of lines: {}", lines_str);
            return 1;
        }
    }

    return TailLogs(log_file, lines, follow);
}

void LogsCommand::PrintHelp() const {
    std::cout << "Usage: instantdb logs [options]\n\n";
    std::cout << "View InstantDB server logs\n\n";
    std::cout << "Options:\n";
    std::cout << "  --file, -f <file>         Log file to view (default: auto-detect)\n";
    std::cout << "  --lines, -n <number>      Number of lines to show (default: 50)\n";
    std::cout << "  --follow, -f              Follow log output (like tail -f)\n";
    std::cout << "  --stderr                  Show stderr logs instead of stdout\n";
    std::cout << "  --help, -h                Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  instantdb logs\n";
    std::cout << "  instantdb logs --lines 100\n";
    std::cout << "  instantdb logs --follow\n";
    std::cout << "  instantdb logs --stderr\n";
}

int LogsCommand::TailLogs(const std::string& log_file, int lines, bool follow) {
    if (!std::filesystem::exists(log_file)) {
        spdlog::error("Log file not found: {}", log_file);
        spdlog::info("Make sure the server is running or has been run before");
        return 1;
    }

    std::ifstream file(log_file);
    if (!file.is_open()) {
        spdlog::error("Cannot open log file: {}", log_file);
        return 1;
    }

    // Read the last N lines
    std::deque<std::string> tail_lines;
    std::string line;

    while (std::getline(file, line)) {
        tail_lines.push_back(line);
        if (tail_lines.size() > static_cast<size_t>(lines)) {
            tail_lines.pop_front();
        }
    }

    // Print the tail
    for (const auto& log_line : tail_lines) {
        std::cout << log_line << "\n";
    }

    if (!follow) {
        return 0;
    }

    // Follow mode - watch for new lines
    spdlog::info("Following log file... (Press Ctrl+C to exit)");

    file.clear(); // Clear EOF flag
    auto last_pos = file.tellg();

    while (true) {
        file.seekg(last_pos);

        while (std::getline(file, line)) {
            std::cout << line << "\n";
            std::cout.flush();
        }

        if (!file.eof()) {
            spdlog::error("Error reading log file");
            return 1;
        }

        file.clear(); // Clear EOF flag
        last_pos = file.tellg();

        // Sleep briefly before checking again
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}

std::string LogsCommand::GetLogFile(const std::vector<std::string>& args) const {
    std::string file = GetOption(args, "--file");
    if (!file.empty()) {
        return file;
    }

    // Auto-detect log file
    bool stderr_logs = HasFlag(args, "--stderr");

    if (stderr_logs) {
        if (std::filesystem::exists("./logs/server.err")) {
            return "./logs/server.err";
        } else if (std::filesystem::exists("./logs/instantdb.err")) {
            return "./logs/instantdb.err";
        }
    } else {
        if (std::filesystem::exists("./logs/server.out")) {
            return "./logs/server.out";
        } else if (std::filesystem::exists("./logs/instantdb.log")) {
            return "./logs/instantdb.log";
        } else if (std::filesystem::exists("./logs/instantdb.out")) {
            return "./logs/instantdb.out";
        }
    }

    // Fallback to default
    return stderr_logs ? "./logs/server.err" : "./logs/server.out";
}

} // namespace instantdb::cli