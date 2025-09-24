# InstantDB Installation Guide

This guide covers installation methods for InstantDB across different platforms, with special focus on C# developers who want to integrate InstantDB into their projects.

## 📦 Installation Methods

### Option 1: One-Liner Install (Recommended)

Quick installation using our install scripts:

#### macOS & Linux
```bash
curl -sSf https://install.instantdb.com | sh
```

#### Windows (PowerShell)
```powershell
iwr -useb https://install.instantdb.com/windows | iex
```

#### Windows (Command Prompt)
```cmd
curl -sSf https://install.instantdb.com/windows.bat | cmd
```

The install script will:
- Detect your OS and architecture automatically
- Download the latest stable release
- Install InstantDB to the appropriate system location
- Add it to your PATH
- Verify the installation

#### Manual Platform-Specific Downloads

If you prefer manual installation:

**macOS:**
```bash
# Intel Macs
curl -L https://github.com/your-org/instantdb/releases/latest/download/instantdb-macos-x64.tar.gz | tar xz
sudo mv instantdb /usr/local/bin/

# Apple Silicon Macs
curl -L https://github.com/your-org/instantdb/releases/latest/download/instantdb-macos-arm64.tar.gz | tar xz
sudo mv instantdb /usr/local/bin/
```

**Linux:**
```bash
# x64
curl -L https://github.com/your-org/instantdb/releases/latest/download/instantdb-linux-x64.tar.gz | tar xz
sudo mv instantdb /usr/local/bin/

# ARM64
curl -L https://github.com/your-org/instantdb/releases/latest/download/instantdb-linux-arm64.tar.gz | tar xz
sudo mv instantdb /usr/local/bin/
```

**Windows:**
```powershell
# Download and extract (PowerShell)
Invoke-WebRequest -Uri "https://github.com/your-org/instantdb/releases/latest/download/instantdb-windows-x64.zip" -OutFile "instantdb.zip"
Expand-Archive -Path "instantdb.zip" -DestinationPath "C:\tools\instantdb"
$env:PATH += ";C:\tools\instantdb"
```

**Verify Installation:**
```bash
instantdb --version
```

### Option 2: Package Managers

#### macOS (Homebrew)
```bash
brew tap your-org/instantdb
brew install instantdb
```

#### Linux (APT - Ubuntu/Debian)
```bash
curl -fsSL https://packages.instantdb.com/gpg.key | sudo gpg --dearmor -o /etc/apt/keyrings/instantdb.gpg
echo "deb [signed-by=/etc/apt/keyrings/instantdb.gpg] https://packages.instantdb.com/apt stable main" | sudo tee /etc/apt/sources.list.d/instantdb.list
sudo apt update
sudo apt install instantdb
```

#### Linux (YUM - CentOS/RHEL/Fedora)
```bash
sudo tee /etc/yum.repos.d/instantdb.repo <<EOF
[instantdb]
name=InstantDB Repository
baseurl=https://packages.instantdb.com/yum/
enabled=1
gpgcheck=1
gpgkey=https://packages.instantdb.com/gpg.key
EOF

sudo yum install instantdb
```

#### Windows (Chocolatey)
```powershell
choco install instantdb
```

#### Windows (Winget)
```powershell
winget install instantdb
```

### Option 3: Docker

```bash
# Run InstantDB server in Docker
docker run -p 8080:8080 -p 50051:50051 instantdb/instantdb:latest

# With persistent data
docker run -p 8080:8080 -p 50051:50051 -v instantdb_data:/data instantdb/instantdb:latest

# Docker Compose
curl -O https://raw.githubusercontent.com/your-org/instantdb/main/docker-compose.yml
docker-compose up -d
```

### Option 4: Build from Source

