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
    // The physics session protocol this build speaks.  SessionStart carries
    // the server's, SessionReady the client's, and either side ends the
    // session when they differ - old and new builds must never share a
    // session, because they would not simulate alike and nothing would say
    // why.  Bump it for any change to what the two sides compute or compare:
    //   3  the sector union (session/sector_union.hpp): every member runs
    //      every sector any member has entered, activated at the stamped tick
    //      of the Sector event the server relays, and nothing is deactivated
    //      any more, on the server either; SessionStart carries sector_lead,
    //      the lead those events are stamped with.
    //   2  sector activation as an input (the server holds the level's own
    //      checkpoint activation; a client stamps its Sector event ahead and
    //      runs the manager at that tick), the order-independent world hash,
    //      lifecycle events sent without Nagle.
    //   1  everything before 2026-10-20 (never sent; a message without the
    //      field reads as 0).
    //   5  the relayed input stream is unreliable again, with a redundancy
    //      window: session_remote_input_msg carries, per other member, the
    //      frames of the last `relay_window` ticks, the camera omitted where
    //      it did not move (session/input_relay.hpp).
    //   6  a lifecycle event is on the wire more than once: reliable, then
    //      repeated unreliably on the following ticks, the first arrival
    //      counts (session/lifecycle.hpp event_seen / event_repeats).
    //   7  the substituted frames go back to their owner: every relayed
    //      datagram carries the recipient's acked tick, held_total and
    //      epoch, and a held section naming each of its own ticks the server
    //      substituted in the last 32 and the frame applied there (late-player
    //      design 1.1); it goes to every member every tick, header-only when
    //      there is nothing else, a solo player included.
    //   8  SessionReady carries the client's capabilities (`caps`,
    //      CAP_* below), and the server may send SessionLead to a client that
    //      declared CAP_LEAD, which answers with SessionLeadAck (late-player
    //      design 2.2).
    //   9  a lifecycle event is re-stamped where the server accepts it
    //      (late-player design 3.1): SessionEvent carries origin_tick (the
    //      sender's stamp) beside tick (the tick the server applies it at,
    //      never earlier than the stamp) and state bits (EVENT_STATE_*); it
    //      is relayed to every member, the owner included, and every
    //      receiver keys, orders and guards a player's events on the origin.
    constexpr uint32_t PHYSICS_PROTOCOL_VERSION = 9;   // 4: SessionStart carries spawn_spacing

    // session_ready_msg::caps bits: what the client can do beyond the
    // protocol's baseline.  CAP_LEAD: it applies a SessionLead (slews its
    // clock, amends and forces the held range) and acknowledges it; the
    // server sends SessionLead to nobody else (late-player design 2.2).
    constexpr uint32_t CAP_LEAD = 1u << 0;

    // Wire limits, mirroring entity/room.hpp's approach: hard caps so a
    // forged count can never make a reader allocate unbounded memory.
    constexpr size_t MAX_PLAYERS_PER_SESSION = 64;
    constexpr size_t MAX_INPUT_FRAMES = 8;
    constexpr size_t MAX_RELAY_FRAMES = 32;     // frames per member in one relayed datagram (the window's cap)
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

    // The tick a substituted input came from when it is the default frame:
    // nothing of the player had arrived yet (session/timeline.hpp
    // input_buffer::take, the held records of session_remote_input_msg).
    constexpr uint32_t INPUT_SOURCE_DEFAULT = 0xFFFFFFFFu;

    // session_snapshot_msg::body_state::flags bits.
    constexpr uint8_t BODY_FLAG_SIMULATED         = 1u << 0;
    constexpr uint8_t BODY_FLAG_COLLISION_ENABLED = 1u << 1;

    // session_event_msg Physicalize `flags` bits.  SPAWN: the retail script
    // physicalized the ball at the level's current resetpoint
    // (CurrentLevel[0,3]) - a spawn or a respawn, not a trafo - so the
    // session's spawn impulse applies (design 9.10, session/spawn_impulse.hpp).
    constexpr uint8_t PHYSICALIZE_FLAG_SPAWN = 1u << 0;

    // session_event_msg `state` bits (protocol 9, late-player design 3.1-3.2),
    // set by the server only; a client sends 0.  MOVED: the server applies the
    // event later than its origin (tick > origin_tick), because it arrived
    // after the session had passed its stamp.  SUPERSEDED: a birth overtaken
    // by a newer accepted Physicalize or Unphysicalize of the same player; it
    // is never applied and goes to its owner only.  REJECTED: the server
    // refused the event (a malformed recipe, plan step 18); owner only.
    constexpr uint8_t EVENT_STATE_MOVED      = 1u << 0;
    constexpr uint8_t EVENT_STATE_SUPERSEDED = 1u << 1;
    constexpr uint8_t EVENT_STATE_REJECTED   = 1u << 2;
    constexpr uint8_t EVENT_STATE_KNOWN = EVENT_STATE_MOVED | EVENT_STATE_SUPERSEDED | EVENT_STATE_REJECTED;

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
