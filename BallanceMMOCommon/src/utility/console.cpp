#include <iostream>
#include <algorithm>
#include <replxx.hxx>
#include "entity/globals.hpp"
#include "utility/console.hpp"
#include "utility/string_utils.hpp"

namespace bmmo {

namespace {
    replxx::Replxx::hints_t command_hint(std::string const& input, int& contextLen, [[maybe_unused]] replxx::Replxx::Color& color) {
        if (input.empty() || input.find_first_of(" \t\n\v\f\r") != std::string::npos)
            return {};
        auto hints = console::instance->get_command_hints(false, input.c_str());
        contextLen = input.length();
        switch (hints.size()) {
            case 1: return hints;
            case 2: if (hints[0] + '#' == hints[1]) return {hints[0]};
        }
        return {};
    }

    size_t last_line_length = 0;
    // deleted characters somehow remain on win32 conhost
    // so we have to clear them manually
    [[maybe_unused]] void command_modify_repaint(std::string& line, [[maybe_unused]] int& cursorPosition) {
        if (line.length() < last_line_length) {
            // fputs("\r\033[0K", stdout); // windows7: no ansi
            // printf("\r%*s\r", int(last_line_length + 2), "");
            replxx_instance.print("\033[0K");
            replxx_instance.invoke(replxx::Replxx::ACTION::REPAINT, '\0');
        }
        last_line_length = line.length();
    }
}

replxx::Replxx replxx_instance = [] {
    replxx::Replxx instance;
    
    instance.install_window_change_handler();
    instance.set_complete_on_empty(false);
    instance.set_hint_callback(command_hint);
    instance.set_indent_multiline(true);
    instance.set_ignore_case(false);
#ifdef _WIN32
    instance.set_modify_callback(command_modify_repaint);
#endif // _WIN32

    console::set_completion_callback([](const std::vector<std::string>& args) -> std::vector<std::string> {
        if (args.size() == 1)
            return console::instance->get_command_hints(false, args[0].c_str());
        return {};
    });

    return instance;
}();

console::console() {
    instance = this;
}

void console::set_completion_callback(std::function<std::vector<std::string>(const std::vector<std::string>&)> func) {
    replxx_instance.set_completion_callback([func](std::string const& input, int& contextLen)
                                            -> replxx::Replxx::completions_t {
        const auto words = string_utils::split_strings(input);
        const auto& last_word = words[words.size() - 1];
        contextLen = last_word.length();
        replxx::Replxx::completions_t completions;
        for (const auto& hint: func(words))
            if (hint.starts_with(last_word)) completions.emplace_back(hint);
        return completions;
    });
}

const std::string console::get_help_string() const {
    std::string help_string;
    std::for_each(commands_.begin(), commands_.end(),
        [&help_string](const auto &i) { help_string += i.first + ", "; });
    help_string.erase(help_string.length() - 2);
    return help_string;
};

const std::vector<std::string> console::get_command_list() const {
    std::remove_const_t<decltype(get_command_list())> command_list;
    for (const auto& i: commands_)
        command_list.emplace_back(i.first);
    return command_list;
};

const std::vector<std::string> console::get_command_hints(bool fuzzy_matching, const char* cmd) const {
    std::string start_name(cmd ? cmd : command_name_.c_str());
    if (start_name.empty())
        return {"help"};
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

bool console::read_input(std::string &buf) {
    replxx_instance.print("\r\033[0K");
    auto input_cstr = replxx_instance.input("> ");
    if (!input_cstr)
        return false;
#ifdef _WIN32
    buf = bmmo::string_utils::ConvertWideToANSI(bmmo::string_utils::ConvertUtf8ToWide(input_cstr));
#else
    buf.assign(input_cstr);
#endif // _WIN32
    // return bool(std::getline(std::cin, buf));
    if (!buf.empty())
        replxx_instance.history_add(buf);
    // bmmo::replxx_instance.invoke(replxx::Replxx::ACTION::CLEAR_SELF, '\0');
    // bmmo::replxx_instance.invoke(replxx::Replxx::ACTION::REPAINT, '\0');
    return std::cin.good();
}

bool console::execute(const std::string &cmd) {
    std::unique_lock lk(console_mutex_);
    parser_ = bmmo::command_parser(cmd);
    command_name_ = bmmo::string_utils::to_lower(parser_.get_next_word());
    if (auto it = commands_.find(command_name_); it != commands_.end()) {
        (it->second)();
        return true;
    };
    return false;
};

bool console::register_command(const std::string &name, const std::function<void()> &handler) {
    auto name_lower = bmmo::string_utils::to_lower(name);
    if (commands_.contains(name_lower))
        return false;
    std::unique_lock lk(console_mutex_);
    commands_[name_lower] = handler;
    return true;
};

bool console::register_aliases(const std::string &name, const std::vector<std::string> &aliases) {
    std::unique_lock lk(console_mutex_);
    auto it = commands_.find(bmmo::string_utils::to_lower(name));
    if (it == commands_.end())
        return false;
    for (const auto& i: aliases) {
        commands_.emplace(i, it->second);
    };
    return true;
};

bool console::unregister_command(const std::string &name) {
    std::unique_lock lk(console_mutex_);
    if (commands_.erase(bmmo::string_utils::to_lower(name)))
        return true;
    return false;
};

bool console::empty() const noexcept {
    return parser_.empty();
};

const std::string console::get_next_word(bool to_lowercase) {
    return parser_.get_next_word(to_lowercase);
};

const std::string console::get_rest_of_line() {
    return parser_.get_rest_of_line();
}

const bmmo::named_map console::get_next_map(bool with_name) {
    std::string hash = get_next_word();
    bmmo::named_map input_map = {{.type = bmmo::map_type::OriginalLevel,
                                .level = std::clamp(get_next_int(), 0, 13)}, {}};
    if (hash == "level")
        bmmo::hex_chars_from_string(input_map.md5, bmmo::map::original_map_hashes[input_map.level]);
    else
        bmmo::hex_chars_from_string(input_map.md5, hash);
    if (with_name && !empty())
        input_map.name = get_rest_of_line();
    return input_map;
}

}
