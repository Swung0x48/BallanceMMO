#pragma once

// The one place the rollback engine's hooks (session/rollback.hpp,
// rollback_world) are bound to a physics world.  Every user of the engine goes
// through it: the headless session client and the SimTool's savestate check
// with the in-process backend (physics/physics_direct.hpp, which calls
// bmmo::physics::* on a CKIpionManager), the retail mod with the plugin-API
// backend (BallanceMMOClient/physics/physics_view.hpp).  The two backends
// expose the same methods under physics_view's names; the binding is a
// template over them, so the mod - which has only the C API of the plugin,
// not the headless library - compiles the same code.
//
// What is bound here is everything a rollback needs from the world and
// nothing a transport or a session knows: body poses and leapfrog extras, the
// clock, friction contacts, the navigation replicas, the IVP savestate, one
// physics step and the caller's work before it.  The mechanism scripts' state
// (game/script_replay.hpp, which needs the CK headers) is bound on top by
// session/rollback_binding_scripts.hpp; this header knows no CK type, so the
// unit tests bind it to a fake backend.  Whatever differs between the users -
// traces, logs, journals, the pause menu's input mute, the deferred births of
// a replay - comes in through rollback_binding_options and never as a second
// copy of a hook (docs/rollback-core-unification-plan.md).

