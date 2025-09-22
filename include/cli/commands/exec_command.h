#pragma once

#include "cli/command.h"

namespace instantdb::cli {

class ExecCommand : public Command {
public:
    int Execute(const std::vector<std::string>& args) override;
    void PrintHelp() const override;

private:
    int ExecuteReducer(const std::vector<std::string>& args);
    int ExecuteSQL(const std::vector<std::string>& args);

    std::string GetServerUrl(const std::vector<std::string>& args) const;
    bool ExecuteGrpcCommand(const std::string& command, const std::string& server_url);
};

} // namespace instantdb::cli