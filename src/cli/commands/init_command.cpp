#include "cli/commands/init_command.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>

namespace instantdb::cli {

int InitCommand::Execute(const std::vector<std::string>& args) {
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintHelp();
        return 0;
    }

    auto positional = GetPositionalArgs(args);
    if (positional.empty()) {
        spdlog::error("Project name is required");
        PrintHelp();
        return 1;
    }

    std::string name = positional[0];
    std::string lang = GetOption(args, "--lang");
    if (lang.empty()) {
        lang = GetOption(args, "-l");
    }
    if (lang.empty()) {
        lang = "rust"; // Default language
    }

    return InitProject(name, lang);
}

void InitCommand::PrintHelp() const {
    std::cout << "Usage: instantdb init [options] <project-name>\n\n";
    std::cout << "Initialize a new InstantDB project with WASM modules\n\n";
    std::cout << "Arguments:\n";
    std::cout << "  <project-name>            Name of the project to create\n\n";
    std::cout << "Options:\n";
    std::cout << "  --lang, -l <language>     Programming language for WASM modules\n";
    std::cout << "                            Options: rust, csharp, javascript, go, cpp\n";
    std::cout << "                            Default: rust\n";
    std::cout << "  --help, -h                Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  instantdb init my_project\n";
    std::cout << "  instantdb init --lang csharp my_server_module\n";
    std::cout << "  instantdb init -l javascript chat_app\n";
}

int InitCommand::InitProject(const std::string& name, const std::string& lang) {
    namespace fs = std::filesystem;

    // Validate language
    if (lang != "rust" && lang != "csharp" && lang != "javascript" && lang != "go" && lang != "cpp") {
        spdlog::error("Unsupported language: {}. Supported: rust, csharp, javascript, go, cpp", lang);
        return 1;
    }

    // Check if directory already exists
    if (fs::exists(name)) {
        spdlog::error("Directory '{}' already exists", name);
        return 1;
    }

    try {
        spdlog::info("Creating InstantDB project '{}' with {} modules...", name, lang);

        // Create project directory
        fs::create_directories(name);

        CreateProjectStructure(name, lang);
        CreateConfigFile(name);
        CreateWasmModule(name, lang);

        spdlog::info("Project '{}' created successfully!", name);
        spdlog::info("Next steps:");
        spdlog::info("  cd {}", name);
        spdlog::info("  instantdb module build");
        spdlog::info("  instantdb server start");

        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Failed to create project: {}", e.what());
        return 1;
    }
}

void InitCommand::CreateProjectStructure(const std::string& path, const std::string& lang) {
    namespace fs = std::filesystem;

    // Create basic directory structure
    fs::create_directories(path + "/modules");
    fs::create_directories(path + "/data");
    fs::create_directories(path + "/logs");
    fs::create_directories(path + "/config");

    // Create .gitignore
    std::ofstream gitignore(path + "/.gitignore");
    gitignore << "# InstantDB\n";
    gitignore << "data/\n";
    gitignore << "logs/\n";
    gitignore << "*.wasm\n";
    gitignore << "build/\n";
    gitignore << "\n";
    gitignore << "# Language-specific\n";
    if (lang == "rust") {
        gitignore << "target/\n";
        gitignore << "Cargo.lock\n";
    } else if (lang == "csharp") {
        gitignore << "bin/\n";
        gitignore << "obj/\n";
        gitignore << "*.dll\n";
    } else if (lang == "javascript") {
        gitignore << "node_modules/\n";
        gitignore << "package-lock.json\n";
    } else if (lang == "go") {
        gitignore << "go.sum\n";
    }
    gitignore.close();

    // Create README
    std::ofstream readme(path + "/README.md");
    readme << "# " << path << "\n\n";
    readme << "InstantDB project with " << lang << " WASM modules.\n\n";
    readme << "## Getting Started\n\n";
    readme << "1. Build the WASM modules:\n";
    readme << "   ```bash\n";
    readme << "   instantdb module build\n";
    readme << "   ```\n\n";
    readme << "2. Start the server:\n";
    readme << "   ```bash\n";
    readme << "   instantdb server start\n";
    readme << "   ```\n\n";
    readme << "3. Deploy modules:\n";
    readme << "   ```bash\n";
    readme << "   instantdb module deploy\n";
    readme << "   ```\n\n";
    readme << "## Project Structure\n\n";
    readme << "- `modules/` - WASM module source code\n";
    readme << "- `config/` - Configuration files\n";
    readme << "- `data/` - Database files (ignored by git)\n";
    readme << "- `logs/` - Log files (ignored by git)\n";
    readme.close();
}

