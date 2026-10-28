#pragma once

// The checkpoint chain's own Activation Phase cell, for a session (N4).
//
// CurrentLevel[0][4] ("Activation Phase?") has two writers in the level's
// graph.  Gameplay_SectorManager writes 0 at the start of a pass and 1 four
// frames later, just before Activate Sector resets the sector's module
// scripts; every module script (P_Modul_17_MF Script, ...) reads the cell in
// its first frame and takes its Physicalize branch on 1, its Unphysicalize /
// Restore IC branch on 0.  Gameplay_Events / activate next Checkpoint, which a
// crossed checkpoint runs two frames after activate Sektor, writes 0, resets
// the old checkpoint's script (PC_TwoFlames_MF Script reads the cell and
// switches its proximity off on 0), and two frames later writes 1 and resets
// the next checkpoint's script (proximity on).  In retail the manager ran
// inside that chain, so the two sets of writes were ordered.  In a session the
// manager runs at the stamped tick (sector_activation.hpp's hold), so they are
// two unsynchronised writers of one cell:
//
//   * a crossing C three frames after a pass M has the chain's 0 (C+2) land on
//     the frame the pass's modules read the cell (M+5): they read 0 and take
//     their teardown branch, and this world has none of the sector's bodies
//     the other worlds made (e8ab2_swing_n1 client 3: Modul17_Dreharme,
//     P_Modul_26_Rope, Sack, Kiste, P_Modul_34_Schiebestein, P_Modul_37_Bridge
//     missing from tick 2155 on);
//   * a pass four or five frames after a crossing has the manager's 0 (M..M+3)
//     land on the frame the next checkpoint's script reads it (C+5): the
//     checkpoint is switched off and this client never crosses another one.
//
// A replay makes it worse: the script replay re-issues the pass but never
// re-runs the chain (Gameplay_Events is not a mechanism script), so a rewind
// across the pass builds the bodies the live frames lost.
//
// So the checkpoints get a cell of their own: a private copy of CurrentLevel
// (the precedent is the server's ball identity copies,
// once physics_world::rewire_ball_identity_reads, now the identity copies of
// game/proximity_union.hpp, CurrentLevel#BMMOId_<owner>_<n>).  A session-owned parameter
// naming the copy feeds the Target of
//
//   * the chain's two Set Cells of the cell;
//   * Event_handler's "deact. Checkpoints" Set Cells of the cell (a level
//     reset or exit writes 0, resets every checkpoint's script - off - and,
//     for a reset, 1 two frames later): checkpoint-only writers, moved so a
//     reset still switches the checkpoints off;
//   * every Get Cell(row 0, column 4) in the scripts of the checkpoint
//     entities (the rows of the array the chain's Get Row reads).
//
// The copy starts at 1, the cell's value in play: the first checkpoint's
// script is reset by Gameplay_Events' set first Checkpoint after the anchor,
// where retail read the 1 Init Ingame wrote (at the anchor the cell is still
// the 0 Levelinit_build left; measured: copying that value disarmed the first
// checkpoint and the client never crossed one).  The modules then see only the
// manager's writes (and the level's own init), which are the same on every
// side, live and replayed, and the checkpoints see only the chain's and the
// reset's, which keep their retail order.  Nothing else changes: the chains
// keep their shape, their delays and their other writes.  (The copy and the
// parameter are new CK objects, so the bodies made after the anchor get
// other CK_IDs than before, and CKIpionManager::m_PhysicsObjects, an
// XHashTable keyed by them, lists them in another order: the simulation and
// the world hash do not depend on it, a journal's checkpoint body order does.)
//
// Left on CurrentLevel[0][4] (other_writers, logged): the manager, the
// level's own init (Init Ingame, init Placeholders) and Gameplay_Events'
// level-finish pair - Level_Finish -> fade -> End Level -> a 50 s Delayer ->
// Set Cell 0, Activate Script(PE_Balloon Script, reset), Unphysicalize and
// Restore IC of the balloon -> Set Cell 1.  That pair stays: its reader,
// PE_Balloon Script (Start -> Get Cell(0, 4) -> Binary Switch, like a
// module), is a mechanism script the rollback replays and must read the
// recorded cell (a reader like it in a checkpoint script is refused below).
// It fires only in a world whose ball finished (the server's: a clone's
// Level_Finish), some 50 s later, when every sector that ball entered is
// long in the union: a pass can be in flight then only if the finisher
// skipped a checkpoint, and the core's phase check logs one if it is.
//
// Once per world at its anchor, like the hold: the server's world, the
// headless client's (handed back at its next session's start, as base.cmo's
// chain outlives a level); the retail mod resolves it with N3 and hands the
// retail sources back at session end (restore).  A graph that is not the one
// this understands is refused whole and left alone, with the reason.
//
// Only the public CK2 API is used: the header compiles against the Virtools
// SDK (client mod) and against the Ballanced engine (headless server).

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "CKAll.h"

