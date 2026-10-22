#include "session_runner.hpp"

#include "CKAll.h"

#include <physics/physics_state.hpp>
#include <session/lifecycle.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <system_error>

namespace bmmo::sim {
    using clock = std::chrono::steady_clock;

    namespace {
        // session_<id>_level<N>_<UTC yyyymmddhhmmss>.bmjr.  The same instant
        // goes into the header as utc_ms: file name and wall clock must agree,
        // or a journal cannot be lined up with a player's "it broke at 21:37".
        std::string journal_file_name(uint32_t session, int level, uint64_t& utc_ms) {
            const auto now = std::chrono::system_clock::now();
            utc_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count());
            const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
            std::tm parts{};
#ifdef _WIN32
            gmtime_s(&parts, &seconds);
#else
            gmtime_r(&seconds, &parts);
#endif
            char stamp[16] = {};
            std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &parts);
            return "session_" + std::to_string(session) + "_level" + std::to_string(level)
                 + "_" + stamp + ".bmjr";
        }

        // The server's lifecycle_event as the journal stores it: a plain field
        // copy plus the player it belongs to (journal.hpp cannot include the
        // server's headers, so the conversion lives here).  `applied` is the
        // tick the world consumed it at, which is the one a replay has to feed
        // it back at; `e.tick` is the tick the world was handed in the event
        // (the applied tick too since protocol 9: the spawn impulse index
        // comes from it), and `origin` the tick the client stamped it for,
        // older when the event arrived late (journal v3).
        bmmo::session::journal_event to_journal_event(uint32_t player, const lifecycle_event& e, uint32_t applied,
                                                      uint32_t origin) {
            bmmo::session::journal_event out;
            out.tick = applied;
            out.event_tick = e.tick;
            out.origin_tick = origin;
            out.id = player;
            out.type = e.type;
            out.ball_type = e.ball_type;
            out.flags = e.flags;
            for (int k = 0; k < 3; ++k) out.position[k] = e.position[k];
            for (int k = 0; k < 9; ++k) out.rotation[k] = e.rotation[k];
            out.sector = e.sector;
            out.name = e.name;
            out.recipe = e.recipe;
            return out;
        }

        std::string fixed2(double value) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.2f", value);
            return text;
        }
    }

    session_runner::session_runner(runner_config config, session_callbacks callbacks)
        : config_(std::move(config)), callbacks_(std::move(callbacks)) {
        thread_ = std::thread([this] { run(); });
    }

    session_runner::~session_runner() {
        {
            std::lock_guard lk(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void session_runner::log(const std::string& text) {
        if (callbacks_.log) callbacks_.log(text);
    }

    void session_runner::post(std::function<void()> command) {
        {
            std::lock_guard lk(mutex_);
            commands_.push_back(std::move(command));
        }
        wake_.notify_all();
    }

    void session_runner::publish_state(const session_state& s) {
        std::lock_guard lk(view_mutex_);
        auto& v = views_[s.id];
        v.next_tick = s.world ? s.world->tick_index() : 0;
        v.running = s.running;
    }

    uint32_t session_runner::current_tick(uint32_t session) const {
        std::lock_guard lk(view_mutex_);
        auto it = views_.find(session);
        return it == views_.end() ? 0 : it->second.next_tick;
    }

    bool session_runner::running(uint32_t session) const {
        std::lock_guard lk(view_mutex_);
        auto it = views_.find(session);
        return it != views_.end() && it->second.running;
    }

    uint32_t session_runner::late_join_tick(uint32_t session) const {
        std::lock_guard lk(view_mutex_);
        auto it = views_.find(session);
        if (it == views_.end() || !it->second.running) return 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - it->second.start).count();
        // the tick the server reaches now, plus a second so the joiner's
        // first inputs arrive in time
        return it->second.first_tick + static_cast<uint32_t>(elapsed * 66 / 1000000) + 66;
    }

    size_t session_runner::session_count() const {
        std::lock_guard lk(view_mutex_);
        return views_.size();
    }

    std::string session_runner::journal_path(uint32_t session) const {
        std::lock_guard lk(view_mutex_);
        auto it = views_.find(session);
        return it == views_.end() ? std::string{} : it->second.journal;
    }

    // Opens this session's black box and writes its header.  Any failure is
    // logged once and the session runs on without a journal: the recorder must
    // never be able to break a simulation.
    void session_runner::open_journal(session_state& s, float spawn_impulse, uint64_t anchor_hash,
                                      uint64_t anchor_surfaces, uint32_t first_tick) {
        if (config_.journal_dir.empty()) return;
        const std::string prefix = "[session " + std::to_string(s.id) + "] journal: ";
        std::error_code ec;
        std::filesystem::create_directories(config_.journal_dir, ec);
        if (ec) {
            log(prefix + config_.journal_dir.string() + ": " + ec.message());
            return;
        }
        bmmo::session::journal_header header;
        header.kind = bmmo::session::journal_kind::server;
        header.session = s.id;
        header.level = s.level;
        header.seed = config_.seed;
        header.spawn_impulse = spawn_impulse;
        header.input_delay = s.input_delay;
        header.checkpoint_ticks = config_.journal_checkpoint_ticks;
        header.first_tick = first_tick;
        header.anchor_hash = anchor_hash;
        header.anchor_surfaces = anchor_surfaces;
        header.build_id = bmmo::physics::build_id();
        const auto path = config_.journal_dir / journal_file_name(s.id, s.level, header.utc_ms);
        s.journal_start = clock::now();
        std::string error;
        if (!s.journal.open(path, header, config_.journal_max_bytes, error)) {
            log(prefix + error);
            return;
        }
        {
            std::lock_guard lk(view_mutex_);
            views_[s.id].journal = path.string();
        }
        log(prefix + path.string());
    }

    void session_runner::create_session(uint32_t session, int level,
                                        const std::vector<std::pair<uint32_t, uint8_t>>& players,
                                        uint32_t input_delay, uint32_t start_base, float spawn_impulse,
                                        const std::string& note, std::vector<std::string> names) {
        post([this, session, level, players, input_delay, start_base, spawn_impulse, note,
              names = std::move(names)] {
            auto& s = sessions_[session];
            s = std::make_unique<session_state>();
            s->id = session;
            s->level = level;
            s->input_delay = input_delay ? input_delay : config_.input_delay;
            {
                std::lock_guard lk(view_mutex_);
                views_[session] = view{};
            }
            world_options options;
            options.game_root = config_.game_root;
            options.level = level;
            options.seed = config_.seed;
            options.trace = config_.trace;
            options.spawn_impulse = spawn_impulse;
            options.log = [this, session](const std::string& text) {
                log("[session " + std::to_string(session) + "] " + text);
            };
            std::string error;
            log("[session " + std::to_string(session) + "] booting level " + std::to_string(level) + " from "
                + config_.game_root.string());
            s->world = physics_world::create(options, error);
            world_ready_info info;
            info.session = session;
            if (!s->world) {
                s->failed = true;
                info.error = error;
                log("[session " + std::to_string(session) + "] world boot failed: " + error);
                // A session that never reached the anchor is exactly when the
                // box is wanted: the header (anchor fields 0), who was in the
                // room and why it died, then close.
                open_journal(*s, spawn_impulse, 0, 0, 0);
                if (!note.empty()) s->journal.note(0, "start: " + note);
                s->journal.note(0, "boot failed: " + error);
                s->journal.close();
                if (callbacks_.on_world_ready) callbacks_.on_world_ready(info);
                return;
            }
            // Design 9.25 phase alignment: the world's first simulated tick is
            // the members' start base, not 0.  Set before anything reads
            // tick_index() - the journal header's first_tick, the PLAYER
            // records, the input buffers and the cross-thread view all take
            // their number from here, so a journal still says where its first
            // TICK record is and replays without a hole.
            if (start_base != 0) s->world->set_tick_index(start_base);
            s->start_base = start_base;
            const uint32_t first_tick = s->world->tick_index();
            open_journal(*s, spawn_impulse, s->world->anchor_hash(), s->world->anchor_surfaces(), first_tick);
            if (!note.empty()) s->journal.note(first_tick, "start: " + note);
            // An explicit signal for a reader that only has the file: the world
            // is at its anchor and its first step is numbered `first_tick`.
            s->journal.note(first_tick, "start base " + std::to_string(first_tick)
                    + ": the world's first simulated tick (design 9.25 phase alignment)");
            for (size_t i = 0; i < players.size(); ++i) {
                const auto [player, join_order] = players[i];
                std::string add_error;
                if (!s->world->add_player(player, join_order, add_error)) {
                    log("[session " + std::to_string(session) + "] add_player failed: " + add_error);
                    continue;
                }
                s->players.insert(player);
                s->join_orders[player] = join_order;
                s->inputs[player].reset(first_tick);
                s->journal.player(first_tick, player, join_order, true,
                                  i < names.size() ? names[i] : std::string{});
            }
            s->cadence.interval = config_.snapshot_interval;
            s->cadence.full_interval = config_.full_snapshot_interval;
            publish_state(*s);
            info.ok = true;
            info.anchor_hash = s->world->anchor_hash();
            info.anchor_surfaces = s->world->anchor_surfaces();
            info.ball_rows = s->world->ball_rows();
            {
                const VxMatrix& spawn = s->world->spawn_matrix();
                VxQuaternion rotation;
                rotation.FromMatrix(spawn);
                for (int k = 0; k < 3; ++k) info.spawn_position[k] = spawn[3][k];
                info.spawn_rotation[0] = rotation.x;
                info.spawn_rotation[1] = rotation.y;
                info.spawn_rotation[2] = rotation.z;
                info.spawn_rotation[3] = rotation.w;
            }
            info.resetpoints = s->world->resetpoints();
            if (callbacks_.on_world_ready) callbacks_.on_world_ready(info);
        });
    }

    void session_runner::destroy_session(uint32_t session, const std::string& reason) {
        post([this, session, reason] {
            if (auto it = sessions_.find(session); it != sessions_.end()) {
                auto& s = *it->second;
                if (config_.patience && s.running && s.world)
                    report_patience(s, s.world->tick_index(), "session end");
                s.journal.note(s.world ? s.world->tick_index() : 0,
                               "end: " + (reason.empty() ? std::string("session destroyed") : reason));
                s.journal.close();
            }
            sessions_.erase(session);
            std::lock_guard lk(view_mutex_);
            views_.erase(session);
        });
    }

    void session_runner::add_player(uint32_t session, uint32_t player, uint8_t join_order,
                                    const std::string& name) {
        post([this, session, player, join_order, name] {
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) return;
            auto& s = *it->second;
            // Back before its old self was taken out: the old one goes now.
            if (s.leaving.count(player)) finish_leave(s, player, s.world->tick_index());
            std::string error;
            if (!s.world->add_player(player, join_order, error)) {
                log("[session " + std::to_string(session) + "] add_player failed: " + error);
                return;
            }
            s.players.insert(player);
            s.join_orders[player] = join_order;
            s.inputs[player].reset(s.world->tick_index());
            s.cadence.force_full();
            s.journal.player(s.world->tick_index(), player, join_order, true, name);
        });
    }

    void session_runner::finish_leave(session_state& s, uint32_t player, uint32_t tick) {
        // Join order is the world's business at add time only; a removal
        // needs the player id alone.
        s.journal.player(tick, player, 0, false, {});
        if (s.world) s.world->remove_player(player);
        s.join_orders.erase(player);
        s.last_physicalize_tick.erase(player);
        s.last_frames.erase(player);
        s.leaving.erase(player);
        s.events.erase(std::remove_if(s.events.begin(), s.events.end(),
                [player](const pending_event& e) { return e.player == player; }), s.events.end());
        s.cadence.force_full();
    }

    void session_runner::remove_player(uint32_t session, uint32_t player) {
        post([this, session, player] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& s = *it->second;
            // Its last patience counts, before the entry goes.
            if (config_.patience && s.running && s.world && s.inputs.count(player))
                report_patience(s, s.world->tick_index(), "player left", &player);
            // Late-player design 3.2: the others still mirror the leaver's
            // ball, so an Unphysicalize goes to them on its behalf - from
            // here, at the runner's own next tick, where the tick is exact,
            // instead of stamped from the network thread's view of it.
            //
            // Its events accepted and relayed but not applied yet (a birth or
            // a Sector stamped ahead) are applied by every receiver at their
            // applied ticks, and the Unphysicalize after them, so the server
            // keeps the player in its world until the tick after the last of
            // them and applies them too (review of step 16): dropped here, the
            // others would make a ball the server never has, or start a
            // sector the server never runs, and the sector union would break.
            // The Unphysicalize then goes through the queue like any event and
            // is an EVENT record; out at once (nothing queued, the usual case)
            // the world drops the ball with the player, and the journal's
            // PLAYER record is what a replay removes it by, so it is a note.
            if (s.world) {
                bmmo::session::lifecycle_acceptance::leaver_event gone;
                const uint32_t next = s.world->tick_index();
                const bool has_ball = s.acceptance.leaver_unphysicalize(player, next, gone);
                const bool queued = std::any_of(s.events.begin(), s.events.end(),
                        [player](const pending_event& e) { return e.player == player; });
                if (has_ball && queued) {
                    lifecycle_event death;
                    death.type = bmmo::session::event_type::Unphysicalize;
                    death.tick = gone.applied;
                    pending_event pe{player, gone.origin, gone.applied, std::move(death), clock::now()};
                    auto pos = std::upper_bound(s.events.begin(), s.events.end(), gone.applied,
                        [](uint32_t t, const pending_event& e) { return static_cast<int32_t>(t - e.applied) < 0; });
                    s.events.insert(pos, std::move(pe));
                }
                if (has_ball) {
                    log("[session " + std::to_string(s.id) + "] player " + std::to_string(player)
                        + " left with a ball: Unphysicalize relayed for tick " + std::to_string(gone.applied)
                        + " (next tick " + std::to_string(next) + ")");
                    s.journal.note(next, "leaver: p" + std::to_string(player) + " Unphysicalize relayed for tick "
                                   + std::to_string(gone.applied));
                    if (callbacks_.on_event_accepted) {
                        accepted_event a;
                        a.session = s.id;
                        a.player = player;
                        a.origin = gone.origin;
                        a.applied = gone.applied;
                        a.type = bmmo::session::event_type::Unphysicalize;
                        a.intake = clock::now();
                        callbacks_.on_event_accepted(a);
                    }
                }
                if (queued) {
                    auto& leaving = s.leaving[player];
                    leaving.remove_at = bmmo::session::lifecycle_leaver_removal_tick(s.events, player, next);
                    if (const auto f = s.last_frames.find(player); f != s.last_frames.end()) leaving.frame = f->second;
                    log("[session " + std::to_string(s.id) + "] player " + std::to_string(player)
                        + " left with events queued: it stays in the world until tick "
                        + std::to_string(leaving.remove_at) + " (next tick " + std::to_string(next) + ")");
                    s.journal.note(next, "leaver: p" + std::to_string(player) + " stays in the world until tick "
                                   + std::to_string(leaving.remove_at) + " for its queued events");
                }
            }
            s.acceptance.forget(player);
            if (!s.leaving.count(player)) finish_leave(s, player, s.world ? s.world->tick_index() : 0);
            s.players.erase(player);
            s.ready.erase(player);
            s.inputs.erase(player);
            s.lateness.erase(player);
            s.patience.erase(player);
            s.leads.erase(player);
            s.acked.erase(player);
            s.lifecycle_missing.erase(player);
            // A session waiting for readiness may now be complete.
            if (!s.running && !s.failed && s.world && !s.players.empty()
                    && std::includes(s.ready.begin(), s.ready.end(), s.players.begin(), s.players.end())) {
                // Design 9.26: the first step (tick start_base) is due start_base
                // periods from now - where a world counting from 0 reached it - so
                // the members keep the wall-clock lead the base gives them.
                s.scheduler.start(clock::now() + std::chrono::duration_cast<clock::duration>(
                        std::chrono::duration<double>(static_cast<double>(s.start_base) / 66.0)),
                        s.world->tick_index(), s.input_delay);
                s.running = true;
                {
                    std::lock_guard lk(view_mutex_);
                    auto& v = views_[session];
                    v.start = clock::now();
                    v.first_tick = s.world->tick_index();
                }
                publish_state(s);
                log_patience_on(s);
            }
        });
    }

    void session_runner::player_ready(uint32_t session, uint32_t player, uint32_t first_tick) {
        post([this, session, player, first_tick] {
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) return;
            auto& s = *it->second;
            if (!s.players.count(player)) return;
            s.ready.insert(player);
            // A resync or a late join re-anchors a buffer that may already hold
            // frames: reset() keeps everything at or after first_tick and
            // refuses to move the cursor backwards, so the batch the client
            // sent right after it was told the base survives the re-anchor.
            // A lead reset's range still ahead (lead_adjust) is cut back to
            // the new base: the client on it knows nothing of the range.
            auto& buffer = s.inputs[player];
            const uint32_t hold_until_before = buffer.hold_until();
            buffer.reset(first_tick);
            const uint32_t hold_until_after = buffer.hold_until();
            // Every assignment measures the reference anew (design 2.8); the
            // meter keeps only the player's total offset.
            // Said in the log, so a run whose would-raise count reads 0
            // because a resync threw the evidence away shows why.
            auto& lateness = s.lateness[player];
            {
                const auto& m = lateness.meter;
                std::string text = "[session " + std::to_string(s.id) + "] S1 reset player=" + std::to_string(player)
                    + " tick=" + std::to_string(s.world->tick_index()) + " base=" + std::to_string(first_tick)
                    + " offset=" + std::to_string(m.offset()) + " samples=" + std::to_string(m.samples_total())
                    + " misses=" + std::to_string(m.misses_total())
                    + " streak=" + std::to_string(m.raise_streak()) + "/" + std::to_string(m.lower_streak());
                if (m.state() == bmmo::session::lateness_state::pending)
                    text += std::string(" dropped=")
                          + (m.pending_action() == bmmo::session::lateness_action::raise ? "would-raise" : "would-lower")
                          + " k=" + std::to_string(m.pending_k());
                if (const auto l = s.leads.find(player); l != s.leads.end() && l->second.outstanding)
                    text += " lead-dropped=" + std::to_string(l->second.id);
                if (hold_until_after != hold_until_before)
                    text += " hold-released=[" + std::to_string(hold_until_after) + "," + std::to_string(hold_until_before) + ")";
                log(text);
            }
            lateness.meter.reset(first_tick);
            lateness.reported = false;
            // The decision sent under the old base goes with it (design 2.8:
            // the client drops it by its epoch), and so does the newest tick
            // of the old numbering.  The caps and the assigned lead stay.
            if (auto l = s.leads.find(player); l != s.leads.end()) l->second.reassigned();
            // Measured anew, waived anew (design 2.8): an episode of the old
            // assignment ends, and its cut ticks lie below the new base where
            // nothing will come for them.  The counters stay.
            if (auto p = s.patience.find(player); p != s.patience.end()) {
                p->second.waiver.reassigned();
                p->second.cut.clear();
            }
            if (s.running) return;
            if (std::includes(s.ready.begin(), s.ready.end(), s.players.begin(), s.players.end())) {
                // Design 9.26: the first step (tick start_base) is due start_base
                // periods from now - where a world counting from 0 reached it - so
                // the members keep the wall-clock lead the base gives them.
                s.scheduler.start(clock::now() + std::chrono::duration_cast<clock::duration>(
                        std::chrono::duration<double>(static_cast<double>(s.start_base) / 66.0)),
                        s.world->tick_index(), s.input_delay);
                s.running = true;
                {
                    std::lock_guard lk(view_mutex_);
                    auto& v = views_[session];
                    v.start = clock::now();
                    v.first_tick = s.world->tick_index();
                }
                publish_state(s);
                log("[session " + std::to_string(session) + "] all " + std::to_string(s.players.size())
                    + " players ready, ticking from " + std::to_string(s.world->tick_index()));
                log_patience_on(s);
            }
            wake_.notify_all();
        });
    }

    void session_runner::set_player_lead(uint32_t session, uint32_t player, uint32_t caps, uint32_t assigned_lead) {
        post([this, session, player, caps, assigned_lead] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& lead = it->second->leads[player];
            lead.cap_lead = (caps & bmmo::session::CAP_LEAD) != 0;
            lead.assigned_lead = assigned_lead;
        });
    }

    void session_runner::set_player_rtt(uint32_t session, uint32_t player, uint32_t rtt_ms) {
        post([this, session, player, rtt_ms] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto lead = it->second->leads.find(player);
            if (lead == it->second->leads.end()) return;
            lead->second.rtt_ms = rtt_ms;
            lead->second.have_rtt = true;
        });
    }

    void session_runner::acknowledge_lead(uint32_t session, uint32_t player, uint32_t id, int applied) {
        post([this, session, player, id, applied] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& s = *it->second;
            auto lead = s.leads.find(player);
            auto meter = s.lateness.find(player);
            if (lead == s.leads.end() || meter == s.lateness.end()) return;
            const uint32_t effective = lead->second.effective;
            const uint32_t tick = s.world ? s.world->tick_index() : 0;
            const std::string prefix = "[session " + std::to_string(s.id) + "] S1 lead-ack player=" + std::to_string(player)
                + " id=" + std::to_string(id) + " applied=" + std::to_string(applied) + " tick=" + std::to_string(tick);
            int counted = 0;
            if (!bmmo::session::lead_acknowledge(lead->second, meter->second.meter, id, applied, &counted)) {
                log(prefix + ": not the outstanding decision, ignored");
                return;
            }
            const auto& m = meter->second.meter;
            log(prefix + (counted != applied ? " counted=" + std::to_string(counted) : std::string())
                + " effective=" + std::to_string(effective) + " offset=" + std::to_string(m.offset())
                + " cooldown-until=" + std::to_string(m.cooldown_until()) + (m.at_cap() ? " at-cap" : ""));
            if (s.journal.is_open() && !s.journal.capped())
                s.journal.note(tick, "lead ack: p" + std::to_string(player) + " id " + std::to_string(id)
                               + " applied " + std::to_string(counted) + ", offset " + std::to_string(m.offset()));
        });
    }

    void session_runner::submit_input(uint32_t session, uint32_t player, uint32_t first_tick,
                                      std::vector<bmmo::session::input_frame> frames, clock::time_point arrival) {
        post([this, session, player, first_tick, arrival, frames = std::move(frames)] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& s = *it->second;
            auto buffer = s.inputs.find(player);
            if (buffer == s.inputs.end()) return;
            // The lookahead the buffer judges this datagram by is the one
            // before it is stored.
            const uint32_t cursor = buffer->second.next_tick();
            buffer->second.submit(first_tick, frames);
            // One lateness sample per datagram, from its newest frame
            // (design 2.1).  Only once the scheduler has deadlines, and never
            // for a frame the buffer drops as garbage: a far-future tick
            // would be "newest" for good and block every real sample after it.
            if (frames.empty() || !s.running || !s.scheduler.started()) return;
            const uint32_t newest = first_tick + static_cast<uint32_t>(frames.size()) - 1;
            if (newest < s.scheduler.first_tick()
                    || newest >= cursor + bmmo::session::input_buffer::kMaxLookahead) return;
            if (config_.patience) {
                // Patience's cost (design 2.6), for the S1 patience line: a
                // frame of a tick the session stepped early without it, that
                // still beat that tick's deadline, is one the deadline would
                // have taken.  A cut tick whose deadline this arrival is past
                // can no longer be taken by anything later.
                if (auto p = s.patience.find(player); p != s.patience.end()) {
                    auto& cut = p->second.cut;
                    while (!cut.empty() && s.scheduler.deadline(cut.front().first) <= arrival) cut.pop_front();
                    for (auto c = cut.begin(); c != cut.end();) {
                        if (c->first < first_tick || c->first > newest) { ++c; continue; }
                        ++(c->second ? p->second.taken_waiver : p->second.taken_patience);
                        c = cut.erase(c);
                    }
                }
            }
            auto meter = s.lateness.find(player);
            if (meter == s.lateness.end()) return;
            const double slack = bmmo::session::lateness_slack_ticks(s.scheduler.deadline(newest), arrival);
            const bool sampled = meter->second.meter.sample(newest, slack);
            // E's c_est (design 2.3) is measured from the newest uplink tick
            // and the session tick it arrived at.
            if (sampled && config_.lead_adjust && s.world)
                if (auto lead = s.leads.find(player); lead != s.leads.end())
                    lead->second.received(newest, s.world->tick_index());
            if (!sampled || !config_.patience) return;
            // The streak waiver ends with the first sample inside the
            // player's patience; the next tick the runner decides waits for
            // it again.
            if (auto p = s.patience.find(player); p != s.patience.end() && p->second.waiver.waived())
                p->second.waiver.sample(slack, patience_lead(s, player));
        });
    }

    void session_runner::submit_event(uint32_t session, uint32_t player, uint32_t tick, lifecycle_event event,
                                      uint64_t token) {
        const auto arrival = clock::now();
        post([this, session, player, tick, arrival, token, event = std::move(event)]() mutable {
            // Not taken: the server still lets go of the message it parked.
            const auto dropped = [&] {
                if (!callbacks_.on_event_accepted) return;
                accepted_event a;
                a.session = session;
                a.player = player;
                a.origin = tick;
                a.applied = tick;
                a.type = event.type;
                a.dropped = true;
                a.token = token;
                a.intake = arrival;
                callbacks_.on_event_accepted(a);
            };
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) { dropped(); return; }
            auto& s = *it->second;
            // A player removed before its event got here: the world has no
            // slot for it, and its leaver's Unphysicalize is already out.
            if (!s.players.count(player)) { dropped(); return; }
            auto& events = s.events;
            // Late-player design 3.1: accepted here, where the session's next
            // tick N is exact, at applied = max(stamp, N) - never before its
            // stamp, and never behind the session, so every side applies it at
            // the same tick.  A birth overtaken by a newer accepted event of
            // its player is superseded: not queued, not relayed to the others.
            const uint32_t next = s.world->tick_index();
            const auto type = event.type;
            const auto verdict = s.acceptance.accept(player, type, tick, next);
            const auto accepted = [&](bool superseded) {
                if (!callbacks_.on_event_accepted) return;
                accepted_event a;
                a.session = s.id;
                a.player = player;
                a.origin = tick;
                a.applied = verdict.applied;
                a.type = type;
                a.superseded = superseded;
                a.token = token;
                a.intake = arrival;
                callbacks_.on_event_accepted(a);
            };
            if (verdict.superseded) {
                ++s.superseded_births;
                if (s.superseded_births <= 50)
                    log("[session " + std::to_string(s.id) + "] Physicalize from " + std::to_string(player)
                        + " stamped " + std::to_string(tick) + " would apply at " + std::to_string(verdict.applied)
                        + " after its newer " + (verdict.by_type == bmmo::session::event_type::Physicalize
                                                 ? "Physicalize" : "Unphysicalize")
                        + " stamped " + std::to_string(verdict.by_origin) + " (applied "
                        + std::to_string(verdict.by_applied) + "): superseded, never applied");
                // Design 4: a superseded birth is never applied, so the box
                // has it as a note only.
                if (s.journal.is_open() && !s.journal.capped())
                    s.journal.note(next, "superseded: p" + std::to_string(player) + " Physicalize stamped "
                                   + std::to_string(tick) + " (by origin " + std::to_string(verdict.by_origin) + ")");
                accepted(true);
                return;
            }
            if (verdict.applied != tick) ++s.restamped_events;
            // Every other member's Sector margin (design 2.9): the server
            // relays the event the moment it takes it, so it reaches member
            // q at about q's c_est, and the stamp less that is how far ahead
            // of q's clock it lands.  A raise of q must leave that positive
            // until a late remote Sector is rewound for (plan step 14).  An
            // event already late at the server is the sender's lateness, not
            // q's path, and is no sample (lead_sector_sample).
            if (config_.lead_adjust && type == bmmo::session::event_type::Sector) {
                for (auto& [other, lead]: s.leads)
                    if (other != player) bmmo::session::lead_sector_sample(lead, tick, next);
            }
            const uint32_t applied = verdict.applied;
            pending_event pe{player, tick, applied, std::move(event), arrival};
            auto pos = std::upper_bound(events.begin(), events.end(), applied,
                [](uint32_t t, const pending_event& e) { return static_cast<int32_t>(t - e.applied) < 0; });
            events.insert(pos, std::move(pe));
            accepted(false);
        });
    }

    void session_runner::request_full_snapshot(uint32_t session) {
        post([this, session] {
            auto it = sessions_.find(session);
            if (it != sessions_.end()) it->second->cadence.force_full();
        });
    }

    void session_runner::describe(uint32_t session) {
        post([this, session] {
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) {
                log("[session " + std::to_string(session) + "] no such session");
                return;
            }
            auto& s = *it->second;
            std::string text = s.world->describe() + " running=" + (s.running ? "1" : "0")
                + " ready=" + std::to_string(s.ready.size()) + "/" + std::to_string(s.players.size())
                + " pending_events=" + std::to_string(s.events.size())
                + " restamped=" + std::to_string(s.restamped_events)
                + " superseded=" + std::to_string(s.superseded_births) + "/" + std::to_string(s.superseded_events)
                + " barrier=" + (config_.lifecycle_barrier ? "on" : "off");
            for (auto& [player, buffer]: s.inputs)
                text += " in[" + std::to_string(player) + "]=" + std::to_string(buffer.pending())
                      + "@" + std::to_string(buffer.last_fresh_tick())
                      + " next=" + std::to_string(buffer.next_tick())
                      + " rx=" + std::to_string(buffer.received())
                      + " kept=" + std::to_string(buffer.stored())
                      + " late=" + std::to_string(buffer.stale());
            log("[session " + std::to_string(session) + "] " + text);
        });
    }

    void session_runner::emit_snapshot(session_state& s, bool full) {
        session_snapshot snapshot;
        snapshot.session = s.id;
        snapshot.tick = s.world->tick_index() - 1;  // the tick just simulated
        snapshot.full = full;
        s.world->snapshot(full, snapshot.bodies);
        for (const auto& [player, tick]: s.acked) snapshot.acked_inputs.emplace_back(player, tick);
        if (callbacks_.on_snapshot) callbacks_.on_snapshot(snapshot);
    }

    // True while an input for `tick` claims a physicalized ball the world does
    // not have and no Physicalize event for it has arrived yet; gives the
    // reliable event up to a second to catch up with the unreliable input.
    bool session_runner::waiting_for_lifecycle(session_state& s, uint32_t tick) {
        std::vector<uint32_t> blocking;
        for (const auto& [player, buffer]: s.inputs) {
            const auto* frame = buffer.peek(tick);
            if (!frame || !(frame->flags & bmmo::session::INPUT_FLAG_PHYSICALIZED)) continue;
            if (s.world->player_physicalized(player)) { s.lifecycle_missing.erase(player); continue; }
            // A Physicalize that belongs to the frame at `tick` is stamped at
            // most tick + input_delay + 1 (see session/lifecycle.hpp), so ask
            // for that horizon: the event is already queued, and waiting for it
            // does not apply it any earlier than its own stamp tick.
            const uint32_t horizon = tick + s.input_delay + 1;
            if (bmmo::session::physicalize_pending(s.events, player, horizon)) {
                s.lifecycle_missing.erase(player);
                continue;
            }
            // Already waited a second for this player's event and gave up; the
            // session must keep running at full speed for everybody else.
            if (s.lifecycle_missing.count(player)) continue;
            blocking.push_back(player);
        }
        if (blocking.empty()) {
            s.barrier_armed = false;
            return false;
        }
        const auto now = clock::now();
        if (!s.barrier_armed) {
            s.barrier_armed = true;
            s.barrier_deadline = now + std::chrono::seconds(1);
            return true;
        }
        if (now >= s.barrier_deadline) {
            s.barrier_armed = false;
            for (uint32_t player: blocking) {
                s.lifecycle_missing.insert(player);
                log("[session " + std::to_string(s.id) + "] no Physicalize for player " + std::to_string(player)
                    + " at tick " + std::to_string(tick) + "; its ball stays out of the simulation until one arrives");
            }
            return false;
        }
        return true;
    }

    // One simulated tick of a running session: inputs, due events, physics,
    // snapshot.
    void session_runner::step(session_state& s) {
        const uint32_t tick = s.world->tick_index();
        // Everything the journal costs hangs off this: a capped or absent box
        // does not even capture a hash.
        const bool recording = s.journal.is_open() && !s.journal.capped();
        // Leavers whose queued events are all applied go before this step,
        // which is where the PLAYER record at this tick puts them in a replay.
        for (auto l = s.leaving.begin(); l != s.leaving.end();) {
            const uint32_t player = l->first;
            const bool due = static_cast<int32_t>(l->second.remove_at - tick) <= 0;
            ++l;
            if (due) finish_leave(s, player, tick);
        }
        std::vector<bmmo::session::applied_input> applied;
        applied.reserve(s.inputs.size());
        for (auto& [player, buffer]: s.inputs) {
            bool fresh = false;
            uint32_t source = bmmo::session::INPUT_SOURCE_DEFAULT;
            // Before take() moves the cursor past it.
            const bool settled = buffer.settled(tick);
            const auto& frame = buffer.take(tick, fresh, source);
            if (fresh) s.acked[player] = tick;
            // The streak waiver (design 2.6) starts at a substitution of a
            // frame the step had to do without.  A tick below a re-anchored
            // cursor was never going to get a frame, so its substitute is not
            // lateness and waives nothing.
            if (config_.patience && !fresh && !settled) s.patience[player].waiver.substituted();
            s.world->set_input(player, frame);
            s.last_frames[player] = frame;
            applied.push_back(bmmo::session::applied_input{player, frame, !fresh, source});
            // What the world consumed, not what arrived on the wire: the wire
            // can lose, duplicate and reorder, the world cannot.  The source
            // of a substitute is not recorded: the applied bytes are what a
            // replay needs, and the server's S2 log lines carry the source.
            if (recording)
                s.journal.input(tick, player, frame, fresh ? bmmo::session::JOURNAL_INPUT_FRESH : 0);
        }
        // A leaver still in the world is driven with the last frame applied
        // for it, which is the last one relayed - what every member's mirror
        // of it goes on with - and is not relayed (it is no member).
        for (const auto& [player, leaving]: s.leaving) {
            s.world->set_input(player, leaving.frame);
            if (recording) s.journal.input(tick, player, leaving.frame, 0);
        }
        // Every step, one player or several: the relay's header and held
        // section are the owner's, and a solo player needs them as much as
        // anyone (late-player design 1.2).
        if (callbacks_.on_inputs && !applied.empty()) callbacks_.on_inputs(s.id, tick, applied);
        // Every event due this tick, in the members' JOIN ORDER - not in the
        // order the network happened to deliver them.  The order two events of
        // the same tick are applied in changes the world (two balls
        // physicalized in one tick are created in that order), so it is part of
        // the determinism contract now, and join order is the one order a
        // client's own journal can reproduce offline: nobody but the server
        // knows who got there first, but every member knows every join order.
        // An unknown player (its removal raced the event) sorts after all known
        // ones; stable_sort keeps arrival order within one rank.
        //
        // Within one player, by ORIGIN and then type rank (late-player design
        // 3.1): events re-stamped into one tick by an outage keep the order
        // their owner made them in - a birth and the death stamped after it
        // leave no ball, the other way round they would leave one.
        size_t due_count = 0;
        while (due_count < s.events.size() && static_cast<int32_t>(s.events[due_count].applied - tick) <= 0) ++due_count;
        std::vector<pending_event> due(std::make_move_iterator(s.events.begin()),
                                       std::make_move_iterator(s.events.begin() + due_count));
        s.events.erase(s.events.begin(), s.events.begin() + due_count);
        const auto join_rank = [&s](uint32_t player) {
            const auto it = s.join_orders.find(player);
            return it == s.join_orders.end() ? 256u : static_cast<uint32_t>(it->second);
        };
        std::stable_sort(due.begin(), due.end(), [&join_rank](const pending_event& a, const pending_event& b) {
            return bmmo::session::lifecycle_apply_before(join_rank(a.player), a.origin, a.event.type,
                                                         join_rank(b.player), b.origin, b.event.type);
        });
        for (pending_event& e: due) {
            // Protocol 6: the copies of a stamp's events can overtake each
            // other, so an Unphysicalize stamped at or before the player's
            // last applied Physicalize is one that lost its place; applied, it
            // would take the newer ball out.  Compared on origins (design
            // 3.1), as every receiver does.
            if (e.event.type == bmmo::session::event_type::Unphysicalize) {
                const auto last = s.last_physicalize_tick.find(e.player);
                if (bmmo::session::lifecycle_unphysicalize_superseded(e.origin, last != s.last_physicalize_tick.end(),
                                                                      last == s.last_physicalize_tick.end() ? 0 : last->second)) {
                    if (++s.superseded_events <= 20)
                        log("[session " + std::to_string(s.id) + "] Unphysicalize from " + std::to_string(e.player)
                            + " stamped " + std::to_string(e.origin) + " is older than its Physicalize stamped "
                            + std::to_string(last->second) + ": superseded");
                    continue;
                }
            } else if (e.event.type == bmmo::session::event_type::Physicalize) {
                s.last_physicalize_tick[e.player] = e.origin;
            }
            // An event that arrived after its stamp tick was simulated was
            // re-stamped at acceptance (design 3.1): applied now, and relayed
            // with this tick, so every receiver applies it here too and the
            // late arrival is no divergence any more.  Still said once per
            // event, capped, as a diagnostic: a journal reader sees the stamp
            // beside the applied tick in the EVENT record as well.
            //
            // With patience on, one whose stamp tick was stepped early with a
            // member unsettled, but which arrived before that tick's deadline,
            // is one patience made late (the deadline schedule would have
            // waited longer for that tick): counted for the S1 patience line
            // and noted in the box.
            bool by_patience = false;
            if (config_.patience && static_cast<int32_t>(e.origin - tick) < 0
                    && std::find(s.early_ticks.begin(), s.early_ticks.end(), e.origin) != s.early_ticks.end()
                    && e.arrival < s.scheduler.deadline(e.origin)) {
                by_patience = true;
                ++s.patience[e.player].late_events;
                if (recording)
                    s.journal.note(tick, "patience: event type " + std::to_string(static_cast<int>(e.event.type))
                                   + " from " + std::to_string(e.player) + " stamped " + std::to_string(e.origin)
                                   + " arrived before its deadline but after its early step");
            }
            if (static_cast<int32_t>(e.origin - tick) < 0 && ++s.late_events <= 50)
                log("[session " + std::to_string(s.id) + "] event type " + std::to_string(static_cast<int>(e.event.type))
                    + " from " + std::to_string(e.player) + " stamped " + std::to_string(e.origin) + " applied at tick "
                    + std::to_string(tick) + " (late by " + std::to_string(tick - e.origin) + "), re-stamped"
                    + (by_patience ? ", stepped early by patience" : ""));
            // The applied tick, not the stamp (design 3.1): the spawn impulse
            // direction is derived from this field, and every receiver makes
            // the ball at the applied tick with the index of that tick.  The
            // record is filed under the tick the world consumes it at, with
            // the origin beside it.
            e.event.tick = e.applied;
            if (recording) s.journal.event(to_journal_event(e.player, e.event, tick, e.origin));
            std::string error;
            if (!s.world->apply_event(e.player, e.event, error)) {
                log("[session " + std::to_string(s.id) + "] event from " + std::to_string(e.player)
                    + " stamped " + std::to_string(e.origin) + " at tick " + std::to_string(tick) + " failed: " + error);
                if (recording) s.journal.note(tick, "event failed: " + error);
            }
        }
        std::string error;
        if (!s.world->tick(error)) {
            s.failed = true;
            s.running = false;
            log("[session " + std::to_string(s.id) + "] tick " + std::to_string(tick) + " failed: " + error);
            if (recording)
                s.journal.note(tick, "end: tick " + std::to_string(tick) + " failed: " + error);
            if (callbacks_.on_failed) callbacks_.on_failed(s.id, error);
            return;
        }
        if (recording) {
            bmmo::physics::world_hash hash;
            std::string hash_error;
            // A hash the engine will not produce leaves the tick without a
            // record; a replay reports those as not compared instead of failing.
            if (bmmo::physics::capture_world_hash(s.world->physics(), hash, hash_error))
                s.journal.tick(tick, hash, static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - s.journal_start).count()));
        }
        s.scheduler.advance();
        const bool changed = s.world->body_set_changed();
        const int decision = s.cadence.decide(tick, changed);
        if (decision != 0) emit_snapshot(s, decision == 2);
        // The box's own checkpoint, last and through the read-only path:
        // snapshot() would recompute body_set_changed_ against its own call and
        // number mechanisms the server has not numbered yet, and the journal
        // must not change one byte of what the server sends.
        if (recording && config_.journal_checkpoint_ticks != 0
                && tick % config_.journal_checkpoint_ticks == 0) {
            std::vector<bmmo::session::body_state> bodies;
            s.world->snapshot_for_journal(bodies);
            s.journal.checkpoint(tick, bmmo::session::JOURNAL_CHECKPOINT_FULL, bodies);
        }
    }

    // Late-player design 2.1, observe only (plan step 7): every stepped tick
    // advances each player's lateness meter; what it would decide is logged as
    // an S1 line and changes nothing.  A decision is never acknowledged here,
    // so the meter stays pending and says it at most once per assignment -
    // which is what the closing measurement counts (would-raise exactly once
    // in A5u, never in the burst arms).
    //
    // Every 660 ticks one summary line per player, so a run shows the meter's
    // view of each member's arrivals without a decision having to happen.
    static constexpr uint32_t kLatenessSummaryTicks = 660;

    void session_runner::observe_lateness(session_state& s, uint32_t tick) {
        using bmmo::session::lateness_action;
        const std::string prefix = "[session " + std::to_string(s.id) + "] S1 ";
        for (auto& [player, entry]: s.lateness) {
            auto& meter = entry.meter;
            // A member with no qualifying window after eight takes the median
            // of the members whose reference was measured.  Nobody's yet:
            // it waits, and asks again next tick.  With lead_adjust on, a
            // member still without one after twelve (a solo session, or
            // every peer on the fallback) takes the deadline reference, so
            // a member late from its start can still be raised; with the
            // flag off nothing changes from the observe-only meter.
            if (meter.wants_fallback()) {
                std::vector<double> r10s, r01s;
                for (const auto& [other, other_entry]: s.lateness) {
                    if (other == player || !other_entry.meter.reference_measured()) continue;
                    r10s.push_back(other_entry.meter.r10());
                    r01s.push_back(other_entry.meter.r01());
                }
                if (config_.lead_adjust) bmmo::session::lateness_settle_reference(meter, r10s, r01s);
                else if (!r10s.empty())
                    meter.adopt_reference(bmmo::session::lateness_median(r10s), bmmo::session::lateness_median(r01s));
            }
            if (meter.has_reference() && !entry.reported) {
                entry.reported = true;
                log(prefix + "reference player=" + std::to_string(player) + " tick=" + std::to_string(tick)
                    + " r10=" + fixed2(meter.r10()) + " r01=" + fixed2(meter.r01())
                    + " source=" + meter.reference_source()
                    + " windows=" + std::to_string(meter.windows()) + "/" + std::to_string(meter.qualifying_windows())
                    + " base=" + std::to_string(meter.base()));
            }
            const auto decision = meter.on_tick(tick);
            if (decision.action == lateness_action::raise || decision.action == lateness_action::lower) {
                const bool raise = decision.action == lateness_action::raise;
                const std::string text = std::string(raise ? "would-raise" : "would-lower")
                    + " player=" + std::to_string(player) + " tick=" + std::to_string(tick)
                    + " k=" + std::to_string(decision.k)
                    + (raise ? " q10=" : " q05=") + fixed2(decision.quantile)
                    + " r10=" + fixed2(meter.r10()) + " r01=" + fixed2(meter.r01())
                    + " misses=" + std::to_string(decision.misses) + "/" + std::to_string(decision.samples)
                    + " streak=" + std::to_string(decision.streak)
                    + " offset=" + std::to_string(meter.offset());
                if (config_.lead_adjust) {
                    apply_lead_decision(s, player, decision, text);
                } else {
                    log(prefix + text + " (observe only: nothing sent)");
                    // The box keeps it too, so a journal read offline shows where
                    // the lead would have moved.  A note changes no replay.
                    if (s.journal.is_open() && !s.journal.capped()) s.journal.note(tick, "lateness: " + text);
                }
            } else if (decision.action == lateness_action::give_up) {
                log(prefix + "gave-up player=" + std::to_string(player) + " tick=" + std::to_string(tick)
                    + " misses=" + std::to_string(decision.misses) + "/" + std::to_string(decision.samples)
                    + ": two raises did not restore its arrivals; no decision until its next assignment");
            }
        }
        if (tick % kLatenessSummaryTicks != 0) return;
        for (const auto& [player, entry]: s.lateness) {
            const auto& meter = entry.meter;
            std::string text = "lateness player=" + std::to_string(player) + " tick=" + std::to_string(tick)
                + " state=" + bmmo::session::lateness_state_name(meter.state())
                + " samples=" + std::to_string(meter.samples_total())
                + " misses=" + std::to_string(meter.misses_total())
                + " trailing=" + std::to_string(meter.trailing_misses()) + "/" + std::to_string(meter.trailing_count());
            if (meter.have_sampled()) text += " last=" + fixed2(meter.last_slack());
            if (meter.trailing_count() > 0)
                text += " q01=" + fixed2(meter.trailing_quantile(0.01)) + " q10=" + fixed2(meter.trailing_quantile(0.10))
                      + " q50=" + fixed2(meter.trailing_quantile(0.50));
            if (meter.has_reference())
                text += " r10=" + fixed2(meter.r10()) + " r01=" + fixed2(meter.r01())
                      + " ref=" + meter.reference_source();
            else
                text += " ref=none windows=" + std::to_string(meter.windows()) + "/" + std::to_string(meter.qualifying_windows());
            text += " streak=" + std::to_string(meter.raise_streak()) + "/" + std::to_string(meter.lower_streak())
                  + " decisions=" + std::to_string(meter.decisions()) + " offset=" + std::to_string(meter.offset());
            if (config_.lead_adjust) {
                const auto l = s.leads.find(player);
                if (l != s.leads.end()) {
                    const auto& lead = l->second;
                    text += std::string(" lead: caps=") + (lead.cap_lead ? "lead" : "none")
                          + " assigned=" + std::to_string(lead.assigned_lead)
                          + " rtt=" + (lead.have_rtt ? std::to_string(lead.rtt_ms) : std::string("?"))
                          + " sent=" + std::to_string(lead.sent) + " acked=" + std::to_string(lead.acked)
                          + " observed=" + std::to_string(lead.observed) + " capped=" + std::to_string(lead.capped)
                          + " declined=" + std::to_string(lead.declined)
                          + (lead.outstanding ? " outstanding=" + std::to_string(lead.id) : std::string())
                          + (lead.sector_margin.known() ? " sector-margin=" + std::to_string(lead.sector_margin.worst())
                                                        : std::string(" sector-margin=?"))
                          + (meter.at_cap() ? " at-cap" : "");
                }
            }
            log(prefix + text);
        }
    }

    // Late-player design 2.2-2.3 (runner_config::lead_adjust).  A decision for
    // a client that declared CAP_LEAD is sent: a raise first resets the
    // player's input buffer over [N, E) with the substitute frozen, which
    // changes nothing in the world (those ticks are substituted the way a
    // missing frame always is) and so nothing a journal replay needs to know.
    // Without CAP_LEAD the decision stays pending, unacknowledged, so the
    // player gets this one logged decision per assignment and nothing is
    // sent.
    void session_runner::apply_lead_decision(session_state& s, uint32_t player,
                                             const bmmo::session::lateness_decision& d, const std::string& text) {
        using bmmo::session::lead_route;
        const std::string prefix = "[session " + std::to_string(s.id) + "] S1 ";
        auto meter = s.lateness.find(player);
        auto buffer = s.inputs.find(player);
        if (meter == s.lateness.end() || buffer == s.inputs.end() || !s.world) return;
        auto& lead = s.leads[player];
        const uint32_t next_tick = s.world->tick_index();
        const auto plan = bmmo::session::lead_decide(true, lead, meter->second.meter, d, next_tick, buffer->second);
        const bool recording = s.journal.is_open() && !s.journal.capped();
        if (plan.route == lead_route::observe) {
            log(prefix + text + " (no CAP_LEAD from this client: nothing sent)");
            if (recording) s.journal.note(next_tick, "lateness: " + text + " (no CAP_LEAD)");
            return;
        }
        if (plan.route == lead_route::declined || plan.route == lead_route::deferred) {
            const bool declined = plan.route == lead_route::declined;
            log(prefix + text + (declined ? " declined" : " deferred") + ": the " + (plan.binding ? plan.binding : "?")
                + " cap leaves nothing of it (caps " + (plan.cap ? plan.cap : "?")
                + ", assigned " + std::to_string(lead.assigned_lead)
                + ", offset " + std::to_string(meter->second.meter.offset())
                + (lead.sector_margin.known() ? ", sector margin " + std::to_string(lead.sector_margin.worst()) : std::string())
                + (declined ? "); no further raise this assignment"
                            : "); no decision before tick " + std::to_string(meter->second.meter.cooldown_until())));
            if (recording)
                s.journal.note(next_tick, "lateness: " + text + (declined ? " declined at the cap" : " deferred by the sector margin"));
            return;
        }
        if (plan.route != lead_route::send) return;
        std::string line = "lead player=" + std::to_string(player) + " id=" + std::to_string(d.id)
            + " k=" + std::to_string(plan.k) + (plan.k != plan.decided_k ? " decided=" + std::to_string(plan.decided_k) : std::string())
            + (plan.cap ? std::string(" cap=") + plan.cap + " binding=" + plan.binding : std::string())
            + " N=" + std::to_string(next_tick) + " held-from=" + std::to_string(plan.held_from)
            + " E=" + std::to_string(plan.effective)
            + " c-est=" + (plan.have_client_tick ? std::to_string(plan.client_tick) : std::string("?"))
            + " rtt=" + (lead.have_rtt ? std::to_string(lead.rtt_ms) : std::string("?"))
            + " held-source=" + (plan.held_source == bmmo::session::INPUT_SOURCE_DEFAULT ? std::string("default")
                                                                                      : std::to_string(plan.held_source))
            + " dropped=" + std::to_string(plan.dropped) + " epoch=" + std::to_string(meter->second.meter.base());
        log(prefix + text + "; " + line);
        // Design 2.3 (S3): the reset is in the box as a note only; the
        // range's frames are journaled with flag 0 as every substitute is.
        if (recording)
            s.journal.note(next_tick, "lead: p" + std::to_string(player) + " " + (plan.k > 0 ? "+" : "")
                           + std::to_string(plan.k) + " at " + std::to_string(next_tick) + ", effective "
                           + std::to_string(plan.effective) + "; buffer reset dropped " + std::to_string(plan.dropped)
                           + " frames");
        if (callbacks_.on_lead) {
            lead_notice notice;
            notice.session = s.id;
            notice.player = player;
            notice.id = d.id;
            notice.k = plan.k;
            notice.held_from = plan.held_from;
            notice.effective = plan.effective;
            notice.held = plan.held;
            notice.held_source = plan.held_source;
            notice.epoch = meter->second.meter.base();
            callbacks_.on_lead(notice);
        }
    }

    std::vector<bmmo::session::patience_member> session_runner::patience_members(const session_state& s,
                                                                               uint32_t tick) const {
        std::vector<bmmo::session::patience_member> members;
        members.reserve(s.inputs.size());
        for (const auto& [player, buffer]: s.inputs) {
            if (!config_.patience) {
                bmmo::session::patience_member m;
                m.has = buffer.has(tick);
                // Only a lead reset (lead_adjust) makes a held range; without
                // one this is false and the test is the old has-only rule.
                m.held = buffer.held(tick);
                members.push_back(m);
                continue;
            }
            const auto p = s.patience.find(player);
            const auto l = s.lateness.find(player);
            members.push_back(bmmo::session::patience_member_of(buffer.has(tick), buffer.settled(tick),
                    p != s.patience.end() && p->second.waiver.waived(),
                    l == s.lateness.end() ? nullptr : &l->second.meter, s.start_base));
        }
        if (config_.patience) bmmo::session::patience_assign_leads(members);
        return members;
    }

    double session_runner::patience_lead(const session_state& s, uint32_t player) const {
        const auto members = patience_members(s, s.world ? s.world->tick_index() : 0);
        size_t i = 0;
        for (const auto& entry: s.inputs) {
            if (entry.first == player) return members[i].lead;
            ++i;
        }
        return 0.0;
    }

    // Every 660 ticks with patience on, one line per player: its lead and
    // where it came from, whether it is waived, and what patience cost it -
    // the ticks it was substituted because the session stepped early without
    // it, how many of those frames then beat the deadline, i.e. would have
    // been taken fresh without patience (split by patience proper and the
    // waiver), and how many of its events patience made late.  The A0j arm
    // reads deadline-would-take to judge the margin.  The counters are
    // cumulative; a `final=` line closes them when the player leaves or the
    // session ends, so nothing after the last 660-tick line is lost.
    void session_runner::report_patience(session_state& s, uint32_t tick, const char* final_reason,
                                         const uint32_t* only) {
        if (final_reason == nullptr && tick % kLatenessSummaryTicks != 0) return;
        const auto members = patience_members(s, tick);
        const session_state::patience_entry none{};
        size_t i = 0;
        for (const auto& [player, buffer]: s.inputs) {
            const auto& m = members[i++];
            if (only != nullptr && *only != player) continue;
            const auto it = s.patience.find(player);
            const auto& e = it == s.patience.end() ? none : it->second;
            log("[session " + std::to_string(s.id) + "] S1 patience player=" + std::to_string(player)
                + " tick=" + std::to_string(tick) + " lead=" + fixed2(m.lead)
                + " source=" + (m.has_statistics ? "own" : (m.capped ? "cap" : "deadline"))
                + " waived=" + (e.waiver.waived() ? "1" : "0")
                + " waivers=" + std::to_string(e.waiver.episodes())
                + " cut=" + std::to_string(e.cut_patience + e.cut_waiver)
                + " (patience " + std::to_string(e.cut_patience) + ", waiver " + std::to_string(e.cut_waiver) + ")"
                + " deadline-would-take=" + std::to_string(e.taken_patience + e.taken_waiver)
                + " (patience " + std::to_string(e.taken_patience) + ", waiver " + std::to_string(e.taken_waiver) + ")"
                + " late-events=" + std::to_string(e.late_events)
                + " early-steps=" + std::to_string(s.early_steps)
                + (final_reason != nullptr ? std::string(" final=") + final_reason : std::string()));
        }
    }

    // Once per session when it starts ticking, on either start path (the last
    // member ready, or the last unready member gone).
    void session_runner::log_patience_on(const session_state& s) {
        if (!config_.patience) return;
        log("[session " + std::to_string(s.id) + "] S1 patience on: a player is waited for until "
            "deadline - (r01 - " + fixed2(bmmo::session::kPatienceMarginTicks)
            + ") ticks, with the streak waiver and the pacing floor (late-player design 2.6)");
    }

    void session_runner::run() {
        std::unique_lock lk(mutex_);
        while (!stopping_) {
            while (!commands_.empty()) {
                auto command = std::move(commands_.front());
                commands_.pop_front();
                lk.unlock();
                command();
                lk.lock();
            }
            lk.unlock();

            auto sleep_for = std::chrono::milliseconds(20);
            const auto now = clock::now();
            for (auto& [id, sp]: sessions_) {
                auto& s = *sp;
                if (!s.running || s.failed || !s.world) continue;
                uint32_t simulated = 0;
                // When the tick waits for a player's patience (design 2.6),
                // how long until it ends: a blacked-out player posts nothing,
                // so nothing else would wake the runner there.
                bool patience_wait = false;
                std::chrono::milliseconds patience_sleep{0};
                while (simulated < config_.max_catch_up_ticks) {
                    const uint32_t tick = s.world->tick_index();
                    // With patience off this is the rule the runner always had:
                    // every buffer has its frame, or the deadline passed.
                    const auto members = patience_members(s, tick);
                    const auto at = clock::now();
                    const auto verdict = bmmo::session::patience_step_test(members, s.scheduler.due(at),
                            config_.patience ? bmmo::session::lateness_slack_ticks(s.scheduler.deadline(tick), at) : 0.0,
                            config_.patience);
                    if (!verdict.step) {
                        if (verdict.wait) {
                            patience_wait = true;
                            patience_sleep = std::chrono::ceil<std::chrono::milliseconds>(
                                    std::chrono::duration<double, std::micro>(
                                            verdict.wait_ticks * bmmo::session::kLatenessTickMicros));
                        }
                        break;
                    }
                    if (config_.lifecycle_barrier && waiting_for_lifecycle(s, tick)) break;
                    if (verdict.early) {
                        // Stepped before the deadline without some frames:
                        // remember which, so a frame that still beats the
                        // deadline is counted as one patience cut off.
                        ++s.early_steps;
                        s.early_ticks.push_back(tick);
                        if (s.early_ticks.size() > 256) s.early_ticks.pop_front();
                        size_t i = 0;
                        for (const auto& [player, buffer]: s.inputs) {
                            const auto& m = members[i++];
                            // Not ready yet: nothing was ever coming for it.
                            if (m.settled || !s.ready.count(player)) continue;
                            auto& entry = s.patience[player];
                            ++(m.waived ? entry.cut_waiver : entry.cut_patience);
                            entry.cut.emplace_back(tick, m.waived);
                            if (entry.cut.size() > 256) entry.cut.pop_front();
                        }
                    }
                    step(s);
                    if (s.running) {
                        observe_lateness(s, tick);
                        if (config_.patience) report_patience(s, tick);
                    }
                    ++simulated;
                    if (!s.running) break;
                }
                publish_state(s);
                if (s.running) {
                    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(s.scheduler.until_due(clock::now()));
                    if (patience_wait) wait = std::min(wait, patience_sleep);
                    sleep_for = std::min(sleep_for, std::max(std::chrono::milliseconds(1), wait));
                }
            }
            (void)now;

            lk.lock();
            if (!commands_.empty() || stopping_) continue;
            wake_.wait_for(lk, sleep_for);
        }
        lk.unlock();
        // A session still running when the server goes down says so in its box
        // rather than just stopping mid-file.
        for (auto& [id, sp]: sessions_) {
            if (!sp) continue;
            if (config_.patience && sp->running && sp->world)
                report_patience(*sp, sp->world->tick_index(), "server stopped");
            sp->journal.note(sp->world ? sp->world->tick_index() : 0, "end: server stopped");
        }
        sessions_.clear();
    }
}
