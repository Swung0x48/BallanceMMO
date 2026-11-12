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
// The same switch spares two more writes (script_replay restore), each of
// bytes the script holds already: a selected root that has been quiet since
// the kept source (below; so only with BMMO_REUSE_KEPT on) was taken and
// whose block there is the record's - its parameters and activation words
// are the record's; and, on every host with a trafo part, a trafo script the
// dormant ledger knows to hold its built state when the record says built
// (the capture's own rule and guard).  Everything else a restore does for
// such a script it still does: the force and joint tails (physics_RT's
// state), the omitted copies, the delayed links (a pending one is re-armed
// as ever).  On the public L6 re-drive with 13 players the roots' parameters
// and flags were 2.3 ms of a 4.2 ms script restore and the 104 trafo scripts
// 1.6 ms.
//
// The kept source (BMMO_REUSE_KEPT = off | on, on when unset): a rewind
// left every whole record after it a full walk - the replay's own (the record
// of t - 1 is a light one, or the restore's barrier is in the way) and the
// live one of the same frame (the engine's barrier at the rewind's end) -,
// and in a room of 13 a third of the frames rewind.  The kept source is the
// script list of the last record these scripts are known to hold: the one a
// capture left, or the one a restore wrote.  The ledger covers every
// selected root then (track), and a capture with no record of t - 1 copies a
// root's block from the kept source when the host-write generation is the
// source's and the root has been quiet at every observation since it was
// kept (OPC's rule, tick by tick: the light records observe).  A restore is
// an observation of its own (restored): a root it wrote byte for byte, whose
// words then read as the record's, has a run from the record's tick; no
// other root has one; a copy the restore skip left as it was keeps its memo
// (keep_memo).  A restore that fails after its first write forgets the
// source and every run.
// BMMO_SCRIPT_REUSE's and BMMO_SLOT_MEMO's verify walk every such copy and
// memo hit too.
//
// The copy plan (BMMO_COPY_PLAN = off | on, on when unset; with the kept
// source): a copy still walked the root's graph in step with its source's
// bytes to find the few it reads fresh - 0.6 ms of a 1.9 ms record in a room
// of 13.  A copy leaves a plan (struct plan below) and the next copies of
// that root are emitted from it.  BMMO_SCRIPT_REUSE's verify walks every
// such copy; a restore keeps a plan only with the restore skip on (not in
// its verify, which leaves no root as it was).
//
// The restore plan (BMMO_RESTORE_PLAN = off | on | verify, on when unset;
// with the restore skip): a script the skip leaves as it is was still walked
// block by block for its tails and omitted copies - 1.2 ms of a 2.9 ms
// script restore for the roots and 1.2 ms for the trafo scripts in a room of
// 13.  With a plan that held at the restore's start (a root's copy plan; a
// trafo script's over its built bytes, script_replay::trafo_plan) and no
// delayed link in flight in it, only the plan's fresh parts are located in
// the record - its copied bytes compared on the way - and put back by the
// walk's own two functions.  Verify walks as ever and compares where the
// two found the parts.
//
// The held trafo scripts (BMMO_TRAFO_HELD = off | on | verify, on when unset;
// script_replay::trafo_part): a trafo script that is not in its built state
// was walked whole by every whole capture and written whole by every restore
// though it had not run since - 0.5 ms of a 1.4 ms record and ~1 ms of a
// restore in a room of 13 (after one transformation a ball's reset scripts
// and its wind hold other bytes for good).  The dormant ledger's entry also
// holds a plan over the block a script is known to hold: a capture emits the
// block from it, and a restore that finds the record's block to be those
// bytes puts back its fresh parts only (with the restore skip).  Verify
// walks every such script and compares; a restore's verify then writes it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // BMMO_REUSE_KEPT, read once per process (on when unset).
    inline bool reuse_kept_from_env() {
        static const bool on = parse_script_reuse_mode(std::getenv("BMMO_REUSE_KEPT"), script_reuse_mode::on)
                               != script_reuse_mode::off;
        return on;
    }
    // BMMO_RESTORE_PLAN, read once per process (on when unset).
    inline script_reuse_mode restore_plan_mode_from_env() {
        static const script_reuse_mode mode = parse_script_reuse_mode(std::getenv("BMMO_RESTORE_PLAN"), script_reuse_mode::on);
        return mode;
    }
    // BMMO_TRAFO_HELD, read once per process (on when unset).
    inline script_reuse_mode trafo_held_mode_from_env() {
        static const script_reuse_mode mode = parse_script_reuse_mode(std::getenv("BMMO_TRAFO_HELD"), script_reuse_mode::on);
        return mode;
    }
    // BMMO_COPY_PLAN, read once per process (on when unset).
    inline bool copy_plan_from_env() {
        static const bool on = parse_script_reuse_mode(std::getenv("BMMO_COPY_PLAN"), script_reuse_mode::on)
                               != script_reuse_mode::off;
        return on;
    }

    // What the memo did in this process (diagnostic: nothing decides on it).
    struct slot_memo_stats {
        uint64_t walked = 0, hits = 0, miss_none = 0, miss_gen = 0, miss_run = 0, miss_tail = 0, joint = 0;
        uint64_t verified = 0, mismatches = 0;
        // The restore skip: omitted copies a restore left as they were, wrote,
        // and (verify) walked and found unlike the image.
        uint64_t restore_skipped = 0, restore_written = 0, restore_verified = 0, restore_mismatches = 0;
        // The roots a restore left (their tails apart) and wrote though it
        // had a kept source to judge by, and the dormant trafo scripts left.
        uint64_t restore_roots_skipped = 0, restore_roots_written = 0, restore_trafo_skipped = 0;
        // The restore plan: roots and trafo scripts put back by their plan,
        // and (verify) walked with the parts compared.
        uint64_t restore_roots_planned = 0, restore_trafo_planned = 0, restore_plan_verified = 0;
        // The held trafo scripts: plans made, blocks a capture emitted from
        // one, scripts a restore left by one (those it put back by the plan
        // count in restore_trafo_planned too), and what refused one - the
        // record's block has other bytes, a delayed link in flight, the
        // root's activation words (a capture's or a restore's: something
        // changed them without the ledger), a part's block gone; verify's
        // walks (a difference counts with the memo's or the restore's).
        uint64_t held_set = 0, held_captured = 0, held_restored = 0, held_other = 0, held_pending = 0, held_guard = 0;
        uint64_t held_gone = 0, held_verified = 0;
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
            char buf[1024];
            std::snprintf(buf, sizeof buf,
                          "slot_memo: mode=%s walked=%llu hits=%llu misses none=%llu gen=%llu run=%llu tail=%llu joint=%llu "
                          "verified=%llu mismatches=%llu restore_skip: mode=%s skipped=%llu written=%llu verified=%llu "
                          "mismatches=%llu roots=%llu/%llu trafo=%llu restore_plan: mode=%s roots=%llu trafo=%llu checked=%llu "
                          "trafo_held: mode=%s set=%llu captured=%llu restored=%llu other=%llu pending=%llu guard=%llu gone=%llu "
                          "checked=%llu",
                          script_reuse_mode_name(mode), static_cast<unsigned long long>(walked),
                          static_cast<unsigned long long>(hits), static_cast<unsigned long long>(miss_none),
                          static_cast<unsigned long long>(miss_gen), static_cast<unsigned long long>(miss_run),
                          static_cast<unsigned long long>(miss_tail), static_cast<unsigned long long>(joint),
                          static_cast<unsigned long long>(verified), static_cast<unsigned long long>(mismatches),
                          script_reuse_mode_name(restore_skip_mode_from_env()), static_cast<unsigned long long>(restore_skipped),
                          static_cast<unsigned long long>(restore_written), static_cast<unsigned long long>(restore_verified),
                          static_cast<unsigned long long>(restore_mismatches),
                          static_cast<unsigned long long>(restore_roots_skipped),
                          static_cast<unsigned long long>(restore_roots_written),
                          static_cast<unsigned long long>(restore_trafo_skipped),
                          script_reuse_mode_name(restore_plan_mode_from_env()),
                          static_cast<unsigned long long>(restore_roots_planned),
                          static_cast<unsigned long long>(restore_trafo_planned),
                          static_cast<unsigned long long>(restore_plan_verified),
                          script_reuse_mode_name(trafo_held_mode_from_env()), static_cast<unsigned long long>(held_set),
                          static_cast<unsigned long long>(held_captured), static_cast<unsigned long long>(held_restored),
                          static_cast<unsigned long long>(held_other), static_cast<unsigned long long>(held_pending),
                          static_cast<unsigned long long>(held_guard), static_cast<unsigned long long>(held_gone),
                          static_cast<unsigned long long>(held_verified));
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
            blocks_.clear(); bound_ = ~uint64_t{0}; learned_ = 0; roots_.clear(); tracked_.clear(); plans_.clear();
            disarm_restore();
            forget();
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
            plans_.clear();   // a copy block more: what a root's copy emits changed
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
        void disarm_restore() { restore_armed_ = false; restore_quiet_.clear(); kept_.clear(); kept_plans_.clear(); }
        bool restore_armed() const { return restore_armed_; }

        // The kept source.  A root the ledger observes besides the copies'.
        void track(uint32_t root) {
            if (root && tracked_.insert(root).second && std::find(roots_.begin(), roots_.end(), root) == roots_.end())
                roots_.push_back(root);
        }
        // `record[0, size)` (a record up to the end of its script list) is
        // what the scripts hold as of this observation, under the
        // host-write generation `writes`.
        void keep_source(const uint8_t* record, size_t size, uint64_t writes, bool restored = false) {
            source_.assign(record, record + size);
            source_seq_ = seq_;
            source_writes_ = writes;
            source_restored_ = restored;
            have_source_ = true;
        }
        // The kept source a capture under `writes_now` may copy from, or
        // null; whether a restore wrote it; whether `root` has been quiet
        // since it was kept.
        const std::vector<uint8_t>* kept_source(uint64_t writes_now) const {
            return have_source_ && source_writes_ == writes_now ? &source_ : nullptr;
        }
        bool kept_restored() const { return source_restored_; }
        bool quiet_since_kept(uint32_t root) const { return have_source_ && quiet_since(root) <= source_seq_; }
        // Nothing is known of any root (a restore failed part-way, or one
        // whose record's tick the caller does not say).
        void forget() { runs_.clear(); have_source_ = false; }

        // The copy plan.  A root's block as a copy emits it (script_replay
        // copy_with_fresh_tails), cut at the bytes read fresh: `statics` are
        // the copied bytes end to end, and each part says after how many of
        // them a block's force and joint tail, or its slot image section,
        // is emitted.  Stored by a copy an observing capture made, it serves
        // the next copies of that root while the memo's rule holds - the
        // same host-write generation, the root quiet since - under the same
        // slot mask: they need neither the source's bytes
        // nor the graph.  A restore that leaves the root as it is keeps its
        // plan (keep_plan, restored); a copy block added drops every plan.
        // A plan checks no structure, as the copy did block by block: it
        // rests on every change of a selected root's graph - a block or a
        // link made or deleted, a copy rewired - bumping the host-write
        // generation (note_script_write) or going through add().
        struct plan_part { uint32_t statics = 0, block = 0; bool images = false; };
        struct plan {
            std::vector<uint8_t> statics;
            std::vector<plan_part> parts;
            uint64_t seq = 0, writes = 0, mask = 0;
            uint32_t tails = 0;     // the force and joint blocks among the parts
            bool pending = false;   // a delayed link in flight in the copied bytes (a restore re-arms it)
            bool valid = false;
        };
        plan& plan_of(uint32_t root) { return plans_[root]; }
        // `root`'s plan when it can serve a copy now, else null and why not
        // in `refused` ("none", "gen", "mask", "run": the memo's words).
        const plan* plan_for(uint32_t root, uint64_t writes_now, const char*& refused) const {
            const auto found = plans_.find(root);
            refused = nullptr;
            if (found == plans_.end() || !found->second.valid) refused = "none";
            else if (found->second.writes != writes_now) refused = "gen";
            else if (found->second.mask != bound_) refused = "mask";
            else if (quiet_since(root) > found->second.seq) refused = "run";
            return refused ? nullptr : &found->second;
        }
        // `root`'s plan when it held at the armed restore's start, under
        // the mask now; null otherwise.
        const plan* plan_at_restore(uint32_t root) const {
            const auto found = plans_.find(root);
            return restore_armed_ && found != plans_.end() && found->second.valid && found->second.writes == restore_writes_
                           && found->second.mask == bound_ && quiet_since(root) <= found->second.seq
                       ? &found->second : nullptr;
        }
        // The restore skip left `root` as it was: its plan, good at the
        // restore's start under the record's mask, is good after it.
        // Whether there was one.
        bool keep_plan(uint32_t root) {
            if (!plan_at_restore(root)) return false;
            kept_plans_.push_back(root);
            return true;
        }
        // A copy the restore skip left as it was (it holds its memo's image).
        void keep_memo(uint32_t id) { kept_.push_back(id); }
        // A restore that ended well, as one observation: `holding` are the
        // roots that now hold exactly the record's block, with the record's
        // words - each has a run from `tick`; no other root has one.  The
        // copies kept are as good as walked now.
        void restored(uint32_t tick, const std::vector<script_root_words>& holding, const uint8_t* record, size_t size,
                      uint64_t writes) {
            ++seq_;
            runs_.clear();
            for (const script_root_words& words: holding) {
                run& r = runs_[words.id];
                r.from = seq_;
                r.seen = true;
                r.tick = tick;
                r.words = words;
            }
            for (uint32_t id: kept_)
                if (copy* c = find(id); c && c->m.valid) {
                    c->m.seq = seq_;
                    c->m.writes = writes;
                }
            kept_.clear();
            for (uint32_t root: kept_plans_) {
                plan& p = plans_[root];
                p.seq = seq_;
                p.writes = writes;
            }
            kept_plans_.clear();
            keep_source(record, size, writes, true);
        }
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
        std::vector<uint32_t> kept_;
        std::unordered_map<uint32_t, plan> plans_;
        std::vector<uint32_t> kept_plans_;
        std::unordered_set<uint32_t> tracked_;
        std::vector<uint8_t> source_;
        uint64_t source_seq_ = 0, source_writes_ = 0;
        bool have_source_ = false, source_restored_ = false;
    };
}
