#pragma once

// SimTool --probe-census (docs/world-agreement-gaps-design.md §7.2 V1, step
// G0): the proximity census of --level's session world at its anchor,
// read-only.  Nothing is rewired, written or ticked: the census says what the
// design's rewire (§2.3, built at G1 in game/proximity_union.hpp) would take
// per class, under the names it would give, and checks V1's facts.
//
// The world is the server's (physics_world), which has already rewired its
// own blocks at the anchor (since step G1 through game/proximity_union.hpp,
// every block, the world option's bit clear).  So the structural test (§2.3
// "Which blocks") reads a block's ObjectA source as either Ball_Pos_Frame
// (not rewired) or a server frame "BMMO_Prox_*" (rewired by the server), and
// an identity read's target as either CurrentLevel or a server copy
// "CurrentLevel#BMMOId_*" (the copy holds the same columns).  That the server
// rewired exactly the structural set, under the names this census computes,
// is itself a check (`server rewire`, `server names`), and so is the union's
// own signature against the census's (`union signature`, step G1).
//
// Per block of the structural set (prototype TT Scaleable Proximity, ObjectA
// = Ball_Pos_Frame), in (owner, script, walk) order:
//   - its class: mechanism (the root script is in the session selection and
//     is not keep_local - session/resync_rules.hpp), balloon (keep_local),
//     flame (owner PC_* / PS_*), extra (owner P_Extra*), other;
//   - the frame name §2.3 gives it: BMMO_Prox_<owner>_<n>, n = the block's
//     position in the pre-order walk of its root script (the root is 0);
//   - ObjectB, Distance, Check Axis (local 3), Use Outputs (local 2),
//     Barycenter (input 3), Squared (local 4), the exactness min / max
//     (inputs 4, 5).
// Per identity read (Get Cell on CurrentLevel's ActiveBall column inside a
// script with at least one probe of the class):
//   - its copy name CurrentLevel#BMMOId_<owner>_<n> and kind (by-name when a
//     parameter operation reads its output - the server's operation_on, PW -
//     otherwise body);
//   - its gating probes: the class's probes with an output linked directly to
//     the Get Cell's input in the Get Cell's graph (exactly one required);
//   - (revision 3, §2.10) the unit around it: D the blocks with an input
//     (or the target) whose source chain starts at the Get Cell's output -
//     through destinations of outputs and parameter operations; C the blocks
//     of the Get Cell's graph on a behaviour-link path from the gating probe
//     to a block of D; S the graph's locals a block of U = {P, R} + D + C
//     writes (an output's destination) and a block of U reads; the shape:
//     "graph" when U and S are every block and every written local of a
//     sub-behaviour graph, otherwise "blocks".  The census asserts the two
//     shapes WAD read (the fan: graph, D = {Box Box Intersection,
//     SetPhysicsForce}, C = 2 Binary Switch + 2 Identity, |S| = 2; the
//     bridge: blocks, D = {Test} through "Get Name", C and S empty) and that
//     no consumer and no gating probe lies outside the Get Cell's graph.
// The signature (§2.3) is a 64-bit FNV-1a over the class's blocks in
// (owner, script, walk) order, each: str frame, str ObjectB, f32 Distance,
// i32 axes, i32 Use Outputs, f32 exactness min, f32 exactness max; then the
// identity reads in the same order, each: str copy, u8 kind (1 by-name,
// 0 body), str gating frame ("" unless exactly one); then str the entity
// CurrentLevel[0][ActiveBall] names; then the ResetPoints row 0 position
// (3 x f32).  A str is a u32 length and its bytes; numbers little-endian.
//
// Placement (SPD): every copy the world's placement made is matched with
// its row's original object: the copy's structural blocks (by the copy's
// own object list) must equal the original's (its scripts walked), class by
// class; the expected counts (§1.1.2, placement off) are then compared with
// the census minus the copies' blocks.
//
// Public CK2 API only, nothing written; parameter values are read with
// update = FALSE, so no parameter operation runs.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "CKAll.h"

#include <game/script_replay.hpp>
#include <game/sector_placement.hpp>
#include <session/resync_rules.hpp>

