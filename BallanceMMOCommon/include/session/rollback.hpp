#pragma once

// Client-side rollback (design 9.6, restore rules rewritten for 9.25): the
// client runs ahead of the server's confirmed progress and predicts; when the
// authoritative snapshot of tick T disagrees with what the client recorded at
// T, the whole shared world is rewound to T - every body the snapshot carries
// a row for goes to its row, every other recorded body and navigation replica
// to its own record - and the ticks T+1 .. now are re-simulated (physics +
// navigation only, no scripts) from the recorded inputs.  Pure logic over a
// small world adapter, shared by the retail mod (through the physics bridge)
// and the headless session client.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../entity/session.hpp"
#include "clock_site.hpp"
#include "../message/message_utils.hpp"
#include "../message/session_snapshot_msg.hpp"
#include "../physics/physics_rt_api.h"

namespace bmmo::session {
    // What the engine did about an authoritative snapshot, for the session
    // journal (session/journal.hpp CORRECTION records) and any other observer.
    // `kind` uses the journal's numbering; this engine emits 0 mismatch,
    // 1 rollback, 5 too_far, 6 frozen and 7 unmatched.  `entity` is the body
    // furthest past its own threshold (empty when there is none) - a mismatch
    // can be triggered by velocity alone, and then the largest position error
    // names the wrong body - and the positions are that body's local and
    // server pose at `tick`.
    struct rollback_correction {
        uint32_t tick = 0;         // the snapshot's tick
        uint32_t local_tick = 0;   // where the client was when it arrived
        uint8_t kind = 0;
        std::string entity;
        double error_m = 0.0;
        double velocity_error = 0.0;
        double local_position[3] = {};
        double server_position[3] = {};
    };

    // What a restore does to the body's sleep state.  Mirrors the values of
    // bmmo::physics::wake_mode (physics/physics_state.hpp), which is what the
    // adapters forward to the bridge: `keep` never touches the sleep state,
    // `wake` revives a body that is not simulated (and leaves an awake body's
    // freeze timers alone), `freeze` disables simulation for a simulated body.
    enum class wake_mode : uint8_t { keep = 0, wake = 1, freeze = 2 };

    // A restore that barely moves a body cannot create a new contact, and the
    // mindist recheck it would trigger is not free; past these two the write
    // may drop the body into something, so the adapter is asked to rebuild the
    // contact pairs.  1 mm is under the pose noise the tolerances already
    // accept, 1 cm/s under a tick of gravity (0.15 m/s).
    constexpr double kRecheckPositionM = 0.001;
    constexpr double kRecheckVelocityMs = 0.01;

    // What a savestate restore (rollback_world::set_savestate) leaves to the
    // engine: see the hook's comment.
    struct savestate_outcome {
        std::vector<std::string> contacts_dropped;   // covered bodies whose contacts the blob could not carry
        std::vector<std::string> unknown_bodies;     // live bodies the blob does not know, out of collision detection
        // A travelling blob (step A3): the restore's counts and first
        // warnings ("skipped=N excluded=N renamed=N | ...").
        std::string summary;
    };

    // The server's IVP savestate as a resync carries it (resync design 2.3,
    // 9 Q1; step A3): restored once into this world as a TRAVELLING blob
    // (physics/physics_savestate.hpp savestate_travel) - another process's,
    // the resynced member's own clone renamed to its own ball, the
    // keep-local bodies left out on both sides.
    struct travelling_blob {
        const std::vector<uint8_t>* blob = nullptr;
        std::vector<std::pair<std::string, std::string>> renames;
        std::vector<std::string> excluded_prefixes;
    };

    // What a resync re-step restores at its anchor S (resync design 2.6.4,
    // 4.1; rollback_engine::resync), decided by the session core:
    //   scripts  the server's script record, localized to this world (v2
    //            bytes): the anchor's own record is replaced by it before the
    //            restore (empty: the anchor's own stays);
    //   blob     the server's savestate (blob null or empty: the anchor's own);
    //   rows     the server's row of every body it has at S, by local entity:
    //            authoritative, the bodies the anchor's records show born in
    //            the window included (a row outranks a birth); a row the body
    //            already has bit for bit after the blob is not written (the
    //            write is a beam: A2's rule);
    //   destroy  bodies of the anchor world the server's state at S does not
    //            have, by the tick they were born at (a pass this world ran
    //            after S, a mirror born after S): destroyed at the restore and
    //            made again by the replay at their tick, never re-posed.
    //   navs     the server's navigation of each ball at S, by local entity:
    //            set in place of the anchor's before the blob (its keys down
    //            and the forces their controllers were made with).
    struct resync_spec {
        std::vector<uint8_t> scripts;
        travelling_blob blob;
        std::map<std::string, bmmo_physics_nav_state> navs;
        std::map<std::string, const body_state*> rows;
        std::map<std::string, uint32_t> destroy;
        // Step T3 (trafo-replication design 4.7, the reconcile): bodies the
        // server's state has none of at S that the client took out before
        // the restore (a trafo type set the server's ledger has no bodies
        // for): no plan item, no revive - the anchor's record of them is the
        // world this resync replaces.
        std::set<std::string> gone;
    };
    // What the restore of a resync did (the core's log line and counters).
    struct resync_outcome {
        bool blob_applied = false;        // the server's blob was restored
        bool anchor_blob = false;         // ...refused, and the anchor's own restored instead
        std::string blob_summary, blob_error;
        uint32_t rows_written = 0, rows_equal = 0;   // rows beamed / already the body's state after the blob
        uint32_t from_record = 0;         // bodies the blob did not know, from the anchor's record (keep-local, local-only)
        uint32_t destroyed = 0, destroy_failed = 0, remade = 0;
        uint32_t rechecked = 0;           // live bodies the blob did not know and no plan covers, rechecked at the restore
        uint32_t deferred = 0;            // ...or at their birth tick
    };

    // Gap L, L1 (rollback-engine gaps design 2.3.1, step EL6): a life of a
    // tracked name that the anchor's record has a body for and that ends
    // inside the window (A, now] - its end is the first record after A
    // whose generation for the name differs from A's (a record without the
    // body but with A's generation, a moved birth's gap, is no end).
    // `reborn_at`: the birth tick of the name's later life inside the
    // window (the restore plan's birth item), 0 when none.  `kind`: what
    // the name was in A's tracked set.
    enum class life_kind : uint8_t { own, mirror, mechanism };
    struct ended_life {
        std::string name;
        uint32_t generation = 0;   // the A-life's
        uint32_t end_tick = 0;     // D
        uint32_t reborn_at = 0;    // E, or 0
        life_kind kind = life_kind::mechanism;
    };
    // What a half of rollback_world::restore_lives did about one ended life:
    //   revived_until  != 0: the A-life's body exists again (made muted at
    //                  the restore); its state is the A record's, and the
    //                  replay removes it at this tick (its end);
    //   remade_at      != 0: the later life of the name was taken out at the
    //                  restore and the replay makes it again at this tick;
    //   a name can carry both; one with neither is unrevivable (counted and
    //   logged by name by the engine).
    struct life_outcome {
        uint32_t revived_until = 0;
        uint32_t remade_at = 0;
    };

    struct rollback_world {
        std::function<bool(const std::string& entity, bmmo_physics_body_state& out)> get_body;
        // Optional (step TR3): a body every world simulates from the same
        // inputs - a member's trafo piece, made, blown and reset by the
        // inputs the server's ledger fixed.  Its snapshot row is compared
        // like any other, but restores it only when it breaches its
        // tolerance: within it the body's own record (the blob) is its
        // exact state, and writing the row - a pose and velocities, none of
        // the collision state - is what made exact pieces drift from the
        // server after every snapshot rollback (TR3's H1 with snapshots off:
        // the full snapshot of each body-set change rewrote every awake
        // piece).  A breaching piece is restored from its row as before.
        std::function<bool(const std::string& entity)> row_on_breach_only;
        // Optional: called with true before a record's capture and with false
        // after it.  The capture only reads, so a backend may serve its
        // by-name lookups from one pass over the world in between
        // (rollback_binding.hpp; physics_state.hpp entity_lookup_scope_begin).
        std::function<void(bool on)> capture_scope;
        // mode: keep = leave the sleep state alone; wake/freeze as above.
        // recheck: the write moves the body into a possibly touching pose, so
        // rebuild its contact pairs (the plain beam keeps the old ones).
        std::function<bool(const std::string& entity, const bmmo_physics_body_state& state,
                           wake_mode mode, bool recheck)> set_body;
        // The recheck as a pass of its own (bridge API v16).  Optional: with
        // it, a restore writes every pose with recheck = false and then
        // rechecks every body the old path would have rechecked, once all the
        // poses are in - a body rechecked at its own write meets the partners
        // written after it still at their live poses, and keeps the pairs it
        // built against them (measured: a prop ball restored with a contact
        // at 0.0100 m to a plate its record had 0.0456 m away).
        std::function<bool(const std::string& entity)> recheck_body;
        // The core's queued impulses (bridge API v11).  Optional: a caller
        // that does not provide them gets exactly the old behaviour, which is
        // a restore that drops whatever IVP had queued for the next PSI.  With
        // them, a replay restore puts the queue back - worth 1.6-3.6 cm per
        // rewind on a body in contact, see bmmo_physics_core_extra.
        //
        // set_body_extra runs AFTER set_body for the same entity, because
        // set_body zeroes the queue by design 8.4 and nothing in it touches the
        // queue after that point.
        std::function<bool(const std::string& entity, bmmo_physics_core_extra& out)> get_body_extra;
        std::function<bool(const std::string& entity, const bmmo_physics_core_extra& state)> set_body_extra;
        // The environment's clock (bmmo_physics_clock_state).  Optional.  A
        // restore puts it back BEFORE the bodies, because the beam stamps each
        // core with the environment's time_of_last_psi; without it every
        // replayed tick runs one tick later in IVP time than the tick it
        // replaces, which the freeze logic and the event queue both read.
        std::function<bool(bmmo_physics_clock_state& out)> get_clock;
        std::function<bool(const bmmo_physics_clock_state& state)> set_clock;
        // The IVP savestate (docs/ivp-savestate-design.md,
        // physics/physics_savestate.hpp): the engine's whole collision state -
        // cores, hull managers, the overlap tree, every mindist and its
        // scheduled event, the friction systems and their contact points, the
        // simulation units, the object caches and the clock - as one opaque,
        // local blob.  Optional.  get captures it with every record (true and a
        // non-empty vector; an empty vector counts as no blob).  set puts it
        // back at an anchor INSTEAD of the per-body pose / extra / contact /
        // clock writes for every body it covers (each body the restore takes
        // from our own record), before any body is written and after the
        // scripts and the navigation replicas are back; snapshot rows and
        // bodies born inside the window are still written on top with the
        // beam.  false: the blob was refused, the world may be damaged, and the
        // restore falls back to the per-body path for everything.
        // `outcome` (out): what the blob could not settle by itself - the
        // bodies whose friction contacts it could not put back (their system
        // had a body the world no longer has; the engine re-creates them from
        // its own record) and the live bodies it does not know, left out of
        // collision detection (the engine keeps a body it will re-pose at its
        // birth tick inert until then and rechecks the rest back in).
        std::function<bool(std::vector<uint8_t>& out)> get_savestate;
        std::function<bool(const std::vector<uint8_t>& blob, savestate_outcome& outcome)> set_savestate;
        // Optional (step A3): a travelling blob - another process's, the
        // resync state's server savestate - restored as set_savestate
        // restores this world's own (travelling_blob; bridge v20).
        std::function<bool(const travelling_blob& blob, savestate_outcome& outcome)> set_travelling_savestate;
        std::function<bool(const std::string& entity, bmmo_physics_nav_state& out)> get_nav;
        std::function<bool(const std::string& entity, const bmmo_physics_nav_state& state)> set_nav;
        // A body's friction contacts (bmmo_physics_contact, bridge v15).
        // Optional.  get fills the vector and returns the count (-1: unknown
        // body); set re-creates the recorded contacts the body lacks and
        // returns how many it made (-1: unknown body).
        std::function<int(const std::string& entity, std::vector<bmmo_physics_contact>& out)> get_contacts;
        std::function<int(const std::string& entity, const std::vector<bmmo_physics_contact>& in, uint32_t flags)> set_contacts;
        // explicit input for the next physics step of a navigated ball
        std::function<bool(const std::string& entity, const input_frame& frame)> nav_input;
        // polling mode on/off (the own ball reads the keyboard itself in live frames)
        std::function<bool(const std::string& entity, bool enable)> nav_poll;
        std::function<bool()> step;   // one physics tick
        // false while the local physics clock is stopped by the retail scripts
        // (Level 1 tutorial, pause menu): bodies are snapped, nothing is
        // re-simulated.  Optional; missing means "simulating".
        std::function<bool()> simulating;
        // Called before each re-simulated tick, with the tick about to be
        // stepped: the script replay runs the mechanism scripts' frame here
        // (game/script_replay.hpp), where the live frame runs them - before
        // the physics step.  Null on the server/tests.
        std::function<void(uint32_t tick)> pre_step;
        // Optional (step N02R): after each re-simulated tick's step, before
        // its record is captured - a write the live frame of `tick` made that
        // no replayed script makes again (game/session_host_support.hpp
        // manager_activations::after_replayed_step: IngameParameter[0][2],
        // the checkpoint and death chains', and [0][1], the death chain's,
        // step N01R), so the replayed record of that tick carries it as the
        // live record does.  Null: none.
        std::function<void(uint32_t tick)> after_step;
        // Optional: the navigated remote balls whose current body was born at
        // or before `tick`, each with the lifetime generation that body has
        // now.  Asked after pre_step(tick), which is where a birth deferred
        // into the replay is made.  A late remote birth is made inside the
        // replay at its stamp, but the records of the window it lands in were
        // captured while the player was dead, so their tracked sets do not
        // name it: without this the newborn rolled from its stamp to the live
        // tick on its spawn impulse alone, fed no input, and was recorded for
        // the first time at the live tick after the replay - every later
        // correction anchored inside that window parked it at that record and
        // re-posed it there.  A name missing from the record's set is added
        // to the replayed tick's set and fed its input before the step, and
        // the re-captured record carries it with this generation.  `tick`
        // must not precede the first tick this world stepped that body: a
        // body the history has born later in the window is parked until then
        // and is skipped whatever the caller says.
        std::function<void(uint32_t tick, std::map<std::string, uint32_t>& out)> replay_remotes;
        // Optional: the mechanism bodies the world has now - the very list
        // the caller's live records track as `mechanisms`.  Asked after each
        // replayed step.  The replay re-records every tick with the live
        // record's tracked set, but the replayed world can have a mechanism
        // the live frames did not have yet (a sector activation the live
        // frames cancelled or made later, re-issued here at its tick): left
        // out, the body's first record is the first live tick after the
        // replay, and a later rewind anchored between the replayed birth and
        // that tick destroys it as a body born inside its window while the
        // restored scripts keep the joints they made on it - IVP frees the
        // joints with the body and the blocks' handles dangle (the teardown
        // crash of e8ab2_swing_n1/capture3).  Named here, it is recorded from
        // the replayed tick that has it, with the generation it has now.
        std::function<std::vector<std::string>()> replay_mechanisms;
        // The mechanism scripts' runtime state as bytes (game/script_replay.hpp):
        // captured with every record, put back at the anchor so the replayed
        // frames start the scripts where the live frames had them.  Optional;
        // without them the replay is physics only, as it was.
        std::function<bool(std::vector<uint8_t>& out)> capture_scripts;
        // `anchor` is the tick the bytes were captured at: the adapter keeps
        // live every script the game reset after it (script_replay::restore).
        std::function<bool(const std::vector<uint8_t>& bytes, uint32_t anchor)> restore_scripts;
        // Gap Q4 (rollback-engine gaps design 2.2.4, step B7EG; filled by
        // bind_script_replay): the PreSimulate entries of every script the
        // replay does not execute held out of the queue - called once a
        // replay is known to follow, before anything is restored; returns
        // the entries held, -1 when nothing was held - and put back in their
        // places after the replay (on every exit of the restore).
        std::function<int(uint32_t anchor, bool have_record)> hold_outside_callbacks;
        std::function<void()> unhold_outside_callbacks;
        // Optional: names how two script snapshots differ (the adapter has
        // the graph).  Returns "" when they are equal in every logical field
        // - a float parameter that follows the physics by an ulp is reported
        // in the text but is not a logical difference.  The engine counts a
        // mismatch only when this says the difference is logical (or when it
        // is absent and the bytes differ).
        std::function<std::string(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool& logical)> diff_scripts;
        std::function<void(const std::string&)> log;
        // Optional: every decision the engine takes about a snapshot, so the
        // session journal can record it without parsing the log lines.
        std::function<void(const rollback_correction&)> on_correction;
        // Optional, diagnostic: names the first field by which two savestate
        // blobs differ (bmmo::physics::diff_savestate).  With it installed,
        // a replay whose last re-simulated tick does not reproduce the live
        // record's blob byte for byte is logged with that field - the
        // in-session form of the SimTool's replay identity.
        std::function<std::string(const std::vector<uint8_t>& live, const std::vector<uint8_t>& replayed)> diff_savestate;
        // Optional, as a pair: a body born inside the window is re-created
        // inside the replay the way the live frame created it, instead of
        // being parked at its birth pose and re-posed after its birth tick.
        // destroy_body removes the body at the restore (before the savestate
        // is applied, so the blob meets exactly the object set it recorded);
        // false means the caller cannot re-create it (no recipe recorded) and
        // it is parked as before.  recreate_body makes it again at the start
        // of its birth tick, from the caller's own record of that birth (the
        // Physicalize recipe, group and matrix), before the tick's scripts and
        // step - the same insertion into IVP's tree, units and caches the live
        // Physicalize made.  Without the pair, or when the savestate is not
        // applied, the parking path runs.
        // destroy_body gets the body's birth tick too (the caller puts the
        // entity back at the matrix its Physicalize found).  Without
        // recreate_body the body is expected to be made again by the replayed
        // scripts themselves at its birth tick (plan doc 10: the sector
        // activation re-issued, the module scripts restored and re-run); a
        // body still missing after that tick's step is counted (births_missing).
        std::function<bool(const std::string& entity, uint32_t birth_tick)> destroy_body;
        std::function<bool(const std::string& entity, uint32_t birth_tick)> recreate_body;
        // Optional, with destroy_body (rollback-engine gaps design 2.4, step
        // B8EG item EGB): a body born inside the window that keeps the
        // parking path by design - a ball (the own ball's birth the scripts
        // made, whose replica's controllers are on it) - is not passed to
        // destroy_body and is counted births_parked_balls, so that
        // birth_recreate_failures counts real failures only.  False for a
        // body destroy_body takes out (a moved own birth, a mirror the core
        // makes again).  The world is the same either way: destroy_body
        // refuses such a ball without touching it.
        std::function<bool(const std::string& entity, uint32_t birth_tick)> keeps_parked;
        // Optional (step B1, resync design 2.8): a body destroy_body took out
        // that the replay is not to make at its live birth tick - the replay
        // re-derives the sector union and moved or forgot the pass that made
        // it live.  Missing after that tick's step it is counted as moved
        // (births_moved), not missing; made later by the moved pass, it is
        // recorded from then on (replay_mechanisms).
        std::function<bool(const std::string& entity, uint32_t birth_tick)> birth_moved;
        // Optional, with destroy_body (step LB of the session-core design): a
        // body destroy_body took out that the caller makes again at its
        // birth tick from the session's inputs - a mirror, made from its
        // Physicalize event and driven with the frames the history has for
        // it now.  After its birth tick's step it is left as the step made
        // it: its birth record was a replay's or a live frame's, made with
        // the inputs known then, so a difference to it is what the history
        // has learnt since, not a fidelity gap to correct.  Its navigation
        // is new (made with the body, before the tick's step), so the
        // tick's frame is fed again after the caller's births.
        std::function<bool(const std::string& entity)> birth_from_inputs;
        // Optional, the other end of a lifetime (step P of the session-core
        // design): a mechanism the anchor's record has and the world no
        // longer has - destroyed inside the window by a script the replay
        // re-runs (Ball_ResetPieces_<type>'s De Physicalize of the trafo
        // explosion pieces) - is made again at the restore, before the
        // scripts' state goes back (a force block restored then needs its
        // body) and before the savestate, which then puts its whole state
        // back from the blob; the replayed script takes it away again at the
        // tick the live one did.  `anchor` is the restore's tick: the caller
        // makes the lifetime alive then (its latest birth up to that tick),
        // muted.  false: not made (no birth known).  The caller counts and
        // logs what it made.
        // Since step EL6 (gap L, L1) the engine never calls this itself: it
        // is the host's primitive, which bind_session_world composes into
        // the mechanism half of restore_lives below (since step EL9, L4: on
        // both paths, and for a name with a later life born in the window
        // after that life is destroyed).
        std::function<bool(const std::string& entity, uint32_t anchor)> revive_body;
        // Optional (gap L, L4; rollback-engine gaps design 2.3.5, step EL9):
        // the mechanisms revive_body may make again - a name the host's
        // selection can kill: bind_session_world sets the host's part (the
        // registry of every trafo set's pieces) and bind_script_replay adds
        // every body of a selected module.  The engine never calls it.
        std::function<bool(const std::string& entity)> revivable;
        // Optional (gap L, L1; rollback-engine gaps design 2.3.1, step EL6):
        // the lives that end inside the window, found by the engine from the
        // records' generations, handed to the world before the scripts'
        // state and the savestate go back, only when a replay follows (on
        // the savestate and the per-body path alike).  The halves -
        // bind_session_world: the core's (the balls, L2/L3), then the host's
        // (the mechanisms, L4) - make what they can revive muted and answer
        // per name in `out` (life_outcome).  `savestate` says whether the
        // blob will be tried (step P's revive was savestate-only until G9;
        // since step EL9 the mechanism half revives on both paths).
        // The engine plans a revived name from its A record and a remade one
        // from its birth record (the two-half plan item); a revived body
        // stays when the blob is refused, and the per-body path writes it.
        // Since step EL7 it is called at every restore a replay follows,
        // `ended` possibly empty (a life born and ended inside the window
        // is known only to the core's life log), and a name answered
        // remade_at a stamp before its birth item's tick moves the item
        // there (a body made after its stamp, replayed at the stamp).
        std::function<void(uint32_t anchor, uint32_t now, const std::vector<ended_life>& ended, bool savestate,
                           std::map<std::string, life_outcome>& out)> restore_lives;
        // Optional, the default way for a body born inside the window when the
        // caller has its Physicalize matrix: the parked body is put where and
        // how its Physicalize made it at the START of its birth tick, before
        // that tick's scripts and step, with the collision recheck - it
        // enters the overlap tree at the moment the live creation did and is
        // stepped with the others, as it was live.  After the step it is
        // expected exactly on its birth record; a difference is counted and
        // corrected as the parked path corrected it.  The body, its joints
        // and its force controllers all stay what they are (unlike
        // destroy_body / recreate_body, which sever what the live scripts
        // attached to the body).
        std::function<bool(const std::string& entity, uint32_t birth_tick)> insert_birth;
        // Optional: the entities born at `tick`, in the order the live frame
        // created them.  IVP's overlap tree takes its layout from the order
        // elements are inserted, so the births of one tick are inserted,
        // re-created and re-posed in this order rather than the plan's
        // alphabetical one (measured: three sector bodies inserted
        // alphabetically gave the tree two extra nodes for good, and the
        // plate row downstream drifted by ulps after every rewind through it).
        // Names not listed come after, in the plan's order.
        std::function<std::vector<std::string>(uint32_t tick)> birth_order;
        // Optional: the tick the live frame created `entity` at, 0 when the
        // caller does not know.  For a body the blob does not know and the
        // records do not track (a fixed part of a module, born inside the
        // window): instead of being rechecked back into the overlap tree at
        // the restore - at the anchor, where the live world had no such body
        // and the tree a different shape for good - it waits, out of collision
        // detection, and is rechecked in at the start of its birth tick, in
        // birth_order, where the live creation inserted it.
        std::function<uint32_t(const std::string& entity)> birth_tick_of;
        // Optional (step RO1): such an untracked body born inside the window
        // at `birth_tick` (birth_order) is instead destroyed at the restore,
        // before the savestate (the blob then meets the object set it
        // recorded), and made again by the replayed scripts at its birth
        // tick - when this says the replay will make it: a FIXED body of a
        // module whose scripts the replay runs and re-activates, with no
        // lifetime at `anchor` (bind_session_world and bind_script_replay
        // compose the guard).  Only when the restore replays the scripts and
        // takes the savestate path; the body goes through destroy_body.  Any
        // other untracked window birth keeps the deferral above (measured:
        // the deferral made the overlap tree identical, but kept the module's
        // fixed parts as the anchor's live bodies - "objects 44 46" at the
        // restore - where the replayed scripts make them anew).
        std::function<bool(const std::string& entity, uint32_t birth_tick, uint32_t anchor)> remade_by_replay;
        // Optional, with remade_by_replay: after the step of its birth tick,
        // whether such a body is back (made again by the replayed scripts).
        // The caller counts (no engine counter: the engine's layout is the
        // legacy retail state's until R7).
        std::function<void(const std::string& entity, uint32_t birth_tick, bool back)> fixed_birth_remade;
        // Step RR of the session-core design: a snapshot row the engine wrote
        // at tick t is the server's state of that body after t's step, and
        // every later replay that crosses t writes it again after stepping t
        // (and re-records t from it), so a rewind anchored before t does not
        // undo the correction.  The rows are kept with the record of t (its
        // `rows`), for as long as the history keeps that record.  A flag, not
        // an engine member: the legacy retail path holds the engine in its
        // own state, whose layout stays as it is until R7, and gets nothing
        // new (session-core design 3.2); the shared bind_session_world sets
        // it for both hosts' cores.  Off, rows are written at their anchor
        // only, as before.
        bool replay_rows = false;
        // Optional, with replay_rows: the local entity a row of the server's
        // body (kind, owner) belongs to at the replayed `tick`, "" when none.
        // A row is kept under the entity it was written on, but a lifecycle
        // event that arrives after the snapshot can give that server body
        // another local body inside the window (a late remote birth: the row
        // went onto the mirror of the player's previous life, which the
        // rewind for the birth destroys, and the new mirror is born before
        // the row's tick).  Without this the replay writes every other row of
        // the tick and leaves that body on its prediction - a world half the
        // server's and half ours, measured worse than writing none in a
        // ball pile.  The own ball needs no hook (the replayed record names it).
        std::function<std::string(body_kind kind, uint32_t owner, uint32_t tick)> row_entity;
    };

