#pragma once

#include "cli/command.h"

namespace instantdb::cli {

class BuildCommand : public Command {
public:
    int Execute(const std::vector<std::string>& args) override;
    void PrintHelp() const override;

private:
    int BuildServer(const std::vector<std::string>& args);
    int BuildClient(const std::vector<std::string>& args);
    int BuildAll(const std::vector<std::string>& args);
    int CleanBuild(const std::vector<std::string>& args);

    bool RunCMake(const std::string& build_type);
    bool RunMake(const std::string& target);
};

} // namespace instantdb::cli