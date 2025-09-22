#pragma once

#include "cli/command.h"

namespace instantdb::cli {

class LogsCommand : public Command {
public:
    int Execute(const std::vector<std::string>& args) override;
    void PrintHelp() const override;

private:
    int TailLogs(const std::string& log_file, int lines, bool follow);
    std::string GetLogFile(const std::vector<std::string>& args) const;
};

} // namespace instantdb::cli