#include <iostream>
#include <memory>
#include <string>
#include <grpcpp/grpcpp.h>
#include "instantdb.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

class InstantDBClient {
public:
    InstantDBClient(std::shared_ptr<Channel> channel)
        : stub_(instantdb::grpc::SQLService::NewStub(channel)) {}

    std::string ExecuteSQL(const std::string& sql) {
        instantdb::grpc::SQLRequest request;
        request.set_sql(sql);

        instantdb::grpc::SQLResponse response;
        ClientContext context;

        Status status = stub_->Execute(&context, request, &response);

        if (status.ok()) {
            if (response.success()) {
                std::string result = "✅ SQL executed successfully\n";
                result += "Execution time: " + std::to_string(response.execution_time_micros()) + " μs\n";
                result += "Rows affected: " + std::to_string(response.rows_affected()) + "\n";

                if (response.rows_size() > 0) {
                    result += "\nResults:\n";
                    for (const auto& row : response.rows()) {
                        result += "  Key: " + row.key();
                        for (const auto& [col_name, col_value] : row.columns()) {
                            result += ", " + col_name + ": ";
                            if (col_value.has_int64_value()) {
                                result += std::to_string(col_value.int64_value());
                            } else if (col_value.has_string_value()) {
                                result += "\"" + col_value.string_value() + "\"";
                            } else if (col_value.has_double_value()) {
                                result += std::to_string(col_value.double_value());
                            } else if (col_value.has_bool_value()) {
                                result += col_value.bool_value() ? "true" : "false";
                            } else if (col_value.has_bytes_value()) {
                                result += "[binary data]";
                            } else {
                                result += "NULL";
                            }
                        }
                        result += "\n";
                    }
                }
                return result;
            } else {
                return "❌ SQL execution failed: " + response.error();
            }
        } else {
            return "❌ gRPC call failed: " + status.error_message();
        }
    }

    std::string GetStatus() {
        instantdb::grpc::StatusRequest request;
        instantdb::grpc::StatusResponse response;
        ClientContext context;

        Status status = stub_->GetStatus(&context, request, &response);

        if (status.ok()) {
            std::string result = "📊 Server Status\n";
            result += "================\n";
            result += "Version: " + response.version() + "\n";
            result += "Uptime: " + std::to_string(response.uptime_seconds()) + " seconds\n\n";

            const auto& stats = response.stats();
            result += "Storage Statistics:\n";
            result += "  Tables: " + std::to_string(stats.total_tables()) + "\n";
            result += "  Rows: " + std::to_string(stats.total_rows()) + "\n";
            result += "  Storage bytes: " + std::to_string(stats.storage_bytes()) + "\n\n";

            result += "Network Statistics:\n";
            result += "  Active WebSocket connections: " + std::to_string(stats.active_connections()) + "\n";
            result += "  Active changefeed subscriptions: " + std::to_string(stats.active_subscriptions()) + "\n";
            result += "  Events published: " + std::to_string(stats.events_published()) + "\n\n";

            result += "gRPC Statistics:\n";
            result += "  Total queries: " + std::to_string(stats.total_queries()) + "\n";
            result += "  Successful queries: " + std::to_string(stats.successful_queries()) + "\n";
            result += "  Failed queries: " + std::to_string(stats.failed_queries()) + "\n";

            return result;
        } else {
            return "❌ gRPC call failed: " + status.error_message();
        }
    }

private:
    std::unique_ptr<instantdb::grpc::SQLService::Stub> stub_;
};

void PrintUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] [COMMAND]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -s, --server ADDRESS     Server address (default: localhost:50051)\n";
    std::cout << "  -h, --help               Show this help message\n\n";
    std::cout << "Commands:\n";
    std::cout << "  status                   Get server status\n";
    std::cout << "  exec \"SQL\"               Execute SQL statement\n";
    std::cout << "  interactive              Start interactive mode\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << " status\n";
    std::cout << "  " << program_name << " exec \"CREATE TABLE users (id INT64 PRIMARY KEY, name STRING)\"\n";
    std::cout << "  " << program_name << " exec \"INSERT INTO users VALUES (1, 'Alice')\"\n";
    std::cout << "  " << program_name << " exec \"SELECT * FROM users\"\n";
    std::cout << "  " << program_name << " interactive\n";
}

int main(int argc, char* argv[]) {
    std::string server_address = "localhost:50051";
    std::string command;
    std::string sql;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "-s" || arg == "--server") {
            if (i + 1 < argc) {
                server_address = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a server address\n";
                return 1;
            }
        } else if (command.empty()) {
            command = arg;
        } else if (command == "exec" && sql.empty()) {
            sql = arg;
        } else {
            std::cerr << "Error: Unknown argument " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (command.empty()) {
        std::cerr << "Error: No command specified\n";
        PrintUsage(argv[0]);
        return 1;
    }

    try {
        // Create gRPC channel
        auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
        InstantDBClient client(channel);

        if (command == "status") {
            std::cout << client.GetStatus() << std::endl;
        } else if (command == "exec") {
            if (sql.empty()) {
                std::cerr << "Error: exec command requires SQL statement\n";
                return 1;
            }
            std::cout << client.ExecuteSQL(sql) << std::endl;
        } else if (command == "interactive") {
            std::cout << "InstantDB Interactive SQL Client\n";
            std::cout << "Connected to: " << server_address << "\n";
            std::cout << "Type 'exit' or 'quit' to exit\n\n";

            std::string line;
            while (true) {
                std::cout << "instantdb> ";
                if (!std::getline(std::cin, line)) {
                    break;
                }

                if (line == "exit" || line == "quit") {
                    break;
                }

                if (line == "status") {
                    std::cout << client.GetStatus() << std::endl;
                } else if (!line.empty()) {
                    std::cout << client.ExecuteSQL(line) << std::endl;
                }
            }
        } else {
            std::cerr << "Error: Unknown command " << command << "\n";
            PrintUsage(argv[0]);
            return 1;
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}