#include "menu_driver.hpp"
#include "script_replay.hpp"

namespace bmmo::game {
    class checkpoint_phase_split {
    public:
        static constexpr int kRow = 0;
        static constexpr int kColumn = 4;

        struct rewire {
            CK_ID block = 0;           // the Set Cell (writer) or Get Cell (reader)
            bool writer = false;
            CK_ID retail_direct = 0;   // its target's direct source before (0 when shared)
            CK_ID retail_shared = 0;   // or the input it shared its source with
            CK_ID retail_real = 0;     // what GetRealSource() answered before
        };

        // Finds the blocks and re-points them.  Empty on success; otherwise
        // why, and nothing was changed.  Once per world and anchor: a split
        // still applied (a previous session's whose restore failed) is
        // handed back first, and refused if it still cannot be.
        std::string apply(CKContext* context) {
            if (applied_ && !restore(context))
                return "a target of the previous split could not be handed back to CurrentLevel";
            rewires_.clear();
            others_.clear();
            if (!context) return "no context";
            auto* current_level = CKDataArray::Cast(
                context->GetObjectByNameAndClass(const_cast<CKSTRING>("CurrentLevel"), CKCID_DATAARRAY, nullptr));
            if (!current_level) return "the CurrentLevel array is missing";
            if (current_level->GetColumnCount() <= kColumn || current_level->GetRowCount() <= kRow)
                return "CurrentLevel has no cell [0][4]";
            CKBehavior* events = find_root_script(context, "Gameplay_Events");
            if (!events) return "Gameplay_Events is missing";
            CKBehavior* chain = find_in_tree(events, "activate next Checkpoint");
            if (!chain) return "the activate next Checkpoint group is missing";

            std::vector<rewire> found;
            // The writers: the chain's two Set Cells of the phase cell.
            std::vector<CKBehavior*> writers;
            collect(chain, "Set Cell", writers);
            for (CKBehavior* block: writers)
                if (is_phase_cell(block, current_level)) found.push_back(make_rewire(block, true));
            if (found.size() != 2)
                return "activate next Checkpoint has " + std::to_string(found.size())
                     + " Set Cell(s) of CurrentLevel[0][4], expected 2";
            // Event_handler's checkpoint resets (optional: a level may not
            // have them; their count is logged).
            size_t resets = 0;
            if (CKBehavior* handler = find_root_script(context, "Event_handler")) {
                std::vector<CKBehavior*> groups;
                find_all_in_tree(handler, "deact. Checkpoints", groups);
                for (CKBehavior* group: groups) {
                    std::vector<CKBehavior*> sets;
                    collect(group, "Set Cell", sets);
                    for (CKBehavior* block: sets)
                        if (is_phase_cell(block, current_level)) { found.push_back(make_rewire(block, true)); ++resets; }
                }
            }

            // The readers: the scripts of the checkpoint entities, the rows of
            // the array(s) the chain's (and set first Checkpoint's) Get Row read.
            std::vector<CKBehavior*> rows;
            collect(chain, "Get Row", rows);
            if (CKBehavior* first = find_in_tree(events, "set first Checkpoint")) collect(first, "Get Row", rows);
            std::vector<CKDataArray*> lists;
            for (CKBehavior* block: rows) {
                CKDataArray* list = target_array(block);
                if (list && list != current_level && std::find(lists.begin(), lists.end(), list) == lists.end())
                    lists.push_back(list);
            }
            if (lists.empty()) return "activate next Checkpoint reads no checkpoint array";
            std::vector<CK_ID> scripts;
            for (CKDataArray* list: lists)
                for (CKBeObject* entity: row_objects(context, list))
                    for (int s = 0; s < entity->GetScriptCount(); ++s)
                        if (CKBehavior* script = entity->GetScript(s))
                            if (std::find(scripts.begin(), scripts.end(), script->GetID()) == scripts.end())
                                scripts.push_back(script->GetID());
            size_t readers = 0;
            for (CK_ID id: scripts) {
                auto* script = CKBehavior::Cast(context->GetObject(id));
                std::vector<CKBehavior*> reads;
                collect(script, "Get Cell", reads);
                for (CKBehavior* block: reads) {
                    if (!is_phase_cell(block, current_level)) continue;
                    // A script the rollback replays reads the recorded cell,
                    // never an unrecorded copy (script_replay's selection).
                    if (script_replay::is_mechanism_script(script))
                        return std::string("the checkpoint entity script ") + (script->GetName() ? script->GetName() : "?")
                             + " reads CurrentLevel[0][4] and is one the script replay records";
                    found.push_back(make_rewire(block, false));
                    ++readers;
                }
            }
            if (readers == 0)
                return "no script of the " + std::to_string(scripts.size()) + " on the checkpoint entities reads CurrentLevel[0][4]";
            for (const auto& r: found)
                if (!r.retail_real) return "a target has no source";

            // The copy, and the parameter that names it.
            CKDependencies dependencies;
            dependencies.Resize(CKCID_MAXCLASSID);
            dependencies.Fill(0);
            dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
            dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
            dependencies[CKCID_DATAARRAY] = CK_DEPENDENCIES_COPY_DATAARRAY_DATA;
            auto* copy = CKDataArray::Cast(context->CopyObject(current_level, &dependencies,
                                                               const_cast<CKSTRING>("_BMMO_CheckpointPhase")));
            if (!copy) return "CopyObject failed for CurrentLevel";
            if (copy->GetRowCount() <= kRow) copy->AddRow();
            int retail_now = -1;
            current_level->GetElementValue(kRow, kColumn, &retail_now);
            int phase = 1;
            copy->SetElementValue(kRow, kColumn, &phase, sizeof(phase));
            CKParameterLocal* parameter = context->CreateCKParameterLocal(
                const_cast<CKSTRING>("BMMO_CheckpointPhase"), CKPGUID_DATAARRAY, TRUE);
            if (!parameter) {
                context->DestroyObject(copy);
                return "could not create the session's array parameter";
            }
            const CK_ID array_id = copy->GetID();
            parameter->SetValue(&array_id, sizeof(array_id));
            for (size_t i = 0; i < found.size(); ++i) {
                CKParameterIn* target = target_of(context, found[i].block);
                if (target && target->SetDirectSource(parameter) == CK_OK) continue;
                // Undo the ones done: half a split is worse than none.
                for (size_t k = 0; k < i; ++k) hand_back(context, found[k]);
                context->DestroyObject(parameter);
                context->DestroyObject(copy);
                return "a target refused the session's parameter";
            }
            rewires_ = std::move(found);
            array_ = array_id;
            parameter_ = parameter->GetID();
            retail_now_ = retail_now;
            resets_ = resets;
            scripts_ = scripts.size();
            applied_ = true;
            others_ = other_writers(context, current_level);
            return {};
        }

