cmake_minimum_required(VERSION 3.20)
project(OriginDB VERSION 0.1.0 LANGUAGES CXX C)

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

# Fetched projects declare old cmake_minimum_required versions; CMake 4 removed
# compatibility with <3.5, so raise the floor for all FetchContent builds.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

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

# Wasmtime C API (prebuilt, platform-selected)
set(WASMTIME_VERSION "36.0.2" CACHE STRING "Wasmtime C API version (LTS line)")
if(APPLE)
    set(WASMTIME_OS "macos")
elseif(UNIX)
    set(WASMTIME_OS "linux")
else()
    message(FATAL_ERROR "Unsupported platform for wasmtime prebuilt C API")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(WASMTIME_ARCH "aarch64")
else()
    set(WASMTIME_ARCH "x86_64")
endif()
FetchContent_Declare(
    wasmtime
    URL https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/wasmtime-v${WASMTIME_VERSION}-${WASMTIME_ARCH}-${WASMTIME_OS}-c-api.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(wasmtime)

add_library(wasmtime_c_api STATIC IMPORTED GLOBAL)
set_target_properties(wasmtime_c_api PROPERTIES
    IMPORTED_LOCATION ${wasmtime_SOURCE_DIR}/lib/libwasmtime.a
    INTERFACE_INCLUDE_DIRECTORIES ${wasmtime_SOURCE_DIR}/include
)
if(NOT APPLE)
    set_property(TARGET wasmtime_c_api APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES dl m pthread rt)
endif()

# Generate gRPC and Protocol Buffer files (only if gRPC is available)
if(GRPC_AVAILABLE)
    set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
    set(GENERATED_PROTOBUF_PATH ${CMAKE_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${GENERATED_PROTOBUF_PATH})

    set(PROTO_SRCS "${GENERATED_PROTOBUF_PATH}/origindb.pb.cc")
    set(PROTO_HDRS "${GENERATED_PROTOBUF_PATH}/origindb.pb.h")
    set(GRPC_SRCS "${GENERATED_PROTOBUF_PATH}/origindb.grpc.pb.cc")
    set(GRPC_HDRS "${GENERATED_PROTOBUF_PATH}/origindb.grpc.pb.h")

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
             ${PROTO_DIR}/origindb.proto
        DEPENDS ${PROTO_DIR}/origindb.proto
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
    "src/changefeed/predicate_evaluator.cpp"
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
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
)

add_library(wasm_engine STATIC ${WASM_SRCS})
target_link_libraries(wasm_engine
    PUBLIC
        storage
        changefeed_engine
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
    PRIVATE
        wasmtime_c_api
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
            wasm_engine
            changefeed_engine
            websocket_server
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
            wasm_engine
            changefeed_engine
            websocket_server
            spdlog::spdlog
            protobuf::libprotobuf
            gRPC::grpc++
            gRPC::grpc++_reflection
        )
    endif()
endif()

# Demo executable with WebSocket server
add_executable(origindb_demo src/cmd/prototype_demo.cpp)
target_link_libraries(origindb_demo
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
add_executable(origindb_sql src/cmd/sql_demo.cpp)
target_link_libraries(origindb_sql
    storage
    sql_engine
    common_lib
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
)

# Init command for project scaffolding
add_executable(origindb_init src/cmd/origindb_init.cpp)
target_link_libraries(origindb_init
    common_lib
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
)

# Production server executable
add_executable(origindb_server src/cmd/origindb_server.cpp)
target_link_libraries(origindb_server
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
    target_link_libraries(origindb_server grpc_service)
    target_compile_definitions(origindb_server PRIVATE GRPC_AVAILABLE)

    # Add protobuf/grpc library directories if using pkg-config
    if(PROTOBUF_FOUND AND GRPC_FOUND)
        target_link_directories(origindb_server PRIVATE ${PROTOBUF_LIBRARY_DIRS} ${GRPC_LIBRARY_DIRS})
    endif()
endif()

# CLI executable (main wrapper for all commands)
add_executable(origindb src/cmd/origindb_cli.cpp)
target_link_libraries(origindb
    common_lib
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
)

# CLI is just a wrapper, doesn't need gRPC directly

# gRPC client executable (only if gRPC is available)
if(GRPC_AVAILABLE)
    add_executable(origindb_client src/cmd/grpc_client.cpp ${PROTO_SRCS} ${GRPC_SRCS})

    if(PROTOBUF_FOUND AND GRPC_FOUND)
        # Use pkg-config flags
        target_include_directories(origindb_client PRIVATE ${PROTOBUF_INCLUDE_DIRS} ${GRPC_INCLUDE_DIRS})
        target_link_directories(origindb_client PRIVATE ${PROTOBUF_LIBRARY_DIRS} ${GRPC_LIBRARY_DIRS})
        target_link_libraries(origindb_client
            ${PROTOBUF_LIBRARIES}
            ${GRPC_LIBRARIES}
            nlohmann_json::nlohmann_json
            Threads::Threads
        )
        target_compile_options(origindb_client PRIVATE ${PROTOBUF_CFLAGS_OTHER} ${GRPC_CFLAGS_OTHER})
    else()
        # Use CMake targets
        target_link_libraries(origindb_client
            protobuf::libprotobuf
            gRPC::grpc++
            nlohmann_json::nlohmann_json
            Threads::Threads
        )
    endif()

    # Installation with gRPC client
    install(TARGETS origindb_demo origindb_server origindb_sql origindb_client origindb origindb_init DESTINATION bin)
else()
    # Installation without gRPC client
    install(TARGETS origindb_demo origindb_server origindb_sql origindb origindb_init DESTINATION bin)
endif()

# Package configuration with CPack
set(CPACK_PACKAGE_NAME "OriginDB")
set(CPACK_PACKAGE_VENDOR "OriginDB Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Real-time database with WebAssembly module support")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "OriginDB")

# Package description
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# Contact information
set(CPACK_PACKAGE_CONTACT "support@origindb.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://origindb.com")

# Platform-specific package configuration
if(WIN32)
    # Windows MSI installer
    set(CPACK_GENERATOR "WIX;ZIP")
    set(CPACK_WIX_UPGRADE_GUID "12345678-1234-1234-1234-123456789012")
    set(CPACK_WIX_PRODUCT_GUID "87654321-4321-4321-4321-210987654321")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "OriginDB")
    set(CPACK_WIX_UI_BANNER "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/banner.bmp")
    set(CPACK_WIX_UI_DIALOG "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/dialog.bmp")

    # Add to PATH
    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/path.xml")

elseif(APPLE)
    # macOS DMG
    set(CPACK_GENERATOR "DragNDrop;TGZ")
    set(CPACK_DMG_VOLUME_NAME "OriginDB")
    set(CPACK_DMG_FORMAT "UDBZ")
    set(CPACK_DMG_BACKGROUND_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/background.png")
    set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/setup.scpt")

elseif(UNIX)
    # Linux packages
    set(CPACK_GENERATOR "DEB;RPM;TGZ")

    # Debian/Ubuntu package
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OriginDB Team <support@origindb.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "database")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://origindb.com")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libprotobuf32, libgrpc++1.45, libssl3")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "OriginDB is a high-performance, distributed database with WebAssembly module support, bringing programmable capabilities to a production SQL engine.")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
    )

    # RPM package
    set(CPACK_RPM_PACKAGE_SUMMARY "Real-time database with WebAssembly module support")
    set(CPACK_RPM_PACKAGE_DESCRIPTION "OriginDB is a high-performance, distributed database with WebAssembly module support, bringing programmable capabilities to a production SQL engine.")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Databases")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_URL "https://origindb.com")
    set(CPACK_RPM_PACKAGE_REQUIRES "protobuf >= 3.0, grpc >= 1.40, openssl >= 1.1")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinstall.sh")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/preuninstall.sh")
