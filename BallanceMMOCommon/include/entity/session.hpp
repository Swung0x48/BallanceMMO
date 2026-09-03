#ifndef BALLANCEMMOSERVER_SESSION_HPP
#define BALLANCEMMOSERVER_SESSION_HPP
#include <cstdint>
#include <string>
#include <vector>

// Shared vocabulary for the collision-overhaul physics session protocol
// (docs/rooms-and-sessions-protocol.md 2.2, docs/collision-overhaul-design.md
// section 8). Kept as plain enums and POD structs so the client mod and the
// server agree on every wire value.
namespace bmmo::session {
    // Wire limits, mirroring entity/room.hpp's approach: hard caps so a
    // forged count can never make a reader allocate unbounded memory.
    constexpr size_t MAX_PLAYERS_PER_SESSION = 64;
    constexpr size_t MAX_INPUT_FRAMES = 8;
    constexpr size_t MAX_BODIES_PER_SNAPSHOT = 1024;
    constexpr size_t MAX_NAME = 64;
    constexpr size_t MAX_REASON = 256;
    // Shared cap for every per-recipe sub-list in a Physicalize event
    // (convex meshes, spheres, concave meshes); each list's own wire count is
    // a u8, so this only needs to keep a forged count from looking plausible,
    // not to prevent integer overflow.
    constexpr size_t MAX_CONVEX = 8;

    // session_event_msg payload discriminator.
    enum class event_type : uint8_t {
        Physicalize = 0,
        Unphysicalize = 1,
        Sector = 2,
        Finish = 3,
        BodyRevived = 4,
    };

    // session_snapshot_msg body kind.
    enum class body_kind : uint8_t {
        Ball = 0,
        Mechanism = 1,
    };

    // session_input_msg::input_frame::keys bits. The four leaves are the
    // SetPhysicsForce blocks in Ball Navigation (design doc 8.1); Shift/Space
    // are recorded but not consumed by the server.
    constexpr uint8_t KEY_LEAF_0 = 1u << 0;
    constexpr uint8_t KEY_LEAF_1 = 1u << 1;
    constexpr uint8_t KEY_LEAF_2 = 1u << 2;
    constexpr uint8_t KEY_LEAF_3 = 1u << 3;
    constexpr uint8_t KEY_SHIFT  = 1u << 4;
    constexpr uint8_t KEY_SPACE  = 1u << 5;

    // session_input_msg::input_frame::flags bits. nav_active mirrors the
    // client's current BallNav activate/deactivate state so the server can
    // reproduce the Key Event On/Off transitions (design doc 8.1).
    constexpr uint8_t INPUT_FLAG_PHYSICALIZED = 1u << 0;
    constexpr uint8_t INPUT_FLAG_PAUSED       = 1u << 1;
    constexpr uint8_t INPUT_FLAG_NAV_ACTIVE   = 1u << 2;

    // session_snapshot_msg::body_state::flags bits.
    constexpr uint8_t BODY_FLAG_SIMULATED         = 1u << 0;
    constexpr uint8_t BODY_FLAG_COLLISION_ENABLED = 1u << 1;

    // session_event_msg Physicalize `flags` bits.  SPAWN: the retail script
    // physicalized the ball at the level's current resetpoint
    // (CurrentLevel[0,3]) - a spawn or a respawn, not a trafo - so the
    // session's spawn impulse applies (design 9.10, session/spawn_impulse.hpp).
    constexpr uint8_t PHYSICALIZE_FLAG_SPAWN = 1u << 0;

    // session_start_msg::players entry: one member's spawn assignment.
    struct player_entry {
        uint32_t id = 0;
        uint8_t join_order = 0;
        uint8_t ball_type = 0;
        float spawn_position[3] = {};
        float spawn_rotation[4] = {};
    };

    // session_input_msg per-tick sample.
    struct input_frame {
        uint8_t keys = 0;
        float cam_right[3] = {};
        float cam_up[3] = {};
        float cam_dir[3] = {};
        uint8_t ball_type = 0;
        uint8_t flags = 0;
    };

    // Physicalize payload of session_event_msg: everything OnPhysicalize
    // reports about a ball's physics setup (design doc 8.1).
    struct ball_recipe {
        struct sphere {
            float center[3] = {};
            float radius = 0.f;
        };

        bool fixed = false;
        float friction = 0.f;
        float elasticity = 0.f;
        float mass = 0.f;
        bool start_frozen = false;
        bool enable_collision = true;
        bool calc_mass_center = false;
        float linear_damp = 0.f;
        float rot_damp = 0.f;
        float mass_center[3] = {};
        std::string collision_surface;             // <= MAX_NAME
        std::vector<std::string> convex_meshes;     // <= MAX_CONVEX, each <= MAX_NAME
        std::vector<sphere> balls;                  // <= MAX_CONVEX
        std::vector<std::string> concave_meshes;    // <= MAX_CONVEX, each <= MAX_NAME
    };

    // session_snapshot_msg per-body row. Position/rotation are double
    // precision because the IVP core's pose is itself double precision
    // (velocity is single precision); mirroring/correction must write these
    // back bit-for-bit.
    struct body_state {
        body_kind kind = body_kind::Ball;
        uint32_t owner = 0;      // ball: player id; mechanism: dictionary index
        std::string name;        // <= MAX_NAME; wire presence depends on `full` and `kind`
        double position[3] = {};
        double rotation[4] = {};
        float linear[3] = {};
        float angular[3] = {};
        uint8_t flags = 0;
    };
}

#endif //BALLANCEMMOSERVER_SESSION_HPP
