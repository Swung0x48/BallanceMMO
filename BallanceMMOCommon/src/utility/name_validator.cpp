#include <ctime>
#include "utility/name_validator.hpp"

namespace bmmo::name_validator {
    std::size_t get_invalid_char_pos(const std::string& name) {
        return name.find_first_not_of(valid_chars);
    };

    std::string get_real_nickname(const std::string& name) {
        if (is_spectator(name))
            return name.substr(1);
        return name;
    };

    std::string get_spectator_nickname(const std::string& name) {
        if (!is_spectator(name))
            return spectator_prefix + name;
        return name;
    };

    bool is_valid(const std::string& name) {
        return (get_invalid_char_pos(name) == std::string::npos
                && is_of_valid_length(name)
                && name.find_first_not_of('_') != std::string::npos);
    };

    std::string get_random_nickname() {
        time_t current_time = std::time(nullptr);
        auto time_struct = std::localtime(&current_time);
        std::srand((unsigned int) current_time);
        std::string name(15, 0);
        name.resize(std::snprintf(name.data(), name.size(), "Player%03d_%02d%02d",
            std::rand() % 1000, time_struct->tm_mon + 1, time_struct->tm_mday));
        return name;
    };

    std::string get_valid_nickname(std::string name) {
        if (!is_of_valid_length(name))
            name = (name + "___").substr(0, max_length);
        size_t invalid_pos = std::string::npos;
        while ((invalid_pos = get_invalid_char_pos(name)) != std::string::npos) {
            name[invalid_pos] = '_';
        };
        if (name.find_first_not_of('_') == std::string::npos)
            return get_random_nickname();
        return name;
    };
};
