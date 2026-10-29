#pragma once

// The simulation thread (design section 8.3): owns every physics_world, takes
// commands from the network thread through a queue, paces each running
// session with a tick_scheduler and hands snapshots back through callbacks.
//
// Thread model: every public method is thread-safe and returns immediately;
// the callbacks run on the simulation thread and must not block (the server
// sends the snapshot straight through GameNetworkingSockets, which is
// thread-safe).  Worlds are created on the simulation thread too, so a boot
// (about ten seconds) stalls the other sessions for that long; M3 runs one
// physics room at a time.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <entity/session.hpp>
#include <session/input_relay.hpp>
#include <session/journal.hpp>
#include <session/lateness.hpp>
#include <session/lead.hpp>
#include <session/lifecycle.hpp>
#include <session/timeline.hpp>
#include <session/trafo_input.hpp>

#include "physics_world.hpp"

namespace bmmo::sim {
    struct runner_config {
        bool trace = false;                 // world_options::trace
        std::filesystem::path game_root;
        uint32_t input_delay = 6;           // floor; each session sizes its own from ping
        uint32_t snapshot_interval = 2;
        uint32_t full_snapshot_interval = 66;
        uint32_t max_catch_up_ticks = 10;   // ticks simulated per loop when behind
        int seed = 1;
        // The session black box (design 9.15): every input the world consumed
        // goes to <journal_dir>/session_<id>_level<N>_<UTC>.bmjr, so a bug that
        // only happens in a live room can be replayed offline bit for bit.
        // Empty = record nothing.
        std::filesystem::path journal_dir;
        uint64_t journal_max_bytes = 256ull << 20;   // per session file; 0 = no limit
        uint32_t journal_checkpoint_ticks = 660;     // full body checkpoint cadence; 0 = none
        // Late-player design 2.6: the session stops waiting for one player at
        // that player's patience instead of the deadline, with the streak
        // waiver.  Off, the step test and the sleep are exactly what they were
        // before patience existed.  Stays off in deployments until the lead
        // adjustment (plan step 11) can lift a player patience cuts off.
        bool patience = false;
        // Late-player design 2.2-2.3, plan step 10: a lateness decision for a
        // client that declared CAP_LEAD becomes a SessionLead (on_lead), a
        // raise resets that player's input buffer over [N, E), and an
        // acknowledged delta moves its offset.  Without CAP_LEAD the decision
        // is logged and nothing is sent.  Off, the runner does exactly what it
        // did before: the decisions are observed and logged, nothing else.
        bool lead_adjust = false;
        // (The lifecycle barrier is gone, plan step 20: a tick whose input
        // shows a ball the world does not have is stepped without it, and
        // the S4 line says so - session/lifecycle.hpp, bodiless_tracker.)
    };

    // One lifecycle event the runner accepted (late-player design 3.1), for
    // the server to relay.  `applied` = max(origin, the session's next tick
    // at acceptance): the tick the world applies it at and every receiver
    // must too.  A superseded birth is never applied; it is handed over for
    // its owner's SUPERSEDED echo only.  `token` is the server's handle on
    // the message it submitted (0 = produced by the runner itself: the
    // leaver's Unphysicalize, or a server-stamped Trafo reset), and `intake`
    // when submit_event was called, for the S5 intake-to-send line.
    struct accepted_event {
        uint32_t session = 0;
        uint32_t player = 0;
        uint32_t origin = 0;
        uint32_t applied = 0;
        bmmo::session::event_type type = bmmo::session::event_type::Physicalize;
        bool superseded = false;
        // Protocol 11 (trafo-replication design 2.2): a Trafo event's
        // payload, for the events the runner makes itself (token 0: the
        // reset before a re-explosion, the deadline's, a leaver's final
        // ones - TRAFO_SERVER, origin = applied); a submitted one keeps the
        // message the server parked.  `rejected`: refused at acceptance (the
        // world option is off), REJECTED to its owner only, never applied.
        uint8_t ball_type = 0, flags = 0;
        float position[3] = {};
        bool rejected = false;
        // Not accepted at all: the session or the player was gone when the
        // event reached the runner.  Nothing is sent; the server only lets go
        // of the message it parked under `token`.
        bool dropped = false;
        uint64_t token = 0;
        std::chrono::steady_clock::time_point intake{};
        // Protocol 18 (world-agreement-gaps-design.md 2.7, step M1): the
        // life of `player` the event belongs to - the one the runner held
        // when it accepted it (a leaver's Unphysicalize and final resets:
        // the leaving life's).  The server relays it in the message.
        uint16_t life = 0;
    };

