#ifndef BALLANCEMMOSERVER_CONSOLE_HPP
#define BALLANCEMMOSERVER_CONSOLE_HPP
#include "../BallanceMMOCommon/utility/command_parser.hpp"
#include <string>
#include <functional>

class console {
    bmmo::command_parser parser_;
    std::unordered_map<std::string, std::function<void()>> commands;

public:
    bool execute(std::string cmd);

    bool register_command(std::string name, std::function<void()> handler);
    bool unregister_command(std::string name);
};

#endif // BALLANCEMMOSERVER_CONSOLE_HPP