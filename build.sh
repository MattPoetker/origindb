#!/bin/bash

# InstantDB Build Script

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
BUILD_TYPE=${BUILD_TYPE:-Debug}
BUILD_DIR=${BUILD_DIR:-build}
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}

# Functions
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --clean)
            print_info "Cleaning build directory..."
            rm -rf ${BUILD_DIR}
            shift
            ;;
        --test)
            RUN_TESTS=1
            shift
            ;;
        --install)
            DO_INSTALL=1
            shift
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --release    Build in Release mode"
            echo "  --debug      Build in Debug mode (default)"
            echo "  --clean      Clean build directory before building"
            echo "  --test       Run tests after building"
            echo "  --install    Install after building"
            echo "  --help       Show this help message"
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check dependencies
print_info "Checking dependencies..."

check_command() {
    if ! command -v $1 &> /dev/null; then
        print_error "$1 is not installed"
        exit 1
    fi
}

check_command cmake
check_command make
check_command protoc

# Create build directory
print_info "Creating build directory: ${BUILD_DIR}"
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}

# Configure with CMake
print_info "Configuring with CMake (Build Type: ${BUILD_TYPE})..."
cmake .. \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_TESTS=ON

# Build
print_info "Building with ${JOBS} jobs..."
make -j${JOBS}

# Run tests if requested
if [ "${RUN_TESTS}" == "1" ]; then
    print_info "Running tests..."
    make test ARGS="--output-on-failure"
fi

# Install if requested
if [ "${DO_INSTALL}" == "1" ]; then
    print_info "Installing..."
    sudo make install
fi

print_info "Build completed successfully!"
print_info "Binary location: ${BUILD_DIR}/instantdb"

# Print next steps
echo ""
echo "Next steps:"
echo "  1. Run the server: ${BUILD_DIR}/instantdb --help"
echo "  2. Run tests: cd ${BUILD_DIR} && make test"
echo "  3. Install: cd ${BUILD_DIR} && sudo make install"