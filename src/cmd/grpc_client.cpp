#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include "origindb.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

class OriginDBClient {
public:
    // Attaches the bearer token to a context. Call credentials would be
    // cleaner but gRPC forbids them over an insecure channel (dev/LAN), so
    // set the metadata directly — works on both plaintext and TLS.
    void Authorize(ClientContext& ctx) const {
        if (!token_.empty()) ctx.AddMetadata("authorization", "Bearer " + token_);
    }
    void SetToken(std::string token) { token_ = std::move(token); }

    OriginDBClient(std::shared_ptr<Channel> channel)
        : stub_(origindb::grpc::SQLService::NewStub(channel)),
          wasm_stub_(origindb::grpc::WasmService::NewStub(channel)) {}

    std::string ExecuteSQL(const std::string& sql) {
        origindb::grpc::SQLRequest request;
        request.set_sql(sql);

        origindb::grpc::SQLResponse response;
        ClientContext context;
        Authorize(context);

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
        origindb::grpc::StatusRequest request;
        origindb::grpc::StatusResponse response;
        ClientContext context;
        Authorize(context);

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

    std::string DeployModule(const std::string& name, const std::string& wasm_path,
                             const std::string& version) {
        std::ifstream in(wasm_path, std::ios::binary);
        if (!in) return "❌ Cannot read file: " + wasm_path;
        std::string bytecode((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

        origindb::grpc::DeployModuleRequest request;
        request.set_name(name);
        request.set_bytecode(bytecode);
        request.set_version(version);

        origindb::grpc::DeployModuleResponse response;
        ClientContext context;
        Authorize(context);
        Status status = wasm_stub_->DeployModule(&context, request, &response);

        if (!status.ok()) return "❌ gRPC call failed: " + status.error_message();
        if (!response.success()) return "❌ Deploy failed: " + response.error();
        return "✅ Deployed module '" + name + "' (" +
               std::to_string(bytecode.size()) + " bytes)";
    }

    std::string UndeployModule(const std::string& name) {
        origindb::grpc::UndeployModuleRequest request;
        request.set_name(name);
        origindb::grpc::UndeployModuleResponse response;
        ClientContext context;
        Authorize(context);
        Status status = wasm_stub_->UndeployModule(&context, request, &response);

        if (!status.ok()) return "❌ gRPC call failed: " + status.error_message();
        if (!response.success()) return "❌ Undeploy failed: " + response.error();
        return "✅ Undeployed module '" + name + "'";
    }

    std::string ListModules() {
        origindb::grpc::ListModulesRequest request;
        origindb::grpc::ListModulesResponse response;
        ClientContext context;
        Authorize(context);
        Status status = wasm_stub_->ListModules(&context, request, &response);

        if (!status.ok()) return "❌ gRPC call failed: " + status.error_message();
        if (response.modules_size() == 0) return "No modules deployed";

        std::string result = "📦 Deployed modules:\n";
        for (const auto& module : response.modules()) {
            result += "  " + module.name();
            if (!module.version().empty()) result += " v" + module.version();
            if (!module.sha256().empty())
                result += "  sha256:" + module.sha256().substr(0, 12);
            result += "\n";
        }
        return result;
    }

    std::string CallReducer(const std::string& module, const std::string& reducer,
                            const std::string& args_json) {
        origindb::grpc::ExecuteReducerRequest request;
        request.set_module_name(module);
        request.set_reducer_name(reducer);
        request.set_sender_identity("origindb_client");

        if (!args_json.empty()) {
            auto args = nlohmann::json::parse(args_json, nullptr, false);
            if (args.is_discarded() || !args.is_array())
                return "❌ Arguments must be a JSON array, e.g. '[\"Alice\", 42]'";
            for (const auto& arg : args) {
                auto* value = request.add_args();
                if (arg.is_boolean()) value->set_bool_value(arg.get<bool>());
                else if (arg.is_number_integer()) value->set_int64_value(arg.get<int64_t>());
                else if (arg.is_number_float()) value->set_double_value(arg.get<double>());
                else if (arg.is_string()) value->set_string_value(arg.get<std::string>());
                else value->set_string_value(arg.dump());
            }
        }

        origindb::grpc::ExecuteReducerResponse response;
        ClientContext context;
        Authorize(context);
        Status status = wasm_stub_->ExecuteReducer(&context, request, &response);

        if (!status.ok()) return "❌ gRPC call failed: " + status.error_message();
        if (!response.success()) return "❌ Reducer failed: " + response.error();

        std::string result = "✅ " + module + "." + reducer + " (" +
                             std::to_string(response.execution_time_micros()) + " μs)";
        for (const auto& value : response.results()) {
            result += "\n  → ";
            if (value.has_int64_value()) result += std::to_string(value.int64_value());
            else if (value.has_string_value()) result += value.string_value();
            else if (value.has_double_value()) result += std::to_string(value.double_value());
            else if (value.has_bool_value()) result += value.bool_value() ? "true" : "false";
            else if (value.has_bytes_value()) result += value.bytes_value();
            else result += "null";
        }
        return result;
    }

private:
    std::unique_ptr<origindb::grpc::SQLService::Stub> stub_;
    std::unique_ptr<origindb::grpc::WasmService::Stub> wasm_stub_;
    std::string token_;
};

void PrintUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] [COMMAND]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -s, --server ADDRESS     Server address (default: localhost:50051)\n";
    std::cout << "  -t, --token TOKEN        Auth token (or ORIGINDB_TOKEN env var)\n";
    std::cout << "  --tls                    Use TLS transport\n";
    std::cout << "  --tls-ca FILE            CA/self-signed cert to trust (implies --tls)\n";
    std::cout << "  -h, --help               Show this help message\n\n";
    std::cout << "Commands:\n";
    std::cout << "  status                             Get server status\n";
    std::cout << "  exec \"SQL\"                         Execute SQL statement\n";
    std::cout << "  interactive                        Start interactive mode\n";
    std::cout << "  deploy NAME FILE.wasm [VERSION]    Deploy a WASM module\n";
    std::cout << "  undeploy NAME                      Remove a WASM module\n";
    std::cout << "  modules                            List deployed WASM modules\n";
    std::cout << "  call MODULE REDUCER [JSON_ARGS]    Execute a reducer\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << " status\n";
    std::cout << "  " << program_name << " exec \"SELECT * FROM users\"\n";
    std::cout << "  " << program_name << " deploy user_service ./module.wasm 1.0.0\n";
    std::cout << "  " << program_name << " call user_service CreateUser '[\"Alice\", \"alice@example.com\"]'\n";
}

int main(int argc, char* argv[]) {
    std::string server_address = "localhost:50051";
    std::string token;
    std::string tls_ca;
    bool use_tls = false;
    std::string command;
    std::vector<std::string> args;

    if (const char* t = std::getenv("ORIGINDB_TOKEN")) token = t;

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
        } else if (arg == "-t" || arg == "--token") {
            if (i + 1 < argc) token = argv[++i];
            else { std::cerr << "Error: " << arg << " requires a token\n"; return 1; }
        } else if (arg == "--tls") {
            use_tls = true;
        } else if (arg == "--tls-ca") {
            if (i + 1 < argc) { tls_ca = argv[++i]; use_tls = true; }
            else { std::cerr << "Error: --tls-ca requires a path\n"; return 1; }
        } else if (command.empty()) {
            command = arg;
        } else {
            args.push_back(arg);
        }
    }
    std::string sql = args.empty() ? "" : args[0];

    if (command.empty()) {
        std::cerr << "Error: No command specified\n";
        PrintUsage(argv[0]);
        return 1;
    }

    try {
        // Channel credentials: TLS transport (optional) + bearer-token call creds.
        std::shared_ptr<grpc::ChannelCredentials> channel_creds;
        if (use_tls) {
            grpc::SslCredentialsOptions ssl_opts;
            if (!tls_ca.empty()) {
                std::ifstream in(tls_ca);
                std::stringstream ss; ss << in.rdbuf();
                ssl_opts.pem_root_certs = ss.str();
            }
            channel_creds = grpc::SslCredentials(ssl_opts);
        } else {
            channel_creds = grpc::InsecureChannelCredentials();
        }
        auto channel = grpc::CreateChannel(server_address, channel_creds);
        OriginDBClient client(channel);
        client.SetToken(token);

        if (command == "status") {
            std::cout << client.GetStatus() << std::endl;
        } else if (command == "exec") {
            if (sql.empty()) {
                std::cerr << "Error: exec command requires SQL statement\n";
                return 1;
            }
            std::cout << client.ExecuteSQL(sql) << std::endl;
        } else if (command == "interactive") {
            std::cout << "OriginDB Interactive SQL Client\n";
            std::cout << "Connected to: " << server_address << "\n";
            std::cout << "Type 'exit' or 'quit' to exit\n\n";

            std::string line;
            while (true) {
                std::cout << "origindb> ";
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
        } else if (command == "deploy") {
            if (args.size() < 2) {
                std::cerr << "Error: deploy requires NAME and FILE.wasm\n";
                return 1;
            }
            std::string result = client.DeployModule(
                args[0], args[1], args.size() > 2 ? args[2] : "");
            std::cout << result << std::endl;
            return result.rfind("✅", 0) == 0 ? 0 : 1;
        } else if (command == "undeploy") {
            if (args.empty()) {
                std::cerr << "Error: undeploy requires NAME\n";
                return 1;
            }
            std::string result = client.UndeployModule(args[0]);
            std::cout << result << std::endl;
            return result.rfind("✅", 0) == 0 ? 0 : 1;
        } else if (command == "modules") {
            std::cout << client.ListModules() << std::endl;
        } else if (command == "call") {
            if (args.size() < 2) {
                std::cerr << "Error: call requires MODULE and REDUCER\n";
                return 1;
            }
            std::string result = client.CallReducer(
                args[0], args[1], args.size() > 2 ? args[2] : "");
            std::cout << result << std::endl;
            return result.rfind("✅", 0) == 0 ? 0 : 1;
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