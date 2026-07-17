#!/bin/bash
set -e

# OriginDB Release Build Script
# Builds and packages OriginDB for multiple platforms

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-release"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Print usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build and package OriginDB for distribution.

OPTIONS:
    -p, --platform PLATFORM    Target platform (linux, macos, windows, all)
    -a, --arch ARCH            Target architecture (x64, arm64, all)
    -t, --type TYPE            Package type (binary, deb, rpm, dmg, msi, all)
    -j, --jobs N               Number of parallel build jobs (default: auto)
    -c, --clean                Clean build directory before building
    -v, --verbose              Enable verbose output
    -h, --help                 Show this help

EXAMPLES:
    $0 --platform linux --type deb
    $0 --platform macos --arch arm64 --type dmg
    $0 --platform all --type binary
    $0 --clean --verbose

EOF
}

# Default values
PLATFORM="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="x64"
PACKAGE_TYPE="binary"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
CLEAN=false
VERBOSE=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--platform)
            PLATFORM="$2"
            shift 2
            ;;
        -a|--arch)
            ARCH="$2"
            shift 2
            ;;
        -t|--type)
            PACKAGE_TYPE="$2"
            shift 2
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Validate platform
case "$PLATFORM" in
    linux|macos|windows|all) ;;
    darwin) PLATFORM="macos" ;;
    *)
        log_error "Unsupported platform: $PLATFORM"
        log_info "Supported platforms: linux, macos, windows, all"
        exit 1
        ;;
esac

# Validate architecture
case "$ARCH" in
    x64|arm64|all) ;;
    amd64) ARCH="x64" ;;
    aarch64) ARCH="arm64" ;;
    *)
        log_error "Unsupported architecture: $ARCH"
        log_info "Supported architectures: x64, arm64, all"
        exit 1
        ;;
esac

# Validate package type
case "$PACKAGE_TYPE" in
    binary|deb|rpm|dmg|msi|all) ;;
    *)
        log_error "Unsupported package type: $PACKAGE_TYPE"
        log_info "Supported types: binary, deb, rpm, dmg, msi, all"
        exit 1
        ;;
esac

# Set verbose flag
if [ "$VERBOSE" = true ]; then
    set -x
    CMAKE_VERBOSE="-DCMAKE_VERBOSE_MAKEFILE=ON"
else
    CMAKE_VERBOSE=""
fi

# Clean build directory if requested
if [ "$CLEAN" = true ]; then
    log_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Build function
build_platform() {
    local platform=$1
    local arch=$2
    local build_subdir="$BUILD_DIR/${platform}-${arch}"

    log_info "Building OriginDB for $platform-$arch..."

    mkdir -p "$build_subdir"
    cd "$build_subdir"

    # Configure CMake based on platform
    local cmake_args=(
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_INSTALL_PREFIX=/usr/local"
        $CMAKE_VERBOSE
    )

    case "$platform" in
        linux)
            cmake_args+=(
                "-DCMAKE_SYSTEM_NAME=Linux"
                "-GNinja"
            )
            ;;
        macos)
            cmake_args+=(
                "-DCMAKE_SYSTEM_NAME=Darwin"
                "-GNinja"
            )
            if [ "$arch" = "arm64" ]; then
                cmake_args+=("-DCMAKE_OSX_ARCHITECTURES=arm64")
            else
                cmake_args+=("-DCMAKE_OSX_ARCHITECTURES=x86_64")
            fi
            ;;
        windows)
            cmake_args+=(
                "-DCMAKE_SYSTEM_NAME=Windows"
                "-G" "Visual Studio 16 2019"
            )
            if [ "$arch" = "arm64" ]; then
                cmake_args+=("-A" "ARM64")
            else
                cmake_args+=("-A" "x64")
            fi
            ;;
    esac

    # Configure
    log_info "Configuring build for $platform-$arch..."
    cmake "$PROJECT_DIR" "${cmake_args[@]}"

    # Build
    log_info "Building OriginDB for $platform-$arch..."
    if [ "$platform" = "windows" ]; then
        cmake --build . --config Release --parallel $JOBS
    else
        ninja -j$JOBS
    fi

    log_success "Build completed for $platform-$arch"
}