void InitCommand::CreateConfigFile(const std::string& path) {
    std::ofstream config(path + "/config/instantdb.toml");
    config << "[server]\n";
    config << "port = 8080\n";
    config << "websocket_port = 8085\n";
    config << "grpc_port = 50051\n";
    config << "data_dir = \"./data\"\n";
    config << "log_level = \"info\"\n";
    config << "log_file = \"./logs/instantdb.log\"\n\n";
    config << "[database]\n";
    config << "name = \"" << path << "\"\n";
    config << "wal_enabled = true\n";
    config << "max_connections = 100\n\n";
    config << "[wasm]\n";
    config << "modules_dir = \"./modules\"\n";
    config << "max_memory = \"128MB\"\n";
    config << "timeout = \"30s\"\n";
    config.close();
}

void InitCommand::CreateWasmModule(const std::string& path, const std::string& lang) {
    std::string module_path = path + "/modules/main";

    if (lang == "rust") {
        CreateRustModule(module_path);
    } else if (lang == "csharp") {
        CreateCSharpModule(module_path);
    } else if (lang == "javascript") {
        CreateJavaScriptModule(module_path);
    } else if (lang == "go") {
        CreateGoModule(module_path);
    } else if (lang == "cpp") {
        CreateCppModule(module_path);
    }
}

void InitCommand::CreateRustModule(const std::string& path) {
    namespace fs = std::filesystem;
    fs::create_directories(path + "/src");

    // Cargo.toml
    std::ofstream cargo(path + "/Cargo.toml");
    cargo << "[package]\n";
    cargo << "name = \"main\"\n";
    cargo << "version = \"0.1.0\"\n";
    cargo << "edition = \"2021\"\n\n";
    cargo << "[lib]\n";
    cargo << "crate-type = [\"cdylib\"]\n\n";
    cargo << "[dependencies]\n";
    cargo << "wasm-bindgen = \"0.2\"\n";
    cargo << "serde = { version = \"1.0\", features = [\"derive\"] }\n";
    cargo << "serde_json = \"1.0\"\n\n";
    cargo << "[dependencies.web-sys]\n";
    cargo << "version = \"0.3\"\n";
    cargo << "features = [\"console\"]\n";
    cargo.close();

    // src/lib.rs
    std::ofstream lib(path + "/src/lib.rs");
    lib << "use wasm_bindgen::prelude::*;\n";
    lib << "use serde::{Deserialize, Serialize};\n\n";
    lib << "#[derive(Serialize, Deserialize)]\n";
    lib << "pub struct User {\n";
    lib << "    pub id: u64,\n";
    lib << "    pub name: String,\n";
    lib << "    pub email: String,\n";
    lib << "}\n\n";
    lib << "#[wasm_bindgen]\n";
    lib << "pub fn create_user(data: &str) -> String {\n";
    lib << "    let user: User = serde_json::from_str(data).unwrap();\n";
    lib << "    // Add user creation logic here\n";
    lib << "    serde_json::to_string(&user).unwrap()\n";
    lib << "}\n\n";
    lib << "#[wasm_bindgen]\n";
    lib << "pub fn get_user(id: u64) -> String {\n";
    lib << "    // Add user retrieval logic here\n";
    lib << "    let user = User {\n";
    lib << "        id,\n";
    lib << "        name: \"Example User\".to_string(),\n";
    lib << "        email: \"user@example.com\".to_string(),\n";
    lib << "    };\n";
    lib << "    serde_json::to_string(&user).unwrap()\n";
    lib << "}\n";
    lib.close();

    // Build script
    std::ofstream build(path + "/build.sh");
    build << "#!/bin/bash\n";
    build << "cargo build --target wasm32-unknown-unknown --release\n";
    build << "cp target/wasm32-unknown-unknown/release/main.wasm ../main.wasm\n";
    build.close();
    std::filesystem::permissions(path + "/build.sh",
                                std::filesystem::perms::owner_exec,
                                std::filesystem::perm_options::add);
}

