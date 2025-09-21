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

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files (minimal set)
file(GLOB_RECURSE STORAGE_SRCS "src/storage/*.cpp")
file(GLOB_RECURSE SQL_SRCS "src/sql/*.cpp")
file(GLOB_RECURSE CHANGEFEED_SRCS "src/changefeed/*.cpp")
file(GLOB_RECURSE WEBSOCKET_SRCS "src/websocket/*.cpp")
file(GLOB_RECURSE COMMON_SRCS "src/common/*.cpp")

# Libraries
add_library(storage STATIC ${STORAGE_SRCS})
target_link_libraries(storage
    spdlog::spdlog
    fmt::fmt
    nlohmann_json::nlohmann_json
    Threads::Threads
)

add_library(sql_engine STATIC ${SQL_SRCS})
target_link_libraries(sql_engine
    storage
    spdlog::spdlog
)

add_library(changefeed_engine STATIC ${CHANGEFEED_SRCS})
target_link_libraries(changefeed_engine
    storage
    spdlog::spdlog
    Threads::Threads
)

add_library(websocket_server STATIC ${WEBSOCKET_SRCS})
target_link_libraries(websocket_server
    changefeed_engine
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

# Installation
install(TARGETS instantdb_demo DESTINATION bin)