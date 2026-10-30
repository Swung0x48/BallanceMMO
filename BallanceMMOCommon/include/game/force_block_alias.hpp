#pragma once

// The scene census behind physics/force_alias_table.hpp (FULL step WFA;
// world-agreement-gaps-design.md §4.3, "As built (step WFA)"): every
// SetPhysicsForce block of every root script of the scene, by its root's
// owner and its pre-order walk position, with Gameplay_Ingame's blocks
// marked outside and the proximity union's slot copies left out of the
// walk (they are units, aliased by the union; leaving them out keeps n the
// pre-copy position, so a world that copied other units - the server copies
// class `all` - numbers the originals the same).  One copy for every host:
// the server's world, the headless client and the retail mod build their
// table with it at the anchor, after the placement's apply and the union's
// copy step, before the session's first capture.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "CKAll.h"

#include <physics/force_alias_table.hpp>

#include "navigation_graph.hpp"
#include "script_replay.hpp"

namespace bmmo::game {

    namespace force_alias_detail {
        // proximity_union.hpp's pre-order walk, a skipped block's whole
        // sub-tree left out (a slot copy of a graph is skipped with its
        // children).
        template <typename F> void walk(CKBehavior* beh, const std::set<CK_ID>& skipped, F&& fn) {
            if (skipped.count(beh->GetID())) return;
            fn(beh);
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i)) walk(child, skipped, fn);
        }
    }

    // Every force block of the scene's root scripts, in (root id, walk)
    // order.  `skip`: blocks left out of the walk (the union's slot copies).
    // `lines`, when given, gets one census line per force block.
    inline std::vector<physics::force_alias::block> force_block_census(CKContext* context, const std::vector<CK_ID>& skip,
                                                                       std::vector<std::string>* lines = nullptr) {
        std::vector<physics::force_alias::block> out;
        if (!context) return out;
        const std::set<CK_ID> skipped(skip.begin(), skip.end());
        std::set<CK_ID> outside;
        for (CK_ID id: outside_force_blocks(context)) outside.insert(id);
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        std::vector<CK_ID> roots;
        for (int i = 0; i < count; ++i) {
            auto* beh = CKBehavior::Cast(context->GetObject(ids[i]));
            if (beh && !beh->GetParent() && !skipped.count(beh->GetID())) roots.push_back(beh->GetID());
        }
        std::sort(roots.begin(), roots.end());
        for (CK_ID root_id: roots) {
            auto* root = CKBehavior::Cast(context->GetObject(root_id));
            CKBeObject* owner = root->GetOwner();
            const std::string owner_name = owner && owner->GetName() ? owner->GetName() : "";
            int n = 0;
            force_alias_detail::walk(root, skipped, [&](CKBehavior* b) {
                const int position = n++;
                if (!script_replay::is_force_block(b)) return;
                physics::force_alias::block rec;
                rec.id = static_cast<uint32_t>(b->GetID());
                rec.owner = owner_name;
                rec.walk = position;
                rec.outside = outside.count(b->GetID()) != 0;
                if (lines)
                    lines->push_back("force block " + std::to_string(rec.id) + " owner '" + owner_name + "' script '"
                                     + (root->GetName() ? root->GetName() : "") + "' walk " + std::to_string(position)
                                     + (rec.outside ? " outside" : ""));
                out.push_back(std::move(rec));
            });
        }
        return out;
    }

    // The table of a scene: the census, built.
    inline void build_force_alias_table(physics::force_alias::table& table, CKContext* context, const std::vector<CK_ID>& skip,
                                        std::vector<std::string>* lines = nullptr) {
        table.build(force_block_census(context, skip, lines));
    }
}
