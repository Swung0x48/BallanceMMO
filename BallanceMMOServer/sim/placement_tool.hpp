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
    // --placement-policy-check (FULL close-out step H2, plan row SP6-c; design
    // 2.4 "Option off", As built S6/S10): no live session reaches the option-off
    // release branch or the scene refusal - the headless client releases its
    // copies at begin_session, before an engine reboot and in its destructor,
    // and the server's world is made fresh per session and releases in its
    // destructor, so prepare_unplaced always meets a level without copies there.
    // This exercises the shared code on a level that holds copies, in the tool:
    //   policy   the registry that made the copies: prepare_unplaced releases
    //            them and returns true; the world equals the unplaced one
    //            (inventory, signature, holds_copies no, no object of a copy's
    //            name left - "--list-placement after");
    //   refuse   another registry (a level whose copies this process did not
    //            record, e.g. a retail level not reloaded): holds_copies by the
    //            PH rows' "_R<digits>" names, its release cannot clear them, so
    //            prepare_unplaced returns false with the error the hosts log
    //            ("the level still holds placement copies (row N names ...)");
    //            the placed world is left as it was;
    //   scene    apply with a scene other than the level scene current: the
    //            refusal "the current scene is not the level scene", nothing made.
    // `placement` holds the re-applied copies on entry; the world is unplaced
    // on exit.  Returns true when all three PASS.
    inline bool list_placement_policy_check(CKContext* context, const game::placement_hooks& hooks,
                                            game::sector_placement& placement, const placement_inventory& before,
                                            uint64_t off, uint64_t on) {
        const game::placement_log log = [](const std::string& line) { std::printf("policy log: %s\n", line.c_str()); };
        std::vector<std::string> copy_names;
        for (const game::sector_placement::copy_record& c: placement.copies()) copy_names.push_back(c.name);
        const auto copies_left = [&]() {
            size_t left = 0;
            for (const std::string& n: copy_names)
                if (context->GetObjectByName(const_cast<CKSTRING>(n.c_str()))) ++left;
            return left;
        };
        std::string why;
        bool all = true;

        // refuse: a registry that did not make the copies.
        {
            const size_t held = copy_names.size(), present = copies_left();
            game::sector_placement other;
            std::string error;
            const bool ok = game::prepare_unplaced(other, context, log, error);
            const uint64_t after = placement.signature(context);
            why.clear();
            const bool holds = placement.holds_copies(context, &why);
            const bool pass = !ok && error.rfind("the level still holds placement copies (row ", 0) == 0 && after == on
                           && holds && copies_left() == present && present == held && held > 0;
            std::printf("policy refuse: a registry that made none, the level holding %zu copies (%zu present): returned %s, "
                        "error '%s', signature %016llx (%s), holds_copies %s, copies present %zu: %s\n", held, present,
                        ok ? "true" : "false", error.c_str(), static_cast<unsigned long long>(after),
                        after == on ? "the placed table's" : "DIFFERS", holds ? "yes" : "no", copies_left(),
                        pass ? "PASS" : "FAIL");
            all = all && pass;
        }
        // policy: the registry that made them.
        {
            std::string error;
            const bool ok = game::prepare_unplaced(placement, context, log, error);
            const bool same = compare_placement_inventory(before, capture_placement_inventory(context), "policy");
            const uint64_t after = placement.signature(context);
            why.clear();
            const bool holds = placement.holds_copies(context, &why);
            const size_t left = copies_left();
            const bool pass = ok && error.empty() && same && after == off && !holds && left == 0;
            std::printf("policy release: the registry that made %zu copies: returned %s%s%s, inventory %s, signature "
                        "%016llx (%s), holds_copies %s%s%s, objects of a copy's name left %zu: %s\n", copy_names.size(),
                        ok ? "true" : "false", error.empty() ? "" : " - ", error.c_str(), same ? "equal" : "DIFFERS",
                        static_cast<unsigned long long>(after), after == off ? "unplaced" : "DIFFERS", holds ? "yes" : "no",
                        why.empty() ? "" : " - ", why.c_str(), left, pass ? "PASS" : "FAIL");
            all = all && pass;
        }
        // scene: a scene other than the level scene is current.
        {
            CKLevel* level = context->GetCurrentLevel();
            CKScene* level_scene = level ? level->GetLevelScene() : nullptr;
            CKScene* current = context->GetCurrentScene();
            auto* other = static_cast<CKScene*>(context->CreateObject(CKCID_SCENE, const_cast<CKSTRING>("BMMO_H2_policy_scene")));
            bool pass = false;
            if (level && level_scene && current == level_scene && other) {
                level->AddScene(other);
                // DONOTHING both ways: the switch must not touch any object's activity.
                level->LaunchScene(other, CK_SCENEOBJECTACTIVITY_DONOTHING, CK_SCENEOBJECTRESET_DONOTHING);
                const bool switched = context->GetCurrentScene() == other;
                const placement_inventory at = capture_placement_inventory(context);
                game::sector_placement fresh;
                const std::string refused = fresh.apply(context, hooks);
                const bool same = compare_placement_inventory(at, capture_placement_inventory(context), "scene");
                const uint64_t after = fresh.signature(context);
                why.clear();
                const bool holds = fresh.holds_copies(context, &why);
                pass = switched && refused.rfind("the current scene is not the level scene", 0) == 0 && same && after == off
                    && !holds && fresh.copies().empty() && copies_left() == 0;
                std::printf("policy scene: current scene %s (the level scene: %s), apply refused '%s', inventory %s, "
                            "signature %016llx (%s), holds_copies %s, copies made %zu: %s\n",
                            game::placement_detail::name_of(context->GetCurrentScene()).c_str(), switched ? "no" : "STILL",
                            refused.c_str(), same ? "equal" : "DIFFERS", static_cast<unsigned long long>(after),
                            after == off ? "unplaced" : "DIFFERS", holds ? "yes" : "no", fresh.copies().size(),
                            pass ? "PASS" : "FAIL");
                level->LaunchScene(level_scene, CK_SCENEOBJECTACTIVITY_DONOTHING, CK_SCENEOBJECTRESET_DONOTHING);
                const bool back = context->GetCurrentScene() == level_scene;
                // Leave no residue: the test scene out of the level and destroyed.
                const CKERROR removed = level->RemoveScene(other);
                const CKERROR destroyed = context->DestroyObject(other);
                const bool gone = !context->GetObjectByName(const_cast<CKSTRING>("BMMO_H2_policy_scene"));
                std::printf("policy scene: the level scene current again: %s; the test scene removed %d, destroyed %d, "
                            "gone %s\n", back ? "yes" : "NO", static_cast<int>(removed), static_cast<int>(destroyed),
                            gone ? "yes" : "NO");
                pass = pass && back && removed == CK_OK && destroyed == CK_OK && gone;
                std::printf("policy scene (with the clean-up): %s\n", pass ? "PASS" : "FAIL");
            } else {
                if (other) context->DestroyObject(other);
                std::printf("policy scene: no level scene to leave (level %p, level scene %p, current %p, made %p): FAIL\n",
                            static_cast<void*>(level), static_cast<void*>(level_scene), static_cast<void*>(current),
                            static_cast<void*>(other));
            }
            all = all && pass;
        }
        std::printf("placement policy check: %s\n", all ? "PASS" : "FAIL");
        std::fflush(stdout);
        return all;
    }

    inline int list_placement_apply(CKContext* context, const game::placement_hooks& hooks, bool policy_check = false) {
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
        // Step S2 of the close-out (FULL plan row SP10-g, SP10's review nit
        // on design step 8): an object-valued attribute of a copy that names
        // an object the copy did not make.  Step 8 remaps a value naming a
        // member of the hierarchy to its copy; any other value keeps naming
        // the original's target (an object outside the hierarchy, shared by
        // the original and its copies).  Counted after the apply; expected 0.
        // The positive control (S2's review): the same filter over every
        // behavioural object of the level, so a 0 for the copies is read
        // against what the filter finds at all; the value is read without
        // an update (GetReadDataPtr(FALSE)), the listing stays read-only.
        {
            CKParameterManager* pm = context->GetParameterManager();
            CKAttributeManager* am = context->GetAttributeManager();
            // The target an object-valued attribute of `o` of `type` names, or null.
            const auto object_target = [&](CKBeObject* o, CKAttributeType type) -> CKObject* {
                CKParameterOut* p = o->GetAttributeParameter(type);
                if (!p || !pm || !pm->IsDerivedFrom(p->GetGUID(), CKPGUID_OBJECT)) return nullptr;
                const void* data = p->GetReadDataPtr(FALSE);
                const CK_ID value = data ? *static_cast<const CK_ID*>(data) : 0;
                return value ? context->GetObject(value) : nullptr;
            };
            size_t object_types = 0;
            std::string type_names;
            if (am && pm)
                for (int t = 0; t < am->GetAttributeCount(); ++t)
                    if (pm->IsDerivedFrom(am->GetAttributeParameterGUID(t), CKPGUID_OBJECT)) {
                        ++object_types;
                        const char* n = am->GetAttributeNameByType(t);
                        type_names += std::string(type_names.empty() ? "" : ", ") + (n ? n : "?") + " ("
                                    + std::to_string(am->GetAttributeListPtr(t).Size()) + " holder(s))";
                    }
            size_t level_holders = 0, level_valued = 0, level_object_attributes = 0;
            const XObjectPointerArray& all = context->GetObjectListByType(CKCID_BEOBJECT, TRUE);
            for (int i = 0; i < all.Size(); ++i) {
                CKBeObject* o = CKBeObject::Cast(all[i]);
                if (!o) continue;
                bool holds = false;
                for (int k = 0; k < o->GetAttributeCount(); ++k) {
                    CKParameterOut* p = o->GetAttributeParameter(o->GetAttributeType(k));
                    if (p && pm && pm->IsDerivedFrom(p->GetGUID(), CKPGUID_OBJECT)) ++level_object_attributes;
                    if (object_target(o, o->GetAttributeType(k))) { ++level_valued; holds = true; }
                }
                if (holds) ++level_holders;
            }
            size_t object_valued = 0, outside = 0;
            for (const game::sector_placement::copy_record& c: placement.copies()) {
                std::set<CK_ID> made;
                for (const auto& [id, cls]: c.objects) made.insert(id);
                for (const auto& [id, cls]: c.objects) {
                    CKBeObject* o = CKBeObject::Cast(context->GetObject(id));
                    if (!o) continue;
                    for (int k = 0; k < o->GetAttributeCount(); ++k) {
                        const CKAttributeType type = o->GetAttributeType(k);
                        CKObject* target = object_target(o, type);
                        if (!target) continue;
                        ++object_valued;
                        if (made.count(target->GetID())) continue;
                        ++outside;
                        const char* an = am ? am->GetAttributeNameByType(type) : nullptr;
                        std::printf("object attribute outside the hierarchy: row %d %s: %s's %s names %s (id %lu)\n", c.row,
                                    c.name.c_str(), o->GetName() ? o->GetName() : "?", an ? an : "?",
                                    target->GetName() ? target->GetName() : "?", static_cast<unsigned long>(target->GetID()));
                    }
                }
            }
            std::printf("object attributes naming outside the hierarchy: %zu (object-valued attributes with a target: %zu, "
                        "copies %zu)\n", outside, object_valued, placement.copies().size());
            std::printf("object attributes, the filter's control over the whole level: %zu object-valued attribute type(s)"
                        " registered [%s]; %zu object-valued attribute(s) held, %zu of them with a target, on %zu of %d"
                        " behavioural object(s)\n", object_types, type_names.c_str(), level_object_attributes, level_valued,
                        level_holders, all.Size());
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
        // Step H2: the option-off policy and the scene refusal on the re-applied world.
        const bool policy_ok = !policy_check || (again.empty() && list_placement_policy_check(context, hooks, placement, before, off, on));
        return duplicates || !release_ok || !policy_ok ? 3 : 0;
    }
}
