#ifndef BALLANCEMMOSERVER_COMMON_HPP
#define BALLANCEMMOSERVER_COMMON_HPP

#ifdef _WIN32
#undef max
#endif

enum PacketType: uint32_t {
    PacketFragment,
    Denied,
    Ping,
    ConnectionRequest,
    ConnectionChallenge,
    ConnectionResponse,
    ConnectionAccepted,
    ClientDisconnect,
    ClientConnected,
    GameState
};

struct PlayerData {
    uint64_t id;
    std::string name;
    uint64_t checksum = 0u;
    uint32_t last_sequence = 0u;
    std::chrono::time_point<std::chrono::system_clock> last_timestamp = std::chrono::system_clock::from_time_t(0);
    ammo::role::client_state state = ammo::role::client_state::Disconnected;

    template <typename T>
    static inline bool sequence_greater_than(T s1, T s2) {
        static_assert(std::is_unsigned<T>(), "T should be an unsigned integral type.");
        return ((s1 > s2) && (s1 - s2 <= std::numeric_limits<T>::max() / 2)) ||
               ((s1 < s2) && (s2 - s1 > std::numeric_limits<T>::max() / 2));
    }

    template<typename T>
    static inline T sequence_max(T s1, T s2) {
        return sequence_greater_than(s1, s2) ? s1 : s2;
    }
};

uint64_t encode_for_validation(uint64_t bin) {
    auto* slice = reinterpret_cast<uint8_t *>(&bin);
    for (size_t i = 0; i < sizeof(bin) / sizeof(uint8_t); i++) {
        slice[i] = (slice[i] << 3 | slice[i] >> 5);
        slice[i] = -(slice[i] ^ uint8_t(0xAE));
    }
    return bin;
}

#endif //BALLANCEMMOSERVER_COMMON_HPP
