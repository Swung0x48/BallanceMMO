#ifndef BALLANCEMMOSERVER_COMMAND_PARSER_HPP
#define BALLANCEMMOSERVER_COMMAND_PARSER_HPP
#include <string>

namespace bmmo {
    class command_parser {
        std::string cmd;
    public:
        command_parser(const std::string& cmd = "") {
            this->cmd = cmd;
        }

        bool empty() {
            return cmd.find_first_not_of(" \t\n\v\f\r") == std::string::npos;
        }

        std::string get_next_word() {
            std::string word;
            while (cmd.length() > 0 && cmd[0] == ' ')
                cmd.erase(0, 1);
            while (cmd.length() > 0 && cmd[0] != ' ') {
                word += cmd[0];
                cmd.erase(0, 1);
            }
            return word;
        }

        std::string get_rest_of_line() {
            std::string rest;
            while (cmd.length() > 0 && cmd[0] == ' ')
                cmd.erase(0, 1);
            while (cmd.length() > 0) {
                rest += cmd[0];
                cmd.erase(0, 1);
            }
            return rest;
        }
    };
}

#endif //BALLANCEMMOSERVER_COMMAND_PARSER_HPP