namespace bmmo::sim {
    class probe_census {
    public:
        struct probe {
            CK_ID id = 0, root = 0;
            std::string owner, script, klass, frame, object_b, source;
            int walk = 0;
            bool mechanism = false;
            bool server_rewired = false;   // ObjectA is a server frame, not Ball_Pos_Frame
            std::string server_name;       // that frame's name (step G1: the union's §2.3 name)
            float distance = 0.0f, exact_min = 0.0f, exact_max = 0.0f;
            int barycenter = -1, axes = -1, use_outputs = -1, squared = -1;
        };
        struct member {
            CK_ID id = 0;
            std::string prototype, via;
            int walk = -1;
        };
        struct identity {
            CK_ID id = 0, root = 0;
            std::string owner, script, copy, operation, source;
            int walk = 0;
            bool by_name = false;
            bool in_all = false, in_mechanism = false;
            bool server_copy = false;   // its target is a server copy (its name in `source`)
            std::vector<size_t> gating_all, gating_mechanism;   // indices into probes
            // The unit (mechanism class only, with exactly one gating probe).
            std::vector<member> consumers, control;
            std::vector<std::pair<int, std::string>> state;   // the graph's local index and name
            std::string shape, unit_error;
            size_t unit_blocks = 0;
            bool placed = false;   // inside a placement copy
        };
        struct copy_count {
            std::string name, source;
            size_t blocks_all = 0, blocks_mechanism = 0, source_all = 0, source_mechanism = 0, units = 0;
        };
        struct result {
            std::vector<probe> probes;
            std::vector<identity> identities;
            std::vector<copy_count> copies;
            size_t unrewired = 0;              // structural blocks the server left on Ball_Pos_Frame
            size_t server_probe_count = 0;     // the world's own count (physics_world::proximity_probe_count)
            size_t identities_on_original = 0; // all-class reads the server left on CurrentLevel
            std::string initial_entity;
            float reset[3] = {0, 0, 0};
            bool have_reset = false;
            uint64_t signature_mechanism = 0, signature_all = 0;
            // The world's proximity union (step G1): its own signatures, set by the caller.
            bool have_union = false;
            uint64_t union_mechanism = 0, union_all = 0;
            std::vector<std::string> errors;   // a refusal of the census itself (missing objects)
        };

        static result run(CKContext* context, const bmmo::game::script_replay::selection& sel,
                          const bmmo::game::sector_placement& placement, const std::vector<VxVector>& resetpoints,
                          size_t server_probe_count) {
            result out;
            out.server_probe_count = server_probe_count;
            if (!context) {
                out.errors.push_back("no context");
                return out;
            }
            if (!resetpoints.empty()) {
                out.reset[0] = resetpoints.front().x;
                out.reset[1] = resetpoints.front().y;
                out.reset[2] = resetpoints.front().z;
                out.have_reset = true;
            } else {
                out.errors.push_back("ResetPoints has no row");
            }
            CKObject* ball_pos_frame = context->GetObjectByNameAndParentClass(const_cast<CKSTRING>("Ball_Pos_Frame"),
                                                                              CKCID_3DENTITY, nullptr);
            if (!ball_pos_frame) out.errors.push_back("Ball_Pos_Frame not found");
            auto* current_level = CKDataArray::Cast(
                context->GetObjectByNameAndClass(const_cast<CKSTRING>("CurrentLevel"), CKCID_DATAARRAY, nullptr));
            int ball_column = -1;
            if (current_level)
                for (int c = 0; c < current_level->GetColumnCount(); ++c) {
                    CKSTRING column = current_level->GetColumnName(c);
                    if (column && std::strcmp(column, "ActiveBall") == 0) { ball_column = c; break; }
                }
            if (!current_level || ball_column < 0) out.errors.push_back("CurrentLevel or its ActiveBall column not found");
            else if (CKObject* ball = current_level->GetElementObject(0, ball_column))
                out.initial_entity = ball->GetName() ? ball->GetName() : "";

            const std::set<CK_ID> selected(sel.scripts.begin(), sel.scripts.end());
            std::vector<CKBehavior*> behaviors;
            {
                const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
                CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
                for (int i = 0; i < count; ++i)
                    if (auto* b = CKBehavior::Cast(context->GetObject(ids[i]))) behaviors.push_back(b);
            }
            std::vector<CKParameterOperation*> operations;
            {
                const int count = context->GetObjectsCountByClassID(CKCID_PARAMETEROPERATION);
                CK_ID* ids = context->GetObjectsListByClassID(CKCID_PARAMETEROPERATION);
                for (int i = 0; i < count; ++i)
                    if (auto* op = CKParameterOperation::Cast(context->GetObject(ids[i]))) operations.push_back(op);
            }
            std::map<CK_ID, std::map<CK_ID, int>> walks;   // root -> block -> pre-order position
            const auto walk_of = [&walks](CKBehavior* root, CK_ID block) {
                auto it = walks.find(root->GetID());
                if (it == walks.end()) {
                    std::map<CK_ID, int> positions;
                    int n = 0;
                    walk(root, [&](CKBehavior* b) { positions[b->GetID()] = n++; });
                    it = walks.emplace(root->GetID(), std::move(positions)).first;
                }
                auto found = it->second.find(block);
                return found == it->second.end() ? -1 : found->second;
            };

            // The structural set.
            std::map<CK_ID, size_t> probe_index;
            for (CKBehavior* block: behaviors) {
                bool rewired = false;
                if (!structural(context, block, ball_pos_frame, rewired)) continue;
                CKBehavior* root = root_of(block);
                probe p;
                if (rewired)
                    if (CKObject* a = input_object(context, block, 1)) p.server_name = a->GetName() ? a->GetName() : "";
                p.id = block->GetID();
                p.root = root->GetID();
                p.owner = owner_name(root);
                p.script = root->GetName() ? root->GetName() : "";
                p.walk = walk_of(root, block->GetID());
                p.frame = "BMMO_Prox_" + p.owner + "_" + std::to_string(p.walk);
                p.server_rewired = rewired;
                p.source = rewired ? "server-frame" : "Ball_Pos_Frame";
                if (!rewired) ++out.unrewired;
                const bool keep = bmmo::session::keep_local_script(p.owner, p.script);
                p.mechanism = selected.count(root->GetID()) != 0 && !keep;
                p.klass = p.mechanism ? "mechanism"
                        : keep ? "balloon"
                        : (starts(p.owner, "PC_") || starts(p.owner, "PS_")) ? "flame"
                        : starts(p.owner, "P_Extra") ? "extra"
                        : "other";
                if (CKObject* b = input_object(context, block, 2)) p.object_b = b->GetName() ? b->GetName() : "";
                read_input(block, 0, p.distance);
                read_input(block, 3, p.barycenter);
                read_input(block, 4, p.exact_min);
                read_input(block, 5, p.exact_max);
                read_local(block, 2, p.use_outputs);
                read_local(block, 3, p.axes);
                read_local(block, 4, p.squared);
                probe_index[p.id] = out.probes.size();
                out.probes.push_back(std::move(p));
            }
            std::set<CK_ID> roots_all, roots_mechanism;
            for (const probe& p: out.probes) {
                roots_all.insert(p.root);
                if (p.mechanism) roots_mechanism.insert(p.root);
            }

            // Identity reads.
            for (CKBehavior* block: behaviors) {
                if (prototype(block) != "Get Cell") continue;
                CKParameterIn* target = block->GetTargetParameter();
                CKObject* array = target ? object_of(context, target->GetRealSource()) : nullptr;
                if (!array || !array->GetName()) continue;
                const std::string array_name = array->GetName();
                const bool original = current_level && array->GetID() == current_level->GetID();
                const bool server_copy = starts(array_name, "CurrentLevel#BMMOId_");
                if (!original && !server_copy) continue;
                CKParameterIn* column_in = block->GetInputParameterCount() > 1 ? block->GetInputParameter(1) : nullptr;
                CKParameter* column_source = column_in ? column_in->GetRealSource() : nullptr;
                int column = -1;
                if (!column_source || column_source->GetValue(&column, FALSE) != CK_OK || column != ball_column) continue;
                CKBehavior* root = root_of(block);
                identity r;
                r.id = block->GetID();
                r.root = root->GetID();
                r.in_all = roots_all.count(r.root) != 0;
                r.in_mechanism = roots_mechanism.count(r.root) != 0;
                if (!r.in_all) continue;   // a retail read (Ball_Shadow, Sound_Manager, ...): no class takes it
                r.owner = owner_name(root);
                r.script = root->GetName() ? root->GetName() : "";
                r.walk = walk_of(root, block->GetID());
                r.copy = "CurrentLevel#BMMOId_" + r.owner + "_" + std::to_string(r.walk);
                r.server_copy = server_copy;
                r.source = array_name;
                if (!server_copy) ++out.identities_on_original;
                r.operation = operation_on(operations, block->GetOutputParameterCount() > 0 ? block->GetOutputParameter(0) : nullptr);
                r.by_name = !r.operation.empty();
                // Gating probes: an output of the probe linked directly to an input of the Get Cell.
                if (CKBehavior* graph = block->GetParent()) {
                    std::set<CK_ID> seen;
                    for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
                        CKBehaviorLink* link = graph->GetSubBehaviorLink(i);
                        CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                        CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                        if (!from || !to || to->GetOwner() != block || from->GetType() != CK_BEHAVIORIO_OUT
                            || to->GetType() != CK_BEHAVIORIO_IN || !from->GetOwner())
                            continue;
                        auto it = probe_index.find(from->GetOwner()->GetID());
                        if (it == probe_index.end() || !seen.insert(it->first).second) continue;
                        r.gating_all.push_back(it->second);
                        if (out.probes[it->second].mechanism) r.gating_mechanism.push_back(it->second);
                    }
                }
                if (r.in_mechanism && r.gating_mechanism.size() == 1)
                    find_unit(context, behaviors, operations, block, out.probes[r.gating_mechanism.front()].id, walk_of, r);
                out.identities.push_back(std::move(r));
            }

