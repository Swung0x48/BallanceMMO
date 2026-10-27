#pragma once
// Step RO2 (a measurement trace, inert unless BMMO_TRACE_LIVE_ORDER is set in
// the environment): the order each LIVE frame ran the replay's selected
// scripts in (script_replay::live_frame_order: the behaviour manager's
// m_BeObjects after the frame, each owner's scripts) and the order of
// physics_RT's movable list (CKIpionManager::m_MovableObjects, which follows
// the IVP creation and revive order), each logged by name when it changes.
// The server world (physics_world::tick: the Linux server, SimTool's
// --replay-session of a server journal) and the headless client (the core's
// record_script_order) log the same two lines, so two hosts' live orders can
// be compared by name (lockstep-without-snapshots.md, As built (step RO2)).
// Nothing is computed when the variable is absent.  A measurement aid only:
// the "last logged" state is a function-local static (physics_world keys it
// by the world's address, the headless client keeps one), so a world freed
// and another made at the same address may skip its first line, and two
// worlds ticking on different threads with the trace on would race on it.

#include "CKAll.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace bmmo::sim {
    inline bool live_order_trace_on() {
        static const bool on = [] {
            const char* v = std::getenv("BMMO_TRACE_LIVE_ORDER");
            return v && *v && *v != '0';
        }();
        return on;
    }

    // "owner/script#id@priority;..." for the ids of a live frame order.
    inline std::string live_order_names(CKContext* context, const std::vector<uint32_t>& order) {
        std::string out;
        for (const uint32_t id: order) {
            if (!out.empty()) out += ';';
            auto* beh = context ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(id))) : nullptr;
            CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
            out += owner && owner->GetName() ? owner->GetName() : "?";
            out += '/';
            out += beh && beh->GetName() ? beh->GetName() : "?";
            // the id and the owner's priority: the replay's fallback order
            // is (-priority, id)
            out += '#' + std::to_string(id) + '@' + std::to_string(owner ? owner->GetPriority() : 0);
        }
        return out;
    }

    // The movable list's names in order (physics_state's
    // describe_movable_objects with each body's "[state/unit]" cut: a sleep
    // or a wake changes no name, only an insertion or a removal does).
    inline std::string movable_order_names(const std::string& described) {
        std::string out;
        out.reserve(described.size());
        bool in_state = false;
        for (const char c: described) {
            if (c == '[') in_state = true;
            else if (c == ']') in_state = false;
            else if (!in_state) out += c;
        }
        return out;
    }
}
