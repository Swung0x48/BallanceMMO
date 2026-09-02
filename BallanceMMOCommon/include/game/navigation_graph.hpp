#pragma once

// Reads the retail ball-navigation graph (Gameplay_Ingame / "Ball Navigation")
// so the client and the headless server agree on which key drives which
// SetPhysicsForce leaf, in which order, with which direction and force.
//
// Retail graph (base.cmo, verified with --dump-script on 2026-09-02):
//   Ball Navigation
//     SetPhysicsForce  (Position 0,0,0; Pos Referential = current ball;
//                       Direction = one of (±1,0,0)/(0,±1,0);
//                       Direction Ref = Cam_OrientRef; Force Value from
//                       Physicalize_GameBall through Gameplay_Refresh)
//     Key Event        (Key Waited from the options database)
//     link: Key Event.Pressed  -> SetPhysicsForce.Create
//     link: Key Event.Released -> SetPhysicsForce.Shutdown
//     link: SetPhysicsForce.Out1/Out2 -> Physics WakeUp.In
//   x4.  A fifth leaf (Direction 0,0,1) is fed through an Op and only enabled
//   by a debug flag; it has no direct Key Event link and is ignored here.
//
// Leaves are numbered by the position of their Key Event among the graph's
// sub-blocks: the engine executes active sub-blocks in that order, each
// Pressed output activates its SetPhysicsForce in the same frame, so this is
// the order in which controllers are added to the core when several keys go
// down in one tick.  session_input keys use bit i for leaf i.
//
// Only the public CK2 API is used, so this compiles against the Virtools SDK
// (client mod) and the Ballanced engine (headless server).

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CKAll.h"

#include "menu_driver.hpp"

namespace bmmo::game {
    struct navigation_leaf {
        int index = 0;             // leaf number (Key Event order, see above)
        int key_order = 0;         // sub-block index of the Key Event
        int key = 0;               // CKKEY_* code of the Key Event feeding Create
        VxVector direction{};      // "Direction" input of the force block
        float force_value = 0.0f;  // "Force Value" input (for the current ball)
        CK_ID force_block = 0;     // the SetPhysicsForce block
        CK_ID key_block = 0;       // the Key Event block
    };

    struct navigation_graph {
        std::vector<navigation_leaf> leaves;  // graph order
        CK_ID ball_navigation = 0;            // the "Ball Navigation" behavior
        CK_ID direction_ref = 0;              // Cam_OrientRef (Direction Ref of the leaves)
        CK_ID wake_up_block = 0;              // the shared "Physics WakeUp" block
        std::string error;

        bool valid() const { return error.empty() && !leaves.empty(); }
        // Bit mask of the leaves whose key is down in a 256-byte keyboard state
        // (KS_PRESSED semantics: bit 0 of each byte).
        uint8_t keys_from_state(const unsigned char* state) const {
            uint8_t mask = 0;
            for (const auto& leaf: leaves)
                if (leaf.key >= 0 && leaf.key < 256 && (state[leaf.key] & 1) && leaf.index < 8)
                    mask |= static_cast<uint8_t>(1u << leaf.index);
            return mask;
        }
    };

    inline const char* behavior_prototype_name(CKBehavior* behavior) {
        if (!behavior) return "";
        if (const char* proto = behavior->GetPrototypeName()) return proto;
        return behavior->GetName() ? behavior->GetName() : "";
    }

    inline navigation_graph read_navigation_graph(CKContext* context) {
        navigation_graph out;
        CKBehavior* ingame = find_root_script(context, "Gameplay_Ingame");
        if (!ingame) {
            out.error = "Gameplay_Ingame script not found";
            return out;
        }
        CKBehavior* navigation = find_sub_behavior(ingame, "Ball Navigation");
        if (!navigation) {
            out.error = "Ball Navigation block not found in Gameplay_Ingame";
            return out;
        }
        out.ball_navigation = navigation->GetID();
        const int links = navigation->GetSubBehaviorLinkCount();
        const int count = navigation->GetSubBehaviorCount();
        int force_index = 0;
        for (int i = 0; i < count; ++i) {
            CKBehavior* sub = navigation->GetSubBehavior(i);
            if (!sub) continue;
            const std::string proto = behavior_prototype_name(sub);
            if (proto == "Physics WakeUp") {
                if (!out.wake_up_block) out.wake_up_block = sub->GetID();
                continue;
            }
            if (proto != "SetPhysicsForce") continue;
            const int leaf_index = force_index++;
            CKBehaviorIO* create = sub->GetInput(0);
            CKBehavior* key_event = nullptr;
            for (int l = 0; l < links && !key_event; ++l) {
                CKBehaviorLink* link = navigation->GetSubBehaviorLink(l);
                if (!link || link->GetOutBehaviorIO() != create) continue;
                CKBehaviorIO* source = link->GetInBehaviorIO();
                CKBehavior* from = source ? source->GetOwner() : nullptr;
                if (from && behavior_prototype_name(from) == std::string("Key Event")
                        && source == from->GetOutput(0))
                    key_event = from;
            }
            if (!key_event) continue;  // the debug leaf (fed through an Op) or an unwired block
            navigation_leaf leaf;
            leaf.index = leaf_index;
            leaf.force_block = sub->GetID();
            leaf.key_block = key_event->GetID();
            for (int k = 0; k < count; ++k)
                if (navigation->GetSubBehavior(k) == key_event) leaf.key_order = k;
            if (CKParameterIn* in = key_event->GetInputParameter(0))
                if (CKParameter* source = in->GetRealSource()) source->GetValue(&leaf.key);
            if (CKParameterIn* in = sub->GetInputParameter(2))
                if (CKParameter* source = in->GetRealSource()) source->GetValue(&leaf.direction);
            if (CKParameterIn* in = sub->GetInputParameter(4))
                if (CKParameter* source = in->GetRealSource()) source->GetValue(&leaf.force_value);
            if (!out.direction_ref)
                if (CKParameterIn* in = sub->GetInputParameter(3))
                    if (CKParameter* source = in->GetRealSource())
                        if (CKObject* ref = source->GetValueObject()) out.direction_ref = ref->GetID();
            out.leaves.push_back(leaf);
        }
        if (out.leaves.empty()) out.error = "no SetPhysicsForce leaf is driven by a Key Event";
        std::sort(out.leaves.begin(), out.leaves.end(),
                  [](const navigation_leaf& a, const navigation_leaf& b) { return a.key_order < b.key_order; });
        for (size_t i = 0; i < out.leaves.size(); ++i) out.leaves[i].index = static_cast<int>(i);
        return out;
    }
}
