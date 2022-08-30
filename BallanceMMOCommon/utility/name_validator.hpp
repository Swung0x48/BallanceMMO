#ifndef BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#define BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#include <string>
#include <ctime>

namespace bmmo {
    class name_validator {
        static inline const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-+=.~()";

    public:
        static inline const std::size_t max_length = 20, min_length = 3;

        static std::size_t get_invalid_char_pos(const std::string& name) {
            return name.find_first_not_of(valid_chars);
        };

        static bool is_spectator(const std::string& name) {
            return name.starts_with('*');
        }

        // spectator name pattern: *NICKNAME
        static std::string get_real_nickname(std::string name) {
            if (is_spectator(name))
                name.erase(0, 1);
            return name;
        }

        static bool is_of_valid_length(const std::string& name) {
            return (name.length() >= min_length && name.length() <= max_length);
        };

        // Valid usernames must be 3~20 characters long and contain only valid characters,
        // which are a-z, A-Z, 0-9, plus "_", "-", "+", "=", ".", "~", "(", and ")".
        // They also must not be consisted of only underlines.
        static bool is_valid(const std::string& name) {
            return (get_invalid_char_pos(name) == std::string::npos
                    && is_of_valid_length(name)
                    && name.find_first_not_of('_') != std::string::npos);
        };

        // Generate a random username in the format of "Player<random_number>_<date>".
        static std::string get_random_nickname() {
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
        static std::string get_valid_nickname(std::string name) {
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