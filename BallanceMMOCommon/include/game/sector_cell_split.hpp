#pragma once

// The checkpoint chain's own sector cell, for every world (step B2SX, the
// [0][1] cell; resync design 7.4, As built (step B2SW), N2's finding).
//
// Crossing a checkpoint runs Gameplay_Events / activate Sektor: a Set Cell
// writes IngameParameter[0][2] (the sector to deactivate, pinned to 0 by N2,
// game/sector_deactivate_pin.hpp), a Set Cell writes IngameParameter[0][1]
// (the sector to activate), and an Execute Script runs Gameplay_SectorManager,
// whose Start reads both cells.  In a session that Execute Script is held
// (game/sector_activation.hpp): the manager is run by the sector union's
// passes only (run_sector_pass: [0][2] = 0, [0][1] = the sector J).  But the
// chain's [0][1] Set Cell still runs on a crossing, and when an own crossing
// of sector K (or, on the server, a clone's) falls in the frame of a pass of
// another sector J, it writes K after the pass wrote J and before the
// manager's Start reads the cell: that world activates K instead of J (the
// union says {1,3}, sector 2's 14 bodies are made instead of sector 3's 6;
// SimTool --level 13 --drop P_Ball_Wood_MF 0 --drop-at 220 3.65 15
// --drop-sector 1 --drop-move 3 80 --cells-trace, T 79 and 81).
//
// The chain's write has one reader left in a session: the own sector poll
// (session_client_core::poll_own_sector, through the host's poll_sector),
// which takes a change of the cell for this ball's crossing.  So the chain's
// [0][1] Set Cell gets a cell of its own, as the checkpoint phase cell did
// (game/checkpoint_phase.hpp): a private copy of IngameParameter, named by a
// session-owned parameter that feeds the block's Target.  The manager then
// reads only what run_sector_pass wrote, on every world, and the own sector
// poll reads the copy (sector_poll::cell_private), which only the chain
// writes - a pass no longer writes the cell the poll watches.  The copy starts
// at IngameParameter[0][1] as the anchor has it (the value the core's poll
// starts from, anchor_report::sector_cell).
//
// Applied only where the hold is in force: without it the chain runs the
// manager itself, which must read the chain's own [0][1].  Once per world at
// its anchor: the server's world (destroyed with its session, never handed
// back), the headless client's (handed back when the next session begins:
// base.cmo's chain survives a level exit), the retail mod's with the session
// core on (handed back at session end; the legacy path, core off, keeps the
// cell it polls).  The copy and the parameter are new CK objects: everything
// made after the anchor gets other CK_IDs, which reorders a journal
// checkpoint's body list (list_bodies), never the simulation.
//
// Only the public CK2 API is used: the header compiles against the Virtools
// SDK (client mod) and against the Ballanced engine (headless server).

#include <cstring>
#include <string>

#include "CKAll.h"

#include "menu_driver.hpp"
#include "sector_deactivate_pin.hpp"

namespace bmmo::game {
    class sector_cell_split {
    public:
        static constexpr int kRow = 0;
        static constexpr int kColumn = 1;

        // Finds Gameplay_Events / activate Sektor's one Set Cell of
        // IngameParameter[0][1] and re-points its Target at a private copy.
        // Empty on success; otherwise why, and nothing was changed.  A split
        // still applied (a previous session's whose restore failed) is handed
        // back first, and refused if it still cannot be.
        std::string apply(CKContext* context) {
            if (applied_ && !restore(context))
                return "the target of the previous split could not be handed back to IngameParameter";
            if (!context) return "no context";
            auto* parameters = CKDataArray::Cast(
                context->GetObjectByNameAndClass(const_cast<CKSTRING>("IngameParameter"), CKCID_DATAARRAY, nullptr));
            if (!parameters) return "the IngameParameter array is missing";
            if (parameters->GetColumnCount() <= kColumn || parameters->GetRowCount() <= kRow)
                return "IngameParameter has no cell [0][1]";
            CKBehavior* events = find_root_script(context, "Gameplay_Events");
            if (!events) return "Gameplay_Events is missing";
            // A direct sub-behaviour, as N2 finds it (find_direct_child).
            CKBehavior* chain = find_direct_child(events, "activate Sektor");
            if (!chain) return "the activate Sektor group is missing";
            CKBehavior* block = nullptr;
            const int subs = chain->GetSubBehaviorCount();
            for (int i = 0; i < subs; ++i) {
                CKBehavior* sub = chain->GetSubBehavior(i);
                const char* prototype = sub && sub->IsUsingFunction() ? sub->GetPrototypeName() : nullptr;
                if (!prototype || std::strcmp(prototype, "Set Cell") != 0) continue;
                if (!is_sector_cell(sub, parameters)) continue;
                if (block) return "more than one Set Cell of activate Sektor writes IngameParameter[0][1]";
                block = sub;
            }
            if (!block) return "no Set Cell of activate Sektor writes IngameParameter[0][1]";
            CKParameterIn* target = block->GetTargetParameter();
            if (!target) return "the Set Cell has no Target input";
            CKParameter* real = target->GetRealSource();
            if (!real) return "its Target has no source";

            CKDependencies dependencies;
            dependencies.Resize(CKCID_MAXCLASSID);
            dependencies.Fill(0);
            dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
            dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
            dependencies[CKCID_DATAARRAY] = CK_DEPENDENCIES_COPY_DATAARRAY_DATA;
            auto* copy = CKDataArray::Cast(context->CopyObject(parameters, &dependencies,
                                                               const_cast<CKSTRING>("_BMMO_SectorCell")));
            if (!copy) return "CopyObject failed for IngameParameter";
            int now = 0;
            parameters->GetElementValue(kRow, kColumn, &now);
            int copied = -1;
            if (copy->GetRowCount() <= kRow || copy->GetColumnCount() <= kColumn
                    || !copy->GetElementValue(kRow, kColumn, &copied) || copied != now) {
                context->DestroyObject(copy);
                return "the copy of IngameParameter does not hold its cell [0][1]";
            }
            CKParameterLocal* parameter = context->CreateCKParameterLocal(
                const_cast<CKSTRING>("BMMO_SectorCell"), CKPGUID_DATAARRAY, TRUE);
            if (!parameter) {
                context->DestroyObject(copy);
                return "could not create the session's array parameter";
            }
            const CK_ID array_id = copy->GetID();
            parameter->SetValue(&array_id, sizeof(array_id));
            CKParameterIn* shared = target->GetSharedSource();
            CKParameter* direct = target->GetDirectSource();
            if (target->SetDirectSource(parameter) != CK_OK) {
                context->DestroyObject(parameter);
                context->DestroyObject(copy);
                return "the Set Cell's Target refused the session's parameter";
            }
            block_ = block->GetID();
            retail_shared_ = shared ? shared->GetID() : 0;
            retail_direct_ = shared ? 0 : (direct ? direct->GetID() : 0);
            retail_real_ = real->GetID();
            array_ = array_id;
            parameter_ = parameter->GetID();
            start_ = now;
            applied_ = true;
            return {};
        }

