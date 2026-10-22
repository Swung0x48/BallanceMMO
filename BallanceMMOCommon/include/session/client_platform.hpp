#pragma once

// The platform interface of the shared session-client core
// (docs/session-client-core-design.md 2): what the core needs from the world,
// as a struct of std::function fields each host fills with what it has.  An
// empty field means "this host cannot" (the rollback_world convention,
// session/rollback_binding.hpp); where a decision depends on more than one
// field the host declares it in session_capabilities, and the core's
// constructor checks that every field a declared capability needs is filled.
//
// Step E1 (design 6.1) starts it with the three operations every host has
// and the headless platform fills: send, now and log.  Each later step adds
// the operations its moved code calls (design 2.2), and the capability checks
// with them: E2 the journal's (journal_path, utc_ms, body_state, list_bodies);
// E3 the first host hooks (session_hooks: numbering_restarted, filter_event,
// ending); E4 the snapshot path's (write_body, own_driven_entity, bind_world,
// keep_live_cells); E6 the mirrors' (mirror_entity, make_mirror,
// unmake_mirror, detach_mirror_navigation, attach_mirror_navigation,
// mute_event_log).
//
// CK-free and header-only.

#include "clock_site.hpp"
#include "rollback.hpp"
#include "../physics/physics_rt_api.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bmmo {
    struct session_event_msg;   // message/session_event_msg.hpp
}

namespace bmmo::session {

    struct journal_header;   // session/journal.hpp
    class session_client_core;   // session/client_core.hpp

    // How a message goes out; the host maps it to k_nSteamNetworkingSend_*.
    enum class send_mode : uint8_t { reliable, reliable_no_nagle, unreliable_no_delay };

    enum class log_level : uint8_t { info, warn };

    // Where the own ball's Physicalize / Unphysicalize are learned (design
    // 2.3): the event log at post_step (headless), or the host's hooks,
    // queued and committed at post_step (retail).
    enum class own_lifecycle_source : uint8_t { event_log, host_hooks };

    // What the host does with an own lifecycle event the core is about to put
    // on the wire (design 2.2 hooks.filter_event): send it, or keep it - held
    // for a later session_client_core::release_event, or dropped.
    enum class event_send_verdict : uint8_t { send, hold, drop };

    // Design 2.2 "host hooks": optional, H.  An empty hook is a host with
    // nothing to do there.
    struct session_hooks {
        // The tick numbering (re)started at `site`, the pacer's origin with it:
        // the anchor (anchor_origin), the hold's timeout (hold_timeout) and a
        // SessionAssign (assign_resync, assign_renumber, assign_release).
        // Headless: origin_ = wall_now(site); retail: nothing (its fixed tick
        // counts on its own).
        std::function<void(clock_site site)> numbering_restarted;
        // An own event, counted and journaled, before it goes on the wire
        // (headless --hold-event).
        std::function<event_send_verdict(const bmmo::session_event_msg& event)> filter_event;
        // The session is ending (session_client_core::end), before anything
        // else: headless discards what --hold-event still holds.
        std::function<void()> ending;
    };

    // A mirror's body (design 2.2 make_mirror; E6): the entity the host named
    // for `player`'s ball of `ball_type` (mirror_entity), posed and
    // physicalized with the recipe in the player's collision group, and
    // kicked with the spawn impulse of `kick_index`
    // (session/spawn_impulse.hpp kSpawnDirectionTable) when `kick`.
    struct mirror_spec {
        uint32_t player = 0;
        uint8_t ball_type = 0;
        std::string entity;
        float position[3] = {};
        float rotation[9] = {};
        bmmo_physics_ball_recipe recipe{};
        std::string group;
        bool kick = false;
        uint32_t kick_index = 0;
        float kick_strength = 0.0f;
    };
    // What make_mirror did: `made` (false: `error` says why, and nothing was
    // kicked), and when the spec asked for a kick, whether it took
    // (`kick_error` otherwise).
    struct mirror_made {
        bool made = false;
        std::string error;
        bool kicked = false;
        std::string kick_error;
    };
    // attach_mirror_navigation's answer: the host has no navigation to give
    // yet (nothing done, nothing to say), the mirror is driven now, or the
    // attach failed (`error` says why).
    enum class mirror_navigation : uint8_t { unavailable, attached, failed };

