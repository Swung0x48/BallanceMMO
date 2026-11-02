#pragma once

// FULL step RTDF (the user's decision 65, 2026-11-01: option (a) of step RTD's
// finding; record-format-gaps-design.md "As built (step RTD)" / "As built (step
// RTDF)").  In a core session the retail Ball Navigation's shared Physics
// WakeUp does not act on the own ball.
//
// The finding.  Menu_Pause's chain sends "BallNav deactivate" (ESCX left it to
// the menu, menu_overlay.hpp); the retail Ball Navigation script, still present
// in the owner's world beside the own replica, sends Shutdown to every
// SetPhysicsForce leaf, and each leaf's Out2 feeds the script's one Physics
// WakeUp block, whose target is the current ball - the own ball.  With the
// ball asleep (resting at the spawn) that wake made it rise 0.0038 m/s and
// settle 1.53 mm higher in its owner's world only: an own-ball script wake is
// not reported (core_host.cpp reportable_wakeup), and every other world - the
// server and each mirror - drives the ball through the replica
// (ball_navigation.cpp), whose deactivation wakes only a leaf that holds a
// controller.
//
// What this does.  The block's per-block function (CKBehavior::SetFunction;
// the precedents are menu_keys.hpp and physics_state.cpp's Random-block patch)
// is replaced by `wrapper`, which tests the block's target: the own ball (the
// host's test, `own_test`) -> the block does its own work minus the queued
// wake (PhysicsWakeUp.cpp: In off, Out on, CKBR_OK; the PreSimulate callback
// that would call WakeUpFromScript is not queued), anything else -> the
// block's own function, unchanged.  The replica keeps every wake its rules
// make (a key's Create / Release), in every world alike, the owner's
// included: the key-driven wakes the retail graph also made are the same
// ensure_in_simulation in the same PreSimulate pass, so only the wake no
// other world makes - a deactivation with no leaf holding a controller - is
// gone.  Retail-only (the Virtools SDK of the mod), host-local (no wire or
// record change), core session only: wrapped at the anchor, kept wrapped every
// frame (a level reset may hand the block its prototype's function back),
// handed back at the session's end; offline play and every other Physics
// WakeUp block are untouched.  File-scope state of core_host.cpp:
// BallanceMMOClient keeps its layout.

#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "CKAll.h"

#include <game/menu_driver.hpp>   // find_root_script, find_sub_behavior

namespace bmmo::retail {
    class own_wake_guard {
    public:
        using own_test = std::function<bool(CK3dEntity*)>;

        ~own_wake_guard() {
            if (active_ == this) active_ = nullptr;
        }

        // The core session's anchor: wraps every Physics WakeUp block of
        // Gameplay_Ingame's Ball Navigation.  Returns the lines to log, "ok " /
        // "error ".
        std::vector<std::string> resolve(CKContext* context, own_test is_own) {
            restore(context);   // the counts stay: they are the session's (clear_counts at its end)
            if (!context) return {"error no context"};
            CKBehavior* ingame = bmmo::game::find_root_script(context, "Gameplay_Ingame");
            CKBehavior* navigation = ingame ? bmmo::game::find_sub_behavior(ingame, "Ball Navigation") : nullptr;
            if (!navigation) return {"error Gameplay_Ingame's Ball Navigation not found; the own ball's wake is left as is"};
            for (int i = 0; i < navigation->GetSubBehaviorCount(); ++i) {
                CKBehavior* block = navigation->GetSubBehavior(i);
                if (!block || !block->IsUsingFunction()) continue;
                const char* prototype = block->GetPrototypeName();
                if (!prototype || std::strcmp(prototype, "Physics WakeUp") != 0) continue;
                CKBEHAVIORFCT original = block->GetFunction();
                if (original == &wrapper) original = prototype_function(block);
                if (!original) continue;
                wrapped_.push_back({block->GetID(), original});
                block->SetFunction(&wrapper);
            }
            if (wrapped_.empty())
                return {"error no Physics WakeUp block in Ball Navigation; the own ball's wake is left as is"};
            is_own_ = std::move(is_own);
            active_ = this;
            std::string ids;
            for (const auto& w: wrapped_) ids += std::format("{}#{}", ids.empty() ? "" : ", ", w.block);
            return {std::format("ok {} Physics WakeUp block(s) of Ball Navigation act on no own ball: {}",
                                wrapped_.size(), ids)};
        }

        // Every frame of a core session: every wrapped block still wrapped.
        void apply(CKContext* context) {
            if (!context) return;
            for (auto& w: wrapped_) {
                auto* block = CKBehavior::Cast(context->GetObject(w.block));
                if (!block || block->GetFunction() == &wrapper) continue;
                if (CKBEHAVIORFCT current = block->GetFunction()) w.original = current;
                block->SetFunction(&wrapper);
                ++rewraps_;
            }
        }

        // The session's end: every wrapped block gets its own function back.
        void restore(CKContext* context) {
            if (context) {
                for (const auto& w: wrapped_) {
                    auto* block = CKBehavior::Cast(context->GetObject(w.block));
                    if (block && block->GetFunction() == &wrapper) block->SetFunction(w.original);
                }
            }
            wrapped_.clear();
            is_own_ = nullptr;
            if (active_ == this) active_ = nullptr;
        }

        // The session's end, after its counts are logged: the next session counts
        // from zero (a re-anchor inside one session keeps counting).
        void clear_counts() { kept_off_ = passed_ = rewraps_ = 0; }

        // The automation's 'pausechain' (core session) and the end-of-session log.
        std::string status() const {
            return std::format("own_wake blocks={} kept_off={} passed={} rewraps={}", wrapped_.size(), kept_off_,
                               passed_, rewraps_);
        }

    private:
        struct wrapped_block {
            CK_ID block = 0;
            CKBEHAVIORFCT original = nullptr;
        };

        static CKBEHAVIORFCT prototype_function(CKBehavior* block) {
            CKBehaviorPrototype* proto = block ? block->GetPrototype() : nullptr;
            CKBEHAVIORFCT f = proto ? proto->GetFunction() : nullptr;
            return f == &wrapper ? nullptr : f;
        }

        static int wrapper(const CKBehaviorContext& context) {
            CKBehavior* block = context.Behavior;
            if (!block) return CKBR_OK;
            own_wake_guard* self = active_;
            CKBEHAVIORFCT original = nullptr;
            if (self)
                for (const auto& w: self->wrapped_)
                    if (w.block == block->GetID()) original = w.original;
            if (!original) original = prototype_function(block);   // not (or no longer) one of ours
            if (self && original && self->is_own_) {
                // The target is read here, when the block runs; the original's
                // PreSimulate callback reads it again (PhysicsWakeUp.cpp
                // beh->GetTarget()).  They could differ only if Ball
                // Navigation's target parameter changed between the block and
                // PreSimulate in one frame, which no retail path does.
                CK3dEntity* target = static_cast<CK3dEntity*>(block->GetTarget());
                if (target && self->is_own_(target)) {
                    // PhysicsWakeUp's own path without its queued callback.
                    ++self->kept_off_;
                    block->ActivateInput(0, FALSE);
                    block->ActivateOutput(0, TRUE);
                    return CKBR_OK;
                }
                ++self->passed_;
            }
            return original ? original(context) : CKBR_OK;
        }

        inline static own_wake_guard* active_ = nullptr;

        own_test is_own_;
        std::vector<wrapped_block> wrapped_;
        unsigned long long kept_off_ = 0;
        unsigned long long passed_ = 0;
        unsigned long long rewraps_ = 0;
    };
}
