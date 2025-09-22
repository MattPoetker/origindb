cmake_minimum_required(VERSION 3.20)
project(InstantDB VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG)
    endif()
endif()

# Find basic packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Find gRPC and Protobuf using pkg-config (more reliable on macOS)
find_package(PkgConfig REQUIRED)

# Try to find protobuf and grpc using pkg-config
pkg_check_modules(PROTOBUF QUIET protobuf)
pkg_check_modules(GRPC QUIET grpc++)

# Check if gRPC is available
message(STATUS "Debug: PROTOBUF_FOUND=${PROTOBUF_FOUND}, GRPC_FOUND=${GRPC_FOUND}")
if(PROTOBUF_FOUND AND GRPC_FOUND)
    set(GRPC_AVAILABLE TRUE)
    message(STATUS "gRPC found via pkg-config - building with gRPC support")
    message(STATUS "Protobuf version: ${PROTOBUF_VERSION}")
    message(STATUS "gRPC version: ${GRPC_VERSION}")
else()
    # Fallback to CMake find_package
    find_package(Protobuf QUIET)
    find_package(gRPC QUIET)

    if(Protobuf_FOUND AND gRPC_FOUND)
        set(GRPC_AVAILABLE TRUE)
        message(STATUS "gRPC found via CMake - building with gRPC support")
    else()
        set(GRPC_AVAILABLE FALSE)
        message(STATUS "gRPC not found - building without gRPC support")
    endif()
endif()

# Third-party dependencies (minimal)
include(FetchContent)

# JSON library
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)

# spdlog for logging
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)

# fmt for formatting
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.1.1
)

FetchContent_MakeAvailable(json spdlog fmt)

# Generate gRPC and Protocol Buffer files (only if gRPC is available)
if(GRPC_AVAILABLE)
    set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
    set(GENERATED_PROTOBUF_PATH ${CMAKE_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${GENERATED_PROTOBUF_PATH})

    set(PROTO_SRCS "${GENERATED_PROTOBUF_PATH}/instantdb.pb.cc")
    set(PROTO_HDRS "${GENERATED_PROTOBUF_PATH}/instantdb.pb.h")
    set(GRPC_SRCS "${GENERATED_PROTOBUF_PATH}/instantdb.grpc.pb.cc")
    set(GRPC_HDRS "${GENERATED_PROTOBUF_PATH}/instantdb.grpc.pb.h")

    # Find protoc and grpc_cpp_plugin
    find_program(PROTOC_EXECUTABLE protoc REQUIRED)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

    add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --grpc_out=${GENERATED_PROTOBUF_PATH}
             --cpp_out=${GENERATED_PROTOBUF_PATH}
             --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
             --proto_path=${PROTO_DIR}
             ${PROTO_DIR}/instantdb.proto
        DEPENDS ${PROTO_DIR}/instantdb.proto
        COMMENT "Generating gRPC and Protocol Buffer files"
    )

    # Include generated files directory
    include_directories(${GENERATED_PROTOBUF_PATH})
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files (minimal set)
file(GLOB_RECURSE STORAGE_SRCS "src/storage/*.cpp")
file(GLOB_RECURSE SQL_SRCS "src/sql/*.cpp")
set(CHANGEFEED_SRCS
    "src/changefeed/changefeed_engine.cpp"
    "src/changefeed/sql_subscription.cpp"
)
file(GLOB_RECURSE WEBSOCKET_SRCS "src/websocket/*.cpp")
file(GLOB_RECURSE COMMON_SRCS "src/common/*.cpp")
file(GLOB_RECURSE WASM_SRCS "src/wasm/*.cpp")

# gRPC sources (only if available)
if(GRPC_AVAILABLE)
    file(GLOB_RECURSE GRPC_SERVICE_SRCS "src/grpc/*.cpp")
endif()

# Libraries
add_library(storage STATIC ${STORAGE_SRCS})
target_link_libraries(storage
    spdlog::spdlog
    fmt::fmt
    nlohmann_json::nlohmann_json
    Threads::Threads
)

add_library(changefeed_engine STATIC ${CHANGEFEED_SRCS})
target_link_libraries(changefeed_engine
    storage
    spdlog::spdlog
    Threads::Threads
)

