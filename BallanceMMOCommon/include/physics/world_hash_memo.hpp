#pragma once

// Step OPA (FULL close-out; rewind-cost-design "Research (step OPTR)" R.1 /
// R.4 row A): the journal TICK hash's surface signature memoized per object.
//
// surface_signature() (physics_state.cpp) sums one FNV-1a value per
// physicalized object - the sequential stream name -> surface-manager type ->
// compact-surface bytes (world_hash.hpp's fnv1a64) - each folded with
// fold_term(), so the sum does not depend on table order.  Hashing every
// object's compact surface every journalled tick cost 1.5 / 3.6 ms per frame
// (L8 / L12).  This memo keeps each object's whole per-object value (never the
// surface term alone: the stream cannot be split and recombined) and re-uses
// it while the object's identity is unchanged, so the summed value is
// bit-identical to a full re-hash.
//
// The rules (the design's, plus two stronger keys - see "As built (step OPA)"):
//  - the memo is a list in table order.  When the environment, the number of
//    objects or any position's (table key, object) pair differs from the
//    previous hashed tick, the whole memo is dropped and every object hashed
//    again (a set change);
//  - otherwise a position whose surface manager, manager type, compact
//    surface pointer, surface size, leading surface bytes (kHeadBytes) or
//    name differs is hashed again (that object only);
//  - the host drops the whole memo at every surface free it makes itself and
//    whenever a body was deleted since the previous hashed tick (the
//    generation it passes in);
//  - mode off hashes everything and keeps no memo; mode verify hashes every
//    object every tick as well, counts and notes each object whose memoized
//    value differs from the fresh one, and still returns the memoized sum (so
//    a verify run journals exactly what mode on would; the entry takes the
//    fresh value afterwards).
//
// Engine-free (no CK / IVP types), so the unit tests drive it with fake
// objects; physics_state.cpp fills the probes from m_PhysicsObjects.

