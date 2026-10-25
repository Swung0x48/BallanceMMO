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
#include <memory>
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
        // The caller re-issues every sector activation inside the replayed
        // window (plan doc 10): every selected script is then written back,
        // the inactive ones included, and the recorded data-array cells with
        // them.  Off, the v14 rule: a script inactive at the anchor, or reset
        // after it, keeps its live state and is not executed.
        bool replay_activation = false;
        std::function<void(const std::string& error)> on_restore_failed;
        // The trafo sets' scripts (game/trafo_sets.hpp, step T1): captured
        // in record v3, restored (a script the record does not list to its
        // set's built state), and run by the replayed frame before the
        // selection, as the trafo frame runs them live.  Null: none.
        const bmmo::game::script_replay::trafo_part* trafo = nullptr;
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
        const auto* trafo = o.trafo;
        world.capture_scripts = [context, selection, hooks, trafo](std::vector<uint8_t>& out) {
            return bmmo::game::script_replay::capture(context, *selection, *hooks, out, trafo);
        };
        world.restore_scripts = [o, context, selection, hooks, trafo](const std::vector<uint8_t>& bytes, uint32_t anchor) {
            std::vector<CK_ID> kept_live;
            if (o.reset_ticks)
                for (const auto& [id, reset_tick]: *o.reset_ticks)
                    if (reset_tick > anchor) kept_live.push_back(id);
            if (o.reset_skips) *o.reset_skips += kept_live.size();
            std::string error;
            const bool ok = bmmo::game::script_replay::restore(context, *selection, *hooks, bytes, &error, o.scripts_replayed,
                                                               &kept_live, o.replay_activation, trafo);
            if (!ok && o.on_restore_failed) o.on_restore_failed(error);
            return ok;
        };
        world.diff_scripts = [o, context, selection](const std::vector<uint8_t>& a, const std::vector<uint8_t>& c, bool& logical) {
            const auto d = bmmo::game::script_replay::diff(context, *selection, a, c);
            logical = d.logical;
            return d.where + " (" + std::to_string(d.logical_count) + " logical, " + std::to_string(d.float_count) + " float)"
                 + (d.logical && o.diff_detail ? " :: " + d.detail : std::string());
        };
        // A body the rollback destroys (rollback_world::destroy_body, bound
        // before this) takes its joints with it inside IVP, and a joint block
        // of these scripts still holding one would delete it again at its
        // next reset, Create or teardown.  The records keep that from
        // happening when they agree - a body born inside the window had no
        // joint at the anchor, and the script restore has reset it already -
        // but nothing else enforces it: the handles the destroy freed are
        // nulled right after it (script_replay::joints_on / release_joints).
        if (world.destroy_body) {
            const auto destroy = world.destroy_body;
            const auto log = world.log;
            auto logs = std::make_shared<uint32_t>(0);
            world.destroy_body = [destroy, log, logs, context, selection](const std::string& entity, uint32_t birth_tick) {
                const std::vector<CKBehavior*> held = bmmo::game::script_replay::joints_on(context, *selection, entity);
                const bool destroyed = destroy(entity, birth_tick);
                if (destroyed && !held.empty()) {
                    const uint32_t released = bmmo::game::script_replay::release_joints(held);
                    if (log && released && (*logs)++ < 8)
                        log("joints: " + std::to_string(released) + " joint handle(s) on " + entity
                            + " released with its body at the restore (the script records held them)");
                }
                return destroyed;
            };
        }
        const auto before = world.pre_step;
        world.pre_step = [b, o, before, context, selection, trafo](uint32_t tick) {
            if (before) before(tick);
            std::string error;
            b->set_event_log_muted(true, error);
            bmmo::game::script_replay::execute_frame(context, *selection, o.delta_ms, o.scripts_replayed,
                                                     trafo && !trafo->empty() ? &trafo->scripts : nullptr);
            b->set_event_log_muted(false, error);
        };
    }
}