add_library(sql_engine STATIC ${SQL_SRCS})
target_link_libraries(sql_engine
    storage
    changefeed_engine
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

add_library(websocket_server STATIC ${WEBSOCKET_SRCS})
target_link_libraries(websocket_server
    changefeed_engine
    wasm_engine
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

add_library(common_lib STATIC ${COMMON_SRCS})
target_link_libraries(common_lib
    spdlog::spdlog
    fmt::fmt
)

add_library(wasm_engine STATIC ${WASM_SRCS})
target_link_libraries(wasm_engine
    storage
    changefeed_engine
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# gRPC service library (only if available)
if(GRPC_AVAILABLE)
    add_library(grpc_service STATIC ${GRPC_SERVICE_SRCS} ${PROTO_SRCS} ${GRPC_SRCS})

    message(STATUS "Debug grpc_service: PROTOBUF_FOUND=${PROTOBUF_FOUND}, GRPC_FOUND=${GRPC_FOUND}")
    if(PROTOBUF_FOUND AND GRPC_FOUND)
        # Use pkg-config flags
        message(STATUS "Using pkg-config for grpc_service linking")
        target_include_directories(grpc_service PRIVATE ${PROTOBUF_INCLUDE_DIRS} ${GRPC_INCLUDE_DIRS})
        target_link_directories(grpc_service PRIVATE ${PROTOBUF_LIBRARY_DIRS} ${GRPC_LIBRARY_DIRS})
        target_link_libraries(grpc_service
            storage
            sql_engine
            spdlog::spdlog
            ${PROTOBUF_LIBRARIES}
            ${GRPC_LIBRARIES}
        )
        target_compile_options(grpc_service PRIVATE ${PROTOBUF_CFLAGS_OTHER} ${GRPC_CFLAGS_OTHER})
    else()
        # Use CMake targets
        message(STATUS "Using CMake targets for grpc_service linking")
        target_link_libraries(grpc_service
            storage
            sql_engine
            spdlog::spdlog
            protobuf::libprotobuf
            gRPC::grpc++
            gRPC::grpc++_reflection
        )
    endif()
endif()

# Demo executable with WebSocket server
add_executable(instantdb_demo src/cmd/prototype_demo.cpp)
target_link_libraries(instantdb_demo
    storage
    sql_engine
    changefeed_engine
    websocket_server
    common_lib
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
    nlohmann_json::nlohmann_json
)

# Simple SQL client (always available, no gRPC required)
add_executable(instantdb_sql src/cmd/sql_demo.cpp)
target_link_libraries(instantdb_sql
    storage
    sql_engine
    common_lib
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
)

# Production server executable
add_executable(instantdb_server src/cmd/instantdb_server.cpp)
target_link_libraries(instantdb_server
    storage
    sql_engine
    changefeed_engine
    websocket_server
    wasm_engine
    common_lib
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
    nlohmann_json::nlohmann_json
)

# Add gRPC support if available
if(GRPC_AVAILABLE)
    target_link_libraries(instantdb_server grpc_service)
    target_compile_definitions(instantdb_server PRIVATE GRPC_AVAILABLE)

    # Add protobuf/grpc library directories if using pkg-config
    if(PROTOBUF_FOUND AND GRPC_FOUND)
        target_link_directories(instantdb_server PRIVATE ${PROTOBUF_LIBRARY_DIRS} ${GRPC_LIBRARY_DIRS})
    endif()
endif()

# gRPC client executable (only if gRPC is available)
if(GRPC_AVAILABLE)
    add_executable(instantdb_client src/cmd/grpc_client.cpp ${PROTO_SRCS} ${GRPC_SRCS})

    if(PROTOBUF_FOUND AND GRPC_FOUND)
        # Use pkg-config flags
        target_include_directories(instantdb_client PRIVATE ${PROTOBUF_INCLUDE_DIRS} ${GRPC_INCLUDE_DIRS})
        target_link_directories(instantdb_client PRIVATE ${PROTOBUF_LIBRARY_DIRS} ${GRPC_LIBRARY_DIRS})
        target_link_libraries(instantdb_client
            ${PROTOBUF_LIBRARIES}
            ${GRPC_LIBRARIES}
            Threads::Threads
        )
        target_compile_options(instantdb_client PRIVATE ${PROTOBUF_CFLAGS_OTHER} ${GRPC_CFLAGS_OTHER})
    else()
        # Use CMake targets
        target_link_libraries(instantdb_client
            protobuf::libprotobuf
            gRPC::grpc++
            Threads::Threads
        )
    endif()

    # Installation with gRPC client
    install(TARGETS instantdb_demo instantdb_server instantdb_sql instantdb_client DESTINATION bin)
else()
    # Installation without gRPC client
    install(TARGETS instantdb_demo instantdb_server instantdb_sql DESTINATION bin)
endif()