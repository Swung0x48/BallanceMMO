#ifndef BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
#define BALLANCEMMOSERVER_MESSAGE_UTILS_HPP

namespace bmmo {
    class message_utils {
    public:
        static void write_string(std::string& str, std::stringstream& stream) {
            uint32_t length = str.length();
            stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
            stream.write(str.c_str(), str.length());
        }

        static void read_string(std::stringstream& stream, std::string& str) {
            uint32_t length = 0;
            stream.read(reinterpret_cast<char*>(&length), sizeof(length));
            str.resize(length);
            stream.read(str.data(), length);
        }
    };
}


#endif //BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