    // What the caller tracks at a tick: the bodies (entity names) and, for
    // the navigated balls, whether they poll their keys in live frames.
    struct rollback_tracked {
        std::string own_entity;                    // empty when the own ball has no body
        bool own_polls = false;
        std::vector<std::string> remote_entities;  // navigated remote balls
        std::vector<std::string> mechanisms;
        // Lifetime generation of every tracked entity (own, remotes,
        // mechanisms): the caller bumps it when the body behind that name is
        // created or destroyed (Physicalize/Unphysicalize/trafo/respawn), so a
        // record made for an earlier body under the same name is never used to
        // restore the current one.  A name missing from the map is generation 0,
        // which is what a caller that does not track lifetimes gets.
        std::map<std::string, uint32_t> generations;
    };

    struct rollback_thresholds {
        // One pair for every tracked body.  The old 1 mm / 0.01 m/s pair was
        // tighter than the client's own moving-prediction noise (median
        // 9 mm / 0.084 m/s, P95 21 mm; build/live-rollback-retest-20260909/
        // findings/P1-evidence.md sections 4 and 8), so about half of all
        // snapshots rolled the session back on noise alone.  The pair below
        // covers the position noise at its P95: the worst measured phase is
        // 33.8 mm, inside 0.05 m.  It does NOT cover the velocity noise at
        // that percentile -- the measured ball velocity P95 is 0.8869 m/s
        // (own ball, L11_move) and 0.6364 m/s (peer ball, L8_move), both
        // above 0.5 m/s -- so velocity coverage stops in the median-to-P95
        // band and a velocity-only breach above ~0.5 m/s still rolls the
        // session back.  The pair keeps position / velocity = 0.1 s -- "an
        // error equivalent to a tenth of a second of motion" -- so a body
        // 0.5 m/s off reaches the tolerance inside 0.1 s.
        double position = 0.05;   // metres
        double velocity = 0.5;    // m/s
        // A shared mechanism is a script-driven body: its pose is no cleaner
        // than a ball's, so it gets the same pair.  Legal mechanism motion
        // measures <= 0.81 m/tick (Level 8 windows,
        // build/online-symptoms-20260909/mech-jitter), so 0.05 m is about 6%
        // of one such tick: a genuine divergence is still caught inside a
        // tick, while sub-tick pose noise (float velocity mirrored into a
        // double pose, one substep of phase) no longer rolls the session back.
        double mechanism_position = 0.05;   // metres
        double mechanism_velocity = 0.5;    // m/s
        // The pairs above are sized for a prediction a tenth of a second old.
        // A body the client predicted WITHOUT the input that drove it - every
        // remote ball, every mechanism - is predicted blind for the whole
        // window between the snapshot's tick and the live one, and that window
        // is input_delay plus a trip: 20 ticks (0.3 s) at a 300 ms round trip,
        // three times the horizon the pair was chosen for.  Judging such a body
        // by the 0.1 s pair asks for accuracy the client has no information
        // for, and every trip costs a full re-simulation of the window.
        //
        // Measured over a 4-player session at a 300 ms round trip
        // (maps/arena-test, --latency-ms 150): 242 of 417 mismatches were
        // remote balls whose position agreed to within 5 cm and which breached
        // on velocity alone, and they carried 58% of all the re-simulation
        // work.  That is the component that grows with the player count, so it
        // is the one a 30-player room cannot afford.
        //
        // So the pair is widened by how old the prediction is, measured in
        // those same tenths of a second (7 ticks at 66 Hz): never below 1, so a
        // short window keeps exactly today's behaviour, and never above the
        // cap, so a pathological window cannot switch correction off.  The own
        // ball is never widened - its prediction used the player's own exact
        // inputs, so an error there is a real divergence rather than news that
        // has not arrived yet.
        uint32_t blind_reference_ticks = 7;   // the pair's own horizon, in ticks
        double blind_scale_max = 4.0;
        // The server's base lead is the input delay (at most 40 ticks,
        // timeline.hpp kInputDelayMaxTicks) plus the round-trip allowance plus
        // a 2-tick margin, so the client's lag can exceed the old 40-tick resim
        // window.  48 covers the clamped worst case (~40 ticks, see
        // late_tick_base in BallanceMMOServer/server.cpp); the history holds
        // two windows so the anchor tick of a full resim is still recorded
        // when its snapshot arrives.
        size_t history_ticks = 96;
        uint32_t max_resim_ticks = 48;   // give up (hard set only) beyond this lag
    };

    // Late-player plan step 12 (late-player design 2.5, the per-client
    // reach): a session client's max_resim_ticks is a runtime value,
    //     clamp(p99 of its relay lag over the last 4 windows + 8, 48, 120),
    // and its history becomes 128 once that exceeds 48 (the deepest resync
    // re-step, history - 1, follows it to 127).  The relay lag is what the
    // client measures (its lead over the server's newest step plus the
    // downlink: session_client_core::note_held_section), not the lead
    // offset; the deeper re-simulation is that player's CPU alone.  The
    // floor is the fixed reach every side assumed before (lead.hpp's
    // kLeadSectorReachTicks, the server's Sector bound, stays at it: a
    // client's reach is never below it), the cap the deepest reach the sent
    // ring resolves (input_relay.hpp kHeldMaxReachTicks).
    //
    // Step R12 (late-player design 2.5 "As built (step R12)", the user's two
    // decisions of 2026-10-27) changes the margin and the cap:
    //     reach = clamp(p99 + 8 + w, 48, cap)
    // w the server's wait in the newest relay header (protocol 13,
    // lateness.hpp server_wait_meter: a notice of a substituted tick is
    // h + w + 1 deep, h the header lag sampled here), and cap this client's
    // own rewind cost's (rewind_cost_meter below), never above 120.
    constexpr uint32_t kReachFloorTicks = 48;
    constexpr uint32_t kReachCapTicks = 120;
    constexpr uint32_t kReachMarginTicks = 8;
    constexpr size_t kReachHistoryTicks = 128;
    static_assert(kReachCapTicks + 1 <= kReachHistoryTicks, "the deepest reach's anchor must stay in the history");

    // Step R12, the cap (the user's decision of 2026-10-27: "estimate the
    // best value at run time"; the hitch budget B: "3 帧，约 45 ms" - three
    // ticks): the deepest rewind this client can do inside B,
    //     cap = clamp(floor((B - restore_ms) / replay_ms_per_tick), 48, 120),
    // from moving averages of its own rewinds (rollback_engine's timer: the
    // restore stage and the replay stage of every rewind that replayed a
    // tick - input, own, lifecycle, snapshot, forced, a resync's re-step).
    // The replay is weighted by ticks (decayed sums of ms and of ticks, so a
    // 2-tick rewind does not count as much as a 48-tick one); the weight of
    // the newest sample is 1/16.  Before the first sample the cap is the
    // floor: no measurement, no deeper claim than every client made before
    // step 12 (and a client that never rewinds needs no deeper reach).
    constexpr double kReachHitchBudgetMs = 3.0 * 1000.0 / 66.0;   // B: 3 ticks, 45.45 ms
    class rewind_cost_meter {
    public:
        static constexpr double kWeight = 1.0 / 16.0;

        void clear() { *this = rewind_cost_meter{}; }

        // One rewind: its restore stage, its replay stage, the ticks it
        // replayed (a rewind that replayed none is no sample).
        void sample(double restore_ms, double replay_ms, uint32_t ticks) {
            if (ticks == 0) return;
            if (!samples_) {
                restore_ms_ = restore_ms;
                replay_ms_sum_ = replay_ms;
                replay_ticks_sum_ = static_cast<double>(ticks);
            } else {
                restore_ms_ += kWeight * (restore_ms - restore_ms_);
                replay_ms_sum_ = (1.0 - kWeight) * replay_ms_sum_ + replay_ms;
                replay_ticks_sum_ = (1.0 - kWeight) * replay_ticks_sum_ + static_cast<double>(ticks);
            }
            ++samples_;
        }

        uint64_t samples() const { return samples_; }
        double restore_ms() const { return restore_ms_; }
        double replay_ms_per_tick() const { return replay_ticks_sum_ > 0.0 ? replay_ms_sum_ / replay_ticks_sum_ : 0.0; }

        uint32_t cap() const {
            if (!samples_) return kReachFloorTicks;
            const double per_tick = replay_ms_per_tick();
            const double room = kReachHitchBudgetMs - restore_ms_;
            if (room <= 0.0) return kReachFloorTicks;
            if (per_tick <= 0.0) return kReachCapTicks;
            const double ticks = std::floor(room / per_tick);
            if (ticks >= static_cast<double>(kReachCapTicks)) return kReachCapTicks;
            if (ticks <= static_cast<double>(kReachFloorTicks)) return kReachFloorTicks;
            return static_cast<uint32_t>(ticks);
        }

    private:
        uint64_t samples_ = 0;
        double restore_ms_ = 0.0;
        double replay_ms_sum_ = 0.0;
        double replay_ticks_sum_ = 0.0;
    };

    // The relay lag samples of the last kWindows windows (a window is the
    // core's window: line, kWindowTicks of own ticks), as histograms, and
    // the reach they give.  A window's slot is reused when a newer window
    // samples into it; a window older than the newest sampled one by
    // kWindows or more no longer counts.
    class reach_meter {
    public:
        static constexpr size_t kWindows = 4;
        static constexpr uint32_t kBuckets = 129;   // 0..127 ticks, 128 = 128 or more

        void clear() { *this = reach_meter{}; }

        void sample(uint32_t window, uint32_t lag) {
            if (!any_ || static_cast<int32_t>(window - newest_) > 0) {
                newest_ = window;
                any_ = true;
            }
            auto& slot = slots_[window % kWindows];
            if (!slot.used || slot.window != window) {
                if (slot.used && static_cast<int32_t>(window - slot.window) < 0) return;   // older than the slot's: gone
                slot = window_slot{};
                slot.used = true;
                slot.window = window;
            }
            ++slot.hist[lag < kBuckets - 1 ? lag : kBuckets - 1];
            ++slot.count;
        }

        // Samples inside the last kWindows windows (the newest sampled one
        // and the kWindows - 1 before it).
        uint64_t samples() const {
            uint64_t total = 0;
            for (const auto& slot: slots_)
                if (counts(slot)) total += slot.count;
            return total;
        }

        // The 99th percentile of those samples, in ticks (0 without any).
        uint32_t p99() const {
            const uint64_t total = samples();
            if (!total) return 0;
            uint64_t seen = 0;
            for (uint32_t b = 0; b < kBuckets; ++b) {
                for (const auto& slot: slots_)
                    if (counts(slot)) seen += slot.hist[b];
                if (seen * 100 >= total * 99) return b;
            }
            return kBuckets - 1;
        }

