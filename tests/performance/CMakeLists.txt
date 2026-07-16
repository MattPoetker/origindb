# Performance Testing Suite CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# Include main project settings
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/tests/performance)

# Use already available targets from parent project
# spdlog and nlohmann_json are already available from parent CMakeLists.txt

# Check if gRPC is available from parent project
if(GRPC_AVAILABLE)
    message(STATUS "gRPC available for performance tests")
else()
    message(STATUS "gRPC not available, some performance tests will be disabled")
endif()

# WebSocket++ (header-only library) - optional
find_path(WEBSOCKETPP_INCLUDE_DIR
    NAMES websocketpp/config/asio_no_tls.hpp
    HINTS /usr/local/include /usr/include
)

if(NOT WEBSOCKETPP_INCLUDE_DIR)
    message(STATUS "WebSocketPP not found, WebSocket tests will be simplified")
    set(ENABLE_WEBSOCKET_TESTS OFF)
else()
    message(STATUS "WebSocketPP found at: ${WEBSOCKETPP_INCLUDE_DIR}")
    set(ENABLE_WEBSOCKET_TESTS ON)
endif()

# Performance test framework library
add_library(perf_framework STATIC
    framework/performance_test.cpp
)

target_link_libraries(perf_framework
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

target_compile_features(perf_framework PRIVATE cxx_std_17)

# Storage performance tests
add_library(storage_perf_tests STATIC
    storage/storage_perf_test.cpp
)

target_link_libraries(storage_perf_tests
    perf_framework
    storage
    common_lib
)

target_compile_features(storage_perf_tests PRIVATE cxx_std_17)

# Simple WebSocket performance tests (no external dependencies)
add_library(websocket_perf_tests STATIC
    websocket/simple_websocket_test.cpp
    ../../src/websocket/binary_protocol.cpp
)

target_link_libraries(websocket_perf_tests
    perf_framework
    nlohmann_json::nlohmann_json
    pthread
)

target_compile_features(websocket_perf_tests PRIVATE cxx_std_17)

# gRPC performance tests (disabled for now due to protobuf interface issues)
# TODO: Fix gRPC protobuf interfaces and re-enable
set(ENABLE_GRPC_TESTS OFF)

# Regression benchmark suite (fixed op counts, JSON baselines)
add_executable(instantdb_bench
    instantdb_bench.cpp
)
target_link_libraries(instantdb_bench
    storage
    changefeed_engine
    wasm_engine
    wasmtime_c_api
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_features(instantdb_bench PRIVATE cxx_std_20)

# Main performance test runner executable
add_executable(perf_test_runner
    perf_test_runner.cpp
)

target_link_libraries(perf_test_runner
    perf_framework
    storage_perf_tests
    storage
    sql_engine
    changefeed_engine
    wasm_engine
    websocket_server
    common_lib
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# gRPC tests disabled for now
# if(ENABLE_GRPC_TESTS)
#     target_link_libraries(perf_test_runner grpc_perf_tests)
#     target_compile_definitions(perf_test_runner PRIVATE GRPC_AVAILABLE)
# endif()

# Add WebSocket tests (simple implementation)
target_link_libraries(perf_test_runner websocket_perf_tests)

target_compile_features(perf_test_runner PRIVATE cxx_std_17)

# Individual test executables for focused testing
add_executable(storage_perf_runner
    tools/storage_runner.cpp
)

target_link_libraries(storage_perf_runner
    perf_framework
    storage_perf_tests
    storage
    changefeed_engine
    common_lib
    spdlog::spdlog
)

target_compile_features(storage_perf_runner PRIVATE cxx_std_17)

# Benchmark comparison tool
add_executable(benchmark_compare
    tools/benchmark_compare.cpp
)

target_link_libraries(benchmark_compare
    perf_framework
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_compile_features(benchmark_compare PRIVATE cxx_std_17)

# Performance monitoring dashboard (disabled due to gRPC dependencies)
# add_executable(perf_monitor
#     tools/perf_monitor.cpp
# )
#
# target_link_libraries(perf_monitor
#     perf_framework
#     spdlog::spdlog
# )
#
# target_compile_features(perf_monitor PRIVATE cxx_std_17)

# Test data generator utility
add_executable(test_data_generator
    tools/test_data_generator.cpp
)

target_link_libraries(test_data_generator
    storage_perf_tests
    storage
    changefeed_engine
    common_lib
    spdlog::spdlog
)

target_compile_features(test_data_generator PRIVATE cxx_std_17)

# Binary protocol test executable
add_executable(binary_protocol_test
    websocket/binary_protocol_test.cpp
    ../../src/websocket/binary_protocol.cpp
)

target_link_libraries(binary_protocol_test
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

target_compile_features(binary_protocol_test PRIVATE cxx_std_17)

# Installation (perf_monitor disabled)
install(TARGETS perf_test_runner storage_perf_runner benchmark_compare test_data_generator
        DESTINATION bin)

# Create performance test directories
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/performance/reports)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/performance/data)

# Copy test scripts
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_all_tests.sh
    ${CMAKE_BINARY_DIR}/tests/performance/run_all_tests.sh
    COPYONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_stress_test.sh
    ${CMAKE_BINARY_DIR}/tests/performance/run_stress_test.sh
    COPYONLY
)

# Performance test targets
add_custom_target(perf-tests-light
    COMMAND ${CMAKE_BINARY_DIR}/tests/performance/perf_test_runner --test storage-crud --duration 30 --threads 2
    DEPENDS perf_test_runner
    COMMENT "Running light performance tests"
)

add_custom_target(perf-tests-medium
    COMMAND ${CMAKE_BINARY_DIR}/tests/performance/perf_test_runner --test storage-crud --duration 120 --threads 4
    DEPENDS perf_test_runner
    COMMENT "Running medium performance tests"
)

add_custom_target(perf-tests-heavy
    COMMAND ${CMAKE_BINARY_DIR}/tests/performance/perf_test_runner --test integration-full --duration 300 --threads 8
    DEPENDS perf_test_runner
    COMMENT "Running comprehensive performance tests"
)

add_custom_target(perf-stress-test
    COMMAND ${CMAKE_BINARY_DIR}/tests/performance/run_stress_test.sh
    DEPENDS perf_test_runner
    COMMENT "Running stress tests"
)

# Generate performance report
add_custom_target(perf-report
    COMMAND ${CMAKE_BINARY_DIR}/tests/performance/benchmark_compare --input reports/ --output performance_report.html
    DEPENDS benchmark_compare
    COMMENT "Generating performance report"
)