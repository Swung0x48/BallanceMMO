#pragma once

// N2, the checkpoint deactivation, for every world (step B2SW; resync design
// 7.4, As built (step A4), "N2 exists only in the retail mod").
//
// Crossing a checkpoint runs Gameplay_Events / activate Sektor, which writes
// IngameParameter[0][1] (the sector to activate) and IngameParameter[0][2]
// (the sector to deactivate) and then runs Gameplay_SectorManager, whose
// "Deactivate Sector" group resets and hides everything of the named sector.
// In a session that run is held (game/sector_activation.hpp): the manager is
// run by the sector union's passes only (run_sector_pass: [0][2] = 0, [0][1] =
// the sector), and the union deactivates nothing anywhere.  But the chain's
// two Set Cells still run on a crossing, and when an own crossing (or, on the
// server, a clone's) falls in the frame of a pass, they write the cells after
// the pass did and before the manager's Start reads them: that world alone
// deactivates the sector the ball left (A4: a4_resync_n2 client 1,
// P_Modul_26_Sack 8498 m).  Event_handler / reset Level writes the same cell
// before it runs the manager (design 9.28b).
//
// So the [0][2] Set Cell of both chains is fed a constant 0 of the session's:
// the manager's own "Test (Not Equal, B = 0)" then skips the deactivation,
// whatever frame the chain runs in.  The source is swapped rather than
// emptied, because the [0][1] Set Cell reads the same kind of parameter and
// must keep working (the own sector poll reads that cell).  The retail mod
// pinned exactly this since design 9.25 (physics_session_client.cpp
// sector_deactivate_*), and hands the original source back at session end;
// since step B2SW that is a use of this header, and the headless client and
// the server's world pin the same two blocks at their anchor.  The server's
// world is booted for its session and destroyed with it (physics_world::
// create), so it never hands anything back.  The headless client's
// base.cmo survives a level exit, so it keeps its pins: re-pinned in its
// frame guard like the retail mod's, handed back when the next session
// begins (unpin_checkpoint_deactivation).
//
// Only the public CK2 API is used: the header compiles against the Virtools
// SDK (client mod) and against the Ballanced engine (headless server).

#include <cstring>
#include <string>
#include <vector>

#include "CKAll.h"

#include "menu_driver.hpp"
#include "sector_activation.hpp"

namespace bmmo::game {
    // One pinned block: the Set Cell writing IngameParameter[0][2], its Value
    // input, what that input read before (a direct source, or the input it
    // shared its source with, and the real source either way), and the
    // session's constant.
    struct deactivate_pin {
        CK_ID block = 0;
        int input = 2;               // Set Cell's "Value"
        CK_ID retail_source = 0;     // the parameter it read (direct source)
        CK_ID retail_shared = 0;     // or the input it shared its source with
        CK_ID retail_real = 0;       // what GetRealSource() answered before the pin
        CK_ID zero = 0;              // the constant int parameter
        bool target_known = false;   // the block names IngameParameter as its target here
    };

    // `id` as a block of the prototype `prototype`, else null.
    inline CKBehavior* pinned_block(CKContext* context, CK_ID id, const char* prototype) {
        auto* block = CKBehavior::Cast(context->GetObject(id));
        if (!block || !block->IsUsingFunction()) return nullptr;
        const char* name = block->GetPrototypeName();
        return name && std::strcmp(name, prototype) == 0 ? block : nullptr;
    }