            // Placement copies against their rows' originals.
            for (const auto& c: placement.copies()) {
                copy_count cc;
                cc.name = c.name;
                cc.source = c.source;
                std::set<CK_ID> objects;
                for (const auto& [id, cid]: c.objects)
                    if (cid == CKCID_BEHAVIOR) objects.insert(id);
                for (const probe& p: out.probes)
                    if (objects.count(p.id)) {
                        ++cc.blocks_all;
                        cc.blocks_mechanism += p.mechanism ? 1u : 0u;
                    }
                for (identity& r: out.identities)
                    if (objects.count(r.id)) {
                        r.placed = true;
                        cc.units += r.in_mechanism ? 1u : 0u;
                    }
                if (auto* source = CKBeObject::Cast(context->GetObject(c.source_id))) {
                    for (int s = 0; s < source->GetScriptCount(); ++s) {
                        CKBehavior* script = source->GetScript(s);
                        if (!script) continue;
                        const bool keep = bmmo::session::keep_local_script(owner_name(script),
                                                                           script->GetName() ? script->GetName() : "");
                        const bool mech = selected.count(script->GetID()) != 0 && !keep;
                        walk(script, [&](CKBehavior* b) {
                            bool rewired = false;
                            if (!structural(context, b, ball_pos_frame, rewired)) return;
                            ++cc.source_all;
                            cc.source_mechanism += mech ? 1u : 0u;
                        });
                    }
                } else {
                    out.errors.push_back("placement copy " + c.name + ": its original " + c.source + " is gone");
                }
                out.copies.push_back(std::move(cc));
            }

