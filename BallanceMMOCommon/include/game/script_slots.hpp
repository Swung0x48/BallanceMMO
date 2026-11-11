#pragma once

// The per-player unit copies of a record (step SLOT, R1): a record carries
// the full state of the copies of its bound slots; a copy of an unbound slot
// is not walked but described against its BASE - its image at the copy step,
// never replaced - by a delta in the record's slot image section
// (script_replay kSectionSlotImages).  An unbound copy runs with its module's
// script (its activation words and its sensors' countdowns change every few
// ticks), so a restore writes base + the record's delta for every copy the
// record leaves out, whatever was restored before.  CK-free.
//
// T3C step e3, the slot memo (BMMO_SLOT_MEMO = off | on | verify, on when
// unset): the walk of an omitted copy was ~40 % of a Level 12 live capture
// and 3.3-4.6 ms of a rewind.  A copy is a sub-behaviour of its unit's root
// script, and the engine executes nothing under a root it does not execute,
// so while that root stays quiet - OPC's premise, game/script_record_reuse.hpp
// reusable_root, observed at every capture, light ones included - the copy's
// bytes stay what they were, all but its force tails (physics_RT's state,
// read again and compared).  The memo is its last walked image and section
// entry; a capture appends the entry instead of walking when the host-write
// generation is the memo's, the root has been quiet at every observation
// since the memo was walked, and every force tail reads as it did.  A copy
// with a joint block is never memoized.  The bytes appended are the walk's,
// so a record is byte-identical with the memo on or off; verify walks every
// hit as well and compares.
//
// The restore skip (BMMO_RESTORE_SKIP = off | on | verify, on when unset): a
// restore wrote every omitted copy back (base + the record's delta), ~0.3-0.8
// ms of a rewind at the public K = 20, and the census found 99.7-100 % of
// them already holding exactly those bytes.  The same premise leaves a copy
// as it is: its root still passes the reuse rule against its last
// observation when the restore starts (quiet_now, read before the restore
// writes any root's flags), its memo is current by the capture's rule
// (memo_refused, every force tail re-read), the memo is the image the record
// names, and the copy has no delayed link (whose re-arm a write performs).
// Verify walks each such copy, compares it with the image and writes it
// anyway, so a verify run is the off run.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "script_record_reuse.hpp"

namespace bmmo::game {
    // BMMO_SLOT_MEMO, read once per process (on when unset: the gate passed -
    // verify at 0 mismatches over the fast LBM and retail L11 / L12).
    inline script_reuse_mode slot_memo_mode_from_env() {
        static const script_reuse_mode mode = parse_script_reuse_mode(std::getenv("BMMO_SLOT_MEMO"), script_reuse_mode::on);
        return mode;
    }
    // BMMO_RESTORE_SKIP, read once per process (on when unset).
    inline script_reuse_mode restore_skip_mode_from_env() {
        static const script_reuse_mode mode = parse_script_reuse_mode(std::getenv("BMMO_RESTORE_SKIP"), script_reuse_mode::on);
        return mode;
    }