    // Finds the one Set Cell of `chain` writing IngameParameter[0][2] (row 0,
    // column 2; a block that names another array is not it, one whose target
    // cannot be read here still counts - the chain and the cell identify it)
    // and feeds its Value input a fresh int parameter holding 0.  False with
    // `error` set when the graph is not the expected one; nothing is changed
    // then.
    inline bool pin_deactivate_cell(CKContext* context, CKBehavior* chain, CKDataArray* parameters, deactivate_pin& out,
                                    std::string& error) {
        out = deactivate_pin{};
        if (!context || !chain) { error = "the group is missing"; return false; }
        if (!parameters) { error = "the IngameParameter array is missing"; return false; }
        CKBehavior* block = nullptr;
        bool target_known = false;
        const int subs = chain->GetSubBehaviorCount();
        for (int i = 0; i < subs; ++i) {
            CKBehavior* sub = chain->GetSubBehavior(i);
            const char* prototype = sub && sub->IsUsingFunction() ? sub->GetPrototypeName() : nullptr;
            if (!prototype || std::strcmp(prototype, "Set Cell") != 0) continue;
            int row = -1, column = -1;
            sub->GetInputParameterValue(0, &row);
            sub->GetInputParameterValue(1, &column);
            if (row != 0 || column != 2) continue;   // [0][1] is the activate write: it stays
            CKBeObject* target = sub->GetTarget();
            if (target && target != static_cast<CKBeObject*>(parameters)) continue;
            if (block) { error = "more than one Set Cell writes IngameParameter[0][2]"; return false; }
            block = sub;
            target_known = target != nullptr;
        }
        if (!block) { error = "no Set Cell in that group writes IngameParameter[0][2]"; return false; }
        CKParameterIn* input = block->GetInputParameter(2);   // "Value"
        if (!input) { error = "the Set Cell block has no Value input"; return false; }
        CKParameter* source = input->GetRealSource();
        if (!source) { error = "its Value input has no source"; return false; }
        if (source->GetGUID() != CKPGUID_INT || source->GetDataSize() != static_cast<int>(sizeof(int))) {
            error = "its Value input is not an int parameter";
            return false;
        }
        auto* zero = context->CreateCKParameterLocal(const_cast<CKSTRING>("BMMO_SectorKeep"), CKPGUID_INT, TRUE);
        if (!zero) { error = "could not create the session's constant"; return false; }
        const int none = 0;
        zero->SetValue(&none, sizeof(none));
        CKParameterIn* shared = input->GetSharedSource();
        CKParameter* direct = input->GetDirectSource();
        if (input->SetDirectSource(zero) != CK_OK) {
            context->DestroyObject(zero);
            error = "the Value input refused the session's constant";
            return false;
        }
        out.block = block->GetID();
        out.input = 2;
        out.retail_source = direct ? direct->GetID() : 0;
        out.retail_shared = shared ? shared->GetID() : 0;
        out.retail_real = source->GetID();
        out.zero = zero->GetID();
        out.target_known = target_known;
        return true;
    }

    // Moves the input back onto the constant, only from the source it had
    // when the pin was made (anything else belongs to a graph the pin no
    // longer describes).  True when it re-pinned.
    inline bool repin_deactivate_cell(CKContext* context, const deactivate_pin& pin) {
        if (!context || !pin.block || !pin.zero) return false;
        CKBehavior* block = pinned_block(context, pin.block, "Set Cell");
        CKParameterIn* input = block ? block->GetInputParameter(pin.input) : nullptr;
        auto* zero = CKParameter::Cast(context->GetObject(pin.zero));
        if (!input || !zero) return false;
        CKParameter* current = input->GetRealSource();
        if (current == zero) return false;   // still pinned
        if (!current || current->GetID() != pin.retail_real) return false;
        return input->SetDirectSource(zero) == CK_OK;
    }

    // Hands the input back to its original source and destroys the constant
    // (kept when the input still reads it and could not be handed back:
    // destroying it would leave a dangling source).  False when the input was
    // left on the constant.
    inline bool unpin_deactivate_cell(CKContext* context, const deactivate_pin& pin) {
        if (!context) return true;
        CKBehavior* block = pin.block ? pinned_block(context, pin.block, "Set Cell") : nullptr;
        CKParameterIn* input = block ? block->GetInputParameter(pin.input) : nullptr;
        auto* zero = pin.zero ? CKParameter::Cast(context->GetObject(pin.zero)) : nullptr;
        bool referenced = false;
        if (input && zero && input->GetRealSource() == zero) {
            referenced = true;
            if (pin.retail_shared) {
                auto* shared = CKParameterIn::Cast(context->GetObject(pin.retail_shared));
                if (shared && input->ShareSourceWith(shared) == CK_OK) referenced = false;
            } else {
                auto* source = CKParameter::Cast(context->GetObject(pin.retail_source));
                if (source && input->SetDirectSource(source) == CK_OK) referenced = false;
            }
        }
        if (zero && !referenced) context->DestroyObject(zero);
        return !referenced;
    }