            const auto order = [&out](size_t a, size_t b) {
                const probe& x = out.probes[a];
                const probe& y = out.probes[b];
                return std::tie(x.owner, x.script, x.walk) < std::tie(y.owner, y.script, y.walk);
            };
            std::vector<size_t> sorted(out.probes.size());
            for (size_t i = 0; i < sorted.size(); ++i) sorted[i] = i;
            std::sort(sorted.begin(), sorted.end(), order);
            std::vector<probe> probes;
            std::vector<size_t> remap(out.probes.size());
            for (size_t i = 0; i < sorted.size(); ++i) {
                remap[sorted[i]] = i;
                probes.push_back(out.probes[sorted[i]]);
            }
            out.probes = std::move(probes);
            for (identity& r: out.identities) {
                for (size_t& g: r.gating_all) g = remap[g];
                for (size_t& g: r.gating_mechanism) g = remap[g];
            }
            std::sort(out.identities.begin(), out.identities.end(), [](const identity& x, const identity& y) {
                return std::tie(x.owner, x.script, x.walk) < std::tie(y.owner, y.script, y.walk);
            });
            out.signature_mechanism = signature(out, true);
            out.signature_all = signature(out, false);
            return out;
        }

        // §1.1.2's table, placement off: all, mechanism, identity body, identity by-name; level 13 = the arena.
        struct expected { int all, mechanism, body, by_name; };
        static bool expected_for(int level, expected& e) {
            static const expected table[13] = {
                {18, 3, 0, 0},   {47, 32, 9, 1}, {32, 15, 3, 1}, {30, 12, 3, 1}, {28, 14, 2, 0},
                {43, 29, 8, 2},  {39, 25, 4, 2}, {29, 14, 2, 1}, {43, 27, 5, 1}, {47, 32, 5, 2},
                {47, 32, 5, 1},  {60, 43, 11, 0}, {24, 12, 1, 1}};
            if (level < 1 || level > 13) return false;
            e = table[level - 1];
            return true;
        }

