#pragma once

#include <string>
#include <vector>
#include <algorithm>

namespace instantdb::cli {

class Command {
public:
    virtual ~Command() = default;
    virtual int Execute(const std::vector<std::string>& args) = 0;
    virtual void PrintHelp() const = 0;

protected:
    bool HasFlag(const std::vector<std::string>& args, const std::string& flag) const {
        return std::find(args.begin(), args.end(), flag) != args.end();
    }

    std::string GetOption(const std::vector<std::string>& args, const std::string& option) const {
        auto it = std::find(args.begin(), args.end(), option);
        if (it != args.end() && std::next(it) != args.end()) {
            return *std::next(it);
        }
        return "";
    }

    std::vector<std::string> GetPositionalArgs(const std::vector<std::string>& args) const {
        std::vector<std::string> positional;
        for (size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (arg.starts_with("--")) {
                // Skip flag and its value if it has one
                if (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
                    ++i; // Skip the value
                }
            } else if (!arg.starts_with("-")) {
                positional.push_back(arg);
            }
        }
        return positional;
    }
};

} // namespace instantdb::cli