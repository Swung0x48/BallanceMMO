#pragma once

// The sector placement's row plan (docs/sector-placement-design.md 2.2,
// step S1): which PH row keeps its object, which shares an earlier row's
// placement and which gets its own copy, decided from the PH table as read at
// the anchor.  CK-free and header-only: the server, both clients, SimTool and
// BallanceMMOMessageTests compile it.  The CK step that executes a plan
// (game/sector_placement.hpp) is step S2.
//
// The rule it implements (the user's, design 1.3 I2): after the anchor every
// PH row names its own object, except a row whose matrix is bit-identical to
// an earlier placement of the same object (a same-pose pair, kept pooled),
// which names the same object as that earlier placement's holder row.  A
// sector's pass then moves no object another sector holds at another pose.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bmmo::session {

    // One PH row as read at the anchor.
    struct placement_row {
        int row = 0, sector = 0;
        std::string type;                 // column 1, Type Name
        std::string object;               // column 3's object name ("" = none)
        uint32_t matrix[16] = {};         // column 2, the 16 floats' bits (VxMatrix row-major)
    };

    struct placement_action {
        enum class kind : uint8_t { keep, share, copy, none };
        kind what = kind::keep;
        int row = 0, sector = 0;
        std::string source;               // the object the row names now ("" for none)
        // The object the row names once the plan is applied: keep - source;
        // copy - the new root's name; share - the holder's object (the
        // original, or an earlier row's copy); none - "".
        std::string name;
        int holder_row = -1;              // share: the row whose placement it shares
    };

    struct placement_plan {
        std::vector<placement_action> actions;   // one per row, in row order
        std::string error;                       // non-empty: fail closed (the host refuses to anchor)

        size_t copies() const {
            return static_cast<size_t>(std::count_if(actions.begin(), actions.end(),
                [](const placement_action& a) { return a.what == placement_action::kind::copy; }));
        }
        size_t shares() const {
            return static_cast<size_t>(std::count_if(actions.begin(), actions.end(),
                [](const placement_action& a) { return a.what == placement_action::kind::share; }));
        }
        // Share rows whose holder object differs from the object they name
        // now (L1 row 38 -> P_Trafo_Wood_MF_R37): the CK step rewrites them.
        size_t rewrites() const {
            return static_cast<size_t>(std::count_if(actions.begin(), actions.end(),
                [](const placement_action& a) {
                    return a.what == placement_action::kind::share && a.name != a.source;
                }));
        }
    };

    // One 3D entity of a row's final hierarchy, for the signature.
    struct entity_print {
        std::string name, mesh;           // mesh: the current mesh's name, "" if none
        int class_id = 0, scripts = 0;
        std::vector<std::string> attributes;     // attribute type NAMES (sorted by the signature)
    };

    // The suffix of a copy made for `row`: keeps the P_/PE_ prefix (the
    // script selection's rule), uses [A-Za-z0-9_] only, and cannot collide
    // with a builder copy's name (those end in three digits directly).
    inline std::string placement_suffix(int row) { return "_R" + std::to_string(row); }

    inline const char* placement_kind_name(placement_action::kind k) {
        switch (k) {
            case placement_action::kind::keep: return "keep";
            case placement_action::kind::share: return "share";
            case placement_action::kind::copy: return "copy";
            case placement_action::kind::none: return "none";
        }
        return "?";
    }

    // The plan of a PH table (design 2.2 "Algorithm").  Rows are walked in the
    // order given (the table's row order); `max_name` is the longest name a
    // copy may get - every host passes game::kPlacementMaxName (63: the
    // wire's MAX_NAME and the bridge's body name, step S5): a copy's root
    // name longer than it is an error, max_name itself is allowed (the names
    // the level already has are not the plan's).  Two
    // objects of one plan with the same name - a copy's name equal to another
    // copy's or to an object the table already names - are an error.  On an
    // error the actions are still filled as far as they were planned, for the
    // log; a host must not apply them.
    inline placement_plan plan_placements(const std::vector<placement_row>& rows, size_t max_name) {
        struct placement {
            const uint32_t* matrix;
            int holder_row;
            std::string holder_object;
        };
        placement_plan plan;
        plan.actions.reserve(rows.size());
        std::map<std::string, std::vector<placement>> placed;   // object -> its placements, in row order
        // The names the table already holds: every object a row names.
        std::set<std::string> existing;
        for (const placement_row& r: rows)
            if (!r.object.empty()) existing.insert(r.object);
        std::set<std::string> made;
        const auto fail = [&plan](const std::string& why) { if (plan.error.empty()) plan.error = why; };

        for (const placement_row& r: rows) {
            placement_action a;
            a.row = r.row;
            a.sector = r.sector;
            a.source = r.object;
            if (r.object.empty()) {
                a.what = placement_action::kind::none;
                plan.actions.push_back(std::move(a));
                continue;
            }
            std::vector<placement>& list = placed[r.object];
            if (list.empty()) {
                a.what = placement_action::kind::keep;
                a.name = r.object;
                list.push_back({r.matrix, r.row, r.object});
                plan.actions.push_back(std::move(a));
                continue;
            }
            const placement* same = nullptr;
            for (const placement& p: list)
                if (std::memcmp(p.matrix, r.matrix, sizeof(r.matrix)) == 0) { same = &p; break; }
            if (same) {
                a.what = placement_action::kind::share;
                a.name = same->holder_object;
                a.holder_row = same->holder_row;
                plan.actions.push_back(std::move(a));
                continue;
            }
            a.what = placement_action::kind::copy;
            a.name = r.object + placement_suffix(r.row);
            if (a.name.size() > max_name)
                fail("row " + std::to_string(r.row) + ": copy name " + a.name + " (" + std::to_string(a.name.size())
                     + " characters) is longer than " + std::to_string(max_name));
            if (existing.count(a.name) || !made.insert(a.name).second)
                fail("row " + std::to_string(r.row) + ": copy name " + a.name + " is already taken");
            list.push_back({r.matrix, r.row, a.name});
            plan.actions.push_back(std::move(a));
        }
        return plan;
    }

    // The table the CK step leaves (design 2.3 step 10), modelled: every row
    // names its action's `name`.  Rows of a failed plan are returned as given.
    inline std::vector<placement_row> placement_applied_rows(const std::vector<placement_row>& rows,
                                                             const placement_plan& plan) {
        std::vector<placement_row> out = rows;
        if (!plan.error.empty() || plan.actions.size() != rows.size()) return out;
        for (size_t i = 0; i < out.size(); ++i)
            if (plan.actions[i].what != placement_action::kind::none) out[i].object = plan.actions[i].name;
        return out;
    }

    // One log line per action: "row 15 s2 P_Modul_30 copy P_Modul_30_MF -> P_Modul_30_MF_R15".
    inline std::string describe_placement_action(const placement_action& a, const std::string& type = {}) {
        std::string line = "row " + std::to_string(a.row) + " s" + std::to_string(a.sector);
        if (!type.empty()) line += " " + type;
        line += std::string(" ") + placement_kind_name(a.what);
        switch (a.what) {
            case placement_action::kind::none: break;
            case placement_action::kind::keep: line += " " + a.source; break;
            case placement_action::kind::copy: line += " " + a.source + " -> " + a.name; break;
            case placement_action::kind::share:
                line += " " + a.source + " with row " + std::to_string(a.holder_row);
                if (a.name != a.source) line += " -> " + a.name + " (rewritten)";
                break;
        }
        return line;
    }

    namespace placement_detail {
        struct fnv64 {
            uint64_t h = 1469598103934665603ull;
            void byte(uint8_t b) { h ^= b; h *= 1099511628211ull; }
            void u32(uint32_t v) { for (int k = 0; k < 4; ++k) byte(static_cast<uint8_t>(v >> (8 * k))); }
            void i32(int v) { u32(static_cast<uint32_t>(v)); }
            void str(const std::string& s) {
                u32(static_cast<uint32_t>(s.size()));
                for (char c: s) byte(static_cast<uint8_t>(c));
            }
        };
    }

    // The placement signature (design 2.2 "Signature"): FNV-1a 64 over, in
    // row order, the row, its sector, its type, the object it names, the 16
    // matrix words and the fingerprint of that object's hierarchy as it
    // stands; integers as 4 little-endian bytes, strings as their length
    // (4 bytes) then their bytes.  The fingerprint is order-free: the
    // entity_prints are sorted by name (then by every other field), each one's
    // attribute names sorted.  `hierarchies` is parallel to `final_rows`; a
    // missing entry (or a row naming no object) hashes as an empty hierarchy.
    inline uint64_t placement_signature(const std::vector<placement_row>& final_rows,
                                        const std::vector<std::vector<entity_print>>& hierarchies) {
        placement_detail::fnv64 f;
        f.u32(static_cast<uint32_t>(final_rows.size()));
        for (size_t i = 0; i < final_rows.size(); ++i) {
            const placement_row& r = final_rows[i];
            f.i32(r.row);
            f.i32(r.sector);
            f.str(r.type);
            f.str(r.object);
            for (uint32_t w: r.matrix) f.u32(w);
            std::vector<entity_print> prints = i < hierarchies.size() ? hierarchies[i] : std::vector<entity_print>{};
            for (entity_print& p: prints) std::sort(p.attributes.begin(), p.attributes.end());
            std::sort(prints.begin(), prints.end(), [](const entity_print& a, const entity_print& b) {
                if (a.name != b.name) return a.name < b.name;
                if (a.class_id != b.class_id) return a.class_id < b.class_id;
                if (a.mesh != b.mesh) return a.mesh < b.mesh;
                if (a.scripts != b.scripts) return a.scripts < b.scripts;
                return a.attributes < b.attributes;
            });
            f.u32(static_cast<uint32_t>(prints.size()));
            for (const entity_print& p: prints) {
                f.str(p.name);
                f.i32(p.class_id);
                f.str(p.mesh);
                f.i32(p.scripts);
                f.u32(static_cast<uint32_t>(p.attributes.size()));
                for (const std::string& a: p.attributes) f.str(a);
            }
        }
        return f.h;
    }
}