        // Re-points a target something handed back to its retail source (a
        // level reset restoring the graph).  The number re-pointed.
        int keep(CKContext* context) {
            if (!applied_ || !context) return 0;
            auto* parameter = CKParameter::Cast(context->GetObject(parameter_));
            if (!parameter) return 0;
            int fixed = 0;
            for (const auto& r: rewires_) {
                CKParameterIn* target = target_of(context, r.block);
                if (!target) continue;
                CKParameter* now = target->GetRealSource();
                if (now == parameter || !now || now->GetID() != r.retail_real) continue;
                if (target->SetDirectSource(parameter) == CK_OK) ++fixed;
            }
            return fixed;
        }

        // Session end (the retail mod) and a new session (the headless
        // client): the retail sources back, the copy and the parameter
        // destroyed once nothing reads them.  False when a target could not
        // be handed back: the copy, the parameter and the targets not handed
        // back are kept, so a later restore (or apply) tries them again.
        bool restore(CKContext* context) {
            if (!applied_ || !context) {
                *this = checkpoint_phase_split{};
                return true;
            }
            std::vector<rewire> left;
            for (const auto& r: rewires_)
                if (!hand_back(context, r)) left.push_back(r);
            if (!left.empty()) {
                rewires_ = std::move(left);
                return false;
            }
            if (CKObject* p = context->GetObject(parameter_)) context->DestroyObject(p);
            if (CKObject* a = context->GetObject(array_)) context->DestroyObject(a);
            *this = checkpoint_phase_split{};
            return true;
        }