void InitCommand::CreateCSharpModule(const std::string& path) {
    namespace fs = std::filesystem;
    fs::create_directories(path);

    // Project file
    std::ofstream csproj(path + "/main.csproj");
    csproj << "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
    csproj << "  <PropertyGroup>\n";
    csproj << "    <TargetFramework>net8.0</TargetFramework>\n";
    csproj << "    <OutputType>Library</OutputType>\n";
    csproj << "    <PublishAot>true</PublishAot>\n";
    csproj << "    <NativeLib>Shared</NativeLib>\n";
    csproj << "  </PropertyGroup>\n";
    csproj << "  <ItemGroup>\n";
    csproj << "    <PackageReference Include=\"System.Text.Json\" Version=\"8.0.0\" />\n";
    csproj << "  </ItemGroup>\n";
    csproj << "</Project>\n";
    csproj.close();

    // Source file
    std::ofstream cs(path + "/Program.cs");
    cs << "using System.Runtime.InteropServices;\n";
    cs << "using System.Text.Json;\n\n";
    cs << "public class User\n";
    cs << "{\n";
    cs << "    public ulong Id { get; set; }\n";
    cs << "    public string Name { get; set; } = \"\";\n";
    cs << "    public string Email { get; set; } = \"\";\n";
    cs << "}\n\n";
    cs << "public static class Module\n";
    cs << "{\n";
    cs << "    [UnmanagedCallersOnly(EntryPoint = \"create_user\")]\n";
    cs << "    public static IntPtr CreateUser(IntPtr dataPtr, int dataLen)\n";
    cs << "    {\n";
    cs << "        var data = Marshal.PtrToStringUTF8(dataPtr, dataLen);\n";
    cs << "        var user = JsonSerializer.Deserialize<User>(data!);\n";
    cs << "        // Add user creation logic here\n";
    cs << "        var result = JsonSerializer.Serialize(user);\n";
    cs << "        return Marshal.StringToHGlobalAnsi(result);\n";
    cs << "    }\n\n";
    cs << "    [UnmanagedCallersOnly(EntryPoint = \"get_user\")]\n";
    cs << "    public static IntPtr GetUser(ulong id)\n";
    cs << "    {\n";
    cs << "        // Add user retrieval logic here\n";
    cs << "        var user = new User { Id = id, Name = \"Example User\", Email = \"user@example.com\" };\n";
    cs << "        var result = JsonSerializer.Serialize(user);\n";
    cs << "        return Marshal.StringToHGlobalAnsi(result);\n";
    cs << "    }\n";
    cs << "}\n";
    cs.close();

    // Build script
    std::ofstream build(path + "/build.sh");
    build << "#!/bin/bash\n";
    build << "dotnet publish -c Release -r wasm-wasi -o output\n";
    build << "cp output/main.wasm ../main.wasm\n";
    build.close();
    std::filesystem::permissions(path + "/build.sh",
                                std::filesystem::perms::owner_exec,
                                std::filesystem::perm_options::add);
}

void InitCommand::CreateJavaScriptModule(const std::string& path) {
    namespace fs = std::filesystem;
    fs::create_directories(path);

    // package.json
    std::ofstream package(path + "/package.json");
    package << "{\n";
    package << "  \"name\": \"main\",\n";
    package << "  \"version\": \"1.0.0\",\n";
    package << "  \"type\": \"module\",\n";
    package << "  \"scripts\": {\n";
    package << "    \"build\": \"node build.js\"\n";
    package << "  },\n";
    package << "  \"devDependencies\": {\n";
    package << "    \"@assemblyscript/loader\": \"^0.27.0\",\n";
    package << "    \"assemblyscript\": \"^0.27.0\"\n";
    package << "  }\n";
    package << "}\n";
    package.close();

    // AssemblyScript config
    std::ofstream asconfig(path + "/asconfig.json");
    asconfig << "{\n";
    asconfig << "  \"targets\": {\n";
    asconfig << "    \"release\": {\n";
    asconfig << "      \"outFile\": \"main.wasm\",\n";
    asconfig << "      \"textFile\": \"main.wat\",\n";
    asconfig << "      \"sourceMap\": false,\n";
    asconfig << "      \"optimizeLevel\": 3,\n";
    asconfig << "      \"shrinkLevel\": 1\n";
    asconfig << "    }\n";
    asconfig << "  }\n";
    asconfig << "}\n";
    asconfig.close();

    // Source file
    std::ofstream ts(path + "/index.ts");
    ts << "export class User {\n";
    ts << "  constructor(\n";
    ts << "    public id: u64,\n";
    ts << "    public name: string,\n";
    ts << "    public email: string\n";
    ts << "  ) {}\n";
    ts << "}\n\n";
    ts << "export function createUser(data: string): string {\n";
    ts << "  const user = JSON.parse<User>(data);\n";
    ts << "  // Add user creation logic here\n";
    ts << "  return JSON.stringify(user);\n";
    ts << "}\n\n";
    ts << "export function getUser(id: u64): string {\n";
    ts << "  // Add user retrieval logic here\n";
    ts << "  const user = new User(id, \"Example User\", \"user@example.com\");\n";
    ts << "  return JSON.stringify(user);\n";
    ts << "}\n";
    ts.close();

    // Build script
    std::ofstream build(path + "/build.sh");
    build << "#!/bin/bash\n";
    build << "npm install\n";
    build << "npx asc index.ts --target release\n";
    build << "cp main.wasm ../main.wasm\n";
    build.close();
    std::filesystem::permissions(path + "/build.sh",
                                std::filesystem::perms::owner_exec,
                                std::filesystem::perm_options::add);
}

