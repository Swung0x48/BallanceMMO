#pragma once

// SimTool --list-placement (docs/sector-placement-design.md 7.2 V1, step
// S1): the sector placement's row plan of a level as the world stands at its
// anchor, read-only.  Prints the level builder's Object Copy blocks (the
// chosen PH copier and the ignored ones), the dependency words the chosen one
// passes to CopyObjects, every PH row (sector, type, object, the matrix as %a
// floats and as its 16 words), the plan (one line per row), the same-pose
// rows, the rewritten share rows, the copies by kind and the applied table's
// changed rows.  Step S2: with --placement on the listing then applies the
// placement in the tool (list_placement_apply): every copy's census and
// fingerprint, the signature before and after, the name review of design 4,
// and with --placement-fail-at the V12 all-or-nothing comparison (the world
// before the refused apply against the world after it).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <entity/session.hpp>
#include <game/sector_placement.hpp>
#include <session/placement_plan.hpp>

#include "CKAll.h"

namespace bmmo::sim {

    // The kind of a PH type, for the design's table (1.4): extra, trafo, fan
    // (P_Modul_18), module (any other P_Modul_) or prop.
    inline const char* placement_type_kind(const std::string& type) {
        if (type.rfind("P_Extra_", 0) == 0) return "extra";
        if (type.rfind("P_Trafo_", 0) == 0) return "trafo";
        if (type == "P_Modul_18") return "fan";
        if (type.rfind("P_Modul_", 0) == 0) return "module";
        return "prop";
    }