        bool applied() const { return applied_; }
        CK_ID array() const { return array_; }
        size_t writers() const { size_t n = 0; for (const auto& r: rewires_) n += r.writer ? 1 : 0; return n; }
        size_t readers() const { return rewires_.size() - writers(); }
        size_t resets() const { return resets_; }
        size_t scripts() const { return scripts_; }
        // The writers of CurrentLevel[0][4] left on it, "root/group xN, ...":
        // the manager and whatever else the level has (for the log).
        const std::string& others() const { return others_; }

        // One line for the log of a world that applied it.
        std::string describe() const {
            return std::to_string(writers() - resets_) + " Set Cells of Gameplay_Events/activate next Checkpoint, "
                 + std::to_string(resets_) + " of Event_handler/deact. Checkpoints and "
                 + std::to_string(readers()) + " Get Cells in " + std::to_string(scripts_)
                 + " checkpoint script(s) now use a private copy of CurrentLevel[0][4] (1; the cell is "
                 + std::to_string(retail_now_) + " now); left on CurrentLevel: "
                 + (others_.empty() ? std::string("none") : others_);
        }

    private:
        std::vector<rewire> rewires_;
        CK_ID array_ = 0;
        CK_ID parameter_ = 0;
        int retail_now_ = 0;
        size_t resets_ = 0;
        size_t scripts_ = 0;
        bool applied_ = false;
        std::string others_;

        static CKBehavior* find_in_tree(CKBehavior* parent, const char* name) {
            if (!parent) return nullptr;
            const int count = parent->GetSubBehaviorCount();
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = parent->GetSubBehavior(i);
                if (!sub) continue;
                if (sub->GetName() && std::strcmp(sub->GetName(), name) == 0) return sub;
                if (CKBehavior* found = find_in_tree(sub, name)) return found;
            }
            return nullptr;
        }

