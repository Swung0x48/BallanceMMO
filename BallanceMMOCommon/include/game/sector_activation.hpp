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
// The clients keep the hold (FULL step N3K, host-platform audit D3): a level
// reset that restored the graph would hand the input its retail source back,
// and the next crossing would run the manager in that world alone.  Both
// session clients - the headless host and the retail core host - hold it
// with checkpoint_activation_hold below, re-hold it every frame (keep(), the
// rule of session/checkpoint_hold_rule.hpp) and hand the retail source back
// at the session's end.  The server's world holds once (hold_checkpoint_
// activation) and never resets a level.  The legacy retail path's own copy
// (physics_session_client.cpp N3) was deleted with that path at R7 (FULL
// step LGD).
//
// Only the public CK2 API is used: the header compiles against the Virtools
// SDK (client mod) and against the Ballanced engine (headless server).

#include <cstring>
#include <string>

#include <session/checkpoint_hold_rule.hpp>

#include "CKAll.h"

#include "menu_driver.hpp"
#include "script_record_reuse.hpp"

namespace bmmo::game {
    // One pass of the sector union, the writes every world makes (the
    // server's update_sectors, the headless host's and the retail core
    // host's platform run_sector; written once at step HU1): deactivate
    // nothing (IngameParameter[0][2] = 0), activate `sector` ([0][1]) and run
    // Gameplay_SectorManager from its start.
    inline void run_sector_pass(CKContext* context, CKDataArray* parameters, CKBehavior* manager, int sector) {
        int none = 0;
        parameters->SetElementValue(0, 2, &none, sizeof(none));
        parameters->SetElementValue(0, 1, &sector, sizeof(sector));
        if (CKScene* scene = context->GetCurrentScene()) scene->Activate(manager, TRUE);
        note_script_write();   // step OPC: the host activates the manager (game/script_record_reuse.hpp)
    }

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

    // The hold's parameter - a script parameter holding no script - made and
    // set as `input`'s direct source.  Null with `error` set when it could not
    // be made or the input refused it (nothing is left behind then).
    inline CKParameter* feed_checkpoint_hold(CKContext* context, CKParameterIn* input, std::string& error) {
        CKParameter* none = context->CreateCKParameterLocal(const_cast<CKSTRING>("BMMO_SectorHold"), CKPGUID_SCRIPT, TRUE);
        if (!none) { error = "could not create the hold's parameter"; return nullptr; }
        const CK_ID no_script = 0;
        none->SetValue(&no_script, sizeof(no_script));
        if (input->SetDirectSource(none) != CK_OK) {
            context->DestroyObject(none);
            error = "the Script input refused the hold's parameter";
            return nullptr;
        }
        return none;
    }

    // Feeds that block's Script input a parameter holding no script.  Returns
    // the block (null with `error` set when the graph is not the expected one
    // or the input refused the parameter).  Once per world, at its anchor: a
    // held block no longer names the manager, so a second call finds nothing.
    // The server's world; the session clients hold with
    // checkpoint_activation_hold, which keeps and restores it.
    inline CKBehavior* hold_checkpoint_activation(CKContext* context, std::string& error) {
        CKBehavior* block = checkpoint_activation_block(context, error);
        if (!block) return nullptr;
        CKParameterIn* input = block->GetInputParameter(1);
        if (!input || input->GetGUID() != CKPGUID_SCRIPT) { error = "its Script input is not a script parameter"; return nullptr; }
        if (!feed_checkpoint_hold(context, input, error)) return nullptr;
        return block;
    }

    // The session clients' N3 (FULL step N3K, host-platform audit D3; AUD-2):
    // the hold made at the anchor with what it replaced recorded, kept every
    // frame (a level reset that handed the input its retail source back is
    // re-held), and the retail source handed back at the session's end.  One
    // implementation for the headless host and the retail core host (moved
    // from the legacy retail copy, physics_session_client.cpp
    // sector_activate_resolve / _apply / _restore, deleted with the legacy
    // path at R7, FULL step LGD).
    class checkpoint_activation_hold {
    public:
        // At the anchor: the block found and its Script input fed the hold's
        // parameter, the retail source recorded.  False with `error` set:
        // the hold is not in force (failed()), the chain runs the manager
        // itself.  A hold still in force is handed back first.
        bool apply(CKContext* context, std::string& error) {
            restore(context);
            const auto refuse = [&](const char* why) {
                if (why) error = why;
                failed_ = true;
                return false;
            };
            if (!context) return refuse("no context");
            CKBehavior* block = checkpoint_activation_block(context, error);
            if (!block) return refuse(nullptr);
            CKParameterIn* input = block->GetInputParameter(kInput);
            if (!input || input->GetGUID() != CKPGUID_SCRIPT) return refuse("its Script input is not a script parameter");
            CKParameter* source = input->GetRealSource();
            if (!source || source->GetDataSize() != static_cast<int>(sizeof(CK_ID)))
                return refuse("its Script input has no usable source");
            CKParameterIn* shared = input->GetSharedSource();
            CKParameter* direct = input->GetDirectSource();
            const CK_ID real = source->GetID();
            CKParameter* none = feed_checkpoint_hold(context, input, error);
            if (!none) return refuse(nullptr);
            block_ = block->GetID();
            retail_source_ = direct ? direct->GetID() : 0;
            retail_shared_ = shared ? shared->GetID() : 0;
            retail_real_ = real;
            none_ = none->GetID();
            return true;
        }