        // Prints the census and its checks; 0 = every check passed, 3 = a check failed.
        static int print(const result& r, int level, bool placement) {
            std::printf("probe census: level %d, placement %s\n", level, placement ? "on" : "off");
            for (const std::string& e: r.errors) std::printf("census error: %s\n", e.c_str());
            for (const probe& p: r.probes)
                std::printf("probe %-9s %s/%s #%d id=%u frame=%s objectB=%s distance=%.9g axes=%d outputs=%d "
                            "barycenter=%d squared=%d exact=%.9g..%.9g source=%s\n",
                            p.klass.c_str(), p.owner.c_str(), p.script.c_str(), p.walk, static_cast<unsigned>(p.id),
                            p.frame.c_str(), p.object_b.c_str(), p.distance, p.axes, p.use_outputs, p.barycenter,
                            p.squared, p.exact_min, p.exact_max, p.source.c_str());
            for (const identity& i: r.identities) {
                std::string gating;
                for (size_t g: i.in_mechanism ? i.gating_mechanism : i.gating_all)
                    gating += (gating.empty() ? "" : ",") + r.probes[g].frame;
                std::printf("identity %-9s %s/%s #%d id=%u copy=%s kind=%s gating=%s source=%s%s\n",
                            i.in_mechanism ? "mechanism" : "all", i.owner.c_str(), i.script.c_str(), i.walk,
                            static_cast<unsigned>(i.id), i.copy.c_str(),
                            i.by_name ? ("by-name(" + i.operation + ")").c_str() : "body",
                            gating.empty() ? "none" : gating.c_str(), i.source.c_str(), i.placed ? " placed" : "");
                if (!i.in_mechanism || i.shape.empty()) continue;
                std::string d, c, s;
                for (const member& m: i.consumers) d += (d.empty() ? "" : ", ") + m.prototype + "#" + std::to_string(m.walk) + "(" + m.via + ")";
                for (const member& m: i.control) c += (c.empty() ? "" : ", ") + m.prototype + "#" + std::to_string(m.walk);
                for (const auto& [index, name]: i.state) s += (s.empty() ? "" : ", ") + std::to_string(index) + " " + name;
                std::printf("  unit shape=%s U=%zu D=[%s] C=[%s] S=[%s]%s%s\n", i.shape.c_str(), i.unit_blocks,
                            d.c_str(), c.c_str(), s.c_str(), i.unit_error.empty() ? "" : " ERROR ",
                            i.unit_error.c_str());
            }

            std::map<std::string, int> by_class;
            for (const probe& p: r.probes) ++by_class[p.klass];
            int id_all = 0, id_all_body = 0, id_all_name = 0, id_mech = 0, id_mech_body = 0, id_mech_name = 0;
            int units_fan = 0, units_bridge = 0, units_placed = 0;
            for (const identity& i: r.identities) {
                ++id_all;
                (i.by_name ? id_all_name : id_all_body)++;
                if (!i.in_mechanism) continue;
                ++id_mech;
                (i.by_name ? id_mech_name : id_mech_body)++;
                (i.by_name ? units_bridge : units_fan)++;
                units_placed += i.placed ? 1 : 0;
            }
            const int all = static_cast<int>(r.probes.size());
            const int mech = by_class["mechanism"];
            std::printf("counts: all %d = mechanism %d + flame %d + extra %d + balloon %d + other %d; server rewired %zu "
                        "(the world's count %zu), left on Ball_Pos_Frame %zu\n",
                        all, mech, by_class["flame"], by_class["extra"], by_class["balloon"], by_class["other"],
                        r.probes.size() - r.unrewired, r.server_probe_count, r.unrewired);
            std::printf("identity reads: all %d (body %d / by-name %d), mechanism %d (body %d / by-name %d), "
                        "left on CurrentLevel %zu; units %d (fans %d, bridges %d, placed %d)\n",
                        id_all, id_all_body, id_all_name, id_mech, id_mech_body, id_mech_name, r.identities_on_original,
                        id_mech, units_fan, units_bridge, units_placed);
            std::printf("signature: mechanism %016llx, all %016llx (initial entity %s, reset point %.9g %.9g %.9g)\n",
                        static_cast<unsigned long long>(r.signature_mechanism),
                        static_cast<unsigned long long>(r.signature_all), r.initial_entity.c_str(), r.reset[0],
                        r.reset[1], r.reset[2]);
            size_t copied_all = 0, copied_mech = 0, copied_units = 0;
            int copy_mismatch = 0;
            for (const copy_count& c: r.copies) {
                copied_all += c.blocks_all;
                copied_mech += c.blocks_mechanism;
                copied_units += c.units;
                const bool same = c.blocks_all == c.source_all && c.blocks_mechanism == c.source_mechanism;
                copy_mismatch += same ? 0 : 1;
                if (c.blocks_all || c.source_all || !same)
                    std::printf("placement copy %s of %s: blocks all %zu mechanism %zu, original's all %zu mechanism %zu, "
                                "units %zu%s\n", c.name.c_str(), c.source.c_str(), c.blocks_all, c.blocks_mechanism,
                                c.source_all, c.source_mechanism, c.units, same ? "" : " MISMATCH");
            }
            std::printf("placement: %zu copies, copied blocks all %zu mechanism %zu, copied units %zu; unplaced counts all "
                        "%zu mechanism %zu\n", r.copies.size(), copied_all, copied_mech, copied_units,
                        r.probes.size() - copied_all, static_cast<size_t>(mech) - copied_mech);

            bool pass = r.errors.empty();
            const auto check = [&pass](const char* name, bool ok, const std::string& detail) {
                std::printf("check %-22s %s%s%s\n", name, ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
                pass = pass && ok;
            };
            expected e{};
            if (expected_for(level, e)) {
                const int unplaced_all = all - static_cast<int>(copied_all);
                const int unplaced_mech = mech - static_cast<int>(copied_mech);
                int unplaced_body = 0, unplaced_name = 0;
                for (const identity& i: r.identities)
                    if (i.in_mechanism && !i.placed) (i.by_name ? unplaced_name : unplaced_body)++;
                char text[160];
                std::snprintf(text, sizeof(text), "all %d mechanism %d (expected %d %d)", unplaced_all, unplaced_mech,
                              e.all, e.mechanism);
                check("table counts", unplaced_all == e.all && unplaced_mech == e.mechanism, text);
                std::snprintf(text, sizeof(text), "body %d by-name %d (expected %d %d)", unplaced_body, unplaced_name,
                              e.body, e.by_name);
                check("table identities", unplaced_body == e.body && unplaced_name == e.by_name, text);
            } else {
                std::printf("check %-22s SKIP  no §1.1.2 row for level %d\n", "table counts", level);
            }
            check("all identities", id_all == id_mech,
                  std::to_string(id_all) + " identity reads, " + std::to_string(id_mech) + " in mechanism scripts");
            check("server rewire", r.unrewired == 0 && r.probes.size() == r.server_probe_count
                                       && r.identities_on_original == 0,
                  "left " + std::to_string(r.unrewired) + ", world " + std::to_string(r.server_probe_count)
                      + ", reads left " + std::to_string(r.identities_on_original));
            {
                // Step G1: the server's objects carry the names this census computes.
                std::string wrong;
                for (const probe& p: r.probes)
                    if (p.server_rewired && p.server_name != p.frame) wrong += (wrong.empty() ? "" : ", ") + p.server_name + " != " + p.frame;
                for (const identity& i: r.identities)
                    if (i.server_copy && i.source != i.copy) wrong += (wrong.empty() ? "" : ", ") + i.source + " != " + i.copy;
                check("server names", wrong.empty(), wrong);
            }
            if (r.have_union) {
                char text[96];
                std::snprintf(text, sizeof(text), "union mechanism %016llx all %016llx",
                              static_cast<unsigned long long>(r.union_mechanism), static_cast<unsigned long long>(r.union_all));
                check("union signature", r.union_mechanism == r.signature_mechanism && r.union_all == r.signature_all, text);
            }
            std::string bad;
            for (const probe& p: r.probes)
                if (p.mechanism && (p.barycenter != 0 || (p.axes != 5 && p.axes != 7) || p.squared != 1))
                    bad += (bad.empty() ? "" : ", ") + p.frame;
            check("mechanism facts", bad.empty(), bad.empty() ? std::to_string(mech) + " blocks: barycenter 0, axes 5|7, squared 1" : bad);
            bad.clear();
            for (const identity& i: r.identities) {
                const size_t n = i.in_mechanism ? i.gating_mechanism.size() : i.gating_all.size();
                if (n != 1) bad += (bad.empty() ? "" : ", ") + i.copy + " (" + std::to_string(n) + ")";
            }
            check("one gating probe", bad.empty(), bad);
            bad.clear();
            {
                std::map<std::string, int> names;
                for (const probe& p: r.probes) ++names[p.frame];
                for (const identity& i: r.identities) ++names[i.copy];
                ++names["BMMO_NoBall"];
                for (const auto& [name, n]: names)
                    if (n > 1) bad += (bad.empty() ? "" : ", ") + name + " x" + std::to_string(n);
                for (const auto& [name, n]: names)
                    if (name.find("_BMMO_") != std::string::npos || name.find("_Peer_") != std::string::npos)
                        bad += (bad.empty() ? "" : ", ") + name + " (filtered infix)";
            }
            check("names unique", bad.empty(), bad);
            bad.clear();
            for (const identity& i: r.identities)
                if (i.in_mechanism && !i.unit_error.empty()) bad += (bad.empty() ? "" : ", ") + i.copy + ": " + i.unit_error;
            check("unit shapes", bad.empty(), bad);
            check("placement copies", copy_mismatch == 0, std::to_string(r.copies.size()) + " copies, "
                                                                + std::to_string(copy_mismatch) + " mismatched");
            std::printf("PROBE-CENSUS: %s\n", pass ? "PASS" : "FAIL");
            return pass ? 0 : 3;
        }

    private:
        template <typename F> static void walk(CKBehavior* beh, F&& fn) {
            fn(beh);
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i)) walk(child, fn);
        }
        static bool starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
        static std::string prototype(CKBehavior* block) {
            if (block->IsUsingFunction() && block->GetPrototypeName()) return block->GetPrototypeName();
            return block->GetName() ? block->GetName() : "?";
        }
        static CKBehavior* root_of(CKBehavior* block) {
            while (block->GetParent()) block = block->GetParent();
            return block;
        }
        static std::string owner_name(CKBehavior* root) {
            CKBeObject* owner = root ? root->GetOwner() : nullptr;
            return owner && owner->GetName() ? owner->GetName() : "";
        }
        static CKObject* object_of(CKContext* context, CKParameter* source) {
            if (!source) return nullptr;
            CKParameterManager* pm = context->GetParameterManager();
            if (!pm || !pm->IsDerivedFrom(source->GetGUID(), CKPGUID_OBJECT)) return nullptr;
            return source->GetValueObject(FALSE);
        }
        static CKObject* input_object(CKContext* context, CKBehavior* block, int index) {
            if (index >= block->GetInputParameterCount()) return nullptr;
            CKParameterIn* in = block->GetInputParameter(index);
            return in ? object_of(context, in->GetRealSource()) : nullptr;
        }
        template <typename T> static void read_input(CKBehavior* block, int index, T& value) {
            if (index >= block->GetInputParameterCount()) return;
            CKParameterIn* in = block->GetInputParameter(index);
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            T v{};
            if (source && source->GetDataSize() == static_cast<int>(sizeof(T)) && source->GetValue(&v, FALSE) == CK_OK) value = v;
        }
        template <typename T> static void read_local(CKBehavior* block, int index, T& value) {
            if (index >= block->GetLocalParameterCount()) return;
            CKParameterLocal* local = block->GetLocalParameter(index);
            T v{};
            if (local && local->GetDataSize() == static_cast<int>(sizeof(T)) && local->GetValue(&v, FALSE) == CK_OK) value = v;
        }
        // §2.3's structural test, on a world the server may have rewired.
        static bool structural(CKContext* context, CKBehavior* block, CKObject* ball_pos_frame, bool& server_rewired) {
            if (!block || prototype(block) != "TT Scaleable Proximity") return false;
            CKObject* a = input_object(context, block, 1);
            if (!a) return false;
            if (ball_pos_frame && a->GetID() == ball_pos_frame->GetID()) {
                server_rewired = false;
                return true;
            }
            if (a->GetName() && std::strncmp(a->GetName(), "BMMO_Prox_", 10) == 0) {
                server_rewired = true;
                return true;
            }
            return false;
        }
        // PW's operation_on: the name of a parameter operation reading `value`, "" when none.
        static std::string operation_on(const std::vector<CKParameterOperation*>& operations, CKParameter* value) {
            if (!value) return {};
            for (CKParameterOperation* op: operations)
                for (CKParameterIn* in: {op->GetInParameter1(), op->GetInParameter2()})
                    if (in && in->GetDirectSource() == value) return op->GetName() ? op->GetName() : "?";
            return {};
        }

        template <typename WalkOf>
        static void find_unit(CKContext* context, const std::vector<CKBehavior*>& behaviors,
                              const std::vector<CKParameterOperation*>& operations, CKBehavior* read, CK_ID gating,
                              WalkOf& walk_of, identity& r) {
            CKBehavior* graph = read->GetParent();
            CKBehavior* root = root_of(read);
            CKParameterOut* value = read->GetOutputParameterCount() > 0 ? read->GetOutputParameter(0) : nullptr;
            if (!graph || !value) {
                r.unit_error = "the Get Cell has no graph or no output";
                return;
            }
            // The parameters whose value is the read's: its output, the
            // destinations of those, and the outputs of the operations reading them.
            std::set<CKParameter*> reach{value};
            std::map<CKParameter*, std::string> via_op;   // an operation output -> the operation's name
            for (bool grew = true; grew;) {
                grew = false;
                std::vector<CKParameter*> current(reach.begin(), reach.end());
                for (CKParameter* p: current) {
                    auto* out = p->GetClassID() == CKCID_PARAMETEROUT ? static_cast<CKParameterOut*>(p) : nullptr;
                    if (!out) continue;
                    for (int i = 0; i < out->GetDestinationCount(); ++i)
                        if (CKParameter* d = out->GetDestination(i); d && reach.insert(d).second) grew = true;
                }
                for (CKParameterOperation* op: operations) {
                    CKParameterOut* result = op->GetOutParameter();
                    if (!result || reach.count(result)) continue;
                    for (CKParameterIn* in: {op->GetInParameter1(), op->GetInParameter2()})
                        if (in && reach.count(in->GetRealSource())) {
                            reach.insert(result);
                            via_op[result] = op->GetName() ? op->GetName() : "?";
                            grew = true;
                            break;
                        }
                }
            }
            // D: every block (anywhere) with an input or a target fed from that set.
            std::set<CK_ID> unit{read->GetID(), gating};
            std::set<CK_ID> consumers;
            std::string outside;
            for (CKBehavior* b: behaviors) {
                if (b == read) continue;
                std::string via;
                const auto look = [&](CKParameterIn* in, const std::string& label) {
                    CKParameter* s = in ? in->GetRealSource() : nullptr;
                    if (!s || !reach.count(s)) return;
                    via += (via.empty() ? "" : ", ") + label;
                    auto op = via_op.find(s);
                    if (op != via_op.end()) via += " via " + op->second;
                    else if (s != value) via += " via a local";
                };
                for (int i = 0; i < b->GetInputParameterCount(); ++i) {
                    CKParameterIn* in = b->GetInputParameter(i);
                    look(in, "pin " + std::to_string(i));
                }
                look(b->GetTargetParameter(), "target");
                if (via.empty()) continue;
                member m;
                m.id = b->GetID();
                m.prototype = prototype(b);
                m.via = via;
                m.walk = root_of(b) == root ? walk_of(root, b->GetID()) : -1;
                if (b->GetParent() != graph) outside += (outside.empty() ? "" : ", ") + m.prototype + " outside the Get Cell's graph";
                consumers.insert(b->GetID());
                unit.insert(b->GetID());
                r.consumers.push_back(m);
            }
            // C: the graph's blocks on a link path from the gating probe to a block of D.
            std::map<CK_ID, std::vector<CK_ID>> forward, backward;
            std::set<CK_ID> children;
            for (int i = 0; i < graph->GetSubBehaviorCount(); ++i)
                if (CKBehavior* child = graph->GetSubBehavior(i)) children.insert(child->GetID());
            if (!children.count(gating)) outside += std::string(outside.empty() ? "" : ", ") + "the gating probe outside the Get Cell's graph";
            for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
                CKBehaviorLink* link = graph->GetSubBehaviorLink(i);
                CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                if (!from || !to || !from->GetOwner() || !to->GetOwner()) continue;
                const CK_ID a = from->GetOwner()->GetID(), b = to->GetOwner()->GetID();
                if (!children.count(a) || !children.count(b)) continue;
                forward[a].push_back(b);
                backward[b].push_back(a);
            }
            const auto closure = [](std::map<CK_ID, std::vector<CK_ID>>& edges, std::set<CK_ID> start) {
                std::vector<CK_ID> stack(start.begin(), start.end());
                while (!stack.empty()) {
                    const CK_ID at = stack.back();
                    stack.pop_back();
                    for (CK_ID next: edges[at])
                        if (start.insert(next).second) stack.push_back(next);
                }
                return start;
            };
            const std::set<CK_ID> from_probe = closure(forward, {gating});
            std::set<CK_ID> d_in_graph;
            for (CK_ID id: consumers)
                if (children.count(id)) d_in_graph.insert(id);
            const std::set<CK_ID> to_consumers = closure(backward, d_in_graph);
            for (CK_ID id: from_probe) {
                if (!to_consumers.count(id) || unit.count(id)) continue;
                auto* b = CKBehavior::Cast(context->GetObject(id));
                if (!b) continue;
                member m;
                m.id = id;
                m.prototype = prototype(b);
                m.walk = walk_of(root, id);
                r.control.push_back(m);
            }
            const auto by_walk = [](const member& a, const member& b) { return std::tie(a.walk, a.id) < std::tie(b.walk, b.id); };
            std::sort(r.control.begin(), r.control.end(), by_walk);
            std::sort(r.consumers.begin(), r.consumers.end(), by_walk);
            for (const member& m: r.control) unit.insert(m.id);
            r.unit_blocks = unit.size();
            // S and the graph's written locals.
            std::set<CKParameter*> written_by_unit, read_by_unit, written_in_graph;
            for (CK_ID id: children) {
                auto* b = CKBehavior::Cast(context->GetObject(id));
                if (!b) continue;
                const bool in_unit = unit.count(id) != 0;
                for (int i = 0; i < b->GetOutputParameterCount(); ++i) {
                    CKParameterOut* out = b->GetOutputParameter(i);
                    for (int k = 0; out && k < out->GetDestinationCount(); ++k)
                        if (CKParameter* d = out->GetDestination(k)) {
                            written_in_graph.insert(d);
                            if (in_unit) written_by_unit.insert(d);
                        }
                }
                if (!in_unit) continue;
                for (int i = 0; i < b->GetInputParameterCount(); ++i)
                    if (CKParameterIn* in = b->GetInputParameter(i)) read_by_unit.insert(in->GetRealSource());
                if (CKParameterIn* target = b->GetTargetParameter()) read_by_unit.insert(target->GetRealSource());
            }
            std::set<CKParameter*> state, written_locals;
            for (int i = 0; i < graph->GetLocalParameterCount(); ++i) {
                CKParameterLocal* local = graph->GetLocalParameter(i);
                if (!local) continue;
                if (written_in_graph.count(local)) written_locals.insert(local);
                if (written_by_unit.count(local) && read_by_unit.count(local)) {
                    state.insert(local);
                    r.state.emplace_back(i, local->GetName() ? local->GetName() : "");
                }
            }
            const bool whole = graph->GetParent() != nullptr && unit.size() == children.size()
                && std::includes(unit.begin(), unit.end(), children.begin(), children.end()) && state == written_locals;
            r.shape = whole ? "graph" : "blocks";
            // The two shapes WAD read (§2.10), and nothing outside the graph.
            std::multiset<std::string> d_protos, c_protos;
            for (const member& m: r.consumers) d_protos.insert(m.prototype);
            for (const member& m: r.control) c_protos.insert(m.prototype);
            std::string error = outside;
            const auto add = [&error](const std::string& text) { error += (error.empty() ? "" : "; ") + text; };
            if (!whole && !state.empty()) add("a block unit with graph state");
            if (r.by_name) {
                const bool test_via_name = r.consumers.size() == 1 && r.consumers.front().prototype == "Test"
                    && r.consumers.front().via.find("via Get Name") != std::string::npos;
                if (r.shape != "blocks" || !test_via_name || !r.control.empty() || !state.empty() || r.unit_blocks != 3)
                    add("not the bridge's shape (blocks, D = {Test via Get Name}, C and S empty)");
            } else {
                const std::multiset<std::string> fan_d{"Box Box Intersection", "SetPhysicsForce"};
                const std::multiset<std::string> fan_c{"Binary Switch", "Binary Switch", "Identity", "Identity"};
                if (r.shape != "graph" || d_protos != fan_d || c_protos != fan_c || state.size() != 2)
                    add("not the fan's shape (graph, D = {Box Box Intersection, SetPhysicsForce}, C = 2 Binary Switch "
                        "+ 2 Identity, |S| = 2)");
            }
            r.unit_error = error;
        }

        struct fnv {
            uint64_t h = 1469598103934665603ull;
            void bytes(const void* p, size_t n) {
                const auto* b = static_cast<const uint8_t*>(p);
                for (size_t i = 0; i < n; ++i) {
                    h ^= b[i];
                    h *= 1099511628211ull;
                }
            }
            void u32(uint32_t v) {
                const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                                      static_cast<uint8_t>(v >> 24)};
                bytes(b, 4);
            }
            void i32(int v) { u32(static_cast<uint32_t>(v)); }
            void f32(float v) {
                uint32_t u;
                std::memcpy(&u, &v, 4);
                u32(u);
            }
            void str(const std::string& s) {
                u32(static_cast<uint32_t>(s.size()));
                bytes(s.data(), s.size());
            }
        };
        static uint64_t signature(const result& r, bool mechanism) {
            fnv f;
            for (const probe& p: r.probes) {
                if (mechanism && !p.mechanism) continue;
                f.str(p.frame);
                f.str(p.object_b);
                f.f32(p.distance);
                f.i32(p.axes);
                f.i32(p.use_outputs);
                f.f32(p.exact_min);
                f.f32(p.exact_max);
            }
            for (const identity& i: r.identities) {
                if (mechanism && !i.in_mechanism) continue;
                f.str(i.copy);
                const uint8_t kind = i.by_name ? 1 : 0;
                f.bytes(&kind, 1);
                const auto& gating = mechanism ? i.gating_mechanism : i.gating_all;
                f.str(gating.size() == 1 ? r.probes[gating.front()].frame : std::string());
            }
            f.str(r.initial_entity);
            f.f32(r.reset[0]);
            f.f32(r.reset[1]);
            f.f32(r.reset[2]);
            return f.h;
        }
    };
}
