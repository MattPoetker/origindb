#include "cli/commands/exec_command.h"
#include <iostream>
#include <fstream>
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <filesystem>

namespace instantdb::cli {

int ExecCommand::Execute(const std::vector<std::string>& args) {
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }

    auto positional = GetPositionalArgs(args);
    if (positional.empty()) {
        spdlog::error("Reducer name or SQL query is required");
        PrintHelp();
        return 1;
    }

    bool is_sql = HasFlag(args, "--sql");

    if (is_sql) {
        return ExecuteSQL(args);
    } else {
        return ExecuteReducer(args);
    }
}

void ExecCommand::PrintHelp() const {
    std::cout << "Usage: instantdb exec [options] <reducer-name|sql-query>\n\n";
    std::cout << "Execute reducer functions or SQL queries\n\n";
    std::cout << "Options:\n";
    std::cout << "  --server, -s <url>        Server URL (default: localhost:50051)\n";
    std::cout << "  --sql                     Execute as SQL query instead of reducer\n";
    std::cout << "  --input, -i <data>        Input data for reducer (JSON format)\n";
    std::cout << "  --file, -f <file>         Read input data from file\n";
    std::cout << "  --help, -h                Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  instantdb exec create_user --input '{\"name\":\"John\",\"email\":\"john@example.com\"}'\n";
    std::cout << "  instantdb exec --sql \"SELECT * FROM users LIMIT 10\"\n";
    std::cout << "  instantdb exec process_order --file order_data.json\n";
}

int ExecCommand::ExecuteReducer(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    std::string reducer_name = positional[0];
    std::string server_url = GetServerUrl(args);

    // Get input data
    std::string input_data;
    std::string input_option = GetOption(args, "--input");
    if (input_option.empty()) {
        input_option = GetOption(args, "-i");
    }

    std::string file_option = GetOption(args, "--file");
    if (file_option.empty()) {
        file_option = GetOption(args, "-f");
    }

    if (!input_option.empty()) {
        input_data = input_option;
    } else if (!file_option.empty()) {
        std::ifstream file(file_option);
        if (!file.is_open()) {
            spdlog::error("Cannot open input file: {}", file_option);
            return 1;
        }
        std::string line;
        while (std::getline(file, line)) {
            input_data += line + "\n";
        }
    } else {
        input_data = "{}"; // Default empty JSON
    }

    spdlog::info("Executing reducer '{}' with input: {}", reducer_name, input_data);

    // For now, use a simple command - in real implementation, use proper gRPC calls
    std::string command = "EXECUTE REDUCER " + reducer_name + " WITH '" + input_data + "'";

    if (ExecuteGrpcCommand(command, server_url)) {
        return 0;
    } else {
        spdlog::error("Failed to execute reducer '{}'", reducer_name);
        return 1;
    }
}

int ExecCommand::ExecuteSQL(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    std::string sql_query = positional[0];
    std::string server_url = GetServerUrl(args);

    spdlog::info("Executing SQL query: {}", sql_query);

    if (ExecuteGrpcCommand(sql_query, server_url)) {
        return 0;
    } else {
        spdlog::error("Failed to execute SQL query");
        return 1;
    }
}

std::string ExecCommand::GetServerUrl(const std::vector<std::string>& args) const {
    std::string server = GetOption(args, "--server");
    if (server.empty()) {
        server = GetOption(args, "-s");
    }
    if (server.empty()) {
        server = "localhost:50051"; // Default gRPC endpoint
    }
    return server;
}

bool ExecCommand::ExecuteGrpcCommand(const std::string& command, const std::string& server_url) {
    // Use instantdb_client if available
    std::string client_path;
    if (std::filesystem::exists("./instantdb_client")) {
        client_path = "./instantdb_client";
    } else if (std::filesystem::exists("./build/instantdb_client")) {
        client_path = "./build/instantdb_client";
    } else {
        spdlog::error("InstantDB client not found. Please build the project first.");
        return false;
    }

    std::string full_command = client_path + " --server " + server_url + " --query \"" + command + "\"";
    spdlog::debug("Executing: {}", full_command);

    int result = system(full_command.c_str());
    return result == 0;
}

} // namespace instantdb::cli