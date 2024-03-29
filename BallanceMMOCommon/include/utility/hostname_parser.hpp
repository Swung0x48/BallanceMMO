#ifndef BALLANCEMMOSERVER_HOSTNAME_PARSER_HPP
#define BALLANCEMMOSERVER_HOSTNAME_PARSER_HPP
#include <utility>
#include <string>
#include "../entity/constants.hpp"

namespace bmmo {
    class hostname_parser {
        std::string address, port;
    public:
        hostname_parser(const std::string& str) {
            size_t pos = str.rfind(":");
            address = (pos == str.rfind("::") + 1) ? str : str.substr(0, pos);
            port = (pos == str.npos || (pos + 1) == str.length() || pos == str.rfind("::") + 1)
                ? std::to_string(DEFAULT_PORT)
                : str.substr(pos + 1);
        };

        std::pair<std::string, std::string> get_host_components() const {
            return { address, port };
        };

        std::string get_address() const { return address; };
        std::string get_port() const { return port; };
    };
};

#endif // BALLANCEMMOSERVER_HOSTNAME_PARSER_HPP