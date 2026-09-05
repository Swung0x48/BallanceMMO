#include "session_runner.hpp"

#include "CKAll.h"

#include <physics/physics_state.hpp>

#include <algorithm>
#include <chrono>
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
        // it back at; `e.tick` is the tick the client stamped it for and can be
        // older when the event arrived late.
        bmmo::session::journal_event to_journal_event(uint32_t player, const lifecycle_event& e, uint32_t applied) {
            bmmo::session::journal_event out;
            out.tick = applied;
            out.event_tick = e.tick;
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
                                        uint32_t input_delay, float spawn_impulse, const std::string& note,
                                        std::vector<std::string> names) {
        post([this, session, level, players, input_delay, spawn_impulse, note, names = std::move(names)] {
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
            const uint32_t first_tick = s->world->tick_index();
            open_journal(*s, spawn_impulse, s->world->anchor_hash(), s->world->anchor_surfaces(), first_tick);
            if (!note.empty()) s->journal.note(first_tick, "start: " + note);
            for (size_t i = 0; i < players.size(); ++i) {
                const auto [player, join_order] = players[i];
                std::string add_error;
                if (!s->world->add_player(player, join_order, add_error)) {
                    log("[session " + std::to_string(session) + "] add_player failed: " + add_error);
                    continue;
                }
                s->players.insert(player);
                s->join_orders[player] = join_order;
                s->inputs[player].reset(0);
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
            if (callbacks_.on_world_ready) callbacks_.on_world_ready(info);
        });
    }

    void session_runner::destroy_session(uint32_t session, const std::string& reason) {
        post([this, session, reason] {
            if (auto it = sessions_.find(session); it != sessions_.end()) {
                auto& s = *it->second;
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

    void session_runner::remove_player(uint32_t session, uint32_t player) {
        post([this, session, player] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& s = *it->second;
            // Join order is the world's business at add time only; a removal
            // needs the player id alone.
            s.journal.player(s.world ? s.world->tick_index() : 0, player, 0, false, {});
            if (s.world) s.world->remove_player(player);
            s.players.erase(player);
            s.join_orders.erase(player);
            s.ready.erase(player);
            s.inputs.erase(player);
            s.acked.erase(player);
            s.lifecycle_missing.erase(player);
            s.events.erase(std::remove_if(s.events.begin(), s.events.end(),
                    [player](const pending_event& e) { return e.player == player; }), s.events.end());
            s.cadence.force_full();
            // A session waiting for readiness may now be complete.
            if (!s.running && !s.failed && s.world && !s.players.empty()
                    && std::includes(s.ready.begin(), s.ready.end(), s.players.begin(), s.players.end())) {
                s.scheduler.start(clock::now(), s.world->tick_index(), s.input_delay);
                s.running = true;
                {
                    std::lock_guard lk(view_mutex_);
                    auto& v = views_[session];
                    v.start = clock::now();
                    v.first_tick = s.world->tick_index();
                }
                publish_state(s);
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
            s.inputs[player].reset(first_tick);
            if (s.running) return;
            if (std::includes(s.ready.begin(), s.ready.end(), s.players.begin(), s.players.end())) {
                s.scheduler.start(clock::now(), s.world->tick_index(), s.input_delay);
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
            }
            wake_.notify_all();
        });
    }

    void session_runner::submit_input(uint32_t session, uint32_t player, uint32_t first_tick,
                                      std::vector<bmmo::session::input_frame> frames) {
        post([this, session, player, first_tick, frames = std::move(frames)] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto buffer = it->second->inputs.find(player);
            if (buffer == it->second->inputs.end()) return;
            buffer->second.submit(first_tick, frames);
        });
    }

    void session_runner::submit_event(uint32_t session, uint32_t player, uint32_t tick, lifecycle_event event) {
        post([this, session, player, tick, event = std::move(event)] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& events = it->second->events;
            pending_event pe{player, tick, std::move(event)};
            auto pos = std::upper_bound(events.begin(), events.end(), tick,
                [](uint32_t t, const pending_event& e) { return t < e.tick; });
            events.insert(pos, std::move(pe));
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
                + " pending_events=" + std::to_string(s.events.size());
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
            bool pending = false;
            for (const auto& e: s.events)
                if (e.player == player && e.tick <= tick && e.event.type == bmmo::session::event_type::Physicalize) {
                    pending = true;
                    break;
                }
            // A Physicalize is on its way (or arrived after we gave up): wait
            // for it again.
            if (pending) { s.lifecycle_missing.erase(player); continue; }
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
        std::vector<std::pair<uint32_t, bmmo::session::input_frame>> applied;
        applied.reserve(s.inputs.size());
        for (auto& [player, buffer]: s.inputs) {
            bool fresh = false;
            const auto& frame = buffer.take(tick, fresh);
            if (fresh) s.acked[player] = tick;
            s.world->set_input(player, frame);
            applied.emplace_back(player, frame);
            // What the world consumed, not what arrived on the wire: the wire
            // can lose, duplicate and reorder, the world cannot.
            if (recording)
                s.journal.input(tick, player, frame, fresh ? bmmo::session::JOURNAL_INPUT_FRESH : 0);
        }
        if (callbacks_.on_inputs && applied.size() > 1) callbacks_.on_inputs(s.id, tick, applied);
        // Every event due this tick, in the members' JOIN ORDER - not in the
        // order the network happened to deliver them.  The order two events of
        // the same tick are applied in changes the world (two balls
        // physicalized in one tick are created in that order), so it is part of
        // the determinism contract now, and join order is the one order a
        // client's own journal can reproduce offline: nobody but the server
        // knows who got there first, but every member knows every join order.
        // An unknown player (its removal raced the event) sorts after all known
        // ones; stable_sort keeps arrival order within one rank.
        size_t due_count = 0;
        while (due_count < s.events.size() && s.events[due_count].tick <= tick) ++due_count;
        std::vector<pending_event> due(std::make_move_iterator(s.events.begin()),
                                       std::make_move_iterator(s.events.begin() + due_count));
        s.events.erase(s.events.begin(), s.events.begin() + due_count);
        const auto join_rank = [&s](uint32_t player) {
            const auto it = s.join_orders.find(player);
            return it == s.join_orders.end() ? 256u : static_cast<uint32_t>(it->second);
        };
        std::stable_sort(due.begin(), due.end(), [&join_rank](const pending_event& a, const pending_event& b) {
            return join_rank(a.player) < join_rank(b.player);
        });
        for (pending_event& e: due) {
            e.event.tick = e.tick;
            // After the tick is stamped: the spawn impulse direction is derived
            // from that field, so the record has to carry it - but the world
            // consumes the event now, at `tick`, which is what the record is
            // filed under.  An event that arrived after its own tick was
            // simulated would otherwise be replayed too early.
            if (recording) s.journal.event(to_journal_event(e.player, e.event, tick));
            std::string error;
            if (!s.world->apply_event(e.player, e.event, error)) {
                log("[session " + std::to_string(s.id) + "] event from " + std::to_string(e.player)
                    + " at tick " + std::to_string(e.tick) + " failed: " + error);
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
                while (simulated < config_.max_catch_up_ticks) {
                    const uint32_t tick = s.world->tick_index();
                    bool all_inputs = true;
                    for (const auto& [player, buffer]: s.inputs)
                        if (!buffer.has(tick)) { all_inputs = false; break; }
                    if (!all_inputs && !s.scheduler.due(clock::now())) break;
                    if (waiting_for_lifecycle(s, tick)) break;
                    step(s);
                    ++simulated;
                    if (!s.running) break;
                }
                publish_state(s);
                if (s.running) {
                    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(s.scheduler.until_due(clock::now()));
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
        for (auto& [id, sp]: sessions_)
            if (sp) sp->journal.note(sp->world ? sp->world->tick_index() : 0, "end: server stopped");
        sessions_.clear();
    }
}
