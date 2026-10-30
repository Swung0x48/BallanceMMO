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
    // Step DX2 (FULL close-out, D5 / RK3F; the user's decision of
    // 2026-10-29, 'D5: a'): Gameplay_Refresh's 'Tutorial freeze/unfreeze'
    // graph (#29832 headless) runs on Level 1 when its column-12 cell is set
    // and writes 0 or the in-play factor by the tutorial's freeze flag (its
    // Binary Switch #29809 -> Parameter Selector #29771 -> Set Physics
    // Globals #29784), so a Gameplay_Refresh activation inside frames 9..15
    // (a respawn, a pause resume) still froze that one world until the exit
    // cleared the flag.  Its Set Physics Globals' Physic Time Factor input is
    // held the same way, onto the same constant: both branches then write
    // the in-play factor (its other branch wrote it already).  Gravity is
    // untouched there too.  Guarded structurally, not by the value the
    // input reads now (the selector's output keeps whatever the last run
    // selected): the input must read a Parameter Selector's output whose
    // every input is 0 or the in-play factor, one of them 0.  Both holds
    // are kept and handed back together, but made independently: the
    // Refresh hold is an addition to RK3H's, so a Refresh block that is
    // missing, wired otherwise or refuses the constant leaves RK3H's Init
    // hold in place (apply still succeeds; `where` says the Refresh write
    // was NOT held and why) - never a world worse off than at RK3H.
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
        // One held Physic Time Factor input (input 1 of a Set Physics Globals).
        struct redirect {
            CK_ID block = 0;          // the Set Physics Globals block
            CK_ID retail_source = 0;  // what its input 1 read before (a direct source)
            CK_ID retail_shared = 0;  // or the input it shared its source with
            CK_ID retail_real = 0;    // GetRealSource() before the hold
        };
        redirect init;            // Gameplay_Tutorial / Init / Set Physics Globals (RK3H)
        redirect refresh;         // Gameplay_Refresh / Tutorial freeze/unfreeze / Set Physics Globals (DX2)
        CK_ID constant = 0;       // the session's constant (the in-play factor)
        float factor = 0.0f;      // the value held
        float original = 0.0f;    // what Init's block would have written
        std::string where;        // a description for the logs

        static constexpr const char* kHoldName = "BMMO_TutorialFactorHold";
        static constexpr const char* kRefreshGraph = "Tutorial freeze/unfreeze";

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

        // The one direct child of `parent` whose name is `name` once its
        // trailing spaces are dropped (the level names the Refresh graph
        // "Tutorial freeze/unfreeze " with a trailing space), else null.
        static CKBehavior* find_sub_trimmed(CKBehavior* parent, const char* name, std::string& error) {
            CKBehavior* found = nullptr;
            const size_t length = std::strlen(name);
            const int count = parent ? parent->GetSubBehaviorCount() : 0;
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = parent->GetSubBehavior(i);
                const char* sub_name = sub ? sub->GetName() : nullptr;
                if (!sub_name || std::strncmp(sub_name, name, length) != 0) continue;
                const char* rest = sub_name + length;
                while (*rest == ' ') ++rest;
                if (*rest) continue;
                if (found) { error = std::string("Gameplay_Refresh has more than one '") + name + "'"; return nullptr; }
                found = sub;
            }
            if (!found) error = std::string("Gameplay_Refresh has no '") + name + "'";
            return found;
        }

        // Points `input` at `value`, remembering what it read.
        static bool redirect_input(CKParameterIn* input, CKParameter* value, redirect& r, CK_ID block) {
            CKParameter* real = input->GetRealSource();
            CKParameterIn* shared = input->GetSharedSource();
            CKParameter* direct = input->GetDirectSource();
            if (input->SetDirectSource(value) != CK_OK) return false;
            r.block = block;
            r.retail_source = direct ? direct->GetID() : 0;
            r.retail_shared = shared ? shared->GetID() : 0;
            r.retail_real = real ? real->GetID() : 0;
            return true;
        }

        // Hands one input back to what it read; true when it no longer
        // reads `value`.
        static bool hand_back(CKContext* context, const redirect& r, CKParameter* value) {
            auto* spg = r.block ? CKBehavior::Cast(context->GetObject(r.block)) : nullptr;
            CKParameterIn* input = spg ? spg->GetInputParameter(1) : nullptr;
            if (!input || !value || input->GetRealSource() != value) return true;
            if (r.retail_shared) {
                auto* other = CKParameterIn::Cast(context->GetObject(r.retail_shared));
                return other && input->ShareSourceWith(other) == CK_OK;
            }
            auto* original_source = CKParameter::Cast(context->GetObject(r.retail_source));
            return original_source && input->SetDirectSource(original_source) == CK_OK;
        }

        // Back onto the constant when something handed `r`'s input back to
        // the source it had.  True when it re-held.
        static bool keep_one(CKContext* context, const redirect& r, CKParameter* value) {
            auto* spg = r.block ? CKBehavior::Cast(context->GetObject(r.block)) : nullptr;
            CKParameterIn* input = spg ? spg->GetInputParameter(1) : nullptr;
            if (!input || !value) return false;
            CKParameter* current = input->GetRealSource();
            if (current == value) return false;
            if (!current || current->GetID() != r.retail_real) return false;
            return input->SetDirectSource(value) == CK_OK;
        }

        // Finds the blocks and the in-play value and holds the writes.
        // Empty when Init's write is held (Refresh's too, unless `where`
        // says it is NOT held and why), otherwise what was missing (nothing
        // changed then).  Call restore() first when a previous hold may
        // still be in place.
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
            // RK3H: Gameplay_Tutorial / Init's write.
            CKBehavior* script = find_root_script(context, "Gameplay_Tutorial");
            if (!script) return "Gameplay_Tutorial is missing";
            CKBehavior* init_graph = find_sub_behavior(script, "Init");
            if (!init_graph) return "Gameplay_Tutorial has no 'Init'";
            CKBehavior* spg = only_globals(init_graph, "Init", error);
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
            // RK3H's write found and checked; the in-play value known.
            auto* made = context->CreateCKParameterLocal(const_cast<CKSTRING>(kHoldName), CKPGUID_FLOAT, TRUE);
            if (!made) return "could not create the session's constant";
            made->SetValue(&in_play, sizeof(in_play));   // a fresh parameter nothing reads yet
            if (!redirect_input(input, made, init, spg->GetID())) {
                context->DestroyObject(made);
                *this = tutorial_factor_hold{};
                return "the tutorial Init's Physic Time Factor input refused the session's constant";
            }
            constant = made->GetID();
            factor = in_play;
            original = before;
            // DX2: Gameplay_Refresh / Tutorial freeze/unfreeze's write, on
            // top of the Init hold: when it cannot be held, the Init hold
            // stays and `where` says why the Refresh one was not made.
            std::string refresh_trouble;
            CK_ID selector_id = 0;
            CKBehavior* refresh_spg = nullptr;
            CKParameterIn* refresh_input = find_refresh_input(context, in_play, refresh_trouble, refresh_spg, selector_id);
            if (refresh_input && !redirect_input(refresh_input, made, refresh, refresh_spg->GetID())) {
                refresh = redirect{};
                refresh_trouble = "Gameplay_Refresh's tutorial Physic Time Factor input refused the session's constant";
            }
            char text[512];
            if (refresh.block)
                std::snprintf(text, sizeof(text), "Gameplay_Tutorial / Init / Set Physics Globals (#%u) writes %g instead of %g, "
                              "Gameplay_Refresh / %s / Set Physics Globals (#%u, by Parameter Selector #%u) writes %g instead of "
                              "0 or %g (Init Ingame's #%u)", static_cast<unsigned>(init.block), static_cast<double>(in_play),
                              static_cast<double>(before), kRefreshGraph, static_cast<unsigned>(refresh.block),
                              static_cast<unsigned>(selector_id), static_cast<double>(in_play), static_cast<double>(in_play),
                              static_cast<unsigned>(play->GetID()));
            else
                std::snprintf(text, sizeof(text), "Gameplay_Tutorial / Init / Set Physics Globals (#%u) writes %g instead of %g "
                              "(Init Ingame's #%u); Gameplay_Refresh's tutorial write is NOT held (%s): a Gameplay_Refresh "
                              "activation before the exit freezes this world", static_cast<unsigned>(init.block),
                              static_cast<double>(in_play), static_cast<double>(before), static_cast<unsigned>(play->GetID()),
                              refresh_trouble.c_str());
            where = text;
            return {};
        }

        // Gameplay_Refresh / Tutorial freeze/unfreeze's Set Physics Globals
        // (`block`) and its Physic Time Factor input when that reads a
        // Parameter Selector whose every input is 0 or `in_play`, one of
        // them 0; else null with `error`.  Changes nothing.
        static CKParameterIn* find_refresh_input(CKContext* context, float in_play, std::string& error,
                                                 CKBehavior*& block, CK_ID& selector_id) {
            CKBehavior* refresh_script = find_root_script(context, "Gameplay_Refresh");
            if (!refresh_script) { error = "Gameplay_Refresh is missing"; return nullptr; }
            CKBehavior* freeze = find_sub_trimmed(refresh_script, kRefreshGraph, error);
            if (!freeze) return nullptr;
            CKBehavior* refresh_spg = only_globals(freeze, kRefreshGraph, error);
            if (!refresh_spg) return nullptr;
            CKParameterIn* refresh_input = refresh_spg->GetInputParameter(1);   // "Physic Time Factor"
            CKParameter* refresh_source = refresh_input ? refresh_input->GetRealSource() : nullptr;
            CKObject* owner = refresh_source ? refresh_source->GetOwner() : nullptr;
            CKBehavior* selector = owner && owner->GetClassID() == CKCID_BEHAVIOR ? static_cast<CKBehavior*>(owner) : nullptr;
            const char* selector_prototype = selector && selector->IsUsingFunction() ? selector->GetPrototypeName() : nullptr;
            if (!selector_prototype || std::strcmp(selector_prototype, "Parameter Selector") != 0) {
                error = "Gameplay_Refresh's tutorial Set Physics Globals does not read a Parameter Selector's output";
                return nullptr;
            }
            const int choices = selector->GetInputParameterCount();
            bool freezes = false;
            for (int i = 0; i < choices; ++i) {
                CKParameterIn* choice_in = selector->GetInputParameter(i);
                float choice = 0.0f;
                if (!choice_in || !read_float(choice_in->GetRealSource(), choice)) {
                    error = "Gameplay_Refresh's tutorial Parameter Selector has an input that is not a float";
                    return nullptr;
                }
                if (choice == 0.0f) freezes = true;
                else if (choice != in_play) {
                    char text[128];
                    std::snprintf(text, sizeof(text), "Gameplay_Refresh's tutorial Parameter Selector offers %g, neither the freeze "
                                  "(0) nor the in-play %g", static_cast<double>(choice), static_cast<double>(in_play));
                    error = text;
                    return nullptr;
                }
            }
            if (!freezes) { error = "Gameplay_Refresh's tutorial Parameter Selector offers no freeze (0)"; return nullptr; }
            block = refresh_spg;
            selector_id = selector->GetID();
            return refresh_input;
        }

        // The frame guard's keep: back onto the constant when something
        // handed an input back to the source it had.  True when it re-held
        // either.
        bool keep(CKContext* context) const {
            if (!context || !constant) return false;
            auto* value = CKParameter::Cast(context->GetObject(constant));
            if (!value) return false;
            const bool a = keep_one(context, init, value);
            const bool b = keep_one(context, refresh, value);
            return a || b;
        }

        // Hands both inputs back and destroys the constant (kept when an
        // input still reads it and could not be handed back).  False when
        // an input was left on the constant.
        bool restore(CKContext* context) {
            bool back = true;
            if (context && constant) {
                auto* value = CKParameter::Cast(context->GetObject(constant));
                if (value && (!value->GetName() || std::strcmp(value->GetName(), kHoldName) != 0))
                    value = nullptr;   // the id names another object now (a reboot): not ours to destroy
                if (value) {
                    const bool a = hand_back(context, init, value);
                    const bool b = hand_back(context, refresh, value);
                    back = a && b;
                    if (back) context->DestroyObject(value);
                }
            }
            if (back) *this = tutorial_factor_hold{};
            return back;
        }
    };
}