        // The reach those samples ask for (step R12: p99 + 8 + the server's
        // wait, at most `cap`, never below the floor): the floor without any.
        uint32_t reach(uint32_t wait = 0, uint32_t cap = kReachCapTicks) const {
            if (!samples()) return kReachFloorTicks;
            const uint32_t top = std::clamp(cap, kReachFloorTicks, kReachCapTicks);
            return std::clamp(p99() + kReachMarginTicks + wait, kReachFloorTicks, top);
        }

    private:
        struct window_slot {
            bool used = false;
            uint32_t window = 0;
            uint64_t count = 0;
            std::array<uint32_t, kBuckets> hist{};
        };
        bool counts(const window_slot& slot) const {
            return slot.used && any_ && static_cast<int32_t>(newest_ - slot.window) >= 0
                && newest_ - slot.window < kWindows;
        }
        std::array<window_slot, kWindows> slots_{};
        uint32_t newest_ = 0;
        bool any_ = false;
    };

    struct rollback_stats {
        uint64_t snapshots = 0, matched = 0, mismatched = 0, rollbacks = 0, resim_ticks = 0;
        uint64_t rewinds = 0;          // re-anchors the caller asked for (rewind_to), not snapshot disagreements
        uint64_t impulses_restored = 0;  // body writes that put a recorded queued impulse back (v11)
        uint64_t calm_restored = 0;      // body writes that put the recorded calm references back (v13)
        uint64_t units_restored = 0;     // body writes that put the recorded unit countdown back (v13)
        uint64_t contacts_restored = 0;         // friction contacts re-created at an anchor (v15)
        uint64_t contact_restore_failures = 0;  // set_contacts calls that failed outright
        // The script replay (game/script_replay.hpp): restores of the scripts'
        // state at an anchor, and replays after which the re-executed scripts'
        // state differed from the live record of the same tick - zero on a
        // solo probe means the snapshot carries everything the scripts keep.
        uint64_t script_restores = 0;
        uint64_t script_restore_failures = 0;
        uint64_t script_mismatches = 0;        // logical: flags, IOs, controllers, counters
        uint64_t savestate_replay_mismatches = 0;   // the last replayed tick's blob != the live record's (diagnostic; counted while logged, the first 8)
        uint64_t unknown_rechecks_deferred = 0;     // untracked window-born bodies rechecked into the tree at their birth tick, not at the restore
        uint64_t births_inserted = 0;               // window-born bodies put where their Physicalize made them before their birth tick's step
        uint64_t births_inserted_exact = 0;         // ...and found at exactly their birth record after that step
        uint64_t birth_insert_mismatches = 0;       // ...or not: re-posed from the record as a parked body would be
        uint64_t births_recreated = 0;              // window-born bodies destroyed at a restore and made again at their birth tick
        uint64_t births_recreated_exact = 0;        // ...and found at exactly their birth record after that tick's step
        uint64_t birth_recreate_mismatches = 0;     // ...or not: re-posed from the record as a parked body would be
        uint64_t birth_recreate_failures = 0;       // destroy or recreate refused: the body was parked / is missing
        uint64_t births_parked_balls = 0;           // birth items of balls kept on the parking path by design (keeps_parked)
        uint64_t births_missing = 0;                // destroyed at a restore and not made again by its birth tick's replayed scripts
        uint64_t births_moved = 0;                  // ...because the replay's union re-derivation moved or forgot their pass (step B1)
        uint64_t replay_mechanisms_added = 0;       // replayed ticks' records that name a mechanism body the live record did not (replay_mechanisms)
        // Gap L, L1 (step EL6; only where the world has restore_lives): the
        // lives alive at an anchor that ended inside its window, those a half
        // revived (by A's kind), those whose later life a half took out for
        // the replay to make again, and those revived by no half - absent
        // from the replay between the anchor and their end (logged by name).
        uint64_t lives_ended = 0;
        uint64_t lives_revived_own = 0, lives_revived_mirror = 0, lives_revived_mech = 0;
        uint64_t lives_remade = 0;
        uint64_t lives_unrevivable = 0;
        uint64_t script_float_mismatches = 0;  // float parameters only (sensors, timers)
        uint64_t clock_rewinds = 0;      // restores that put the environment's clock back first
        // The IVP savestate (rollback_world::set_savestate): restores that put
        // the engine's whole collision state back at the anchor in place of the
        // per-body writes (each also counts as a clock rewind: the blob carries
        // the clock), restores whose blob the world refused (each fell back to
        // the per-body path), and the size of the last blob applied.
        uint64_t savestates_restored = 0;
        uint64_t savestate_failures = 0;
        uint64_t savestate_bytes = 0;
        // Records whose blob the world could not capture (get_savestate said
        // no): those ticks fall back to the per-body path when rewound to.
        uint64_t savestate_capture_failures = 0;
        // Wall time the rollback paths took, in microseconds: the restore stage
        // of every rewind (the plan's writes, the blob, the scripts, the
        // destroyed births), the replay stage (the re-simulated steps and the
        // births' bookkeeping), the longest single rewind, and the per-tick
        // capture of the record (its blob and scripts) with the longest one.
        // What a client's frame budget pays for the rollback - a frame is 15 ms.
        uint64_t restore_us = 0, replay_us = 0, rewind_us_max = 0, capture_us = 0, capture_us_max = 0;
        uint64_t unmatched = 0;        // snapshot tick not in the history
        uint64_t captures_never_made = 0;   // a deferred record (capture_now false) no amend_record captured
        uint64_t too_far = 0;          // a snapshot or a rewind deeper than max_resim_ticks: refused, nothing written
        uint64_t resyncs = 0;          // resync re-steps (resync(): the server's state of S restored, S+1..now replayed)
        uint64_t frozen = 0;           // local physics clock stopped: bodies snapped, no re-simulation
        double last_error = 0.0, max_error = 0.0;
        std::string last_mismatch;     // entity of the last mismatch
    };

    class rollback_engine {
    public:
        // One snapshot row that maps to a local entity, and what the compare
        // pass concluded about it.
        struct row_ref {
            const body_state* row = nullptr;
            bool simulated = false;      // BODY_FLAG_SIMULATED
            bool breached = false;       // past the tolerance of its kind
            bool authoritative = false;  // the row is the restore source for this body
        };

        explicit rollback_engine(rollback_thresholds thresholds = {}) : thresholds_(thresholds) {}

        // Drops every record.  Lifecycle events no longer need this - a body
        // created or destroyed mid-window is handled by its generation and by
        // the birth record (see on_snapshot) - but a re-anchoring caller
        // (session assignment, resync, a full snapshot overwriting the world)
        // still has to say that the recorded ticks no longer describe
        // anything.  Keeps session diagnostics across the boundary.
        void invalidate_history() {
            history_.clear();
            moved_births_.clear();
            awaiting_capture_ = false;
        }

        void clear() {
            invalidate_history();
            stats_ = {};
            detailed_logs_ = 0;
            resim_traces_ = 0;
        }
        // Per-step traces of the re-simulation (diagnostics).
        void set_verbose(bool verbose) { verbose_ = verbose; }
        const rollback_stats& stats() const { return stats_; }
        size_t history_size() const { return history_.size(); }

        // After the physics step of `tick`: remember every tracked body and
        // navigation state, plus the inputs that produced this tick (own and
        // remote), for a later re-simulation.  A body appearing in or leaving
        // the tracked set is NOT a reason to drop the history: the restore
        // rules below judge every name on its own (row, record of the same
        // lifetime, birth record, or nothing at all), so a window may span
        // several live sets.
        //
        // `capture_now` false (step T3's fix-up): the tick's world is captured
        // by the amend_record the caller makes for this tick before anything
        // reads the record or steps the world - the session core's live
        // frame, which always amends its record after the frame's corrections
        // and before the next tick, and whose amend overwrote this capture
        // whole.  Until then the record has the tick, the tracked set and the
        // inputs, and no world: a record of a later tick, or a resync, while
        // one is still waiting is a caller's bug, counted and logged
        // (stats().captures_never_made).
        void record(const rollback_world& world, uint32_t tick, const rollback_tracked& tracked,
                    const std::map<std::string, input_frame>& inputs_applied, bool capture_now = true) {
            if (awaiting_capture_) {
                ++stats_.captures_never_made;
                if (world.log && never_made_logs_++ < 4)
                    world.log("record of tick " + std::to_string(awaiting_tick_) + " was never captured (no amend before tick "
                              + std::to_string(tick) + ")");
                awaiting_capture_ = false;
            }
            tick_state state;
            state.tick = tick;
            state.tracked = tracked;
            state.inputs = inputs_applied;
            if (!capture_now) {
                awaiting_capture_ = true;
                awaiting_tick_ = tick;
            } else {
                const auto started = std::chrono::steady_clock::now();
                capture(world, state);
                const uint64_t took = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
                stats_.capture_us += took;
                stats_.capture_us_max = std::max(stats_.capture_us_max, took);
            }
            if (!history_.empty() && history_.back().tick >= tick) {
                // re-recording (resim) or a numbering restart: drop from here
                while (!history_.empty() && history_.back().tick >= tick) history_.pop_back();
            }
            history_.push_back(std::move(state));
            while (history_.size() > thresholds_.history_ticks) history_.pop_front();
        }

        // Test support (SimTool --savestate-check with bodies born and
        // destroyed inside its window, trafo-replication design 7.2 S6):
        // every record after `tick` dropped, so the newest record is the
        // round's live tick, as a session's always is - the lifetime
        // generations a restore compares against are the newest record's.
        // No host calls it.  Returns the records dropped.
        size_t forget_after(uint32_t tick) {
            size_t n = 0;
            while (!history_.empty() && static_cast<int32_t>(history_.back().tick - tick) > 0) {
                history_.pop_back();
                ++n;
            }
            return n;
        }

        // The corrections a live frame applies after its physics step (snapshot
        // rollback, blend, mechanism) change the state the next step starts
        // from, so the record for that tick must reflect them: a later rollback
        // restores this record and re-simulates from it exactly as the live
        // frame did.  The entry was captured before those corrections, and the
        // re-simulation writes its result back into the same entry, so an
        // unamended record re-bakes the loss of its own correction into the
        // history.  Keeps the tick's inputs and tracked set.  Returns false
        // when the tick is no longer recorded (evicted, invalidated or never
        // recorded), so the caller must not treat the amendment as applied.
        bool amend_record(const rollback_world& world, uint32_t tick) {
            tick_state* at = find(tick);
            if (!at) return false;
            if (awaiting_capture_ && awaiting_tick_ == tick) {
                // The record's one capture (record's capture_now false),
                // timed as the record's.
                awaiting_capture_ = false;
                const auto started = std::chrono::steady_clock::now();
                capture(world, *at);
                const uint64_t took = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
                stats_.capture_us += took;
                stats_.capture_us_max = std::max(stats_.capture_us_max, took);
                return true;
            }
            capture(world, *at);
            return true;
        }

        // Authoritative snapshot of `tick`.  `entity_of` maps a snapshot body
        // to the local entity name (empty = not tracked here); `input_at`
        // supplies the input a navigated ball had at a tick during the
        // re-simulation. Relayed remote inputs supersede recorded predictions;
        // the own ball keeps the input recorded for that tick's entity.
        // Returns true when a rollback happened.
        bool on_snapshot(const rollback_world& world, const session_snapshot_msg& snapshot, uint32_t current_tick,
                         const std::function<std::string(const body_state&)>& entity_of,
                         const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at) {
            ++stats_.snapshots;
            rollback_correction correction;
            correction.tick = snapshot.tick;
            correction.local_tick = current_tick;
            auto report = [&](uint8_t kind) {
                if (!world.on_correction) return;
                correction.kind = kind;
                world.on_correction(correction);
            };
            tick_state* at = find(snapshot.tick);
            if (!at) {
                ++stats_.unmatched;
                report(7);
                return false;
            }
            // The lifetime a name carries right now is the newest one the
            // caller has told us about (a name that left the tracked set keeps
            // the generation it had, so its records stay usable while the body
            // itself is still there).  A record of an older generation belongs
            // to a body that no longer exists and may not be restored onto the
            // current one.
            const auto generation_now = [&](const std::string& entity) -> uint32_t {
                for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
                    auto found = it->tracked.generations.find(entity);
                    if (found != it->tracked.generations.end()) return found->second;
                }
                return 0;
            };
            // The record of T that may be used for `entity`: same name and
            // same lifetime.  Null means "no opinion of our own about this
            // body at T", which is not by itself a disagreement.
            const auto record_at = [&](const std::string& entity) -> const bmmo_physics_body_state* {
                auto it = at->bodies.find(entity);
                if (it == at->bodies.end()) return nullptr;
                if (generation_in(*at, entity) != generation_now(entity)) return nullptr;
                return &it->second;
            };

            // How deep the window is.  Needed before the compare pass, which
            // judges a blind-predicted body by a tolerance widened for it, and
            // again below to decide whether the window may be re-simulated.
            const uint32_t window = current_tick > snapshot.tick ? current_tick - snapshot.tick : 0;
            const double blind_window_scale = blind_scale(window);

