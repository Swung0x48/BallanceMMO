#pragma once

// The checkpoint hold's keep rule (N3; FULL step N3K, host-platform audit D3,
// AUD-2), CK-free so the unit tests reach it.
//
// N3 feeds the Execute Script of Gameplay_Events / activate Sektor a Script
// parameter that holds no script (game/sector_activation.hpp
// checkpoint_activation_hold), so a crossed checkpoint no longer runs
// Gameplay_SectorManager in its own frame: the crossing is stamped and the
// union runs the manager at that tick in every world.  A level reset that
// restored the graph would hand the input its retail source back, and the
// next crossing would run the manager in this world alone.  So every frame
// the hold is kept: the input re-held when it reads the retail source again,
// left alone when it reads anything else.  The rule was the legacy retail
// copy's (physics_session_client.cpp sector_activate_apply); from N3K both
// clients keep N3 by this one rule.

#include <cstdint>

namespace bmmo::session {

    // Where the held block's Script input reads from now.
    enum class checkpoint_hold_state : uint8_t {
        not_held = 0,   // no hold in force (none was made, or it failed)
        held,           // the hold's own parameter: nothing to do
        handed_back,    // the retail source again (a level reset restored it): re-hold
        foreign,        // anything else - no source, or one neither ours nor retail's: left alone
    };

    inline const char* checkpoint_hold_state_name(checkpoint_hold_state state) {
        switch (state) {
            case checkpoint_hold_state::held: return "held";
            case checkpoint_hold_state::handed_back: return "handed back";
            case checkpoint_hold_state::foreign: return "foreign source";
            default: return "not held";
        }
    }

    // By object id: `current` the input's real source now (0: none), `none`
    // the hold's parameter (0: no hold), `retail_real` the real source it had
    // before the hold.
    inline checkpoint_hold_state classify_checkpoint_hold(uint32_t current, uint32_t none, uint32_t retail_real) {
        if (none == 0) return checkpoint_hold_state::not_held;
        if (current == none) return checkpoint_hold_state::held;
        if (current != 0 && current == retail_real) return checkpoint_hold_state::handed_back;
        return checkpoint_hold_state::foreign;
    }

    // The keep over an input and the hold's parameter (CK: CKParameterIn and
    // CKParameter; the tests' fakes have the same three calls): the input
    // re-held when it was handed back.  `reheld`: it was, and the input took
    // the parameter again (SetDirectSource returned CK_OK, 0).  Returns the
    // state found before the re-hold.
    template <class Input, class Parameter>
    checkpoint_hold_state keep_checkpoint_hold(Input& input, Parameter& none, uint32_t retail_real, bool& reheld) {
        reheld = false;
        Parameter* current = input.GetRealSource();
        const checkpoint_hold_state state = classify_checkpoint_hold(
            current ? static_cast<uint32_t>(current->GetID()) : 0u, static_cast<uint32_t>(none.GetID()), retail_real);
        if (state == checkpoint_hold_state::handed_back) reheld = input.SetDirectSource(&none) == 0;
        return state;
    }
}
