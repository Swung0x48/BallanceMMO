#ifndef BALLANCEMMOSERVER_ROOM_HPP
#define BALLANCEMMOSERVER_ROOM_HPP
#include <cstdint>
#include <string>
#include <vector>
#include "map.hpp"

// Shared vocabulary for the collision-overhaul room & session protocol
// (docs/rooms-and-sessions-protocol.md). Kept as plain enums and POD structs so
// the client mod and the server agree on every wire value.
namespace bmmo::room {
    // Wire limits (bytes of UTF-8). Names use a u16 length prefix on the wire.
    constexpr size_t MAX_ROOM_NAME = 32;
    constexpr size_t MAX_MEMBER_NAME = 64;
    constexpr size_t MAX_REASON = 256;
    // A hard cap on how many rooms / members a single message may describe, so a
    // forged count cannot make the reader allocate unbounded memory.
    constexpr size_t MAX_ROOMS_PER_MESSAGE = 4096;
    constexpr size_t MAX_MEMBERS_PER_MESSAGE = 256;

    enum class action : uint8_t {
        List = 0,
        Create = 1,
        Join = 2,
        Leave = 3,
        Ready = 4,
        Unready = 5,
        Start = 6,
        Kick = 7,
        Close = 8,
    };

    // Session kind chosen by the host at Start.
    enum class mode : uint8_t {
        Shadow = 0,   // legacy shadow-ball logic, scoped to the room
        Physics = 1,  // server-authoritative shared physics (milestone M3)
    };

    enum class phase : uint8_t {
        Lobby = 0,
        Running = 1,
    };

    enum class event_type : uint8_t {
        RequestAccepted = 0,
        RequestDenied = 1,
        PlayerJoined = 2,
        PlayerLeft = 3,
        HostChanged = 4,
        Kicked = 5,
        RoomClosed = 6,
        ReadyChanged = 7,
        SessionStarting = 8,
        SessionEnded = 9,
    };

    enum class error_code : uint8_t {
        None = 0,
        NotFound,
        Full,
        AlreadyInRoom,
        NotInRoom,
        NotHost,
        NotReady,
        MapMismatch,
        ModMismatch,
        PhysicsUnavailable,
        InvalidName,
        ServerBusy,
        Unsupported,
    };

    inline const char* error_string(error_code e) {
        switch (e) {
            case error_code::None: return "no error";
            case error_code::NotFound: return "room not found";
            case error_code::Full: return "room is full";
            case error_code::AlreadyInRoom: return "already in a room";
            case error_code::NotInRoom: return "not in a room";
            case error_code::NotHost: return "only the host may do that";
            case error_code::NotReady: return "not everyone is ready";
            case error_code::MapMismatch: return "players are on different maps";
            case error_code::ModMismatch: return "mod versions do not match";
            case error_code::PhysicsUnavailable: return "physics sessions are unavailable";
            case error_code::InvalidName: return "invalid room name";
            case error_code::ServerBusy: return "server is busy";
            case error_code::Unsupported: return "unsupported request";
            default: return "unknown error";
        }
    }

    // One row in a room_state listing.
    struct room_info {
        uint32_t id = 0;
        std::string name;
        uint32_t host = 0;
        uint16_t member_count = 0;
        uint16_t capacity = 0;
        // Named room_phase/room_mode, not phase/mode: a member named exactly
        // like its own enum type builds fine but some compilers (GCC 16)
        // reject it as a hard error under -Wchanges-meaning.
        phase room_phase = phase::Lobby;
        mode room_mode = mode::Shadow;
    };

    // One member of the caller's own room.
    struct room_member {
        uint32_t id = 0;
        std::string name;
        bool ready = false;
        bool is_host = false;
        bmmo::map map{};
    };
}

#endif // BALLANCEMMOSERVER_ROOM_HPP