    // Step M1 (world-agreement-gaps-design.md 2.7): a life of a player
    // entered or left the runner's world.  `tick`: an add's J (the tick of
    // its PLAYER record, world->tick_index() at the add), a removal's R
    // (finish_leave's).  `join_order` the life's, read before the runner
    // forgets it.  `in_world`: false for a life whose add never reached the
    // world (a failed add, or a deferred rejoin cancelled by a second
    // leave) - no member was told of it, the server only lets go of its
    // join order.
    struct member_change {
        uint32_t session = 0;
        uint32_t player = 0;
        uint16_t life = 0;
        uint8_t join_order = 0;
        uint32_t tick = 0;
        bool in_world = true;
    };

    // One SessionLead for the server to send (late-player design 2.2).
    struct lead_notice {
        uint32_t session = 0;
        uint32_t player = 0;
        uint32_t id = 0;                 // the meter's decision id: the message's seq
        int k = 0;                       // the delta
        uint32_t held_from = 0, effective = 0;
        bmmo::session::input_frame held{};
        uint32_t held_source = bmmo::session::INPUT_SOURCE_DEFAULT;
        uint32_t epoch = 0;              // the base of the assignment it was decided under
    };

    struct session_snapshot {
        uint32_t session = 0;
        uint32_t tick = 0;
        bool full = false;
        std::vector<bmmo::session::body_state> bodies;
        // per player: the last tick whose input was actually applied
        std::vector<std::pair<uint32_t, uint32_t>> acked_inputs;
    };

    // The resync state of tick base - 1 for one player (resync design 2.3,
    // step A3; physics_world::resync_state), emitted by the simulation thread
    // after the step of that tick, for the server to send to that member
    // alone.  `emit_us`: what capturing it cost the step.
    struct resync_state_out {
        uint32_t session = 0, player = 0, tick = 0, base = 0;
        physics_world::resync_state state;
        uint64_t emit_us = 0;
    };

    // Outcome of a world boot.  On success anchor_hash/surfaces are what
    // SessionReady must match and spawn_* is the level's retail spawn pose.
    struct world_ready_info {
        uint32_t session = 0;
        bool ok = false;
        uint64_t anchor_hash = 0, anchor_surfaces = 0;
        float spawn_position[3] = {};
        float spawn_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        // Every resetpoint of the level (physics_world::resetpoints): a
        // respawn after a checkpoint is at one of the later ones.
        std::vector<VxVector> resetpoints;
        // The level's Physicalize_GameBall rows: the reference the event
        // validation compares a reported ball recipe with (design 9.4).
        std::vector<physics_world::ball_row> ball_rows;
        // Every P_Trafo_* of the level (a world with trafo inputs): what a
        // client's Trafo explosion is validated against (protocol 11,
        // session/trafo_input.hpp trafo_validate).
        std::vector<bmmo::session::trafo_point> trafo_points;
        // The world's placement signature at its anchor (protocol 12, sector
        // placement design 2.4): what a SessionReady with CAP_PLACEMENT must
        // carry, late joiners included.
        uint64_t anchor_placement = 0;
        // Protocol 16 (world-agreement-gaps-design.md 2.3, 5; step G3): the
        // world's mechanism-class probe signature, what a SessionReady's
        // anchor_probes must carry under WORLD_PROXIMITY_UNION.
        uint64_t anchor_probes = 0;
        std::string error;
    };

