#pragma once

// The Level 1 tutorial switched off in a physics session (record-format-gaps-
// design.md 8, row K3 = C4, option (c) of 11 Q1, the user's decision of
// 2026-10-24; step RK3).
//
// Gameplay_Tutorial is own-player flow (own-ball probes, the own keyboard)
// that writes world state every player shares: its Set Physics Globals blocks
// write the time factor 0 at the tutorial's start and at every chapter, and
// 2.0 again on Enter or a 25 s timer, each world at its own moment (design
// 1.4, G-T).  IVP has one clock per world, so a session switches the tutorial
// off instead, the way the main branch's multiplayer restart does
// (BallanceMMOClient edit_Gameplay_Tutorial): the block that feeds
// `Kapitel Aktion` / `Tut continue/exit`'s Set Physics Globals has its output
// 0 activated, which runs the tutorial's exit path, the 2.0 unfreeze
// included.  The texts and arrows of Level 1 are not shown in sessions.
//
// When: kTutorialExitFrame frames after the level start, counted in frames
// (never wall time), the same frame in every world - the server's world, the
// headless client and the retail core host - each counting the world steps
// since its own anchor (the first frame after the anchor is frame 1).  The
// main branch uses the same 10-frame delay, and the user says it is
// necessary: the tutorial writes its first 0 in frame 9 (K0's
// --globals-trace, tick 8), and an exit activated before that is undone by
// it.  The activation is live only: a rollback's replay never re-runs
// Gameplay_Tutorial (it is not a selected script), and the record of each
// tick carries the factor the live step used (C3), so a replay across the
// frozen frames is exact.
//
// Where the decision lives: the session core decides when a client world
// takes the exit (session_client_core::pre_step, the platform's
// tutorial_exit), the server's world decides it in physics_world::tick from
// its own world option; both call this header, which only finds and
// activates the IO.  Only the public CK2 API is used: the header compiles
// against the Virtools SDK (client mod) and against the Ballanced engine
// (headless server).

#include <cstdio>
#include <cstring>
#include <string>

#include "CKAll.h"

#include "entity/session.hpp"
#include "menu_driver.hpp"

namespace bmmo::game {
    // The frame (1-based, world steps since the anchor) whose behaviour pass
    // runs the exit: the host activates the IO right before that frame.
    constexpr int kTutorialExitFrame = bmmo::session::SESSION_CLOCK_TUTORIAL_EXIT_FRAME;

    // The level the tutorial runs on (its script exists on every level and
    // is activated on Level 1 only).
    constexpr int kTutorialLevel = bmmo::session::SESSION_CLOCK_TUTORIAL_LEVEL;

    struct tutorial_exit {
        CK_ID io = 0;       // the activated output (the exit event)
        CK_ID block = 0;    // its owner
        CK_ID globals = 0;  // the Set Physics Globals block it feeds
        std::string where;  // a description for the logs

        void clear() { *this = tutorial_exit{}; }

        // The structural find (the main branch's edit_Gameplay_Tutorial):
        // Gameplay_Tutorial / Kapitel Aktion / Tut continue/exit (direct
        // children, by name), its Set Physics Globals (a direct child), and
        // the block whose link feeds that block's input 0; its output 0 is
        // the exit event.  Empty on success, otherwise what was missing.
        std::string resolve(CKContext* context) {
            clear();
            CKBehavior* script = context ? find_root_script(context, "Gameplay_Tutorial") : nullptr;
            if (!script) return "Gameplay_Tutorial is missing";
            CKBehavior* chapters = find_sub_behavior(script, "Kapitel Aktion");
            if (!chapters) return "Gameplay_Tutorial has no 'Kapitel Aktion'";
            CKBehavior* logic = find_sub_behavior(chapters, "Tut continue/exit");
            if (!logic) return "'Kapitel Aktion' has no 'Tut continue/exit'";
            CKBehavior* spg = find_sub_behavior(logic, "Set Physics Globals");
            if (!spg) return "'Tut continue/exit' has no 'Set Physics Globals'";
            CKBehaviorIO* in = spg->GetInputCount() > 0 ? spg->GetInput(0) : nullptr;
            if (!in) return "the Set Physics Globals block has no input";
            CKBehavior* previous = nullptr;
            const int links = logic->GetSubBehaviorLinkCount();
            for (int l = 0; l < links && !previous; ++l) {
                CKBehaviorLink* link = logic->GetSubBehaviorLink(l);
                if (!link || link->GetOutBehaviorIO() != in) continue;
                CKBehaviorIO* source = link->GetInBehaviorIO();
                previous = source ? source->GetOwner() : nullptr;
            }
            if (!previous) return "nothing is linked into the Set Physics Globals block's input 0";
            CKBehaviorIO* out = previous->GetOutputCount() > 0 ? previous->GetOutput(0) : nullptr;
            if (!out) return std::string("the block before Set Physics Globals ('") + (previous->GetName() ? previous->GetName() : "?")
                             + "') has no output";
            io = out->GetID();
            block = previous->GetID();
            globals = spg->GetID();
            char text[256];
            std::snprintf(text, sizeof(text), "Gameplay_Tutorial / Kapitel Aktion / Tut continue/exit: '%s' (#%u) output 0 (#%u) "
                          "-> Set Physics Globals (#%u)", previous->GetName() ? previous->GetName() : "?",
                          static_cast<unsigned>(block), static_cast<unsigned>(io), static_cast<unsigned>(globals));
            where = text;
            return {};
        }

