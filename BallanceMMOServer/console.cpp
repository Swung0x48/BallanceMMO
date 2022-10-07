#include "console.hpp"

const std::string console::get_command_list() const {
    std::string help_string;
    std::for_each(commands_.begin(), commands_.end(),
        [&help_string](const auto &i) { help_string += i.first + ", "; });
    help_string.erase(help_string.length() - 2);
    return help_string;
}

bool console::execute(std::string cmd) {
    parser_ = bmmo::command_parser(cmd);
    command_name_ = parser_.get_next_word();
    if (auto it = commands_.find(command_name_); it != commands_.end()) {
        (it->second)();
        return true;
    };
    return false;
};

bool console::register_command(std::string name, std::function<void()> handler) {
    if (commands_.contains(name))
        return false;
    commands_[name] = handler;
    return true;
};

bool console::unregister_command(std::string name) {
    if (auto it = commands_.find(name); it != commands_.end()) {
        commands_.erase(name);
        return true;
    };
    return false;
};

const std::string console::get_next_word() {
    return parser_.get_next_word();
}

const std::string console::get_rest_of_line() {
    return parser_.get_rest_of_line();
}
