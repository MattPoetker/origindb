#include "../storage/storage_perf_test.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>

using namespace instantdb::performance;

void PrintUsage() {
    std::cout << R"(
Test Data Generator Utility

Usage: test_data_generator [OPTIONS]

Options:
  --count <N>          Number of records to generate (default: 10000)
  --output <FILE>      Output JSON file (default: test_data.json)
  --format <FORMAT>    Output format: json, csv, sql (default: json)
  --table <NAME>       Table name for SQL format (default: test_table)
  --size <SIZE>        Data size: small, medium, large (default: medium)
  --help               Show this help message

Examples:
  test_data_generator --count 50000 --output large_dataset.json
  test_data_generator --count 1000 --format csv --output test_data.csv
  test_data_generator --count 10000 --format sql --table users --output users.sql

)";
}

int main(int argc, char* argv[]) {
    uint32_t count = 10000;
    std::string output_file = "test_data.json";
    std::string format = "json";
    std::string table_name = "test_table";
    std::string size = "medium";
    bool help = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            help = true;
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::stoul(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            format = argv[++i];
        } else if (arg == "--table" && i + 1 < argc) {
            table_name = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            size = argv[++i];
        }
    }

    if (help) {
        PrintUsage();
        return 0;
    }

    spdlog::set_level(spdlog::level::info);

    spdlog::info("📊 Test Data Generator");
    spdlog::info("Generating {} records", count);
    spdlog::info("Format: {}, Size: {}", format, size);

    // Get data configuration based on size
    DataGenConfig config;
    if (size == "small") {
        config = StorageTestFactory::GetSmallDataConfig();
    } else if (size == "large") {
        config = StorageTestFactory::GetLargeDataConfig();
    } else {
        config = StorageTestFactory::GetMediumDataConfig();
    }
    config.table_name = table_name;

    TestDataGenerator generator(config);

    spdlog::info("Starting data generation...");

    std::ofstream file(output_file);
    if (!file.is_open()) {
        spdlog::error("❌ Cannot open output file: {}", output_file);
        return 1;
    }

    if (format == "json") {
        // Generate JSON array
        file << "[\n";
        for (uint32_t i = 0; i < count; ++i) {
            auto row = generator.GenerateRandomRow();

            file << "  {\n";
            file << "    \"key\": \"" << row.key << "\",\n";
            file << "    \"values\": {\n";

            bool first = true;
            for (const auto& [col_name, value] : row.columns) {
                if (!first) file << ",\n";
                first = false;

                file << "      \"" << col_name << "\": ";
                std::visit([&file](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        file << "null";
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        file << "\"" << v << "\"";
                    } else if constexpr (std::is_same_v<T, std::vector<unsigned char>>) {
                        file << "\"" << std::string(v.begin(), v.end()) << "\"";
                    } else if constexpr (std::is_same_v<T, bool>) {
                        file << (v ? "true" : "false");
                    } else if constexpr (std::is_same_v<T, std::chrono::time_point<std::chrono::system_clock>>) {
                        auto time_t = std::chrono::system_clock::to_time_t(v);
                        auto tm = *std::gmtime(&time_t);
                        file << "\"" << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ") << "\"";
                    } else {
                        file << v;
                    }
                }, value);
            }

            file << "\n    }\n";
            file << "  }";
            if (i < count - 1) file << ",";
            file << "\n";

            if (i % 1000 == 0) {
                spdlog::info("Generated {} records", i);
            }
        }
        file << "]\n";

    } else if (format == "csv") {
        // Generate CSV
        file << "key";
        auto sample = generator.GenerateRandomRow();
        for (const auto& [col_name, _] : sample.columns) {
            file << "," << col_name;
        }
        file << "\n";

        for (uint32_t i = 0; i < count; ++i) {
            auto row = generator.GenerateRandomRow();
            file << row.key;

            for (const auto& [col_name, value] : row.columns) {
                file << ",";
                std::visit([&file](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        file << "null";
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        file << "\"" << v << "\"";
                    } else if constexpr (std::is_same_v<T, std::vector<unsigned char>>) {
                        file << "\"" << std::string(v.begin(), v.end()) << "\"";
                    } else if constexpr (std::is_same_v<T, bool>) {
                        file << (v ? "true" : "false");
                    } else if constexpr (std::is_same_v<T, std::chrono::time_point<std::chrono::system_clock>>) {
                        auto time_t = std::chrono::system_clock::to_time_t(v);
                        auto tm = *std::gmtime(&time_t);
                        file << "\"" << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ") << "\"";
                    } else {
                        file << v;
                    }
                }, value);
            }
            file << "\n";

            if (i % 1000 == 0) {
                spdlog::info("Generated {} records", i);
            }
        }

    } else if (format == "sql") {
        // Generate SQL INSERT statements
        auto sample = generator.GenerateRandomRow();

        file << "-- Generated test data for table " << table_name << "\n";
        file << "-- " << count << " INSERT statements\n\n";

        for (uint32_t i = 0; i < count; ++i) {
            auto row = generator.GenerateRandomRow();

            file << "INSERT INTO " << table_name << " (key";
            for (const auto& [col_name, _] : row.columns) {
                file << ", " << col_name;
            }
            file << ") VALUES ('" << row.key << "'";

            for (const auto& [col_name, value] : row.columns) {
                file << ", ";
                std::visit([&file](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        file << "NULL";
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        file << "'" << v << "'";
                    } else if constexpr (std::is_same_v<T, std::vector<unsigned char>>) {
                        file << "'" << std::string(v.begin(), v.end()) << "'";
                    } else if constexpr (std::is_same_v<T, bool>) {
                        file << (v ? "TRUE" : "FALSE");
                    } else if constexpr (std::is_same_v<T, std::chrono::time_point<std::chrono::system_clock>>) {
                        auto time_t = std::chrono::system_clock::to_time_t(v);
                        auto tm = *std::gmtime(&time_t);
                        file << "'" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "'";
                    } else {
                        file << v;
                    }
                }, value);
            }
            file << ");\n";

            if (i % 1000 == 0) {
                spdlog::info("Generated {} records", i);
            }
        }

    } else {
        spdlog::error("❌ Unknown format: {}", format);
        return 1;
    }

    file.close();

    spdlog::info("✅ Successfully generated {} records", count);
    spdlog::info("📁 Output file: {}", output_file);

    // Print file size
    std::ifstream check_file(output_file, std::ios::ate | std::ios::binary);
    if (check_file.is_open()) {
        auto size_bytes = check_file.tellg();
        double size_mb = size_bytes / (1024.0 * 1024.0);
        spdlog::info("📏 File size: {:.2f} MB", size_mb);
    }

    return 0;
}