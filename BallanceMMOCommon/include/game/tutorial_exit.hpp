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
}
