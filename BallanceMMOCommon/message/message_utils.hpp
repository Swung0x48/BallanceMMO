#ifndef BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
#define BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
#include <steam/steamnetworkingtypes.h>
#include <string>
#include <sstream>
#include "../utility/string_utils.hpp"

namespace bmmo {
    namespace message_utils {
        // for compatibility with our old code
        using bmmo::string_utils::to_lower;
        using bmmo::string_utils::join_strings;

        inline void write_string(std::string str, std::stringstream& stream) {
#ifdef _WIN32
            str = string_utils::ConvertWideToUtf8(string_utils::ConvertAnsiToWide(str));
#endif
            uint32_t length = str.length();
            stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
            stream.write(str.c_str(), str.length());
        }

        inline bool read_string(std::stringstream& stream, std::string& str) {
            uint32_t length = 0;
            stream.read(reinterpret_cast<char*>(&length), sizeof(length));
            if (length > stream.tellp())
                return false;
            str.resize(length);
            stream.read(str.data(), length);
#ifdef _WIN32
            str = string_utils::ConvertWideToANSI(string_utils::ConvertUtf8ToWide(str));
#endif
            return true;
        }

        template<trivially_copyable_msg T>
        constexpr inline T deserialize(void* data, int) {
            return *reinterpret_cast<T*>(data);
        }

        template<non_trivially_copyable_msg T>
        constexpr inline T deserialize(void* data, int size) {
            T msg{};
            msg.raw.write(reinterpret_cast<char*>(data), size);
            msg.deserialize();
            return msg;
        }

        template<typename T>
        constexpr inline T deserialize(ISteamNetworkingMessage* networking_msg) {
            return deserialize<T>(networking_msg->m_pData, networking_msg->m_cbSize);
        }
    };
}

#endif //BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