    // Prints the listing; 0 when exactly one builder copy block is chosen and
    // the plan has no error, 3 otherwise (printed as such).
    inline int list_placement(CKContext* context, int level) {
        std::printf("placement listing: level %d\n", level);
        int result = 0;

        std::string block_error;
        const std::vector<game::builder_copy_block> blocks = game::find_builder_copy_blocks(context, block_error);
        CKBehavior* chosen = nullptr;
        for (const game::builder_copy_block& b: blocks) {
            std::printf("%s copy block: %s/Object Copy (id %u) -> %s\n", b.chosen ? "builder" : "ignored",
                        b.graph.c_str(), static_cast<unsigned>(b.block->GetID()),
                        b.out_target.empty() ? "(nothing)" : b.out_target.c_str());
            if (b.chosen) chosen = b.block;
        }
        if (!block_error.empty()) {
            std::printf("BLOCK ERROR: %s\n", block_error.c_str());
            chosen = nullptr;
            result = 3;
        }
        if (chosen) std::printf("%s", game::describe_copy_dependencies(chosen).c_str());

        std::vector<session::placement_row> rows;
        const std::string read_error = game::read_placement_rows(context, rows);
        if (!read_error.empty()) {
            std::printf("PH ERROR: %s\n", read_error.c_str());
            std::fflush(stdout);
            return 3;
        }
        std::printf("PH rows: %zu\n", rows.size());
        for (const session::placement_row& r: rows) {
            float m[16];
            std::memcpy(m, r.matrix, sizeof(m));
            std::printf("ph %d s%d %s %s [%a,%a,%a,%a][%a,%a,%a,%a][%a,%a,%a,%a][%a,%a,%a,%a] words=", r.row, r.sector,
                        r.type.c_str(), r.object.empty() ? "-" : r.object.c_str(),
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            for (int k = 0; k < 16; ++k) std::printf("%s%08x", k ? "," : "", static_cast<unsigned>(r.matrix[k]));
            std::printf("\n");
        }

        const session::placement_plan plan = session::plan_placements(rows, bmmo::game::kPlacementMaxName);
        std::map<std::string, int> by_kind;
        size_t longest = 0;
        std::string same_pose, rewritten;
        for (size_t i = 0; i < plan.actions.size(); ++i) {
            const session::placement_action& a = plan.actions[i];
            std::printf("plan %s\n", session::describe_placement_action(a, rows[i].type).c_str());
            if (a.what == session::placement_action::kind::copy) {
                ++by_kind[placement_type_kind(rows[i].type)];
                longest = std::max(longest, a.name.size());
            }
            if (a.what == session::placement_action::kind::share) {
                same_pose += (same_pose.empty() ? "" : ", ") + std::string("r") + std::to_string(a.row) + " = r"
                           + std::to_string(a.holder_row) + " (" + a.source + ")";
                if (a.name != a.source)
                    rewritten += (rewritten.empty() ? "" : ", ") + std::string("r") + std::to_string(a.row) + " "
                               + a.source + " -> " + a.name;
            }
        }
        std::printf("plan copies by kind: extra %d / trafo %d / fan %d / module %d / prop %d\n", by_kind["extra"],
                    by_kind["trafo"], by_kind["fan"], by_kind["module"], by_kind["prop"]);
        std::printf("plan same-pose rows: %s\n", same_pose.empty() ? "-" : same_pose.c_str());
        std::printf("plan rewritten share rows: %s\n", rewritten.empty() ? "-" : rewritten.c_str());
        std::printf("plan longest copy name: %zu (bound %zu)\n", longest, bmmo::game::kPlacementMaxName);
        const std::vector<session::placement_row> applied = session::placement_applied_rows(rows, plan);
        for (size_t i = 0; i < applied.size(); ++i)
            if (applied[i].object != rows[i].object)
                std::printf("applied row %d: %s -> %s\n", applied[i].row, rows[i].object.c_str(), applied[i].object.c_str());
        // The plan of the applied table must be all keep/share with nothing
        // to rewrite (design 2.2 "Idempotence").
        const session::placement_plan again = session::plan_placements(applied, bmmo::game::kPlacementMaxName);
        std::printf("applied table replanned: %zu copies, %zu rewrites%s\n", again.copies(), again.rewrites(),
                    again.error.empty() ? "" : (" ERROR " + again.error).c_str());
        if (!plan.error.empty()) {
            std::printf("PLAN ERROR: %s\n", plan.error.c_str());
            result = 3;
        }
        std::printf("placement: level %d, %zu rows, %zu copies, %zu shared (%zu rewritten)%s\n", level, rows.size(),
                    plan.copies(), plan.shares(), plan.rewrites(), result ? " FAILED" : "");
        std::fflush(stdout);
        return result;
    }

    // ------------------------------------------------------------ step S2

    // What a refused apply must leave as it found (design 7.2 V12): the PH
    // table's objects, every attribute type's object list, every group's
    // member list, and the names of every object in the world.
    struct placement_inventory {
        std::vector<std::string> ph;                              // "row id name" per row
        std::map<std::string, std::vector<CK_ID>> attributes;     // "name#type" -> holders, in list order
        std::map<std::string, std::vector<CK_ID>> groups;         // "name#id" -> members, in list order
        std::vector<std::string> names;                           // every object's name, sorted
    };

    inline placement_inventory capture_placement_inventory(CKContext* context) {
        placement_inventory out;
        if (CKDataArray* ph = game::placement_detail::ph_array(context))
            for (int r = 0; r < ph->GetRowCount(); ++r) {
                CKObject* o = ph->GetElementObject(r, 3);
                out.ph.push_back(std::to_string(r) + " " + std::to_string(o ? o->GetID() : 0) + " "
                                 + game::placement_detail::name_of(o));
            }
        if (CKAttributeManager* am = context->GetAttributeManager())
            for (int t = 0; t < am->GetAttributeCount(); ++t) {
                const char* n = am->GetAttributeNameByType(t);
                std::vector<CK_ID>& list = out.attributes[std::string(n ? n : "?") + "#" + std::to_string(t)];
                const XObjectPointerArray& holders = am->GetAttributeListPtr(t);
                for (int i = 0; i < holders.Size(); ++i) list.push_back(holders[i] ? holders[i]->GetID() : 0);
            }
        const XObjectPointerArray& groups = context->GetObjectListByType(CKCID_GROUP, TRUE);
        for (int g = 0; g < groups.Size(); ++g) {
            CKGroup* group = CKGroup::Cast(groups[g]);
            if (!group) continue;
            std::vector<CK_ID>& members = out.groups[game::placement_detail::name_of(group) + "#" + std::to_string(group->GetID())];
            for (int i = 0; i < group->GetObjectCount(); ++i) {
                CKBeObject* m = group->GetObject(i);
                members.push_back(m ? m->GetID() : 0);
            }
        }
        const XObjectPointerArray& all = context->GetObjectListByType(CKCID_OBJECT, TRUE);
        for (int i = 0; i < all.Size(); ++i)
            if (CKObject* o = all[i]) out.names.push_back(game::placement_detail::name_of(o));
        std::sort(out.names.begin(), out.names.end());
        return out;
    }

    // One line per part; true when all four are equal.
    inline bool compare_placement_inventory(const placement_inventory& a, const placement_inventory& b, const char* label) {
        size_t holders = 0, members = 0;
        for (const auto& [n, l]: a.attributes) holders += l.size();
        for (const auto& [n, l]: a.groups) members += l.size();
        const bool ph = a.ph == b.ph, attributes = a.attributes == b.attributes, groups = a.groups == b.groups,
                   names = a.names == b.names;
        std::printf("%s PH table: %s (%zu rows)\n", label, ph ? "equal" : "DIFFERS", a.ph.size());
        std::printf("%s attribute lists: %s (%zu types, %zu holders)\n", label, attributes ? "equal" : "DIFFERS",
                    a.attributes.size(), holders);
        std::printf("%s group member lists: %s (%zu groups, %zu members)\n", label, groups ? "equal" : "DIFFERS",
                    a.groups.size(), members);
        std::printf("%s object names: %s (%zu vs %zu objects)\n", label, names ? "equal" : "DIFFERS", a.names.size(),
                    b.names.size());
        for (const auto& [n, l]: b.attributes) {
            auto it = a.attributes.find(n);
            if (it == a.attributes.end() || it->second != l)
                std::printf("  attribute %s: %zu -> %zu holders\n", n.c_str(), it == a.attributes.end() ? size_t(0) : it->second.size(),
                            l.size());
        }
        for (const auto& [n, l]: b.groups) {
            auto it = a.groups.find(n);
            if (it == a.groups.end() || it->second != l)
                std::printf("  group %s: %zu -> %zu members\n", n.c_str(), it == a.groups.end() ? size_t(0) : it->second.size(),
                            l.size());
        }
        return ph && attributes && groups && names;
    }

    // Design 4's name review, on the placed world: every 3D entity of every
    // PH row's hierarchy - the bodies' names, which put_back_birth,
    // destroy_body, local_birth_log, the recipe lines and joints_on look up
    // by name (first match) - must be the only 3D entity of its name.
    // Prints the count and every duplicate; returns how many.
    inline size_t review_placement_names(CKContext* context) {
        std::map<std::string, int> entity_names;
        const XObjectPointerArray& all = context->GetObjectListByType(CKCID_3DENTITY, TRUE);
        for (int i = 0; i < all.Size(); ++i)
            if (CKObject* o = all[i]) ++entity_names[game::placement_detail::name_of(o)];
        CKDataArray* ph = game::placement_detail::ph_array(context);
        std::set<std::string> checked, duplicates;
        size_t longest = 0;
        for (int r = 0; ph && r < ph->GetRowCount(); ++r)
            for (CK3dEntity* e: game::placement_detail::hierarchy_of(CK3dEntity::Cast(ph->GetElementObject(r, 3)))) {
                const std::string n = game::placement_detail::name_of(e);
                longest = std::max(longest, n.size());
                if (!checked.insert(n).second) continue;
                if (n.empty() || entity_names[n] != 1) duplicates.insert(n);
            }
        std::printf("name review: %zu distinct entity names across the PH rows' hierarchies, longest %zu (MAX_NAME %zu), "
                    "%zu not unique among the 3D entities\n", checked.size(), longest, session::MAX_NAME, duplicates.size());
        for (const std::string& n: duplicates) std::printf("  not unique: '%s' x%d\n", n.c_str(), entity_names[n]);
        return duplicates.size();
    }

    // --list-placement --placement on: the placement applied in the tool on
    // the listed (unplaced) world.  Exit 0, or 3 when the apply refused
    // without a --placement-fail-at, V12 failed, or a name is not unique.
    inline int list_placement_apply(CKContext* context, const game::placement_hooks& hooks) {
        game::sector_placement placement;
        std::string signature_error;
        const uint64_t off = placement.signature(context, &signature_error);
        std::printf("signature (placement off): %016llx%s\n", static_cast<unsigned long long>(off), signature_error.c_str());
        const placement_inventory before = capture_placement_inventory(context);
        const std::string refused = placement.apply(context, hooks);
        std::printf("%s", placement.describe().c_str());
        if (!refused.empty()) {
            std::printf("apply refused: %s\n", refused.c_str());
            const bool same = compare_placement_inventory(before, capture_placement_inventory(context), "V12");
            const uint64_t again = placement.signature(context);
            std::printf("signature after the refusal: %016llx (%s)\n", static_cast<unsigned long long>(again),
                        again == off ? "the unplaced table's" : "DIFFERS");
            std::string why;
            const bool holds = placement.holds_copies(context, &why);
            std::printf("holds_copies after the refusal: %s%s%s\n", holds ? "yes" : "no", why.empty() ? "" : " - ", why.c_str());
            const bool pass = hooks.fail_at_row >= 0 && same && again == off && !holds;
            std::printf("V12: %s\n", pass ? "PASS" : "FAIL");
            std::fflush(stdout);
            return pass ? 0 : 3;
        }
        for (const game::sector_placement::copy_record& c: placement.copies()) {
            std::printf("census row %d %s: %s\n", c.row, c.name.c_str(), c.census.c_str());
            std::printf("fingerprint row %d %s: %s\n", c.row, c.name.c_str(), c.fingerprint.c_str());
        }
        const uint64_t on = placement.signature(context);
        std::printf("signature (placement on): %016llx\n", static_cast<unsigned long long>(on));
        std::string why;
        const bool holds = placement.holds_copies(context, &why);
        std::printf("holds_copies: %s%s%s\n", holds ? "yes" : "no", why.empty() ? "" : " - ", why.c_str());
        const size_t duplicates = review_placement_names(context);
        // release puts the world back the way the refused apply must leave
        // it, and a second apply then makes the same copies with no clash.
        const int released = placement.release(context);
        std::printf("%s", placement.describe().c_str());
        const bool released_same = compare_placement_inventory(before, capture_placement_inventory(context), "release");
        why.clear();
        const bool holds_after = placement.holds_copies(context, &why);
        const uint64_t after_release = placement.signature(context);
        const std::string again = placement.apply(context, hooks);
        const uint64_t reapplied = placement.signature(context);
        const bool release_ok = released == static_cast<int>(placement.copies().size()) && released_same && !holds_after
                             && after_release == off && again.empty() && reapplied == on;
        std::printf("release check: %d copies released, inventory %s, holds_copies %s, signature %016llx (%s), "
                    "re-apply %s, signature %016llx (%s): %s\n", released, released_same ? "equal" : "DIFFERS",
                    holds_after ? "yes" : "no", static_cast<unsigned long long>(after_release),
                    after_release == off ? "unplaced" : "DIFFERS", again.empty() ? "ok" : again.c_str(),
                    static_cast<unsigned long long>(reapplied), reapplied == on ? "same" : "DIFFERS", release_ok ? "PASS" : "FAIL");
        std::fflush(stdout);
        return duplicates || !release_ok ? 3 : 0;
    }
}
