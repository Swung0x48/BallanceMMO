#pragma once

// Step OPB (FULL close-out; rewind-cost-design "Research (step OPTR)" R.1 /
// R.4 row B, option B1): the rollback binding's written-cell section memoized.
//
// bind_script_replay (session/rollback_binding_scripts.hpp) asks
// game/array_reads.hpp written_cells for the record's written-cell columns at
// every binding, and the session core binds its rollback world on every use
// (two binds per post_step): a walk of every block of every selected script,
// its arrays and its constant row / column pins - 0.46 / 1.51 / 0.14 ms per
// bind on L8 / L12 / h1.  The columns change only when what the walk reads
// changes, so this memo keeps the last walk's columns under a key and walks
// again only when the key differs:
//
//  - `scripts`: the selection's script ids and the objects they resolve to
//    (a script born or gone, an id resolving elsewhere);
//  - `pins`: what the walk's result can follow at run time - for every block
//    that can put a column into the section (a column-confined writer: Set
//    Cell, Column Transform, Column Operation) the arrays its target and
//    inputs resolve to now (arrays_of follows GetRealSource(): a pin a script
//    repoints, or one the host rewires with SetDirectSource), and its row and
//    column pins' real sources and constant values;
//  - `layout`: each array those blocks name - the object it resolves to, its
//    name (the census classifies by it), its column count and column types
//    (table_of reads them live).
//
// Any difference walks again (and takes the new key).  The host drops the
// memo where it makes a new selection (an anchor, a level), so a CK_ID reused
// across a reload never meets the previous level's memo.
//
// Modes (BMMO_CELLS_MEMO=off | on | verify, on when unset): off walks every
// bind and keeps nothing; verify walks every memo hit as well, compares, counts
// and notes a mismatch, and still returns the memo's columns (so a verify run
// records exactly what mode on would; the memo is dropped after a mismatch, so
// the next bind walks).
//
// Engine-free (no CK types): array_reads.hpp supplies the probe (the key now,
// from the watch list the last walk made) and the walk; the unit tests drive
// it with fakes.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace bmmo::game {
    enum class cells_memo_mode : uint8_t { off = 0, on = 1, verify = 2 };

    inline const char* cells_memo_mode_name(cells_memo_mode mode) {
        switch (mode) {
            case cells_memo_mode::off: return "off";
            case cells_memo_mode::verify: return "verify";
            default: return "on";
        }
    }

    // "off" | "on" | "verify" (nullptr or anything else: `fallback`).
    inline cells_memo_mode parse_cells_memo_mode(const char* text, cells_memo_mode fallback) {
        if (!text) return fallback;
        if (std::strcmp(text, "off") == 0 || std::strcmp(text, "0") == 0) return cells_memo_mode::off;
        if (std::strcmp(text, "on") == 0 || std::strcmp(text, "1") == 0) return cells_memo_mode::on;
        if (std::strcmp(text, "verify") == 0) return cells_memo_mode::verify;
        return fallback;
    }

    // BMMO_CELLS_MEMO, read once per process; on when unset.
    inline cells_memo_mode cells_memo_mode_from_env() {
        static const cells_memo_mode mode = parse_cells_memo_mode(std::getenv("BMMO_CELLS_MEMO"), cells_memo_mode::on);
        return mode;
    }

    // The key, in three parts so a re-walk can say which one changed.
    struct cells_memo_key {
        std::vector<uint64_t> scripts, pins, layout;
        std::vector<std::string> names;   // the layout's array names
        void clear() {
            scripts.clear();
            pins.clear();
            layout.clear();
            names.clear();
        }
    };

    struct cells_memo_stats {
        uint64_t binds = 0;         // columns asked for
        uint64_t hits = 0;          // answered from the memo
        uint64_t walks = 0;         // walks whose result was returned (first, changed, off)
        uint64_t first = 0;         // walks with no memo (the first bind, after a drop or a mismatch)
        uint64_t changed_scripts = 0, changed_pins = 0, changed_layout = 0;   // walks because that key part differed
        uint64_t verified = 0;      // verify: hits walked again and compared
        uint64_t mismatches = 0;    // verify: the memo's columns differed from the fresh walk's
        uint64_t drops = 0;         // host drops of a non-empty memo
        // The last walk's watch list: the column-confined writer blocks, the
        // pins of theirs re-resolved at every bind, of those the pins whose
        // real source is not a never-written local of the block's script (a
        // value that can change at run time: OPTR's "dynamic pins"), and the
        // arrays whose layout is compared.
        size_t blocks = 0, pins = 0, dynamic_pins = 0, arrays = 0;
        size_t columns = 0;         // the last result's column count
    };

    // Value: the walk's result (operator== comparable, with size()).  Watch:
    // what the probe needs to build the key now (the walk fills it), with
    // the counts count_blocks, count_pins, count_dynamic, count_arrays.
    template <class Value, class Watch>
    class written_cells_memo {
    public:
        explicit written_cells_memo(cells_memo_mode mode = cells_memo_mode_from_env()) : mode_(mode) {}

        cells_memo_mode mode() const { return mode_; }
        void set_mode(cells_memo_mode mode) {
            mode_ = mode;
            valid_ = false;
        }
        const cells_memo_stats& stats() const { return stats_; }
        const std::vector<std::string>& notes() const { return notes_; }
        const Watch& watch() const { return watch_; }
        bool valid() const { return valid_; }

        // The host drops the memo where it makes a new selection.
        void drop() {
            if (valid_) ++stats_.drops;
            valid_ = false;
        }

        // probe(const Watch&, cells_memo_key& now) -> bool: the key now from
        // the last walk's watch list (false: the list no longer resolves -
        // walk again).  walk(Watch* watch, cells_memo_key* key) -> Value: the
        // fresh walk; with non-null pointers it also fills the watch list and
        // the key for the result.  describe(const Value&) -> std::string, for
        // a mismatch note.  `tick` only labels a note.
        template <class Probe, class Walk, class Describe>
        Value get(Probe&& probe, Walk&& walk, Describe&& describe, uint64_t tick = 0) {
            ++stats_.binds;
            if (mode_ == cells_memo_mode::off) {
                ++stats_.walks;
                valid_ = false;
                Value v = walk(static_cast<Watch*>(nullptr), static_cast<cells_memo_key*>(nullptr));
                stats_.columns = size_of(v);
                return v;
            }
            if (valid_) {
                now_.clear();
                const bool resolves = probe(static_cast<const Watch&>(watch_), now_);
                if (resolves && now_.scripts == key_.scripts && now_.pins == key_.pins && now_.layout == key_.layout
                    && now_.names == key_.names) {
                    ++stats_.hits;
                    if (mode_ == cells_memo_mode::verify) {
                        ++stats_.verified;
                        const Value fresh = walk(static_cast<Watch*>(nullptr), static_cast<cells_memo_key*>(nullptr));
                        if (!(fresh == value_)) {
                            ++stats_.mismatches;
                            if (notes_.size() < kMaxNotes)
                                notes_.push_back("cells_memo mismatch (bind " + std::to_string(stats_.binds) + ", tick "
                                                 + std::to_string(tick) + "): memo " + describe(value_) + " fresh "
                                                 + describe(fresh) + " - the record carried the memo's columns");
                            Value kept = value_;
                            valid_ = false;   // the next bind walks
                            return kept;
                        }
                    }
                    return value_;
                }
                if (!resolves || now_.scripts != key_.scripts) ++stats_.changed_scripts;
                else if (now_.pins != key_.pins) ++stats_.changed_pins;
                else ++stats_.changed_layout;
            } else {
                ++stats_.first;
            }
            ++stats_.walks;
            watch_ = Watch{};
            key_.clear();
            value_ = walk(&watch_, &key_);
            valid_ = true;
            stats_.columns = size_of(value_);
            note_watch(watch_);
            return value_;
        }

        // The counters as one line ("cells_memo: ...").
        std::string line() const {
            const cells_memo_stats& s = stats_;
            std::string out = "cells_memo: mode=" + std::string(cells_memo_mode_name(mode_))
                            + " binds=" + std::to_string(s.binds) + " hits=" + std::to_string(s.hits)
                            + " walks=" + std::to_string(s.walks) + " (first=" + std::to_string(s.first)
                            + " scripts=" + std::to_string(s.changed_scripts) + " pins=" + std::to_string(s.changed_pins)
                            + " layout=" + std::to_string(s.changed_layout) + ") drops=" + std::to_string(s.drops)
                            + " verified=" + std::to_string(s.verified) + " mismatches=" + std::to_string(s.mismatches)
                            + " watch: blocks=" + std::to_string(s.blocks) + " pins=" + std::to_string(s.pins)
                            + " dynamic_pins=" + std::to_string(s.dynamic_pins) + " arrays=" + std::to_string(s.arrays)
                            + " columns=" + std::to_string(s.columns);
            return out;
        }

        // Tests only: overwrite the memo's columns (a forced stale memo).
        void force_value(const Value& v) { value_ = v; }

    private:
        static constexpr size_t kMaxNotes = 8;

        static size_t size_of(const Value& v) { return static_cast<size_t>(v.size()); }
        void note_watch(const Watch& w) {
            stats_.blocks = w.count_blocks;
            stats_.pins = w.count_pins;
            stats_.dynamic_pins = w.count_dynamic;
            stats_.arrays = w.count_arrays;
        }

        cells_memo_mode mode_;
        bool valid_ = false;
        Watch watch_{};
        cells_memo_key key_, now_;
        Value value_{};
        cells_memo_stats stats_;
        std::vector<std::string> notes_;
    };
}