    // The first direct sub-behaviour of `script` named `name` (BML's
    // ScriptHelper::FindFirstBB with its default, not hierarchical: what the
    // retail mod resolves the two chains with - Event_handler has a nested
    // behaviour named "reset Level" too, which a depth-first search meets
    // first).
    inline CKBehavior* find_direct_child(CKBehavior* script, const char* name) {
        if (!script || !name) return nullptr;
        const int count = script->GetSubBehaviorCount();
        for (int i = 0; i < count; ++i) {
            CKBehavior* sub = script->GetSubBehavior(i);
            if (sub && sub->GetName() && std::strcmp(sub->GetName(), name) == 0) return sub;
        }
        return nullptr;
    }

    // The two chains N2 pins, in order: the checkpoint's and the level
    // reset's.
    struct deactivate_chain {
        const char* script;
        const char* group;
    };
    inline const deactivate_chain* deactivate_chains(int& count) {
        static const deactivate_chain chains[] = {{"Gameplay_Events", "activate Sektor"}, {"Event_handler", "reset Level"}};
        count = 2;
        return chains;
    }

    // The headless client's and the server's world, at their anchor: both
    // chains pinned.  Returns how many were pinned; `report` names each
    // chain's outcome; `pins` (when given) receives the pins made, for
    // repin_checkpoint_deactivation and unpin_checkpoint_deactivation (the
    // server's world, destroyed with its session, keeps none).
    inline int pin_checkpoint_deactivation(CKContext* context, std::string& report,
                                           std::vector<deactivate_pin>* pins = nullptr) {
        report.clear();
        if (pins) pins->clear();
        int count = 0, pinned = 0;
        const deactivate_chain* chains = deactivate_chains(count);
        auto* parameters = CKDataArray::Cast(
                context ? context->GetObjectByNameAndClass(const_cast<CKSTRING>("IngameParameter"), CKCID_DATAARRAY, nullptr)
                        : nullptr);
        for (int i = 0; i < count; ++i) {
            if (!report.empty()) report += "; ";
            report += std::string(chains[i].script) + "/" + chains[i].group + ": ";
            CKBehavior* script = context ? find_root_script(context, chains[i].script) : nullptr;
            if (!script) { report += "the script is missing"; continue; }
            deactivate_pin pin;
            std::string error;
            if (!pin_deactivate_cell(context, find_direct_child(script, chains[i].group), parameters, pin, error)) {
                report += "NOT pinned (" + error + ")";
                continue;
            }
            ++pinned;
            if (pins) pins->push_back(pin);
            report += pin.target_known ? "pinned" : "pinned (the block does not name its array here)";
        }
        return pinned;
    }

    // The frame guard's keep: every pin moved back onto its constant if
    // something handed the input back to the source it had.  Returns how
    // many were re-pinned.
    inline int repin_checkpoint_deactivation(CKContext* context, const std::vector<deactivate_pin>& pins) {
        int again = 0;
        for (const auto& pin: pins)
            if (repin_deactivate_cell(context, pin)) ++again;
        return again;
    }

    // Hands every pin back and forgets them.  Returns how many inputs had to
    // be left on their constant (0 normally).
    inline int unpin_checkpoint_deactivation(CKContext* context, std::vector<deactivate_pin>& pins) {
        int left = 0;
        for (const auto& pin: pins)
            if (!unpin_deactivate_cell(context, pin)) ++left;
        pins.clear();
        return left;
    }
}