    // Start barrier (design 8.3): how long a session waits for its members to
    // report SessionReady after SessionStart went out, before the ones that
    // stayed silent are dropped and the rest start without them.  The server
    // checks the deadline from its own main loop rather than from a tick: a
    // session whose members never answered has no ticks at all.
    inline constexpr std::chrono::seconds kStartBarrierTimeout{8};

    struct session_callbacks {
        std::function<void(const world_ready_info&)> on_world_ready;
        std::function<void(const session_snapshot&)> on_snapshot;
        // Simulation thread (step A3): a requested resync state, after the
        // step of its tick; and a request whose base - 1 the world had
        // already stepped when the command ran (the server answers the
        // member again with a newer base).
        std::function<void(const resync_state_out&)> on_resync_state;
        std::function<void(uint32_t session, uint32_t player, uint32_t base)> on_resync_base_missed;
        // Every simulated tick, a solo session's too: the input frame the
        // world applied for each player, fresh or substituted, and for a
        // substitute the tick it came from.  The server relays them to the
        // other members so clients predict remote balls with the same input
        // (design 9.1), and the substituted ones back to their owner
        // (late-player design 1.2), whose client is otherwise never told.
        std::function<void(uint32_t session, uint32_t tick,
                           const std::vector<bmmo::session::applied_input>& applied)> on_inputs;
        // Simulation thread: a lead decision to send (runner_config::lead_adjust,
        // a client that declared CAP_LEAD).  The server sends it reliably with
        // repeats until the client acknowledges it (acknowledge_lead).
        std::function<void(const lead_notice&)> on_lead;
        // Simulation thread, from the command that accepted the event: every
        // accepted lifecycle event, re-stamped (late-player design 3.1).  The
        // server relays it from here to every member, the owner included.
        std::function<void(const accepted_event&)> on_event_accepted;
        // Step M1 (world-agreement-gaps-design.md 2.7), simulation thread:
        // the end of every add that reached the world (a posted add, or a
        // rejoin deferred to its old life's removal); an add the world
        // refused (`error`); the end of every finish_leave of a life the
        // world held, and a life dropped before it got there (in_world
        // false).  The server sends SessionMember from the first and the
        // last, a late joiner's SessionStart from the first.
        std::function<void(const member_change&)> on_player_added;
        std::function<void(const member_change&, const std::string& error)> on_player_add_failed;
        std::function<void(const member_change&)> on_player_removed;
        // A tick failed or the world died; the server ends the session.
        std::function<void(uint32_t session, const std::string& reason)> on_failed;
        std::function<void(const std::string&)> log;
    };

    class session_runner {
    public:
        session_runner(runner_config config, session_callbacks callbacks);
        ~session_runner();
        session_runner(const session_runner&) = delete;
        session_runner& operator=(const session_runner&) = delete;