        // Re-points the Target if something handed it back to its retail
        // source (a level reset restoring the graph).  True when it did.
        bool keep(CKContext* context) {
            if (!applied_ || !context) return false;
            auto* parameter = CKParameter::Cast(context->GetObject(parameter_));
            CKParameterIn* target = target_of(context);
            if (!parameter || !target) return false;
            CKParameter* now = target->GetRealSource();
            if (now == parameter || !now || now->GetID() != retail_real_) return false;
            return target->SetDirectSource(parameter) == CK_OK;
        }

        // The retail source back, the copy and the parameter destroyed.  False
        // when the Target could not be handed back: everything is kept so a
        // later restore (or apply) tries again.
        bool restore(CKContext* context) {
            if (!applied_ || !context) {
                *this = sector_cell_split{};
                return true;
            }
            if (CKParameterIn* target = target_of(context)) {
                bool back = false;
                if (retail_shared_) {
                    auto* shared = CKParameterIn::Cast(context->GetObject(retail_shared_));
                    back = shared && target->ShareSourceWith(shared) == CK_OK;
                } else {
                    auto* direct = CKParameter::Cast(context->GetObject(retail_direct_));
                    back = direct && target->SetDirectSource(direct) == CK_OK;
                }
                if (!back) return false;
            }
            if (CKObject* p = context->GetObject(parameter_)) context->DestroyObject(p);
            if (CKObject* a = context->GetObject(array_)) context->DestroyObject(a);
            *this = sector_cell_split{};
            return true;
        }

        // The chain's cell as it stands (what the own sector poll reads).
        bool read(CKContext* context, int& cell) const {
            if (!applied_ || !context) return false;
            auto* copy = CKDataArray::Cast(context->GetObject(array_));
            return copy && copy->GetElementValue(kRow, kColumn, &cell);
        }

        bool applied() const { return applied_; }

        // One line for the log of a world that applied it.
        std::string describe() const {
            return "the Set Cell of Gameplay_Events/activate Sektor writes a private copy of IngameParameter[0][1] (from "
                 + std::to_string(start_) + "); the sector manager reads only the union's passes there";
        }

    private:
        CK_ID block_ = 0;
        CK_ID retail_shared_ = 0;   // the Target shared its source with this input
        CK_ID retail_direct_ = 0;   // or read this parameter directly
        CK_ID retail_real_ = 0;     // what GetRealSource() answered before
        CK_ID array_ = 0;
        CK_ID parameter_ = 0;
        int start_ = 0;
        bool applied_ = false;

        CKParameterIn* target_of(CKContext* context) const {
            auto* block = CKBehavior::Cast(context->GetObject(block_));
            return block ? block->GetTargetParameter() : nullptr;
        }

        static bool int_input(CKBehavior* block, int index, int& value) {
            CKParameterIn* in = index < block->GetInputParameterCount() ? block->GetInputParameter(index) : nullptr;
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            if (!source || source->GetDataSize() != static_cast<int>(sizeof(int))) return false;
            return source->GetValue(&value, FALSE) == CK_OK;   // no operation run for it
        }

        // A Set Cell of `array`[0][1] (pins 0 and 1: Row, Column), its Target
        // reading that array.
        static bool is_sector_cell(CKBehavior* block, CKDataArray* array) {
            CKParameterIn* target = block->GetTargetParameter();
            CKParameter* source = target ? target->GetRealSource() : nullptr;
            if (!source || CKDataArray::Cast(source->GetValueObject()) != array) return false;
            int row = -1, column = -1;
            return int_input(block, 0, row) && int_input(block, 1, column) && row == kRow && column == kColumn;
        }
    };
}