See [Building from Source](#building-from-source) section below.

## 🎯 Quick Start for C# Developers

Once InstantDB is installed, here's how to integrate it with your C# project:

### 1. Initialize a New InstantDB Project

```bash
# Create a new C# project with InstantDB
mkdir MyRealtimeApp
cd MyRealtimeApp

# Initialize InstantDB project
instantdb init --lang csharp --template unity-game

# This creates:
# ├── instantdb.config.json     # Project configuration
# ├── modules/                  # WASM modules directory
# │   └── gamelogic/           # Sample C# module
# ├── schema.sql               # Database schema
# ├── scripts/                 # Build and deployment scripts
# └── client/                  # C# client code examples
```

### 2. Start the InstantDB Server

```bash
# Start server as daemon
instantdb server start --daemon

# Or start in foreground for development
instantdb server start --dev
```

### 3. Add InstantDB to Your C# Project

```bash
# Navigate to your existing C# project
cd path/to/your/csharp/project

# Add InstantDB NuGet package
dotnet add package InstantDB.Client

# Generate C# client code from your schema
instantdb generate --lang csharp --output ./Generated/InstantDB
```

### 4. Connect from C# Code

```csharp
using InstantDB.Client;

public class GameManager : MonoBehaviour
{
    private IInstantDBConnection _connection;

    async void Start()
    {
        // Connect to local InstantDB server
        _connection = new InstantDBConnection("ws://localhost:8080");
        await _connection.ConnectAsync();

        // Register event handlers
        _connection.OnPlayerInsert += HandlePlayerInsert;
        _connection.OnPlayerUpdate += HandlePlayerUpdate;
        _connection.OnPlayerDelete += HandlePlayerDelete;

        // Subscribe to real-time updates
        await _connection.SubscribeToTable("players");
    }

    private void HandlePlayerInsert(Player player)
    {
        Debug.Log($"New player: {player.Name}");
        // Spawn player in game world
    }

    private void HandlePlayerUpdate(Player oldPlayer, Player newPlayer)
    {
        Debug.Log($"Player updated: {newPlayer.Name}");
        // Update player in game world
    }

    private void HandlePlayerDelete(Player player)
    {
        Debug.Log($"Player left: {player.Name}");
        // Remove player from game world
    }
}
```

### 5. Define Your Schema

```sql
-- schema.sql
CREATE TABLE players (
    id INT64 PRIMARY KEY,
    name STRING NOT NULL,
    position_x FLOAT,
    position_y FLOAT,
    position_z FLOAT,
    health INT64 DEFAULT 100,
    last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE game_events (
    id INT64 PRIMARY KEY,
    player_id INT64,
    event_type STRING,
    data JSON,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

Apply the schema:
```bash
instantdb schema apply schema.sql
```

### 6. Deploy and Monitor

```bash
# Build and deploy modules
instantdb module build --all
instantdb module deploy gamelogic

# Monitor logs
instantdb logs --follow

# Check server status
instantdb status
```

## 🏗️ Building from Source

### Prerequisites

**System Requirements:**
- **CMake** 3.20 or higher
- **C++20** compatible compiler:
  - GCC 10+ (Linux)
  - Clang 12+ (macOS/Linux)
  - Visual Studio 2019+ (Windows)
- **Git**

**Dependencies (auto-fetched by CMake):**
- gRPC
- Protocol Buffers
- OpenSSL
- spdlog
- nlohmann/json
- fmt

### macOS Build

```bash
# Install build tools
brew install cmake protobuf grpc openssl

# Clone repository
git clone https://github.com/your-org/instantdb.git
cd instantdb

# Build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# Install locally
sudo make install

# Or create package
cpack
```

### Linux Build

#### Ubuntu/Debian
```bash
# Install dependencies
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    libprotobuf-dev \
    protobuf-compiler \
    libgrpc++-dev \
    libssl-dev \
    pkg-config

# Clone and build
git clone https://github.com/your-org/instantdb.git
cd instantdb
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Install
sudo make install

# Create DEB package
cpack -G DEB
```

#### CentOS/RHEL/Fedora
```bash
# Install dependencies
sudo yum groupinstall -y "Development Tools"
sudo yum install -y cmake3 git protobuf-devel grpc-devel openssl-devel

# Clone and build
git clone https://github.com/your-org/instantdb.git
cd instantdb
mkdir build && cd build
cmake3 .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Install
sudo make install

# Create RPM package
cpack -G RPM
```

### Windows Build

#### Visual Studio
```powershell
# Install tools via winget
winget install Microsoft.VisualStudio.2022.Community
winget install Kitware.CMake
winget install Git.Git

# Or use vcpkg for dependencies
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
.\vcpkg install grpc:x64-windows protobuf:x64-windows openssl:x64-windows

# Clone and build InstantDB
git clone https://github.com/your-org/instantdb.git
cd instantdb
mkdir build
cd build

# Configure with vcpkg
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows

# Build
cmake --build . --config Release

# Create installer
cpack -G NSIS
```

#### MSYS2/MinGW
```bash
# Install MSYS2, then in MSYS2 terminal:
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-grpc mingw-w64-x86_64-protobuf mingw-w64-x86_64-openssl

# Clone and build
git clone https://github.com/your-org/instantdb.git
cd instantdb
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
mingw32-make -j4

# Create ZIP package
cpack -G ZIP
```

## 🔧 Development Setup

### IDE Configuration

#### Visual Studio Code
```bash
# Install recommended extensions
code --install-extension ms-vscode.cpptools
code --install-extension ms-vscode.cmake-tools
code --install-extension ms-dotnettools.csharp

# Open project
code .
```

#### Visual Studio
```powershell
# Open the CMake project directly
devenv .
```

#### CLion
```bash
# Open the CMakeLists.txt file directly
```

### Environment Variables

Create a `.env` file for development:

```bash
# InstantDB Development Environment
INSTANTDB_DATA_DIR=./dev_data
INSTANTDB_LOG_LEVEL=debug
INSTANTDB_WS_PORT=8080
INSTANTDB_GRPC_PORT=50051

# Build configuration
CMAKE_BUILD_TYPE=Debug
CMAKE_GENERATOR="Unix Makefiles"  # or "Visual Studio 16 2019" on Windows
```

## 🐳 Container Setup

### Docker Development

```dockerfile
# Dockerfile.dev
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential cmake git \
    libprotobuf-dev protobuf-compiler \
    libgrpc++-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .

RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Debug && \
    make -j$(nproc)

EXPOSE 8080 50051
CMD ["./build/instantdb_server"]
```

Build and run:
```bash
docker build -f Dockerfile.dev -t instantdb:dev .
docker run -p 8080:8080 -p 50051:50051 instantdb:dev
```

### Production Container

```dockerfile
# Dockerfile
FROM ubuntu:22.04 as builder

RUN apt-get update && apt-get install -y \
    build-essential cmake git \
    libprotobuf-dev protobuf-compiler \
    libgrpc++-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    libprotobuf32 libgrpc++1.45 libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/instantdb_server /usr/local/bin/
COPY --from=builder /src/build/instantdb_sql /usr/local/bin/
COPY --from=builder /src/build/instantdb /usr/local/bin/

EXPOSE 8080 50051
VOLUME ["/data"]

CMD ["instantdb_server", "--data-dir", "/data"]
```

## 🔍 Troubleshooting

### Common Issues

#### Build Failures

**"CMake version too old"**
```bash
# Update CMake
pip install --upgrade cmake
# or
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | sudo apt-key add -
sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ focal main'
sudo apt update && sudo apt install cmake
```

**"gRPC not found"**
```bash
# On Ubuntu/Debian
sudo apt install libgrpc++-dev protobuf-compiler-grpc

# On macOS
brew install grpc protobuf

# On Windows with vcpkg
vcpkg install grpc:x64-windows
```

**"OpenSSL not found"**
```bash
# On Ubuntu/Debian
sudo apt install libssl-dev

# On macOS
brew install openssl
# Then add to CMake: -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl

# On Windows
vcpkg install openssl:x64-windows
```

#### Runtime Issues

**"Connection refused" when connecting**
```bash
# Check if server is running
instantdb status

# Check ports are open
netstat -tlnp | grep :8080
netstat -tlnp | grep :50051

# Check firewall
sudo ufw allow 8080
sudo ufw allow 50051
```

**"Permission denied" on Unix socket**
```bash
# Check data directory permissions
ls -la ./instantdb_data
sudo chown -R $USER:$USER ./instantdb_data
```

### Getting Help

- 📚 [Documentation](https://docs.instantdb.com)
- 💬 [Discord Community](https://discord.gg/instantdb)
- 🐛 [GitHub Issues](https://github.com/your-org/instantdb/issues)
- 📧 [Support Email](mailto:support@instantdb.com)

## 📋 Next Steps

After installation:

1. **Read the [CLI Guide](CLI_GUIDE.md)** for comprehensive command-line usage
2. **Check out [C# Examples](examples/csharp/)** for Unity and .NET integration
3. **Review the [API Documentation](docs/API.md)** for WebSocket and gRPC APIs
4. **Join the community** to share your projects and get help

---

**Happy building with InstantDB! 🚀**