        // Boots a world for the level; on_world_ready follows.  Each player is
        // paired with its join order (design 9.10): the world's per-player
        // slot and the spawn direction table both key off it.
        // `input_delay` is this session's, sized by the caller from the
        // members' round trips; 0 falls back to the runner's configured floor.
        // `spawn_impulse` is this session's kick speed in metres per second -
        // the same number session_start_msg carries to every member, so the
        // world is built with what the clients were told.  The runner has no
        // copy of its own: there is nowhere else for the world to get it.
        // `start_base` is the tick number the members present at the start are
        // numbered from (server.cpp start_tick_base, sim/late_tick.hpp): the
        // world's FIRST simulated tick carries it, so the world and every
        // member take their first step from their anchor under one number
        // (design 9.25 phase alignment).  0 keeps the old numbering from tick
        // 0.  It is passed here rather than set later because everything a
        // session derives from tick_index() - the input buffers, the journal
        // header, the cross-thread views - is fixed at the world's boot.
        // `note` goes into the journal as `start: <note>` (the room and its
        // members: what a human needs to recognise the recording later) and
        // `names` are the players' display names, parallel to `players` -
        // either the same length or empty.
        // `sector_lead` is the session's (SessionStart's): the lead the
        // trafo deadline is queued within (trafo-replication design 2.2).
        // `world_options` the session's session::WORLD_* bits (SessionStart
        // carries the same), fixed for the session's life: the world is
        // built with them and the journal's header records them.
        void create_session(uint32_t session, int level, const std::vector<std::pair<uint32_t, uint8_t>>& players,
                            uint32_t input_delay, uint32_t start_base, float spawn_impulse,
                            const std::string& note = {}, std::vector<std::string> names = {},
                            uint32_t sector_lead = 0, uint32_t world_options = 0);
        // `reason` goes into the journal as `end: <reason>` before it closes.
        void destroy_session(uint32_t session, const std::string& reason = {});
        // `name` is the player's display name for the journal's PLAYER record.
        // `life` (step M1): which of the player's lives in this session this
        // add starts - the server numbers them (its count of adds of the id,
        // from 1; the members present at create_session are life 1), so the
        // SessionStart it sends before the add can already list it.  An add
        // of a player whose old life is still leaving waits for that life's
        // removal and runs right after it, at the same tick (J-f): no event
        // of the old life is erased.
        void add_player(uint32_t session, uint32_t player, uint8_t join_order, uint16_t life,
                        const std::string& name = {});
        // A player that leaves with a ball gets its Unphysicalize from here
        // (late-player design 3.2), at the runner's own tick, through
        // on_event_accepted (token 0).  One with events accepted and relayed
        // but not applied yet stays in the world until they and that
        // Unphysicalize are applied (session_state::leaving).
        void remove_player(uint32_t session, uint32_t player);
        // The player anchored at first_tick; ticking starts when every player
        // of a not-yet-running session is ready.  A late join or a resync calls
        // this again on a running session with the base the server assigned:
        // input_buffer::reset() keeps the frames already queued at or after it
        // and never moves the cursor back, so the client's first batch after
        // the re-anchor is not lost and the last frame applied stays the
        // fallback for the ticks in between.
        // The assignment also starts that player's lateness meter over
        // (late-player design 2.1: its reference is measured anew after every
        // assignment).
        void player_ready(uint32_t session, uint32_t player, uint32_t first_tick);
        // Late-player design 2.2, 2.5: what the player's SessionReady
        // declared (session::CAP_*) and the lead of its first assignment (the
        // start base, or a late joiner's lead), against which the total lead
        // cap is measured.  Called once per player, before its first
        // player_ready; a resync keeps both.
        void set_player_lead(uint32_t session, uint32_t player, uint32_t caps, uint32_t assigned_lead);
        // The player's current round trip (the connection's m_nPing, not the
        // peak), which E is sized with (design 2.3).  The server posts it
        // once a second, with lead_adjust on only.
        void set_player_rtt(uint32_t session, uint32_t player, uint32_t rtt_ms);
        // The player's client acknowledged SessionLead `id`, having applied
        // `applied` ticks of it.  An ack of anything but the outstanding
        // decision is ignored.
        void acknowledge_lead(uint32_t session, uint32_t player, uint32_t id, int applied);
        // `arrival` is when the datagram reached the server, taken on the
        // network thread: the command runs on the simulation thread later, and
        // the lateness meter (design 2.1) measures the path and the client's
        // schedule, not this queue.
        void submit_input(uint32_t session, uint32_t player, uint32_t first_tick,
                          std::vector<bmmo::session::input_frame> frames,
                          std::chrono::steady_clock::time_point arrival);
        // Events due in the same tick are applied in the members' JOIN ORDER,
        // never in arrival order: the order is part of the determinism
        // contract (design 9.15), because it is the only one a client can
        // reproduce offline - its own journal cannot know which of two players
        // reached the server first, but every member knows every join order.
        // Within one player, by ORIGIN (`tick`, the stamp), then type rank.
        // The event is accepted on the simulation thread and re-stamped there
        // (late-player design 3.1): it is applied at max(tick, the session's
        // next tick), and on_event_accepted hands it back with `token`.
        void submit_event(uint32_t session, uint32_t player, uint32_t tick, lifecycle_event event,
                          uint64_t token = 0);
        void request_full_snapshot(uint32_t session);
        // Step A3 (resync design 2.3): the state of tick base - 1 for
        // `player`, emitted after that tick's step (on_resync_state).  A
        // newer request for the same player replaces it.  When the world has
        // stepped base - 1 already by the time the command runs (the base is
        // computed on the network thread from the published tick), nothing is
        // emitted and on_resync_base_missed says so.
        void request_resync_state(uint32_t session, uint32_t player, uint32_t base);
        // Diagnostics through the log callback.
        void describe(uint32_t session);
        // A NOTE record in the session's journal at the tick it will simulate
        // next (from any thread; posted).  Step G5: the first BodyRevived a
        // member sent under WORLD_PROXIMITY_UNION, dropped.
        void journal_note(uint32_t session, const std::string& text);