        bool resolved() const { return io != 0; }

        // Activates the exit event (the next behaviour pass runs the exit
        // path).  False with `error` when the IO is gone.
        bool activate(CKContext* context, std::string& error) const {
            CKObject* object = context && io ? context->GetObject(io) : nullptr;
            if (!object || object->GetClassID() != CKCID_BEHAVIORIO) {
                error = io ? "the tutorial's exit event no longer exists" : "the tutorial's exit event was not found at the anchor";
                return false;
            }
            static_cast<CKBehaviorIO*>(object)->Activate(TRUE);
            return true;
        }
    };

    // Step RK3H (the user's decision of 2026-10-29, 'Hold frame-9 write',
    // answering RK3's gate): the exit above takes only in frame 16, so the
    // tutorial's own first write - time factor 0 in frame 9 - froze every
    // world for 7 ticks (frames 9..15).  That one write is held: the
    // Physic Time Factor input (1) of Gameplay_Tutorial / Init's Set Physics
    // Globals (the only one in that graph, the tutorial's start; #31475 in
    // the headless level) is fed a private constant holding the in-play
    // factor, read from Gameplay_Ingame / Init Ingame's Set Physics Globals
    // (2.0, the value it writes in frame 2), so frame 9 writes 2.0 over 2.0
    // and no world freezes.  Gravity (input 0) is untouched (0,-20,0 there
    // as in play).  The block still runs and its output still fires: the
    // tutorial's flow is unchanged until the exit ends it in frame 16; no
    // other tutorial write is held (not design 2.4's hold of all of them).
    // The source is swapped with SetDirectSource and never written: the
    // original local keeps its 0 (the propagation trap of
    // script-restore-parameter-propagation).
    //
    // When: made at the world's anchor, before its frame 1 - every world
    // (the server's, the headless client's, the retail core host's, a late
    // joiner's at its own anchor) holds it before the frame the write runs
    // in, counted the way the exit is.  The server's world is destroyed with
    // its session and keeps it; base.cmo survives a level exit on both
    // client hosts, so they keep it in their frame guard and hand it back at
    // the session's end / the next anchor (the pattern of
    // sector_deactivate_pin.hpp).
    struct tutorial_factor_hold {
        CK_ID block = 0;          // Gameplay_Tutorial / Init / Set Physics Globals
        CK_ID retail_source = 0;  // what its input 1 read before (a direct source)
        CK_ID retail_shared = 0;  // or the input it shared its source with
        CK_ID retail_real = 0;    // GetRealSource() before the hold
        CK_ID constant = 0;       // the session's constant (the in-play factor)
        float factor = 0.0f;      // the value held
        float original = 0.0f;    // what the block would have written
        std::string where;        // a description for the logs

        static constexpr const char* kHoldName = "BMMO_TutorialFactorHold";

        bool held() const { return constant != 0; }

        // The one Set Physics Globals block that is a direct child of
        // `graph`, else null (with `error`).
        static CKBehavior* only_globals(CKBehavior* graph, const char* name, std::string& error) {
            CKBehavior* found = nullptr;
            const int count = graph ? graph->GetSubBehaviorCount() : 0;
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = graph->GetSubBehavior(i);
                const char* prototype = sub && sub->IsUsingFunction() ? sub->GetPrototypeName() : nullptr;
                if (!prototype || std::strcmp(prototype, "Set Physics Globals") != 0) continue;
                if (found) { error = std::string("'") + name + "' has more than one Set Physics Globals"; return nullptr; }
                found = sub;
            }
            if (!found) error = std::string("'") + name + "' has no Set Physics Globals";
            return found;
        }

        // A float parameter's value, read without running its operation.
        static bool read_float(CKParameter* source, float& value) {
            if (!source || source->GetGUID() != CKPGUID_FLOAT || source->GetDataSize() != static_cast<int>(sizeof(float)))
                return false;
            return source->GetValue(&value, FALSE) == CK_OK;
        }

