#pragma once

// The mechanism scripts' part of the rollback binding (game/script_replay.hpp):
// their state with every record, back at the anchor, and their frame before
// every replayed step - with the bridge event log muted, so a replayed Physics
// WakeUp is not reported to the server as a live BodyRevived.  Separate from
// session/rollback_binding.hpp because it needs the CK headers; the core
// binding does not, so the unit tests can bind a fake backend.

#include <game/array_reads.hpp>
#include <game/script_replay.hpp>
#include <session/rollback_binding.hpp>
#include <session/script_order.hpp>

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
        // Step RO1 (session/script_order.hpp): the order the host's own live
        // frames ran the selected scripts in, recorded after every live frame
        // under the tick a replay re-runs that frame as; each replayed frame
        // runs them in it (order_by_live_rank: a script it does not name
        // falls back to the selection's order).  Null: the selection's order,
        // as before (the legacy retail path).  `live_orders_applied` counts
        // the replayed frames that had one.
        const live_script_order* live_order = nullptr;
        uint64_t* live_orders_applied = nullptr;
        // Step RO1: a string parameter whose length changed since the record
        // is written back at the record's length (script_replay
        // resize_strings).  Off: left alone, as before (the legacy path).
        bool resize_strings = false;
        // Step B2SW (resync finding G1, rollback-engine gap F): the restore
        // re-registers with the behaviour manager what it writes back active,
        // and a replayed frame processes a script's next-frame flags only
        // where the live frame would (script_replay live_membership).  Off:
        // as before (the legacy path).
        bool live_membership = false;
        // Step B7EG (rollback-engine gaps design 2.2.3-2.2.4, gap Q): the
        // PreSimulate queue's entries of the selected and trafo scripts
        // travel with the records (record v4 / v5) and come back with them,
        // and every other script's entry is held out of the queue for the
        // whole of a replay and put back after it (rollback_world's
        // hold_outside_callbacks / unhold_outside_callbacks).  Only with
        // replay_activation (every selected script written and executed).
        // Off: as before (the legacy path).
        bool callback_queue = false;
        // Step RLQ (roadmap section 4 item 4, the harness half of gap Q):
        // Q4's hold alone, bound without replay_activation too - SimTool's
        // --savestate-check on its v14 recipe (a script inactive at the
        // anchor keeps its live state), which the clients never run.  The
        // hold is the clients' code below; its executed set is still every
        // selected and trafo script, so an entry of a selected script the
        // v14 restore does not write stays compared (fail-closed, the
        // refusal of before).  No queue section is recorded without
        // callback_queue and replay_activation.  Off: as before.
        bool hold_outside = false;
        // Step G3 (docs/world-agreement-gaps-design.md 4.2): the proximity
        // union's hold section (game/proximity_union.hpp capture / restore)
        // with every record - record v6 - and back at the anchor.  Bound by
        // a client host under WORLD_PROXIMITY_UNION only; empty: no section,
        // the records stay v2-v5 byte for byte.
        bmmo::game::hold_hooks hold;
        // Step RK2 (record-format-gaps design 11 Q4, the user's decision:
        // "the written cells are recorded with the other cells"): the
        // record's written-cell section - every row of each int / float
        // column a selected script writes (game/array_reads.hpp
        // written_cells, found here once from the selection), captured with
        // every record and written back at the anchor with the three cells'
        // rule (replay_activation).  Off: no section, the records as before
        // (the legacy path).  `written_columns`, when given, receives the
        // columns found (the host's report).
        bool written_cells = false;
        std::vector<bmmo::game::written_column>* written_columns = nullptr;
        // Step OPB (rewind-cost-design R.4 row B, option B1): the host's
        // memo of those columns (game/written_cells_memo.hpp), so a binding
        // walks the selection only when what the walk reads changed.  The
        // host owns it, one per selection, and drops it where it makes a new
        // selection.  Null: walked at every binding, as before.
        bmmo::game::array_reads::cells_memo* cells_memo = nullptr;
    };

    // Gap Q4's executed scripts (rollback-engine gaps design 2.2.4): every
    // selected one and every trafo script.  An entry of any other script is
    // held out of a replay (the hold below); step RLQ's savestate check
    // holds the same set around its expected blobs.
    inline std::vector<uint32_t> replay_executed_scripts(const bmmo::game::script_replay::selection& selection,
                                                         const bmmo::game::script_replay::trafo_part* trafo) {
        std::vector<uint32_t> executed(selection.scripts.begin(), selection.scripts.end());
        if (trafo) executed.insert(executed.end(), trafo->scripts.begin(), trafo->scripts.end());
        return executed;
    }

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
        // Gap Q (step B7EG): the queue hooks from the backend (bridge API
        // v23), one implementation for every host that turns them on.
        std::shared_ptr<bmmo::game::queue_hooks> queue;
        if (o.callback_queue && o.replay_activation) {
            using bmmo::game::queued_callback;
            queue = std::make_shared<bmmo::game::queue_hooks>();
            queue->capture = [b](std::vector<queued_callback>& out) {
                std::vector<bmmo_physics_callback> all;
                std::string error;
                out.clear();
                if (!b->callbacks_get(all, error)) return false;
                out.reserve(all.size());
                for (const bmmo_physics_callback& e: all) {
                    queued_callback q;
                    q.behavior = e.behavior;
                    q.owner_script = e.owner_script;
                    q.slot = e.slot;
                    q.bmmo = e.bmmo != 0;
                    out.push_back(q);
                }
                return true;
            };
            queue->restore = [b](const std::vector<CK_ID>& scope, const std::vector<queued_callback>& target, std::string& error) {
                const std::vector<uint32_t> s(scope.begin(), scope.end());
                std::vector<bmmo_physics_callback> t;
                t.reserve(target.size());
                for (const queued_callback& q: target) {
                    bmmo_physics_callback e{};
                    e.behavior = q.behavior;
                    e.owner_script = q.owner_script;
                    e.slot = q.slot;
                    e.bmmo = q.bmmo ? 1 : 0;
                    t.push_back(e);
                }
                return b->callbacks_rebuild(s, t, error);
            };
        }
        if ((o.callback_queue && o.replay_activation) || o.hold_outside) {
            using bmmo::game::script_replay;
            // Q4: every entry of a script the replay does not execute is
            // live state - held out of the queue from before the restore
            // touches anything to the end of the replay, so no replayed
            // Process() runs it and no restore refuses for it; the queue
            // check (the blob's) then compares the executed scripts' entries
            // only.  The executed scripts: every selected one and every
            // trafo script (replay_activation; with hold_outside alone, see
            // there).
            const auto log = world.log;
            auto hold_logs = std::make_shared<uint32_t>(0);
            world.hold_outside_callbacks = [b, selection, trafo, log, hold_logs](uint32_t anchor, bool have_record) {
                const std::vector<uint32_t> executed = replay_executed_scripts(*selection, trafo);
                std::string error;
                const int held = b->callbacks_hold(executed, error);
                auto& stats = script_replay::queue_stats();
                if (held < 0) {
                    ++stats.hold_fail;
                    if (log && (*hold_logs)++ < 8)
                        log("PreSimulate queue: the hold at the restore to tick " + std::to_string(anchor) + " failed: " + error);
                    return -1;
                }
                ++stats.holds;
                stats.held += static_cast<uint64_t>(held);
                if (!have_record) ++stats.no_record;
                if (held > 0 && log && (*hold_logs)++ < 8)
                    log("PreSimulate queue: " + std::to_string(held) + " entr" + (held == 1 ? "y" : "ies")
                        + " of scripts the replay does not run held out of the replay from tick " + std::to_string(anchor));
                b->callback_scope_set(1, executed, error);
                return held;
            };
            world.unhold_outside_callbacks = [b]() {
                std::string error;
                const int clamped = b->callbacks_unhold(error);
                if (clamped > 0) script_replay::queue_stats().unhold_clamped += static_cast<uint64_t>(clamped);
                b->callback_scope_set(0, {}, error);
            };
        }
        const bmmo::game::queue_hooks* queue_ptr = queue.get();
        // Step G3: the hold section's hooks, when the host bound them.
        std::shared_ptr<bmmo::game::hold_hooks> hold;
        if (o.hold.capture && o.hold.restore) hold = std::make_shared<bmmo::game::hold_hooks>(o.hold);
        const bmmo::game::hold_hooks* hold_ptr = hold.get();
        // Step RK2: the written-cell section's columns, from the selection.
        std::shared_ptr<std::vector<bmmo::game::written_column>> cells;
        if (o.written_cells) {
            cells = std::make_shared<std::vector<bmmo::game::written_column>>(
                o.cells_memo ? bmmo::game::array_reads::written_cells(context, *selection, *o.cells_memo)
                             : bmmo::game::array_reads::written_cells(context, *selection));
            if (o.written_columns) *o.written_columns = *cells;
        }
        const std::vector<bmmo::game::written_column>* cells_ptr = cells && !cells->empty() ? cells.get() : nullptr;
        world.capture_scripts = [context, selection, hooks, trafo, queue, queue_ptr, hold, hold_ptr, cells, cells_ptr](
                                    std::vector<uint8_t>& out) {
            return bmmo::game::script_replay::capture(context, *selection, *hooks, out, trafo, queue_ptr, hold_ptr, cells_ptr);
        };
        world.restore_scripts = [o, context, selection, hooks, trafo, queue, queue_ptr, hold, hold_ptr](
                                    const std::vector<uint8_t>& bytes, uint32_t anchor) {
            std::vector<CK_ID> kept_live;
            if (o.reset_ticks)
                for (const auto& [id, reset_tick]: *o.reset_ticks)
                    if (reset_tick > anchor) kept_live.push_back(id);
            if (o.reset_skips) *o.reset_skips += kept_live.size();
            std::string error;
            bool& resize = bmmo::game::script_replay::resize_strings();
            resize = o.resize_strings;
            bool& membership = bmmo::game::script_replay::live_membership();
            membership = o.live_membership;
            const bool ok = bmmo::game::script_replay::restore(context, *selection, *hooks, bytes, &error, o.scripts_replayed,
                                                               &kept_live, o.replay_activation, trafo, queue_ptr, hold_ptr);
            resize = false;
            membership = false;
            if (!ok && o.on_restore_failed) o.on_restore_failed(error);
            return ok;
        };
        world.diff_scripts = [o, context, selection](const std::vector<uint8_t>& a, const std::vector<uint8_t>& c, bool& logical) {
            const auto d = bmmo::game::script_replay::diff(context, *selection, a, c);
            logical = d.logical;
            // Step RK2: equal but for the written-cell section (decision Q4,
            // never compared): the empty answer, not a mismatch (rollback.hpp).
            if (!d.any) return std::string();
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
        // Step RO1: an untracked fixed body born inside the window is
        // destroyed at the restore only when these scripts make it again:
        // the activations are re-issued in the replay (replay_activation;
        // without it a script activated inside the window keeps its live
        // state and is not executed - the v14 rule) and a selected script's
        // owner is the body's entity or one of its parents (its module).  The
        // CK-free half (a fixed birth with no lifetime at the anchor, no
        // ball) is bind_session_world's.
        if (world.remade_by_replay) {
            const auto base = world.remade_by_replay;
            const bool activations = o.replay_activation;
            world.remade_by_replay = [base, activations, context, selection](const std::string& entity, uint32_t birth_tick,
                                                                             uint32_t anchor) {
                return activations && base(entity, birth_tick, anchor)
                    && bmmo::game::script_replay::owned_by_selection(context, *selection, entity);
            };
        }
        // Gap L, L4 (rollback-engine gaps design 2.3.5, step EL9): the
        // revivable set is "a name the host's selection can kill" - the
        // host's (the trafo sets' pieces) plus every body of a module a
        // selected script owns (owned_by_selection), which the replayed
        // script takes away again at the tick the live one did.  Only for a
        // host that names a revivable set (one that revives at all).
        if (world.revivable) {
            const auto base = world.revivable;
            world.revivable = [base, context, selection](const std::string& entity) {
                return base(entity) || bmmo::game::script_replay::owned_by_selection(context, *selection, entity);
            };
        }
        const auto before = world.pre_step;
        world.pre_step = [b, o, before, context, selection, trafo](uint32_t tick) {
            if (before) before(tick);
            std::string error;
            b->set_event_log_muted(true, error);
            bool& membership = bmmo::game::script_replay::live_membership();
            membership = o.live_membership;
            // Step RO1: the replayed scripts in the order the live frame of
            // this tick ran them (a copy: scripts_replayed is the restore's).
            const std::vector<CK_ID>& listed = o.scripts_replayed ? *o.scripts_replayed : selection->scripts;
            const live_script_order::order* live = o.live_order ? o.live_order->at(tick) : nullptr;
            if (live && !live->empty() && listed.size() > 1) {
                std::vector<uint32_t> ids(listed.begin(), listed.end());
                order_by_live_rank(ids, *live, [context](uint32_t id) {
                    auto* script = CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(id)));
                    CKBeObject* owner = script ? script->GetOwner() : nullptr;
                    return owner ? owner->GetPriority() : 0;
                });
                const std::vector<CK_ID> ordered(ids.begin(), ids.end());
                if (o.live_orders_applied) ++*o.live_orders_applied;
                bmmo::game::script_replay::execute_frame(context, *selection, o.delta_ms, &ordered,
                                                         trafo && !trafo->empty() ? trafo : nullptr, live);
            } else {
                bmmo::game::script_replay::execute_frame(context, *selection, o.delta_ms, o.scripts_replayed,
                                                         trafo && !trafo->empty() ? trafo : nullptr, live);
            }
            membership = false;
            b->set_event_log_muted(false, error);
        };
    }
}