        // Readable from any thread: the tick the session will simulate next
        // (0 when unknown) and whether it is ticking.
        uint32_t current_tick(uint32_t session) const;
        bool running(uint32_t session) const;
        // Step RL1: the base of a re-anchor (a resync, a late join) taken
        // from the session's fixed deadline schedule - the tick it names for
        // now, plus lead (sim/late_tick.hpp reanchor_tick_on_schedule; signed
        // before the schedule's start, step RL1b), so deadline(base) - now is
        // lead + input_delay periods whatever the server's pace - floored at
        // current_tick + 2 (+ 3 when `resync_state` is false: a member
        // without CAP_RESYNC_STATE has no base-missed re-answer) so it is
        // never in the runner's past.  A session not yet started (no
        // schedule; unreachable from the server, which re-anchors only a
        // running session): current_tick + lead, the rule it replaced.
        uint32_t reanchor_tick(uint32_t session, uint32_t lead, bool resync_state) const;
        // Tick to assign to a late joiner anchoring now.
        uint32_t late_join_tick(uint32_t session) const;
        // Step M1 (J4): the largest removal tick of the session's leaving
        // lives, 0 when none.  A late joiner's base is not before it, so a
        // life the joiner is never told of has ended in the server's world
        // by its first tick.  A view value, like current_tick.
        uint32_t leaving_horizon(uint32_t session) const;
        size_t session_count() const;
        // The session's journal file, empty when it is not recording.
        std::string journal_path(uint32_t session) const;

