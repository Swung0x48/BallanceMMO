#ifndef BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#define BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#include <string>
#include <ctime>

namespace bmmo {
    namespace name_validator {
        inline constexpr const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-+=.~()";
        inline constexpr const char spectator_prefix = '*';

        inline constexpr std::size_t max_length = 20, min_length = 3;

        std::size_t get_invalid_char_pos(const std::string& name) {
            return name.find_first_not_of(valid_chars);
        };

        bool is_spectator(const std::string& name) {
            return name.starts_with(spectator_prefix);
        }

        // spectator name pattern: *NICKNAME
        std::string get_real_nickname(const std::string& name) {
            if (is_spectator(name))
                return name.substr(1);
            return name;
        }

        std::string get_spectator_nickname(const std::string& name) {
            if (!is_spectator(name))
                return spectator_prefix + name;
            return name;
        }

        bool is_of_valid_length(const std::string& name) {
            return (name.length() >= min_length && name.length() <= max_length);
        };

        // Valid usernames must be 3~20 characters long and contain only valid characters,
        // which are a-z, A-Z, 0-9, plus "_", "-", "+", "=", ".", "~", "(", and ")".
        // They also must not be consisted of only underlines.
        bool is_valid(const std::string& name) {
            return (get_invalid_char_pos(name) == std::string::npos
                    && is_of_valid_length(name)
                    && name.find_first_not_of('_') != std::string::npos);
        };

        // Generate a random username in the format of "Player<random_number>_<date>".
        std::string get_random_nickname() {
            time_t current_time = std::time(nullptr);
            auto time_struct = std::localtime(&current_time);
            std::srand((unsigned int) current_time);
            std::string name(15, 0);
            name.resize(std::sprintf(name.data(), "Player%03d_%02d%02d",
                std::rand() % 1000, time_struct->tm_mon + 1, time_struct->tm_mday));
            return name;
        };

        // Invalid characters are replaced with underscores.
        // If the name is too long, it is truncated; too short and it is appended with 3 underscores.
        // Then names with only underscores are discarded and regenerated randomly.
        std::string get_valid_nickname(std::string name) {
            if (!is_of_valid_length(name))
                name = (name + "___").substr(0, 20);
            size_t invalid_pos = std::string::npos;
            while ((invalid_pos = get_invalid_char_pos(name)) != std::string::npos) {
                name[invalid_pos] = '_';
            };
            if (name.find_first_not_of('_') == std::string::npos)
                return get_random_nickname();
            return name;
        };
    };
}

#endif //BALLANCEMMOSERVER_NAME_VALIDATOR_HPP