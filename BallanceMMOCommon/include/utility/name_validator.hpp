#ifndef BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#define BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#include <string>

namespace bmmo {
    namespace name_validator {
        inline constexpr const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-+=.~()";
        inline constexpr const char spectator_prefix = '*';

        inline constexpr std::size_t max_length = 20, min_length = 3;

        std::size_t get_invalid_char_pos(const std::string& name);

        // spectator status is only indicated by the prefix as in spectator_prefix;
        // their different behaviors from normal players are entirely client-side
        inline bool is_spectator(const std::string& name) {
            return name.starts_with(spectator_prefix);
        }

        // spectator name pattern: *NICKNAME
        std::string get_real_nickname(const std::string& name);

        std::string get_spectator_nickname(const std::string& name);

        inline bool is_of_valid_length(const std::string& name) {
            return (name.length() >= min_length && name.length() <= max_length);
        };

        // Valid usernames must be 3~20 characters long and contain only valid characters,
        // which are a-z, A-Z, 0-9, plus "_", "-", "+", "=", ".", "~", "(", and ")".
        // They also must not be consisted of only underlines.
        bool is_valid(const std::string& name);

        // Generate a random username in the format of "Player<random_number>_<date>".
        std::string get_random_nickname();

        // Invalid characters are replaced with underscores.
        // If the name is too long, it is truncated; too short and it is appended with 3 underscores.
        // Then names with only underscores are discarded and regenerated randomly.
        std::string get_valid_nickname(std::string name);
    };
}

#endif //BALLANCEMMOSERVER_NAME_VALIDATOR_HPP