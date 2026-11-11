#pragma once
// Parameter links that cross root scripts.
//
// Every reuse of a script's recorded bytes - step OPC, the kept source, the
// slot memo, the restore skip (game/script_record_reuse.hpp,
// game/script_slots.hpp) - rests on one premise: a root script that is not
// executed keeps its bytes.  The engine has one way around it that a graph
// shows: a CKParameterOut copies its value into its destinations whenever it
// is written.  A block's output is written when its script runs, so its
// destination in a block of ANOTHER root script changes that script without
// its running.  A parameter operation's output is written whenever anything
// reads it (the read runs the operation), from whichever script, so its
// destination changes its script without its running even when that script
// owns the operation.  A root with such a destination is FED.  script_feeds
// lists every such link; a caller takes the fed roots out of the reuses.
//
// Not listed: an input that reads another script's parameter (a read; what
// reading an operation's output writes is listed above), a parameter no
// block owns, and what a building block writes through an input's source at
// run time, which no graph shows.

#include <cstdint>
#include <vector>

#include "CKAll.h"

namespace bmmo::game {
    // The root script a parameter belongs to: its owner block - an
    // operation's parameter, that operation's owner - up to the top.  Null
    // for a parameter of no block.
    inline CKBehavior* script_of_parameter(CKParameter* parameter) {
        CKObject* owner = parameter ? parameter->GetOwner() : nullptr;
        if (owner && CKIsChildClassOf(owner, CKCID_PARAMETEROPERATION))
            owner = static_cast<CKParameterOperation*>(owner)->GetOwner();
        if (!owner || owner->GetClassID() != CKCID_BEHAVIOR) return nullptr;
        auto* beh = static_cast<CKBehavior*>(owner);
        while (beh->GetParent()) beh = beh->GetParent();
        return beh;
    }

    struct script_feed {
        CK_ID out = 0;           // the writing parameter
        CK_ID destination = 0;   // the parameter it writes
        CK_ID source = 0;        // the root script of `out`, 0 when no block owns it
        CK_ID target = 0;        // the root script of `destination`
        bool operation = false;  // `out` is a parameter operation's: written when the operation is read
    };

    // Every link from a block's output into a block of another root script,
    // and from an operation's output into any block.
    inline std::vector<script_feed> script_feeds(CKContext* context) {
        std::vector<script_feed> feeds;
        if (!context) return feeds;
        const int count = context->GetObjectsCountByClassID(CKCID_PARAMETEROUT);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_PARAMETEROUT);
        for (int i = 0; i < count; ++i) {
            CKObject* object = context->GetObject(ids[i]);
            if (!object || object->GetClassID() != CKCID_PARAMETEROUT) continue;
            auto* out = static_cast<CKParameterOut*>(object);
            const int destinations = out->GetDestinationCount();
            if (destinations <= 0) continue;
            CKBehavior* source = script_of_parameter(out);
            CKObject* owner = out->GetOwner();
            const bool operation = owner && CKIsChildClassOf(owner, CKCID_PARAMETEROPERATION);
            for (int d = 0; d < destinations; ++d) {
                CKParameter* destination = out->GetDestination(d);
                CKBehavior* target = script_of_parameter(destination);
                if (!target || (target == source && !operation)) continue;
                feeds.push_back({out->GetID(), destination->GetID(), source ? source->GetID() : 0, target->GetID(), operation});
            }
        }
        return feeds;
    }
}