        static void find_all_in_tree(CKBehavior* parent, const char* name, std::vector<CKBehavior*>& out) {
            if (!parent) return;
            const int count = parent->GetSubBehaviorCount();
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = parent->GetSubBehavior(i);
                if (!sub) continue;
                if (sub->GetName() && std::strcmp(sub->GetName(), name) == 0) out.push_back(sub);
                else find_all_in_tree(sub, name, out);
            }
        }

        // Every building block in `parent`'s tree whose prototype is `prototype`.
        static void collect(CKBehavior* parent, const char* prototype, std::vector<CKBehavior*>& out) {
            if (!parent) return;
            if (parent->IsUsingFunction()) {
                const char* name = parent->GetPrototypeName();
                if (name && std::strcmp(name, prototype) == 0) out.push_back(parent);
                return;
            }
            const int count = parent->GetSubBehaviorCount();
            for (int i = 0; i < count; ++i) collect(parent->GetSubBehavior(i), prototype, out);
        }

        static CKParameterIn* target_of(CKContext* context, CK_ID block_id) {
            auto* block = CKBehavior::Cast(context->GetObject(block_id));
            return block ? block->GetTargetParameter() : nullptr;
        }

        static CKDataArray* target_array(CKBehavior* block) {
            CKParameterIn* target = block ? block->GetTargetParameter() : nullptr;
            CKParameter* source = target ? target->GetRealSource() : nullptr;
            return source ? CKDataArray::Cast(source->GetValueObject()) : nullptr;
        }

        static bool int_input(CKBehavior* block, int index, int& value) {
            CKParameterIn* in = index < block->GetInputParameterCount() ? block->GetInputParameter(index) : nullptr;
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            if (!source || source->GetDataSize() != static_cast<int>(sizeof(int))) return false;
            return source->GetValue(&value, FALSE) == CK_OK;   // no operation run for it
        }

        // A Set Cell / Get Cell of `array`[0][4] (pins 0 and 1: Row, Column).
        static bool is_phase_cell(CKBehavior* block, CKDataArray* array) {
            if (target_array(block) != array) return false;
            int row = -1, column = -1;
            return int_input(block, 0, row) && int_input(block, 1, column) && row == kRow && column == kColumn;
        }

        static rewire make_rewire(CKBehavior* block, bool writer) {
            rewire r;
            r.block = block->GetID();
            r.writer = writer;
            CKParameterIn* target = block->GetTargetParameter();
            if (CKParameterIn* shared = target->GetSharedSource()) r.retail_shared = shared->GetID();
            else if (CKParameter* direct = target->GetDirectSource()) r.retail_direct = direct->GetID();
            if (CKParameter* real = target->GetRealSource()) r.retail_real = real->GetID();
            return r;
        }

        static bool hand_back(CKContext* context, const rewire& r) {
            CKParameterIn* target = target_of(context, r.block);
            if (!target) return true;   // the block is gone; nothing reads the copy through it
            if (r.retail_shared) {
                auto* shared = CKParameterIn::Cast(context->GetObject(r.retail_shared));
                return shared && target->ShareSourceWith(shared) == CK_OK;
            }
            auto* direct = CKParameter::Cast(context->GetObject(r.retail_direct));
            return direct && target->SetDirectSource(direct) == CK_OK;
        }

        // The entities in the object columns of `list` (the checkpoints).
        static std::vector<CKBeObject*> row_objects(CKContext* context, CKDataArray* list) {
            std::vector<CKBeObject*> out;
            CKParameterManager* pm = context->GetParameterManager();
            for (int c = 0; c < list->GetColumnCount(); ++c) {
                const CK_ARRAYTYPE type = list->GetColumnType(c);
                const bool object_parameter = type == CKARRAYTYPE_PARAMETER && pm
                    && pm->IsDerivedFrom(list->GetColumnParameterGuid(c), CKPGUID_OBJECT);
                if (type != CKARRAYTYPE_OBJECT && !object_parameter) continue;
                for (int r = 0; r < list->GetRowCount(); ++r) {
                    CKObject* object = nullptr;
                    if (type == CKARRAYTYPE_OBJECT) object = list->GetElementObject(r, c);
                    else {
                        CK_ID id = 0;
                        if (list->GetElementValue(r, c, &id)) object = context->GetObject(id);
                    }
                    auto* entity = CKBeObject::Cast(object);
                    if (entity && std::find(out.begin(), out.end(), entity) == out.end()) out.push_back(entity);
                }
            }
            return out;
        }

        // Every Set Cell still writing `array`[0][4], by root script / group.
        static std::string other_writers(CKContext* context, CKDataArray* array) {
            std::map<std::string, int> counts;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                auto* block = CKBehavior::Cast(context->GetObject(ids[i]));
                if (!block || !block->IsUsingFunction() || !block->GetPrototypeName()) continue;
                if (std::strcmp(block->GetPrototypeName(), "Set Cell") != 0 || !is_phase_cell(block, array)) continue;
                CKBehavior* group = block->GetParent();
                CKBehavior* root = block;
                while (root->GetParent()) root = root->GetParent();
                std::string where = root->GetName() ? root->GetName() : "?";
                if (group && group != root) where += std::string("/") + (group->GetName() ? group->GetName() : "?");
                ++counts[where];
            }
            std::string text;
            for (const auto& [where, n]: counts) text += (text.empty() ? "" : ", ") + where + " x" + std::to_string(n);
            return text;
        }
    };
}
