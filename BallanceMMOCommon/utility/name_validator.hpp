#ifndef BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#define BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#include <string>

namespace bmmo {
    class name_validator {
        static inline const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_()";
    public:
        static inline const std::size_t max_length = 20, min_length = 3;
        static std::size_t get_invalid_char_pos (const std::string& name) {
            return name.find_first_not_of(valid_chars);
        };
        static bool is_of_valid_length (const std::string& name) {
            return (name.length() >= min_length && name.length() <= max_length);
        };
    };
}

#endif //BALLANCEMMOSERVER_NAME_VALIDATOR_HPP