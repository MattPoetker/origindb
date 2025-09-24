#!/bin/bash
# InstantDB Installation Script for macOS and Linux
# Usage: curl -sSf https://install.instantdb.com | sh

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
GITHUB_REPO="${GITHUB_REPO:-instantdb/instantdb}"  # Allow override via environment
MAIN_BINARY="instantdb"  # Main CLI wrapper
BINARIES="instantdb instantdb_server instantdb_sql instantdb_client instantdb_demo instantdb_init"  # All binaries to install
INSTALL_DIR="/usr/local/bin"
SHARE_DIR="/usr/local/share/instantdb"  # Directory for proto files and other resources
TMP_DIR="/tmp/instantdb-install"
LOCAL_BUILD_DIR="$(dirname "$0")/.."  # Path to root directory (where current binaries are)
LOCAL_PROTO_FILE="$(dirname "$0")/../instantdb.proto"  # Path to proto file in repo root

# Logging functions
log() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

# Detect OS and architecture
detect_platform() {
    local os=""
    local arch=""

    case "$(uname -s)" in
        Darwin*)
            os="macos"
            ;;
        Linux*)
            os="linux"
            ;;
        *)
            error "Unsupported operating system: $(uname -s)"
            exit 1
            ;;
    esac

    case "$(uname -m)" in
        x86_64|amd64)
            arch="x64"
            ;;
        arm64|aarch64)
            arch="arm64"
            ;;
        *)
            error "Unsupported architecture: $(uname -m)"
            exit 1
            ;;
    esac

    echo "${os}-${arch}"
}

# Check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check prerequisites
check_prerequisites() {
    log "Checking prerequisites..."

    if ! command_exists curl; then
        error "curl is required but not installed. Please install curl and try again."
        exit 1
    fi

    if ! command_exists tar; then
        error "tar is required but not installed. Please install tar and try again."
        exit 1
    fi

    success "Prerequisites check passed"
}

# Get latest release version
get_latest_version() {
    log "Fetching latest release version..."

    local version=""
    if command_exists curl; then
        version=$(curl -sSf "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" | grep '"tag_name"' | sed -E 's/.*"([^"]+)".*/\1/')
    fi

    if [ -z "$version" ]; then
        warn "Could not fetch latest version, using 'latest'"
        version="latest"
    fi

    echo "$version"
}

# Download and extract
download_and_extract() {
    local platform="$1"
    local version="$2"

    log "Downloading InstantDB for $platform..."

    # Create temporary directory
    mkdir -p "$TMP_DIR"
    cd "$TMP_DIR"

    # Download URL
    local download_url=""
    if [ "$version" = "latest" ]; then
        download_url="https://github.com/${GITHUB_REPO}/releases/latest/download/instantdb-${platform}.tar.gz"
    else
        download_url="https://github.com/${GITHUB_REPO}/releases/download/${version}/instantdb-${platform}.tar.gz"
    fi

    # Download and extract
    if curl -sSfL "$download_url" | tar xz; then
        success "Download completed"
    else
        error "Failed to download InstantDB"
        exit 1
    fi
}

# Install proto file
install_proto() {
    log "Installing proto file..."

    # Create share directory if it doesn't exist
    if [ -w "$(dirname "$SHARE_DIR")" ]; then
        mkdir -p "$SHARE_DIR"
    else
        log "Creating share directory requires sudo privileges"
        sudo mkdir -p "$SHARE_DIR"
    fi

    # Install proto file if it exists
    if [ -f "$TMP_DIR/instantdb.proto" ]; then
        log "Installing instantdb.proto..."

        if [ -w "$SHARE_DIR" ]; then
            cp "$TMP_DIR/instantdb.proto" "$SHARE_DIR/"
            chmod 644 "$SHARE_DIR/instantdb.proto"
        else
            log "Installing to system directory requires sudo privileges"
            sudo cp "$TMP_DIR/instantdb.proto" "$SHARE_DIR/"
            sudo chmod 644 "$SHARE_DIR/instantdb.proto"
        fi

        success "Proto file installed to $SHARE_DIR/instantdb.proto"
    else
        warn "instantdb.proto not found in package"
    fi
}

