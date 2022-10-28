#ifndef BALLANCEMMOSERVER_CONSOLE_HPP
#define BALLANCEMMOSERVER_CONSOLE_HPP
#include "../BallanceMMOCommon/utility/command_parser.hpp"
#include <string>
#include <functional>
#include <map>
#include <algorithm>

class console {
    bmmo::command_parser parser_;
    std::map<std::string, std::function<void()>> commands_;
    std::string command_name_;

public:
    const std::string get_help_string() const;
    const std::vector<std::string> get_command_hints(bool fuzzy_matching = false) const;

    bool execute(const std::string &cmd);

    bool register_command(const std::string &name, const std::function<void()> &handler);
    bool register_aliases(const std::string &name, const std::vector<std::string> &aliases);
    bool unregister_command(const std::string &name);

    bool empty() const noexcept;
    const std::string get_next_word();
    const std::string get_rest_of_line();
    const std::string get_command_name() const { return command_name_; };
};

#endif // BALLANCEMMOSERVER_CONSOLE_HPP
