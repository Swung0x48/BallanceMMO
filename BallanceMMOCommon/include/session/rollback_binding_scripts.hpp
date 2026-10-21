#pragma once

// The mechanism scripts' part of the rollback binding (game/script_replay.hpp):
// their state with every record, back at the anchor, and their frame before
// every replayed step - with the bridge event log muted, so a replayed Physics
// WakeUp is not reported to the server as a live BodyRevived.  Separate from
// session/rollback_binding.hpp because it needs the CK headers; the core
// binding does not, so the unit tests can bind a fake backend.

#include <game/script_replay.hpp>
#include <session/rollback_binding.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class CKContext;

namespace bmmo::session {

    struct script_binding_options {
        CKContext* context = nullptr;
        const bmmo::game::script_replay::selection* scripts = nullptr;
        const bmmo::game::script_replay_hooks* hooks = nullptr;
        std::vector<CK_ID>* scripts_replayed = nullptr;     // the scripts the last restore wrote: active at its anchor
        // A script the game reset after the anchor keeps its live state, since
        // the anchor's record is what the reset tore down: the caller's
        // bookkeeping of the tick each script was last told to reset, and its
        // count of scripts kept live for that reason.
        const std::map<CK_ID, uint32_t>* reset_ticks = nullptr;
        uint64_t* reset_skips = nullptr;
        float delta_ms = 1000.0f / 66.0f;
        bool diff_detail = false;                           // diff_scripts appends every logical difference
        std::function<void(const std::string& error)> on_restore_failed;
    };

    // Installs capture/restore/diff and runs the scripts' frame after the
    // caller's own pre_step work.  No-op when there is no selection.
    template <class Backend>
    void bind_script_replay(rollback_world& world, Backend& backend, const script_binding_options& o) {
        if (!o.context || !o.scripts || !o.hooks || o.scripts->empty()) return;
        Backend* b = &backend;
        CKContext* context = o.context;
        const auto* selection = o.scripts;
        const auto* hooks = o.hooks;
        world.capture_scripts = [context, selection, hooks](std::vector<uint8_t>& out) {
            return bmmo::game::script_replay::capture(context, *selection, *hooks, out);
        };
        world.restore_scripts = [o, context, selection, hooks](const std::vector<uint8_t>& bytes, uint32_t anchor) {
            std::vector<CK_ID> kept_live;
            if (o.reset_ticks)
                for (const auto& [id, reset_tick]: *o.reset_ticks)
                    if (reset_tick > anchor) kept_live.push_back(id);
            if (o.reset_skips) *o.reset_skips += kept_live.size();
            std::string error;
            const bool ok = bmmo::game::script_replay::restore(context, *selection, *hooks, bytes, &error, o.scripts_replayed,
                                                               &kept_live);
            if (!ok && o.on_restore_failed) o.on_restore_failed(error);
            return ok;
        };
        world.diff_scripts = [o, context, selection](const std::vector<uint8_t>& a, const std::vector<uint8_t>& c, bool& logical) {
            const auto d = bmmo::game::script_replay::diff(context, *selection, a, c);
            logical = d.logical;
            return d.where + " (" + std::to_string(d.logical_count) + " logical, " + std::to_string(d.float_count) + " float)"
                 + (d.logical && o.diff_detail ? " :: " + d.detail : std::string());
        };
        const auto before = world.pre_step;
        world.pre_step = [b, o, before, context, selection](uint32_t tick) {
            if (before) before(tick);
            std::string error;
            b->set_event_log_muted(true, error);
            bmmo::game::script_replay::execute_frame(context, *selection, o.delta_ms, o.scripts_replayed);
            b->set_event_log_muted(false, error);
        };
    }
}