    // What the memo did in this process (diagnostic: nothing decides on it).
    struct slot_memo_stats {
        uint64_t walked = 0, hits = 0, miss_none = 0, miss_gen = 0, miss_run = 0, miss_tail = 0, joint = 0;
        uint64_t verified = 0, mismatches = 0;
        // The restore skip: omitted copies a restore left as they were, wrote,
        // and (verify) walked and found unlike the image.
        uint64_t restore_skipped = 0, restore_written = 0, restore_verified = 0, restore_mismatches = 0;
        std::vector<std::string> notes;
        void note(const std::string& what) {
            ++mismatches;
            if (notes.size() < 16) notes.push_back(what);
        }
        void restore_note(const std::string& what) {
            ++restore_mismatches;
            if (notes.size() < 16) notes.push_back(what);
        }
        std::string line(script_reuse_mode mode) const {
            char buf[448];
            std::snprintf(buf, sizeof buf,
                          "slot_memo: mode=%s walked=%llu hits=%llu misses none=%llu gen=%llu run=%llu tail=%llu joint=%llu "
                          "verified=%llu mismatches=%llu restore_skip: mode=%s skipped=%llu written=%llu verified=%llu "
                          "mismatches=%llu",
                          script_reuse_mode_name(mode), static_cast<unsigned long long>(walked),
                          static_cast<unsigned long long>(hits), static_cast<unsigned long long>(miss_none),
                          static_cast<unsigned long long>(miss_gen), static_cast<unsigned long long>(miss_run),
                          static_cast<unsigned long long>(miss_tail), static_cast<unsigned long long>(joint),
                          static_cast<unsigned long long>(verified), static_cast<unsigned long long>(mismatches),
                          script_reuse_mode_name(restore_skip_mode_from_env()), static_cast<unsigned long long>(restore_skipped),
                          static_cast<unsigned long long>(restore_written), static_cast<unsigned long long>(restore_verified),
                          static_cast<unsigned long long>(restore_mismatches));
            return buf;
        }
    };
    inline slot_memo_stats& slot_memo_totals() {
        static slot_memo_stats stats;
        return stats;
    }

    class script_slots {
    public:
        // A force or joint block's tail in a copy's image: its id, offset
        // and length (put_force_and_joint's bytes for that block).
        struct tail { uint32_t block = 0, offset = 0, length = 0; bool joint = false; };
        struct memo {
            std::vector<uint8_t> image, entry;   // the walked image; the section entry it made (empty: equal to the base)
            std::vector<tail> tails;
            uint64_t seq = 0, writes = 0;        // the observation it was walked at; the host-write generation
            bool valid = false;
        };
        struct copy {
            uint64_t slot = 0;
            std::vector<uint8_t> base;
            uint32_t base_hash = 0;
            uint32_t root = 0;                   // the unit's root script (0: unknown, never memoized)
            int8_t delayed = -1;                 // a link with an activation delay in the copy (-1: not looked at yet)
            memo m;
        };

        void clear() {
            blocks_.clear(); bound_ = ~uint64_t{0}; learned_ = 0; roots_.clear(); runs_.clear();
            disarm_restore();
        }
        // A re-copy of the same block id (another rewire) replaces its base
        // (and so its memo).
        void add(uint32_t id, uint8_t slot, std::vector<uint8_t> base, uint32_t root = 0) {
            const uint32_t h = hash(base);
            copy c;
            c.slot = uint64_t{1} << slot;
            c.base = std::move(base);
            c.base_hash = h;
            c.root = root;
            blocks_.insert_or_assign(id, std::move(c));
            if (root && std::find(roots_.begin(), roots_.end(), root) == roots_.end()) roots_.push_back(root);
        }
        copy* find(uint32_t id) {
            const auto found = blocks_.find(id);
            return found == blocks_.end() ? nullptr : &found->second;
        }

