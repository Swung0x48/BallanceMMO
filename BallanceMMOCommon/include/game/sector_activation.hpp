#pragma once

// The level's own sector activation on a crossed checkpoint, held.
//
// Crossing a checkpoint runs Gameplay_Events / activate Sektor: two Set Cells
// write IngameParameter[0][1] (the sector to activate) and [0][2] (the one to
// deactivate), and an Execute Script runs Gameplay_SectorManager on them in
// the same frame.  In a physics session that run is an INPUT
// (docs/lockstep-without-snapshots.md, "sector activation as an input"): it
// has to happen at the same tick on every side, and every world that sees a
// crossing at all sees it at its own tick - the server's rewired proximity
// probes fire for any player's clone, a client's chain only for its own ball.
// Measured: a retail client's crossing had the server's own chain activate the
// next sector 14 ticks before the tick the client stamped, and deactivate the
// previous one with it, whoever was still in it.
//
// So the Execute Script is fed a parameter that holds no script.  Execute
// Script activates its Out at once on a null script (ExecuteScript.cpp:66-70),
// so the chain, its two Set Cells and whatever hangs behind it keep their
// shape; only the manager run is gone.  The activation is then scheduled: by
// the server's update_sectors() from the stamped Sector event, and by every
// client at that same tick (the session clients' sector holds).
//
// The retail client does the same with its own copy (physics_session_client's
// N3), which also hands the original source back at session end; the server's
// world and the headless client's never need to.
//
// Only the public CK2 API is used: the header compiles against the Virtools
// SDK (client mod) and against the Ballanced engine (headless server).

#include <cstring>
#include <string>

#include "CKAll.h"

#include "menu_driver.hpp"

namespace bmmo::game {
    // Depth first: the first behaviour named `name` in `parent`'s tree, the
    // parent itself excluded.
    inline CKBehavior* find_behavior_in_tree(CKBehavior* parent, const char* name) {
        if (!parent || !name) return nullptr;
        const int count = parent->GetSubBehaviorCount();
        for (int i = 0; i < count; ++i) {
            CKBehavior* sub = parent->GetSubBehavior(i);
            if (!sub) continue;
            if (sub->GetName() && std::strcmp(sub->GetName(), name) == 0) return sub;
            if (CKBehavior* found = find_behavior_in_tree(sub, name)) return found;
        }
        return nullptr;
    }

    // The Execute Script of Gameplay_Events / activate Sektor whose Script
    // input names Gameplay_SectorManager, or null with `error` set.  Exactly
    // one is expected: a graph with none or two is not one this understands.
    inline CKBehavior* checkpoint_activation_block(CKContext* context, std::string& error) {
        CKBehavior* events = find_root_script(context, "Gameplay_Events");
        if (!events) { error = "Gameplay_Events is missing"; return nullptr; }
        CKBehavior* chain = find_behavior_in_tree(events, "activate Sektor");
        if (!chain) { error = "the activate Sektor group is missing"; return nullptr; }
        CKBehavior* block = nullptr;
        const int count = chain->GetSubBehaviorCount();
        for (int i = 0; i < count; ++i) {
            CKBehavior* sub = chain->GetSubBehavior(i);
            if (!sub || !sub->IsUsingFunction() || !sub->GetPrototypeName()) continue;
            if (std::strcmp(sub->GetPrototypeName(), "Execute Script") != 0) continue;
            CKObject* target = sub->GetInputParameterObject(1);   // "Script"
            CKBehavior* script = as_behavior(target);
            if (!script || !script->GetName() || std::strcmp(script->GetName(), "Gameplay_SectorManager") != 0) continue;
            if (block) { error = "more than one Execute Script in activate Sektor runs Gameplay_SectorManager"; return nullptr; }
            block = sub;
        }
        if (!block) error = "no Execute Script in activate Sektor runs Gameplay_SectorManager";
        return block;
    }

    // Feeds that block's Script input a parameter holding no script.  Returns
    // the block (null with `error` set when the graph is not the expected one
    // or the input refused the parameter).  Once per world, at its anchor: a
    // held block no longer names the manager, so a second call finds nothing.
    inline CKBehavior* hold_checkpoint_activation(CKContext* context, std::string& error) {
        CKBehavior* block = checkpoint_activation_block(context, error);
        if (!block) return nullptr;
        CKParameterIn* input = block->GetInputParameter(1);
        if (!input || input->GetGUID() != CKPGUID_SCRIPT) { error = "its Script input is not a script parameter"; return nullptr; }
        CKParameter* none = context->CreateCKParameterLocal(const_cast<CKSTRING>("BMMO_SectorHold"), CKPGUID_SCRIPT, TRUE);
        if (!none) { error = "could not create the hold's parameter"; return nullptr; }
        const CK_ID no_script = 0;
        none->SetValue(&no_script, sizeof(no_script));
        if (input->SetDirectSource(none) != CK_OK) {
            context->DestroyObject(none);
            error = "the Script input refused the hold's parameter";
            return nullptr;
        }
        return block;
    }
}
