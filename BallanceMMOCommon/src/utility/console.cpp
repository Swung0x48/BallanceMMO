#include <iostream>
#include <algorithm>
#ifndef _WIN32 // vcpkg win32 readline doesn't support multibyte characters
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include "utility/console.hpp"
#include "utility/string_utils.hpp"

namespace bmmo {

namespace {
#ifndef _WIN32
    char* command_name_generator(const char* text, int state) {
        static char** cmd_matches;
        static int list_index;

        if (!state) {
            auto cmd_matches_stdstring = console::instance_->get_command_hints(false, text);
            cmd_matches = (char**) std::malloc((cmd_matches_stdstring.size() + 1) * sizeof(char*));
            if (!cmd_matches) return nullptr;
            for (size_t i = 0; i < cmd_matches_stdstring.size(); ++i) {
                // not using strdup as it is posix-only before c23
                auto data = (char*) std::malloc(cmd_matches_stdstring[i].size() * sizeof(char));
                if (!data) return nullptr;
                std::strcpy(data, cmd_matches_stdstring[i].c_str());
                cmd_matches[i] = data;
            }
            cmd_matches[cmd_matches_stdstring.size()] = nullptr;
            list_index = 0;
        }
        return cmd_matches[list_index++];
    }

    char** command_name_completion(const char* text, [[maybe_unused]] int start, [[maybe_unused]] int end) {
        if (std::strchr(rl_line_buffer, ' ')) {
            if (std::strncmp(rl_line_buffer, "playstream", sizeof("playstream") - 1) != 0)
                rl_attempted_completion_over = 1;
            return nullptr;
        }
        rl_attempted_completion_over = 1;
        return rl_completion_matches(text, command_name_generator);
    }
#endif // !_WIN32
}

console::console() {
    instance_ = this;
#ifndef _WIN32
    rl_attempted_completion_function = command_name_completion;
#endif // !_WIN32
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
    bool success{};
#ifdef _WIN32
    std::cout << "\r> " << std::flush;
    std::wstring wbuf;
    success = bool(std::getline(std::wcin, wbuf));
    buf = bmmo::string_utils::ConvertWideToANSI(wbuf);
    if (auto pos = buf.rfind('\r'); pos != std::string::npos)
        buf.erase(pos);
#else
    std::putchar('\r');
    auto input_cstr = readline("> ");
    if (!input_cstr)
        return false;
    buf.assign(input_cstr);
    free(input_cstr);
    success = std::cin.good();
    // return bool(std::getline(std::cin, buf));
    rl_delete_text(0, rl_end);
    add_history(buf.c_str());
#endif
    return success;
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
