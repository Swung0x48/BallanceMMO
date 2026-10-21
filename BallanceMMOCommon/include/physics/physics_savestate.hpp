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
#include <string>
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

    // Validate a blob's framing without touching any world.
    bool check_savestate(const uint8_t* data, size_t size, std::string& error);

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
    std::string diff_savestate(const uint8_t* a, size_t a_size, const uint8_t* b, size_t b_size, bool ignore_ov = false,
                               int max_differences = 1);

    // FNV-1a over the blob bytes.
    uint64_t hash_savestate(const uint8_t* data, size_t size);
}
