#pragma once

#include "cli/command.h"

namespace instantdb::cli {

class ServerCommand : public Command {
public:
    int Execute(const std::vector<std::string>& args) override;
    void PrintHelp() const override;

private:
    int StartServer(const std::vector<std::string>& args);
    int StopServer(const std::vector<std::string>& args);
    int RestartServer(const std::vector<std::string>& args);
    int ServerStatus(const std::vector<std::string>& args);

    bool IsServerRunning() const;
    int GetServerPid() const;
    void SaveServerPid(int pid) const;
    void RemoveServerPid() const;
    std::string GetPidFilePath() const;
    std::string GetConfigPath(const std::vector<std::string>& args) const;
    std::vector<std::string> BuildServerArgs(const std::vector<std::string>& args) const;
};

} // namespace instantdb::cli