#include "world_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace bmmo::physics {
    enum class surface_memo_mode : uint8_t { off = 0, on = 1, verify = 2 };

    inline const char* surface_memo_mode_name(surface_memo_mode mode) {
        switch (mode) {
            case surface_memo_mode::off: return "off";
            case surface_memo_mode::verify: return "verify";
            default: return "on";
        }
    }

    // "off" | "on" | "verify" (nullptr or anything else: `fallback`).
    inline surface_memo_mode parse_surface_memo_mode(const char* text, surface_memo_mode fallback) {
        if (!text) return fallback;
        if (std::strcmp(text, "off") == 0 || std::strcmp(text, "0") == 0) return surface_memo_mode::off;
        if (std::strcmp(text, "on") == 0 || std::strcmp(text, "1") == 0) return surface_memo_mode::on;
        if (std::strcmp(text, "verify") == 0) return surface_memo_mode::verify;
        return fallback;
    }

    // One physicalized object as the memo keys it.  `surface` must point at
    // `surface_size` readable bytes when it is not null.
    struct surface_memo_probe {
        uint32_t entry_key = 0;          // the m_PhysicsObjects key (CK_ID)
        const void* object = nullptr;    // IVP_Real_Object*
        const void* manager = nullptr;   // its IVP_SurfaceManager* (null: none)
        int manager_type = -1;           // IVP_SURMAN_TYPE, -1 without a manager
        const void* surface = nullptr;   // IVP_Compact_Surface* of a polygon manager
        uint32_t surface_size = 0;
        const char* name = "";           // never null
    };

    struct surface_memo_stats {
        uint64_t calls = 0;          // signatures computed
        uint64_t objects = 0;        // objects summed over all calls
        uint64_t hits = 0;           // objects whose memoized value was used
        uint64_t hashed = 0;         // objects hashed because the memo had no valid value
        uint64_t set_drops = 0;      // whole memo dropped: environment / count / (key, object) pairs changed
        uint64_t host_drops = 0;     // whole memo dropped by the host (a surface free, a body deleted)
        uint64_t verified = 0;       // verify mode: hits re-hashed and compared
        uint64_t mismatches = 0;     // verify mode: hits whose value was stale
        void add(const surface_memo_stats& o) {
            calls += o.calls; objects += o.objects; hits += o.hits; hashed += o.hashed;
            set_drops += o.set_drops; host_drops += o.host_drops; verified += o.verified; mismatches += o.mismatches;
        }
    };

    class surface_memo {
    public:
        static constexpr size_t kHeadBytes = 32;

        // The order-free sum over `probes` (in table order); `hash_one(i)`
        // returns probes[i]'s per-object FNV value.  `generation` is the
        // host's body-deletion count: a change drops the whole memo.  Each
        // verify mismatch is described into `notes` (when given, at most
        // `max_notes` in it).
        template <class HashOne>
        uint64_t signature(const void* environment, uint64_t generation, const std::vector<surface_memo_probe>& probes,
                           surface_memo_mode mode, HashOne&& hash_one, surface_memo_stats& stats,
                           std::vector<std::string>* notes = nullptr, size_t max_notes = 16) {
            ++stats.calls;
            stats.objects += probes.size();
            uint64_t total = 0;
            if (mode == surface_memo_mode::off) {
                entries_.clear();
                environment_ = nullptr;
                for (size_t i = 0; i < probes.size(); ++i) total += fold_term(hash_one(i));
                stats.hashed += probes.size();
                return total;
            }
            if (generation != generation_ && !entries_.empty()) {
                entries_.clear();
                ++stats.host_drops;
            }
            generation_ = generation;
            bool same_set = environment == environment_ && probes.size() == entries_.size();
            for (size_t i = 0; same_set && i < probes.size(); ++i)
                same_set = entries_[i].entry_key == probes[i].entry_key && entries_[i].object == probes[i].object;
            if (!same_set) {
                if (!entries_.empty()) ++stats.set_drops;
                entries_.clear();
                entries_.reserve(probes.size());
                environment_ = environment;
                for (size_t i = 0; i < probes.size(); ++i) {
                    entries_.emplace_back();
                    fill(entries_.back(), probes[i]);
                    entries_.back().value = hash_one(i);
                    total += fold_term(entries_.back().value);
                }
                stats.hashed += probes.size();
                return total;
            }
            for (size_t i = 0; i < probes.size(); ++i) {
                entry& e = entries_[i];
                const surface_memo_probe& p = probes[i];
                if (!matches(e, p)) {
                    fill(e, p);
                    e.value = hash_one(i);
                    ++stats.hashed;
                } else {
                    ++stats.hits;
                    if (mode == surface_memo_mode::verify) {
                        const uint64_t fresh = hash_one(i);
                        ++stats.verified;
                        if (fresh != e.value) {
                            ++stats.mismatches;
                            if (notes && notes->size() < max_notes) {
                                char line[320];
                                std::snprintf(line, sizeof(line),
                                              "key %u '%.64s' object %p manager %p type %d surface %p size %u: "
                                              "memo %016llx fresh %016llx",
                                              p.entry_key, p.name, p.object, p.manager, p.manager_type, p.surface,
                                              p.surface_size, static_cast<unsigned long long>(e.value),
                                              static_cast<unsigned long long>(fresh));
                                notes->push_back(line);
                            }
                            // This tick journals what mode on would (the memo's
                            // value); later ticks use the fresh one.
                            total += fold_term(e.value);
                            e.value = fresh;
                            continue;
                        }
                    }
                }
                total += fold_term(e.value);
            }
            return total;
        }

        // The host's drop (a surface it freed, a level reload).
        void drop(surface_memo_stats& stats) {
            if (!entries_.empty()) ++stats.host_drops;
            entries_.clear();
            environment_ = nullptr;
        }

        size_t size() const { return entries_.size(); }

        // Unit tests: make entry `index` stale (its memoized value xor `mask`).
        bool corrupt_for_test(size_t index, uint64_t mask) {
            if (index >= entries_.size()) return false;
            entries_[index].value ^= mask;
            return true;
        }

    private:
        struct entry {
            uint32_t entry_key = 0;
            const void* object = nullptr;
            const void* manager = nullptr;
            int manager_type = -1;
            const void* surface = nullptr;
            uint32_t surface_size = 0;
            uint32_t head_size = 0;
            unsigned char head[kHeadBytes] = {};
            std::string name;
            uint64_t value = 0;
        };

        static uint32_t head_size_of(const surface_memo_probe& p) {
            return p.surface ? static_cast<uint32_t>(p.surface_size < kHeadBytes ? p.surface_size : kHeadBytes) : 0u;
        }

        static bool matches(const entry& e, const surface_memo_probe& p) {
            if (e.manager != p.manager || e.manager_type != p.manager_type || e.surface != p.surface
                || e.surface_size != p.surface_size)
                return false;
            const uint32_t head = head_size_of(p);
            if (head != e.head_size || (head && std::memcmp(e.head, p.surface, head) != 0)) return false;
            return std::strcmp(e.name.c_str(), p.name) == 0;
        }

        static void fill(entry& e, const surface_memo_probe& p) {
            e.entry_key = p.entry_key;
            e.object = p.object;
            e.manager = p.manager;
            e.manager_type = p.manager_type;
            e.surface = p.surface;
            e.surface_size = p.surface_size;
            e.head_size = head_size_of(p);
            if (e.head_size) std::memcpy(e.head, p.surface, e.head_size);
            e.name = p.name;
        }

        const void* environment_ = nullptr;
        uint64_t generation_ = 0;
        std::vector<entry> entries_;
    };
}