# Install binaries
install_binaries() {
    log "Installing InstantDB tools..."

    for binary in $BINARIES; do
        if [ -f "$TMP_DIR/$binary" ]; then
            log "Installing $binary..."

            # Check if we need sudo
            if [ -w "$INSTALL_DIR" ]; then
                cp "$TMP_DIR/$binary" "$INSTALL_DIR/"
                chmod +x "$INSTALL_DIR/$binary"
            else
                log "Installing to system directory requires sudo privileges"
                sudo cp "$TMP_DIR/$binary" "$INSTALL_DIR/"
                sudo chmod +x "$INSTALL_DIR/$binary"
            fi

            success "$binary installed to $INSTALL_DIR/$binary"
        else
            warn "$binary not found in package"
        fi
    done

    success "InstantDB tools installed"
}

# Verify installation
verify_installation() {
    log "Verifying installation..."

    if command_exists "$MAIN_BINARY"; then
        success "InstantDB installed successfully!"
        success "Location: $(which $MAIN_BINARY)"

        # Try to get version
        if "$MAIN_BINARY" --version >/dev/null 2>&1; then
            "$MAIN_BINARY" --version
        else
            # Show available commands
            if "$MAIN_BINARY" --help >/dev/null 2>&1; then
                success "Binary verified. Run 'instantdb --help' for available commands"
            fi
        fi
    else
        error "Installation verification failed. $MAIN_BINARY not found in PATH."
        warn "You may need to restart your terminal or add $INSTALL_DIR to your PATH"
        exit 1
    fi
}

# Cleanup
cleanup() {
    log "Cleaning up..."
    rm -rf "$TMP_DIR"
}

# Install from local build (for development)
install_local() {
    log "Installing from local build..."

    # Check if main binary exists
    if [ ! -f "$LOCAL_BUILD_DIR/$MAIN_BINARY" ]; then
        error "Local build not found at $LOCAL_BUILD_DIR/$MAIN_BINARY"
        error "Please build the project first with: make"
        exit 1
    fi

    # Create temporary directory
    mkdir -p "$TMP_DIR"

    # Copy all available binaries
    for binary in $BINARIES; do
        if [ -f "$LOCAL_BUILD_DIR/$binary" ]; then
            cp "$LOCAL_BUILD_DIR/$binary" "$TMP_DIR/"
            log "Found $binary"
        fi
    done

    # Copy proto file if it exists
    if [ -f "$LOCAL_PROTO_FILE" ]; then
        cp "$LOCAL_PROTO_FILE" "$TMP_DIR/"
        log "Found instantdb.proto"
    else
        warn "Proto file not found at $LOCAL_PROTO_FILE"
    fi

    success "Using local build"
}

# Main installation function
main() {
    log "Starting InstantDB installation..."

    # Parse arguments
    local use_local=false
    for arg in "$@"; do
        case $arg in
            --local)
                use_local=true
                ;;
            --help|-h)
                echo "InstantDB Installation Script"
                echo ""
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --local    Install from local build instead of downloading"
                echo "  --help     Show this help message"
                echo ""
                exit 0
                ;;
        esac
    done

    # Trap to ensure cleanup on exit
    trap cleanup EXIT

    # Check prerequisites
    check_prerequisites

    if [ "$use_local" = true ]; then
        # Install from local build
        install_local
    else
        # Detect platform
        local platform
        platform=$(detect_platform)
        log "Detected platform: $platform"

        # Get latest version
        local version
        version=$(get_latest_version)
        log "Installing version: $version"

        # Download and extract
        download_and_extract "$platform" "$version"
    fi

    # Install binaries
    install_binaries

    # Install proto file
    install_proto

    # Verify installation
    verify_installation

    success "🚀 InstantDB installation completed!"
    echo ""
    echo "Next steps:"
    echo "  1. Start the InstantDB server: instantdb server"
    echo "  2. Open the SQL shell: instantdb sql"
    echo "  3. See all commands: instantdb --help"
    echo "  4. Check out the documentation: https://docs.instantdb.com"
    echo "  5. Try the C# quickstart: https://docs.instantdb.com/csharp"
    echo ""
}

# Run main function
main "$@"