# Package function
create_package() {
    local platform=$1
    local arch=$2
    local type=$3
    local build_subdir="$BUILD_DIR/${platform}-${arch}"

    log_info "Creating $type package for $platform-$arch..."

    cd "$build_subdir"

    case "$type" in
        binary)
            # Create binary archive
            local archive_name="origindb-${platform}-${arch}"
            mkdir -p "$archive_name"

            # Copy binaries
            if [ "$platform" = "windows" ]; then
                cp Release/*.exe "$archive_name/"
            else
                cp origindb* "$archive_name/"
            fi

            # Copy documentation
            cp "$PROJECT_DIR/README.md" "$archive_name/"
            cp "$PROJECT_DIR/INSTALL.md" "$archive_name/"
            cp "$PROJECT_DIR/LICENSE" "$archive_name/" 2>/dev/null || true

            # Create archive
            if [ "$platform" = "windows" ]; then
                zip -r "${archive_name}.zip" "$archive_name"
                log_success "Created ${archive_name}.zip"
            else
                tar czf "${archive_name}.tar.gz" "$archive_name"
                log_success "Created ${archive_name}.tar.gz"
            fi
            ;;
        deb|rpm|dmg|msi)
            # Use CPack to create platform-specific packages
            local cpack_generator
            case "$type" in
                deb) cpack_generator="DEB" ;;
                rpm) cpack_generator="RPM" ;;
                dmg) cpack_generator="DragNDrop" ;;
                msi) cpack_generator="WIX" ;;
            esac

            cpack -G "$cpack_generator"
            log_success "Created $type package for $platform-$arch"
            ;;
    esac
}

# Main build logic
build_targets=()
package_targets=()

if [ "$PLATFORM" = "all" ]; then
    case "$(uname -s)" in
        Linux) build_targets+=("linux") ;;
        Darwin) build_targets+=("macos") ;;
        CYGWIN*|MINGW*|MSYS*) build_targets+=("windows") ;;
        *) build_targets+=("linux" "macos" "windows") ;;
    esac
else
    build_targets+=("$PLATFORM")
fi

if [ "$ARCH" = "all" ]; then
    arch_targets=("x64" "arm64")
else
    arch_targets=("$ARCH")
fi

if [ "$PACKAGE_TYPE" = "all" ]; then
    case "$PLATFORM" in
        linux) package_targets=("binary" "deb" "rpm") ;;
        macos) package_targets=("binary" "dmg") ;;
        windows) package_targets=("binary" "msi") ;;
        all) package_targets=("binary") ;;
    esac
else
    package_targets=("$PACKAGE_TYPE")
fi

# Check dependencies
log_info "Checking build dependencies..."

# Check for required tools
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        log_error "$1 is required but not installed"
        return 1
    fi
}

check_tool cmake
check_tool git

case "$(uname -s)" in
    Linux)
        check_tool ninja || check_tool make
        ;;
    Darwin)
        check_tool ninja || check_tool make
        ;;
    CYGWIN*|MINGW*|MSYS*)
        # Windows-specific checks
        ;;
esac

# Perform builds
cd "$PROJECT_DIR"

for platform in "${build_targets[@]}"; do
    for arch in "${arch_targets[@]}"; do
        # Skip incompatible combinations
        if [ "$platform" = "windows" ] && [ "$arch" = "arm64" ]; then
            log_warning "Skipping Windows ARM64 build (not yet supported)"
            continue
        fi

        build_platform "$platform" "$arch"

        for package_type in "${package_targets[@]}"; do
            # Skip incompatible package types
            case "$platform-$package_type" in
                linux-dmg|linux-msi|macos-deb|macos-rpm|macos-msi|windows-deb|windows-rpm|windows-dmg)
                    log_warning "Skipping incompatible package type: $package_type for $platform"
                    continue
                    ;;
            esac

            create_package "$platform" "$arch" "$package_type"
        done
    done
done

# Generate checksums
log_info "Generating checksums..."
cd "$BUILD_DIR"
find . -name "*.tar.gz" -o -name "*.zip" -o -name "*.deb" -o -name "*.rpm" -o -name "*.dmg" -o -name "*.msi" | \
    xargs sha256sum > checksums.txt

log_success "Release build completed!"
log_info "Build artifacts are in: $BUILD_DIR"
log_info "Checksums: $BUILD_DIR/checksums.txt"

# Print summary
echo
echo "=== Build Summary ==="
find "$BUILD_DIR" -maxdepth 2 -name "*.tar.gz" -o -name "*.zip" -o -name "*.deb" -o -name "*.rpm" -o -name "*.dmg" -o -name "*.msi" | \
    sort | while read -r file; do
        size=$(du -h "$file" | cut -f1)
        echo "  $size  $(basename "$file")"
    done