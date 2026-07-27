#ifndef BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
#define BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
#include <steam/steamnetworkingtypes.h>
#include <algorithm>
#include <limits>
#include <string>
#include <sstream>
#include <concepts>
#include "message.hpp"
#include "../utility/string_utils.hpp"

namespace bmmo::message_utils {
    // for compatibility with our old code
    using bmmo::string_utils::to_lower;
    using bmmo::string_utils::join_strings;

    // template T: type to store length of the string.
    // Strings longer than what T can express are truncated; writing the full
    // string with a wrapped-around length would desynchronize the stream and
    // make every subsequent field unreadable.
    template<typename T = uint32_t>
    inline void write_string(const std::string& str, std::stringstream& stream) {
        const auto max_length = static_cast<size_t>(std::numeric_limits<T>::max());
        const size_t written_length = std::min(str.length(), max_length);
        T length = static_cast<T>(written_length);
        stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
        stream.write(str.data(), written_length);
    }

    // template T: type to store length of the string
    template<typename T = uint32_t>
    inline bool read_string(std::stringstream& stream, std::string& str) {
        T length = 0;
        if (!stream.read(reinterpret_cast<char*>(&length), sizeof(length)))
            return false;
        // bound the length by what is actually left in the buffer;
        // a forged length would otherwise make us allocate and read garbage
        const auto remaining = static_cast<std::streamoff>(stream.tellp()) - stream.tellg();
        if (remaining < 0 || static_cast<std::streamoff>(length) > remaining)
            return false;
        str.resize(length);
        stream.read(str.data(), length);
        return stream.good() || length == 0;
    }

    template<std::semiregular T>
    constexpr inline bool read_variable(std::istream& stream, T* t) {
        stream.read(reinterpret_cast<char*>(t), sizeof(T));
        if (stream.good() && stream.gcount() == sizeof(T))
            return true;
        return false;
    }

    // Value-initialized so a failed read yields a zeroed T rather than
    // whatever was on the stack; callers that need to detect failure should
    // use the pointer overload above.
    template<std::semiregular T>
    constexpr inline T read_variable(std::istream& stream) {
        T t{};
        if (!stream.read(reinterpret_cast<char*>(&t), sizeof(T)))
            return T{};
        return t;
    }

    // Number of bytes left to read in a message stream.
    inline std::streamoff bytes_remaining(std::stringstream& stream) {
        const auto remaining = static_cast<std::streamoff>(stream.tellp()) - stream.tellg();
        return remaining < 0 ? 0 : remaining;
    }

    template<std::semiregular T>
    constexpr inline void write_variable(const T* t, std::ostream& stream) {
        stream.write(reinterpret_cast<const char*>(t), sizeof(T));
    }

    // A packet too short to hold a T yields a default-constructed message
    // instead of a read past the end of the received buffer.
    template<trivially_copyable_msg T>
    inline T deserialize(void* data, int size) {
        if (!data || size < static_cast<int>(sizeof(T)))
            return T{};
        return *reinterpret_cast<T*>(data);
    }

    // Likewise, a message that fails to parse yields an empty T rather than
    // one half-filled with attacker-controlled fields.
    template<non_trivially_copyable_msg T>
    inline T deserialize(void* data, int size) {
        T msg{};
        if (!data || size <= 0)
            return msg;
        msg.raw.write(reinterpret_cast<char*>(data), size);
        if (!msg.deserialize())
            return T{};
        return msg;
    }

    template<typename T>
    constexpr inline T deserialize(ISteamNetworkingMessage* networking_msg) {
        return deserialize<T>(networking_msg->m_pData, networking_msg->m_cbSize);
    }

    // Size-checked view of a fixed-layout message payload.
    // Returns nullptr if the packet is too short to contain a T,
    // protecting reinterpret_cast message handlers from out-of-bounds
    // reads on truncated or malicious packets.
    template<trivially_copyable_msg T>
    inline T* view_as(ISteamNetworkingMessage* networking_msg) {
        if (networking_msg->m_cbSize < static_cast<int>(sizeof(T)))
            return nullptr;
        return reinterpret_cast<T*>(networking_msg->m_pData);
    }
}

#endif //BALLANCEMMOSERVER_MESSAGE_UTILS_HPP
