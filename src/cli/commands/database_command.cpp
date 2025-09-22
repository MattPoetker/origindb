#include "cli/commands/database_command.h"
#include <iostream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace instantdb::cli {

int DatabaseCommand::Execute(const std::vector<std::string>& args) {
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }

    auto positional = GetPositionalArgs(args);
    if (positional.empty()) {
        spdlog::error("Database action is required");
        PrintHelp();
        return 1;
    }

    std::string action = positional[0];

    if (action == "create") {
        return CreateDatabase(args);
    } else if (action == "list" || action == "ls") {
        return ListDatabases(args);
    } else if (action == "drop" || action == "rm") {
        return DropDatabase(args);
    } else if (action == "backup") {
        return BackupDatabase(args);
    } else if (action == "restore") {
        return RestoreDatabase(args);
    } else {
        spdlog::error("Unknown database action: {}", action);
        PrintHelp();
        return 1;
    }
}

void DatabaseCommand::PrintHelp() const {
    std::cout << "Usage: instantdb database <action> [options]\n\n";
    std::cout << "Manage InstantDB databases\n\n";
    std::cout << "Actions:\n";
    std::cout << "  create <name>             Create a new database\n";
    std::cout << "  list, ls                  List all databases\n";
    std::cout << "  drop, rm <name>           Drop a database\n";
    std::cout << "  backup <name> <file>      Backup database to file\n";
    std::cout << "  restore <name> <file>     Restore database from file\n\n";
    std::cout << "Options:\n";
    std::cout << "  --server, -s <url>        Server URL (default: localhost:50051)\n";
    std::cout << "  --isolated                Create isolated database\n";
    std::cout << "  --help, -h                Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  instantdb database create myapp\n";
    std::cout << "  instantdb database create --isolated testdb\n";
    std::cout << "  instantdb database list\n";
    std::cout << "  instantdb database backup myapp myapp_backup.db\n";
}

int DatabaseCommand::CreateDatabase(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    if (positional.size() < 2) {
        spdlog::error("Database name is required for create");
        return 1;
    }

    std::string db_name = positional[1];
    bool isolated = HasFlag(args, "--isolated");
    std::string server_url = GetServerUrl(args);

    spdlog::info("Creating database '{}'...", db_name);

    // Use instantdb_client or direct gRPC calls
    std::string command;
    if (isolated) {
        command = "CREATE ISOLATED DATABASE " + db_name;
    } else {
        command = "CREATE DATABASE " + db_name;
    }

    if (ExecuteGrpcCommand(command, server_url)) {
        spdlog::info("Database '{}' created successfully", db_name);
        return 0;
    } else {
        spdlog::error("Failed to create database '{}'", db_name);
        return 1;
    }
}

int DatabaseCommand::ListDatabases(const std::vector<std::string>& args) {
    std::string server_url = GetServerUrl(args);

    spdlog::info("Listing databases...");

    if (ExecuteGrpcCommand("SHOW DATABASES", server_url)) {
        return 0;
    } else {
        spdlog::error("Failed to list databases");
        return 1;
    }
}

int DatabaseCommand::DropDatabase(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    if (positional.size() < 2) {
        spdlog::error("Database name is required for drop");
        return 1;
    }

    std::string db_name = positional[1];
    std::string server_url = GetServerUrl(args);

    spdlog::warn("Dropping database '{}'... This action cannot be undone!", db_name);

    std::string command = "DROP DATABASE " + db_name;

    if (ExecuteGrpcCommand(command, server_url)) {
        spdlog::info("Database '{}' dropped successfully", db_name);
        return 0;
    } else {
        spdlog::error("Failed to drop database '{}'", db_name);
        return 1;
    }
}

int DatabaseCommand::BackupDatabase(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    if (positional.size() < 3) {
        spdlog::error("Database name and backup file are required");
        return 1;
    }

    std::string db_name = positional[1];
    std::string backup_file = positional[2];
    std::string server_url = GetServerUrl(args);

    spdlog::info("Backing up database '{}' to '{}'...", db_name, backup_file);

    std::string command = "BACKUP DATABASE " + db_name + " TO '" + backup_file + "'";

    if (ExecuteGrpcCommand(command, server_url)) {
        spdlog::info("Database '{}' backed up successfully", db_name);
        return 0;
    } else {
        spdlog::error("Failed to backup database '{}'", db_name);
        return 1;
    }
}

int DatabaseCommand::RestoreDatabase(const std::vector<std::string>& args) {
    auto positional = GetPositionalArgs(args);
    if (positional.size() < 3) {
        spdlog::error("Database name and backup file are required");
        return 1;
    }

    std::string db_name = positional[1];
    std::string backup_file = positional[2];
    std::string server_url = GetServerUrl(args);

    spdlog::info("Restoring database '{}' from '{}'...", db_name, backup_file);

    std::string command = "RESTORE DATABASE " + db_name + " FROM '" + backup_file + "'";

    if (ExecuteGrpcCommand(command, server_url)) {
        spdlog::info("Database '{}' restored successfully", db_name);
        return 0;
    } else {
        spdlog::error("Failed to restore database '{}'", db_name);
        return 1;
    }
}

std::string DatabaseCommand::GetServerUrl(const std::vector<std::string>& args) const {
    std::string server = GetOption(args, "--server");
    if (server.empty()) {
        server = GetOption(args, "-s");
    }
    if (server.empty()) {
        server = "localhost:50051"; // Default gRPC endpoint
    }
    return server;
}

bool DatabaseCommand::ExecuteGrpcCommand(const std::string& command, const std::string& server_url) {
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