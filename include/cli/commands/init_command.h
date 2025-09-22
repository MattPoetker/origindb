#pragma once

#include "cli/command.h"

namespace instantdb::cli {

class InitCommand : public Command {
public:
    int Execute(const std::vector<std::string>& args) override;
    void PrintHelp() const override;

private:
    int InitProject(const std::string& name, const std::string& lang);
    void CreateProjectStructure(const std::string& path, const std::string& lang);
    void CreateConfigFile(const std::string& path);
    void CreateWasmModule(const std::string& path, const std::string& lang);
    void CreateCSharpModule(const std::string& path);
    void CreateRustModule(const std::string& path);
    void CreateJavaScriptModule(const std::string& path);
    void CreateGoModule(const std::string& path);
    void CreateCppModule(const std::string& path);
};

} // namespace instantdb::cli