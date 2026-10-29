// The session-client core's journal (session/client_journal.hpp,
// docs/session-client-core-design.md 1.2): the headless client's journal
// helpers, moved verbatim at step E2.  What the headless client took from its
// arguments and the engine comes from the core now: the path from the
// platform's journal_path (--journal), the cadence from core_config
// (--checkpoint-ticks), the clocks from now / utc_ms at the same sites, the
// bodies from body_state / list_bodies, and the log line from the core's
// logf.

#include "session/client_journal.hpp"

#include "session/bridge_recipe.hpp"
#include "session/client_core.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bmmo::session::core {

    uint32_t client_journal::cadence() const {
        return core_.config.journal_checkpoint_ticks ? core_.config.journal_checkpoint_ticks : kJournalCheckpointTicks;
    }

    uint32_t client_journal::ms() {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            core_.platform().now(clock_site::journal_ms) - journal_start_).count());
    }

    void client_journal::open(const bmmo::physics::world_hash& hash, const std::string& build_id,
                              const std::string& own_name, uint64_t anchor_placement) {
        const client_core_state& s = core_.s;
        const session_platform& platform = core_.platform();
        bmmo::session::journal_header header;
        header.kind = bmmo::session::journal_kind::client;
        header.session = s.session_;
        header.level = s.level_;
        header.seed = s.seed_;
        header.spawn_impulse = s.spawn_impulse_;
        header.input_delay = s.input_delay_;
        header.checkpoint_ticks = cadence();
        header.first_tick = s.tick_base_;
        header.anchor_hash = hash.pose;
        header.anchor_surfaces = hash.surfaces;
        header.build_id = build_id;
        header.own_player = s.own_id_;
        header.own_join_order = s.own_join_order_ < 0 ? 255 : static_cast<uint8_t>(s.own_join_order_);
        // Journal v5/v6 (steps T2, S5): the session's world options and the
        // anchor's placement signature, so a replay of this file builds the
        // world the session ran (the copies, the trafo sets) and checks the
        // placement against it.  Before S5 a client header carried no world
        // options (0).
        header.world_options = s.world_options_;
        header.mechanism_slots = s.mechanism_slots_;   // journal v8 (step Q7)
        header.anchor_placement = anchor_placement;
        const std::filesystem::path path = platform.journal_path ? platform.journal_path(header) : std::filesystem::path();
        if (path.empty()) return;
        close("a new session started");
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        header.utc_ms = platform.utc_ms(clock_site::journal_utc);
        journal_start_ = platform.now(clock_site::journal_start);
        std::string error;
        // Step SIO: the host's I/O strand writes the file, flushing as the
        // host's mode says (step IOF, batch by default); none: the tick
        // thread does, as before.
        if (!journal_.open(path, header, kJournalMaxBytes, error,
                           platform.io_strand ? platform.io_strand() : bmmo::session::io_poster{},
                           bmmo::session::kJournalMaxQueued,
                           platform.journal_flush ? platform.journal_flush() : bmmo::session::io_flush::batch)) {
            core_.logf("journal: %s", error.c_str());
            return;
        }
        // The headless client knows ids, not nicknames: only its own name
        // comes from the command line.
        std::string note = "start: session " + std::to_string(s.session_) + " level " + std::to_string(s.level_)
                         + " as " + own_name + "(" + std::to_string(s.own_id_) + ", join "
                         + std::to_string(s.own_join_order_) + "):";
        journal_announced_.clear();
        own_name_ = own_name;
        // Step M1: SessionStart's lives (from 0) name the start note; every
        // life's PLAYER records, a SessionMember's that came while loading
        // included, are written by the core's journal_member_lives below.
        for (auto& l: core_.s.lives_) {
            l.journaled_add = l.journaled_remove = false;
            if (l.from != 0) continue;
            const std::string name = l.entry.id == s.own_id_ ? own_name : std::to_string(l.entry.id);
            note += " " + name + "(" + std::to_string(l.entry.id) + ", join " + std::to_string(l.entry.join_order) + ")";
        }
        journal_.note(header.first_tick, note);
        first_tick_ = header.first_tick;
        core_.journal_member_lives();
        journal_last_checkpoint_ = 0;
        journal_have_checkpoint_ = false;
        journal_checkpoint_pending_ = false;
        core_.logf("journal: %s", path.string().c_str());
    }

    void client_journal::close(const std::string& reason) {
        if (!journal_.is_open()) return;
        journal_.note(core_.current_tick(), "end: " + reason);
        // Step SIO: the writer queue's high-water mark, in the log (never in
        // the file: its bytes are what they were before SIO).
        if (journal_.asynchronous())
            core_.logf("journal: writer queue peak %llu bytes", static_cast<unsigned long long>(journal_.peak_queued()));
        journal_.close();
    }

    void client_journal::note(const std::string& text) {
        if (journalling()) journal_.note(core_.current_tick(), text);
    }

    // Step M1: the PLAYER records come from the lives (add_player at J,
    // leave_player at R).  A record of a player no life names yet writes a
    // note once and nothing else - no guessed join order (it was 255).
    void client_journal::announce_player(uint32_t player, uint32_t tick) {
        if (player == 0 || journal_announced_.count(player)) return;
        for (const auto& l: core_.s.lives_)
            if (l.entry.id == player) return;
        journal_announced_.insert(player);
        if (journalling())
            journal_.note(tick, "p" + std::to_string(player)
                          + " has records before its member message: its PLAYER record follows from that message");
    }

    void client_journal::add_player(const bmmo::session::player_entry& entry, uint32_t from) {
        if (!journalling()) return;
        // At J (a life from the start: the journal's first tick), not before
        // the numbering's first tick: a late join or a resync leaves a hole
        // in the file below its base, and the replay restores the server's
        // state there (RESYNC) with every body of the life in it.
        const uint32_t lowest = core_.s.assigned_ && static_cast<int32_t>(core_.s.tick_base_ - first_tick_) > 0
                              ? core_.s.tick_base_ : first_tick_;
        const uint32_t tick = from == 0 ? first_tick_ : static_cast<int32_t>(from - lowest) < 0 ? lowest : from;
        const std::string name = entry.id == core_.s.own_id_ ? own_name_ : std::to_string(entry.id);
        journal_.player(tick, entry.id, entry.join_order, true, name, entry.life);
    }

    void client_journal::leave_player(uint32_t player, uint32_t tick) {
        if (!journalling()) return;
        const uint32_t lowest = core_.s.assigned_ && static_cast<int32_t>(core_.s.tick_base_ - first_tick_) > 0
                              ? core_.s.tick_base_ : first_tick_;
        journal_.player(static_cast<int32_t>(tick - lowest) < 0 ? lowest : tick, player, 0, false, {});
    }

    void client_journal::lifecycle_event(uint32_t player, const bmmo::session_event_msg& e) {
        if (!journalling()) return;
        announce_player(player, e.tick);
        bmmo::session::journal_event out;
        out.tick = e.tick;
        // Protocol 9: a relayed event's tick is the one the server applied
        // it at, and the world is handed that tick (the spawn impulse
        // index); the origin goes beside it (journal v3), and a replay of
        // a client journal orders a tick's events by it.  Our own events
        // carry their stamp in both.
        out.event_tick = e.tick;
        out.origin_tick = e.origin_tick;
        out.state = e.state;
        out.id = player;
        out.type = e.type;
        out.ball_type = e.ball_type;
        out.flags = e.flags;
        for (int k = 0; k < 3; ++k) out.position[k] = e.position[k];
        for (int k = 0; k < 9; ++k) out.rotation[k] = e.rotation[k];
        out.sector = e.sector;
        out.name = e.name;
        out.recipe = to_bridge_recipe(e.recipe);
        journal_.event(out);
    }

    void client_journal::rollback_correction(const bmmo::session::rollback_correction& c) {
        if (!journalling()) return;
        bmmo::session::journal_correction out;
        out.tick = c.tick;
        out.local_tick = c.local_tick;
        out.kind = c.kind;
        out.entity = c.entity;
        out.error_m = static_cast<float>(c.error_m);
        out.velocity_error = static_cast<float>(c.velocity_error);
        for (int k = 0; k < 3; ++k) {
            out.local_position[k] = c.local_position[k];
            out.server_position[k] = c.server_position[k];
        }
        journal_.correction(out);
        // A rollback, a too-far set and a freeze all moved bodies: the
        // next checkpoint says where they ended up.
        if (c.kind == 1 || c.kind == 5 || c.kind == 6) journal_checkpoint_pending_ = true;
    }

    void client_journal::body_correction(uint8_t kind, const std::string& entity, uint32_t tick, double error,
                                         const double server_position[3]) {
        if (!journalling()) return;
        bmmo::session::journal_correction c;
        c.tick = tick;
        c.local_tick = core_.current_tick();
        c.kind = kind;
        c.entity = entity;
        c.error_m = static_cast<float>(error);
        bmmo_physics_body_state local{};
        if (!entity.empty() && core_.platform().body_state(entity, local))
            for (int k = 0; k < 3; ++k) c.local_position[k] = local.position[k];
        if (server_position)
            for (int k = 0; k < 3; ++k) c.server_position[k] = server_position[k];
        journal_.correction(c);
        if (kind == 2 || kind == 4) journal_checkpoint_pending_ = true;
    }

    bool client_journal::checkpoint_due(uint32_t tick) const {
        const uint32_t cadence = this->cadence();
        // `tick % cadence`, the same rule the server's journal uses
        // (session_runner::step), so the two sides checkpoint the SAME
        // ticks and can be compared body by body.  A "every N since the
        // last one" rule drifts onto the opposite parity and then never
        // coincides with the server's at all - which is how a first attempt
        // at this comparison got 1055 client and 1320 server checkpoints
        // with zero ticks in common.
        if (cadence != 0 && tick % cadence == 0) return true;
        if (!journal_have_checkpoint_ || tick < journal_last_checkpoint_) return true;
        const uint32_t since = tick - journal_last_checkpoint_;
        return journal_checkpoint_pending_ && since >= std::min(cadence, kJournalCheckpointGap);
    }

    // Balls carry the OWNER, because the name cannot be an identity across
    // sides.  The server records a ball by owner and leaves the name empty;
    // the headless client names its own ball Ball_<type> whatever its id, and
    // names a mirror Ball_<type>_BMMO_<id> - except CopyObject is asked for
    // CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME, so Virtools increments the
    // trailing digits whenever that name is already taken by an earlier
    // lifetime, and the id in the name drifts away from the real one (it
    // was observed one and then two higher).  The owner is known here for
    // every ball, so record it and let the comparison use it.
    void client_journal::local_checkpoint(uint32_t tick, const std::string& own_ball) {
        const client_core_state& s = core_.s;
        const std::string& own_name = own_ball;
        const std::vector<bmmo_physics_body_state> bodies = core_.platform().list_bodies();
        std::vector<bmmo::session::body_state> out;
        out.reserve(bodies.size());
        for (const auto& body: bodies) {
            if (!body.movable) continue;
            bmmo::session::body_state state;
            // A ball by what it is (step P: the own ball, an entity it has
            // been, a mirror), not by the "Ball_" prefix: the trafo
            // explosion pieces share it and are mechanisms, journaled by name.
            const bool ball = (!own_name.empty() && own_name == body.name) || core_.is_ball(body.name);
            state.kind = ball ? bmmo::session::body_kind::Ball : bmmo::session::body_kind::Mechanism;
            state.owner = 0;
            if (state.kind == bmmo::session::body_kind::Ball) {
                if (!own_name.empty() && own_name == body.name) state.owner = s.own_id_;
                else
                    for (const auto& [id, remote]: s.remotes_)
                        if (remote.physicalized && remote.entity == body.name) { state.owner = id; break; }
            }
            state.name = body.name;
            for (int k = 0; k < 3; ++k) {
                state.position[k] = body.position[k];
                state.linear[k] = body.linear[k];
                state.angular[k] = body.angular[k];
            }
            for (int k = 0; k < 4; ++k) state.rotation[k] = body.rotation[k];
            state.flags = static_cast<uint8_t>((body.simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0u)
                | (body.collision_enabled ? bmmo::session::BODY_FLAG_COLLISION_ENABLED : 0u));
            out.push_back(std::move(state));
        }
        journal_.checkpoint(tick, bmmo::session::JOURNAL_CHECKPOINT_LOCAL, out);
        journal_last_checkpoint_ = tick;
        journal_have_checkpoint_ = true;
        journal_checkpoint_pending_ = false;
    }

}
