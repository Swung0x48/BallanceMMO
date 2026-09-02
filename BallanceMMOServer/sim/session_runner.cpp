#include "session_runner.hpp"

#include "CKAll.h"

#include <algorithm>
#include <chrono>

namespace bmmo::sim {
    using clock = std::chrono::steady_clock;

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

    void session_runner::create_session(uint32_t session, int level, const std::vector<uint32_t>& players) {
        post([this, session, level, players] {
            auto& s = sessions_[session];
            s = std::make_unique<session_state>();
            s->id = session;
            s->level = level;
            {
                std::lock_guard lk(view_mutex_);
                views_[session] = view{};
            }
            world_options options;
            options.game_root = config_.game_root;
            options.level = level;
            options.seed = config_.seed;
            options.trace = config_.trace;
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
                if (callbacks_.on_world_ready) callbacks_.on_world_ready(info);
                return;
            }
            for (uint32_t player: players) {
                std::string add_error;
                if (!s->world->add_player(player, add_error)) {
                    log("[session " + std::to_string(session) + "] add_player failed: " + add_error);
                    continue;
                }
                s->players.insert(player);
                s->inputs[player].reset(0);
            }
            s->cadence.interval = config_.snapshot_interval;
            s->cadence.full_interval = config_.full_snapshot_interval;
            publish_state(*s);
            info.ok = true;
            info.anchor_hash = s->world->anchor_hash();
            info.anchor_surfaces = s->world->anchor_surfaces();
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

    void session_runner::destroy_session(uint32_t session) {
        post([this, session] {
            sessions_.erase(session);
            std::lock_guard lk(view_mutex_);
            views_.erase(session);
        });
    }

    void session_runner::add_player(uint32_t session, uint32_t player) {
        post([this, session, player] {
            auto it = sessions_.find(session);
            if (it == sessions_.end() || !it->second->world) return;
            auto& s = *it->second;
            std::string error;
            if (!s.world->add_player(player, error)) {
                log("[session " + std::to_string(session) + "] add_player failed: " + error);
                return;
            }
            s.players.insert(player);
            s.inputs[player].reset(s.world->tick_index());
            s.cadence.force_full();
        });
    }

    void session_runner::remove_player(uint32_t session, uint32_t player) {
        post([this, session, player] {
            auto it = sessions_.find(session);
            if (it == sessions_.end()) return;
            auto& s = *it->second;
            if (s.world) s.world->remove_player(player);
            s.players.erase(player);
            s.ready.erase(player);
            s.inputs.erase(player);
            s.acked.erase(player);
            s.events.erase(std::remove_if(s.events.begin(), s.events.end(),
                    [player](const pending_event& e) { return e.player == player; }), s.events.end());
            s.cadence.force_full();
            // A session waiting for readiness may now be complete.
            if (!s.running && !s.failed && s.world && !s.players.empty()
                    && std::includes(s.ready.begin(), s.ready.end(), s.players.begin(), s.players.end())) {
                s.scheduler.start(clock::now(), s.world->tick_index(), config_.input_delay);
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
                s.scheduler.start(clock::now(), s.world->tick_index(), config_.input_delay);
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
                      + "@" + std::to_string(buffer.last_fresh_tick());
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
        bool blocked = false;
        for (const auto& [player, buffer]: s.inputs) {
            const auto* frame = buffer.peek(tick);
            if (!frame || !(frame->flags & bmmo::session::INPUT_FLAG_PHYSICALIZED)) continue;
            if (s.world->player_physicalized(player)) continue;
            bool pending = false;
            for (const auto& e: s.events)
                if (e.player == player && e.tick <= tick && e.event.type == bmmo::session::event_type::Physicalize) {
                    pending = true;
                    break;
                }
            if (!pending) { blocked = true; break; }
        }
        if (!blocked) {
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
            log("[session " + std::to_string(s.id) + "] lifecycle event missing at tick " + std::to_string(tick)
                + "; continuing without it");
            return false;
        }
        return true;
    }

    // One simulated tick of a running session: inputs, due events, physics,
    // snapshot.
    void session_runner::step(session_state& s) {
        const uint32_t tick = s.world->tick_index();
        for (auto& [player, buffer]: s.inputs) {
            bool fresh = false;
            const auto& frame = buffer.take(tick, fresh);
            if (fresh) s.acked[player] = tick;
            s.world->set_input(player, frame);
        }
        while (!s.events.empty() && s.events.front().tick <= tick) {
            pending_event e = std::move(s.events.front());
            s.events.erase(s.events.begin());
            std::string error;
            if (!s.world->apply_event(e.player, e.event, error))
                log("[session " + std::to_string(s.id) + "] event from " + std::to_string(e.player)
                    + " at tick " + std::to_string(e.tick) + " failed: " + error);
        }
        std::string error;
        if (!s.world->tick(error)) {
            s.failed = true;
            s.running = false;
            log("[session " + std::to_string(s.id) + "] tick " + std::to_string(tick) + " failed: " + error);
            if (callbacks_.on_failed) callbacks_.on_failed(s.id, error);
            return;
        }
        s.scheduler.advance();
        const bool changed = s.world->body_set_changed();
        const int decision = s.cadence.decide(tick, changed);
        if (decision != 0) emit_snapshot(s, decision == 2);
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
        sessions_.clear();
    }
}