#include <physics/physics_rt_api.h>
#include <physics/physics_state.hpp>   // bmmo::physics::wake_mode, SAVESTATE_* flags (declarations only)
#include <session/rollback.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bmmo::session {

    struct rollback_binding_options {
        float delta_ms = 1000.0f / 66.0f;          // one replayed physics step
        bool restore_clock = false;                // install set_clock (the engine's own switch decides whether it runs)
        bool savestate = true;                     // install get_savestate / set_savestate at all
        uint32_t savestate_flags = 0;              // bmmo::physics::SAVESTATE_* bits for set_savestate

        // Navigation polling of the own ball (the key codes and Key Event
        // blocks the replica reads in live frames; the backend is called with
        // whatever is here, as the clients always did - disabling needs no
        // codes, enabling without them fails in the bridge).  `input_muted`:
        // polling is not switched back on after a replay while it says so
        // (the retail pause menu).  `on_poll_resumed`: runs after polling was
        // switched back on for an entity, i.e. at the end of every rewind
        // that replayed - the place to re-arm the live frame's own navigation
        // input, which the replay's own input feed consumed (without it a key
        // edge on the first live tick after a replay lands a tick late).
        const int* own_key_codes = nullptr;
        const uint32_t* own_key_blocks = nullptr;
        int own_key_count = 0;
        std::function<bool()> input_muted;
        std::function<void(const std::string& entity)> on_poll_resumed;

        // Callbacks, all optional.
        std::function<void(uint32_t tick)> before_replayed_tick;   // before a replayed tick's step (and its scripts' frame)
        std::function<bool()> whole_frame;                         // --replay-scripts: the step is the whole engine frame
        std::function<void(uint32_t tick, std::map<std::string, uint32_t>& out)> replay_remotes;   // rollback_world::replay_remotes
        std::function<std::vector<std::string>()> replay_mechanisms;   // rollback_world::replay_mechanisms
        std::function<void(const std::string& entity, int made, const std::string& error,
                           const std::vector<bmmo_physics_contact>& recorded)> on_contacts_restored;
        std::function<void(const std::string& error)> on_savestate_capture_failed;
        std::function<void(bool ok, const std::string& error, const savestate_outcome& outcome)> on_savestate_restored;
        std::function<void(const std::string& error)> on_write_failed;   // a body, recheck or clock write the backend refused
        std::function<void(const std::string&)> log;
        std::function<void(const rollback_correction&)> on_correction;
    };

    // `backend` must outlive the returned world (every hook keeps a pointer;
    // both clients rebuild the world on every engine call, so the backend is
    // a member of theirs, never a temporary).
    template <class Backend>
    rollback_world bind_rollback_world(Backend& backend, const rollback_binding_options& o) {
        Backend* b = &backend;
        rollback_world world;
        const auto failed = [o](const std::string& error) {
            if (o.on_write_failed) o.on_write_failed(error);
        };
        world.get_body = [b](const std::string& entity, bmmo_physics_body_state& out) {
            std::string error;
            return b->get_body_state(entity.c_str(), out, error);
        };
        // The engine decides the sleep handling and the contact recheck per
        // write (design 9.25): `keep` is the common case - most restores move
        // a body already in the state the server reports, and
        // ensure_in_simulation would re-arm its freeze timers (engine change
        // #14); `recheck` only when the write moved the body, because the
        // optimized beam builds no contact when it lands one body inside
        // another.  The wake_mode overload, never the old `bool wake` beam.
        world.set_body = [b, failed](const std::string& entity, const bmmo_physics_body_state& state, wake_mode mode,
                                     bool recheck) {
            std::string error;
            const bmmo::physics::wake_mode bridge_mode = mode == wake_mode::wake   ? bmmo::physics::wake_mode::wake
                                                       : mode == wake_mode::freeze ? bmmo::physics::wake_mode::freeze
                                                                                   : bmmo::physics::wake_mode::keep;
            if (b->set_body_state(entity.c_str(), state.position, state.rotation, state.linear, state.angular, bridge_mode,
                                  recheck, error))
                return true;
            failed(error);
            return false;
        };
        // The restore's collision recheck as a pass of its own (bridge v16).
        world.recheck_body = [b, failed](const std::string& entity) {
            std::string error;
            if (b->body_recheck(entity.c_str(), error)) return true;
            failed(error);
            return false;
        };
        // The queued impulses and the rest of the leapfrog state (bridge v11):
        // always captured; the engine's switches decide what a restore writes
        // back, so every half is an A/B in one binary.
        world.get_body_extra = [b](const std::string& entity, bmmo_physics_core_extra& out) {
            std::string error;
            return b->get_core_extra(entity.c_str(), out, error);
        };
        world.set_body_extra = [b](const std::string& entity, const bmmo_physics_core_extra& state) {
            std::string error;
            return b->set_core_extra(entity.c_str(), state, error);
        };
        // The clock is always READ (a restore shifts the calm references it
        // puts back by the live clock's offset); it is written only when the
        // caller asked for the clock rewind (the headless --restore-clock,
        // where it is measured; never the retail mod - the engine's own
        // restore_clock_ switch defaults on, so not installing the hook is the
        // mod's one way of saying no).
        world.get_clock = [b](bmmo_physics_clock_state& out) {
            std::string error;
            return b->get_clock_state(out, error);
        };
        if (o.restore_clock) {
            world.set_clock = [b, failed](const bmmo_physics_clock_state& state) {
                std::string error;
                if (b->set_clock_state(state, error)) return true;
                failed(error);
                return false;
            };
        }
        // Before each replayed tick's step: the caller's own work (the births
        // deferred into this replay, traces).  bind_script_replay wraps this
        // to run the mechanism scripts' frame after it.
        if (o.before_replayed_tick) world.pre_step = o.before_replayed_tick;
        // The remotes a replay drives although the record does not name them
        // (a late birth made inside the replay).
        if (o.replay_remotes) world.replay_remotes = o.replay_remotes;
        // The mechanism bodies a replayed tick's record names although the
        // live record did not (a body the replay made earlier).
        if (o.replay_mechanisms) world.replay_mechanisms = o.replay_mechanisms;
        world.get_nav = [b](const std::string& entity, bmmo_physics_nav_state& out) {
            std::string error;
            return b->navigation_get_state(entity.c_str(), out, error);
        };
        world.set_nav = [b](const std::string& entity, const bmmo_physics_nav_state& state) {
            std::string error;
            return b->navigation_set_state(entity.c_str(), state, error);
        };
        // A body's friction contacts (bridge v15).
        world.get_contacts = [b](const std::string& entity, std::vector<bmmo_physics_contact>& out) {
            std::string error;
            bmmo_physics_contact buffer[16];
            const int n = b->contacts_get(entity.c_str(), buffer, 16, error);
            if (n < 0) return -1;
            out.assign(buffer, buffer + std::min(n, 16));
            return n;
        };
        world.set_contacts = [b, o](const std::string& entity, const std::vector<bmmo_physics_contact>& in, uint32_t flags) {
            std::string error;
            const int made = b->contacts_set(entity.c_str(), in.data(), static_cast<int>(in.size()), flags, error);
            if (o.on_contacts_restored) o.on_contacts_restored(entity, made, error, in);
            return made;
        };
        // The IVP savestate (bridge v17/v18, docs/ivp-savestate-design.md):
        // every record carries the engine's whole state; a restore puts it
        // back whole, lenient unless the caller asks for strict.
        if (o.savestate) {
            world.get_savestate = [b, o](std::vector<uint8_t>& out) {
                std::string error;
                if (b->savestate_get(out, error)) return true;
                if (o.on_savestate_capture_failed) o.on_savestate_capture_failed(error);
                return false;
            };
            world.set_savestate = [b, o](const std::vector<uint8_t>& blob, savestate_outcome& outcome) {
                std::string error;
                const bool ok = b->savestate_set(blob.data(), blob.size(), o.savestate_flags, error, &outcome.contacts_dropped,
                                                 &outcome.unknown_bodies);
                if (o.on_savestate_restored) o.on_savestate_restored(ok, error, outcome);
                return ok;
            };
        }
        world.nav_input = [b](const std::string& entity, const input_frame& frame) {
            std::string error;
            const bool active = (frame.flags & INPUT_FLAG_NAV_ACTIVE) != 0;
            return b->navigation_input(entity.c_str(), frame.keys, frame.cam_right, frame.cam_up, frame.cam_dir, active, error);
        };
        // A replay must not switch the polling back on while the pause menu
        // has it off: the ball would follow the held arrow keys again.
        world.nav_poll = [b, o](const std::string& entity, bool enable) {
            std::string error;
            const bool muted = o.input_muted && o.input_muted();
            const bool on = enable && !muted;
            const bool ok = b->navigation_poll(entity.c_str(), on, o.own_key_codes, o.own_key_blocks, o.own_key_count, error);
            if (on && o.on_poll_resumed) o.on_poll_resumed(entity);
            return ok;
        };
        // One physics step with the fixed tick: the primitive the live frame's
        // Simulate() runs, without the frame around it - or the whole frame
        // when the caller asked (an experiment, not the lockstep recipe: the
        // scripts outside the replay's selection are not rewound).
        if (o.whole_frame) {
            world.step = o.whole_frame;
        } else {
            world.step = [b, o]() {
                std::string error;
                const bool ok = b->step_physics(o.delta_ms, error);
                if (!ok && o.log) o.log("replay step: " + error);
                return ok;
            };
        }
        // false while the local physics clock is stopped by the retail
        // scripts (Level 1 tutorial, pause menu).
        world.simulating = [b]() {
            float factor = 0.0f, delta = 0.0f;
            std::string error;
            return !b->get_clock(factor, delta, error) || (factor > 0.0f && delta > 0.0f);
        };
        world.log = o.log;
        world.on_correction = o.on_correction;
        return world;
    }
}
