#ifndef BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#define BALLANCEMMOSERVER_NAME_VALIDATOR_HPP
#include <string>

namespace bmmo {
    class name_validator {
        static inline const char *valid_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-+=@~()";
    public:
        static inline const std::size_t max_length = 20, min_length = 3;
        static std::size_t get_invalid_char_pos(const std::string& name) {
            return name.find_first_not_of(valid_chars);
        };
        static bool is_of_valid_length(const std::string& name) {
            return (name.length() >= min_length && name.length() <= max_length);
        };
        static bool is_valid(const std::string& name) {
            return (get_invalid_char_pos(name) == std::string::npos && is_of_valid_length(name));
        };
        static std::string get_valid_nickname(std::string name) {
            if (!is_of_valid_length(name))
                name = (name + "___").substr(0, 20);
            size_t invalid_pos = std::string::npos;
            while ((invalid_pos = get_invalid_char_pos(name)) != std::string::npos) {
                name[invalid_pos] = '_';
            };
            return name;
        };
    };
}

#endif //BALLANCEMMOSERVER_NAME_VALIDATOR_HPP