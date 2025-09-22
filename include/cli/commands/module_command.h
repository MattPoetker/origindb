#pragma once

#include "cli/command.h"
#include <nlohmann/json.hpp>

namespace instantdb::cli {

class ModuleCommand : public Command {
public:
    int Execute(const std::vector<std::string>& args) override;
    void PrintHelp() const override;

private:
    int InitModule(const std::vector<std::string>& args);
    int BuildModule(const std::vector<std::string>& args);
    int DeployModule(const std::vector<std::string>& args);
    int ListModules(const std::vector<std::string>& args);
    int RemoveModule(const std::vector<std::string>& args);

    bool RunBuildScript(const std::string& module_path);
    bool DeployToServer(const std::string& module_name, const std::string& wasm_path, const std::string& server_url);
    std::vector<std::string> FindModules() const;
    std::string GetServerUrl(const std::vector<std::string>& args) const;
};

} // namespace instantdb::cli