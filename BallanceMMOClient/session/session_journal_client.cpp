// The retail mod's session black box; see session_journal_client.hpp for what
// it is and why the state lives here instead of in the mod class.

#include "session_journal_client.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <format>
#include <utility>

namespace bmmo::session {
    client_journal& client_journal::instance() {
        // A file-scope object, like the input freshness counters in
        // physics_session_client.cpp: the mod class does not survive having its
        // layout moved, and this one outlives a single session anyway.
        static client_journal journal;
        return journal;
    }

    std::filesystem::path client_journal::directory() {
        // The game runs from <game>/Bin, and the mods' data belongs next to the
        // loader, not next to the executable.
        std::error_code ec;
        const auto bin = std::filesystem::current_path(ec);
        if (ec) return std::filesystem::path("..") / "ModLoader" / "BMMOJournals";
        return bin.parent_path() / "ModLoader" / "BMMOJournals";
    }

    std::string client_journal::file_name(uint32_t session, int32_t level, uint32_t own_player) {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
        gmtime_s(&utc, &seconds);
        char stamp[32] = {};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &utc);
        return std::format("session_{}_level{}_{}_p{}.bmjr", session, level, stamp, own_player);
    }

    void client_journal::prune(const std::filesystem::path& directory, size_t keep) {
        // Nothing here may throw: this runs at the anchor frame, in the middle
        // of a session start.
        std::error_code ec;
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
        std::filesystem::directory_iterator it(directory, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
            const auto name = it->path().filename().string();
            if (name.rfind("session_", 0) != 0 || it->path().extension() != ".bmjr") continue;
            files.emplace_back(it->last_write_time(ec), it->path());
            ec.clear();
        }
        if (files.size() <= keep) return;
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = keep; i < files.size(); ++i) std::filesystem::remove(files[i].second, ec);
    }

    bool client_journal::begin(const journal_header& header, const std::filesystem::path& directory,
                               uint64_t max_bytes, const std::vector<journal_player>& players,
                               std::string& error) {
        // A journal that is still open here was never ended (the session was
        // replaced without going through physics_session_end_local); close it
        // with a reason rather than truncating it silently.
        if (writer_.is_open()) end(last_checkpoint_tick_, "replaced by a new session");
        records_ = 0;
        have_checkpoint_ = false;
        checkpoint_pending_ = false;
        known_players_.clear();
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            error = "cannot create " + directory.string() + ": " + ec.message();
            return false;
        }
        // Room for the one about to be opened.
        prune(directory, kKeepFiles > 0 ? kKeepFiles - 1 : 0);
        journal_header full = header;
        full.kind = journal_kind::client;
        full.checkpoint_ticks = kCheckpointTicks;
        full.utc_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (!writer_.open(directory / file_name(full.session, full.level, full.own_player), full, max_bytes, error))
            return false;
        started_ = std::chrono::steady_clock::now();
        own_player_ = full.own_player;
        last_checkpoint_tick_ = full.first_tick;
        records_ = 1;   // the header record
        for (const auto& p: players) {
            writer_.player(p.tick, p.id, p.join_order, p.added, p.name);
            if (p.added) known_players_.insert(p.id);
            ++records_;
        }
        // Every id that ever appears in an INPUT or EVENT record needs a PLAYER
        // record before it, ours included: a roster that does not list us (the
        // server builds it, we only mirror it) would leave a replay without our
        // own ball.
        if (own_player_ != 0 && known_players_.insert(own_player_).second) {
            writer_.player(full.first_tick, own_player_, full.own_join_order, true, std::string{});
            ++records_;
        }
        return true;
    }

    void client_journal::late_player(uint32_t tick, uint32_t id, uint8_t join_order, const std::string& name) {
        if (!recording() || id == 0) return;
        if (!known_players_.insert(id).second) return;
        writer_.player(tick, id, join_order, true, name);
        // The order is the mirror's fallback, not the server's: a replay of
        // this file may put that player in another slot (and give its spawn
        // impulse another direction) than the session did.
        writer_.note(tick, std::format("late join: player {} ({}) first seen here; join order {} is a guess, "
                                       "the server tells only the joiner", id, name, join_order));
        records_ += 2;
    }

    void client_journal::leave_player(uint32_t tick, uint32_t id) {
        if (!recording()) return;
        // Only a player this file already announced can leave it; erasing the
        // id also lets a rejoin write a fresh PLAYER record.
        if (!known_players_.erase(id)) return;
        // The join order is ignored on a removal, exactly as the server's own
        // journal writes it.
        writer_.player(tick, id, 0, false, std::string{});
        ++records_;
    }

    void client_journal::own_input(uint32_t tick, const input_frame& frame) {
        if (!recording()) return;
        writer_.input(tick, own_player_, frame, JOURNAL_INPUT_FRESH);
        ++records_;
    }

    void client_journal::relayed_input(uint32_t tick, uint32_t id, const input_frame& frame) {
        if (!recording()) return;
        writer_.input(tick, id, frame, JOURNAL_INPUT_RELAYED);
        ++records_;
    }

    void client_journal::event(const journal_event& e) {
        if (!recording()) return;
        writer_.event(e);
        ++records_;
    }

    void client_journal::tick(uint32_t tick, const bmmo::physics::world_hash& hash) {
        if (!recording()) return;
        writer_.tick(tick, hash, elapsed_ms());
        ++records_;
    }

    void client_journal::received_snapshot(const session_snapshot_msg& snapshot) {
        if (!recording()) return;
        // Exactly what came off the wire, before any staleness or resync filter
        // decides what to do with it: these rows are the server's truth.
        const auto flags = static_cast<uint8_t>(JOURNAL_CHECKPOINT_RECEIVED
                | (snapshot.full ? JOURNAL_CHECKPOINT_FULL : 0));
        writer_.checkpoint(snapshot.tick, flags, snapshot.bodies);
        ++records_;
    }

    void client_journal::local_checkpoint(uint32_t tick, const std::vector<bmmo_physics_body_state>& bodies) {
        if (!recording()) return;
        std::vector<body_state> rows;
        rows.reserve(bodies.size());
        for (const auto& body: bodies) {
            // Only what can move: the level's fixed geometry never differs from
            // the server's, and a checkpoint is there to be compared with one.
            if (!body.movable) continue;
            body_state row;
            // No dictionary here - the client knows bodies by name, and a ball
            // is what the retail scripts call "Ball_<type>[_Peer_<id>]".
            row.kind = std::strncmp(body.name, "Ball_", 5) == 0 ? body_kind::Ball : body_kind::Mechanism;
            row.owner = 0;
            row.name.assign(body.name, ::strnlen(body.name, sizeof(body.name)));
            for (int k = 0; k < 3; ++k) {
                row.position[k] = body.position[k];
                row.linear[k] = body.linear[k];
                row.angular[k] = body.angular[k];
            }
            for (int k = 0; k < 4; ++k) row.rotation[k] = body.rotation[k];
            row.flags = static_cast<uint8_t>((body.simulated ? BODY_FLAG_SIMULATED : 0)
                    | (body.collision_enabled ? BODY_FLAG_COLLISION_ENABLED : 0));
            rows.push_back(std::move(row));
        }
        writer_.checkpoint(tick, JOURNAL_CHECKPOINT_LOCAL, rows);
        ++records_;
        last_checkpoint_tick_ = tick;
        have_checkpoint_ = true;
        checkpoint_pending_ = false;
    }

    void client_journal::note(uint32_t tick, const std::string& text) {
        if (!recording()) return;
        writer_.note(tick, text);
        ++records_;
    }

    void client_journal::mark(uint32_t tick, const std::string& text,
                              const std::vector<bmmo_physics_body_state>& bodies) {
        if (!recording()) return;
        writer_.note(tick, "mark: " + text);
        ++records_;
        local_checkpoint(tick, bodies);
    }

    void client_journal::correction(const journal_correction& c) {
        if (!recording()) return;
        writer_.correction(c);
        ++records_;
    }

    void client_journal::end(uint32_t tick, const std::string& reason) {
        if (!writer_.is_open()) return;
        if (recording()) {
            writer_.note(tick, "end: " + reason);
            ++records_;
        }
        writer_.close();
    }

    bool client_journal::checkpoint_due(uint32_t tick) const {
        if (!recording()) return false;
        if (have_checkpoint_ && last_checkpoint_tick_ == tick) return false;
        if (tick % kCheckpointTicks == 0) return true;
        if (!checkpoint_pending_) return false;
        // A tick before the last one means the numbering restarted (a resync):
        // that world is new, so it gets its checkpoint.
        return !have_checkpoint_ || tick < last_checkpoint_tick_
            || tick - last_checkpoint_tick_ >= kCheckpointMinGap;
    }

    std::string client_journal::status() const {
        if (!writer_.is_open())
            return std::format("journal={} recording=0 (no session journal is open)", enabled_ ? "on" : "off");
        return std::format("journal={} recording={} capped={} records={} bytes={} path={}",
                           enabled_ ? "on" : "off", recording() ? 1 : 0, writer_.capped() ? 1 : 0,
                           records_, writer_.bytes(), writer_.path().string());
    }

    uint32_t client_journal::elapsed_ms() const {
        const auto elapsed = std::chrono::steady_clock::now() - started_;
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    }
}