    // The host's work around a scope of the core (keep_live_cells): what it
    // did on entry is the host's; `on_exit` runs when the scope ends, once.
    class scope_guard {
    public:
        scope_guard() = default;
        explicit scope_guard(std::function<void()> on_exit) : on_exit_(std::move(on_exit)) {}
        scope_guard(scope_guard&& other) noexcept : on_exit_(std::move(other.on_exit_)) { other.on_exit_ = nullptr; }
        scope_guard(const scope_guard&) = delete;
        scope_guard& operator=(const scope_guard&) = delete;
        scope_guard& operator=(scope_guard&&) = delete;
        ~scope_guard() {
            if (on_exit_) on_exit_();
        }

    private:
        std::function<void()> on_exit_;
    };

    struct session_platform {
        // ---- transport and time (design 2.2) ----
        std::function<void(const std::string& bytes, send_mode mode)> send;
        // The wall clock as the flow reads it, by the read's place in the flow
        // (session/clock_site.hpp): the headless client hands it to the
        // inbound capture or takes the captured value; retail ignores it.
        std::function<std::chrono::steady_clock::time_point(clock_site site)> now;
        // One line of text, written as it is (the harness parses the headless
        // client's lines).
        std::function<void(log_level level, const std::string& text)> log;
        // The system clock in ms since the epoch, by the read's site (the
        // journal header's utc_ms, clock_site::journal_utc).
        std::function<uint64_t(clock_site site)> utc_ms;

        // ---- reading the world (design 2.2) ----
        // One body's state by entity name; false when it has no body.
        std::function<bool(const std::string& entity, bmmo_physics_body_state& out)> body_state;
        // Every physicalized entity, in physics-table order.
        std::function<std::vector<bmmo_physics_body_state>()> list_bodies;

        // ---- writing the world (design 2.2; E4) ----
        // Beam a body to the pose and overwrite its speeds; `wake` wakes it, a
        // simulated body is frozen otherwise (physics_state.hpp set_body_state,
        // the bool overload).  `error` is the failure's text: the host clears
        // it first, as write_body_state does - the core keeps one string
        // across its writes and logs it only after a failed one.
        std::function<bool(const std::string& entity, const double position[3], const double rotation[4],
                           const float linear[3], const float angular[3], bool wake, std::string& error)> write_body;

        // ---- mirrors (design 2.2; E6) ----
        // The entity that mirrors `player` with a ball of `ball_type`, made
        // when it does not exist yet; "" with `error` when there is none.
        std::function<std::string(uint32_t player, uint8_t ball_type, std::string& error)> mirror_entity;
        // Pose, physicalize and kick a mirror's body (mirror_spec).  The core
        // asks mirror_entity for `spec.entity` first, in the same frame, but
        // the operation stands alone: the spec names the player and the ball
        // type, so a host that keeps nothing from mirror_entity finds the
        // entity again (headless: the pointer mirror_entity found when it
        // names spec.entity, else the same lookup).
        std::function<mirror_made(const mirror_spec& spec)> make_mirror;
        // A mirror's body goes (unphysicalized).  Only the body: the core
        // takes the navigation out first (detach_mirror_navigation) and mutes
        // the event log around it where its flow does (mute_event_log).
        std::function<void(const std::string& entity)> unmake_mirror;
        // A mirror's navigation replica goes.
        std::function<void(const std::string& entity)> detach_mirror_navigation;
        // A physicalized mirror without navigation is driven by the shared
        // navigation from now on (headless: the leaf directions of the
        // level's graph, the ball row's force, CamRef_BMMO_<player>).
        std::function<mirror_navigation(uint32_t player, const std::string& entity, uint8_t ball_type,
                                        std::string& error)> attach_mirror_navigation;
        // The bridge's event log muted or not; the core keeps the nesting
        // (session_client_core::event_log_mute).
        std::function<void(bool muted)> mute_event_log;