        // Every frame (the hosts' script guards): the input re-held when it
        // reads its retail source again (session/checkpoint_hold_rule.hpp);
        // a source neither ours nor retail's is left alone.  True when it
        // re-held (the hosts log "held again").
        bool keep(CKContext* context) {
            CKParameterIn* input = nullptr;
            CKParameter* none = nullptr;
            if (!parts(context, input, none)) return false;
            bool reheld = false;
            bmmo::session::keep_checkpoint_hold(*input, *none, static_cast<uint32_t>(retail_real_), reheld);
            if (reheld) {
                ++reholds_;
                note_script_write();   // step OPC: a write into the graph outside the engine's restore
            }
            return reheld;
        }

        // Where the input reads from now (the D3 diagnostic's attribution,
        // game/session_host_support.hpp manager_activations::record_pending).
        bmmo::session::checkpoint_hold_state state(CKContext* context) const {
            CKParameterIn* input = nullptr;
            CKParameter* none = nullptr;
            if (!parts(context, input, none)) return bmmo::session::checkpoint_hold_state::not_held;
            CKParameter* current = input->GetRealSource();
            return bmmo::session::classify_checkpoint_hold(current ? static_cast<uint32_t>(current->GetID()) : 0u,
                                                           static_cast<uint32_t>(none_), static_cast<uint32_t>(retail_real_));
        }

        // At the session's end (and before the next apply): the retail source
        // handed back where the input still reads the hold's parameter, the
        // parameter destroyed.  False when the input could not be handed back
        // (the parameter is then left in place, still referenced).  The hold
        // is forgotten either way.
        bool restore(CKContext* context) {
            bool referenced = false;
            if (context && none_) {
                CKParameterIn* input = nullptr;
                CKParameter* none = nullptr;
                parts(context, input, none);
                if (!none) none = hold_parameter(context, none_);
                if (input && none && input->GetRealSource() == none) {
                    referenced = true;
                    if (retail_shared_) {
                        auto* shared = CKParameterIn::Cast(context->GetObject(retail_shared_));
                        if (shared && input->ShareSourceWith(shared) == CK_OK) referenced = false;
                    } else {
                        auto* source = CKParameter::Cast(context->GetObject(retail_source_));
                        if (source && input->SetDirectSource(source) == CK_OK) referenced = false;
                    }
                }
                if (none && !referenced) context->DestroyObject(none);
            }
            *this = checkpoint_activation_hold{};
            return !referenced;
        }

        // In force: made at this anchor and not restored since.
        bool held() const { return block_ != 0 && none_ != 0; }
        // The last apply refused (cleared by restore and the next apply).
        bool failed() const { return failed_; }
        // Re-holds this session (keep()).
        uint64_t reholds() const { return reholds_; }

    private:
        static constexpr int kInput = 1;   // Execute Script's "Script"
        static constexpr const char* kHoldName = "BMMO_SectorHold";   // feed_checkpoint_hold's parameter

        // The held block's Script input and the hold's parameter, both
        // still there and the block still an Execute Script.
        bool parts(CKContext* context, CKParameterIn*& input, CKParameter*& none) const {
            input = nullptr;
            none = nullptr;
            if (!context || !held()) return false;
            auto* block = CKBehavior::Cast(context->GetObject(block_));
            const char* prototype = block && block->IsUsingFunction() ? block->GetPrototypeName() : nullptr;
            if (!prototype || std::strcmp(prototype, "Execute Script") != 0) return false;
            input = block->GetInputParameter(kInput);
            none = hold_parameter(context, none_);
            return input && none;
        }

        // The hold's parameter by id, only while the object there is still
        // ours (named BMMO_SectorHold): a level unloaded since the anchor may
        // have handed the id to another object, which restore must not
        // destroy (the step's review).
        static CKParameter* hold_parameter(CKContext* context, CK_ID id) {
            CKParameter* parameter = id ? CKParameter::Cast(context->GetObject(id)) : nullptr;
            const char* name = parameter ? parameter->GetName() : nullptr;
            return name && std::strcmp(name, kHoldName) == 0 ? parameter : nullptr;
        }

        CK_ID block_ = 0;           // the Execute Script running Gameplay_SectorManager
        CK_ID retail_source_ = 0;   // the parameter it read (direct source)
        CK_ID retail_shared_ = 0;   // or the input it shared its source with
        CK_ID retail_real_ = 0;     // what GetRealSource() answered before the hold
        CK_ID none_ = 0;            // the hold's parameter, destroyed on restore
        bool failed_ = false;
        uint64_t reholds_ = 0;
    };
}