    private:
        struct pending_event {
            uint32_t player;
            // The tick the sender stamped it for, which every side orders
            // and guards a player's events by (late-player design 3.1).
            uint32_t origin;
            // max(origin, the session's next tick at acceptance): the tick
            // the world applies it at.  Never behind the session.
            uint32_t applied;
            lifecycle_event event;
            // When submit_event was called: only to tell, for the S1
            // patience line, an event patience made late from one that was
            // late anyway.
            std::chrono::steady_clock::time_point arrival{};
        };
        struct session_state {
            uint32_t id = 0;
            int level = 0;
            std::unique_ptr<physics_world> world;
            std::set<uint32_t> players;
            // Join order per player: the rank step() sorts a tick's due events
            // by (the world keeps its own copy, but only for players it could
            // add, and the order is needed for every member).
            std::map<uint32_t, uint8_t> join_orders;
            std::set<uint32_t> ready;
            std::map<uint32_t, bmmo::session::input_buffer> inputs;
            // Late-player design 2.1, observe only (plan step 7): every
            // player's lateness meter, fed one sample per uplink datagram and
            // evaluated at every stepped tick.  Nothing acts on it yet: its
            // decisions are logged as S1 lines and never acknowledged, so each
            // player gets at most one per assignment.  `reported` keeps the
            // reference line to one per assignment.
            struct lateness_entry {
                bmmo::session::lateness_meter meter;
                bool reported = false;
            };
            std::map<uint32_t, lateness_entry> lateness;
            // Late-player design 2.6, behind runner_config::patience: each
            // player's streak waiver and what patience cost it.  Keyed like
            // `inputs` (not `lateness`), so a member added but not ready yet
            // can be waived too.  `cut` holds the ticks stepped early without
            // the player's frame (true = it was waived, false = past its
            // patience) until their deadline passes; a frame that arrives for
            // one of them before its deadline is one the deadline would have
            // taken, which is what the S1 patience line reports.  Members not
            // ready yet are left out of the cut counts: their ticks were never
            // going to get a frame, so they are not lateness.
            //
            // `late_events` counts this player's lifecycle and Sector events
            // that patience made late at the server: applied after their
            // stamp tick, which was stepped early with a member unsettled,
            // although they arrived before that tick's deadline.  The server
            // applies a late event at the current tick and the clients at the
            // stamp - the silent divergence of design 0 that only the re-stamp
            // (plan step 16) removes - so the flag-on arms must read it.
            struct patience_entry {
                bmmo::session::patience_waiver waiver;
                std::deque<std::pair<uint32_t, bool>> cut;
                uint64_t cut_patience = 0, cut_waiver = 0;       // ticks substituted by an early step
                uint64_t taken_patience = 0, taken_waiver = 0;   // of those, frames that beat the deadline
                uint64_t late_events = 0;
            };
            std::map<uint32_t, patience_entry> patience;
            // Late-player design 2.2-2.3, behind runner_config::lead_adjust:
            // each player's caps, assigned lead, round trip, newest uplink
            // tick, Sector margin and the decision outstanding.  Kept apart
            // from `lateness`, whose presence patience reads as "assigned".
            std::map<uint32_t, bmmo::session::lead_player> leads;
            uint64_t early_steps = 0;   // ticks stepped before the deadline with a member unsettled
            // Wall time of each step() since the last "step time" line (step
            // T3's H6: the server's tick cost against its 15.15 ms budget).
            std::vector<uint32_t> step_us;
            // The last such ticks, for the late-event attribution above.
            std::deque<uint32_t> early_ticks;
            std::map<uint32_t, uint32_t> acked;
            // The ORIGIN of each player's last applied Physicalize: an
            // Unphysicalize stamped at or before it is a copy that lost its
            // place (protocol 6) and would take the newer ball out.  Origins,
            // not applied ticks (late-player design 3.1): a death stamped
            // after a re-stamped birth must still be applied.
            std::map<uint32_t, uint32_t> last_physicalize_tick;
            uint64_t superseded_events = 0;
            // The frame the world applied for each player at the last step:
            // what the members' mirrors of a leaver go on with (the relay's
            // last frame of it), so what the server drives a leaver with
            // until it takes it out.
            std::map<uint32_t, bmmo::session::input_frame> last_frames;
            // Late-player design 3.2 (review of step 16): a player who left
            // with events accepted and relayed but not applied yet stays in
            // the world - out of every member structure, so nothing waits for
            // it - until `remove_at`, before whose step it is taken out
            // (lifecycle_leaver_removal_tick): its queued events, its
            // Unphysicalize among them, are applied here at the ticks every
            // receiver applies them at, and it is driven with `frame` until
            // then.
            struct leaving_player {
                uint32_t remove_at = 0;
                bmmo::session::input_frame frame{};
                // Step M1 (J-f): an add of the same id that came while this
                // life was leaving, run right after its finish_leave.
                bool rejoin = false;
                uint8_t rejoin_order = 0;
                uint16_t rejoin_life = 0;
                std::string rejoin_name;
            };
            std::map<uint32_t, leaving_player> leaving;
            // Step M1: each player's current life (the number its last add
            // carried); kept after its removal, for the events the leaving
            // life still has queued.
            std::map<uint32_t, uint16_t> lives;
            // The re-stamp and the superseded-birth rule (design 3.1), per
            // player, decided when an event is accepted.
            bmmo::session::lifecycle_acceptance acceptance;
            // Protocol 11 (trafo-replication design 2.2): the session's world
            // options, its Sector lead and the trafo ledger per (player, piece
            // type) - the applied tick of every accepted Trafo event, the
            // server's own resets (before a re-explosion, at the deadline, for
            // a leaver), the superseded resets.  Kept for a leaver too (its
            // final resets are in it): a rejoin's first explosion keeps the
            // span after them.
            uint32_t world_options = 0;
            uint32_t sector_lead = 0;
            bmmo::session::trafo_acceptance trafo;
            uint64_t trafo_server_resets = 0, trafo_superseded = 0, trafo_rejected = 0;
            bool trafo_on() const { return (world_options & bmmo::session::WORLD_TRAFO_INPUTS) != 0; }
            // Step G2 (world-agreement-gaps-design.md §2.2, §2.4), under
            // WORLD_PROXIMITY_UNION: the candidate set (player ids) after the
            // last step, and the guard: a step whose candidates at its top
            // differ from it (a ball holder removed between two steps - J-e,
            // step M1's) counts one in probe_set_drift (required 0).
            bool union_on() const { return (world_options & bmmo::session::WORLD_PROXIMITY_UNION) != 0; }
            std::vector<uint32_t> probe_set;
            bool have_probe_set = false;
            uint64_t probe_set_drift = 0;
            uint64_t superseded_births = 0, restamped_events = 0;
            std::vector<pending_event> events;   // sorted by applied tick on insertion
            uint32_t input_delay = 0;   // ticks of grace this session gives inputs
            uint32_t late_events = 0;   // applied after the tick they were stamped for (logged, capped)
            // Design 9.26: the start base B the members are numbered from.  The
            // world's first step is numbered B, and it is DUE B tick periods
            // after the assignment goes out - where a world counting from 0
            // used to reach B - so the members keep the wall-clock lead the
            // base gives them (their first frame happens half a round trip
            // after the assignment; stepping B at once made every input late
            // by the round trip and the starvation detector resynced everyone
            // two seconds into a real-network session).
            uint32_t start_base = 0;
            bmmo::session::tick_scheduler scheduler;
            bmmo::session::snapshot_cadence cadence;
            // Step R12 (protocol 13, lateness.hpp server_wait_ticks): each
            // member's wait at the step about to run, set by run() from the
            // step test's view right before step() (in `inputs` order; empty
            // = no wait known, 0), the meter of the last 4 windows' largest,
            // which the relay header carries (applied_input::wait), and the
            // value last journalled (a WAIT record when it changes).
            std::vector<double> step_waits;
            std::map<uint32_t, bmmo::session::server_wait_meter> waits;
            std::map<uint32_t, uint8_t> waits_journalled;
            // Step A3: the resync states owed, player -> base (emitted after
            // the step of base - 1).
            std::map<uint32_t, uint32_t> resync_requests;
            bool running = false;
            bool failed = false;
            // S4 (late-player design 3.2): the runs of ticks a member's frame
            // claimed a ball the world did not have.  The session steps them
            // (nothing waits for a Physicalize any more); the runs are logged
            // (capped by bodiless_logs) and noted in the box.
            bmmo::session::bodiless_tracker bodiless;
            uint32_t bodiless_logs = 0;
            // Runs no longer than input_delay + 1 ticks (every birth makes
            // one): counted, not logged.
            uint64_t bodiless_short = 0;
            // The black box (design 9.15).  Touched on the simulation thread
            // only; `journal_start` is the instant its header was written, the
            // origin every TICK record's `ms` is measured from.
            bmmo::session::journal_writer journal;
            std::chrono::steady_clock::time_point journal_start{};
        };
        // S4: every applied frame of `tick` that claims a ball the world
        // has none of, after the tick's events (step()).
        void note_bodiless(session_state& s, uint32_t tick, const std::vector<bmmo::session::applied_input>& applied,
                           bool recording);
        // Takes a leaver out of the world before the step of `tick`, with
        // the journal's PLAYER record at that tick.
        void finish_leave(session_state& s, uint32_t player, uint32_t tick);
        // Step M1: the add itself (the posted one, or a deferred rejoin's
        // after finish_leave), with on_player_added / on_player_add_failed.
        void add_now(session_state& s, uint32_t player, uint8_t join_order, uint16_t life, const std::string& name);
        // The player's current life (0: never added).
        static uint16_t life_of(const session_state& s, uint32_t player) {
            const auto it = s.lives.find(player);
            return it == s.lives.end() ? 0 : it->second;
        }
        // A Trafo decision of the ledger queued as a pending event (the
        // server's own resets and the accepted client events alike), and
        // for a runner-made one (token 0) the relay through
        // on_event_accepted.  `why` names a server reset in the log and box.
        void queue_trafo(session_state& s, const bmmo::session::trafo_decision& d, const float position[3],
                         std::chrono::steady_clock::time_point arrival, uint64_t token, const char* why);
        void observe_lateness(session_state& s, uint32_t tick);
        // What a decision does with lead_adjust on (design 2.3): the plan,
        // the buffer reset, the S1 lead line, the journal note and on_lead.
        void apply_lead_decision(session_state& s, uint32_t player, const bmmo::session::lateness_decision& d,
                                 const std::string& text);
        // Every member's view for patience_step_test at `tick`, in `inputs`
        // order, leads assigned (design 2.6).
        std::vector<bmmo::session::patience_member> patience_members(const session_state& s, uint32_t tick) const;
        double patience_lead(const session_state& s, uint32_t player) const;
        // The S1 patience lines: every 660 ticks from the run loop, and once
        // more (`final_reason` set) for a player that leaves and for every
        // player when its session ends, so no count after the last 660-tick
        // line is lost.  `only` limits it to one player.
        void report_patience(session_state& s, uint32_t tick, const char* final_reason = nullptr,
                             const uint32_t* only = nullptr);
        void log_patience_on(const session_state& s);
        void open_journal(session_state& s, float spawn_impulse, uint64_t anchor_hash,
                          uint64_t anchor_surfaces, uint32_t first_tick, uint64_t anchor_placement = 0);