void InitCommand::CreateGoModule(const std::string& path) {
    namespace fs = std::filesystem;
    fs::create_directories(path);

    // go.mod
    std::ofstream mod(path + "/go.mod");
    mod << "module main\n\n";
    mod << "go 1.21\n";
    mod.close();

    // Source file
    std::ofstream go(path + "/main.go");
    go << "package main\n\n";
    go << "import (\n";
    go << "    \"encoding/json\"\n";
    go << "    \"unsafe\"\n";
    go << ")\n\n";
    go << "type User struct {\n";
    go << "    ID    uint64 `json:\"id\"`\n";
    go << "    Name  string `json:\"name\"`\n";
    go << "    Email string `json:\"email\"`\n";
    go << "}\n\n";
    go << "//export create_user\n";
    go << "func create_user(data *byte, length int) *byte {\n";
    go << "    dataSlice := unsafe.Slice(data, length)\n";
    go << "    var user User\n";
    go << "    json.Unmarshal(dataSlice, &user)\n";
    go << "    // Add user creation logic here\n";
    go << "    result, _ := json.Marshal(user)\n";
    go << "    return &result[0]\n";
    go << "}\n\n";
    go << "//export get_user\n";
    go << "func get_user(id uint64) *byte {\n";
    go << "    // Add user retrieval logic here\n";
    go << "    user := User{ID: id, Name: \"Example User\", Email: \"user@example.com\"}\n";
    go << "    result, _ := json.Marshal(user)\n";
    go << "    return &result[0]\n";
    go << "}\n\n";
    go << "func main() {}\n";
    go.close();

    // Build script
    std::ofstream build(path + "/build.sh");
    build << "#!/bin/bash\n";
    build << "GOOS=wasip1 GOARCH=wasm go build -o main.wasm main.go\n";
    build << "cp main.wasm ../main.wasm\n";
    build.close();
    std::filesystem::permissions(path + "/build.sh",
                                std::filesystem::perms::owner_exec,
                                std::filesystem::perm_options::add);
}

void InitCommand::CreateCppModule(const std::string& path) {
    namespace fs = std::filesystem;
    fs::create_directories(path);

    // CMakeLists.txt
    std::ofstream cmake(path + "/CMakeLists.txt");
    cmake << "cmake_minimum_required(VERSION 3.16)\n";
    cmake << "project(main)\n\n";
    cmake << "set(CMAKE_CXX_STANDARD 20)\n";
    cmake << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    cmake << "add_executable(main.wasm main.cpp)\n\n";
    cmake << "target_compile_options(main.wasm PRIVATE\n";
    cmake << "    -fno-exceptions\n";
    cmake << "    -fno-rtti\n";
    cmake << "    -Oz\n";
    cmake << ")\n\n";
    cmake << "target_link_options(main.wasm PRIVATE\n";
    cmake << "    -Wl,--export-all\n";
    cmake << "    -Wl,--no-entry\n";
    cmake << "    -Wl,--allow-undefined\n";
    cmake << ")\n";
    cmake.close();

    // Source file
    std::ofstream cpp(path + "/main.cpp");
    cpp << "#include <string>\n";
    cpp << "#include <cstring>\n\n";
    cpp << "struct User {\n";
    cpp << "    uint64_t id;\n";
    cpp << "    std::string name;\n";
    cpp << "    std::string email;\n";
    cpp << "};\n\n";
    cpp << "extern \"C\" {\n\n";
    cpp << "const char* create_user(const char* data, int length) {\n";
    cpp << "    // Simple JSON parsing - in real implementation use a JSON library\n";
    cpp << "    // Add user creation logic here\n";
    cpp << "    static std::string result = \"{\\\"id\\\":1,\\\"name\\\":\\\"Example User\\\",\\\"email\\\":\\\"user@example.com\\\"}\";\n";
    cpp << "    return result.c_str();\n";
    cpp << "}\n\n";
    cpp << "const char* get_user(uint64_t id) {\n";
    cpp << "    // Add user retrieval logic here\n";
    cpp << "    static std::string result = \"{\\\"id\\\":\" + std::to_string(id) + \",\\\"name\\\":\\\"Example User\\\",\\\"email\\\":\\\"user@example.com\\\"}\";\n";
    cpp << "    return result.c_str();\n";
    cpp << "}\n\n";
    cpp << "}\n";
    cpp.close();

    // Build script
    std::ofstream build(path + "/build.sh");
    build << "#!/bin/bash\n";
    build << "emcc main.cpp -o main.wasm -s STANDALONE_WASM=1 -s EXPORTED_FUNCTIONS='[\"_create_user\",\"_get_user\"]' -O3\n";
    build << "cp main.wasm ../main.wasm\n";
    build.close();
    std::filesystem::permissions(path + "/build.sh",
                                std::filesystem::perms::owner_exec,
                                std::filesystem::perm_options::add);
}

} // namespace instantdb::cli