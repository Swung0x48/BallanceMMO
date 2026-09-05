#pragma once

// The retail mod's session black box (design 9.15, docs/session-journal-plan.md):
// everything this client fed its own physics world, everything the server sent
// it, and every correction it made, written in the shared journal format
// (BallanceMMOCommon/include/session/journal.hpp) so a bug that only happens in
// a live room can be replayed and diffed offline with the SimTool.
//
// The state is a file-scope object behind client_journal::instance(), never a
// member of BallanceMMOClient or physics_session_state: the mod class's layout
// must not move (the same reason the input freshness counters live in a .cpp,
// see physics_session_client.cpp).  Every method runs on the game thread; the
// network thread only queues, it never touches the journal.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include <message/message_utils.hpp>   // session_snapshot_msg.hpp reads its helpers from here
#include <message/session_snapshot_msg.hpp>
#include <physics/physics_rt_api.h>
#include <physics/world_hash.hpp>
#include <session/journal.hpp>

namespace bmmo::session {
    class client_journal {
    public:
        // One LOCAL checkpoint every 10 s, and one after a rollback or a hard
        // correction - but not more often than every half second, or a burst of
        // corrections would fill the file with bodies instead of inputs.
        static constexpr uint32_t kCheckpointTicks = 660;
        static constexpr uint32_t kCheckpointMinGap = 33;
        static constexpr uint64_t kMaxBytes = 256ull << 20;
        static constexpr size_t kKeepFiles = 10;

        static client_journal& instance();

        // Armed per session (the BML property is read when one begins), so
        // toggling it in the menu takes effect from the next session.
        bool enabled() const { return enabled_; }
        void set_enabled(bool enabled) { enabled_ = enabled; }
        // A file is open and has not hit the size cap.
        bool recording() const { return writer_.is_open() && !writer_.capped(); }
        // A file is open, capped or not: whoever closes one asks this, not
        // recording(), or a capped journal would be left holding its handle.
        bool open() const { return writer_.is_open(); }
        const std::filesystem::path& path() const { return writer_.path(); }

        // Opens <directory>/session_<id>_level<N>_<UTC>_p<own id>.bmjr, writes
        // the header and one PLAYER record per member.  `kind`, `utc_ms` and
        // `checkpoint_ticks` are filled in here; everything else comes from the
        // caller.  Returns false with `error` set when nothing was opened.
        bool begin(const journal_header& header, const std::filesystem::path& directory, uint64_t max_bytes,
                   const std::vector<journal_player>& players, std::string& error);

        void own_input(uint32_t tick, const input_frame& frame);
        void relayed_input(uint32_t tick, uint32_t id, const input_frame& frame);
        void event(const journal_event& e);
        // True when this player has no PLAYER record yet, so the caller only
        // pays for the name lookup when the file needs one.  False when
        // nothing is being recorded.
        bool needs_player(uint32_t id) const { return recording() && !known_players_.count(id); }
        // The PLAYER record of a member this client only learned of from a
        // relayed frame or event: the server announces a late join to the
        // joiner alone, so without this the file would carry inputs, events
        // and a ball for a player a replay never adds.  `join_order` is the
        // mirror's own guess (nothing on the wire carries a late joiner's
        // order); a NOTE says so.
        void late_player(uint32_t tick, uint32_t id, uint8_t join_order, const std::string& name);
        // The removal record of a member that left the room: the server drops
        // that player from its world (body and input state both), so a replay
        // has to as well.  No-op for an id that has no PLAYER record here; the
        // id is forgotten, so a rejoin announces itself again.
        void leave_player(uint32_t tick, uint32_t id);
        // Milliseconds since the header are measured here, from the same steady
        // clock for every tick of the session.
        void tick(uint32_t tick, const bmmo::physics::world_hash& hash);
        void received_snapshot(const session_snapshot_msg& snapshot);
        // The client's own world, from physics_view::list_bodies().  Writes
        // unconditionally: checkpoint_due() is the policy, this is the write.
        void local_checkpoint(uint32_t tick, const std::vector<bmmo_physics_body_state>& bodies);
        void note(uint32_t tick, const std::string& text);
        // NOTE "mark: <text>" plus a LOCAL checkpoint whatever the rate limit
        // says: a mark is a human pointing at this moment.
        void mark(uint32_t tick, const std::string& text, const std::vector<bmmo_physics_body_state>& bodies);
        void correction(const journal_correction& c);
        void end(uint32_t tick, const std::string& reason);

        // A rollback or a hard correction moved the bodies: take a checkpoint at
        // the end of this frame, once the frame's corrections are all applied.
        void request_checkpoint() { checkpoint_pending_ = true; }
        // True when local_checkpoint() should be called for this tick, so the
        // caller only pays for list_bodies() when the box wants the bodies.
        bool checkpoint_due(uint32_t tick) const;

        // One line for "journal status" and the F3-less diagnostics.
        std::string status() const;
        uint32_t elapsed_ms() const;

        // <game>/ModLoader/BMMOJournals (the working directory is <game>/Bin).
        static std::filesystem::path directory();
        static std::string file_name(uint32_t session, int32_t level, uint32_t own_player);
        // Deletes every session_*.bmjr in `directory` beyond the newest `keep`.
        static void prune(const std::filesystem::path& directory, size_t keep);

    private:
        journal_writer writer_;
        bool enabled_ = true;
        uint32_t own_player_ = 0;
        // Every id that already has a PLAYER record in the open file.
        std::unordered_set<uint32_t> known_players_;
        // Records handed to the writer, the header included; approximate once
        // the size cap is hit (the writer stops on its own).
        uint64_t records_ = 0;
        std::chrono::steady_clock::time_point started_{};
        uint32_t last_checkpoint_tick_ = 0;
        bool have_checkpoint_ = false;
        bool checkpoint_pending_ = false;
    };
}
