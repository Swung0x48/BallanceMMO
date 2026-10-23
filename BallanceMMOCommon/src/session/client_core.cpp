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
        world_hooks_.before_replayed_tick = [this](uint32_t tick) { drain_pending_births(tick); };
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
        world_hooks_.replay_mechanisms = [this]() { return platform_.movable_mechanisms(); };
        // Every decision the engine takes about a snapshot, straight into
        // the box: no log parsing needed to know what this client did.
        world_hooks_.on_correction = [this](const bmmo::session::rollback_correction& c) { journal_.rollback_correction(c); };
        // A moved own birth (late-player design 3.3) is destroyed at the
        // restore: its birth record is its applied tick's, where the replay
        // makes it again the server's way (apply_own_lives).
        world_hooks_.destroy_own_birth = [this](const std::string& entity, uint32_t birth_tick, bool& destroyed) {
            return destroy_moved_own_birth(entity, birth_tick, destroyed);
        };
        world_hooks_.own_body = [this](const std::string& entity) { return entity == s.own_body_entity_; };
        world_hooks_.mirror = [this](const std::string& entity) {
            for (const auto& [id, remote]: s.remotes_) if (remote.entity == entity) return true;
            return false;
        };
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
        if (msg.protocol != bmmo::session::PHYSICS_PROTOCOL_VERSION) {
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
        s.session_ = msg.session;
        s.snapshot_interval_ = msg.snapshot_interval;
        s.input_delay_ = msg.input_delay;
        s.sector_lead_ = msg.sector_lead;
        s.seed_ = msg.seed;
        s.spawn_impulse_ = msg.spawn_impulse;
        s.spawn_spacing_ = msg.spawn_spacing;
        s.level_ = msg.map.level > 0 ? msg.map.level : config.level;
        s.players_ = msg.players;
        s.own_join_order_ = -1;
        for (const auto& p: s.players_) {
            if (p.id != s.own_id_) continue;
            s.own_join_order_ = p.join_order;
        }
        s.own_group_ = "P#" + std::to_string(s.own_join_order_ < 0 ? 63 : s.own_join_order_);
        logf("session %u starting: level %d, %zu players, join order %d, seed %d, input delay %u, sector lead %u",
             s.session_, s.level_, s.players_.size(), s.own_join_order_, s.seed_, static_cast<unsigned>(s.input_delay_),
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
        s.start_sector_pending_ = false;
        s.start_sectors_.clear();
        s.sectors_.reset();
        s.sectors_requested_.clear();
        s.sector_cell_ = 0;
        for (auto& pass: s.recent_passes_) pass = {};
        s.phase_misses_ = 0;
        // R1: the retail policies' and readers' state, and no frame
        // prepared (post_step refuses one no pre_step of this session made).
        s.ball_rows_.clear();
        s.own_last_birth_ = {};
        s.snapshots_without_own_ = 0;
        s.own_backstops_ = 0;
        s.last_backstop_ = {};
        s.snapshot_acked_known_ = false;
        s.snapshot_acked_tick_ = 0;
        s.snapshot_acked_frame_ = s.snapshot_frame_ = -1;
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
        ready.caps = caps_.slew ? bmmo::session::CAP_LEAD : 0u;
        ready.serialize();
        platform_.send(ready.raw.str(), send_mode::reliable);
        logf("anchored after %d load ticks: pose %016llx surfaces %016llx cores=%d factor=%.6f", report.load_ticks,
             static_cast<unsigned long long>(report.hash.pose), static_cast<unsigned long long>(report.hash.surfaces),
             report.hash.cores, report.hash.time_factor);
        // The box starts where the session does: at the anchor the server
        // has to agree with, before the tick base is known.
        journal_.open(report.hash, report.build_id, report.own_name);
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
        journal_.close(reason);
        // After the journal closed: the own body guard is lifted (host
        // state; hooks.ended, a back-call until E11), and every mirror's
        // navigation and body taken out (the headless end_session_world
        // until E6; not muted).
        if (platform_.hooks.ended) platform_.hooks.ended();
        for (auto& [id, remote]: s.remotes_) {
            if (remote.navigation) platform_.detach_mirror_navigation(remote.entity);
            if (remote.physicalized) platform_.unmake_mirror(remote.entity);
        }
        s.remotes_.clear();
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
        // Both branches below renumber: what the server acked under the old
        // numbering says nothing about the new one (R1: the retail
        // starvation detector's baseline, acked_input).
        s.snapshot_acked_known_ = false;
        s.snapshot_acked_tick_ = 0;
        s.snapshot_acked_frame_ = s.snapshot_frame_ = -1;
        // A newer base answers every resync asked for so far, and any
        // the limiter still owes: its full snapshot replaces the world.
        s.resync_in_flight_ = false;
        s.resync_owed_ = false;
        if (s.assigned_) {
            // Resync (design 9.2): numbering restarts from the server's
            // current tick; the next full snapshot rebuilds every body.
            s.tick_base_ = msg.first_tick;
            s.held_floor_ = s.tick_base_;
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
            // An own beam the new numbering has not reached yet is the
            // server's to apply at its stamp, and so ours: live once
            // more, rather than retired with the history.  The births
            // go: the full snapshot puts the server's own ball in.
            rebase_own_lives();
            s.own_inputs_.clear();
            s.mechanism_tracking_.clear();
            s.resync_pending_ = true;
            s.have_snapshot_ = false;
            if (platform_.hooks.numbering_restarted) platform_.hooks.numbering_restarted(clock_site::assign_resync);
            logf("resynced: tick base %u", s.tick_base_);
            journal_.note("resync: tick base " + std::to_string(s.tick_base_));
            return;
        }
        s.assigned_ = true;
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
            s.own_inputs_.clear();
            // ...and our own body changes, numbered the same way
            // (an unanswered birth is set aside for its echo).
            set_aside_unanswered(own_life::aside::resync);
            s.own_life_.clear();
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
        flush_inputs();
    }

    void session_client_core::on_session_end(const bmmo::session_end_msg& msg) {
        logf("session %u ended: %s", msg.session, msg.reason.c_str());
        if (msg.session == s.session_) end(msg.reason.empty() ? "session ended" : msg.reason);
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
    // waiting for its snapshot already is the answer.
    void session_client_core::retry_owed_resync() {
        if (!s.resync_owed_) return;
        if (s.resync_pending_) { s.resync_owed_ = false; return; }
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