        // Finds the block and the in-play value and holds the write.  Empty
        // on success, otherwise what was missing (nothing changed then).
        // Call restore() first when a previous hold may still be in place.
        std::string apply(CKContext* context) {
            *this = tutorial_factor_hold{};
            std::string error;
            CKBehavior* ingame = context ? find_root_script(context, "Gameplay_Ingame") : nullptr;
            if (!ingame) return "Gameplay_Ingame is missing";
            CKBehavior* init_ingame = find_sub_behavior(ingame, "Init Ingame");
            if (!init_ingame) return "Gameplay_Ingame has no 'Init Ingame'";
            CKBehavior* play = only_globals(init_ingame, "Init Ingame", error);
            if (!play) return error;
            CKParameterIn* play_in = play->GetInputParameter(1);
            float in_play = 0.0f;
            if (!play_in || !read_float(play_in->GetRealSource(), in_play))
                return "Init Ingame's Set Physics Globals has no float Physic Time Factor";
            if (!(in_play > 0.0f)) return "Init Ingame writes a time factor that is not positive";
            CKBehavior* script = find_root_script(context, "Gameplay_Tutorial");
            if (!script) return "Gameplay_Tutorial is missing";
            CKBehavior* init = find_sub_behavior(script, "Init");
            if (!init) return "Gameplay_Tutorial has no 'Init'";
            CKBehavior* spg = only_globals(init, "Init", error);
            if (!spg) return error;
            CKParameterIn* input = spg->GetInputParameter(1);   // "Physic Time Factor"
            CKParameter* source = input ? input->GetRealSource() : nullptr;
            float before = 0.0f;
            if (!read_float(source, before)) return "the tutorial Init's Set Physics Globals has no float Physic Time Factor";
            if (before != 0.0f) {
                char text[96];
                std::snprintf(text, sizeof(text), "the tutorial Init's write is %g, not the freeze (0)", static_cast<double>(before));
                return text;
            }
            auto* made = context->CreateCKParameterLocal(const_cast<CKSTRING>(kHoldName), CKPGUID_FLOAT, TRUE);
            if (!made) return "could not create the session's constant";
            made->SetValue(&in_play, sizeof(in_play));   // a fresh parameter nothing reads yet
            CKParameterIn* shared = input->GetSharedSource();
            CKParameter* direct = input->GetDirectSource();
            if (input->SetDirectSource(made) != CK_OK) {
                context->DestroyObject(made);
                return "the tutorial Init's Physic Time Factor input refused the session's constant";
            }
            block = spg->GetID();
            retail_source = direct ? direct->GetID() : 0;
            retail_shared = shared ? shared->GetID() : 0;
            retail_real = source->GetID();
            constant = made->GetID();
            factor = in_play;
            original = before;
            char text[256];
            std::snprintf(text, sizeof(text), "Gameplay_Tutorial / Init / Set Physics Globals (#%u) writes %g instead of %g "
                          "(Init Ingame's #%u)", static_cast<unsigned>(block), static_cast<double>(in_play),
                          static_cast<double>(before), static_cast<unsigned>(play->GetID()));
            where = text;
            return {};
        }

        // The frame guard's keep: back onto the constant when something
        // handed the input back to the source it had.  True when it re-held.
        bool keep(CKContext* context) const {
            if (!context || !block || !constant) return false;
            auto* spg = CKBehavior::Cast(context->GetObject(block));
            CKParameterIn* input = spg ? spg->GetInputParameter(1) : nullptr;
            auto* value = CKParameter::Cast(context->GetObject(constant));
            if (!input || !value) return false;
            CKParameter* current = input->GetRealSource();
            if (current == value) return false;
            if (!current || current->GetID() != retail_real) return false;
            return input->SetDirectSource(value) == CK_OK;
        }

        // Hands the input back and destroys the constant (kept when the
        // input still reads it and could not be handed back).  False when
        // the input was left on the constant.
        bool restore(CKContext* context) {
            bool back = true;
            if (context && constant) {
                auto* spg = block ? CKBehavior::Cast(context->GetObject(block)) : nullptr;
                CKParameterIn* input = spg ? spg->GetInputParameter(1) : nullptr;
                auto* value = CKParameter::Cast(context->GetObject(constant));
                if (value && (!value->GetName() || std::strcmp(value->GetName(), kHoldName) != 0))
                    value = nullptr;   // the id names another object now (a reboot): not ours to destroy
                bool referenced = false;
                if (input && value && input->GetRealSource() == value) {
                    referenced = true;
                    if (retail_shared) {
                        auto* other = CKParameterIn::Cast(context->GetObject(retail_shared));
                        if (other && input->ShareSourceWith(other) == CK_OK) referenced = false;
                    } else {
                        auto* original_source = CKParameter::Cast(context->GetObject(retail_source));
                        if (original_source && input->SetDirectSource(original_source) == CK_OK) referenced = false;
                    }
                }
                if (value && !referenced) context->DestroyObject(value);
                back = !referenced;
            }
            if (back) *this = tutorial_factor_hold{};
            return back;
        }
    };
}
