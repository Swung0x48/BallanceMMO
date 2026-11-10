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
        // The host's settings for the session its room will start, while the
        // room waits (refused once a session runs): the map - the one the
        // host's game has loaded when it asks - and the kind.
        SetMap = 9,
        SetMode = 10,
        // While the room's session runs: a session member's vote to restart
        // it (and its withdrawal), and the host's Restart, which the server
        // takes once every member in the session has voted - a new session
        // of the same kind on the same map, with every member of the room.
        VoteRestart = 11,
        UnvoteRestart = 12,
        Restart = 13,
    };

    // Session kind: the room's setting (SetMode), confirmed by the host's Start.
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
        SessionRunning,
        NoSession,
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
            case error_code::SessionRunning: return "the room's session is running";
            case error_code::NoSession: return "the room has no running session";
            default: return "unknown error";
        }
    }

    // A room's map is set once it names one: a zeroed map (no md5) is "none
    // yet" - the room waits in the lobby, and a Start takes the host's own.
    inline bool map_set(const bmmo::map& m) {
        static constexpr uint8_t none[sizeof(m.md5)] = {};
        return std::memcmp(m.md5, none, sizeof(none)) != 0;
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
        bool vote = false;      // voted to restart the running session
        bool in_session = false;  // takes part in the room's running session (a late joiner yet to join,
                                  // a finisher is in the room but out of it)
        bool is_host = false;
        bmmo::map map{};
    };
}

#endif // BALLANCEMMOSERVER_ROOM_HPP
