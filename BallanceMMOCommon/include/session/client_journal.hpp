#pragma once

// The session-client core's journal (docs/session-client-core-design.md 1.2):
// the black box of design 9.15, written by the shared core for whichever host
// runs it.  Everything the client fed its own world, everything the server
// sent it and every correction it made, in the shared format
// (session/journal.hpp) so the SimTool and journal_trace.py can line it up
// with the server's own journal of the same room.
//
// It is the headless client's policy, moved verbatim at step E2 (the helpers
// open_journal ... journal_local_checkpoint of session_client.cpp): the file
// is the platform's journal_path, a checkpoint is due on `tick % cadence`
// (and before the first one, after the tick went back, and when one is
// pending, at most every kJournalCheckpointGap ticks), LOCAL checkpoints carry
// the ball owners, and the header's first tick is the tick base.
//
// Named bmmo::session::core::client_journal: the retail mod already has a
// bmmo::session::client_journal (its legacy singleton,
// BallanceMMOClient/session/session_journal_client.hpp), which stays until R7
// and is compiled into the same DLL as this one.
//
// Single threaded like the rest of the core: the host feeds the network in
// between frames, so there is no lock anywhere near the writer.  CK-free.

#include "journal.hpp"
#include "../entity/session.hpp"
#include "../physics/world_hash.hpp"

#include <chrono>
#include <cstdint>
#include <set>
#include <string>

namespace bmmo {
    struct session_event_msg;   // message/session_event_msg.hpp (not self-contained: the core includes it)
}

namespace bmmo::session {

    class session_client_core;
    struct rollback_correction;   // session/rollback.hpp

    namespace core {

        // A LOCAL checkpoint every 10 s (unless the host configures another
        // cadence) and one after a rollback or a hard correction, but never
        // more often than every half second - a burst of corrections must
        // not fill the file with bodies instead of inputs.  The file stops at
        // 256 MB.
        constexpr uint32_t kJournalCheckpointTicks = 660;
        constexpr uint32_t kJournalCheckpointGap = 33;
        constexpr uint64_t kJournalMaxBytes = 256ull << 20;

        class client_journal {
        public:
            // The core owns one (session_client_core::journal()); it reads the
            // core's state, configuration, platform and current tick.
            explicit client_journal(session_client_core& core) : core_(core) {}
            client_journal(const client_journal&) = delete;
            client_journal& operator=(const client_journal&) = delete;

            // Open and below the size cap: records are written.
            bool journalling() const { return journal_.is_open() && !journal_.capped(); }

            // ms since the header was written: a TICK record's ms
            // (clock_site::journal_ms).
            uint32_t ms();

            // Opened at the anchor, where the hash the server has to agree
            // with is known and the tick base is not: first_tick is 0 until
            // the assignment arrives, which lands in a NOTE of its own.  A
            // journal already open is closed first ("a new session started").
            // `build_id` is the binary's (the header's build_id); `own_name`
            // names the own player in the start note and its PLAYER record,
            // every other player is named by its id.
            void open(const bmmo::physics::world_hash& hash, const std::string& build_id, const std::string& own_name);
            void close(const std::string& reason);
            void note(const std::string& text);

            // The roster only ever comes from SessionStart, which the server
            // sends to the joining client alone: a player who joins later is
            // never named to us, yet its relayed frames and its events are
            // journalled all the same.  Without a PLAYER record a replay drops
            // every one of them and that player's ball is silently missing,
            // so the first record that names an unknown id announces it here.
            // The join order decides the spawn impulse direction, so a guessed
            // one is worth a warning rather than a silent wrong answer.
            void announce_player(uint32_t player, uint32_t tick);
            // The other end of announce_player: PlayerLeft is the only
            // unambiguous "that member is gone" signal a remaining client gets
            // (the leaver's relayed Unphysicalize is the same message a living
            // player sends when its ball dies), and the server drops it from
            // the session world at that moment, so a replay of this file has
            // to drop it too.  Forgetting the id lets a rejoin announce itself
            // again.
            void leave_player(uint32_t player);
            // A lifecycle event as the wire carries it; `player` is who it
            // belongs to (the server fills that in on the way out, so an own
            // event has to name itself here).
            void lifecycle_event(uint32_t player, const bmmo::session_event_msg& e);
            // The rollback engine's own verdict about a snapshot (kinds 0, 1,
            // 5, 6, 7): no log parsing needed to know what the client did.
            void rollback_correction(const bmmo::session::rollback_correction& c);
            // The blending corrector's decisions (hard = 2, blend = 3) and the
            // full-snapshot resync (4), which the rollback engine never sees.
            void body_correction(uint8_t kind, const std::string& entity, uint32_t tick, double error,
                                 const double server_position[3]);

            bool checkpoint_due(uint32_t tick) const;
            // This client's own world, from the platform's body list: the
            // balls (every entity named Ball_*) and the mechanisms it is
            // running.  `own_ball` is the own ball's entity name (empty: none).
            void local_checkpoint(uint32_t tick, const std::string& own_ball);

            // The records the flow writes itself (INPUT, TICK, RECEIVED
            // checkpoints) go straight to the writer while that flow is still
            // the host's.
            journal_writer& writer() { return journal_; }

        private:
            uint32_t cadence() const;

            session_client_core& core_;
            // `journal_start_` is the instant the header was written, the
            // origin every TICK record's ms is measured from; the checkpoint
            // bookkeeping is the LOCAL cadence and its rate limit.
            journal_writer journal_;
            std::chrono::steady_clock::time_point journal_start_{};
            uint32_t journal_last_checkpoint_ = 0;
            bool journal_have_checkpoint_ = false;
            bool journal_checkpoint_pending_ = false;
            std::set<uint32_t> journal_announced_;   // ids that already have a PLAYER record
        };

    }

}