        // ---- driving (design 2.2; E4) ----
        // The entity the own navigation replica drives, "" when there is none.
        std::function<std::string()> own_driven_entity;

        // ---- rollback (design 2.2; E4) ----
        // The rollback engine's view of the host's world, bound anew on every
        // use, never cached (the options take the own key count by value).
        // E4: the host's own binding (headless rollback_world()); the core's
        // hooks and bind_session_world arrive with the frame (E9).
        std::function<rollback_world()> bind_world;
        // Around every rewind_to / on_snapshot: the host keeps the live cells
        // the replay rewinds (headless live_cells_guard) and re-issues what a
        // rollback lost when the returned guard goes out of scope.  The guard
        // reads the core's rollback_stats() and current_tick() (headless
        // reads the same state through core_.s until E11).
        std::function<scope_guard(const session_client_core& core)> keep_live_cells;

        // ---- journal (design 1.2) ----
        // The whole path of the session's journal for this header (the
        // session, level and own player are filled; utc_ms is not yet); an
        // empty path means no journal.  An empty field means this host never
        // journals.  Headless: --journal verbatim; retail (R1): its journal
        // directory and file name, after pruning old files.
        std::function<std::filesystem::path(const journal_header& header)> journal_path;

        // ---- host hooks (design 2.2), all optional ----
        session_hooks hooks;
    };

    // Design 2.3.  Every flag defaults to "cannot"; a host declares what it
    // can.  Each comes with the operations it needs (own_body_control:
    // make_own / unmake_own at E7; whole_frame with bind_world at E9).  From
    // E3 the core reads `slew` (SessionReady's caps: CAP_LEAD iff slew); the
    // pacer operations it will need arrive at E5/E9.  `nonblocking_hold`
    // changes nothing in the core (one hold state machine, poll_hold); it
    // says how the host waits.
    struct session_capabilities {
        bool own_body_control = false;   // make_own / unmake_own filled
        bool slew = false;               // the host's pacer applies slew_step (2x raise, 0.9x lower)
        bool nonblocking_hold = false;   // the anchor hold returns to the host loop
        bool whole_frame = false;        // a replayed tick may run a whole engine frame (bind_world fills whole_frame)
        own_lifecycle_source own_lifecycle = own_lifecycle_source::event_log;   // or host_hooks
    };

    // legacy_calls (design 6.1, "Linking while the core is incomplete") is in
    // session/client_core.hpp from E3 on: its entries name the core state's
    // types (own_life::aside).

    // The first operation `platform` lacks that every host must fill, or that
    // a capability `caps` declares needs; nullptr when nothing is missing.
    inline const char* missing_platform_operation(const session_platform& platform, const session_capabilities& caps) {
        (void)caps;
        if (!platform.send) return "send";
        if (!platform.now) return "now";
        if (!platform.log) return "log";
        // The snapshot path (E4): every host applies snapshots.
        if (!platform.body_state) return "body_state";
        if (!platform.write_body) return "write_body";
        if (!platform.own_driven_entity) return "own_driven_entity";
        if (!platform.bind_world) return "bind_world";
        if (!platform.keep_live_cells) return "keep_live_cells";
        // The remote lifecycle (E6): every host mirrors the other players.
        if (!platform.mirror_entity) return "mirror_entity";
        if (!platform.make_mirror) return "make_mirror";
        if (!platform.unmake_mirror) return "unmake_mirror";
        if (!platform.detach_mirror_navigation) return "detach_mirror_navigation";
        if (!platform.attach_mirror_navigation) return "attach_mirror_navigation";
        if (!platform.mute_event_log) return "mute_event_log";
        // A host that journals (session/client_journal.hpp) stamps the header,
        // and writes bodies into LOCAL checkpoints (body_state, which body
        // corrections read too, is required above).
        if (platform.journal_path) {
            if (!platform.utc_ms) return "utc_ms";
            if (!platform.list_bodies) return "list_bodies";
        }
        return nullptr;
    }

}
