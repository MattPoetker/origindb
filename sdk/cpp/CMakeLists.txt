cmake_minimum_required(VERSION 3.20)
project(OriginDB_CPP_SDK VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Configure for WebAssembly target if building for WASM
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    message(STATUS "Building for WebAssembly")

    # Emscripten-specific flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s WASM=1 -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s ALLOW_MEMORY_GROWTH=1")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s MAXIMUM_MEMORY=67108864") # 64MB limit
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2") # Optimize for size and speed

    # Export functions for OriginDB host
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s EXPORTED_FUNCTIONS=['_malloc','_free']")

else()
    message(STATUS "Building for native target (testing)")

    # Native build flags for testing
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Wpedantic)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_options(-g -O0)
        elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
            add_compile_options(-O3)
        endif()
    endif()
endif()

# OriginDB SDK library (header-only)
add_library(origindb_sdk INTERFACE)
target_include_directories(origindb_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Example: Counter module
add_executable(counter_module examples/counter_module.cpp)
target_link_libraries(counter_module origindb_sdk)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # Set output extension for WASM
    set_target_properties(counter_module PROPERTIES SUFFIX ".wasm")

    # Export specific functions for this module
    set_target_properties(counter_module PROPERTIES
        LINK_FLAGS "-s EXPORTED_FUNCTIONS=['_create_counter','_increment_counter','_get_counter_value','_delete_counter','_module_init','_client_connected','_client_disconnected']"
    )
endif()

# Installation
install(FILES origindb.hpp DESTINATION include/origindb)
install(DIRECTORY examples/ DESTINATION share/origindb/examples
        FILES_MATCHING PATTERN "*.cpp" PATTERN "*.hpp")

# Package configuration
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/OriginDBConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/OriginDBConfig.cmake"
    INSTALL_DESTINATION lib/cmake/OriginDB
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/OriginDBConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/OriginDBConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/OriginDBConfigVersion.cmake"
    DESTINATION lib/cmake/OriginDB
)

export(TARGETS origindb_sdk FILE "${CMAKE_CURRENT_BINARY_DIR}/OriginDBTargets.cmake")
install(EXPORT OriginDBTargets DESTINATION lib/cmake/OriginDB)