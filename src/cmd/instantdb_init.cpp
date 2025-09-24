#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <map>
#include <vector>
#include <cstdlib>
#include <sstream>

namespace fs = std::filesystem;

// ANSI color codes
const std::string RESET = "\033[0m";
const std::string BOLD = "\033[1m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";

// File templates
struct FileTemplate {
    std::string path;
    std::string content;
};

// Project templates
std::map<std::string, std::vector<FileTemplate>> templates = {
    {"csharp", {
        {"Program.cs", R"(using InstantDB;

namespace {{PROJECT_NAME}}
{
    // Define your data models
    [Table("users")]
    public class User
    {
        [PrimaryKey]
        public long Id { get; set; }

        public string Name { get; set; }
        public string Email { get; set; }
        public long CreatedAt { get; set; }
    }

    [Table("posts")]
    public class Post
    {
        [PrimaryKey]
        public long Id { get; set; }

        [ForeignKey(typeof(User))]
        public long AuthorId { get; set; }

        public string Title { get; set; }
        public string Content { get; set; }
        public long CreatedAt { get; set; }
    }

    // Define your events
    [Event]
    public class UserCreatedEvent
    {
        public long UserId { get; set; }
        public string Name { get; set; }
        public long Timestamp { get; set; }
    }

    [Event]
    public class PostPublishedEvent
    {
        public long PostId { get; set; }
        public long AuthorId { get; set; }
        public string Title { get; set; }
        public long Timestamp { get; set; }
    }

    // Your business logic reducers (run server-side)
    public static class {{PROJECT_NAME}}Module
    {
        [Reducer]
        public static ReducerResult<long> CreateUser(string name, string email)
        {
            // Validate input
            if (string.IsNullOrEmpty(name))
                return ReducerResult<long>.Error("Name cannot be empty");

            if (string.IsNullOrEmpty(email))
                return ReducerResult<long>.Error("Email cannot be empty");

            // Generate unique ID
            var userId = GenerateId();
            var timestamp = Now();

            // Write to database (atomic within transaction)
            var user = new User
            {
                Id = userId,
                Name = name,
                Email = email,
                CreatedAt = timestamp
            };

            var writeResult = TableWrite("users", userId.ToString(), user);
            if (!writeResult.Success)
                return ReducerResult<long>.Error($"Failed to create user: {writeResult.Error}");

            // Emit event for real-time subscribers
            var evt = new UserCreatedEvent
            {
                UserId = userId,
                Name = name,
                Timestamp = timestamp
            };

            EmitEvent("user_created", userId.ToString(), evt);

            return ReducerResult<long>.Success(userId);
        }

        [Reducer]
        public static ReducerResult<long> CreatePost(long authorId, string title, string content)
        {
            // Validate user exists
            var userResult = TableRead<User>("users", authorId.ToString());
            if (!userResult.Success || userResult.Value == null)
                return ReducerResult<long>.Error("User not found");

            if (string.IsNullOrEmpty(title))
                return ReducerResult<long>.Error("Title cannot be empty");

            // Generate post ID
            var postId = GenerateId();
            var timestamp = Now();

            // Create post
            var post = new Post
            {
                Id = postId,
                AuthorId = authorId,
                Title = title,
                Content = content,
                CreatedAt = timestamp
            };

            var writeResult = TableWrite("posts", postId.ToString(), post);
            if (!writeResult.Success)
                return ReducerResult<long>.Error($"Failed to create post: {writeResult.Error}");

            // Emit event
            var evt = new PostPublishedEvent
            {
                PostId = postId,
                AuthorId = authorId,
                Title = title,
                Timestamp = timestamp
            };

            EmitEvent("post_published", postId.ToString(), evt);

            return ReducerResult<long>.Success(postId);
        }

        [Reducer]
        public static ReducerResult<Post[]> GetUserPosts(long userId)
        {
            // Validate user exists
            var userResult = TableRead<User>("users", userId.ToString());
            if (!userResult.Success || userResult.Value == null)
                return ReducerResult<Post[]>.Error("User not found");

            // Get user's posts via scan
            var postsResult = TableScan<Post>("posts", $"author:{userId}", 100);
            if (!postsResult.Success)
                return ReducerResult<Post[]>.Error($"Failed to get posts: {postsResult.Error}");

            return ReducerResult<Post[]>.Success(postsResult.Value);
        }

        [InitReducer]
        public static ReducerResult<string> OnModuleInit()
        {
            Log("{{PROJECT_NAME}} module initialized successfully");
            return ReducerResult<string>.Success("Module initialized");
        }

        [OnClientConnected]
        public static ReducerResult<string> OnClientConnected(string connectionId)
        {
            Log($"Client connected: {connectionId}");
            return ReducerResult<string>.Success("Welcome!");
        }

        [OnClientDisconnected]
        public static ReducerResult<string> OnClientDisconnected(string connectionId)
        {
            Log($"Client disconnected: {connectionId}");
            return ReducerResult<string>.Success("Goodbye!");
        }
    }
}
)"},
        {"{{PROJECT_NAME}}.csproj", R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Library</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <RootNamespace>{{PROJECT_NAME}}</RootNamespace>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <RuntimeIdentifier>wasm-wasi</RuntimeIdentifier>
    <PublishAot>true</PublishAot>
    <InvariantGlobalization>true</InvariantGlobalization>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
  </PropertyGroup>

  <ItemGroup>
    <PackageReference Include="InstantDB.WASM" Version="1.0.0" />
    <PackageReference Include="Microsoft.DotNet.ILCompiler.LLVM" Version="9.0.0-*" />
  </ItemGroup>

  <Target Name="PublishWasm" AfterTargets="Publish">
    <Exec Command="wizer --allow-wasi --wasm-bulk-memory true --dir . -o $(ProjectName).wasm $(PublishDir)$(ProjectName).wasm" ContinueOnError="false" />
  </Target>
</Project>
)"},
        {"instantdb.config.json", R"({
  "serverUrl": "http://localhost:8080",
  "moduleName": "{{PROJECT_NAME_LOWER}}",
  "tables": [
    {
      "name": "users",
      "columns": [
        {"name": "id", "type": "INTEGER", "primaryKey": true},
        {"name": "name", "type": "TEXT"},
        {"name": "email", "type": "TEXT"},
        {"name": "created_at", "type": "TIMESTAMP"}
      ]
    }
  ],
  "migrations": {
    "directory": "./migrations",
    "autoRun": true
  }
}
)"},
        {"README.md", R"(# {{PROJECT_NAME}}

An InstantDB WASM module written in C#.

## Overview

This module contains server-side business logic that runs inside the InstantDB server as WebAssembly. Your reducers execute at database speed with full transactional guarantees.

## Getting Started

1. **Install .NET 8 and WASM tools:**
   ```bash
   dotnet workload install wasm-tools
   ```

2. **Restore packages:**
   ```bash
   dotnet restore
   ```

3. **Build and publish the WASM module:**
   ```bash
   dotnet publish --configuration Release
   ```

4. **Deploy to InstantDB server:**
   ```bash
   instantdb publish --server=http://localhost:9090
   ```

5. **Test your reducers:**
   ```bash
   # Execute a reducer via gRPC
   grpcurl -plaintext -d '{
     "module_name": "{{PROJECT_NAME_LOWER}}",
     "reducer_name": "CreateUser",
     "sender_identity": "user123",
     "args": [
       {"string_value": "Alice"},
       {"string_value": "alice@example.com"}
     ]
   }' localhost:50051 instantdb.grpc.WasmService.ExecuteReducer
   ```

## Project Structure

- `Program.cs` - Your reducers, tables, and events
- `{{PROJECT_NAME}}.csproj` - WASM compilation configuration
- `instantdb.config.json` - Module metadata
- `build.sh` - Build automation script

## How It Works

1. **Tables** - Define your data structures with `[Table]` attributes
2. **Reducers** - Server-side functions with `[Reducer]` attributes
3. **Events** - Real-time notifications with `[Event]` attributes
4. **Compilation** - C# → .NET IL → WASM bytecode
5. **Deployment** - Upload .wasm file to InstantDB server
6. **Execution** - Reducers run inside server with database access

## Example Usage

```csharp
// Call reducers from clients via gRPC/WebSocket
var result = await client.CallReducer("CreateUser", "Alice", "alice@example.com");
var posts = await client.CallReducer("GetUserPosts", userId);
```

## Documentation

- [InstantDB WASM Modules](https://docs.instantdb.com/wasm)
- [C# WASM SDK](https://docs.instantdb.com/csharp-wasm)
)"},
        {".gitignore", R"(bin/
obj/
.vs/
*.user
*.suo
.DS_Store
instantdb_data/
logs/
*.wasm
)"},
        {"build.sh", R"BUILDSCRIPT(#!/bin/bash
set -e

echo "🔨 Building {{PROJECT_NAME}} WASM module..."

# Check dependencies
if ! command -v dotnet &> /dev/null; then
    echo "❌ .NET SDK not found. Please install .NET 8+."
    exit 1
fi

# Check WASM workload
if ! dotnet workload list | grep -q "wasm-tools"; then
    echo "📦 Installing WASM tools workload..."
    dotnet workload install wasm-tools
fi

# Restore packages
echo "📦 Restoring packages..."
dotnet restore

# Build in Release mode
echo "🏗️  Building WASM module..."
dotnet publish --configuration Release --verbosity quiet

# Check if WASM file was created
WASM_FILE="bin/Release/net8.0/wasm-wasi/publish/{{PROJECT_NAME}}.wasm"
if [ -f "$WASM_FILE" ]; then
    echo "✅ WASM module built successfully: $WASM_FILE"
    echo "📊 Size: $(ls -lh "$WASM_FILE" | awk '{print $5}')"
    echo ""
    echo "🚀 To deploy: instantdb publish --server=http://localhost:9090"
else
    echo "❌ WASM build failed - no output file found"
    exit 1
fi
)BUILDSCRIPT"},
        {"build.bat", R"(@echo off
setlocal enabledelayedexpansion

echo 🔨 Building {{PROJECT_NAME}} WASM module...

REM Check dependencies
dotnet --version >nul 2>&1
if errorlevel 1 (
    echo ❌ .NET SDK not found. Please install .NET 8+.
    exit /b 1
)

REM Check WASM workload
dotnet workload list | findstr "wasm-tools" >nul
if errorlevel 1 (
    echo 📦 Installing WASM tools workload...
    dotnet workload install wasm-tools
)

REM Restore packages
echo 📦 Restoring packages...
dotnet restore

REM Build in Release mode
echo 🏗️  Building WASM module...
dotnet publish --configuration Release --verbosity quiet

REM Check if WASM file was created
set WASM_FILE=bin\Release\net8.0\wasm-wasi\publish\{{PROJECT_NAME}}.wasm
if exist "%WASM_FILE%" (
    echo ✅ WASM module built successfully: %WASM_FILE%
    echo.
    echo 🚀 To deploy: instantdb publish --server=http://localhost:9090
) else (
    echo ❌ WASM build failed - no output file found
    exit /b 1
)
)"}
    }},

    {"unity", {
        {"Assets/Scripts/GameManager.cs", R"(using UnityEngine;
using InstantDB.Unity;
using InstantDB.Client;
using System.Threading.Tasks;

public class GameManager : MonoBehaviour
{
    [SerializeField] private InstantDBConfig config;
    private IInstantDBConnection connection;

    async void Start()
    {
        Debug.Log("Initializing InstantDB connection...");

        // Get or create network manager
        if (InstantDBNetworkManager.Instance == null)
        {
            var managerObj = new GameObject("InstantDB Network Manager");
            managerObj.AddComponent<InstantDBNetworkManager>();
        }

        // Create connection
        connection = InstantDBNetworkManager.Instance.CreateDefaultConnection();

        // Subscribe to events
        connection.OnConnected += OnConnected;
        connection.OnDisconnected += OnDisconnected;
        connection.OnPlayerInsert += OnPlayerJoined;
        connection.OnPlayerUpdate += OnPlayerUpdated;
        connection.OnPlayerDelete += OnPlayerLeft;

        // Connect
        await ConnectToServer();
    }

    async Task ConnectToServer()
    {
        try
        {
            await connection.ConnectAsync();
            await connection.SubscribeToTable("players");
            Debug.Log("Connected to InstantDB!");
        }
        catch (System.Exception ex)
        {
            Debug.LogError($"Failed to connect: {ex.Message}");
        }
    }

    void OnConnected()
    {
        Debug.Log("✅ Connected to InstantDB");
    }

    void OnDisconnected(System.Exception ex)
    {
        Debug.LogWarning($"Disconnected from InstantDB: {ex?.Message}");
    }

    void OnPlayerJoined(Player player)
    {
        Debug.Log($"Player joined: {player.Name}");
        // Spawn player GameObject
    }

    void OnPlayerUpdated(Player oldPlayer, Player newPlayer)
    {
        Debug.Log($"Player updated: {newPlayer.Name}");
        // Update player GameObject
    }

    void OnPlayerLeft(Player player)
    {
        Debug.Log($"Player left: {player.Name}");
        // Remove player GameObject
    }

    void OnDestroy()
    {
        connection?.Disconnect();
    }
}
)"},
        {"Assets/InstantDBConfig.asset.meta", R"(fileFormatVersion: 2
guid: YOUR_GUID_HERE
NativeFormatImporter:
  externalObjects: {}
  mainObjectFileID: 11400000
  userData:
  assetBundleName:
  assetBundleVariant:
)"},
        {"instantdb.config.json", R"({
  "serverUrl": "http://localhost:8080",
  "moduleName": "{{PROJECT_NAME_LOWER}}",
  "unity": {
    "autoConnect": true,
    "debugLogging": true
  },
  "tables": [
    {
      "name": "players",
      "columns": [
        {"name": "id", "type": "INTEGER", "primaryKey": true},
        {"name": "name", "type": "TEXT"},
        {"name": "position_x", "type": "REAL"},
        {"name": "position_y", "type": "REAL"},
        {"name": "position_z", "type": "REAL"},
        {"name": "health", "type": "INTEGER"},
        {"name": "score", "type": "INTEGER"}
      ]
    },
    {
      "name": "game_sessions",
      "columns": [
        {"name": "id", "type": "INTEGER", "primaryKey": true},
        {"name": "name", "type": "TEXT"},
        {"name": "max_players", "type": "INTEGER"},
        {"name": "current_players", "type": "INTEGER"},
        {"name": "status", "type": "TEXT"}
      ]
    }
  ]
}
)"},
        {"README.md", R"(# {{PROJECT_NAME}}

An InstantDB Unity multiplayer game project.

## Getting Started

1. Install the InstantDB Unity package:
   ```bash
   # In Unity Package Manager, add from git URL:
   https://github.com/instantdb/instantdb-unity.git
   ```

2. Start the InstantDB server:
   ```bash
   instantdb server
   ```

3. Open the project in Unity

4. Configure the InstantDB settings in the Inspector

5. Play the scene!

## Project Structure

- `Assets/Scripts/GameManager.cs` - Main game manager with InstantDB integration
- `Assets/InstantDBConfig.asset` - Connection configuration (create via menu)
- `instantdb.config.json` - Server-side configuration

## Creating the Config Asset

1. Right-click in Project window
2. Create → InstantDB → Configuration
3. Configure your server settings
4. Assign to GameManager

## Documentation

- [InstantDB Unity Guide](https://docs.instantdb.com/unity)
- [API Reference](https://docs.instantdb.com/api)
)"},
        {".gitignore", R"(# Unity
[Ll]ibrary/
[Tt]emp/
[Oo]bj/
[Bb]uild/
[Bb]uilds/
[Ll]ogs/
[Uu]ser[Ss]ettings/

# Visual Studio / MonoDevelop
.vs/
*.csproj
*.sln
*.suo
*.user
*.userprefs
*.pidb
*.booproj

# OS
.DS_Store
Thumbs.db

# InstantDB
instantdb_data/
logs/
)"}
    }},

    {"nodejs", {
        {"index.js", R"DELIMITER(const { InstantDBClient } = require('@instantdb/client');

async function main() {
  console.log('🚀 Connecting to InstantDB...');

  // Create client
  const client = new InstantDBClient({
    serverUrl: 'http://localhost:8080',
    moduleName: '{{PROJECT_NAME_LOWER}}'
  });

  // Connect to server
  await client.connect();
  console.log('✅ Connected to InstantDB!');

  // Subscribe to real-time updates
  await client.subscribeToTable('users');

  client.on('dataChange', (event) => {
    console.log(`Data changed: ${event.table} - ${event.operation}`);
    console.log('Data:', event.data);
  });

  // Execute a query
  const result = await client.sql('SELECT * FROM users');
  console.log(`Query returned ${result.rows.length} rows`);

  // Insert data
  await client.sql("INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com')");

  // Keep the process alive
  process.stdin.resume();
  console.log('Press Ctrl+C to exit...');
}

main().catch(console.error);
)DELIMITER"},
        {"package.json", R"({
  "name": "{{PROJECT_NAME_LOWER}}",
  "version": "1.0.0",
  "description": "An InstantDB Node.js project",
  "main": "index.js",
  "scripts": {
    "start": "node index.js",
    "dev": "nodemon index.js"
  },
  "dependencies": {
    "@instantdb/client": "^1.0.0"
  },
  "devDependencies": {
    "nodemon": "^3.0.0"
  }
}
)"},
        {"instantdb.config.json", R"({
  "serverUrl": "http://localhost:8080",
  "moduleName": "{{PROJECT_NAME_LOWER}}",
  "tables": [
    {
      "name": "users",
      "columns": [
        {"name": "id", "type": "INTEGER", "primaryKey": true},
        {"name": "name", "type": "TEXT"},
        {"name": "email", "type": "TEXT"},
        {"name": "created_at", "type": "TIMESTAMP"}
      ]
    }
  ]
}
)"},
        {"README.md", R"(# {{PROJECT_NAME}}

An InstantDB Node.js project.

## Getting Started

1. Install dependencies:
   ```bash
   npm install
   ```

2. Start the InstantDB server:
   ```bash
   instantdb server
   ```

3. Run the application:
   ```bash
   npm start
   ```

## Documentation

- [InstantDB Documentation](https://docs.instantdb.com)
- [Node.js Client SDK](https://docs.instantdb.com/nodejs)
)"},
        {".gitignore", R"(node_modules/
.env
.DS_Store
instantdb_data/
logs/
)"}
    }}
};

void printUsage() {
    std::cout << BOLD << "Usage:" << RESET << " instantdb init [PROJECT_NAME] [OPTIONS]\n\n";
    std::cout << BOLD << "Options:\n" << RESET;
    std::cout << "  --template TEMPLATE  Project template (default: csharp)\n";
    std::cout << "                       Available: csharp, unity, nodejs\n";
    std::cout << "  --force             Overwrite existing files\n";
    std::cout << "  --no-git            Don't initialize git repository\n";
    std::cout << "  -h, --help          Show this help message\n\n";
    std::cout << BOLD << "Examples:\n" << RESET;
    std::cout << "  instantdb init myproject                    # C# project\n";
    std::cout << "  instantdb init mygame --template unity      # Unity project\n";
    std::cout << "  instantdb init myapp --template nodejs      # Node.js project\n";
}

std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

bool createFile(const fs::path& filepath, const std::string& content, bool force = false) {
    // Create parent directories
    fs::create_directories(filepath.parent_path());

    // Check if file exists
    if (fs::exists(filepath) && !force) {
        std::cout << YELLOW << "  [SKIP]" << RESET << " " << filepath.string() << " (already exists)\n";
        return true;
    }

    // Write file
    std::ofstream file(filepath);
    if (!file) {
        std::cerr << RED << "  [ERROR]" << RESET << " Failed to create " << filepath.string() << "\n";
        return false;
    }

    file << content;
    file.close();

    std::cout << GREEN << "  [CREATE]" << RESET << " " << filepath.string() << "\n";
    return true;
}

bool initGitRepo(const fs::path& projectPath) {
    std::cout << "\n" << CYAN << "Initializing git repository..." << RESET << "\n";

    std::string cmd = "cd \"" + projectPath.string() + "\" && git init -q && git add . && git commit -q -m \"Initial commit\"";
    int result = std::system(cmd.c_str());

    if (result == 0) {
        std::cout << GREEN << "  ✓ Git repository initialized" << RESET << "\n";
        return true;
    } else {
        std::cout << YELLOW << "  ⚠ Could not initialize git repository" << RESET << "\n";
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Parse arguments
    if (argc < 2) {
        std::cerr << RED << "Error: Project name required" << RESET << "\n\n";
        printUsage();
        return 1;
    }

    std::string projectName;
    std::string templateName = "csharp";
    bool force = false;
    bool initGit = true;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "--template") {
            if (i + 1 < argc) {
                templateName = argv[++i];
            }
        } else if (arg.substr(0, 11) == "--template=") {
            templateName = arg.substr(11);
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--no-git") {
            initGit = false;
        } else if (projectName.empty()) {
            projectName = arg;
        }
    }

    // Validate template
    if (templates.find(templateName) == templates.end()) {
        std::cerr << RED << "Error: Unknown template '" << templateName << "'" << RESET << "\n";
        std::cerr << "Available templates: csharp, unity, nodejs\n";
        return 1;
    }

    // Create project directory
    fs::path projectPath = fs::current_path() / projectName;

    if (fs::exists(projectPath) && !force) {
        std::cerr << RED << "Error: Directory '" << projectName << "' already exists" << RESET << "\n";
        std::cerr << "Use --force to overwrite existing files\n";
        return 1;
    }

    std::cout << CYAN << BOLD << "\n🚀 Initializing InstantDB " << templateName << " project: "
              << projectName << RESET << "\n\n";

    // Create project directory
    fs::create_directories(projectPath);

    // Get template files
    const auto& templateFiles = templates[templateName];
    std::string projectNameLower = toLower(projectName);

    // Create files from template
    for (const auto& fileTemplate : templateFiles) {
        // Replace placeholders in path and content
        std::string filePath = replaceAll(fileTemplate.path, "{{PROJECT_NAME}}", projectName);
        std::string content = replaceAll(fileTemplate.content, "{{PROJECT_NAME}}", projectName);
        content = replaceAll(content, "{{PROJECT_NAME_LOWER}}", projectNameLower);

        // Create file
        fs::path fullPath = projectPath / filePath;
        if (!createFile(fullPath, content, force)) {
            std::cerr << RED << "Failed to create project files" << RESET << "\n";
            return 1;
        }
    }

    // Create additional directories
    if (templateName == "unity") {
        fs::create_directories(projectPath / "Assets" / "Scripts");
        fs::create_directories(projectPath / "Assets" / "Prefabs");
        fs::create_directories(projectPath / "Assets" / "Materials");
    }

    // Initialize git repository
    if (initGit) {
        initGitRepo(projectPath);
    }

    // Success message
    std::cout << "\n" << GREEN << BOLD << "✅ Project created successfully!" << RESET << "\n\n";

    std::cout << BOLD << "Next steps:" << RESET << "\n";
    std::cout << "  1. Navigate to your project:\n";
    std::cout << "     " << BLUE << "cd " << projectName << RESET << "\n";

    if (templateName == "csharp") {
        std::cout << "  2. Restore packages:\n";
        std::cout << "     " << BLUE << "dotnet restore" << RESET << "\n";
        std::cout << "  3. Start the InstantDB server:\n";
        std::cout << "     " << BLUE << "instantdb server" << RESET << "\n";
        std::cout << "  4. Run your application:\n";
        std::cout << "     " << BLUE << "dotnet run" << RESET << "\n";
    } else if (templateName == "unity") {
        std::cout << "  2. Open the project in Unity\n";
        std::cout << "  3. Install the InstantDB Unity package\n";
        std::cout << "  4. Start the InstantDB server:\n";
        std::cout << "     " << BLUE << "instantdb server" << RESET << "\n";
        std::cout << "  5. Play the scene in Unity\n";
    } else if (templateName == "nodejs") {
        std::cout << "  2. Install dependencies:\n";
        std::cout << "     " << BLUE << "npm install" << RESET << "\n";
        std::cout << "  3. Start the InstantDB server:\n";
        std::cout << "     " << BLUE << "instantdb server" << RESET << "\n";
        std::cout << "  4. Run your application:\n";
        std::cout << "     " << BLUE << "npm start" << RESET << "\n";
    }

    std::cout << "\n" << BOLD << "Happy coding! 🎉" << RESET << "\n\n";

    return 0;
}