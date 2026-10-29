#include "session_runner.hpp"
#include "late_tick.hpp"

#include "CKAll.h"

#include <physics/physics_state.hpp>
#include <session/lifecycle.hpp>
#include <session/member_lives.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

        // Step B2SW: BMMO_SESSION_STALL=TICK:MS[,TICK:MS...] (the harness's
        // server-side stall); a malformed entry is skipped.
        std::map<uint32_t, uint32_t> parse_session_stalls(const char* text) {
            std::map<uint32_t, uint32_t> out;
            if (!text) return out;
            const std::string spec = text;
            size_t from = 0;
            while (from < spec.size()) {
                const size_t comma = spec.find(',', from);
                const std::string item = spec.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                const size_t colon = item.find(':');
                if (colon != std::string::npos && colon > 0 && colon + 1 < item.size()) {
                    const unsigned long tick = std::strtoul(item.substr(0, colon).c_str(), nullptr, 10);
                    const unsigned long ms = std::strtoul(item.substr(colon + 1).c_str(), nullptr, 10);
                    if (ms > 0 && ms <= 10000) out[static_cast<uint32_t>(tick)] = static_cast<uint32_t>(ms);
                }
                if (comma == std::string::npos) break;
                from = comma + 1;
            }
            return out;
        }
    }

    session_runner::session_runner(runner_config config, session_callbacks callbacks)
        : config_(std::move(config)), callbacks_(std::move(callbacks)) {
        debug_stalls_ = parse_session_stalls(std::getenv("BMMO_SESSION_STALL"));
        for (const auto& [tick, ms]: debug_stalls_)
            log("debug: BMMO_SESSION_STALL holds the step of tick " + std::to_string(tick) + " for " + std::to_string(ms)
                + " ms (a harness stall; unset in deployments)");
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
        // Step M1 (J4): the largest removal tick of a leaving life.
        uint32_t horizon = 0;
        for (const auto& [player, leaving]: s.leaving) {
            (void) player;
            if (horizon == 0 || static_cast<int32_t>(leaving.remove_at - horizon) > 0) horizon = leaving.remove_at;
        }
        v.leaving_horizon = horizon;
    }

    uint32_t session_runner::leaving_horizon(uint32_t session) const {
        std::lock_guard lk(view_mutex_);
        auto it = views_.find(session);
        return it == views_.end() ? 0 : it->second.leaving_horizon;
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

    uint32_t session_runner::reanchor_tick(uint32_t session, uint32_t lead, bool resync_state) const {
        std::lock_guard lk(view_mutex_);
        auto it = views_.find(session);
        if (it == views_.end()) return lead;
        const auto& v = it->second;
        if (!v.scheduled) return v.next_tick + lead;
        return bmmo::sim::reanchor_tick_on_schedule(v.schedule_start, v.first_tick, clock::now(), lead, v.next_tick,
                                                    bmmo::sim::reanchor_margin_ticks(resync_state));
    }

    // Both start paths (the last unready member left, the last member became
    // ready) start the deadline schedule here, so the cross-thread view's
    // copy of it (reanchor_tick) cannot be left out of either (step RL1b).
    void session_runner::start_ticking(session_state& s) {
        // Design 9.26: the first step (tick start_base) is due start_base
        // periods from now - where a world counting from 0 reached it - so
        // the members keep the wall-clock lead the base gives them.
        const auto now = clock::now();
        const auto schedule_start = now + std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(static_cast<double>(s.start_base) / 66.0));
        s.scheduler.start(schedule_start, s.world->tick_index(), s.input_delay);
        s.running = true;
        {
            std::lock_guard lk(view_mutex_);
            auto& v = views_[s.id];
            v.start = now;
            v.first_tick = s.world->tick_index();
            v.schedule_start = schedule_start;
            v.scheduled = true;
        }
        publish_state(s);
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
                                      uint64_t anchor_surfaces, uint32_t first_tick, uint64_t anchor_placement) {
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
        header.world_options = s.world_options;
        header.anchor_placement = anchor_placement;   // journal v6 (sector placement step S5)
        const auto path = config_.journal_dir / journal_file_name(s.id, s.level, header.utc_ms);
        s.journal_start = clock::now();
        std::string error;
        if (!s.journal.open(path, header, config_.journal_max_bytes, error,
                            config_.io_strand ? config_.io_strand() : bmmo::session::io_poster{})) {
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
                                        const std::string& note, std::vector<std::string> names,
                                        uint32_t sector_lead, uint32_t world_option_bits) {
        post([this, session, level, players, input_delay, start_base, spawn_impulse, note,
              names = std::move(names), sector_lead, world_option_bits] {
            auto& s = sessions_[session];
            s = std::make_unique<session_state>();
            s->id = session;
            s->level = level;
            s->input_delay = input_delay ? input_delay : config_.input_delay;
            s->sector_lead = sector_lead;
            s->world_options = world_option_bits;
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
            options.trafo_inputs = (world_option_bits & bmmo::session::WORLD_TRAFO_INPUTS) != 0;
            // Protocol 12 (sector placement design 2.4, step S5).
            options.placement_clones = (world_option_bits & bmmo::session::WORLD_PLACEMENT_CLONES) != 0;
            // Step G2 (world-agreement-gaps-design.md §2.4): the proximity
            // union's rules; place_probes() runs first in every step.
            options.proximity_union = (world_option_bits & bmmo::session::WORLD_PROXIMITY_UNION) != 0;
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
            open_journal(*s, spawn_impulse, s->world->anchor_hash(), s->world->anchor_surfaces(), first_tick,
                         s->world->anchor_placement());
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
                    if (s->world->trafo_failed()) {
                        // Fail closed (trafo-replication design 2.4): a set
                        // that fails its invariants ends the session.
                        s->failed = true;
                        info.error = add_error;
                        s->journal.note(first_tick, "end: " + add_error);
                        s->journal.close();
                        if (callbacks_.on_world_ready) callbacks_.on_world_ready(info);
                        return;
                    }
                    continue;
                }
                s->players.insert(player);
                s->join_orders[player] = join_order;
                s->lives[player] = 1;   // step M1: a member present at the start is in its first life
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
            info.anchor_placement = s->world->anchor_placement();
            info.anchor_probes = s->world->probe_signature();   // step G3
            info.ball_rows = s->world->ball_rows();
            info.trafo_points = s->world->trafo_points();
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
                if (s.union_on() && s.world) {   // step G2
                    const auto& u = s.world->proximity();
                    const std::string text = "proximity union: skipped " + std::to_string(u.skipped()) + ", relatched "
                        + std::to_string(u.relatched()) + ", candidate set drift " + std::to_string(s.probe_set_drift);
                    log("[session " + std::to_string(s.id) + "] " + text);
                    s.journal.note(s.world->tick_index(), text);
                }
                if (s.world) {   // step M1 (J2): the slot fallbacks, 0 by the server's join orders
                    const std::string text = "member lives: join orders taken in the world "
                        + std::to_string(s.world->join_orders_taken());
                    log("[session " + std::to_string(s.id) + "] " + text);
                    s.journal.note(s.world->tick_index(), text);
                }
                s.journal.note(s.world ? s.world->tick_index() : 0,
                               "end: " + (reason.empty() ? std::string("session destroyed") : reason));
                if (s.journal.asynchronous())   // step SIO: the writer queue's high-water mark
                    log("[session " + std::to_string(s.id) + "] journal writer queue peak "
                        + std::to_string(s.journal.peak_queued()) + " bytes");
                s.journal.close();
            }
            sessions_.erase(session);
            std::lock_guard lk(view_mutex_);
            views_.erase(session);
        });
    }

    void session_runner::add_player(uint32_t session, uint32_t player, uint8_t join_order, uint16_t life,
                                    const std::string& name) {
        post([this, session, player, join_order, life, name] {
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) return;
            auto& s = *it->second;
            // Step M1 (J-f): back before its old life was taken out.  The old
            // life keeps every event it has queued (they were relayed, every
            // member applies them); this add runs right after its removal,
            // so the new life's J is the old life's R.
            if (const auto l = s.leaving.find(player); l != s.leaving.end()) {
                l->second.rejoin = true;
                l->second.rejoin_order = join_order;
                l->second.rejoin_life = life;
                l->second.rejoin_name = name;
                log("[session " + std::to_string(session) + "] player " + std::to_string(player) + " rejoined (life "
                    + std::to_string(life) + ", join order " + std::to_string(join_order)
                    + ") while its old life is leaving: added at its removal, tick " + std::to_string(l->second.remove_at));
                s.journal.note(s.world->tick_index(), "rejoin: p" + std::to_string(player) + " life " + std::to_string(life)
                               + " waits for its old life's removal at tick " + std::to_string(l->second.remove_at));
                return;
            }
            add_now(s, player, join_order, life, name);
        });
    }

    void session_runner::add_now(session_state& s, uint32_t player, uint8_t join_order, uint16_t life,
                                 const std::string& name) {
        member_change change;
        change.session = s.id;
        change.player = player;
        change.life = life;
        change.join_order = join_order;
        change.tick = s.world->tick_index();
        std::string error;
        if (!s.world->add_player(player, join_order, error)) {
            log("[session " + std::to_string(s.id) + "] add_player failed: " + error);
            change.in_world = false;
            if (s.world->trafo_failed()) {
                s.failed = true;
                s.running = false;
                s.journal.note(s.world->tick_index(), "end: " + error);
                if (callbacks_.on_failed) callbacks_.on_failed(s.id, error);
            }
            // Step M1: the member gets SessionEnd, not a SessionStart for a
            // world with no slot for it.
            if (!s.failed && callbacks_.on_player_add_failed) callbacks_.on_player_add_failed(change, error);
            return;
        }
        s.players.insert(player);
        s.join_orders[player] = join_order;
        s.lives[player] = life;
        s.inputs[player].reset(s.world->tick_index());
        s.cadence.force_full();
        s.journal.player(s.world->tick_index(), player, join_order, true, name);
        if (callbacks_.on_player_added) callbacks_.on_player_added(change);
    }

    void session_runner::finish_leave(session_state& s, uint32_t player, uint32_t tick) {
        // Step M1: what on_player_removed reports, read before the entries
        // go; and a rejoin that waited for this removal (J-f).
        member_change removed;
        removed.session = s.id;
        removed.player = player;
        removed.tick = tick;
        const auto held = s.join_orders.find(player);
        removed.in_world = held != s.join_orders.end();
        removed.join_order = removed.in_world ? held->second : 0;
        if (const auto l = s.lives.find(player); l != s.lives.end()) removed.life = l->second;
        session_state::leaving_player rejoin;
        if (const auto l = s.leaving.find(player); l != s.leaving.end() && l->second.rejoin) rejoin = l->second;
        // Join order is the world's business at add time only; a removal
        // needs the player id alone.
        s.journal.player(tick, player, 0, false, {});
        if (s.world) s.world->remove_player(player);
        s.join_orders.erase(player);
        s.last_physicalize_tick.erase(player);
        s.last_frames.erase(player);
        s.leaving.erase(player);
        // Its Trafo events stay (trafo-replication design 2.4): they act on
        // the player's piece set, which outlives its slot, and every member
        // was relayed them - a rejoin before the old self's removal must not
        // drop the final resets the others apply.
        s.events.erase(std::remove_if(s.events.begin(), s.events.end(),
                [player](const pending_event& e) {
                    return e.player == player && e.event.type != bmmo::session::event_type::Trafo;
                }), s.events.end());
        s.cadence.force_full();
        if (removed.in_world && callbacks_.on_player_removed) callbacks_.on_player_removed(removed);
        if (rejoin.rejoin && s.world) add_now(s, player, rejoin.rejoin_order, rejoin.rejoin_life, rejoin.rejoin_name);
    }

    void session_runner::remove_player(uint32_t session, uint32_t player) {
        post([this, session, player] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& s = *it->second;
            // Step M1: a player the world does not hold - its add failed, or
            // it rejoined while its old life was leaving and left again
            // before that add ran.  The waiting add is dropped (its join
            // order released by on_player_removed, in_world false); nothing
            // of it is in the world, so nothing is relayed or queued.
            if (!s.players.count(player)) {
                if (const auto l = s.leaving.find(player); l != s.leaving.end() && l->second.rejoin) {
                    member_change dropped;
                    dropped.session = s.id;
                    dropped.player = player;
                    dropped.life = l->second.rejoin_life;
                    dropped.join_order = l->second.rejoin_order;
                    dropped.tick = s.world ? s.world->tick_index() : 0;
                    dropped.in_world = false;
                    l->second.rejoin = false;
                    log("[session " + std::to_string(s.id) + "] player " + std::to_string(player) + " left again before"
                        " its rejoin (life " + std::to_string(dropped.life) + ") was added: the rejoin is dropped");
                    if (callbacks_.on_player_removed) callbacks_.on_player_removed(dropped);
                }
                s.ready.erase(player);
                s.inputs.erase(player);
                return;
            }
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
                // Trafo-replication design 2.2 "Leavers": with the trafo as
                // an input the leaver always gets its three final resets
                // queued below, so its Unphysicalize is queued as it is when
                // other events are (and applied before them).
                const bool queued = s.trafo_on() || std::any_of(s.events.begin(), s.events.end(),
                        [player](const pending_event& e) { return e.player == player; });
                // Step M1 (J-e): every leave with a ball goes through the
                // queue, so the server applies the Unphysicalize at `next`
                // inside the events loop, in the leaver's rank, as every
                // client does, and takes the player out at next + 1.
                if (has_ball) {
                    lifecycle_event death;
                    death.type = bmmo::session::event_type::Unphysicalize;
                    death.tick = gone.applied;
                    pending_event pe{player, gone.origin, gone.applied, std::move(death), clock::now()};
                    auto pos = std::upper_bound(s.events.begin(), s.events.end(), gone.applied,
                        [](uint32_t t, const pending_event& e) { return static_cast<int32_t>(t - e.applied) < 0; });
                    s.events.insert(pos, std::move(pe));
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
                        a.life = life_of(s, player);
                        callbacks_.on_event_accepted(a);
                    }
                }
                if (s.trafo_on()) {
                    // After the Unphysicalize: F counts every queued event of
                    // the player, an explosion accepted at E > next included.
                    // Every live type set is reset at >= its explosion + 1,
                    // the quiet ones at F; all three TRAFO_FINAL (no more
                    // inputs for this set: every world retires it).
                    const uint32_t F = bmmo::session::lifecycle_leaver_removal_tick(s.events, player, next);
                    const float none[3] = {};
                    for (const auto& d: s.trafo.leaver(player, F)) queue_trafo(s, d, none, clock::now(), 0, "leaver");
                }
                if (bmmo::session::leaver_stays(has_ball, queued)) {
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
            s.waits.erase(player);
            s.waits_journalled.erase(player);
            s.bodiless.forget(player);
            publish_state(s);   // step M1: the leaving horizon (J4)
            // A session waiting for readiness may now be complete.
            if (!s.running && !s.failed && s.world && !s.players.empty()
                    && std::includes(s.ready.begin(), s.ready.end(), s.players.begin(), s.players.end())) {
                start_ticking(s);
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
                start_ticking(s);
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
            // Resync step B2: a client that applies the resync state is a
            // session core with the rollback on, which rewinds a late remote
            // Sector (step B1); its raises are bounded by its reach.
            lead.rewinds_sectors = (caps & bmmo::session::CAP_RESYNC_STATE) != 0;
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
            // Step B2SX: the arrival goes with the frames, so the step judges
            // each by its own tick's deadline (input_buffer::take's cutoff).
            buffer->second.submit(first_tick, frames, arrival);
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
                if (const auto found = sessions_.find(session); found != sessions_.end()) a.life = life_of(*found->second, player);
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
            if (type == bmmo::session::event_type::Trafo) {
                // Trafo-replication design 2.2: through the ledger per
                // (player, piece type), not the lifecycle acceptance.
                const auto handed = [&](uint32_t applied, bool superseded, bool rejected) {
                    if (!callbacks_.on_event_accepted) return;
                    accepted_event a;
                    a.session = s.id;
                    a.player = player;
                    a.origin = tick;
                    a.applied = applied;
                    a.type = type;
                    a.superseded = superseded;
                    a.rejected = rejected;
                    a.ball_type = event.ball_type;
                    a.flags = event.flags;
                    for (int k = 0; k < 3; ++k) a.position[k] = event.position[k];
                    a.token = token;
                    a.intake = arrival;
                    a.life = life_of(s, player);
                    callbacks_.on_event_accepted(a);
                };
                if (!s.trafo_on() || event.ball_type >= bmmo::session::kTrafoTypes) {
                    // The network side refuses these first; never applied.
                    ++s.trafo_rejected;
                    log("[session " + std::to_string(s.id) + "] Trafo from " + std::to_string(player) + " stamped "
                        + std::to_string(tick) + " refused: " + (s.trafo_on() ? "bad piece type" : "the session has no trafo inputs"));
                    handed(tick, false, true);
                    return;
                }
                const uint8_t piece = event.ball_type;
                if (bmmo::session::trafo_is_reset(event.flags)) {
                    const auto d = s.trafo.reset(player, piece, tick, next);
                    if (d.superseded) {
                        ++s.trafo_superseded;
                        log("[session " + std::to_string(s.id) + "] Trafo reset of " + bmmo::session::trafo_type_name(piece)
                            + " from " + std::to_string(player) + " stamped " + std::to_string(tick)
                            + " is older than its newest explosion: superseded, never applied");
                        if (s.journal.is_open() && !s.journal.capped())
                            s.journal.note(next, "superseded: p" + std::to_string(player) + " Trafo reset of "
                                           + bmmo::session::trafo_type_name(piece) + " stamped " + std::to_string(tick));
                        handed(tick, true, false);
                        return;
                    }
                    if (d.applied != tick) ++s.restamped_events;
                    queue_trafo(s, d, event.position, arrival, token, nullptr);
                    handed(d.applied, false, false);
                    return;
                }
                for (const auto& d: s.trafo.explode(player, piece, tick, next)) {
                    if (d.flags & bmmo::session::TRAFO_SERVER) {
                        const float none[3] = {};
                        queue_trafo(s, d, none, arrival, 0, "re-explosion of a live type set");
                        continue;
                    }
                    if (d.applied != tick) ++s.restamped_events;
                    queue_trafo(s, d, event.position, arrival, token, nullptr);
                    handed(d.applied, false, false);
                }
                return;
            }
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
                a.life = life_of(s, player);
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
            // of q's clock it lands.  A raise of q must leave that positive,
            // or, for a q that rewinds a late remote Sector (resync step B2),
            // within its rewind reach (lead_sector_bound).  An
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

    void session_runner::queue_trafo(session_state& s, const bmmo::session::trafo_decision& d, const float position[3],
                                     clock::time_point arrival, uint64_t token, const char* why) {
        lifecycle_event event;
        event.type = bmmo::session::event_type::Trafo;
        event.ball_type = d.type;
        event.flags = d.flags;
        if (!bmmo::session::trafo_is_reset(d.flags))
            for (int k = 0; k < 3; ++k) event.position[k] = position[k];
        event.tick = d.applied;
        pending_event pe{d.player, d.origin, d.applied, std::move(event), arrival};
        auto pos = std::upper_bound(s.events.begin(), s.events.end(), d.applied,
            [](uint32_t t, const pending_event& e) { return static_cast<int32_t>(t - e.applied) < 0; });
        s.events.insert(pos, std::move(pe));
        if (token != 0) return;
        // The server's own: relayed to every member (the owner included,
        // for which it is a remote input), an EVENT record when applied.
        ++s.trafo_server_resets;
        const uint32_t next = s.world ? s.world->tick_index() : 0;
        const std::string text = std::string("trafo: server ") + bmmo::session::trafo_type_name(d.type) + " reset of p"
            + std::to_string(d.player) + " for tick " + std::to_string(d.applied) + " (" + (why ? why : "?") + ")"
            + ((d.flags & bmmo::session::TRAFO_FINAL) ? ", final" : "");
        log("[session " + std::to_string(s.id) + "] " + text + " (next tick " + std::to_string(next) + ")");
        if (s.journal.is_open() && !s.journal.capped()) s.journal.note(next, text);
        if (callbacks_.on_event_accepted) {
            accepted_event a;
            a.session = s.id;
            a.player = d.player;
            a.origin = d.origin;
            a.applied = d.applied;
            a.type = bmmo::session::event_type::Trafo;
            a.ball_type = d.type;
            a.flags = d.flags;
            a.intake = arrival;
            a.life = life_of(s, d.player);
            callbacks_.on_event_accepted(a);
        }
    }

    void session_runner::request_full_snapshot(uint32_t session) {
        post([this, session] {
            auto it = sessions_.find(session);
            if (it != sessions_.end()) it->second->cadence.force_full();
        });
    }

    void session_runner::request_resync_state(uint32_t session, uint32_t player, uint32_t base) {
        post([this, session, player, base] {
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) return;
            auto& s = *it->second;
            const uint32_t next = s.world->tick_index();
            // base - 1 stepped already (next = tick_index(), the tick the
            // runner steps next, is past it: next >= base): nothing is sent
            // for a base the runner has passed - the client's check
            // tick == base-1 never refuses a valid answer - and the server
            // re-answers.  (next == base is a miss too: emit_resync_states
            // runs after the step of base - 1, which has already happened.)
            if (static_cast<int32_t>(next - base) >= 0) {
                s.resync_requests.erase(player);
                log("[session " + std::to_string(s.id) + "] resync base " + std::to_string(base) + " for player "
                    + std::to_string(player) + " was stepped before the request reached the runner (next tick "
                    + std::to_string(next) + ")");
                if (s.journal.is_open() && !s.journal.capped())
                    s.journal.note(next, "resync: base " + std::to_string(base) + " for player " + std::to_string(player)
                                         + " missed (stepped before the request)");
                if (callbacks_.on_resync_base_missed) callbacks_.on_resync_base_missed(s.id, player, base);
                return;
            }
            s.resync_requests[player] = base;
        });
    }

    void session_runner::emit_resync_states(session_state& s, uint32_t tick, bool recording) {
        for (auto r = s.resync_requests.begin(); r != s.resync_requests.end();) {
            if (r->second - 1 != tick) {
                ++r;
                continue;
            }
            const auto started = clock::now();
            resync_state_out out;
            out.session = s.id;
            out.player = r->first;
            out.tick = tick;
            out.base = r->second;
            s.world->capture_resync_state(out.player, out.state);
            out.emit_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - started).count());
            if (recording)
                s.journal.note(tick, "resync state for player " + std::to_string(out.player) + ": tick "
                                     + std::to_string(tick) + " base " + std::to_string(out.base) + ", "
                                     + std::to_string(out.state.rows.size()) + " rows, "
                                     + std::to_string(out.state.scripts.size()) + " script bytes, "
                                     + std::to_string(out.state.savestate.size()) + " savestate bytes (packed)"
                                     + (out.state.error.empty() ? std::string() : " (" + out.state.error + ")"));
            r = s.resync_requests.erase(r);
            if (callbacks_.on_resync_state) callbacks_.on_resync_state(out);
        }
    }

    void session_runner::journal_note(uint32_t session, const std::string& text) {
        post([this, session, text] {
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) return;
            it->second->journal.note(it->second->world->tick_index(), text);
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
                + (s.trafo_on() ? " trafo: server_resets=" + std::to_string(s.trafo_server_resets) + " superseded="
                                  + std::to_string(s.trafo_superseded) + " refused=" + std::to_string(s.trafo_rejected)
                                : std::string())
                + " bodiless=" + std::to_string(s.bodiless.runs()) + "/" + std::to_string(s.bodiless.open_runs())
                + "/" + std::to_string(s.bodiless_short);
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

    // S4 (late-player design 3.2, plan step 20).  The lifecycle barrier used
    // to hold the whole session up to a second here, waiting for the
    // Physicalize of a frame that claimed a ball; a late event is re-stamped
    // at acceptance instead (design 3.1), so every receiver applies it at the
    // same later tick and the tick is stepped without the ball - the world
    // never reads INPUT_FLAG_PHYSICALIZED.  Only the record is left: one
    // line per run of such ticks longer than input_delay + 1 when it ends,
    // one while it is still open after a second (a dropped birth never
    // ends), capped per session, and a journal note for each, so a run is
    // visible beside the substitutions and the re-stamped event that explain
    // it; the short runs every birth makes are only counted.
    void session_runner::note_bodiless(session_state& s, uint32_t tick,
                                       const std::vector<bmmo::session::applied_input>& applied, bool recording) {
        using change = bmmo::session::bodiless_tracker::change;
        static constexpr uint32_t kBodilessLogCap = 50;
        for (const auto& a: applied) {
            const bool claimed = (a.frame.flags & bmmo::session::INPUT_FLAG_PHYSICALIZED) != 0;
            // Cheap first: a frame that claims nothing and no open run is
            // the common case, and asks the world nothing.
            if (!claimed && !s.bodiless.open(a.player)) continue;
            bmmo::session::bodiless_tracker::run run;
            const auto what = s.bodiless.step(a.player, tick, claimed, s.world->player_physicalized(a.player), run);
            if (what == change::none) continue;
            // A run no longer than input_delay + 1 comes with every birth
            // (its stamp trails the first flagged frame by up to that): only
            // counted (describe), so the cap is spent on the runs a late or
            // lost Physicalize makes, the ones the measurement looks for.
            if (what == change::ended && run.ticks() <= s.input_delay + 1) {
                ++s.bodiless_short;
                continue;
            }
            const std::string range = std::to_string(run.first) + ".." + std::to_string(run.last);
            const std::string text = what == change::ended
                ? "S4 bodiless player=" + std::to_string(a.player) + " ticks=" + range + " n=" + std::to_string(run.ticks())
                : "S4 bodiless player=" + std::to_string(a.player) + " ticks=" + range + " n=" + std::to_string(run.ticks())
                  + " open";
            if (s.bodiless_logs < kBodilessLogCap) {
                ++s.bodiless_logs;
                log("[session " + std::to_string(s.id) + "] " + text
                    + (s.bodiless_logs == kBodilessLogCap ? " (S4 lines capped here)" : ""));
            }
            if (recording) s.journal.note(tick, text);
        }
    }

    // One simulated tick of a running session: inputs, due events, physics,
    // snapshot.
    void session_runner::step(session_state& s) {
        const uint32_t tick = s.world->tick_index();
        // Everything the journal costs hangs off this: a capped or absent box
        // does not even capture a hash.
        const bool recording = s.journal.is_open() && !s.journal.capped();
        // Step G2 (world-agreement-gaps-design.md §2.2, §2.4): under
        // WORLD_PROXIMITY_UNION the union is placed first, from the balls
        // with a body at the end of the previous step - before the leavers
        // below and the tick's events, the point every world can place at.
        // Nothing between two steps may change that set (the guard).
        if (s.union_on()) {
            std::vector<uint32_t> now;
            for (const auto& c: s.world->probe_candidates()) now.push_back(c.player);
            if (s.have_probe_set && now != s.probe_set) {
                ++s.probe_set_drift;
                std::string was, is;
                for (uint32_t id: s.probe_set) was += (was.empty() ? "" : ",") + std::to_string(id);
                for (uint32_t id: now) is += (is.empty() ? "" : ",") + std::to_string(id);
                const std::string text = "proximity union: the candidate set changed between two steps ({" + was
                    + "} after the last, {" + is + "} at " + std::to_string(tick) + ")";
                if (s.probe_set_drift <= 20) log("[session " + std::to_string(s.id) + "] " + text);
                if (recording) s.journal.note(tick, text);
            }
            s.world->place_probes();
        }
        // Leavers whose queued events are all applied go before this step,
        // which is where the PLAYER record at this tick puts them in a replay.
        for (auto l = s.leaving.begin(); l != s.leaving.end();) {
            const uint32_t player = l->first;
            const bool due = static_cast<int32_t>(l->second.remove_at - tick) <= 0;
            ++l;
            if (due) finish_leave(s, player, tick);
        }
        // Step B2SX (the trial's server stall, the user's option (b)): a step
        // that runs more than a tick past its deadline is a catch-up (a
        // stalled runner stepping overdue ticks); it takes a frame only if
        // the frame arrived by this tick's deadline, and one that arrived
        // after it is late, as it would have been without the stall.  Any
        // other step takes what has arrived, as before: the runner wakes a
        // millisecond or two past a deadline, and a frame that came in that
        // gap is applied at its tick exactly as it always was.
        auto cutoff = clock::time_point::max();
        if (s.scheduler.started() && tick >= s.scheduler.first_tick()) {
            const auto deadline = s.scheduler.deadline(tick);
            if (clock::now() - deadline > bmmo::session::kTickDuration) cutoff = deadline;
        }
        std::vector<bmmo::session::applied_input> applied;
        applied.reserve(s.inputs.size());
        size_t member_index = 0;
        for (auto& [player, buffer]: s.inputs) {
            // Step R12 (protocol 13): this member's wait at this step into
            // its meter; the relay header carries the meter's value.
            auto& wait_meter = s.waits[player];
            wait_meter.sample(tick, member_index < s.step_waits.size() ? s.step_waits[member_index] : 0.0);
            ++member_index;
            const uint8_t wait = wait_meter.value();
            if (recording) {
                const auto journalled = s.waits_journalled.find(player);
                if (journalled == s.waits_journalled.end() || journalled->second != wait) {
                    s.waits_journalled[player] = wait;
                    s.journal.wait(tick, player, wait);
                }
            }
            bool fresh = false;
            uint32_t source = bmmo::session::INPUT_SOURCE_DEFAULT;
            // Before take() moves the cursor past it.
            const bool settled = buffer.settled(tick);
            const auto& frame = buffer.take(tick, fresh, source, cutoff);
            if (fresh) s.acked[player] = tick;
            // The streak waiver (design 2.6) starts at a substitution of a
            // frame the step had to do without.  A tick below a re-anchored
            // cursor was never going to get a frame, so its substitute is not
            // lateness and waives nothing.
            if (config_.patience && !fresh && !settled) s.patience[player].waiver.substituted();
            s.world->set_input(player, frame);
            s.last_frames[player] = frame;
            applied.push_back(bmmo::session::applied_input{player, frame, !fresh, source, wait});
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
        // After the events: a Physicalize due this tick has made its ball.
        note_bodiless(s, tick, applied, recording);
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
        if (s.union_on()) {   // step G2: the candidates of the next step, for its guard
            s.probe_set.clear();
            for (const auto& c: s.world->probe_candidates()) s.probe_set.push_back(c.player);
            s.have_probe_set = true;
        }
        // Step BALA: the balls that entered the airship zone in this step
        // (ghosts from here, out of the sync) - in the journal.
        for (const auto& [player, zone_tick]: s.world->take_airship_entries())
            if (recording)
                s.journal.note(tick, "airship: p" + std::to_string(player) + " in the airship zone at tick "
                                     + std::to_string(zone_tick) + ", a ghost from here");
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
        // Trafo-replication design 2.2, the deadline (decision 9.6): after
        // each tick, every live type set whose 20 s ends within the lead of
        // the next tick N gets the server's reset at max(N, explosion +
        // kTrafoLife), in every world - Game Over and an owner that never
        // resets included.
        if (s.trafo_on()) {
            const float none[3] = {};
            for (const auto& d: s.trafo.deadline(s.world->tick_index(), s.sector_lead))
                queue_trafo(s, d, none, clock::now(), 0, "deadline");
        }
        const bool changed = s.world->body_set_changed();
        const int decision = s.cadence.decide(tick, changed);
        if (decision != 0) emit_snapshot(s, decision == 2);
        // Step A3: the resync states due after this step (base - 1 == tick),
        // after the snapshot so its numbering of new bodies comes first.
        if (!s.resync_requests.empty()) emit_resync_states(s, tick, recording);
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
                          + (lead.rewinds_sectors ? "+sector-rewind" : "")
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
                // A lead reset's held range (lead_adjust), or ticks a resync's
                // or a late joiner's base skipped (plan step 20): nothing comes
                // for either, so the session does not wait for their
                // deadlines while another member's frame paces it.  Without
                // either this is false and the test is the old has-only rule.
                m.held = buffer.held(tick) || buffer.reanchored(tick);
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
            // Step B2SX (the trial's server stall, the user's option (b)):
            // before each step the queue is drained, so the frames the
            // network thread received while a step (or a stall) held this
            // thread are in their buffers when their tick is taken, and the
            // take judges each by its arrival against its own tick's deadline
            // (input_buffer::take's cutoff).  A drained command may end a
            // session, so the sessions are walked by id and looked up again.
            const auto drain = [this, &lk]() {
                lk.lock();
                while (!commands_.empty()) {
                    auto command = std::move(commands_.front());
                    commands_.pop_front();
                    lk.unlock();
                    command();
                    lk.lock();
                }
                lk.unlock();
            };
            std::vector<uint32_t> ids;
            ids.reserve(sessions_.size());
            for (const auto& entry: sessions_) ids.push_back(entry.first);
            for (const uint32_t id: ids) {
                const auto found = sessions_.find(id);
                if (found == sessions_.end() || !found->second) continue;
                session_state* sp = found->second.get();
                if (!sp->running || sp->failed || !sp->world) continue;
                uint32_t simulated = 0;
                // When the tick waits for a player's patience (design 2.6),
                // how long until it ends: a blacked-out player posts nothing,
                // so nothing else would wake the runner there.
                bool patience_wait = false;
                std::chrono::milliseconds patience_sleep{0};
                bool gone = false;
                while (simulated < config_.max_catch_up_ticks) {
                    drain();
                    if (const auto again = sessions_.find(id); again == sessions_.end() || again->second.get() != sp) {
                        gone = true;
                        break;
                    }
                    auto& s = *sp;
                    if (!s.running || s.failed || !s.world) break;
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
                    // Step R12: each member's wait at this step (lateness.hpp
                    // server_wait_ticks), from the same view the test just
                    // passed: how much longer it would have held this tick
                    // for that member's missing frame.
                    {
                        const double to_deadline = bmmo::session::lateness_slack_ticks(s.scheduler.deadline(tick), at);
                        s.step_waits.assign(members.size(), 0.0);
                        for (size_t i = 0; i < members.size(); ++i) {
                            bool paced = false;
                            for (size_t k = 0; k < members.size(); ++k)
                                if (k != i && members[k].has) { paced = true; break; }
                            s.step_waits[i] = bmmo::session::server_wait_ticks(to_deadline, members[i], config_.patience, paced);
                        }
                    }
                    {
                        const auto started = clock::now();
                        // Step B2SW: the harness's stall (BMMO_SESSION_STALL),
                        // inside the step's timing as a slow step would be.
                        if (!debug_stalls_.empty())
                            if (const auto stall = debug_stalls_.find(tick); stall != debug_stalls_.end()) {
                                const uint32_t ms = stall->second;
                                debug_stalls_.erase(stall);
                                log("[session " + std::to_string(s.id) + "] debug stall: the step of tick " + std::to_string(tick)
                                    + " held " + std::to_string(ms) + " ms (BMMO_SESSION_STALL)");
                                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                            }
                        step(s);
                        s.step_waits.clear();
                        s.step_us.push_back(static_cast<uint32_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - started).count()));
                        // Every 10 s of ticks: the step's cost, the budget
                        // being one tick (15.15 ms) per tick.
                        if (s.step_us.size() >= 660) {
                            std::vector<uint32_t> v;
                            v.swap(s.step_us);
                            std::sort(v.begin(), v.end());
                            log("[session " + std::to_string(s.id) + "] step time over ticks " + std::to_string(tick + 1 - v.size())
                                + ".." + std::to_string(tick) + ": p50 " + std::to_string(v[v.size() / 2]) + " us, p99 "
                                + std::to_string(v[v.size() * 99 / 100]) + " us, max " + std::to_string(v.back()) + " us");
                        }
                    }
                    if (s.running) {
                        observe_lateness(s, tick);
                        if (config_.patience) report_patience(s, tick);
                    }
                    ++simulated;
                    if (!s.running) break;
                }
                if (gone) continue;
                auto& s = *sp;
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