endif()

# Source package
set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_IGNORE_FILES
    "/build/"
    "/\\.git/"
    "/\\.github/"
    "\\.gitignore"
    "\\.DS_Store"
    "/\\.vscode/"
    "/\\.idea/"
    "CMakeCache\\.txt"
    "CMakeFiles"
    "Makefile"
    "cmake_install\\.cmake"
    "CPackConfig\\.cmake"
    "CPackSourceConfig\\.cmake"
    "_CPack_Packages"
    "\\.tar\\.gz$"
    "\\.zip$"
)

# Component-based installation
set(CPACK_COMPONENTS_ALL Runtime Development)
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "OriginDB Runtime")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "OriginDB server and client tools")
set(CPACK_COMPONENT_DEVELOPMENT_DISPLAY_NAME "Development Files")
set(CPACK_COMPONENT_DEVELOPMENT_DESCRIPTION "Headers and development libraries")

# Install documentation
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/INSTALL.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/CLI_GUIDE.md"
    DESTINATION share/doc/origindb
    COMPONENT Runtime
)

# Install systemd service file (Linux only)
if(UNIX AND NOT APPLE)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/origindb.service"
        DESTINATION lib/systemd/system
        COMPONENT Runtime
    )
endif()

# Install shell completion scripts
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/shell/origindb.bash"
    DESTINATION share/bash-completion/completions
    RENAME origindb
    COMPONENT Runtime
)

# Unit tests
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(unit_tests
        tests/wasm/wasm_engine_test.cpp
        tests/unit/predicate_evaluator_test.cpp
        tests/unit/module_store_test.cpp
        tests/unit/wal_durability_test.cpp
        tests/unit/row_codec_test.cpp
    )
    target_link_libraries(unit_tests PRIVATE
        wasm_engine
        changefeed_engine
        storage
        wasmtime_c_api
        GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(unit_tests)

    # Small utility to compile .wat fixtures to .wasm for e2e testing
    add_executable(wat2wasm_tool tools/wat2wasm_tool.cpp)
    target_link_libraries(wat2wasm_tool PRIVATE wasmtime_c_api)
endif()

# Performance Testing Framework (optional)
option(BUILD_PERFORMANCE_TESTS "Build performance testing framework" ON)
if(BUILD_PERFORMANCE_TESTS)
    message(STATUS "Building performance testing framework")
    add_subdirectory(tests/performance)
endif()

include(CPack)