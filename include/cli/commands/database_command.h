#pragma once

#include "cli/command.h"

namespace instantdb::cli {

class DatabaseCommand : public Command {
public:
    int Execute(const std::vector<std::string>& args) override;
    void PrintHelp() const override;

private:
    int CreateDatabase(const std::vector<std::string>& args);
    int ListDatabases(const std::vector<std::string>& args);
    int DropDatabase(const std::vector<std::string>& args);
    int BackupDatabase(const std::vector<std::string>& args);
    int RestoreDatabase(const std::vector<std::string>& args);

    std::string GetServerUrl(const std::vector<std::string>& args) const;
    bool ExecuteGrpcCommand(const std::string& command, const std::string& server_url);
};

} // namespace instantdb::cli