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
    //  10  a resync anchors at base-1 (resync design 2.3, step A3):
    //      SessionAssign carries flags (ASSIGN_FLAG_STATE_FOLLOWS, on the
    //      reliable copy and every unreliable repeat), and the resynced member
    //      alone gets SessionResyncState - the server's state at the end of
    //      tick base-1: the rows, the sector union, the portable script
    //      record, every ball's navigation and the server's IVP savestate -
    //      instead of a full snapshot broadcast to every member; a member
    //      declares it can (SessionReady caps CAP_RESYNC_STATE).
    //  11  the trafo as a lockstep input (docs/trafo-replication-design.md,
    //      step T2): SessionEvent type Trafo (ball_type = the piece type,
    //      flags TRAFO_*, position = the explosion's referential), accepted
    //      through the server's per-(player, type) ledger and relayed to
    //      every member; SessionStart carries world_options (the sector
    //      placement design's u32 after `protocol`; bit WORLD_TRAFO_INPUTS);
    //      SessionReady caps CAP_TRAFO_INPUTS; SessionResyncState carries the
    //      trafo ledger; the portable script record carries the trafo sets'
    //      scripts (version 2) when the world option is on.
    //  12  the sector placement on the wire (docs/sector-placement-design.md
    //      5, step S5): world_options bit WORLD_PLACEMENT_CLONES (a world
    //      with it gives every PH row its own object); SessionReady carries
    //      the client's placement signature (`anchor_placement`, after
    //      `caps`) and CAP_PLACEMENT, and the server compares the signature
    //      of every member that declares the cap with its own world's -
    //      late joiners included - and refuses a member without the cap in
    //      a session with the bit.
    //  13  the server's wait in the relay header (late-player design 2.5,
    //      step R12, the user's decision of 2026-10-27): every
    //      session_remote_input_msg ends its header with wait u8, the
    //      recipient's wait at the server (session/lateness.hpp
    //      server_wait_meter), which a session-core client adds to its
    //      rollback reach's margin of 8.
    constexpr uint32_t PHYSICS_PROTOCOL_VERSION = 13;   // 4: SessionStart carries spawn_spacing

    // session_ready_msg::caps bits: what the client can do beyond the
    // protocol's baseline.  CAP_LEAD: it applies a SessionLead (slews its
    // clock, amends and forces the held range) and acknowledges it; the
    // server sends SessionLead to nobody else (late-player design 2.2).
    constexpr uint32_t CAP_LEAD = 1u << 0;
    // Protocol 10 (resync design 2.3, step A3): the client applies a
    // SessionResyncState (the session core).  A resync of a member without it
    // (the legacy retail path) is answered as before protocol 10: an
    // assignment without flags and a full snapshot broadcast to every member.
    constexpr uint32_t CAP_RESYNC_STATE = 1u << 1;
    // Protocol 11 (trafo-replication design 2.14, step T2): the client runs
    // the trafo as an input - its own trafo scripts held and sent as Trafo
    // events, every member's piece set built and driven by the relayed ones
    // (the session core).  The design named bit 1, which CAP_RESYNC_STATE
    // took at protocol 10.  A member that does not declare it is refused by
    // a session whose world_options has WORLD_TRAFO_INPUTS.
    constexpr uint32_t CAP_TRAFO_INPUTS = 1u << 2;
    // Protocol 12 (sector placement design 2.4, 5; step S5): the client's
    // host computes the placement signature of its world at every anchor
    // (session_capabilities::placement) and builds the copies when the
    // session's world_options has WORLD_PLACEMENT_CLONES.  SessionReady's
    // anchor_placement is then the host's signature and the server ends the
    // session when it differs from its own world's (late joiners too: the
    // signature does not depend on time).  A member without it (the legacy
    // retail path; the headless host before step S6, the retail core host
    // before S10) sends 0, which is not compared; it was refused by a
    // session with WORLD_PLACEMENT_CLONES, and since step B9DX by every
    // session (session_ready_refusal below: the bit marks the session core).
    constexpr uint32_t CAP_PLACEMENT = 1u << 3;

    // session_start_msg::world_options bits (protocol 11): what every world
    // of the session runs, fixed by the server for the session's life (never
    // a per-client choice).  Bit 2 is the world-agreement design's, bit 3
    // the record-format design's (neither built yet).
    // WORLD_PLACEMENT_CLONES (protocol 12, sector placement design 2.4, step
    // S5): every PH row whose object another row already places at another
    // matrix gets its own copy at the anchor (game/sector_placement.hpp), so
    // a player entering the next sector changes nothing in the previous one;
    // the server's config physics.placement_clones (default on since the
    // retail core host builds the copies, step S10).  WORLD_TRAFO_INPUTS
    // (trafo-replication design 2.14): every member has its own trafo piece
    // set in every world, the trafo's explosion and reset are Trafo events,
    // a piece never collides with a local body (PE_Balloon*).
    constexpr uint32_t WORLD_PLACEMENT_CLONES = 1u << 0;
    constexpr uint32_t WORLD_TRAFO_INPUTS = 1u << 1;

    // Who a session admits (the server's SessionReady handling; step B9DX,
    // the user's answer 7 of 2026-10-28, quoted in docs/roadmap.md 2: refuse
    // it outright, stop keeping clients without the session core
    // compatible).  The server serves a physics session only to a member
    // that runs the session core, whatever the session's world_options.  No
    // new bit and no protocol change: since steps S6 / S10 both session-core
    // hosts declare CAP_PLACEMENT in every SessionReady (the host capability
    // session_capabilities::placement, set unconditionally by the headless
    // host and the retail core host, with or without
    // WORLD_PLACEMENT_CLONES), and the legacy retail path (session core off)
    // declares no capability at all (caps 0).  A session with
    // WORLD_TRAFO_INPUTS still refuses a core member that does not run the
    // trafo inputs (the headless --no-trafo-cap / --no-script-replay arms).
    // A refused member leaves the session, not the room.
    enum class ready_refusal : uint8_t {
        none = 0,
        no_session_core,   // no CAP_PLACEMENT: the legacy retail path
        no_trafo_inputs,   // WORLD_TRAFO_INPUTS without CAP_TRAFO_INPUTS
    };
    inline ready_refusal session_ready_refusal(uint32_t caps, uint32_t world_options) {
        if (!(caps & CAP_PLACEMENT)) return ready_refusal::no_session_core;
        if ((world_options & WORLD_TRAFO_INPUTS) && !(caps & CAP_TRAFO_INPUTS)) return ready_refusal::no_trafo_inputs;
        return ready_refusal::none;
    }
    // The SessionEnd reason the refused member is sent (the retail mod shows
    // it in the chat: "Physics session N over (ended by the server: ...)").
    inline const char* ready_refusal_reason(ready_refusal refusal) {
        switch (refusal) {
        case ready_refusal::no_session_core:
            return "this server runs physics sessions only for clients with the session core, and this client runs "
                   "the legacy session path (turn it on: session core on)";
        case ready_refusal::no_trafo_inputs:
            return "this session runs the trafo as an input (world option trafo inputs) and "
                   "this client cannot (it needs the session core: session core on)";
        case ready_refusal::none: break;
        }
        return "";
    }

    // session_assign_msg::flags (protocol 10).  STATE_FOLLOWS: a
    // SessionResyncState of tick first_tick - 1 follows on the reliable
    // channel, to this member only; the client anchors its history there
    // and applies it as that tick (resync design 2.6).
    constexpr uint8_t ASSIGN_FLAG_STATE_FOLLOWS = 1u << 0;

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
        // Protocol 11 (trafo-replication design 2.2): a trafo's explosion or
        // reset of one piece type (ball_type) of the player's own set;
        // flags session/trafo_input.hpp TRAFO_*; position the explosion's
        // referential (zeros for a reset).
        Trafo = 5,
    };

    // One type set of the trafo ledger a resync or join state carries
    // (trafo-replication design 4.7; protocol 11): the three type sets of
    // EVERY set the server's world holds at the state's tick S - a member's
    // set that never exploded and a leaver's set between its final resets
    // and its retirement included (step T2's review: the portable record
    // lists every selected set's scripts, and a client must hold exactly
    // those sets to take it).  The explosion's seed follows from (session
    // seed, player, explode_applied, type).
    struct trafo_ledger_entry {
        uint32_t player = 0;
        uint8_t type = 0;             // 0 wood, 1 stone, 2 paper
        bool has_explosion = false;   // an explosion applied at or before S (explode_applied, position valid)
        uint32_t explode_applied = 0;
        float position[3] = {};
        bool has_bodies = false;      // its pieces have bodies at the end of S
        bool has_reset = false;       // a reset applied after that explosion, at or before S
        uint32_t reset_applied = 0;
        bool final = false;           // the type set's TRAFO_FINAL reset was applied at or before S
        uint32_t final_applied = 0;   // the set's newest TRAFO_FINAL reset (retirement: + history_ticks)
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
