#pragma once

// The IVP savestate (docs/ivp-savestate-design.md): the engine's whole dynamic
// state at a tick boundary - cores, hull managers, overlap elements, every
// collision checker (mindists, recursive mindists, watchers), the scheduled
// events, every friction system with its contact points and pairs, the
// simulation units, the revive list and the clock - as one byte blob, and the
// restore that tears the live state down and rebuilds it from the blob.
//
// A rollback that restores a body's pose, velocity, queue and quaternions but
// leaves IVP's collision bookkeeping as the LIVE world had it replays a ball
// in contact metres away from where the live run took it; with the blob the
// replay is meant to be bit-exact.  Compiled into the server's headless
// engine and the client's physics_RT plugin alike (physics_savestate_*.cpp).
//
// Everything here is local: the blob never goes on the wire.  Its layout is
// private to one build (little-endian, versioned, see
// physics_savestate_internal.hpp) and only ever read back by the same binary.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class CKIpionManager;

namespace bmmo::physics {
    // set_savestate flags.
    //
    // STRICT: the first inconsistency between the blob and the live world (a
    // recorded object that no longer exists, an edge offset outside its
    // surface, a missing friction partner) fails the restore after the
    // teardown instead of being logged in the report and skipped.  The tests
    // run strict; the rollback runs lenient so one vanished body does not
    // cost the whole anchor.
    constexpr uint32_t SAVESTATE_STRICT = 1u;
    // KEEP_UNKNOWN: a body alive now but absent from the blob (born after the
    // anchor) keeps its live collision state instead of being torn down with
    // the rest (it is then parked by the rollback and rechecked at its birth
    // tick, where the pairs come back).
    constexpr uint32_t SAVESTATE_KEEP_UNKNOWN = 2u;

    // A TRAVELLING blob (resync design 2.3, step A3): the server's savestate
    // of one tick, restored once into a client's world.  The blob is
    // otherwise local (see the note above); a travelling one is restored
    // under three rules of its own:
    //  - the capturing environment's identity in the header is not checked
    //    (it is another process's by definition);
    //  - `renames` maps a blob object's name to the live one's before the
    //    lookup (the resynced member's own clone, Ball_<type>_BMMO_<id> on the
    //    server, is the client's own ball); a name several objects share is
    //    matched by occurrence (the k-th such record to the k-th live object
    //    of that name in entity-id order), never by the capturing world's
    //    entity ids, which are another process's;
    //  - an object whose (renamed) name starts with one of `excluded_prefixes`
    //    is left out on both sides: its records are skipped and the live body
    //    stays unknown to the blob (the keep-local bodies, I3: the level-end
    //    balloon is each player's own), counted as `excluded`; an entry that
    //    starts with '=' names one object exactly instead (step BALA: a
    //    resync keeps the own ball that left the sync in its airship zone);
    //  - the PreSimulate queue is not compared: its entries name the blocks
    //    by the capturing process's CK ids, and the queue a restore keeps is
    //    the live world's own anyway (it is never rebuilt).  A difference in
    //    length is noted in the report's warnings.
    // Every other check (record framing, surfaces, edges, movability) is the
    // ordinary restore's.
    struct savestate_travel {
        std::vector<std::pair<std::string, std::string>> renames;
        std::vector<std::string> excluded_prefixes;
    };

    // What a capture or a restore did, for logs and tests.
    struct savestate_report {
        uint32_t objects = 0;       // objects in the blob (movable and static)
        uint32_t cores = 0;         // movable cores
        uint32_t collisions = 0;    // mindists, recursive mindists and watchers
        uint32_t contacts = 0;      // friction contact points
        uint32_t systems = 0;       // friction systems
        uint32_t units = 0;         // simulation units
        uint32_t events = 0;        // queued time events (PSI event included)
        uint32_t hull_entries = 0;  // hull-manager listener entries over all objects
        uint32_t skipped = 0;       // records the restore could not apply (lenient mode)
        // A travelling blob (savestate_travel): records left out by an
        // excluded prefix, and blob objects found under a renamed name.
        uint32_t excluded = 0, renamed = 0;
        // Live bodies the blob does not know (born after the anchor, a sector
        // physicalized inside the window).  They keep their place in the
        // movable list and the revive list but are left OUT of collision
        // detection (no tree node, no hull entry): the caller decides - a body
        // it will re-pose at its birth tick stays inert until then, one it has
        // no record of is rechecked back in at once (rollback_engine).
        std::vector<std::string> unknown_bodies;
        // Bodies whose friction contacts the restore could not put back: their
        // system had a body the world no longer has, and a system is rebuilt
        // whole or not at all.  The caller re-creates them from its own
        // per-body record (rollback_engine: set_contacts for a covered body).
        std::vector<std::string> contacts_dropped;
        std::vector<std::string> warnings;   // one line each, capped
    };

