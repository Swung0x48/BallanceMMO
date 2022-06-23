#include "console.hpp"

bool console::execute(std::string cmd) {
    parser_ = bmmo::command_parser(cmd);
    if (auto name = parser_.get_next_word(); commands.find(name) != commands.end()) {
        commands.at(name)();
        return true;
    };
    return false;
};

bool console::register_command(std::string name, std::function<void()> handler) {
    if (commands.find(name) != commands.end())
        return false;
    commands[name] = handler;
};

bool console::unregister_command(std::string name) {
    if (auto it = commands.find(name); it != commands.end()) {
        commands.erase(name);
        return true;
    };
    return false;
};