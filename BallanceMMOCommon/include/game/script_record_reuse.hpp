#pragma once

// Step OPC (FULL close-out; rewind-cost-design "Research (step OPTR)" R.4 row
// C, RC4's dormant ledger generalized to the whole selection): an inactive
// script's record reused.
//
// Every live and every replayed tick captures the runtime state of every
// selected script (game/script_replay.hpp capture): a walk of every block,
// its activation words, its locals and outputs, its force and joint state.
// Most selected scripts are inactive most of the time, and the engine does not
// execute an inactive root with no next-frame change pending, so its bytes are
// those of the record before.  The capture of tick t therefore takes a
// selected root's block from record t - 1 (copied, not walked) when:
//
//  - record t - 1 is valid for it: the rollback engine passes it only when no
//    engine write (RC2 (b)'s barrier: a restore - the scripts, the proximity
//    hold -, a re-creation, a row) and no host write into the scripts
//    (note_script_write below: a re-issued activation, a sector pass, a unit
//    copy, ...) came between the two captures (session/rollback.hpp capture);
//  - its block in record t - 1 is present, and the root's activation words now
//    - the runtime flags (active, the three next-frame changes, launched once)
//    and the active inputs and outputs - equal the recorded ones, with no
//    frame flag set (reusable_root): inactive at t - 1 and at t, nothing
//    pending, nothing ran it in between through its IOs;
//  - no block of its subtree is a force or a joint block (script_replay
//    root_has_physics_block): their bytes are physics_RT's state (the force
//    controller, the joint's image and controller order), which can change
//    while the script is inactive.
//
// The record's bytes are unchanged (the copy is what the walk would write), so
// no record version, no journal, wire or resync change.
//
// Modes (BMMO_SCRIPT_REUSE=off | on | verify, on when unset; read once per
// process): off walks every script; verify walks every script it would reuse
// as well, compares, counts and notes a mismatch (naming the script and the
// tick), and still records the copied bytes - a verify run records exactly
// what mode on would, so its journals and wire judge on.
//
// Engine-free (no CK types): script_replay.hpp reads the activation words and
// copies the bytes; the unit tests drive the decision and the counters here.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bmmo::game {
    enum class script_reuse_mode : uint8_t { off = 0, on = 1, verify = 2 };

    inline const char* script_reuse_mode_name(script_reuse_mode mode) {
        switch (mode) {
            case script_reuse_mode::off: return "off";
            case script_reuse_mode::verify: return "verify";
            default: return "on";
        }
    }

    // "off" | "on" | "verify" (nullptr or anything else: `fallback`).
    inline script_reuse_mode parse_script_reuse_mode(const char* text, script_reuse_mode fallback) {
        if (!text) return fallback;
        if (std::strcmp(text, "off") == 0 || std::strcmp(text, "0") == 0) return script_reuse_mode::off;
        if (std::strcmp(text, "on") == 0 || std::strcmp(text, "1") == 0) return script_reuse_mode::on;
        if (std::strcmp(text, "verify") == 0) return script_reuse_mode::verify;
        return fallback;
    }

    // BMMO_SCRIPT_REUSE, read once per process; on when unset.
    inline script_reuse_mode script_reuse_mode_from_env() {
        static const script_reuse_mode mode =
            parse_script_reuse_mode(std::getenv("BMMO_SCRIPT_REUSE"), script_reuse_mode::on);
        return mode;
    }

    // The host's writes into the selected scripts outside the engine's
    // restore (a sector pass, a re-issued activation, the proximity union's
    // unit copies, a placement copy's stop, the sequencers' anchor values):
    // one process-wide generation, bumped by every such writer.  A record
    // captured under another generation is never a reuse source.  A bump only
    // costs walks, so a writer that may not touch a selected script bumps too.
    inline std::atomic<uint64_t>& script_write_counter() {
        static std::atomic<uint64_t> counter{0};
        return counter;
    }
    inline void note_script_write() { script_write_counter().fetch_add(1, std::memory_order_relaxed); }
    inline uint64_t script_write_generation() { return script_write_counter().load(std::memory_order_relaxed); }

    // A root block's first four record words (script_replay put_activation:
    // id, runtime flags, active-input mask, active-output mask).
    struct script_root_words {
        uint32_t id = 0, flags = 0, in_mask = 0, out_mask = 0;
    };
    inline bool read_script_root_words(const uint8_t* block, size_t size, script_root_words& w) {
        if (!block || size < 16) return false;
        std::memcpy(&w.id, block, 4);
        std::memcpy(&w.flags, block + 4, 4);
        std::memcpy(&w.in_mask, block + 8, 4);
        std::memcpy(&w.out_mask, block + 12, 4);
        return true;
    }
    // The reuse rule: the same root, no frame flag (active or a next-frame
    // change) now, and the recorded words equal to the words now (so none at
    // t - 1 either, launched-once unchanged, no IO switched in between).
    inline bool reusable_root(const script_root_words& recorded, const script_root_words& now, uint32_t frame_flags) {
        return recorded.id == now.id && (now.flags & frame_flags) == 0 && recorded.flags == now.flags
            && recorded.in_mask == now.in_mask && recorded.out_mask == now.out_mask;
    }

    // What the reuse did in this process (diagnostic: nothing decides on it).
    // Captures with a record of t - 1 to reuse from, and without one (the
    // first record, after a barrier or a host write); scripts copied from it,
    // walked though it had their block (not reusable: active, a flag, words
    // changed), walked without one and walked for a force or joint block;
    // verify's re-walks, mismatches and the first notes naming them.
    struct script_reuse_stats {
        uint64_t captures = 0, with_previous = 0, index_failed = 0;
        uint64_t reused = 0, walked = 0, walked_unrecorded = 0;
        // Roots never reused because a force or joint block in their
        // subtree carries physics_RT state (script_replay root_has_physics_block).
        uint64_t walked_physics = 0;
        uint64_t verified = 0, mismatches = 0;
        std::vector<std::string> notes;
        static constexpr size_t kNotes = 16;
        void note(const std::string& what) {
            ++mismatches;
            if (notes.size() < kNotes) notes.push_back(what);
        }
        std::string line(script_reuse_mode mode) const {
            const uint64_t scripts = reused + walked + walked_unrecorded + walked_physics;
            char buf[420];
            std::snprintf(buf, sizeof buf,
                          "script_reuse: mode=%s captures=%llu with_previous=%llu index_failed=%llu scripts=%llu reused=%llu "
                          "(%.1f%%) walked=%llu unrecorded=%llu physics=%llu verified=%llu mismatches=%llu",
                          script_reuse_mode_name(mode), static_cast<unsigned long long>(captures),
                          static_cast<unsigned long long>(with_previous), static_cast<unsigned long long>(index_failed),
                          static_cast<unsigned long long>(scripts), static_cast<unsigned long long>(reused),
                          scripts ? 100.0 * static_cast<double>(reused) / static_cast<double>(scripts) : 0.0,
                          static_cast<unsigned long long>(walked), static_cast<unsigned long long>(walked_unrecorded),
                          static_cast<unsigned long long>(walked_physics),
                          static_cast<unsigned long long>(verified), static_cast<unsigned long long>(mismatches));
            return buf;
        }
    };
    // One process-wide set, on the game thread (captures run there; a tool
    // that captured on several threads would race on the counters only).
    inline script_reuse_stats& script_reuse_totals() {
        static script_reuse_stats stats;
        return stats;
    }

    // The first byte at which two blocks differ (verify's note), or the
    // shorter size when one is a prefix of the other.
    inline size_t first_difference(const uint8_t* a, size_t na, const uint8_t* b, size_t nb) {
        const size_t n = na < nb ? na : nb;
        for (size_t i = 0; i < n; ++i)
            if (a[i] != b[i]) return i;
        return n;
    }
}
