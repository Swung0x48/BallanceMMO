#ifndef BALLANCEMMOSERVER_CONSOLE_HPP
#define BALLANCEMMOSERVER_CONSOLE_HPP
#include "../BallanceMMOCommon/entity/map.hpp"
#include "../BallanceMMOCommon/utility/command_parser.hpp"
#include "../BallanceMMOCommon/utility/string_utils.hpp"
#include <string>
#include <functional>
#include <map>
#include <iostream>
#include <algorithm>
#include <mutex>

class console {
    bmmo::command_parser parser_;
    std::map<std::string, std::function<void()>> commands_;
    std::string command_name_;
    std::mutex console_mutex_;

public:
    const std::string get_help_string() const;
    const std::vector<std::string> get_command_hints(bool fuzzy_matching = false) const;

    // returns whether the stream has no errors.
    bool read_input(std::string& buf);
    bool execute(const std::string &cmd);

    bool register_command(const std::string &name, const std::function<void()> &handler);
    bool register_aliases(const std::string &name, const std::vector<std::string> &aliases);
    bool unregister_command(const std::string &name);

    bool empty() const noexcept;
    inline const std::string get_command_name() const { return command_name_; };

    const std::string get_next_word();
    const std::string get_rest_of_line();
    const bmmo::named_map get_next_map(bool with_name = false);
    inline int32_t get_next_int() { return std::atoi(get_next_word().c_str()); };
    inline int64_t get_next_long() { return std::atoll(get_next_word().c_str()); };
    inline double get_next_double() { return std::atof(get_next_word().c_str()); };
};

#endif // BALLANCEMMOSERVER_CONSOLE_HPP