        // The quiet-run ledger.  One observation per capture: begin(tick)
        // numbers it, then every root's activation words.  A root's run goes
        // on across consecutive ticks while OPC's rule holds (reusable_root:
        // inactive at both ends, nothing pending, no IO switched); anything
        // else - a word change, a frame flag, a gap, a backward jump (a
        // rewind), the same tick again (an amend), a root not found - starts
        // it again at this observation.
        const std::vector<uint32_t>& roots() const { return roots_; }
        uint64_t begin_observation() { return ++seq_; }
        uint64_t seq() const { return seq_; }
        void observe(uint32_t root, uint32_t tick, bool present, const script_root_words& now, uint32_t frame_flags) {
            run& r = runs_[root];
            const bool goes_on = r.seen && present && tick == r.tick + 1 && reusable_root(r.words, now, frame_flags);
            if (!goes_on) r.from = seq_;
            r.seen = present;
            r.tick = tick;
            r.words = now;
        }
        uint64_t quiet_since(uint32_t root) const {
            const auto found = runs_.find(root);
            return found == runs_.end() ? std::numeric_limits<uint64_t>::max() : found->second.from;
        }
        // Whether this capture observed (only such a capture may use a memo:
        // another, the hold digest's or a tool's, may come after a frame the
        // ledger has not seen).
        bool observing() const { return observing_; }
        void set_observing(bool on) { observing_ = on; }
        // Why `c`'s memo cannot serve now (nullptr: it can, its tails still
        // to be compared).
        const char* memo_refused(const copy& c, uint64_t writes_now) const {
            if (!c.m.valid) return "none";
            if (c.m.writes != writes_now) return "gen";
            if (quiet_since(c.root) > c.m.seq) return "run";
            return nullptr;
        }
        // The restore skip: whether `root` would go on quiet if observed now
        // (its run seen, the reuse rule against its last words).
        bool quiet_now(uint32_t root, const script_root_words& now, uint32_t frame_flags) const {
            const auto found = runs_.find(root);
            return found != runs_.end() && found->second.seen && reusable_root(found->second.words, now, frame_flags);
        }
        // A restore's roots still quiet at its start and the host-write
        // generation then, until disarm_restore.
        void arm_restore(std::vector<uint32_t> quiet, uint64_t writes) {
            std::sort(quiet.begin(), quiet.end());
            restore_quiet_ = std::move(quiet);
            restore_writes_ = writes;
            restore_armed_ = true;
        }
        void disarm_restore() { restore_armed_ = false; restore_quiet_.clear(); }
        bool restore_armed() const { return restore_armed_; }
        uint64_t restore_writes() const { return restore_writes_; }
        bool restore_quiet(uint32_t root) const {
            return restore_armed_ && std::binary_search(restore_quiet_.begin(), restore_quiet_.end(), root);
        }
        void select(uint64_t bound) { bound_ = bound | learned_; }
        void select_record(uint64_t bound) { bound_ = bound; }
        void learn_mask(uint64_t bound) { learned_ |= bound; }
        uint64_t mask() const { return bound_; }
        bool bound(uint8_t slot) const { return (bound_ & (uint64_t{1} << slot)) != 0; }
        bool omitted(const copy& c) const { return !(bound_ & c.slot); }
        // A copy block whose slot the selected mask leaves out.
        bool omit(uint32_t id) const {
            const auto found = blocks_.find(id);
            return found != blocks_.end() && !(bound_ & found->second.slot);
        }
        const std::vector<uint8_t>* base(uint32_t id) const {
            const auto found = blocks_.find(id);
            return found == blocks_.end() ? nullptr : &found->second.base;
        }
        uint32_t base_hash(uint32_t id) const {
            const auto found = blocks_.find(id);
            return found == blocks_.end() ? 0u : found->second.base_hash;
        }

        // FNV-1a 32 of a base (the section names the base it was made from).
        static uint32_t hash(const std::vector<uint8_t>& bytes) {
            uint32_t h = 2166136261u;
            for (uint8_t b: bytes) h = (h ^ b) * 16777619u;
            return h;
        }