            // 1. compare every row we can compare
            bool mismatch = false;
            double worst = 0.0;        // largest position error: the log lines and the stats
            double worst_breach = 0.0; // furthest past a threshold: the journal's subject
            std::string worst_entity, detail;
            std::map<std::string, row_ref> rows;
            for (const auto& body: snapshot.bodies) {
                const std::string entity = entity_of(body);
                if (entity.empty()) continue;
                row_ref ref;
                ref.row = &body;
                ref.simulated = (body.flags & BODY_FLAG_SIMULATED) != 0;
                const bmmo_physics_body_state* recorded = record_at(entity);
                if (!recorded) {
                    // No record of this lifetime at T: nothing to disagree
                    // with.  The row is still the best pose we have for the
                    // body, so it is a restore source if the body exists now
                    // (unless a birth record supersedes it, below).
                    ref.authoritative = true;
                    rows[entity] = ref;
                    continue;
                }
                // Each row is judged by the tolerance of its own kind - a
                // mechanism row at the same error as a ball is not a
                // divergence - widened by how long we have been predicting it
                // blind.  The own ball is the one body whose prediction used
                // the exact inputs that drove it, so it keeps the unwidened
                // pair (rollback_thresholds, blind_reference_ticks).
                const bool blind = entity != at->tracked.own_entity;
                const tolerance tol = tolerance_for(body.kind, blind ? blind_window_scale : 1.0);
                double dp = 0.0, dv = 0.0;
                for (int k = 0; k < 3; ++k) {
                    dp += (body.position[k] - recorded->position[k]) * (body.position[k] - recorded->position[k]);
                    dv += (static_cast<double>(body.linear[k]) - recorded->linear[k])
                        * (static_cast<double>(body.linear[k]) - recorded->linear[k]);
                }
                dp = std::sqrt(dp);
                dv = std::sqrt(dv);
                if (dp > worst) {
                    worst = dp;
                    worst_entity = entity;
                }
                // The mismatch can be a velocity one (for a ball, 0.5 m/s
                // trips before 5 cm of drift does), so the record names the
                // body that is furthest past its own threshold, not the one
                // that moved most: otherwise the journal blames an
                // in-tolerance body and reports dv = 0 for exactly the event
                // it was written for.
                const double breach = std::max(tol.position > 0.0 ? dp / tol.position : dp,
                                               tol.velocity > 0.0 ? dv / tol.velocity : dv);
                if (breach > worst_breach) {
                    worst_breach = breach;
                    correction.entity = entity;
                    correction.error_m = dp;
                    correction.velocity_error = dv;
                    for (int k = 0; k < 3; ++k) {
                        correction.local_position[k] = recorded->position[k];
                        correction.server_position[k] = body.position[k];
                    }
                }
                ref.breached = dp > tol.position || dv > tol.velocity;
                // A sleeping row inside the tolerance is no news at all: the
                // body is where we have it and the server says it stopped
                // there.  Copying that row would put the server's sleep flag
                // on our body - which re-arms the freeze timer of a mechanism
                // on every snapshot - so the row is not a restore source and
                // the body rewinds with the group from its own record instead.
                // Only a breaching sleeping row restores and freezes.
                // An input-driven body (row_on_breach_only, a trafo piece)
                // is restored from its row only on a breach, awake or not.
                ref.authoritative = ref.breached
                                 || (ref.simulated && !(world.row_on_breach_only && world.row_on_breach_only(entity)));
                if (ref.breached) {
                    mismatch = true;
                    if (detailed_logs_ < 40) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), " %s dp=%.4f dv=%.3f local=(%.3f,%.3f,%.3f)%s server=(%.3f,%.3f,%.3f)%s",
                                      entity.c_str(), dp, dv, recorded->position[0], recorded->position[1],
                                      recorded->position[2], recorded->simulated ? "" : "z", body.position[0],
                                      body.position[1], body.position[2], ref.simulated ? "" : "z");
                        detail += buf;
                    }
                }
                rows[entity] = ref;
            }
            stats_.last_error = worst;
            if (worst > stats_.max_error) stats_.max_error = worst;
            if (!mismatch) {
                ++stats_.matched;
                return false;
            }
            ++stats_.mismatched;
            stats_.last_mismatch = worst_entity;
            report(0);
            // Deeper than the engine re-simulates: refused, nothing written.
            // The branch this replaces wrote every tracked body back to the
            // rows without a replay and dropped the history, which put the
            // world 49-96 ticks into the past with nothing to carry it forward
            // and left the next history's worth of relayed frames with nothing
            // to replay from (measured: a lifecycle-forced full snapshot
            // retransmitted past the reach under 10 % loss, then 95 ticks of
            // frames "too late" per remote and a 20 m excursion).  The relayed
            // inputs and events correct the prediction on their own now, and a
            // snapshot this late is also the stale one.  (A resync applies its
            // full snapshot outside this engine and is unaffected.)
            if (window > thresholds_.max_resim_ticks) {
                ++stats_.too_far;
                if (world.log && too_far_logs_++ < 8)
                    world.log("snapshot of tick " + std::to_string(snapshot.tick) + " is " + std::to_string(window)
                              + " ticks deep at tick " + std::to_string(current_tick) + ": past the rollback's reach, refused");
                report(5);
                return false;
            }
            const bool frozen = world.simulating && !world.simulating();
            if (!detail.empty() && world.log && !frozen && detailed_logs_++ < 40)
                world.log("mismatch at tick " + std::to_string(snapshot.tick) + " (local " + std::to_string(current_tick) + "):"
                          + detail);

            return restore_and_replay(world, snapshot.tick, current_tick, rows, input_at, report,
                                      worst_entity + " off by " + std::to_string(worst) + " m");
        }

    private:
        // Put every recorded body back to `anchor` and re-simulate
        // anchor+1 .. current_tick from the recorded inputs.  Steps 2 and 3 of
        // on_snapshot, which is the only caller that has `rows` to restore
        // from; rewind_to() calls it with none.  Returns true when the caller
        // must treat the history as re-anchored (it always does today: either
        // the window was replayed, or it was dropped and a resync is due).
        bool restore_and_replay(const rollback_world& world, uint32_t anchor, uint32_t current_tick,
                                const std::map<std::string, row_ref>& rows,
                                const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at,
                                const std::function<void(uint8_t)>& report, const std::string& reason,
                                const resync_spec* rs = nullptr, resync_outcome* ro = nullptr, uint32_t deep = 0) {
            tick_state* at = find(anchor);
            if (!at) return false;
            // A resync re-steps as deep as the history (resync()), and so does
            // a correction inside its re-step (rewind_to's `reach`, passed as
            // `deep`); every other restore stops at max_resim_ticks.  Never
            // false past this point: the anchor record changed below (the
            // server's scripts and navigation) is that of a re-step that runs.
            const uint32_t reach = rs ? resync_reach() : deep ? deep : thresholds_.max_resim_ticks;
            // A resync: the server's scripts replace the anchor's before the
            // restore (resync design 4.5 "anchor scripts"), and the server's
            // navigation state of each ball replaces the anchor's.
            if (rs && !rs->scripts.empty()) {
                at->scripts = rs->scripts;
                at->have_scripts = true;
            }
            if (rs)
                for (const auto& [entity, nav]: rs->navs) at->navs[entity] = nav;
            // Times the two stages for the stats: everything up to the replay
            // loop is the restore, the rest is the replay.
            // Step R12: and feeds the cap's meter, the stages in ms and the
            // ticks this rewind replayed.  The cap decides max_resim_ticks,
            // so the meter's times are the flow's wall-clock reads
            // (cost_clock_, clock_site rewind_*: the headless client
            // captures them and a capture replay takes the captured ones);
            // the stats keep their own reads (a replay's t_rest / t_rep are
            // the replay's own cost).
            // Step B5RX (R12D, the user's option (b) of B5RC's stop): the
            // meter's span ends at the last replayed tick's capture
            // (cost_ends), before the end-of-replay savestate check whose
            // text diff only the headless client runs (7-10 ms a call), so
            // a headless client measures the cap a retail client would.  The
            // same three reads in the same order: a capture replays as
            // before.  A rewind that never reaches its last tick (frozen,
            // too far, a failed step) still reads rewind_end at its end.
            struct rewind_timer {
                using time_point = std::chrono::steady_clock::time_point;
                rollback_stats& stats;
                rewind_cost_meter& cost;
                const std::function<time_point(clock_site)>& clock;
                uint64_t resim_before = stats.resim_ticks;
                time_point started = std::chrono::steady_clock::now(), split{};
                time_point cost_started = read(clock_site::rewind_start), cost_split{}, cost_ended{};
                bool have_split = false, have_cost_end = false;
                time_point read(clock_site site) const { return clock ? clock(site) : std::chrono::steady_clock::now(); }
                void replay_begins() {
                    split = std::chrono::steady_clock::now();
                    cost_split = read(clock_site::rewind_replay);
                    have_split = true;
                }
                void cost_ends() {
                    if (!have_split || have_cost_end) return;
                    cost_ended = read(clock_site::rewind_end);
                    have_cost_end = true;
                }
                ~rewind_timer() {
                    const auto ended = std::chrono::steady_clock::now();
                    const auto us = [](time_point a, time_point b) {
                        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
                    };
                    stats.restore_us += us(started, have_split ? split : ended);
                    if (have_split) stats.replay_us += us(split, ended);
                    stats.rewind_us_max = std::max(stats.rewind_us_max, us(started, ended));
                    if (have_split) {
                        cost_ends();
                        cost.sample(static_cast<double>(us(cost_started, cost_split)) / 1000.0,
                                    static_cast<double>(us(cost_split, cost_ended)) / 1000.0,
                                    static_cast<uint32_t>(stats.resim_ticks - resim_before));
                    }
                }
            } timer{stats_, rewind_cost_, cost_clock_};
            const bool frozen = world.simulating && !world.simulating();
            const auto generation_now = [&](const std::string& entity) -> uint32_t {
                for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
                    auto found = it->tracked.generations.find(entity);
                    if (found != it->tracked.generations.end()) return found->second;
                }
                return 0;
            };
            const auto record_at = [&](const std::string& entity) -> const bmmo_physics_body_state* {
                auto it = at->bodies.find(entity);
                if (it == at->bodies.end()) return nullptr;
                if (generation_in(*at, entity) != generation_now(entity)) return nullptr;
                return &it->second;
            };
            // 2. restore tick T.  The world is one interacting system: a body
            //    left where it is while the others rewind meets the replayed
            //    ball at the wrong pose and pushes it out (the rope-and-sack
            //    case, where restoring only the breaching half tore the pair
            //    apart).  So every body we have an opinion about goes back to
            //    tick T together - from its row when the snapshot carries one,
            //    otherwise from its own record - and a body we have no opinion
            //    about is left alone rather than guessed at.
            const rollback_tracked tracked = at->tracked;
            const auto extra_at = [&](const std::string& entity) -> const bmmo_physics_core_extra* {
                const auto it = at->extras.find(entity);
                return it == at->extras.end() ? nullptr : &it->second;
            };
            std::map<std::string, restore> plan;
            std::vector<std::string> rechecks;   // bodies rechecked after every pose is in (v16)
            // Untracked bodies the blob does not know, rechecked into the tree
            // at their birth tick instead of now (rollback_world::birth_tick_of).
            std::map<uint32_t, std::vector<std::string>> deferred_rechecks;
            // Step RO1: untracked fixed births destroyed at the restore for
            // the replayed scripts to make again (remade_by_replay), by birth tick.
            std::map<uint32_t, std::vector<std::string>> fixed_remakes;
            for (const auto& entity: restore_candidates(anchor, rows)) {
                if (rs && rs->gone.count(entity)) continue;   // step T3: the reconcile took it out
                const auto row = rows.find(entity);
                const bmmo_physics_body_state* recorded = record_at(entity);
                // A resync (resync design 4.1): a body the reconcile destroys
                // comes back only by the replay, at its birth tick; a row is
                // authoritative whatever the records say - it outranks a
                // birth, the server says the body exists at S.
                if (rs) {
                    if (const auto d = rs->destroy.find(entity); d != rs->destroy.end()) {
                        restore item;
                        if (recorded) item.state = *recorded;
                        item.repose_tick = d->second;
                        item.rebirth = true;
                        item.force_recheck = true;
                        plan[entity] = item;
                        continue;
                    }
                    if (row != rows.end() && row->second.authoritative) {
                        restore item;
                        item.state = state_of(*row->second.row);
                        item.mode = row->second.simulated ? wake_mode::wake : wake_mode::freeze;
                        item.from_row = true;
                        plan[entity] = item;
                        continue;
                    }
                }
                // A body born after T outranks its row: the row describes the
                // body this one replaced (or one the server does not have yet).
                const tick_state* birth = recorded ? nullptr
                                                   : birth_record(entity, anchor, generation_now(entity));
                // A lifetime with a gap the server put in it (move_birth): the
                // body made again at the applied tick is born there, even when
                // the same lifetime has records before the gap (a beam keeps
                // its generation) that make the oldest record useless.
                if (!recorded && !birth) birth = moved_birth_record(entity, anchor, generation_now(entity));
                restore item;
                if (birth) {
                    // It did not exist at T: it was created during the window
                    // we are about to replay.  Nothing can put it back where it
                    // was at T, so it is parked at the pose it was born with,
                    // kept there through T+1..birth-1 (below) and released into
                    // the replay at its birth tick.
                    item.state = birth->bodies.find(entity)->second;
                    // Parked, and re-posed at its birth tick, in the sleep
                    // state its birth record has.  It used to be parked and
                    // re-posed AWAKE whatever the record said, and most sector
                    // bodies are born frozen (start_frozen, or calm from their
                    // first check): a see-saw or a bridge parked awake tilts
                    // under gravity during the replayed window and is then
                    // released awake - measured as the 0.3-0.6 m a rewind
                    // across a sector's births cost (scripts/sleep_divergence.py:
                    // the control had them asleep, the arm awake, from the
                    // first checkpoint after the rewind).  The birth record's
                    // freeze bookkeeping goes with the re-pose, so the first
                    // movement check after it sees the history the live run
                    // had.
                    item.mode = sleep_as_recorded_
                        ? (item.state.simulated ? wake_mode::wake : wake_mode::freeze) : wake_mode::wake;
                    item.repose_tick = birth->tick;
                    item.force_recheck = true;
                    if (const auto ex = birth->extras.find(entity); ex != birth->extras.end()) {
                        item.extra = ex->second;
                        item.have_extra = true;
                    }
                } else if (row != rows.end() && row->second.authoritative) {
                    item.state = state_of(*row->second.row);
                    item.mode = row->second.simulated ? wake_mode::wake : wake_mode::freeze;
                    item.from_row = true;
                } else if (recorded) {
                    // Our own record of T, with the sleep state we had THEN -
                    // the record's, not the live one.  The sleep state used to
                    // be kept as it is now, on the reasoning that the snapshot
                    // said nothing about it; but a body that fell asleep after
                    // T then replayed T+1.. asleep while the live frames ran it
                    // awake (and the other way round), and the replay diverged
                    // from the live run on exactly the bodies whose sleep
                    // decision fell inside the window.
                    item.state = *recorded;
                    item.mode = sleep_as_recorded_
                        ? (recorded->simulated ? wake_mode::wake : wake_mode::freeze) : wake_mode::keep;
                    // ...and with the impulses IVP had queued at T.  Only on
                    // this branch: a row is an authoritative pose from the
                    // server, which carries no queue, and the queue we happen
                    // to hold locally does not belong to the state being
                    // written (the zeroing is right for that case - it IS the
                    // hard set design 8.4 is about).  A body born inside the
                    // window is parked rather than restored, so its queue is
                    // not meaningful either.
                    if (const bmmo_physics_core_extra* extra = extra_at(entity)) {
                        item.extra = *extra;
                        item.have_extra = true;
                    }
                } else {
                    continue;   // no row, no record: no opinion
                }
                plan[entity] = item;
            }
            // Gap L, L1: a birth item's birth half is what it was planned
            // with (a revive below gives the item its A-life half).
            for (auto& [entity, item]: plan)
                if (item.repose_tick) item.birth = {item.state, item.mode, item.extra, item.have_extra};
            // The clock first: the beam below stamps every restored core with
            // the environment's time_of_last_psi, so the environment has to be
            // at T before any body is.  Only when a replay will follow - a
            // frozen clock or a too-far window sets the bodies and drops the
            // history, and a clock left behind with nothing to advance it
            // would stay behind.
            const uint32_t window = current_tick > anchor ? current_tick - anchor : 0;
            // Whether a replay follows this restore (the clock, the scripts and
            // the savestate are put back only then).
            const bool replay_follows = !frozen && window <= reach;
            // Gap Q4: no entry of a script the replay does not run takes
            // part in it; back in place on every exit (after the replay
            // loop, after a failed step).
            struct outside_callbacks {
                const rollback_world& world;
                bool held = false;
                ~outside_callbacks() {
                    if (held) world.unhold_outside_callbacks();
                }
            } outside{world};
            if (replay_follows && world.hold_outside_callbacks && world.unhold_outside_callbacks)
                outside.held = world.hold_outside_callbacks(anchor, at->have_scripts) >= 0;
            // The savestate path (rollback_world::set_savestate): decided here,
            // before the clock, because the blob carries the clock and the
            // separate clock write is skipped when the blob is applied.
            // A resync restores the server's blob (a travelling one) in place
            // of the anchor's own.
            const bool travelling = rs && rs->blob.blob && !rs->blob.blob->empty() && world.set_travelling_savestate;
            const bool try_savestate = exact_restore_ && replay_follows
                                    && (travelling || (at->have_savestate && world.set_savestate));
            // The clock offset the restored calm references have to carry
            // when the clock itself stays live (the rewind is opt-in: IVP
            // keeps other time-anchored state a rewound clock turns into
            // negative deltas).  The references are absolute IVP times; with
            // the live clock `offset` seconds ahead of the anchor's, a
            // reference shifted by the same amount is as old relative to
            // every replayed step as it was in the live run.
            double calm_shift = 0.0;
            {
                bmmo_physics_clock_state now_clock{};
                if (at->have_clock && world.get_clock && world.get_clock(now_clock))
                    calm_shift = now_clock.current_time - at->clock.current_time;
            }
            // Skipped when the savestate will be tried (its blob carries the
            // clock); a refused blob runs it below, after the scripts but still
            // before any body.
            const auto rewind_clock = [&]() {
                if (!restore_clock_ || !at->have_clock || !world.set_clock) return;
                world.set_clock(at->clock);
                ++stats_.clock_rewinds;
                calm_shift = 0.0;
            };
            if (replay_follows && !try_savestate) rewind_clock();
            // Gap L, L1 (rollback-engine gaps design 2.3.1, step EL6): the
            // lives alive at T that end inside the window, handed to the
            // world's halves (rollback_world::restore_lives) - step P's
            // revive of the mechanisms the window destroyed is the host's
            // half now - before the scripts' state (a force block's
            // controller on a revived body) and the blob, which then find
            // them; then each answer goes into its plan item.
            if (replay_follows && world.restore_lives)
                revive_ended_lives(world, *at, anchor, current_tick, try_savestate, rs, plan);
            if (trace_plan_ && world.log && plan_traces_ < 16) {
                ++plan_traces_;
                int rows_used = 0, records = 0, births = 0, revived = 0;
                std::string born, alive;
                for (const auto& [entity, item]: plan) {
                    if (item.revived_until && ++revived <= 12)
                        alive += " " + entity + "<" + std::to_string(item.revived_until);
                    if (item.repose_tick) {
                        ++births;
                        if (births <= 12) born += " " + entity + "@" + std::to_string(item.repose_tick);
                    } else if (item.from_row) {
                        ++rows_used;
                    } else {
                        ++records;
                    }
                }
                world.log("restore plan at tick " + std::to_string(anchor) + " (now " + std::to_string(current_tick)
                          + ", " + reason + "): " + std::to_string(records) + " from our records, "
                          + std::to_string(rows_used) + " from rows, " + std::to_string(births)
                          + " born in the window" + (born.empty() ? "" : ":" + born)
                          + (revived ? ", " + std::to_string(revived) + " revived:" + alive : std::string()));
            }
            // The mechanism scripts' state at T, before the bodies: a force
            // block restored here re-creates or deletes its IVP controller,
            // and the replayed frames then toggle it from the right phase.
            // Only when a replay will follow - a frozen clock or a too-far
            // window sets the bodies and drops the history.
            const bool scripts_replayed = replay_scripts_ && at->have_scripts && world.restore_scripts && !frozen
                                        && window <= reach;
            // The live record of `current_tick`, kept aside before the replay
            // overwrites it: what the re-executed scripts must arrive at.
            std::vector<uint8_t> live_scripts, live_savestate;
            bool have_live_scripts = false, have_live_savestate = false;
            if (const tick_state* live = find(current_tick);
                    live && live->have_savestate && world.diff_savestate && world.log && savestate_replay_logs_ < 8) {
                live_savestate = live->savestate;
                have_live_savestate = true;
            }
            // Step RO1: whether the scripts really came back to T (a failed
            // restore leaves some at their live state, past the Physicalize
            // that made a window-born fixed body: it must not be destroyed).
            bool scripts_restored = false;
            if (scripts_replayed) {
                if (const tick_state* live = find(current_tick); live && live->have_scripts) {
                    live_scripts = live->scripts;
                    have_live_scripts = true;
                }
                if (world.restore_scripts(at->scripts, anchor)) {
                    ++stats_.script_restores;
                    scripts_restored = true;
                } else {
                    ++stats_.script_restore_failures;
                }
            }
            // The IVP savestate: the whole engine back to T in one write, after
            // the scripts (a force block restored above re-creates or deletes
            // its IVP controller) and before any body.  The navigation replicas
            // go first: set_nav re-creates the navigation force controllers,
            // and the savestate puts every core's and every unit's controller
            // list back in its recorded order, so they have to be there to be
            // ordered.  Every body the restore takes from our own record is
            // then covered - its pose, integrator state, freeze bookkeeping,
            // contacts and collision state all came back with the blob, and a
            // beam would only zero what the blob restored.  Snapshot rows and
            // bodies born inside the window are written on top as before.  A
            // refused blob falls back to the per-body path for everything (the
            // world may be damaged by then; that path is the best effort left)
            // and is dropped: it would only be refused again.
            bool savestate_applied = false;
            if (try_savestate) {
                // Bodies born inside the window go first, when the caller can
                // make them again at their birth tick: destroyed here, the
                // blob meets the object set it recorded and nothing is parked.
                if (world.destroy_body) {
                    for (auto& [entity, item]: plan) {
                        // A later life a half of restore_lives took out
                        // already (remade_at): recreate set there.
                        if (!item.repose_tick || item.recreate) continue;
                        const bool parked_ball = world.keeps_parked && world.keeps_parked(entity, item.repose_tick);
                        if (!parked_ball && world.destroy_body(entity, item.repose_tick)) {
                            item.recreate = true;
                            item.from_inputs = world.birth_from_inputs && world.birth_from_inputs(entity);
                            ++stats_.births_recreated;
                            if (item.rebirth && ro) ++ro->destroyed;
                        } else {
                            if (parked_ball) ++stats_.births_parked_balls;
                            else ++stats_.birth_recreate_failures;
                            if (item.rebirth) {
                                // Not destroyed: it stays, from the anchor's
                                // record (counted; the reconcile could not
                                // undo it).
                                item.rebirth = false;
                                item.repose_tick = 0;
                                if (ro) ++ro->destroy_failed;
                            }
                        }
                    }
                }
                // Step RO1: the window's untracked FIXED births the replayed
                // scripts make again (remade_by_replay), newest birth first,
                // one per name: destroyed here too, not deferred below.  Only
                // when the script restore succeeded (a failed one leaves the
                // making script past its Physicalize: nothing would make the
                // body again), and never under a travelling blob (a resync:
                // the server's blob at S may know a fixed body this world made
                // after S, and a destroyed one would make it refuse the blob -
                // the deferral below keeps A3's exact resync there).
                if (scripts_restored && !travelling && world.remade_by_replay && world.birth_order && world.destroy_body && world.get_body) {
                    std::set<std::string> seen;
                    for (uint32_t t = current_tick; static_cast<int32_t>(t - anchor) > 0; --t) {
                        for (const std::string& name: world.birth_order(t)) {
                            if (!seen.insert(name).second || plan.find(name) != plan.end()) continue;
                            bmmo_physics_body_state live{};
                            if (!world.get_body(name, live) || !world.remade_by_replay(name, t, anchor)) continue;
                            if (world.destroy_body(name, t)) fixed_remakes[t].push_back(name);
                        }
                    }
                }
                for (const auto& [entity, nav]: at->navs) world.set_nav(entity, nav);
                savestate_outcome outcome;
                bool blob_ok = false;
                if (travelling) {
                    blob_ok = world.set_travelling_savestate(rs->blob, outcome);
                    if (ro) {
                        ro->blob_applied = blob_ok;
                        ro->blob_summary = outcome.summary;
                    }
                    // Refused: the anchor's own blob, as before A3 (the rows
                    // then carry the server's bodies; the IVP state under them
                    // is the anchor's).
                    if (!blob_ok && at->have_savestate && world.set_savestate) {
                        if (world.log) world.log("resync: the server's savestate was refused (" + outcome.summary
                                                 + "): the anchor's own restored instead");
                        outcome = {};
                        blob_ok = world.set_savestate(at->savestate, outcome);
                        if (ro) ro->anchor_blob = blob_ok;
                    }
                } else {
                    blob_ok = world.set_savestate(at->savestate, outcome);
                }
                if (blob_ok) {
                    savestate_applied = true;
                    ++stats_.savestates_restored;
                    stats_.savestate_bytes = travelling && ro && ro->blob_applied ? rs->blob.blob->size() : at->savestate.size();
                    // The blob carries the clock: no separate write, but the
                    // clock did go back.
                    ++stats_.clock_rewinds;
                    calm_shift = 0.0;
                    if (!rs) {
                        // A revived life (L1) is covered at A like any body
                        // from our record, its later birth or not.
                        for (auto& [entity, item]: plan)
                            if (!item.from_row && (!item.repose_tick || item.revived_until)) item.covered = true;
                    } else {
                        // A resync: the blob put back every body it knows (the
                        // server's state at S); a row the body has now bit for
                        // bit is not written (a beam loses what no row
                        // carries: A2's rule).  A body it does not know - a
                        // keep-local or local-only body - comes from the
                        // anchor's record and is rechecked into the tree.
                        const std::set<std::string> unknown(outcome.unknown_bodies.begin(), outcome.unknown_bodies.end());
                        for (auto& [entity, item]: plan) {
                            if (item.repose_tick && !item.revived_until) continue;
                            if (unknown.count(entity)) {
                                item.force_recheck = true;
                                if (!item.from_row && ro) ++ro->from_record;
                                continue;
                            }
                            if (!item.from_row) {
                                item.covered = true;
                                continue;
                            }
                            bmmo_physics_body_state live{};
                            if (world.get_body(entity, live) && same_body_state(live, item.state)) {
                                item.covered = true;
                                if (ro) ++ro->rows_equal;
                            }
                        }
                    }
                    // A covered body whose contacts the blob could not carry
                    // (its friction system had a body the world lost) gets
                    // them from our own record below, like an uncovered one.
                    for (const std::string& name: outcome.contacts_dropped)
                        if (auto it = plan.find(name); it != plan.end()) it->second.recontact = true;
                    // A live body the blob does not know is out of collision
                    // detection now.  One born inside the window has a plan
                    // item that parks it and re-poses it at its birth tick
                    // with a recheck: it stays inert until then, as the live
                    // run had no such body before that tick.  One with no
                    // record at all (physicalized in this very frame) would
                    // stay out for good: rechecked back in at once.
                    for (const std::string& name: outcome.unknown_bodies) {
                        if (plan.find(name) != plan.end() || !world.recheck_body) continue;
                        const uint32_t born = world.birth_tick_of ? world.birth_tick_of(name) : 0;
                        if (born > anchor && born <= current_tick) {
                            deferred_rechecks[born].push_back(name);   // in at its birth tick (below)
                            ++stats_.unknown_rechecks_deferred;
                            if (ro) ++ro->deferred;
                        } else {
                            world.recheck_body(name);
                            if (ro) ++ro->rechecked;
                        }
                    }
                } else {
                    ++stats_.savestate_failures;
                    // Gap L, L1 (2.3.1 step 7): the bodies restore_lives
                    // revived stay; their plan items (the A records) are not
                    // covered, so the per-body path below writes them.
                    if (world.log && stats_.savestate_failures % 8 == 1)
                        world.log("savestate: the blob of tick " + std::to_string(anchor) + " ("
                                  + std::to_string(at->savestate.size()) + " bytes) was refused, per-body restore instead ("
                                  + std::to_string(stats_.savestate_failures) + " so far)");
                    at->have_savestate = false;
                    at->savestate.clear();
                    rewind_clock();
                }
            }
            // What a record's extra may write back, by switch and by sleep
            // state: a body being frozen gets no queue (a frozen body's queue
            // is meaningless), the unit countdown is written in a pass of its
            // own below, after every body is back and IVP has regrouped them.
            const auto extra_mask = [&](const bmmo_physics_core_extra& extra, wake_mode mode, bool unit_pass) -> uint32_t {
                uint32_t fields = extra.fields;
                if (!restore_impulses_ || mode == wake_mode::freeze) fields &= ~BMMO_PHYSICS_EXTRA_IMPULSES;
                if (!restore_quaternions_) fields &= ~BMMO_PHYSICS_EXTRA_QUATS;
                if (!restore_calm_) fields &= ~BMMO_PHYSICS_EXTRA_CALM;
                if (!restore_unit_) fields &= ~BMMO_PHYSICS_EXTRA_UNIT;
                if (unit_pass) fields &= BMMO_PHYSICS_EXTRA_UNIT;
                else fields &= ~BMMO_PHYSICS_EXTRA_UNIT;
                return fields;
            };
            // The extra of the item's A half (state / mode / extra) or of its
            // birth half (`birth`: the re-pose after its birth tick's step).
            const auto write_extra = [&](const std::string& entity, const restore& item, bool unit_pass, bool birth_half = false) {
                const bmmo_physics_core_extra& source = birth_half ? item.birth.extra : item.extra;
                const wake_mode mode = birth_half ? item.birth.mode : item.mode;
                if (!(birth_half ? item.birth.have_extra : item.have_extra) || !world.set_body_extra) return;
                bmmo_physics_core_extra extra = source;
                extra.fields = extra_mask(source, mode, unit_pass);
                if (extra.fields == 0) return;
                if (extra.fields & BMMO_PHYSICS_EXTRA_CALM)
                    for (int k = 0; k < 2; ++k) extra.calm_time[k] += calm_shift;
                if (world.set_body_extra(entity, extra)) {
                    if (extra.fields & BMMO_PHYSICS_EXTRA_IMPULSES) ++stats_.impulses_restored;
                    if (extra.fields & BMMO_PHYSICS_EXTRA_CALM) ++stats_.calm_restored;
                    if (extra.fields & BMMO_PHYSICS_EXTRA_UNIT) ++stats_.units_restored;
                }
            };
            bool rows_written = false;   // a snapshot row went into the world at T (below)
            for (auto& [entity, item]: plan) {
                // Put back whole by the savestate: no beam (it zeroes the
                // integrator state the blob restored), no recheck, no queue.
                if (item.covered) continue;
                bmmo_physics_body_state live{};
                // A body destroyed after its last record simply has nothing to
                // restore: skip it and correct the rest of the world.  (The
                // engine cannot recreate it, and the caller's next record will
                // stop carrying it.)
                if (!world.get_body(entity, live)) continue;
                // A write that moves a body normally asks IVP to recheck its
                // contacts, because the optimized beam generates no collision
                // when a body lands inside another one.  But the recheck
                // rebuilds the body's mindist/hull bookkeeping, which is state
                // the rollback does not record - so it is also a candidate
                // CAUSE of a replay's infidelity, and this flag is how that is
                // measured rather than argued about.
                // A body born inside the window stays inert while it is parked
                // on the savestate path: the restore left it out of collision
                // detection, the live run had no such body before its birth
                // tick, and the re-pose after that tick rechecks it in.
                // (Measured without this: a box born inside a 48-tick window
                // left its spot 5 m up in the first live tick after the
                // replay.)
                // (A revived life with a later birth is the A-life here: written.)
                const bool parked_inert = savestate_applied && item.repose_tick != 0 && !item.revived_until;
                const bool recheck = recheck_on_restore_ && !parked_inert
                                   && (item.force_recheck || write_moves_body(live, item.state));
                const bool deferred = recheck && two_pass_recheck_ && world.recheck_body;
                world.set_body(entity, item.state, item.mode, recheck && !deferred);
                if (deferred) rechecks.push_back(entity);
                if (item.from_row) rows_written = true;
                if (item.from_row && ro) ++ro->rows_written;
                // After the pose write, which zeroed the queue.  A body born in
                // the window is parked here and gets its record's extra at the
                // re-pose instead; a revived life is written from its A record.
                if (!item.repose_tick || item.revived_until) write_extra(entity, item, false);
                // What we wrote from a row is the authoritative state of T now:
                // without this the next snapshot of T compares against the
                // prediction we just discarded and rolls back again.
                if (!item.from_row) continue;
                auto existing = at->bodies.find(entity);
                if (existing != at->bodies.end()) existing->second = item.state;
                // ...and stays so in every later replay that crosses T (step
                // RR): kept with T's record, written again after T's step.
                // (Not a resync's: S is the history's oldest tick, and its
                // record is captured anew below.)
                if (world.replay_rows && !rs) {
                    applied_row kept{};
                    kept.state = item.state;
                    kept.mode = item.mode;
                    // The lifetime the body has now: the one it had at T, since
                    // a body born after T takes the birth branch above and is
                    // never written from a row.
                    kept.generation = generation_now(entity);
                    if (const auto row = rows.find(entity); row != rows.end() && row->second.row) {
                        kept.kind = row->second.row->kind;
                        kept.owner = row->second.row->owner;
                        kept.identified = true;
                    }
                    kept.own = !tracked.own_entity.empty() && entity == tracked.own_entity;
                    // The newer row wins also when an earlier snapshot of T put
                    // the same server body's row on another local entity (a
                    // late birth gave the player a new mirror in between): one
                    // row per server body, or a replay could write the older
                    // one through row_entity and skip this one.
                    for (auto other = at->rows.begin(); other != at->rows.end();) {
                        const bool same = other->first != entity
                                       && ((kept.own && other->second.own)
                                           || (kept.identified && other->second.identified && !other->second.own
                                               && other->second.kind == kept.kind && other->second.owner == kept.owner));
                        other = same ? at->rows.erase(other) : std::next(other);
                    }
                    at->rows[entity] = std::move(kept);
                }
            }
            // The rechecks, once every pose is in (see rollback_world::recheck_body).
            for (const auto& entity: rechecks) world.recheck_body(entity);
            // The unit countdowns last: a unit is several cores, grouped by
            // IVP after the rechecks above, so the write lands on the units as
            // they are now (last writer wins within a unit).
            for (auto& [entity, item]: plan)
                if ((!item.repose_tick || item.revived_until) && !item.covered) write_extra(entity, item, true);
            // The friction contacts the anchor's record had and the live world
            // lacks - a contact that ended inside the window.  IVP creates a
            // contact only on an impact, and a body put back on the floor it
            // has since rolled off has none coming: it rests on nothing until
            // it leaves (measured: the own ball 0.76 m low after a rewind
            // whose anchor still had the floor, exact when the contact lived
            // on).  After every body is back, so the partners stand where they
            // did; a body born in the window has no record of its own here.
            if (restore_contacts_ && world.set_contacts)
                for (auto& [entity, item]: plan) {
                    if ((item.repose_tick && !item.revived_until) || (item.covered && !item.recontact)) continue;
                    const auto it = at->contacts.find(entity);
                    if (it == at->contacts.end()) continue;
                    const int made = world.set_contacts(entity, it->second, contact_flags_);
                    if (made < 0) ++stats_.contact_restore_failures;
                    else stats_.contacts_restored += static_cast<uint64_t>(made);
                    if (item.from_row && world.replay_rows) {
                        if (const auto kept = at->rows.find(entity); kept != at->rows.end()) {
                            kept->second.contacts = it->second;
                            kept->second.have_contacts = true;
                        }
                    }
                }
            // The navigation replicas: here on the per-body path (a refused
            // savestate included, whose early write may not have survived); the
            // savestate path wrote them before the blob.
            if (!savestate_applied)
                for (const auto& [entity, nav]: at->navs) world.set_nav(entity, nav);
            // Snapshot rows changed the world at T, so the anchor's blob must
            // say so: a later rewind to T would otherwise restore the
            // prediction the rows replaced.  Written on top of the savestate,
            // the corrected world is captured again (with any body born in the
            // window parked in it, which that rewind parks again anyway); on the
            // per-body path the blob no longer describes the anchor and is
            // dropped.
            if (rs) {
                // A resync: the anchor's record becomes the server's state at
                // S as this world now holds it - the mechanisms it has (but
                // the ones parked or destroyed for a birth in the window), the
                // blob, the scripts, every body's state - so a later rewind to
                // S restores that, not the anchor world the resync replaced.
                if (world.replay_mechanisms) {
                    std::vector<std::string> mechanisms;
                    for (std::string& name: world.replay_mechanisms()) {
                        if (const auto p = plan.find(name); p != plan.end() && p->second.repose_tick != 0
                                && !p->second.revived_until)
                            continue;
                        if (!at->tracked.generations.count(name)) at->tracked.generations[name] = generation_now(name);
                        mechanisms.push_back(std::move(name));
                    }
                    at->tracked.mechanisms = std::move(mechanisms);
                }
                auto& remotes = at->tracked.remote_entities;
                remotes.erase(std::remove_if(remotes.begin(), remotes.end(), [&](const std::string& name) {
                    const auto p = plan.find(name);
                    return p != plan.end() && p->second.rebirth;
                }), remotes.end());
                at->rows.clear();
                capture(world, *at);
            } else if (rows_written && at->have_savestate) {
                at->savestate.clear();
                at->have_savestate = savestate_applied && world.get_savestate && world.get_savestate(at->savestate)
                                   && !at->savestate.empty();
                if (!at->have_savestate) at->savestate.clear();
            }

            // 3. re-simulate T+1 .. current with the recorded inputs
            timer.replay_begins();
            const uint32_t lag = current_tick > anchor ? current_tick - anchor : 0;
            if (frozen) {
                // the scripts stopped the local clock: the authoritative
                // states stand until the clock runs again.  No re-simulation
                // runs, so no record can carry this tick, and the caller's tick
                // counter keeps running: truncating would keep T but drop
                // T+1..current, and every later snapshot of that range would be
                // unmatched.  Drop the history; the caller sees frozen grow and
                // must re-anchor (resync) before recording again.
                ++stats_.frozen;
                report(6);
                invalidate_history();
                return true;
            }
            ++stats_.rollbacks;
            if (lag > reach) {
                // Unreachable since both callers refuse a window this deep
                // before restoring (on_snapshot, rewind_to); kept as the guard
                // it was, with its old behaviour.
                ++stats_.too_far;
                if (world.log) world.log("rollback: lag " + std::to_string(lag) + " ticks, bodies set without re-simulation");
                report(5);
                // No re-simulation runs, so no record can carry this tick and
                // the caller's tick counter keeps running: keeping the records
                // at or before T would leave a hole T+1..current that makes
                // every later snapshot of that range unmatched and feeds later
                // inputs time-shifted.  Drop the history; the caller sees
                // too_far grow and must re-anchor (resync) before recording
                // again.
                invalidate_history();
                return true;
            }
            // A replayed tick must use recorded input, not the live keyboard.
            std::vector<std::string> polled;
            auto stop_polling = [&](const rollback_tracked& t) {
                if (t.own_entity.empty() || !t.own_polls) return;
                for (const auto& name: polled) if (name == t.own_entity) return;
                polled.push_back(t.own_entity);
                world.nav_poll(t.own_entity, false);
            };
            stop_polling(tracked);
            for (uint32_t t = anchor + 1; t <= current_tick; ++t)
                if (tick_state* recorded = find(t)) stop_polling(recorded->tracked);
            // rollback_world::replay_remotes, asked once per replayed tick, and
            // the last frame each remote the records do not name was fed in
            // this replay (carried into the tail the relay has not reached).
            std::map<std::string, uint32_t> alive;
            std::map<std::string, input_frame> carried;
            size_t rows_reapplied = 0, row_ticks = 0;   // step RR, for the log line
            uint32_t first_row_tick = 0;
            for (uint32_t t = anchor + 1; t <= current_tick; ++t) {
                tick_state* recorded = find(t);
                // A copy: a remote born inside the replay is added to it below
                // (rollback_world::replay_remotes), and the re-captured record
                // takes the extended set.
                rollback_tracked step_tracked = recorded ? recorded->tracked : tracked;
                std::map<std::string, input_frame> inputs;
                auto feed = [&](const std::string& entity, bool remote) {
                    input_frame frame{};
                    // A live remote step records our prediction, which may
                    // predate a key edge the server has since relayed. Reusing
                    // that prediction would reproduce the same divergence on
                    // every rollback. Own inputs are already exact and must
                    // stay attached to their historical entity across a trafo.
                    bool have = remote && input_at(entity, t, frame);
                    if (!have && recorded) {
                        auto it = recorded->inputs.find(entity);
                        if (it != recorded->inputs.end()) { frame = it->second; have = true; }
                    }
                    if (!have && !remote) have = input_at(entity, t, frame);
                    if (have) {
                        world.nav_input(entity, frame);
                        inputs[entity] = frame;
                    }
                };
                if (!step_tracked.own_entity.empty()) feed(step_tracked.own_entity, false);
                for (const auto& remote: step_tracked.remote_entities) feed(remote, true);
                std::string trace;
                if (verbose_ && world.log && resim_traces_ < 60) {
                    for (const auto& remote: step_tracked.remote_entities) {
                        bmmo_physics_body_state before{};
                        if (!world.get_body(remote, before)) continue;
                        char buf[200];
                        std::snprintf(buf, sizeof(buf), " %s before=(%.3f,%.3f,%.3f)%s v=(%.3f,%.3f,%.3f)", remote.c_str(),
                                      before.position[0], before.position[1], before.position[2], before.simulated ? "" : "z",
                                      before.linear[0], before.linear[1], before.linear[2]);
                        trace += buf;
                    }
                }
                // The bodies born at this tick, made again before its scripts
                // and step (the live Physicalize ran in this frame's script
                // pass, before its step).
                // This tick's births, in the live frame's creation order.
                std::vector<std::string> born_now;
                for (const auto& [entity, item]: plan)
                    if (item.repose_tick == t) born_now.push_back(entity);
                if (born_now.size() > 1 && world.birth_order) {
                    const std::vector<std::string> order = world.birth_order(t);
                    const auto rank = [&order](const std::string& name) {
                        const auto it = std::find(order.begin(), order.end(), name);
                        return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
                    };
                    std::stable_sort(born_now.begin(), born_now.end(),
                                     [&rank](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
                }
                for (const std::string& entity: born_now) {
                    restore& item = plan[entity];
                    if (item.recreate || !world.insert_birth || !savestate_applied) continue;
                    if (world.insert_birth(entity, t)) {
                        item.inserted = true;
                        ++stats_.births_inserted;
                    }
                }
                if (const auto deferred = deferred_rechecks.find(t); deferred != deferred_rechecks.end()) {
                    std::vector<std::string> names = deferred->second;
                    if (names.size() > 1 && world.birth_order) {
                        const std::vector<std::string> order = world.birth_order(t);
                        const auto rank = [&order](const std::string& name) {
                            const auto it = std::find(order.begin(), order.end(), name);
                            return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
                        };
                        std::stable_sort(names.begin(), names.end(),
                                         [&rank](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
                    }
                    for (const std::string& name: names) world.recheck_body(name);
                }
                for (const std::string& entity: born_now) {
                    restore& item = plan[entity];
                    if (!item.recreate || !world.recreate_body) continue;   // no hook: the replayed scripts make it
                    if (!world.recreate_body(entity, t)) {
                        ++stats_.birth_recreate_failures;
                        item.recreate = false;   // nothing to compare after the step; the re-pose below finds no body
                        if (world.log && recreate_logs_++ < 8)
                            world.log("birth: " + entity + " could not be re-created at tick " + std::to_string(t));
                    }
                }
                if (world.pre_step) world.pre_step(t);
                // The remotes alive at this tick that the record does not name
                // (a late birth made by pre_step just now, or at an earlier
                // replayed tick): driven like the others from here on, with
                // the frame the relay has for this tick or the nearest earlier
                // one.  Every remote the caller names gets the generation of
                // the body it has now, so the re-captured records are of the
                // same lifetime as the live ones that follow them.
                if (world.replay_remotes) {
                    alive.clear();
                    world.replay_remotes(t, alive);
                    auto& remotes = step_tracked.remote_entities;
                    for (const auto& [entity, generation]: alive) {
                        // Parked at this tick: the history has its birth later
                        // in this window, and until that re-pose it is a body
                        // held out of the world - neither driven nor recorded
                        // (recorded, a later rewind would restore the parked
                        // pose as an ordinary body and release it early).
                        // (Not a revived life before its end: the A-life is
                        // driven and recorded over [A, D).)
                        if (const auto parked = plan.find(entity); parked != plan.end() && parked->second.repose_tick != 0
                                && static_cast<int32_t>(parked->second.repose_tick - t) > 0
                                && !(static_cast<int32_t>(parked->second.revived_until - t) > 0))
                            continue;
                        const bool named = std::find(remotes.begin(), remotes.end(), entity) != remotes.end();
                        const auto was = step_tracked.generations.find(entity);
                        const bool same_life = was != step_tracked.generations.end() && was->second == generation;
                        step_tracked.generations[entity] = generation;
                        // Made again by pre_step at this, its birth tick, from
                        // the inputs (birth_from_inputs): its navigation is the
                        // one made with it, after the feed above.
                        const auto made = plan.find(entity);
                        const bool remade = made != plan.end() && made->second.from_inputs && made->second.repose_tick == t;
                        if (named && same_life && !remade) continue;
                        // Named, but under the lifetime that went (a same-name
                        // death and birth at one stamp), or made again just now:
                        // the feed above ran before pre_step, on the navigation
                        // the new body replaced, so it is fed again.  Not
                        // named: added.
                        if (!named) remotes.push_back(entity);
                        feed(entity, true);
                        if (const auto fed = inputs.find(entity); fed != inputs.end()) {
                            carried[entity] = fed->second;
                        } else if (const auto last = carried.find(entity); last != carried.end()) {
                            // The tail the relay has not reached, and no record
                            // of a prediction for a body the record did not
                            // have: its navigation keeps the frame it was fed
                            // last, which is what drove this step - recorded
                            // so, an arrival equal to it is no news.
                            world.nav_input(entity, last->second);
                            inputs[entity] = last->second;
                        }
                    }
                }
                if (!world.step()) break;
                ++stats_.resim_ticks;
                if (world.after_step) world.after_step(t);   // step N02R
                // Step RO1: a fixed birth destroyed at the restore is back
                // after its birth tick's step when the replayed scripts made
                // it; one that is not stays out (logged, the caller counts).
                if (const auto remade = fixed_remakes.find(t); remade != fixed_remakes.end()) {
                    for (const std::string& name: remade->second) {
                        bmmo_physics_body_state live{};
                        const bool back = world.get_body(name, live);
                        if (world.fixed_birth_remade) world.fixed_birth_remade(name, t, back);
                        if (!back && world.log && recreate_logs_++ < 8)
                            world.log("birth: the fixed body " + name + " was destroyed at the restore and is not back after its"
                                      " birth tick " + std::to_string(t));
                    }
                }
                // A body born inside the replayed window has been sitting at
                // its birth pose since the restore; the ticks up to and
                // including its birth tick may have dragged it (gravity, a
                // contact), so after that tick's step it is put back exactly
                // where its birth record has it - that record IS the state the
                // live frame left at the end of the birth tick.  From the next
                // tick on it is an ordinary replayed body.
                for (const std::string& entity: born_now) {
                    restore& item = plan[entity];
                    bmmo_physics_body_state live{};
                    if (!world.get_body(entity, live)) {
                        // Destroyed at the restore and not made again by this
                        // tick's replayed scripts: the activation the live
                        // frame had is missing from the replay.
                        if (item.recreate && world.birth_moved && world.birth_moved(entity, t)) {
                            ++stats_.births_moved;
                            if (world.log && recreate_logs_++ < 8)
                                world.log("birth: " + entity + " is not back after its birth tick " + std::to_string(t)
                                          + ": its sector pass was moved or forgotten by the replay");
                        } else if (item.recreate) {
                            ++stats_.births_missing;
                            if (world.log && recreate_logs_++ < 8)
                                world.log("birth: " + entity + " was destroyed at the restore and is not back after its birth tick "
                                          + std::to_string(t));
                        }
                        continue;
                    }
                    // A resync's destroy, made again by the replay: there is no
                    // birth record of this history to land on (its record is
                    // the anchor world's), so the step is the state.
                    if (item.rebirth && !item.from_inputs) {
                        if (ro) ++ro->remade;
                        continue;
                    }
                    // Made again from the inputs (birth_from_inputs): the step
                    // is the state, whatever the record of the earlier inputs
                    // says.
                    if (item.from_inputs) {
                        if (item.rebirth && ro) ++ro->remade;
                        if (same_body_state(live, item.birth.state)) {
                            ++stats_.births_recreated_exact;
                        } else if (world.log && recreate_logs_++ < 8) {
                            char buf[200];
                            std::snprintf(buf, sizeof(buf), " dp=(%.3g,%.3g,%.3g) dv=(%.3g,%.3g,%.3g)%s",
                                          live.position[0] - item.birth.state.position[0], live.position[1] - item.birth.state.position[1],
                                          live.position[2] - item.birth.state.position[2],
                                          static_cast<double>(live.linear[0]) - item.birth.state.linear[0],
                                          static_cast<double>(live.linear[1]) - item.birth.state.linear[1],
                                          static_cast<double>(live.linear[2]) - item.birth.state.linear[2],
                                          live.simulated != item.birth.state.simulated ? " sleep differs" : "");
                            world.log("birth: " + entity + " made again at tick " + std::to_string(t)
                                      + " from the inputs it has now, kept against its birth record:" + buf);
                        }
                        continue;
                    }
                    // A re-created body is expected exactly where its birth
                    // record has it: the record is the state the live frame
                    // left at the end of this tick, and this replay made the
                    // body the same way.  Then there is nothing to write - a
                    // beam would only zero what the step computed.  A
                    // difference is measured and corrected as before.
                    if (item.recreate || item.inserted) {
                        if (same_body_state(live, item.birth.state)) {
                            ++(item.recreate ? stats_.births_recreated_exact : stats_.births_inserted_exact);
                            continue;
                        }
                        ++(item.recreate ? stats_.birth_recreate_mismatches : stats_.birth_insert_mismatches);
                        if (world.log && recreate_logs_++ < 8) {
                            char buf[200];
                            std::snprintf(buf, sizeof(buf), " dp=(%.3g,%.3g,%.3g) dv=(%.3g,%.3g,%.3g)%s",
                                          live.position[0] - item.birth.state.position[0], live.position[1] - item.birth.state.position[1],
                                          live.position[2] - item.birth.state.position[2],
                                          static_cast<double>(live.linear[0]) - item.birth.state.linear[0],
                                          static_cast<double>(live.linear[1]) - item.birth.state.linear[1],
                                          static_cast<double>(live.linear[2]) - item.birth.state.linear[2],
                                          live.simulated != item.birth.state.simulated ? " sleep differs" : "");
                            world.log("birth: " + entity + (item.recreate ? " re-created" : " inserted") + " at tick "
                                      + std::to_string(t) + " is not at its birth record after the step:" + buf);
                        }
                    }
                    world.set_body(entity, item.birth.state, item.birth.mode, true);
                    write_extra(entity, item, false, true);
                    write_extra(entity, item, true, true);
                }
                // The snapshot rows a restore anchored at this tick wrote (step
                // RR): the server's state after this step, written again the
                // way that restore wrote them, so the record re-captured below
                // and every tick after it carry the correction.  After the
                // births' re-pose (a row describes the body after the whole
                // tick).  Only for a tick after the anchor: the anchor's own
                // rows are in its blob already (re-captured at that restore).
                if (recorded && !recorded->rows.empty()) {
                    const size_t n = reapply_rows(world, *recorded, step_tracked, plan, t);
                    if (n) {
                        rows_reapplied += n;
                        if (!row_ticks++) first_row_tick = t;
                    }
                }
                if (!trace.empty()) {
                    for (const auto& remote: step_tracked.remote_entities) {
                        bmmo_physics_body_state after{};
                        if (!world.get_body(remote, after)) continue;
                        char buf[200];
                        std::snprintf(buf, sizeof(buf), " %s after=(%.3f,%.3f,%.3f)%s v=(%.3f,%.3f,%.3f)", remote.c_str(),
                                      after.position[0], after.position[1], after.position[2], after.simulated ? "" : "z",
                                      after.linear[0], after.linear[1], after.linear[2]);
                        trace += buf;
                    }
                    ++resim_traces_;
                    world.log("resim tick " + std::to_string(t) + ":" + trace);
                }
                // The mechanism bodies the replayed world has after this step
                // that the live record's set does not name (rollback_world::
                // replay_mechanisms): recorded from here on, under the
                // lifetime the body has now, as a live record would have
                // named them.  Not a body still parked for a later birth (it
                // is held out of the world until then).  A body destroyed at
                // this restore for a birth the live frames made later
                // (recreate) that the replayed scripts have made already is
                // the replay's body from here on: recorded, and taken off its
                // birth tick, which would otherwise beam it to a birth record
                // of the other history - the records then start where the
                // body and the joints the script records hold on it start.
                if (world.replay_mechanisms) {
                    auto& mechanisms = step_tracked.mechanisms;
                    std::unordered_set<std::string_view> named(mechanisms.begin(), mechanisms.end());
                    std::vector<std::string> added;
                    for (std::string& name: world.replay_mechanisms()) {
                        if (named.count(name)) continue;
                        // (Not before a revived life's end: the body is the A-life then.)
                        if (const auto it = plan.find(name); it != plan.end() && it->second.repose_tick != 0
                                && static_cast<int32_t>(it->second.repose_tick - t) > 0
                                && !(static_cast<int32_t>(it->second.revived_until - t) > 0)) {
                            if (!it->second.recreate) continue;   // parked
                            it->second.repose_tick = t;           // made early by the replay: no birth re-pose later
                        }
                        added.push_back(std::move(name));
                    }
                    for (std::string& name: added) {
                        step_tracked.generations[name] = generation_now(name);
                        ++stats_.replay_mechanisms_added;
                        if (world.log && replay_mechanism_logs_++ < 8)
                            world.log("replay: " + name + " is recorded from tick " + std::to_string(t)
                                      + " on (the live record did not name it)");
                        mechanisms.push_back(std::move(name));
                    }
                }
                tick_state state;
                state.tick = t;
                // The set this tick was replayed with: a remote born inside the
                // window is recorded from its birth tick on, so a later rewind
                // anchored there restores it from its own record and the
                // oldest record of its lifetime (birth_record) is its birth.
                // The same for a mechanism the replay made earlier than the
                // live frames did (above).
                state.tracked = std::move(step_tracked);
                state.inputs = std::move(inputs);
                capture(world, state);
                // The last replayed tick: do the re-executed scripts stand where
                // the live frames left them?  A difference is state the
                // snapshot does not carry (solo), or a sensor that decided
                // differently on the corrected world (networked, by design).
                // The same question for the engine's whole state: does the
                // re-simulated world arrive at the blob the live frames left?
                // Not where a body was born or restored from a row inside the
                // window (the live record then describes a world this replay
                // was told to differ from).
                // Only while it can still be logged: the diff renders both
                // blobs as text, and a replay that corrects an input differs
                // from the live record by design - with every member's trafo
                // pieces in the blob that rendering was a twelfth of a 150 ms
                // client's frame time (step TR3), for lines past the cap.
                // Step B5RX (R12D): the scripts' check is made first (both
                // hosts make it, so its time stays in the rewind's cost),
                // then the rewind's cost ends, then the savestate check - the
                // headless client's text diff, out of the meter; the lines
                // are logged in their old order (savestate, then scripts).
                bool script_differs = false, script_logical = true;
                std::string script_where;
                if (t == current_tick && scripts_replayed && have_live_scripts && state.have_scripts && !rs
                        && state.scripts != live_scripts) {
                    script_differs = true;
                    if (world.diff_scripts) script_where = world.diff_scripts(live_scripts, state.scripts, script_logical);
                }
                if (t == current_tick) timer.cost_ends();
                if (t == current_tick && have_live_savestate && state.have_savestate && world.log
                        && savestate_replay_logs_ < 8 && state.savestate != live_savestate && rows.empty()) {
                    // A parked birth makes the live record describe a world
                    // this replay was told to differ from; a re-created one
                    // that landed on its record does not.
                    bool births = false;
                    for (const auto& [entity, item]: plan) births = births || (item.repose_tick != 0 && !item.recreate && !item.inserted);
                    if (!births) {
                        // An empty answer from the hook means the bytes differ
                        // only in what it was told to ignore (the order
                        // bookkeeping): not a mismatch.
                        const std::string where = world.diff_savestate(live_savestate, state.savestate);
                        if (!where.empty()) {
                            ++stats_.savestate_replay_mismatches;
                            ++savestate_replay_logs_;
                            world.log("savestate replay: the world re-simulated to tick " + std::to_string(t)
                                      + " differs from the live record: " + where);
                        }
                    }
                }
                if (script_differs) {
                    if (script_logical) ++stats_.script_mismatches; else ++stats_.script_float_mismatches;
                    if (world.log && (script_logical ? script_mismatch_logs_ : script_float_logs_)++ < script_log_cap_)
                        world.log(std::string("script replay: the scripts re-executed to tick ") + std::to_string(t)
                                  + (script_logical ? " differ from the live record: " : " differ in float parameters only: ")
                                  + (script_where.empty() ? std::to_string(state.scripts.size()) + " vs "
                                                              + std::to_string(live_scripts.size()) + " bytes"
                                                          : script_where));
                }
                if (recorded) {
                    // The rows stay with their tick for the next replay.
                    state.rows = std::move(recorded->rows);
                    *recorded = std::move(state);
                } else {
                    history_.push_back(std::move(state));
                }
            }
            for (const auto& entity: polled) world.nav_poll(entity, true);
            if (world.log)
                world.log("rollback to tick " + std::to_string(anchor) + " (" + reason + "), re-simulated "
                          + std::to_string(lag) + " ticks"
                          + (rows_reapplied ? ", " + std::to_string(rows_reapplied) + " snapshot row(s) of "
                                                  + std::to_string(row_ticks) + " tick(s) written again (first "
                                                  + std::to_string(first_row_tick) + ")"
                                            : std::string()));
            report(1);
            return true;
        }

    public:
        // The input this engine recorded as APPLIED for `entity` at `tick`.
        //
        // The caller needs it to decide whether a relayed remote input is news:
        // a remote ball is predicted with its last known frame, and the frame
        // the server actually applied arrives input_delay + a trip later.  If
        // it equals what the prediction used, the prediction was right and
        // nothing has to be replayed; if it differs, every tick from there on
        // was simulated from the wrong input and has to be re-simulated.  That
        // is the rollback trigger lockstep runs on - the authoritative snapshot
        // is only the fallback for a correction that arrives too late to
        // replay.
        // Whether `tick` is still in the history at all, i.e. whether there is
        // anything to replay from it.  Distinguishing this from "that body had
        // no input recorded at that tick" matters: the second case is news, not
        // a reason to do nothing - we simulated the tick with no input for a
        // body that in fact had one.
        bool knows_tick(uint32_t tick) const { return find_const(tick) != nullptr; }
        // The savestate blob the record of `tick` carries: what a rewind to
        // that tick restores, and after a replay the state the replayed tick
        // left (the replay re-records every tick it steps).  Null when the
        // tick is not recorded or its record has no blob.
        const std::vector<uint8_t>* recorded_savestate(uint32_t tick) const {
            const tick_state* at = find_const(tick);
            return at && at->have_savestate ? &at->savestate : nullptr;
        }
        // The mechanism scripts' state the record of `tick` carries, or null.
        const std::vector<uint8_t>* recorded_scripts(uint32_t tick) const {
            const tick_state* at = find_const(tick);
            return at && at->have_scripts ? &at->scripts : nullptr;
        }
        rollback_thresholds& thresholds() { return thresholds_; }
        // Step R12: this client's rewind cost and the cap it gives, and the
        // clock its samples read (the session core passes the platform's
        // `now`, so a capture replay measures the live cost; unset,
        // steady_clock).
        rewind_cost_meter& rewind_cost() { return rewind_cost_; }
        const rewind_cost_meter& rewind_cost() const { return rewind_cost_; }
        void set_cost_clock(std::function<std::chrono::steady_clock::time_point(clock_site)> clock) {
            cost_clock_ = std::move(clock);
        }
        // Late-player plan step 12: the per-client reach (reach_meter, or a
        // pinned value).  max_resim_ticks takes `reach`; the history grows to
        // kReachHistoryTicks once the reach exceeds the floor and stays
        // there (never shrunk under a pending resync's anchor or a rewind's;
        // the extra 32 records are memory only, every tick is captured once
        // either way).  Returns whether max_resim_ticks changed.
        bool set_reach(uint32_t reach) {
            const bool changed = thresholds_.max_resim_ticks != reach;
            thresholds_.max_resim_ticks = reach;
            if (reach > kReachFloorTicks && thresholds_.history_ticks < kReachHistoryTicks)
                thresholds_.history_ticks = kReachHistoryTicks;
            return changed;
        }
        // The tracked set the record of `tick` carries, or null (step A3:
        // the reconcile reads the synthetic anchor's).
        const rollback_tracked* recorded_tracked(uint32_t tick) const {
            const tick_state* at = find_const(tick);
            return at ? &at->tracked : nullptr;
        }

        // Diagnostic only (see the recheck comment in restore_and_replay).
        void set_recheck_on_restore(bool enable) { recheck_on_restore_ = enable; }
        // On, every body is rechecked after all the poses are in; off (the
        // default), each at its own write.  Measured no better on the prop
        // probe - the contact set, not the recheck order, is what a restore
        // gets wrong - so it stays an A/B switch.
        void set_two_pass_recheck(bool enable) { two_pass_recheck_ = enable; }
        // Off reproduces the pre-v11 restore, for an A/B in one binary - which
        // is the only honest way to measure this, because two builds' plain
        // runs disagree by more than the effect does.
        void set_restore_impulses(bool enable) { restore_impulses_ = enable; }
        void set_restore_quaternions(bool enable) { restore_quaternions_ = enable; }
        void set_restore_clock(bool enable) { restore_clock_ = enable; }
        // v13: IVP's freeze bookkeeping.  Off reproduces the restore that
        // left the calm references, the unit countdown and the live sleep
        // state in place - each half an A/B in one binary.
        void set_restore_calm(bool enable) { restore_calm_ = enable; }
        void set_restore_unit(bool enable) { restore_unit_ = enable; }
        void set_sleep_as_recorded(bool enable) { sleep_as_recorded_ = enable; }
        // v15: the friction contacts a record had and the world lacks are
        // re-created at the anchor.  Off reproduces the restore that left a
        // restored body resting on nothing.
        void set_restore_contacts(bool enable) { restore_contacts_ = enable; }
        // Which recorded fields a re-created contact gets, and whether a body
        // with some contact left is left alone (BMMO_PHYSICS_CONTACT_*).
        void set_contact_flags(uint32_t flags) { contact_flags_ = flags; }
        // The script replay (game/script_replay.hpp).  Off: the records carry
        // no script state and a replay is physics only, as before.
        void set_replay_scripts(bool enable) { replay_scripts_ = enable; }
        // The IVP savestate (rollback_world::get_savestate / set_savestate,
        // docs/ivp-savestate-design.md).  On (the default), an anchor whose
        // record carries a blob is restored from it and the per-body writes are
        // left to snapshot rows and births.  Off reproduces the per-body restore
        // over the same records (the blobs are still captured), for the A/B in
        // one binary.
        void set_script_log_cap(uint32_t n) { script_log_cap_ = n; }
        const rollback_thresholds& thresholds() const { return thresholds_; }
        void set_exact_restore(bool enable) { exact_restore_ = enable; }
        // Log which branch every body of a restore took (capped at 16).
        void set_trace_plan(bool enable) { trace_plan_ = enable; }

        // Gap L (rollback-engine gaps design 2.3.4, read at step EL6 for
        // L3's replay-tick life test): the lifetime generation `entity` has
        // in the record of `tick` (0 when the record does not list it);
        // false when no record of that tick is kept.  Inside a replay the
        // ticks before the one being replayed are its re-captured records,
        // which take their generations from the live records.
        bool generation_at(const std::string& entity, uint32_t tick, uint32_t& out) const {
            const tick_state* state = find_const(tick);
            if (!state) return false;
            out = generation_in(*state, entity);
            return true;
        }

        bool recorded_input(const std::string& entity, uint32_t tick, input_frame& out) const {
            const tick_state* at = find_const(tick);
            if (!at) return false;
            const auto it = at->inputs.find(entity);
            if (it == at->inputs.end()) return false;
            out = it->second;
            return true;
        }

        // The own ball's entity the record of `tick` tracked, empty when it
        // had no body there or the tick is not recorded.  A trafo renames the
        // own ball, so a correction of an old tick must use this name and not
        // the one the ball has now (late-player design 1.3).
        std::string recorded_own_entity(uint32_t tick) const {
            const tick_state* at = find_const(tick);
            return at ? at->tracked.own_entity : std::string();
        }

        // Replaces the input the record of `tick` holds for `entity`.  A
        // replay feeds the own ball from the record (feed() in
        // restore_and_replay prefers it to input_at), so this, and not the
        // caller's own input map, is what makes a re-simulation use the frame
        // the server applied instead of the one this client played.  The
        // replay re-captures every record with what it fed, so the amendment
        // survives later replays.  False when the tick is not recorded.
        bool amend_recorded_input(const std::string& entity, uint32_t tick, const input_frame& frame) {
            tick_state* at = find(tick);
            if (!at || entity.empty()) return false;
            at->inputs[entity] = frame;
            return true;
        }

        // An own birth the server applied later than this client made it, or
        // never (late-player design 3.3: a MOVED, SUPERSEDED or REJECTED
        // echo).  The body `entity` of lifetime `generation` was made at
        // `origin` in the live run; the server's world has it only from
        // `applied` on, or not at all when `made` is false.
        //
        // Every record of that lifetime from `origin` on loses the body - its
        // pose, extra, contacts and navigation state describe motion the
        // server never stepped - so the next restore has no opinion about it
        // (the caller takes the live body out itself before rewinding).  The
        // records before `applied` (all of them when `made` is false) lose the
        // name and its input too: a replay neither feeds nor re-captures the
        // body there, and the re-captured records say there was none.  The
        // records from `applied` on keep the name, the generation and the
        // input, so the replay drives the body the caller makes again at
        // `applied` and re-captures it under the same lifetime.  From then on
        // a rewind anchored inside the gap finds the body born at `applied`
        // (moved_birth_record), also for a lifetime that has records before
        // `origin` (a beam keeps its generation).  Returns the number of
        // records changed.
        size_t move_birth(const std::string& entity, uint32_t generation, uint32_t origin, uint32_t applied, bool made) {
            if (entity.empty()) return 0;
            size_t changed = 0;
            for (auto& state: history_) {
                if (static_cast<int32_t>(state.tick - origin) < 0) continue;
                if (generation_in(state, entity) != generation) continue;
                const bool gap = !made || static_cast<int32_t>(state.tick - applied) < 0;
                bool touched = state.bodies.erase(entity) > 0;
                touched = state.extras.erase(entity) > 0 || touched;
                touched = state.contacts.erase(entity) > 0 || touched;
                touched = state.navs.erase(entity) > 0 || touched;
                if (gap) {
                    // Step RR: no row for motion the server never stepped.  A
                    // row of a tick from `applied` on is the server's state of
                    // the body made again there, and stays.
                    touched = state.rows.erase(entity) > 0 || touched;
                    touched = state.inputs.erase(entity) > 0 || touched;
                    if (state.tracked.own_entity == entity) {
                        state.tracked.own_entity.clear();
                        state.tracked.own_polls = false;
                        touched = true;
                    }
                    for (auto* names: {&state.tracked.remote_entities, &state.tracked.mechanisms}) {
                        const size_t before = names->size();
                        names->erase(std::remove(names->begin(), names->end(), entity), names->end());
                        touched = touched || names->size() != before;
                    }
                }
                if (touched) ++changed;
            }
            // One entry per lifetime and origin: a later echo of the same
            // birth replaces it.  Entries whose gap no rewind can be anchored
            // in any more go.
            moved_births_.erase(std::remove_if(moved_births_.begin(), moved_births_.end(), [&](const moved_birth& m) {
                return (m.entity == entity && m.origin == origin) || !find(m.applied - 1);
            }), moved_births_.end());
            if (made) moved_births_.push_back(moved_birth{entity, generation, origin, applied});
            return changed;
        }

        // Whether the record of `tick` holds a body for `entity`: a relayed
        // frame for a tick at which the name had none drove nothing, and is
        // no reason to replay.
        bool recorded_body(const std::string& entity, uint32_t tick) const {
            const tick_state* at = find_const(tick);
            return at && at->bodies.find(entity) != at->bodies.end();
        }
        // The state the record of `tick` holds for `entity` (the end of that
        // tick, as the last capture or replay left it); false when the tick
        // is not recorded or has no such body.  Diagnostics only (the retail
        // host's mirror-birth log, R3): nothing decides on it.
        bool recorded_body_state(const std::string& entity, uint32_t tick, bmmo_physics_body_state& out) const {
            const tick_state* at = find_const(tick);
            if (!at) return false;
            const auto it = at->bodies.find(entity);
            if (it == at->bodies.end()) return false;
            out = it->second;
            return true;
        }

        // Re-anchor on a tick in the past because the WORLD changed there, not
        // because an authoritative snapshot disagreed: the caller has created a
        // body for a lifecycle event stamped at `anchor` that arrived late (a
        // remote ball is relayed a round trip after it was born).  The new body
        // is in no record at `anchor`, so the restore leaves it exactly where
        // the caller put it and the replay steps it from there, which is what
        // makes every client's body set agree tick for tick without waiting for
        // a snapshot to notice.
        //
        // `anchor` must still be in the history; too late and there is nothing
        // to rewind to, which is the point at which a snapshot or a resync has
        // to take over.  Returns false then, and the caller must re-anchor.
        // `reach` (step A3): a deeper limit than max_resim_ticks, at most the
        // resync reach, for a correction inside an exact resync's re-step
        // (the client core's window_rewind_reach) and for a forced rewind
        // deeper than max_resim_ticks (force_rewind, step A4); 0 =
        // max_resim_ticks.
        bool rewind_to(const rollback_world& world, uint32_t anchor, uint32_t current_tick,
                       const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at,
                       const std::string& reason, uint32_t reach = 0) {
            if (!find(anchor)) {
                ++stats_.unmatched;
                return false;
            }
            const uint32_t limit = reach ? std::min(std::max(reach, thresholds_.max_resim_ticks), resync_reach())
                                         : thresholds_.max_resim_ticks;
            // Deeper than the engine re-simulates is never right without a
            // snapshot's rows: the too_far branch would write every tracked
            // body back to a record up to the whole history old, re-simulate
            // nothing and drop the history.  A caller's own rewind that deep
            // (an input corrected 49-96 ticks late, a lifecycle event that
            // late) is refused and counted as too far instead, unless the
            // caller passed a `reach` (above) that covers it.
            if (current_tick - anchor > limit) {
                ++stats_.too_far;
                return false;
            }
            // NOT stats_.mismatched: that counts authoritative snapshots this
            // engine disagreed with, and it is read as the measure of how far
            // the client is from the server.  A rewind the caller asked for is
            // not a disagreement with a snapshot and must not pollute it.
            ++stats_.rewinds;
            const std::map<std::string, row_ref> no_rows;
            auto no_report = [](uint8_t) {};
            return restore_and_replay(world, anchor, current_tick, no_rows, input_at, no_report, reason, nullptr, nullptr,
                                      limit);
        }

        // The resync re-step (resync design 2.6.4, 4.1; step A3): the
        // server's state of `anchor` = S restored over the client's anchor
        // record there - the localized scripts, the server's blob, its rows,
        // the reconcile's destroys (resync_spec) - and S+1 .. current_tick
        // re-simulated from the recorded inputs, as every rewind does.  The
        // reach is the history (history_ticks - 1), not max_resim_ticks: a
        // resync is the one replay allowed that deep (9 Q2).  False when S is
        // not recorded or deeper than the history; `out` says what the restore
        // did.
        bool resync(const rollback_world& world, uint32_t anchor, uint32_t current_tick, const resync_spec& spec,
                    const std::function<bool(const std::string& entity, uint32_t tick, input_frame& out)>& input_at,
                    resync_outcome& out, const std::string& reason) {
            out = {};
            if (!find(anchor)) {
                ++stats_.unmatched;
                return false;
            }
            if (static_cast<int32_t>(current_tick - anchor) < 0 || current_tick - anchor > resync_reach()) {
                ++stats_.too_far;
                return false;
            }
            ++stats_.resyncs;
            std::map<std::string, row_ref> rows;
            for (const auto& [entity, row]: spec.rows) {
                if (!row || entity.empty()) continue;
                row_ref ref;
                ref.row = row;
                ref.simulated = (row->flags & BODY_FLAG_SIMULATED) != 0;
                ref.authoritative = true;
                rows[entity] = ref;
            }
            auto no_report = [](uint8_t) {};
            return restore_and_replay(world, anchor, current_tick, rows, input_at, no_report, reason, &spec, &out);
        }
        // The deepest resync re-step: the history less its anchor.
        uint32_t resync_reach() const {
            return thresholds_.history_ticks > 0 ? static_cast<uint32_t>(thresholds_.history_ticks - 1) : 0u;
        }

    private:
        // One snapshot row a restore wrote, as it wrote it (step RR): the
        // pose and sleep write, the lifetime of the body it was written on,
        // and the friction contacts the restore then re-created for it from
        // the anchor's own record (the contact pass runs for a row like for
        // any body the blob did not cover), so the write after a replayed
        // step repeats the restore's sequence.
        struct applied_row {
            bmmo_physics_body_state state{};
            wake_mode mode = wake_mode::keep;
            uint32_t generation = 0;
            // The server's identity of the body (rollback_world::row_entity),
            // and whether it was the own ball (the replayed record names it).
            body_kind kind = body_kind::Ball;
            uint32_t owner = 0;
            bool own = false;
            bool identified = false;   // kind/owner came from the row (the restore had it)
            std::vector<bmmo_physics_contact> contacts;
            bool have_contacts = false;   // the restore called set_contacts with them
        };

        struct tick_state {
            uint32_t tick = 0;
            rollback_tracked tracked;
            std::map<std::string, bmmo_physics_body_state> bodies;
            // Parallel to `bodies`, and deliberately a separate map: a body we
            // recorded OURSELVES has a queued-impulse state, a body an
            // authoritative snapshot row describes does not, and the absence
            // is the information that tells the restore which it is holding.
            std::map<std::string, bmmo_physics_core_extra> extras;
            // The environment's clock at the end of this tick, when the world
            // provides it.
            bmmo_physics_clock_state clock{};
            bool have_clock = false;
            // The mechanism scripts' state at the end of this tick (game/script_replay.hpp).
            std::vector<uint8_t> scripts;
            bool have_scripts = false;
            std::map<std::string, bmmo_physics_nav_state> navs;
            std::map<std::string, input_frame> inputs;   // applied at this tick, per navigated ball
            // The friction contacts each recorded body had at the end of this
            // tick (bodies with none are absent).
            std::map<std::string, std::vector<bmmo_physics_contact>> contacts;
            // The IVP savestate at the end of this tick (rollback_world::get_savestate),
            // when the world provides one.  A restore that writes snapshot rows
            // on top of it captures it again, so it describes the world the
            // replay of T+1.. starts from.
            std::vector<uint8_t> savestate;
            bool have_savestate = false;
            // The snapshot rows a restore anchored at this tick wrote into the
            // world (rollback_world::replay_rows), by entity: what a replay
            // that crosses this tick writes again after its step.  Carried
            // over when a replay re-records the tick; merged when a later
            // snapshot of the same tick writes rows again (the newer row
            // wins).  Not touched by capture(), so an amendment keeps them.
            std::map<std::string, applied_row> rows;
        };


        // One body's part in the restore of tick T.
        struct restore {
            bmmo_physics_body_state state{};
            wake_mode mode = wake_mode::keep;
            bool from_row = false;
            bool force_recheck = false;
            bool recreate = false;       // destroyed at this restore, re-created at repose_tick (rollback_world::recreate_body)
            bool inserted = false;       // put at its Physicalize pose before repose_tick's step (rollback_world::insert_birth)
            // Put back by the anchor's savestate (rollback_world::set_savestate):
            // only an item from our own record, never a row or a birth.  Gets
            // no pose, extra or contact write of its own.
            bool covered = false;
            // Covered, but its friction contacts were not in the blob (its
            // system was skipped): the contact write from our record runs for
            // it anyway.
            bool recontact = false;
            uint32_t repose_tick = 0;    // 0: none - re-pose at this replayed tick
            // Gap L, L1 (step EL6): != 0, a life alive at the anchor that a
            // half of restore_lives revived; `state` is then its A record (or
            // a row), written at the anchor (covered by the blob), and the
            // replay removes the body at this tick.  With repose_tick too (a
            // later life of the name, born inside the window), the item has
            // two halves: the A-life over [A, revived_until), the birth at
            // repose_tick (`birth`).
            uint32_t revived_until = 0;
            // The birth half: what a body born at repose_tick is re-posed to
            // and compared with after that tick's step (its birth record).
            // For a plain birth item the same as state / mode / extra.
            struct birth_half {
                bmmo_physics_body_state state{};
                wake_mode mode = wake_mode::keep;
                bmmo_physics_core_extra extra{};
                bool have_extra = false;
            } birth;
            bool from_inputs = false;    // destroyed and made again from the inputs: kept as stepped (birth_from_inputs)
            // A resync's destroy (resync_spec::destroy): a body of the anchor
            // world the server's state at S lacks, made again by the replay at
            // repose_tick with no birth record to land on (never re-posed).
            bool rebirth = false;
            // Only set when the state came from our OWN record (the recorded
            // branch of the planning loop) or from the birth record of a body
            // born inside the window (written at its re-pose, not at the
            // parking write).
            bmmo_physics_core_extra extra{};
            bool have_extra = false;
        };

        static uint32_t generation_in(const tick_state& state, const std::string& entity) {
            auto it = state.tracked.generations.find(entity);
            return it == state.tracked.generations.end() ? 0u : it->second;
        }

        static bmmo_physics_body_state state_of(const body_state& row) {
            bmmo_physics_body_state state{};
            for (int k = 0; k < 3; ++k) {
                state.position[k] = row.position[k];
                state.linear[k] = row.linear[k];
                state.angular[k] = row.angular[k];
            }
            for (int k = 0; k < 4; ++k) state.rotation[k] = row.rotation[k];
            state.simulated = (row.flags & BODY_FLAG_SIMULATED) != 0;
            return state;
        }

        // Bit-identical pose, velocities and sleep state: what a re-created
        // birth has to show after its birth tick's step.
        static bool same_body_state(const bmmo_physics_body_state& a, const bmmo_physics_body_state& b) {
            return std::memcmp(a.position, b.position, sizeof a.position) == 0
                && std::memcmp(a.rotation, b.rotation, sizeof a.rotation) == 0
                && std::memcmp(a.linear, b.linear, sizeof a.linear) == 0
                && std::memcmp(a.angular, b.angular, sizeof a.angular) == 0
                && a.simulated == b.simulated;
        }

        // Does writing `target` actually move the body?  A write that does not
        // cannot touch anything new, so it skips the mindist recheck.
        static bool write_moves_body(const bmmo_physics_body_state& live, const bmmo_physics_body_state& target) {
            double dp = 0.0, dv = 0.0;
            for (int k = 0; k < 3; ++k) {
                dp += (target.position[k] - live.position[k]) * (target.position[k] - live.position[k]);
                dv += (static_cast<double>(target.linear[k]) - live.linear[k])
                    * (static_cast<double>(target.linear[k]) - live.linear[k]);
            }
            return std::sqrt(dp) > kRecheckPositionM || std::sqrt(dv) > kRecheckVelocityMs;
        }

        // Step RR: the rows a restore anchored at `t` wrote, written again
        // after `t`'s step inside a later replay, with that restore's
        // sequence for a row - the pose and sleep write (rechecked when it
        // moves the body, at the write or in a second pass), then the
        // friction contacts it re-created from the anchor's record.  A row
        // writes no extra (a row carries no queue).  Skipped: a body the
        // replayed world does not have at `t`, one of another lifetime than
        // the row was written on, and one the plan still parks for a later
        // birth - each tried again under the identity the server gave the
        // row (rollback_world::row_entity).  Returns the rows written.
        size_t reapply_rows(const rollback_world& world, const tick_state& recorded, const rollback_tracked& tracked,
                            const std::map<std::string, restore>& plan, uint32_t t) {
            size_t written = 0;
            std::vector<std::string> rechecks;
            std::vector<std::pair<std::string, const applied_row*>> contacted;
            std::unordered_set<std::string> targets;
            const auto parked = [&](const std::string& name) {
                const auto it = plan.find(name);
                return it != plan.end() && it->second.repose_tick != 0 && static_cast<int32_t>(it->second.repose_tick - t) > 0
                    && !(static_cast<int32_t>(it->second.revived_until - t) > 0);   // gap L: the A-life until its end
            };
            bmmo_physics_body_state live{};
            for (const auto& [entity, row]: recorded.rows) {
                // The body the row was written on, when the replayed world
                // has it at `t` in the same lifetime; otherwise the body the
                // server's identity has here (the own ball by the replayed
                // record, the others through row_entity).
                std::string target = entity;
                const auto g = tracked.generations.find(entity);
                bool usable = (g == tracked.generations.end() ? 0u : g->second) == row.generation && !parked(entity)
                            && world.get_body(entity, live);
                if (!usable) {
                    target = row.own ? tracked.own_entity
                           : world.row_entity && row.identified ? world.row_entity(row.kind, row.owner, t) : std::string();
                    usable = !target.empty() && target != entity && !parked(target) && world.get_body(target, live);
                }
                if (!usable || !targets.insert(target).second) continue;
                const bool recheck = recheck_on_restore_ && write_moves_body(live, row.state);
                const bool deferred = recheck && two_pass_recheck_ && world.recheck_body;
                world.set_body(target, row.state, row.mode, recheck && !deferred);
                if (deferred) rechecks.push_back(target);
                // The contacts the restore re-created were the anchor record's
                // of the body it wrote: not another body's.
                if (row.have_contacts && target == entity) contacted.emplace_back(target, &row);
                ++written;
            }
            for (const auto& entity: rechecks) world.recheck_body(entity);
            if (restore_contacts_ && world.set_contacts)
                for (const auto& [entity, row]: contacted) {
                    const int made = world.set_contacts(entity, row->contacts, contact_flags_);
                    if (made < 0) ++stats_.contact_restore_failures;
                    else stats_.contacts_restored += static_cast<uint64_t>(made);
                }
            return written;
        }

        // Gap L, L1 (rollback-engine gaps design 2.3.1, step EL6): the lives
        // the anchor's record has a body for that end inside (A, now] - in
        // A's tracked order (own, remotes, mechanisms: the order step P made
        // the mechanisms again in) - handed to rollback_world::restore_lives,
        // and each answer put into the plan: a revived name from its A record
        // (a row it has outranks the record, 4), a remade one as a birth item
        // at that tick from its birth record, already taken out (`recreate`);
        // a name no half revived is counted and logged by name.  The end of
        // a life is a change of its generation only (a moved birth's gap
        // keeps it); generations only grow, so a name whose newest record in
        // the window still has A's generation is alive now.
        void revive_ended_lives(const rollback_world& world, const tick_state& at, uint32_t anchor, uint32_t current_tick,
                                bool savestate, const resync_spec* rs, std::map<std::string, restore>& plan) {
            const tick_state* newest = nullptr;
            for (auto it = history_.rbegin(); it != history_.rend(); ++it)
                if (static_cast<int32_t>(it->tick - anchor) > 0 && static_cast<int32_t>(it->tick - current_tick) <= 0) {
                    newest = &*it;
                    break;
                }
            if (!newest) return;
            std::vector<ended_life> ended;
            std::set<std::string> seen;
            const auto consider = [&](const std::string& name, life_kind kind) {
                if (name.empty() || !seen.insert(name).second) return;
                if (at.bodies.find(name) == at.bodies.end()) return;
                if (rs && rs->gone.count(name)) return;   // step T3: the reconcile took it out
                const uint32_t generation = generation_in(at, name);
                if (generation_in(*newest, name) == generation) return;
                ended_life life;
                life.name = name;
                life.generation = generation;
                life.end_tick = newest->tick;
                life.kind = kind;
                for (const tick_state& state: history_) {
                    if (static_cast<int32_t>(state.tick - anchor) <= 0 || static_cast<int32_t>(state.tick - newest->tick) > 0)
                        continue;
                    if (generation_in(state, name) != generation) {
                        life.end_tick = state.tick;
                        break;
                    }
                }
                if (const auto p = plan.find(name); p != plan.end()) life.reborn_at = p->second.repose_tick;
                ended.push_back(std::move(life));
            };
            consider(at.tracked.own_entity, life_kind::own);
            for (const auto& name: at.tracked.remote_entities) consider(name, life_kind::mirror);
            for (const auto& name: at.tracked.mechanisms) consider(name, life_kind::mechanism);
            // Asked with no ended life too (step EL7, review finding 1): a
            // mirror life born and ended inside the window was no life at A
            // and has no body now, so no record names it here - only the
            // core's life log knows it (2.3.3: every player with a log
            // entry in (A, now]).
            std::map<std::string, life_outcome> out;
            world.restore_lives(anchor, current_tick, ended, savestate, out);
            stats_.lives_ended += ended.size();
            const auto sleep_of = [this](const bmmo_physics_body_state& state, wake_mode otherwise) {
                return sleep_as_recorded_ ? (state.simulated ? wake_mode::wake : wake_mode::freeze) : otherwise;
            };
            for (const ended_life& life: ended) {
                const auto o = out.find(life.name);
                const life_outcome got = o == out.end() ? life_outcome{} : o->second;
                if (got.remade_at) {
                    restore* item = nullptr;
                    if (const auto p = plan.find(life.name); p != plan.end() && p->second.repose_tick == got.remade_at) {
                        item = &p->second;
                    } else if (p != plan.end() && move_to_stamp(p->second, got.remade_at)) {
                        item = &p->second;
                    } else if (const tick_state* born = find_const(got.remade_at); born) {
                        if (const auto b = born->bodies.find(life.name); b != born->bodies.end()) {
                            restore made;
                            made.state = b->second;
                            made.mode = sleep_of(made.state, wake_mode::wake);
                            made.repose_tick = got.remade_at;
                            made.force_recheck = true;
                            if (const auto ex = born->extras.find(life.name); ex != born->extras.end()) {
                                made.extra = ex->second;
                                made.have_extra = true;
                            }
                            made.birth = {made.state, made.mode, made.extra, made.have_extra};
                            item = &(plan[life.name] = made);
                        }
                    }
                    if (item) {
                        item->recreate = true;
                        item->from_inputs = world.birth_from_inputs && world.birth_from_inputs(life.name);
                        ++stats_.lives_remade;
                    } else if (world.log && lives_logs_++ < 8) {
                        world.log("lives: " + life.name + " remade at tick " + std::to_string(got.remade_at)
                                  + " has no birth record there: not compared after that tick");
                    }
                }
                if (got.revived_until) {
                    const auto p = plan.find(life.name);
                    if (p != plan.end() && p->second.repose_tick && !p->second.recreate) {
                        // A later life of the name is still in the world:
                        // one body per name - the half must take it out.
                        if (world.log && lives_logs_++ < 8)
                            world.log("lives: " + life.name + " revived at tick " + std::to_string(anchor)
                                      + " while its later life born at " + std::to_string(p->second.repose_tick)
                                      + " is not taken out: kept on the birth item");
                    } else {
                        restore& item = plan[life.name];
                        if (!item.from_row) {
                            item.state = at.bodies.at(life.name);
                            item.mode = sleep_of(item.state, wake_mode::keep);
                            const auto ex = at.extras.find(life.name);
                            item.have_extra = ex != at.extras.end();
                            item.extra = item.have_extra ? ex->second : bmmo_physics_core_extra{};
                        }
                        item.revived_until = got.revived_until;
                        ++(life.kind == life_kind::own      ? stats_.lives_revived_own
                           : life.kind == life_kind::mirror ? stats_.lives_revived_mirror
                                                            : stats_.lives_revived_mech);
                        continue;
                    }
                }
                ++stats_.lives_unrevivable;
                if (world.log && lives_logs_++ < 8)
                    world.log("lives: " + life.name + " (alive at tick " + std::to_string(anchor) + ", generation "
                              + std::to_string(life.generation) + ") ended at tick " + std::to_string(life.end_tick)
                              + " inside the window and was revived by no half: the replay runs without it until then"
                              + (savestate ? " (the savestate skips it)" : "")
                              + (life.reborn_at ? "; its later life is born at " + std::to_string(life.reborn_at) : std::string())
                              + " (" + std::to_string(stats_.lives_unrevivable) + " so far)");
            }
            // Gap L, L2 (step EL7): a later life a half took out and the
            // replay makes again (remade_at) under a name that ended no life
            // here - the headless host names one mirror clone per life, so
            // the reconcile of the ended life takes the player's later
            // clone out under its own name, and a player whose window lives
            // the reconcile replays (no ended A-life) has its body now taken
            // out under whatever name it has: its birth item is recreated,
            // not destroyed by the loop below.
            std::set<std::string> ended_names;
            for (const ended_life& life: ended) ended_names.insert(life.name);
            for (const auto& [name, got]: out) {
                if (!got.remade_at || ended_names.count(name)) continue;
                const auto p = plan.find(name);
                if (p == plan.end() || p->second.recreate) continue;
                if (p->second.repose_tick != got.remade_at && !move_to_stamp(p->second, got.remade_at)) continue;
                p->second.recreate = true;
                p->second.from_inputs = world.birth_from_inputs && world.birth_from_inputs(name);
                ++stats_.lives_remade;
            }
        }

        // Step EL7 (review finding 3): a birth item planned at the tick the
        // body was made (its first record, L) for a life a half makes again
        // at its stamp E < L (the RC stamp rule: a body made live after its
        // stamp - a refused or too-deep lifecycle rewind's fallback - is
        // replayed at the stamp).  The item moves to E; its birth half stays
        // L's record, which a body made from the inputs is only compared
        // with (births_recreated_exact or a logged difference).
        static bool move_to_stamp(restore& item, uint32_t stamp) {
            if (!item.repose_tick || item.recreate || item.revived_until) return false;
            if (static_cast<int32_t>(stamp - item.repose_tick) >= 0) return false;
            item.repose_tick = stamp;
            return true;
        }

        // The oldest record that carries `entity` in its current lifetime: the
        // tick the body we have now was born at, as far as the history knows.
        const tick_state* find_const(uint32_t tick) const {
            for (const auto& state: history_) if (state.tick == tick) return &state;
            return nullptr;
        }

        const tick_state* birth_record(const std::string& entity, uint32_t after, uint32_t generation) const {
            for (const auto& state: history_) {
                if (state.bodies.find(entity) == state.bodies.end()) continue;
                if (generation_in(state, entity) != generation) continue;
                return state.tick > after ? &state : nullptr;
            }
            return nullptr;
        }

        // The birth record of a lifetime whose birth the server moved
        // (move_birth): for an anchor inside the gap, the record of the
        // applied tick, once a replay has re-captured the body there.  Null
        // when no moved birth of this lifetime covers the anchor, or the
        // applied tick's record has no body yet.
        const tick_state* moved_birth_record(const std::string& entity, uint32_t anchor, uint32_t generation) const {
            for (const auto& m: moved_births_) {
                if (m.entity != entity || m.generation != generation) continue;
                if (static_cast<int32_t>(anchor - m.applied) >= 0) continue;
                const tick_state* at = find_const(m.applied);
                if (at && at->bodies.find(entity) != at->bodies.end() && generation_in(*at, entity) == generation) return at;
            }
            return nullptr;
        }

        // Every name the restore may have an opinion about: recorded at T,
        // carried by a row, or recorded somewhere in the window we are about to
        // replay (a body born mid-window still has to be held at its birth pose
        // while the earlier ticks are stepped).
        std::vector<std::string> restore_candidates(uint32_t tick, const std::map<std::string, row_ref>& rows) const {
            std::vector<std::string> names;
            for (const auto& state: history_) {
                if (state.tick < tick) continue;
                for (const auto& [entity, body]: state.bodies) names.push_back(entity);
            }
            for (const auto& [entity, row]: rows) names.push_back(entity);
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        void capture(const rollback_world& world, tick_state& state) {
            struct scope {
                const rollback_world& w;
                explicit scope(const rollback_world& world) : w(world) { if (w.capture_scope) w.capture_scope(true); }
                ~scope() { if (w.capture_scope) w.capture_scope(false); }
            } lookups(world);
            auto grab = [&](const std::string& entity) {
                if (entity.empty()) return;
                bmmo_physics_body_state body{};
                if (world.get_body(entity, body)) state.bodies[entity] = body;
                // A tracked name the world no longer knows must not survive an
                // amendment: a later rollback would write the body back.
                else { state.bodies.erase(entity); state.extras.erase(entity); state.contacts.erase(entity); return; }
                bmmo_physics_core_extra extra{};
                if (world.get_body_extra && world.get_body_extra(entity, extra)) state.extras[entity] = extra;
                else state.extras.erase(entity);
                std::vector<bmmo_physics_contact> contacts;
                if (world.get_contacts && world.get_contacts(entity, contacts) > 0 && !contacts.empty()) state.contacts[entity] = std::move(contacts);
                else state.contacts.erase(entity);
            };
            grab(state.tracked.own_entity);
            for (const auto& remote: state.tracked.remote_entities) grab(remote);
            for (const auto& mechanism: state.tracked.mechanisms) grab(mechanism);
            auto grab_nav = [&](const std::string& entity) {
                if (entity.empty()) return;
                bmmo_physics_nav_state nav{};
                if (world.get_nav(entity, nav)) state.navs[entity] = nav;
                else state.navs.erase(entity);
            };
            grab_nav(state.tracked.own_entity);
            for (const auto& remote: state.tracked.remote_entities) grab_nav(remote);
            bmmo_physics_clock_state clock{};
            state.have_clock = world.get_clock && world.get_clock(clock);
            if (state.have_clock) state.clock = clock;
            state.have_scripts = replay_scripts_ && world.capture_scripts && world.capture_scripts(state.scripts);
            if (!state.have_scripts) state.scripts.clear();
            // Recorded whatever set_exact_restore says, so the switch is an A/B
            // over the same records.  An empty blob counts as none.
            state.savestate.clear();
            state.have_savestate = world.get_savestate && world.get_savestate(state.savestate) && !state.savestate.empty();
            if (!state.have_savestate) state.savestate.clear();
            if (world.get_savestate && !state.have_savestate) ++stats_.savestate_capture_failures;
        }

        tick_state* find(uint32_t tick) {
            for (auto it = history_.rbegin(); it != history_.rend(); ++it)
                if (it->tick == tick) return &*it;
            return nullptr;
        }

        struct tolerance {
            double position = 0.0;
            double velocity = 0.0;
        };

        tolerance tolerance_for(body_kind kind, double scale) const {
            const tolerance base = kind == body_kind::Mechanism
                ? tolerance{thresholds_.mechanism_position, thresholds_.mechanism_velocity}
                : tolerance{thresholds_.position, thresholds_.velocity};
            return {base.position * scale, base.velocity * scale};
        }

        // How far to widen the pair for a body predicted without the input that
        // drove it, over a window `lag` ticks deep (rollback_thresholds).
        double blind_scale(uint32_t lag) const {
            if (thresholds_.blind_reference_ticks == 0) return 1.0;
            const double s = static_cast<double>(lag) / thresholds_.blind_reference_ticks;
            if (s < 1.0) return 1.0;
            return s > thresholds_.blind_scale_max ? thresholds_.blind_scale_max : s;
        }

        rollback_thresholds thresholds_;
        std::deque<tick_state> history_;
        // The births the server moved (move_birth), while a rewind can still
        // be anchored inside their gap.
        struct moved_birth {
            std::string entity;
            uint32_t generation = 0, origin = 0, applied = 0;
        };
        std::vector<moved_birth> moved_births_;
        rollback_stats stats_;
        rewind_cost_meter rewind_cost_;   // step R12: the cap's moving averages
        std::function<std::chrono::steady_clock::time_point(clock_site)> cost_clock_;   // step R12: its samples' clock
        // A live record whose capture waits for its amend (record's
        // capture_now false).
        bool awaiting_capture_ = false;
        uint32_t awaiting_tick_ = 0;
        uint32_t never_made_logs_ = 0;
        uint32_t detailed_logs_ = 0;   // mismatch detail lines logged (capped)
        uint32_t resim_traces_ = 0;    // re-simulation trace lines logged (capped)
        bool verbose_ = false;
        bool recheck_on_restore_ = true;
        bool two_pass_recheck_ = false;   // measured no better (docs: 'Movable partners'); opt-in A/B
        bool restore_impulses_ = true;
        bool restore_quaternions_ = true;
        bool restore_clock_ = true;
        bool restore_calm_ = true;
        bool restore_contacts_ = true;
        uint32_t contact_flags_ = BMMO_PHYSICS_CONTACT_DEFAULT;
        bool restore_unit_ = true;
        bool sleep_as_recorded_ = true;
        bool replay_scripts_ = true;
        bool exact_restore_ = true;
        uint32_t script_mismatch_logs_ = 0;
        uint32_t too_far_logs_ = 0;
        uint32_t script_log_cap_ = 8;   // set_script_log_cap: a diagnostic run raises it to name every differing script
        uint32_t savestate_replay_logs_ = 0;
        uint32_t recreate_logs_ = 0;
        uint32_t replay_mechanism_logs_ = 0;
        uint32_t lives_logs_ = 0;   // gap L: unrevivable and inconsistent answers logged (capped)
        uint32_t script_float_logs_ = 0;
        bool trace_plan_ = false;
        uint32_t plan_traces_ = 0;
    };
}
