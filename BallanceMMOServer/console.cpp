#include "console.hpp"

const std::string console::get_help_string() const {
    std::string help_string;
    std::for_each(commands_.begin(), commands_.end(),
        [&help_string](const auto &i) { help_string += i.first + ", "; });
    help_string.erase(help_string.length() - 2);
    return help_string;
};

const std::vector<std::string> console::get_command_hints(bool fuzzy_matching) const {
    std::string start_name(command_name_);
    if (fuzzy_matching && start_name.length() > 1)
        start_name.erase((start_name.length() - 1) * 2 / 3 + 1);
    std::string end_name(start_name);
    ++end_name[end_name.length() - 1];
    auto end = commands_.lower_bound(end_name);
    std::vector<std::string> hints;
    for (auto it = commands_.lower_bound(start_name); it != end; it++) {
        hints.push_back(it->first);
    };
    return hints;
};

bool console::read_input(std::string& buf) {
#ifdef _WIN32
        std::wstring wbuf;
        bool success = bool(std::getline(std::wcin, wbuf));
        buf = bmmo::message_utils::ConvertWideToANSI(wbuf);
        if (auto pos = buf.rfind('\r'); pos != std::string::npos)
            buf.erase(pos);
        return success;
#else
        return bool(std::getline(std::cin, buf));
#endif
}

bool console::execute(const std::string &cmd) {
    parser_ = bmmo::command_parser(cmd);
    command_name_ = parser_.get_next_word();
    if (auto it = commands_.find(command_name_); it != commands_.end()) {
        (it->second)();
        return true;
    };
    return false;
};

bool console::register_command(const std::string &name, const std::function<void()> &handler) {
    if (commands_.contains(name))
        return false;
    commands_[name] = handler;
    return true;
};

bool console::register_aliases(const std::string &name, const std::vector<std::string> &aliases) {
    auto it = commands_.find(name);
    if (it == commands_.end())
        return false;
    for (const auto& i: aliases) {
        commands_.emplace(i, it->second);
    };
    return true;
};

bool console::unregister_command(const std::string &name) {
    if (commands_.erase(name))
        return true;
    return false;
};

bool console::empty() const noexcept {
    return parser_.empty();
};

const std::string console::get_next_word() {
    return parser_.get_next_word();
};

const std::string console::get_rest_of_line() {
    return parser_.get_rest_of_line();
};
