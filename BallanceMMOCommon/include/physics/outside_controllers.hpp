#pragma once

// The outside force controllers on the own core (rollback-engine-gaps design
// 9.3, option (b); FULL step DX3).
//
// Gameplay_Ingame's SetPhysicsForce blocks (the navigation leaves and the
// debug leaf) run outside the script replay's selection: their IVP
// controllers ("rt:force/<block id>", engine change #18) are live state that
// no script record carries.  After a replay the own core's controller list
// can therefore differ from the live run's - a revived or remade own core
// lacks the controller the live core had, or a core carries one made after
// the anchor - while every body state is the same (L-S3, "As built (step
// S3)").  The user chose (b) on 2026-10-29: the identity tools name this one
// difference instead of failing on it, and it stays recorded.  It is safe
// only while those controllers push nothing: retail navigation's force
// leaves are zeroed (the host's zero_retail_forces) and the input reaches
// the ball through the bridge's own "bmmo:force/<leaf>".  The savestate blob
// records a controller's name and priority but not its force, so that
// precondition is checked at run time (the core's per-tick check over the
// host's outside_forces readings), not from a blob.
//
// Header-only and free of IVP and CK: the blob diff (physics_savestate),
// the core and the unit tests share these decisions.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bmmo::physics::outside_controllers {

    // The named kind, as the tools and the logs print it.
    constexpr const char* kind_name = "outside zero-force controller on the own core";

    // A physics_RT force controller (engine change #18: "rt:force/<behavior
    // id>"); the bridge's own navigation leaves are "bmmo:force/<leaf>".
    inline bool is_force_controller_name(const std::string& name) {
        static const char prefix[] = "rt:force/";
        return name.compare(0, sizeof(prefix) - 1, prefix) == 0 && name.size() > sizeof(prefix) - 1;
    }

    // Gameplay_Ingame's SetPhysicsForce blocks as the design lists them
    // (rollback-engine-gaps design §1.1, Q-S3: the same ids in L1-L12, the
    // original L13 and the arena).  A host reads its own list from the
    // script (game::outside_force_blocks); this one is the tools' default,
    // where no script is loaded (SimTool --savestate-diff).
    inline const std::vector<uint32_t>& default_outside_force_blocks() {
        static const std::vector<uint32_t> blocks = {26354, 26392, 26414, 26432, 26447};
        return blocks;
    }

    // The controller names those blocks give their controllers: the only
    // names the named difference may cover.  An "rt:force/" controller of
    // any other block (a level's fan, P_Modul_18's 46475; an explosion's
    // force) carries a real force and is never named.
    inline std::vector<std::string> controller_names(const std::vector<uint32_t>& blocks) {
        std::vector<std::string> names;
        for (uint32_t id: blocks) names.push_back("rt:force/" + std::to_string(id));
        return names;
    }

    // Do two controller lists (a core's controllers_of_core, or a unit's
    // controller_cores, in list order) differ ONLY by the presence of force
    // controllers?  `Entry` has `kind` (1 = a controller kept by name) and
    // `name`; `same(x, y)` compares two entries field by field.  The
    // controllers present on one side only (by name, counted) must all be
    // kind 1 with an "rt:force/" name - and, when `allowed` is given, among
    // its names (the savestate diff always gives it: the outside blocks'
    // controller_names on a core, the names found on the cores on a unit);
    // with them taken out, the two lists must be identical in
    // order and in every field.  `only_a` / `only_b` get those names (sorted).
    // Equal lists, or lists that differ in anything else (another
    // controller, an order, a field), return false.
    template <class Entry, class Same>
    bool force_only_delta(const std::vector<Entry>& a, const std::vector<Entry>& b, Same same,
                          std::vector<std::string>& only_a, std::vector<std::string>& only_b,
                          const std::vector<std::string>* allowed = nullptr) {
        only_a.clear();
        only_b.clear();
        std::map<std::string, int> count;   // > 0: more on a, < 0: more on b
        for (const Entry& e: a) ++count[e.name];
        for (const Entry& e: b) --count[e.name];
        for (const auto& [name, n]: count) {
            if (n == 0) continue;
            if (!is_force_controller_name(name)) return false;
            if (allowed) {
                bool listed = false;
                for (const std::string& x: *allowed) listed = listed || x == name;
                if (!listed) return false;
            }
            for (int k = 0; k < (n > 0 ? n : -n); ++k) (n > 0 ? only_a : only_b).push_back(name);
        }
        if (only_a.empty() && only_b.empty()) return false;
        // Take each side's surplus out (the LAST entries of a name, so a
        // name present on both sides keeps its first places), then compare.
        const auto strip = [&count](const std::vector<Entry>& list, bool side_a) {
            std::map<std::string, int> left;
            for (const auto& [name, n]: count)
                if (side_a ? n > 0 : n < 0) left[name] = side_a ? n : -n;
            std::vector<const Entry*> kept;
            for (size_t i = list.size(); i-- > 0;) {
                const auto it = left.find(list[i].name);
                if (it != left.end() && it->second > 0) {
                    if (list[i].kind != 1) return std::vector<const Entry*>{nullptr};   // a handle under that name
                    --it->second;
                    continue;
                }
                kept.push_back(&list[i]);
            }
            return std::vector<const Entry*>(kept.rbegin(), kept.rend());
        };
        const std::vector<const Entry*> ka = strip(a, true), kb = strip(b, false);
        if ((!ka.empty() && !ka[0]) || (!kb.empty() && !kb[0]) || ka.size() != kb.size()) return false;
        for (size_t i = 0; i < ka.size(); ++i)
            if (!same(*ka[i], *kb[i])) return false;
        return true;
    }

    // One outside SetPhysicsForce block as the host reads it (the bridge's
    // force_block_get, API v14): whether it holds an IVP controller now, and
    // the force (object space) that controller was built with.
    struct force_reading {
        uint32_t block = 0;
        bool exists = false;
        double force[3] = {0.0, 0.0, 0.0};
    };

    // The precondition of option (b): a controller that exists pushes
    // nothing.  Exactly zero - the leaves are zeroed, not small.
    inline bool carries_force(const force_reading& r) {
        return r.exists && (r.force[0] != 0.0 || r.force[1] != 0.0 || r.force[2] != 0.0);
    }

    // Reads `blocks` through a physics backend with force_block_get
    // (physics_direct, the mod's physics_view: the same method on both).  A
    // block the backend cannot read is left out.
    template <class Backend, class ForceBlock>
    void read_forces(const Backend& backend, const std::vector<uint32_t>& blocks, std::vector<force_reading>& out) {
        out.clear();
        for (uint32_t id: blocks) {
            ForceBlock state{};
            std::string error;
            if (!backend.force_block_get(id, state, error)) continue;
            force_reading r;
            r.block = id;
            r.exists = state.exists != 0;
            for (int k = 0; k < 3; ++k) r.force[k] = state.force[k];
            out.push_back(r);
        }
    }
}