    // Capture the environment behind `physics`.  `out` is replaced.  The
    // blob of one world captured twice without a step in between is byte for
    // byte the same, and two worlds that are bit-exact copies of each other
    // produce the same blob: object order is by name, everything else in the
    // engine's own list orders.
    bool get_savestate(CKIpionManager* physics, std::vector<uint8_t>& out, std::string& error,
                       savestate_report* report = nullptr);

    // Put the environment back to the blob.  Flags above.  On a false return
    // the world is NOT guaranteed consistent (the teardown may have run):
    // the caller treats it like a failed anchor and re-anchors.
    bool set_savestate(CKIpionManager* physics, const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                       savestate_report* report = nullptr);
    // The same for a travelling blob (savestate_travel above).
    bool set_savestate(CKIpionManager* physics, const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                       savestate_report* report, const savestate_travel& travel);

    // Validate a blob's framing without touching any world.
    bool check_savestate(const uint8_t* data, size_t size, std::string& error);

    // The physics globals a blob was captured with (step H7X): the manager's
    // time factor and the environment's gravity as the blob records them
    // (floats), read without touching any world.  A restore writes neither
    // (the factor stays live, the gravity is compared); a resync re-step
    // runs at the server's (record-format-gaps design 4.2, A17).
    bool savestate_globals(const uint8_t* data, size_t size, float& time_factor, float gravity[3], std::string& error);

    // A deterministic, line-per-record text rendering of a blob (names, not
    // indices, wherever a record refers to an object), for traces and for
    // `diff_savestate`.  Floats are printed as hex so a one-ulp change shows.
    std::string describe_savestate(const uint8_t* data, size_t size);

    // The first difference between two blobs as one line ("" when the blobs
    // are identical): the record kind, the object or pair it belongs to, the
    // field, both values.  Byte identity is checked first, so an identical
    // pair costs one memcmp.
    // `ignore_ov` leaves the overlap tree out (its node count and records, the
    // root, each object's node): two runs that inserted a body at different
    // moments keep different tree layouts for good, and the question is then
    // what ELSE differs.
    // `max_differences` differing fields are listed, " | "-joined, in record order.
    // `ignore_order` also leaves out the object cache ring and the simulation
    // units' numbering, the bookkeeping two runs may order differently.
    std::string diff_savestate(const uint8_t* a, size_t a_size, const uint8_t* b, size_t b_size, bool ignore_ov = false,
                               int max_differences = 1, bool ignore_order = false);

    // diff_savestate with the one difference rollback-engine-gaps design 9.3
    // names (option (b), FULL step DX3; physics/outside_controllers.hpp): a
    // core of `own_cores` whose controller list differs only by controllers
    // named in `outside_names` (outside_controllers::controller_names of
    // Gameplay_Ingame's SetPhysicsForce blocks - never any other block's
    // "rt:force/", whose force is real) present on one side (and every
    // simulation unit's list by the same names) is "outside zero-force
    // controller on the own core".  An empty `outside_names` names nothing.
    // `named` lists those cores and names ("core Ball_Wood a+[rt:force/26432]
    // b+[]"), `named_cores` counts them; `where` is diff_savestate's answer
    // once they are set aside - "" when nothing else differs.  Any other
    // difference - another field, another core's controllers, another
    // controller, an order - stays in `where`; with neither switch, bytes
    // that still differ once the named difference is set aside ("bytes
    // differ but every field renders the same") stay there too, as in
    // diff_savestate.  With a switch, a difference only in the ignored
    // bookkeeping is not in `where` (the in-session check: not a mismatch,
    // as before) but in `residual` ("no difference outside the ignored
    // bookkeeping"; also without the named one), which SimTool fails on as
    // its plain path does.
    // The blob holds no force: that half of 9.3's precondition is the run
    // time check's (outside_controllers::carries_force).
    struct savestate_named_difference {
        std::string where;
        std::string named;
        std::string residual;
        int named_cores = 0;
    };
    savestate_named_difference diff_savestate_named(const uint8_t* a, size_t a_size, const uint8_t* b, size_t b_size,
                                                    const std::vector<std::string>& own_cores,
                                                    const std::vector<std::string>& outside_names, bool ignore_ov = false,
                                                    int max_differences = 1, bool ignore_order = false);

    // FNV-1a over the blob bytes.
    uint64_t hash_savestate(const uint8_t* data, size_t size);

    // A controller name the blob records and matches under another (step
    // TR3).  A force controller is named after the block that made it,
    // "rt:force/<block id>" (engine change #18), and a block's CK_ID is
    // this world's: a trafo set's copies are built at another moment in
    // every world, so the server's travelling blob (a resync's, a join's)
    // named controllers no client has, and the client's paper wind kept
    // its own state under the server's rows (TR3's H7: 6.6e-7 m).  A host
    // that knows a name every world gives a controller installs this: the
    // capture records that name, and the restore matches every live
    // controller under it.  It returns false to keep the name.  Per physics
    // manager; an empty function removes it.  Not thread-safe against a
    // capture or a restore of the same manager.
    void set_controller_alias(CKIpionManager* physics, std::function<bool(const char* name, std::string& alias)> alias);
}