        void run();
        void post(std::function<void()> command);
        void step(session_state& s);
        void emit_snapshot(session_state& s, bool full);
        void emit_resync_states(session_state& s, uint32_t tick, bool recording);
        void log(const std::string& text);
        void publish_state(const session_state& s);
        // Starts the session's deadline schedule and publishes it to the
        // cross-thread view (step RL1b: one place for both start paths).
        void start_ticking(session_state& s);

        runner_config config_;
        session_callbacks callbacks_;
        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable wake_;
        std::deque<std::function<void()>> commands_;
        bool stopping_ = false;
        std::map<uint32_t, std::unique_ptr<session_state>> sessions_;  // simulation thread only
        // Step B2SW: the harness's server-side stall, from the environment
        // (BMMO_SESSION_STALL=TICK:MS[,TICK:MS...], unset in every deployment):
        // the step of each TICK is held MS ms after the step test passed it,
        // as a slow step would be (the trial's 646 / 595 ms, a journal write
        // to a slow file system).  Read once, before the thread starts;
        // simulation thread only after that.
        std::map<uint32_t, uint32_t> debug_stalls_;

        // cross-thread view
        mutable std::mutex view_mutex_;
        // `schedule_start`: the session's tick_scheduler start (the first
        // tick's deadline less the input delay), with `first_tick` the whole
        // schedule reanchor_tick needs; both never move once `scheduled`
        // (start_ticking, the one place a schedule starts).
        struct view { uint32_t next_tick = 0; bool running = false; std::chrono::steady_clock::time_point start{}; uint32_t first_tick = 0;
                      std::string journal; std::chrono::steady_clock::time_point schedule_start{}; bool scheduled = false;
                      uint32_t leaving_horizon = 0; };
        std::map<uint32_t, view> views_;
    };
}
