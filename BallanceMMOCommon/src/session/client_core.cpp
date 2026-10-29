// The shared session-client core (docs/session-client-core-design.md 1.1):
// begin, anchor, the hold, end; numbering, SessionAssign, the resync request
// and its limiter.  Step E1 built the object (the state and the platform it
// is given); E2 added the core's logf; E3 moves the session lifecycle,
// numbering, the hold, the resync request and the sends out of the headless
// client (session_client.cpp at f8b07e5), verbatim but for the platform calls
// (send, now, the hooks) and the legacy back-calls (design 6.1).
// E11 deleted the back-calls: end's own_body_guard_off is hooks.ended.

#include "session/client_core.hpp"

#include "message/session_ready_msg.hpp"
#include "message/session_resync_msg.hpp"
#include "session/spawn_impulse.hpp"

#include <cstdarg>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace bmmo::session {

    session_client_core::session_client_core(session_platform platform, session_capabilities caps)
        : platform_(std::move(platform)), caps_(caps) {
        if (const char* missing = missing_platform_operation(platform_, caps_))
            throw std::invalid_argument(std::string("session_client_core: the platform has no ") + missing);
        // The core's half of the rollback binding (design 2.4; E9: moved
        // from the headless client's rollback_world()).  Before each
        // replayed tick's scripts and step: the births deferred into this
        // replay (apply_event), made at their tick in the world as it stands
        // then, with the own beam stamped for it among them.
        // Step B1: then, in a Sector rewind, the union's tick re-derived
        // (resync design 2.8: events, then the union, then the step - the
        // server's order), before the host re-issues the tick's activations.
        // Step A3: the deaths a resync re-applies come after the births (the
        // remote lifecycle's order), before the union.
        // Step T3 (trafo-replication design 2.5, 4.2): the tick's trafo
        // inputs from the log before the union, as the live tick applies
        // them before its union pass (pre_step); the trafo frame runs first
        // in the replayed frame (script_replay::execute_frame's trafo list).
        // Step EL7 (gap L, L2): the births and the deaths in one drain, in
        // the server's order (drain_pending_lives).
        // Step EL8 (design 9.7 (a)): first the own entities' matrices of the
        // live frame before, for those without a body.
        world_hooks_.before_replayed_tick = [this](uint32_t tick) {
            write_own_matrices(tick);
            drain_pending_lives(tick);
            apply_trafo_inputs(tick, true);
            rederive_union_tick(tick);
        };
        // Step B1: a window-born body of a live pass the re-derivation moved
        // or forgot is not made at its live birth tick: no missing birth.
        world_hooks_.birth_moved = [this](const std::string&, uint32_t birth_tick) {
            return s.rederive_.active && s.rederive_.r.birth_moved(birth_tick);
        };
        // The mirrors a replayed tick drives: every predicted remote this
        // world has stepped and driven by then (driven_from: the stamp of
        // a birth made inside a replay, never a tick before a live make),
        // with the generation it has now.  A late
        // birth made by before_replayed_tick is not in the records of the
        // window it lands in (they were captured while the player was
        // dead); named here, it is fed its relayed frames from its stamp
        // and recorded from there, instead of coasting on its spawn
        // impulse to the live tick and being parked at that record by
        // every later correction.
        world_hooks_.replay_remotes = [this](uint32_t tick, std::map<std::string, uint32_t>& out) {
            for (const auto& [id, remote]: s.remotes_) {
                if (!remote.physicalized || !remote.navigation || static_cast<int32_t>(remote.driven_from - tick) > 0) continue;
                const auto generation = s.generations_.find(remote.entity);
                out[remote.entity] = generation == s.generations_.end() ? 0u : generation->second;
            }
        };
        // The mechanism bodies a replayed tick has: the same list the live
        // record takes (tracked.mechanisms in the frame), so a body the
        // replay made before the live frames did is recorded from the
        // replayed tick that has it.
        world_hooks_.replay_mechanisms = [this]() { return movable_mechanisms(); };
        // Step G3 (world agreement 2.5): the proximity union's place point
        // of every replayed tick - the replayed world's balls at the end of
        // t-1, minus what the replay holds out - when the session runs it.
        world_hooks_.place_probes = [this](uint32_t tick, const std::function<bool(const std::string&)>& held_out) {
            if (s.probes_active_) place_probes(tick, held_out, true);
        };
        // Every decision the engine takes about a snapshot, straight into
        // the box: no log parsing needed to know what this client did.
        world_hooks_.on_correction = [this](const bmmo::session::rollback_correction& c) { journal_.rollback_correction(c); };
        // A moved own birth (late-player design 3.3) is destroyed at the
        // restore: its birth record is its applied tick's, where the replay
        // makes it again the server's way (apply_own_lives).
        world_hooks_.destroy_own_birth = [this](const std::string& entity, uint32_t birth_tick, bool& destroyed) {
            return destroy_moved_own_birth(entity, birth_tick, destroyed);
        };
        // Step LB: a mirror born inside the window is taken out at the
        // restore and made again at its birth tick by the core's births, as
        // the first replay (or the live frame) made it - stepped with the
        // other bodies of that tick and driven with the frame the history
        // has for it now - instead of being parked inert and re-posed at a
        // birth record made with the inputs known then.
        world_hooks_.remake_mirror_birth = [this](const std::string& entity, uint32_t birth_tick) {
            return remake_mirror_birth(entity, birth_tick);
        };
        world_hooks_.own_body = [this](const std::string& entity) { return entity == s.own_body_entity_; };
        // Step EL7: every mirror entity this session made, not only the
        // current ones - the reconcile of an ended life points the remote
        // back at the A-life, and the later life it took out is made again
        // from the inputs at its birth tick (rollback_world::
        // birth_from_inputs).  An old life's entity has no body outside such
        // a replay, so the binding's other tests answer as before for it.
        world_hooks_.mirror = [this](const std::string& entity) {
            for (const auto& [id, remote]: s.remotes_) if (remote.entity == entity) return true;
            return s.mirror_entities_.count(entity) != 0;
        };
        world_hooks_.ball = [this](const std::string& entity) { return is_ball(entity); };
        // Rollback-engine gaps design 2.4 (step B8EG, item EGB):
        // destroy_own_birth's test alone (destroyable_own_birth is the one
        // both use), for bind_session_world's keeps_parked.
        world_hooks_.own_birth_destroyable = [this](const std::string& entity, uint32_t birth_tick) {
            return destroyable_own_birth(entity, birth_tick) != nullptr;
        };
        // Step OWNB: the own birth destroy_moved_own_birth just queued.
        world_hooks_.own_birth_remade = [this](const std::string& entity) {
            for (const auto& due: s.own_due_)
                if (due.scripted && !due.death && due.entity == entity) return true;
            return false;
        };
        // Step RR: a snapshot row's body at a replayed tick, by the server's
        // identity - the mirror the player's ball is driven as by then (the
        // replay_remotes rule), or the resolved mechanism.  The own ball is
        // the engine's (the replayed record names it).
        // Gap L, L2 (rollback-engine gaps design 2.3.3, step EL7): the
        // core's half of rollback_world::restore_lives - the mirrors (the
        // own ball's half is L3, step G8).
        // Step EL8 (L3): the own ball's half first (design 2.3.1 step 3:
        // the balls, own and mirrors).
        world_hooks_.restore_lives = [this](uint32_t anchor, uint32_t now, const std::vector<ended_life>& ended, bool,
                                            std::map<std::string, life_outcome>& out) {
            reconcile_own_lives(anchor, now, ended, out);
            reconcile_remote_lives(anchor, now, ended, out);
        };
        world_hooks_.row_entity = [this](bmmo::session::body_kind kind, uint32_t owner, uint32_t tick) -> std::string {
            if (kind != bmmo::session::body_kind::Ball) return s.mechanism_tracking_.entity_of(owner);
            if (owner == s.own_id_) return {};
            const auto it = s.remotes_.find(owner);
            if (it == s.remotes_.end() || !it->second.physicalized || !it->second.navigation
                    || static_cast<int32_t>(it->second.driven_from - tick) > 0)
                return {};
            return it->second.entity;
        };
    }

    // Every movable body but the balls (step P): the balls are the own
    // ball's entities and the mirrors (is_ball), each tracked by entity as
    // own and remote; everything else - the trafo explosion pieces included,
    // which share the balls' "Ball_" prefix - is a mechanism the records
    // carry and a rollback restores.
    std::vector<std::string> session_client_core::movable_mechanisms() const {
        std::vector<std::string> names;
        for (auto& name: platform_.movable_bodies())
            if (!is_ball(name)) names.push_back(std::move(name));
        return names;
    }

    // The headless client's logf wrote the formatted text and a newline; the
    // platform's log writes the line it is given and the newline, so the
    // output is the same bytes.  A growing buffer: the status line alone runs
    // past 1.5 KB.
    void session_client_core::logf(const char* format, ...) {
        std::string text(256, '\0');
        va_list args;
        va_start(args, format);
        va_list again;
        va_copy(again, args);
        const int length = std::vsnprintf(text.data(), text.size(), format, args);
        if (length >= 0 && static_cast<size_t>(length) >= text.size()) {
            text.resize(static_cast<size_t>(length) + 1);
            std::vsnprintf(text.data(), text.size(), format, again);
        }
        va_end(again);
        va_end(args);
        text.resize(length > 0 ? static_cast<size_t>(length) : 0);
        platform_.log(log_level::info, text);
    }

    // ---------------------------------------------------------- session
    // SessionStart (session_client.cpp begin_session, less the host's own
    // resets and the engine reboot, which the host does after this returns).
    begin_result session_client_core::begin(const bmmo::session_start_msg& msg, const core_config& cfg) {
        // A server of another protocol (entity/session.hpp) computes and
        // compares other things: refuse, loudly, rather than play a session
        // that cannot agree.
        if (msg.protocol != bmmo::session::PHYSICS_PROTOCOL_VERSION
                && !(cfg.accept_protocol != 0 && msg.protocol == cfg.accept_protocol)) {
            logf("session %u refused: the server runs physics protocol %u, this client runs %u", msg.session,
                 msg.protocol, bmmo::session::PHYSICS_PROTOCOL_VERSION);
            return begin_result{true};
        }
        // A box of the previous session is closed with its own numbering,
        // before begin_session resets the tick base under it.
        journal_.close("a new session started");
        config = cfg;
        s.rollback_.set_verbose(config.verbose);
        s.rollback_.set_recheck_on_restore(config.restore_recheck);
        s.rollback_.set_two_pass_recheck(config.two_pass_recheck);
        s.rollback_.set_restore_impulses(config.restore_impulses);
        s.rollback_.set_restore_quaternions(config.restore_quaternions);
        s.rollback_.set_restore_clock(config.restore_clock);
        s.rollback_.set_restore_calm(config.restore_calm);
        s.rollback_.set_restore_unit(config.restore_unit);
        s.rollback_.set_restore_contacts(config.restore_contacts);
        s.rollback_.set_contact_flags(config.contact_flags);
        s.rollback_.set_sleep_as_recorded(config.sleep_as_recorded);
        s.rollback_.set_replay_scripts(config.script_replay);
        s.rollback_.set_exact_restore(config.savestate);
        s.rollback_.set_script_log_cap(config.script_diff_logs);
        s.rollback_.set_trace_plan(config.trace_plan);
        // Late-player plan step 12: every session starts at the floor with
        // the default history (a previous session's deeper reach is its
        // own), or at the pinned reach.  Set without apply_reach's log line:
        // the tick base is still the previous session's here, and the
        // starting reach is in the status line anyway.
        s.reach_meter_.clear();
        s.reach_changes_ = 0;
        s.reach_logs_ = 0;
        // Step R12: the session's own rewinds measure its cap (its level's
        // world is what a rewind restores and replays), and its server's
        // headers say the wait.
        s.rollback_.rewind_cost().clear();
        // Its samples are the flow's clock reads (the cap decides the
        // reach): the headless client captures them, retail reads its clock.
        s.rollback_.set_cost_clock([this](clock_site site) { return platform_.now(site); });
        s.server_wait_ = 0;
        s.server_wait_known_ = false;
        s.rollback_.thresholds().history_ticks = bmmo::session::rollback_thresholds{}.history_ticks;
        s.rollback_.set_reach(config.reach_ticks ? std::clamp(config.reach_ticks, bmmo::session::kReachFloorTicks,
                                                              bmmo::session::kReachCapTicks)
                                                 : bmmo::session::kReachFloorTicks);
        s.reach_max_ = s.rollback_.thresholds().max_resim_ticks;
        s.session_ = msg.session;
        s.snapshot_interval_ = msg.snapshot_interval;
        s.input_delay_ = msg.input_delay;
        s.sector_lead_ = msg.sector_lead;
        s.seed_ = msg.seed;
        s.spawn_impulse_ = msg.spawn_impulse;
        s.spawn_spacing_ = msg.spawn_spacing;
        s.level_ = msg.map.level > 0 ? msg.map.level : config.level;
        // Step M1 (world-agreement-gaps-design.md 2.8): the lives listed in
        // SessionStart, each from the start (from 0); SessionMember adds the
        // later ones.
        s.lives_.clear();
        for (const auto& p: msg.players) {
            bmmo::session::member_life life;
            life.entry = p;
            s.lives_.push_back(life);
        }
        s.parked_events_.clear();
        s.early_removals_.clear();
        s.member_messages_ = s.member_copies_ = s.events_parked_ = s.parked_applied_ = 0;
        s.member_life_outside_ = s.mirrors_outside_lives_ = s.own_other_life_echoes_ = s.catchup_unknown_lives_ = 0;
        s.own_join_order_ = -1;
        s.own_member_life_ = 0;
        for (const auto& p: msg.players) {
            if (p.id != s.own_id_) continue;
            s.own_join_order_ = p.join_order;
            s.own_member_life_ = p.life;
        }
        s.own_group_ = "P#" + std::to_string(s.own_join_order_ < 0 ? 63 : s.own_join_order_);
        logf("session %u starting: level %d, %zu players, join order %d, life %u, seed %d, input delay %u, sector lead %u",
             s.session_, s.level_, msg.players.size(), s.own_join_order_, static_cast<unsigned>(s.own_member_life_), s.seed_,
             static_cast<unsigned>(s.input_delay_),
             static_cast<unsigned>(s.sector_lead_));
        // Fresh runtime state.
        s.assigned_ = false;
        s.held_ = false;
        s.hold_polls_ = 0;
        s.tick_base_ = 0;
        s.frames_since_anchor_ = -1;
        s.input_history_.clear();
        s.previous_cam_valid_ = false;
        s.corrector_.clear();
        s.mechanism_correctors_.clear();
        s.mechanism_tracking_.clear();
        s.generations_.clear();
        s.amend_failures_ = 0;
        s.own_life_.clear();
        clear_own_window_state();   // step EL8
        s.own_echoes_.clear();
        s.own_echoes_stale_.clear();
        s.own_unanswered_.clear();
        s.own_moved_unphysicalize_.clear();
        s.beam_requested_ = false;
        s.own_reports_.clear();
        s.held_lifecycle_.clear();
        s.remotes_.clear();
        s.relay_seen_.clear();
        s.held_seen_.clear();
        s.held_stale_seen_.clear();
        s.held_header_known_ = false;
        s.acked_header_known_ = false;
        s.held_floor_ = 0;
        s.resync_window_ = false;
        reset_held_epoch("a new session");
        // Decision ids are unique per player for a session's life; the
        // offset is this session's.
        s.lead_seen_.clear();
        s.lead_offset_ = 0;
        s.pending_relay_.clear();
        s.event_seen_.clear();
        s.own_event_repeats_.clear();
        s.remote_last_physicalize_.clear();
        s.remote_last_unphysicalize_.clear();
        s.own_physicalized_ = false;
        s.own_body_entity_.clear();
        s.own_ball_entities_.clear();
        s.mirror_entities_.clear();
        s.mirror_owners_.clear();
        s.probes_active_ = false;
        s.probe_places_live_ = s.probe_places_replayed_ = s.probe_two_bodies_ = 0;
        s.revived_local_ = 0;
        s.start_sector_pending_ = false;
        s.start_sectors_.clear();
        s.sectors_.reset();
        s.sectors_requested_.clear();
        s.sector_cell_ = 0;
        s.chain_cell_ = 0;
        for (auto& pass: s.recent_passes_) pass = {};
        s.phase_misses_ = 0;
        // Step B1: the union's rollback state and its counters.
        s.sector_stamps_.clear();
        s.sector_stamp_seq_ = 0;
        s.union_records_.clear();
        s.pass_log_.clear();
        s.sector_late_.clear();
        s.rederive_ = client_core_state::union_rederivation{};
        s.last_rewind_rederived_ = false;
        s.sector_rewinds_ = s.sector_rewinds_missed_ = s.sector_rewind_depth_ = 0;
        s.sector_passes_forgotten_ = 0;
        // Step A3: no exact resync pending, no state, no hold, no logs.
        s.resync_exact_ = false;
        s.resync_base_ = 0;
        s.have_resync_state_ = false;
        s.resync_state_ = bmmo::session_resync_state_msg{};
        s.resync_held_ = false;
        s.resync_join_ = false;   // step A5
        s.life_log_.clear();
        s.pending_deaths_.clear();
        // R1: the retail policies' and readers' state, and no frame
        // prepared (post_step refuses one no pre_step of this session made).
        s.own_last_birth_ = {};
        s.snapshots_without_own_ = 0;
        s.own_backstops_ = 0;
        s.last_backstop_ = {};
        // Step B2SW (U30): the run of unmatched snapshots and the resyncs it
        // asked for are this session's (the retail host's copy reset both here).
        s.consecutive_unmatched_ = 0;
        s.unmatched_resyncs_ = 0;
        // Step T3: the trafo as an input runs in this session when the
        // server's world option says so and this host can (design 2.14).
        s.world_options_ = msg.world_options;
        s.trafo_active_ = caps_.trafo_inputs && (msg.world_options & WORLD_TRAFO_INPUTS) != 0;
        s.trafo_log_.clear();
        s.trafo_sets_.clear();
        s.trafo_pieces_.clear();
        s.trafo_pending_.clear();
        s.input_rewind_trafo_ = false;
        s.trafo_sent_ = s.trafo_received_ = s.trafo_applied_ = s.trafo_replayed_ = 0;
        s.trafo_late_ = s.trafo_missed_ = s.trafo_covered_ = s.trafo_leaks_ = 0;
        s.trafo_echo_moved_ = s.trafo_echo_dropped_ = 0;
        s.trafo_sets_made_ = s.trafo_set_failures_ = s.trafo_run_failures_ = 0;
        s.trafo_unmade_ = s.trafo_remade_ = s.trafo_retired_ = 0;
        s.trafo_leak_logs_ = 0;
        if (msg.world_options & WORLD_TRAFO_INPUTS)
            logf("session %u: the trafo is an input (world option trafo inputs)%s", s.session_,
                 s.trafo_active_ ? "" : ", which this host cannot run (no trafo_inputs capability)");
        // Step S5 (sector placement design 2.4): the host builds the copies
        // at its anchor; a host that cannot is refused by the server at its
        // SessionReady (no CAP_PLACEMENT).
        if (msg.world_options & WORLD_PLACEMENT_CLONES)
            logf("session %u: every PH row gets its own object (world option placement clones)%s", s.session_,
                 caps_.placement ? "" : ", which this host cannot build (no placement capability)");
        // Step G3 (world-agreement-gaps-design.md 2.4, 2.5): every world
        // places the mechanism proximity blocks by the union's rule; a host
        // that cannot sends no probe signature and the server refuses it.
        s.probes_active_ = proximity_union_active();
        if (msg.world_options & WORLD_PROXIMITY_UNION)
            logf("session %u: the mechanism proximity blocks follow every ball (world option proximity union)%s",
                 s.session_, s.probes_active_ ? "" : ", which this host cannot place (no place_probes): the server will "
                                                     "refuse its signature");
        frame_ = frame_scratch{};
        return begin_result{};
    }

    void session_client_core::level_requested() {
        s.phase_ = client_core_state::phase::loading;
    }

    // The anchor's SessionReady half (session_client.cpp anchor(), from the
    // phase change on); the host's anchor work comes before it.
    void session_client_core::anchor(const anchor_report& report) {
        // Sectors are inputs to the whole session (session/sector_union.hpp):
        // sector 1 is the level's own on every side, and the cell is what
        // the level wrote by the anchor, so the own sector poll takes only
        // a later write for a crossing (E8: the headless anchor set both
        // before its anchor work; nothing in that work reads them).
        s.sectors_requested_ = {1};
        s.sector_cell_ = report.sector_cell;
        s.chain_cell_ = report.sector_cell;   // step B2SX: a private copy starts from the anchor's cell
        s.frames_since_anchor_ = 0;
        s.phase_ = client_core_state::phase::running;
        if (platform_.hooks.numbering_restarted) platform_.hooks.numbering_restarted(clock_site::anchor_origin);
        // The window: line starts over with the session (its first call
        // takes the base, prints nothing), and --hold-event counts the
        // N-th own event of this session.
        s.window_index_ = 0xFFFFFFFFu;
        s.window_lag_.clear();
        s.window_inrew_src_.clear();
        drop_pending_rewind();
        s.resync_in_flight_ = false;
        s.resync_owed_ = false;
        // Hold the world here until the tick base arrives (design 9.25
        // phase alignment): the first frame this client steps must be the
        // first frame the server's world steps, the one both of them number
        // with the start base.  The network keeps being polled while we
        // wait; nothing else in the frame runs, so no input, no journal
        // TICK record and no rollback record is made for a tick that does
        // not exist.
        s.held_ = true;
        s.hold_polls_ = 0;
        s.hold_deadline_ = platform_.now(clock_site::anchor_hold) + kAnchorHoldTimeout;
        bmmo::session_ready_msg ready;
        ready.session = s.session_;
        ready.first_tick = 0;
        ready.anchor_hash = report.hash.pose;
        ready.anchor_surfaces = report.hash.surfaces;
        ready.physics_sha256 = report.physics_sha256;
        ready.build_id = report.build_id;
        // A client that applies a SessionLead (late-player design 2.4:
        // amend, force, slew, acknowledge) says so, and the server may send
        // it one and anchor its late join and resyncs at its own lead
        // (design 2.3: CAP_LEAD iff the host can slew).
        // Protocol 10 (step A3): a core that can apply the resync state says
        // so - with rollback on, since the apply is a re-step of the history
        // (on_session_assign takes the exact path only then).  A core with
        // rollback off gets the legacy answer, a full snapshot, which the
        // server sends only to a member without the cap.
        const bool resync_state = config.rollback;
        // Protocol 11 (trafo-replication design 2.14, step T3): a host that
        // builds the piece sets and runs the trafo inputs says so; a session
        // with the world option refuses a member without it.
        ready.caps = (caps_.slew ? bmmo::session::CAP_LEAD : 0u) | (resync_state ? bmmo::session::CAP_RESYNC_STATE : 0u)
                   | (caps_.trafo_inputs ? bmmo::session::CAP_TRAFO_INPUTS : 0u);
        // Protocol 12 (sector placement design 2.4, step S5): a host that
        // computes the placement signature says so and sends it; the server
        // compares it with its own world's (late joiners too).  Without the
        // capability: 0, not compared.
        const uint64_t placement = caps_.placement ? report.placement : 0;
        if (caps_.placement) ready.caps |= bmmo::session::CAP_PLACEMENT;
        ready.anchor_placement = placement;
        // Protocol 16 (world agreement 2.3, 5; step G3): the mechanism-class
        // probe signature of this world's rewire, always filled (0 when the
        // session does not run the union); the server compares it under
        // WORLD_PROXIMITY_UNION, late joiners included.
        ready.anchor_probes = s.probes_active_ ? report.probes : 0;
        ready.serialize();
        platform_.send(ready.raw.str(), send_mode::reliable);
        logf("anchored after %d load ticks: pose %016llx surfaces %016llx cores=%d factor=%.6f", report.load_ticks,
             static_cast<unsigned long long>(report.hash.pose), static_cast<unsigned long long>(report.hash.surfaces),
             report.hash.cores, report.hash.time_factor);
        if (caps_.placement)
            logf("anchor placement signature %016llx (world option placement clones %s)",
                 static_cast<unsigned long long>(placement), (s.world_options_ & WORLD_PLACEMENT_CLONES) ? "on" : "off");
        if (s.world_options_ & WORLD_PROXIMITY_UNION)
            logf("anchor probe signature %016llx (world option proximity union)",
                 static_cast<unsigned long long>(ready.anchor_probes));
        // The box starts where the session does: at the anchor the server
        // has to agree with, before the tick base is known.
        journal_.open(report.hash, report.build_id, report.own_name, placement);
        // Step BALA (the airship rule of 2026-10-28): the zone over this
        // world, from the level's airship as the anchor finds it - its
        // starting pose - with the own ball's group.
        s.own_airship_tick_ = 0;
        s.airship_active_ = false;
        if (platform_.airship_zone) {
            double box[6] = {};
            std::string error;
            const int r = platform_.airship_zone(true, s.own_group_, box, error);
            s.airship_active_ = r == 1;
            s.airship_box_logged_ = false;
            if (r < 0) logf("airship zone: %s", error.c_str());
            else if (r == 0) logf("airship zone: the level has no airship");
            else logf("airship zone on, own group %s (the box is taken once the platform has its body)", s.own_group_.c_str());
        }
        // Step SPGH (the user's spawn ghost rule of 2026-10-28): every ball
        // born a ghost in its spawn area, solid once it left the area and
        // overlaps no other ball - the physics layer's, in every world alike.
        s.spawn_ghost_active_ = false;
        s.own_spawn_ghost_ = false;
        if (bmmo::session::kSpawnGhost && platform_.spawn_ghost) {   // step SPX: the ghost-off build skips it
            double info[7] = {};
            std::string error;
            const int r = platform_.spawn_ghost(true, bmmo::session::kSpawnGhostRespawns, bmmo::session::kSpawnGhostRadius,
                                                info, error);
            s.spawn_ghost_active_ = r == 1;
            if (r < 0) logf("spawn ghost: %s", error.c_str());
            else if (r == 1)
                logf("spawn ghost on: %.0f spawn areas of %.3f m (the level start and the resetpoints), respawns %s", info[0],
                     info[1], bmmo::session::kSpawnGhostRespawns ? "included" : "excluded");
        }
        // Step T3 (design 2.4): every member's piece set and the own one, by
        // player id, after the host's quiesce (its anchor work): the server
        // built the same at add_player.
        if (s.trafo_active_) {
            std::set<uint32_t> members;
            for (const auto& l: s.lives_) members.insert(l.entry.id);
            members.insert(s.own_id_);
            for (uint32_t player: members) ensure_trafo_set(player, "the anchor");
        }
    }

    // Design 9.25 phase alignment: the world stands at the anchor until
    // SessionAssign arrives.  Its first step and the server world's first
    // step are then the same tick number, so the two worlds are the same
    // number of steps from their anchors at every tick of the session and a
    // script-driven mechanism runs in phase on both.  (The frames this used
    // to run were renumbered away by the assignment anyway - they cost
    // inputs the server never asked for and bought nothing but the offset.)
    // A bounded wait: if the assignment never comes the client runs on
    // rather than sitting here for ever, and says so.
    hold_state session_client_core::poll_hold() {
        hold_state state;
        if (!s.held_) return state;
        ++s.hold_polls_;
        if (platform_.now(clock_site::hold_poll) < s.hold_deadline_) {
            state.held = true;
            return state;   // poll the network, step nothing
        }
        s.held_ = false;
        if (platform_.hooks.numbering_restarted) platform_.hooks.numbering_restarted(clock_site::hold_timeout);
        logf("anchor hold: no tick base after %.1f s; running anyway (the world will be out of phase with "
             "the server's)", std::chrono::duration<double>(kAnchorHoldTimeout).count());
        journal_.note("anchor hold timed out; running without a tick base");
        state.timed_out = true;
        return state;
    }

    // The hold given up by the host (R1): the retail host waits for the
    // assignment inside one frame and stops waiting when the connection is
    // lost (PSC physics_session_hold_until_assigned).  The world then runs
    // without a tick base, as after a timeout.
    void session_client_core::release_hold(const char* why) {
        if (!s.held_) return;
        s.held_ = false;
        logf("anchor hold released after %d polls: %s", s.hold_polls_, why);
        journal_.note(std::string("anchor hold released: ") + why);
    }

    // SessionEnd: drop the mirrors and go idle; a later SessionStart of the
    // same room reloads the level (host restart) through begin.
    void session_client_core::end(const std::string& reason) {
        if (platform_.hooks.ending) platform_.hooks.ending();
        // Step M1: the member lives' account.  A parked event whose life was
        // never told of is dropped here (the server cannot produce one).
        if (s.session_ != 0) {
            size_t parked_left = 0;
            for (const auto& [key, events]: s.parked_events_) parked_left += events.size();
            size_t journal_pending = 0;
            if (journal_.journalling())
                for (const auto& l: s.lives_) if (!l.journaled_add) ++journal_pending;
            const std::string text = "member lives: " + std::to_string(s.lives_.size()) + " lives, "
                + std::to_string(s.member_messages_) + " member messages (" + std::to_string(s.member_copies_)
                + " copies), events parked " + std::to_string(s.events_parked_) + " (applied "
                + std::to_string(s.parked_applied_) + ", dropped " + std::to_string(parked_left)
                + "), outside their life " + std::to_string(s.member_life_outside_) + ", mirrors outside the lives "
                + std::to_string(s.mirrors_outside_lives_) + ", own echoes of another life "
                + std::to_string(s.own_other_life_echoes_) + ", catch-up stamps of untold lives "
                + std::to_string(s.catchup_unknown_lives_) + ", PLAYER records not written " + std::to_string(journal_pending);
            logf("%s", text.c_str());
            journal_.note(text);
            s.parked_events_.clear();
        }
        journal_.close(reason);
        // After the journal closed: the own body guard is lifted (host
        // state; hooks.ended, a back-call until E11), and every mirror's
        // navigation and body taken out (the headless end_session_world
        // until E6; not muted).
        if (platform_.hooks.ended) platform_.hooks.ended();
        for (auto& [id, remote]: s.remotes_) {
            if (remote.ghost_shown && platform_.show_ghost) platform_.show_ghost(remote.entity, false);
            else if (remote.spawn_ghost_shown && platform_.show_spawn_ghost) platform_.show_spawn_ghost(remote.entity, false, false);
            if (remote.navigation) platform_.detach_mirror_navigation(remote.entity);
            if (remote.physicalized) platform_.unmake_mirror(remote.entity);
        }
        s.remotes_.clear();
        // Step SPGH: the rule off with the session, every ball shown as itself.
        if (s.spawn_ghost_active_) {
            if (s.own_spawn_ghost_shown_ && platform_.show_spawn_ghost)
                platform_.show_spawn_ghost(s.own_spawn_ghost_entity_, true, false);
            s.own_spawn_ghost_shown_ = false;
            double info[7] = {};
            std::string error;
            if (platform_.spawn_ghost && platform_.spawn_ghost(false, false, bmmo::session::kSpawnGhostRadius, info, error) >= 0)
                logf("spawn ghost: %llu ghost -> solid transitions seen after frames, %llu spawn births marked; physics "
                     "layer: %.0f solidified, %.0f pair rechecks, %.0f overlap waits, %.0f births, %.0f resumed solid",
                     static_cast<unsigned long long>(s.spawn_ghost_transitions_),
                     static_cast<unsigned long long>(s.spawn_ghost_births_), info[2], info[3], info[4], info[5], info[6]);
        }
        s.spawn_ghost_active_ = false;
        s.own_spawn_ghost_ = false;
        // Step BALA: the zone off with the session.
        if (s.airship_active_)
            logf("airship: ghosts shown %llu; own corrections not applied %llu; ball rows not applied %llu; resyncs that kept the own ball %llu",
                 static_cast<unsigned long long>(s.ghosts_shown_),
                 static_cast<unsigned long long>(s.own_corrections_out_of_sync_),
                 static_cast<unsigned long long>(s.airship_rows_skipped_),
                 static_cast<unsigned long long>(s.resyncs_own_kept_));
        if (s.airship_active_ && platform_.airship_zone) {
            double box[6] = {};
            std::string error;
            platform_.airship_zone(false, std::string(), box, error);
        }
        s.airship_active_ = false;
        s.own_airship_tick_ = 0;
        s.relay_seen_.clear();
        s.held_seen_.clear();
        s.held_stale_seen_.clear();
        reset_held_epoch("the session's end");
        s.pending_relay_.clear();
        s.event_seen_.clear();
        s.own_event_repeats_.clear();
        s.remote_last_physicalize_.clear();
        s.remote_last_unphysicalize_.clear();
        // The status line (E10: the core's text, the host prints it).
        if (platform_.hooks.status) platform_.hooks.status(status_text(host_values()));
        s.phase_ = client_core_state::phase::idle;
        s.session_ = 0;
        s.assigned_ = false;
        ++s.sessions_ended_;
    }

    // ------------------------------------------------------- messages
    void session_client_core::on_session_assign(const bmmo::session_assign_msg& msg) {
        using own_life = client_core_state::own_life;
        if (msg.session != s.session_ || s.phase_ != client_core_state::phase::running) return;
        // The server sends an assignment several times, unreliably,
        // beside the reliable one (a lost reliable copy used to arrive
        // ~400 ms late and start this client that far behind for good):
        // a copy of the one already applied changes nothing.
        // ...and a copy of an older one (a resync's base is always
        // newer than the start's) must not be taken for a re-assignment.
        if (s.assigned_ && static_cast<int32_t>(msg.first_tick - s.last_assign_first_tick_) <= 0) {
            ++s.duplicate_assigns_;
            return;
        }
        s.last_assign_first_tick_ = msg.first_tick;
        // A newer base answers every resync asked for so far, and any
        // the limiter still owes: its full snapshot replaces the world.
        s.resync_in_flight_ = false;
        s.resync_owed_ = false;
        if (s.assigned_) {
            // Resync (design 9.2): numbering restarts from the server's
            // current tick; the next full snapshot rebuilds every body.
            // Protocol 10 (resync design 2.6.1, step A3): with
            // ASSIGN_FLAG_STATE_FOLLOWS the server's state of base - 1
            // follows instead, and the history is anchored there.
            const uint32_t old_tick = current_tick();
            const bool exact = (msg.flags & bmmo::session::ASSIGN_FLAG_STATE_FOLLOWS) != 0 && config.rollback;
            // Delta > 0 (step A3): the old numbering already sent its frames
            // for base .. old_tick, and the server's buffer keeps a frame it
            // has for a tick at or after the base (input_buffer::reset) and
            // takes the first copy of a tick (submit) - it applies those, not
            // what the new numbering plays for the same ticks.  Taken from
            // the sent ring before the epoch reset clears it, and played
            // again below (forced own frames, as a SessionLead's range).
            std::map<uint32_t, bmmo::session::input_frame> resent;
            if (exact)
                for (auto it = s.sent_inputs_.lower_bound(msg.first_tick);
                     it != s.sent_inputs_.end() && static_cast<int32_t>(it->first - old_tick) <= 0; ++it)
                    resent.emplace(it->first, it->second);
            s.tick_base_ = msg.first_tick;
            s.held_floor_ = s.tick_base_;
            s.resync_window_ = false;   // the old numbering's
            reset_held_epoch("the new base");
            s.frames_since_anchor_ = 0;
            s.input_history_.clear();
            s.corrector_.clear();
            for (auto& [name, corrector]: s.mechanism_correctors_) corrector.clear();
            for (auto& [id, remote]: s.remotes_) remote.corrector.clear();
            // A renumbering makes every record unreachable: no incoming
            // snapshot can match a tick of the old base (design 9.25 -
            // the retail mod does this in rebase_tick).  The registry
            // goes with it: its rows are keyed by the old numbers.
            s.rollback_.invalidate_history();
            s.union_records_.clear();   // step B1: beside the records
            s.sectors_.forget_walk_tick();   // step A5: an old numbering's tick
            // An own beam the new numbering has not reached yet is the
            // server's to apply at its stamp, and so ours: live once
            // more, rather than retired with the history.  The births
            // go: the full snapshot puts the server's own ball in.
            rebase_own_lives();
            s.own_inputs_.clear();
            s.mechanism_tracking_.clear();
            s.resync_pending_ = true;
            s.have_snapshot_ = false;
            s.resync_exact_ = exact;
            // A late join superseded by an exact re-assign is still a join;
            // one answered by the full snapshot is not (step A5).
            if (!exact) s.resync_join_ = false;
            if (s.resync_held_) {
                // A newer base answers the held resync: the hold ends here.
                s.resync_held_ = false;
                logf("resync hold ended by a newer assignment");
            }
            if (exact) {
                anchor_resync(msg.first_tick, old_tick);
                for (const auto& [tick, frame]: resent) s.forced_own_[tick] = client_core_state::forced_own{frame, false};
                s.resync_resent_ += resent.size();
                if (!resent.empty())
                    logf("resync: the old numbering sent own frames for %u..%u (%zu): played again at those ticks, "
                         "as the server applies them", resent.begin()->first, resent.rbegin()->first, resent.size());
                if (platform_.hooks.numbering_restarted) platform_.hooks.numbering_restarted(clock_site::assign_resync);
                return;
            }
            s.have_resync_state_ = false;
            if (platform_.hooks.numbering_restarted) platform_.hooks.numbering_restarted(clock_site::assign_resync);
            logf("resynced: tick base %u", s.tick_base_);
            journal_.note("resync: tick base " + std::to_string(s.tick_base_));
            journal_member_lives();   // step M1: a life told of meanwhile, at the new base at the earliest
            return;
        }
        s.assigned_ = true;
        // Step A5 (resync design 2.9): a late joiner's first assignment with
        // ASSIGN_FLAG_STATE_FOLLOWS is taken as a resync's - the server's
        // state of base - 1 follows, and the world as it stands (the
        // anchored level start) is recorded as that tick.  Read before the
        // renumbering, like a re-assign's old tick: Delta is then the whole
        // distance to the level start.
        const bool exact_join = (msg.flags & bmmo::session::ASSIGN_FLAG_STATE_FOLLOWS) != 0 && config.rollback
                             && msg.first_tick != 0;
        const uint32_t join_old_tick = current_tick();
        // A numbered base is the ordinary case: the server anchors the
        // members present at the start at its start lead and a late
        // joiner (or a resync) at its current tick plus the same lead,
        // so the numbering is not the one this client has been running
        // since its own anchor.  Renumber onto it, exactly as the retail
        // client does (physics_session_client.cpp).  Base 0 is the one
        // case left over: a server that still numbers a start member
        // from its own anchor leaves the numbering alone.
        if (msg.first_tick != 0) {
            s.frames_since_anchor_ = 0;
            s.input_history_.clear();
            // Same as the resync above: the frames recorded under the
            // anchor-relative numbering name ticks the server never
            // asked us for, and nothing can match them again.
            s.rollback_.invalidate_history();
            s.union_records_.clear();   // step B1: beside the records
            s.sectors_.forget_walk_tick();   // step A5: an old numbering's tick
            s.own_inputs_.clear();
            // ...and our own body changes, numbered the same way
            // (an unanswered birth is set aside for its echo).
            set_aside_unanswered(own_life::aside::resync);
            s.own_life_.clear();
            clear_own_window_state();   // step EL8
            s.mechanism_tracking_.clear();
            if (platform_.hooks.numbering_restarted) platform_.hooks.numbering_restarted(clock_site::assign_renumber);
        }
        s.tick_base_ = msg.first_tick;
        s.held_floor_ = s.tick_base_;
        reset_held_epoch("the new base");
        // The anchor hold ends here: the next frame is the world's
        // first step and it is numbered `tick_base_`, exactly like the
        // server's own first step (design 9.25 phase alignment).
        if (s.held_) {
            s.held_ = false;
            if (platform_.hooks.numbering_restarted) platform_.hooks.numbering_restarted(clock_site::assign_release);
            logf("anchor hold released after %d polls: the first frame is tick %u", s.hold_polls_, s.tick_base_);
            journal_.note("anchor hold released at tick base " + std::to_string(s.tick_base_));
        }
        logf("tick base %u assigned (%lld frames since anchor)", s.tick_base_, static_cast<long long>(s.frames_since_anchor_));
        journal_.note("assigned: tick base " + std::to_string(s.tick_base_) + " ("
                      + std::to_string(s.frames_since_anchor_) + " frames since the anchor)");
        journal_member_lives();   // step M1: the lives told of while unassigned
        if (exact_join) {
            // The resync's anchor (2.6.1 steps 2-5): the frames from the base
            // run normally, the owed passes live (stamped late, 2.7 case 4),
            // and the apply waits for their bodies (2.6.4 step 2), then takes
            // the server's rows, scripts and union of base - 1 and re-steps.
            s.resync_pending_ = true;
            s.resync_exact_ = true;
            s.resync_join_ = true;
            s.have_snapshot_ = false;
            logf("late join: the server's state of tick %u follows (protocol 10)", s.tick_base_ - 1);
            anchor_resync(s.tick_base_, join_old_tick);
        }
        flush_inputs();
    }

    void session_client_core::on_session_end(const bmmo::session_end_msg& msg) {
        logf("session %u ended: %s", msg.session, msg.reason.c_str());
        if (msg.session == s.session_) end(msg.reason.empty() ? "session ended" : msg.reason);
    }

    // ------------------------------------------------- the core's rewinds
    // A frozen local clock (the retail scripts' pause, the Level 1
    // tutorial): the engine sets the bodies and drops the history with no
    // replay and returns true, and the world stays on the anchor while the
    // tick counter runs on; only a re-anchor brings the two back together
    // (design 4.1, 7 Q1).  on_snapshot asks for it after its own rewind;
    // with the session's snapshots off no snapshot rewinds any more (step
    // RC3), so every other rewind asks here.  Owed: a request the limiter
    // holds back is asked again (retry_owed_resync), since the history is
    // gone and nothing else would ask.  Never true on the headless client
    // (its clock never stops), so no capture's reads change.
    bool session_client_core::rewind_world(uint32_t anchor, uint32_t now,
                                           const std::function<bool(const std::string& entity, uint32_t tick,
                                                                    bmmo::session::input_frame& out)>& input_at,
                                           const std::string& reason, uint32_t reach) {
        const uint64_t frozen_before = s.rollback_.stats().frozen;
        const bool rolled = s.rollback_.rewind_to(world(), anchor, now, input_at, reason, reach);
        if (s.rollback_.stats().frozen != frozen_before) request_resync("local physics clock frozen", true);
        // Step EL7: what the reconcile queued and no replay applied.
        settle_unreplayed_lives(now);
        settle_own_due();   // step EL8: the own ball's
        return rolled;
    }

    // ------------------------------------------------- resync request
    // `owed`: the own correction needs this resync (late-player design
    // 1.3's late gate) - one the 2 s limiter holds back is asked again
    // from frame() (retry_owed_resync) rather than lost, since the tick
    // that asked is deduplicated and nothing would ask a second time.
    // While a request is unanswered no other goes out: its assignment
    // renumbers and its full snapshot replaces the world, which covers
    // every reason found meanwhile (a blackout can hold the answer past
    // the limiter's 2 s; 10 s is the give-up for an answer never coming).
    void session_client_core::request_resync(const char* reason, bool owed) {
        if (s.phase_ != client_core_state::phase::running || !s.assigned_) return;
        const auto now = platform_.now(clock_site::resync_limiter);
        if (s.resync_in_flight_ && now - s.last_resync_request_ < std::chrono::seconds(10)) return;
        if (s.resyncs_sent_ > 0 && now - s.last_resync_request_ < std::chrono::seconds(2)) {
            if (owed && !s.resync_owed_) {
                s.resync_owed_ = true;
                s.resync_owed_reason_ = reason;
                logf("resync owed (%s) at tick %u: the limiter holds it back", reason, current_tick());
            }
            return;
        }
        s.resync_owed_ = false;
        s.resync_in_flight_ = true;
        s.last_resync_request_ = now;
        s.consecutive_hard_ = 0;
        s.consecutive_unmatched_ = 0;   // step B2SW: a resync answers the run too
        // A SessionLead's slew in flight ends here, acknowledged in full
        // (late-player design 2.8, lead_slew::finish_reanchored): the
        // server re-anchors this client at L_eff, the first lead plus the
        // acknowledged offset, and the ack sent first on the same
        // reliable channel is counted in it - so the new clock has the
        // whole delta the server decided, not the part the slew had
        // moved.  Only here, past the limiter: a request held back
        // re-anchors nothing, and the slew carries on.
        if (s.lead_slew_.active())
            finish_lead_slew(std::string("ended by a resync request (") + reason + ")", true);
        bmmo::session_resync_msg msg;
        msg.session = s.session_;
        msg.last_full_tick = s.last_snapshot_tick_;
        msg.serialize();
        platform_.send(msg.raw.str(), send_mode::reliable);
        ++s.resyncs_sent_;
        logf("resync requested (%s) at tick %u", reason, current_tick());
        // Why this client gave up on its own world belongs in the box: the
        // reason ("tick schedule rebased") leaves no other trace in the
        // file, and the assignment that answers it looks unprompted.
        journal_.note(std::string("resync: requested (") + reason + ")");
    }

    // A resync request_resync held back for the own correction: asked
    // now if the limiter allows (it keeps the debt otherwise).  A resync
    // waiting for its snapshot already is the answer - but not a pending
    // exact resync whose state was refused or never came (step A3: the
    // apply's refusals and the hold's timeout clear resync_in_flight_ and
    // ask again; every other trigger waits while a resync is pending, so the
    // debt is the only way that request goes out).
    void session_client_core::retry_owed_resync() {
        if (!s.resync_owed_) return;
        if (s.resync_pending_ && !(s.resync_exact_ && !s.resync_in_flight_ && !s.have_resync_state_)) {
            s.resync_owed_ = false;
            return;
        }
        const std::string reason = s.resync_owed_reason_ + " (owed)";
        request_resync(reason.c_str(), true);
    }

    // ---------------------------------------------------------- sending
    void session_client_core::flush_inputs() {
        if (!s.assigned_) return;
        s.own_event_repeats_.send_due([this](uint32_t, const std::string& bytes) {
            platform_.send(bytes, send_mode::unreliable_no_delay);
        });
        if (s.input_history_.empty()) return;
        size_t index = 0;
        while (index < s.input_history_.size()) {
            bmmo::session_input_msg msg;
            msg.session = s.session_;
            msg.first_tick = s.input_history_[index].first;
            while (index < s.input_history_.size() && msg.frames.size() < bmmo::session::MAX_INPUT_FRAMES) {
                msg.frames.push_back(s.input_history_[index].second);
                ++index;
            }
            remember_sent(msg);
            msg.serialize();
            platform_.send(msg.raw.str(), send_mode::unreliable_no_delay);
            ++s.inputs_sent_;
        }
    }

    // The sent ring (late-player design 1.3): the bytes each uplink
    // datagram carried, by tick, which a held record's `source` names.
    // Never amended - a source tick can itself have been substituted,
    // and the server applied the frame as we SENT it, not what our own
    // input history says after a correction.  Cleared with the numbering
    // (reset_held_epoch); kSentInputRing ticks reach every reference a
    // record inside the deepest reach can make (input_relay.hpp).
    void session_client_core::remember_sent(const bmmo::session_input_msg& msg) {
        for (size_t i = 0; i < msg.frames.size(); ++i)
            s.sent_inputs_.emplace(msg.first_tick + static_cast<uint32_t>(i), msg.frames[i]);
        while (s.sent_inputs_.size() > bmmo::session::kSentInputRing) s.sent_inputs_.erase(s.sent_inputs_.begin());
    }

    void session_client_core::send_event(bmmo::session_event_msg& event) {
        event.session = s.session_;
        // Protocol 9: a client's event goes out with its origin equal to
        // its stamp; only the server re-stamps.
        event.origin_tick = event.tick;
        event.state = 0;
        event.serialize();
        ++s.events_sent_;
        // Own events go on the wire with player 0; the box names the
        // player the server will fill in.
        journal_.lifecycle_event(s.own_id_, event);
        if (platform_.hooks.filter_event && platform_.hooks.filter_event(event) != event_send_verdict::send) return;
        put_event_on_wire(event.raw.str());
    }

    void session_client_core::put_event_on_wire(const std::string& bytes) {
        // ReliableNoNagle, not Reliable: a reliable message waits up to the
        // Nagle delay to be batched, and this frame's input goes out
        // unreliable with no delay a moment later - so an event stamped
        // for this tick could arrive after the input that lets the server
        // step it, and be applied a tick late (the server logs it: "late
        // by 1", seen on BodyRevived and Physicalize).
        platform_.send(bytes, send_mode::reliable_no_nagle);
        // ...and again, unreliably, on each of the next ticks
        // (flush_inputs): the server takes the first copy to arrive.
        s.own_event_repeats_.push(bytes, {0u});
    }

}