        // `image` against `base`, appended to `out`; false (nothing written)
        // when they are equal.  Same size: u8 0, u16 run count, each run u32
        // offset, u16 length, the bytes; another size (or too many runs):
        // u8 1, u32 size, the whole image.
        static bool put_delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& image, std::vector<uint8_t>& out) {
            if (image == base) return false;
            const auto u8 = [&out](uint8_t v) { out.push_back(v); };
            const auto u16 = [&out](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
            const auto u32 = [&out](uint32_t v) { for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k))); };
            const auto whole = [&] {
                u8(1);
                u32(static_cast<uint32_t>(image.size()));
                out.insert(out.end(), image.begin(), image.end());
                return true;
            };
            if (image.size() != base.size()) return whole();
            // Runs of differing bytes; two runs closer than a run header
            // (6 bytes) are one.
            std::vector<std::pair<size_t, size_t>> runs;   // [begin, end)
            for (size_t k = 0; k < image.size();) {
                if (image[k] == base[k]) { ++k; continue; }
                size_t end = k + 1;
                while (end < image.size() && image[end] != base[end]) ++end;
                if (!runs.empty() && k - runs.back().second < 6 && end - runs.back().first <= 0xffffu) runs.back().second = end;
                else runs.emplace_back(k, end);
                k = end;
            }
            size_t bytes = 3;
            for (auto& r: runs) bytes += 6 + (r.second - r.first);
            if (runs.size() > 0xffffu || bytes >= image.size() + 5) return whole();
            for (const auto& r: runs)
                if (r.second - r.first > 0xffffu) return whole();
            u8(0);
            u16(static_cast<uint16_t>(runs.size()));
            for (const auto& r: runs) {
                u32(static_cast<uint32_t>(r.first));
                u16(static_cast<uint16_t>(r.second - r.first));
                out.insert(out.end(), image.begin() + static_cast<std::ptrdiff_t>(r.first),
                           image.begin() + static_cast<std::ptrdiff_t>(r.second));
            }
            return true;
        }
        // The image a delta at `pos` describes against `base`; false on a
        // truncated or malformed delta.
        static bool get_delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& in, size_t& pos,
                              std::vector<uint8_t>& image) {
            const auto need = [&](size_t n) { return pos + n <= in.size(); };
            const auto u16 = [&]() { uint16_t v = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8)); pos += 2; return v; };
            const auto u32 = [&]() {
                uint32_t v = 0;
                for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(in[pos + static_cast<size_t>(k)]) << (8 * k);
                pos += 4;
                return v;
            };
            if (!need(1)) return false;
            const uint8_t kind = in[pos++];
            if (kind == 1) {
                if (!need(4)) return false;
                const uint32_t size = u32();
                if (!need(size)) return false;
                image.assign(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + size));
                pos += size;
                return true;
            }
            if (kind != 0 || !need(2)) return false;
            const uint16_t runs = u16();
            image = base;
            for (uint16_t i = 0; i < runs; ++i) {
                if (!need(6)) return false;
                const uint32_t offset = u32();
                const uint16_t length = u16();
                if (!need(length) || static_cast<size_t>(offset) + length > image.size()) return false;
                std::memcpy(image.data() + offset, in.data() + pos, length);
                pos += length;
            }
            return true;
        }
        // Past a delta (no base needed).
        static bool skip_delta(const std::vector<uint8_t>& in, size_t& pos) {
            if (pos + 1 > in.size()) return false;
            const uint8_t kind = in[pos++];
            if (kind == 1) {
                if (pos + 4 > in.size()) return false;
                uint32_t size = 0;
                for (int k = 0; k < 4; ++k) size |= static_cast<uint32_t>(in[pos + static_cast<size_t>(k)]) << (8 * k);
                pos += 4;
                if (pos + size > in.size()) return false;
                pos += size;
                return true;
            }
            if (kind != 0 || pos + 2 > in.size()) return false;
            const uint16_t runs = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
            pos += 2;
            for (uint16_t i = 0; i < runs; ++i) {
                if (pos + 6 > in.size()) return false;
                const uint16_t length = static_cast<uint16_t>(in[pos + 4] | (in[pos + 5] << 8));
                pos += 6;
                if (pos + length > in.size()) return false;
                pos += length;
            }
            return true;
        }
    private:
        struct run {
            uint64_t from = 0;
            uint32_t tick = 0;
            script_root_words words;
            bool seen = false;
        };
        std::unordered_map<uint32_t, copy> blocks_;
        uint64_t bound_ = ~uint64_t{0}, learned_ = 0;
        std::vector<uint32_t> roots_;
        std::unordered_map<uint32_t, run> runs_;
        uint64_t seq_ = 0;
        bool observing_ = false;
        std::vector<uint32_t> restore_quiet_;
        uint64_t restore_writes_ = 0;
        bool restore_armed_ = false;
    };
}
