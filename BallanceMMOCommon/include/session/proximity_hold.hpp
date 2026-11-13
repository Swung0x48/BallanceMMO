#pragma once

// The proximity union's hold section (docs/world-agreement-gaps-design.md
// 4.2; game/proximity_union.hpp capture / restore) and its reference roots
// (step OBJB, docs/realtime-design.md), CK-free.  The section, little-endian:
//   u16 frames, per frame f32 x, y, z;
//   u16 by-name copies, per copy u32 the held entity's id;
//   u16 roots, u8 flags, per root u32 the generation of its pose
//       (kReferenceKeep: leave the live pose);
//   with flags & kHoldReferencePositions (BMMO_REFERENCE_POSES=verify):
//       u16 references, per reference f32 x, y, z (each probe's ObjectB).
// A root is the topmost ancestor of a probe's ObjectB: a module frame with
// no body, which no other part of a record carries.  Its generation names a
// pose of the root's history, so a restore can put back the pose a replay
// would otherwise read at its moved place.
//
// Declarations only, defined in src/session/proximity_hold.cpp: the union is
// header code compiled into core_host.cpp, which is at MSVC's section limit
// (C1128), so nothing here may instantiate a template or emit a function
// there.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bmmo::session {

    constexpr uint8_t kHoldReferencePositions = 1u;
    constexpr uint32_t kReferenceKeep = 0xFFFFFFFFu;

    // Where the parts of a hold section start, from the section's start.
    struct hold_layout {
        size_t frames = 0, names = 0, roots = 0, references = 0;
        size_t names_at = 0;        // the copies' u16 count
        size_t roots_at = 0;        // the roots' u16 count; the flags byte follows, then the generations
        size_t references_at = 0;   // the references' u16 count (0 without the flag)
        size_t end = 0;
        uint8_t flags = 0;
    };
    bool parse_hold(const uint8_t* data, size_t size, hold_layout& h);

    // Two hold sections of one world (the record diff, game/script_replay.hpp).
    // A count, a held by-name entity or a root's generation that differs is
    // logical: another candidate set, or a reference at another pose.  A
    // frame copies the position of the candidate place() chose, and a
    // correction of that ball moves it: float.  The references' positions
    // (verify only) are not compared.
    enum class hold_difference : uint8_t { none, float_only, logical };
    hold_difference compare_hold(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, std::string& what);

    // The reference roots of one union: the probes' ObjectB ids in the hold
    // order, their roots, and each root's history - the poses (world
    // matrices, 16 floats, compared bit for bit) it was observed at, in order,
    // each with its generation, and the pose the world is at.  An observation
    // of the pose the world is at changes nothing; of a later pose of the
    // history, advances to it (a replay re-making a move it made before); of
    // any other, drops the poses after the current one and appends it under
    // a generation never given before (a replay making another move, or the
    // live run a new one).  So a generation names one pose for the life of
    // the rewire: a record naming a dropped one (a record left past an anchor
    // whose replay moved the root elsewhere) is refused, and a replayed
    // record compares logical against the live one it differs from.  The
    // history keeps at least kDepth poses a root (the oldest kDepth go once
    // it holds 2 kDepth).  A refused root keeps its live pose (refused=);
    // only a section of another shape fails the restore.
    // BMMO_REFERENCE_POSES off | on | verify, on when unset: off tracks no
    // root; verify carries every ObjectB position and compares them after a
    // restore's writes, and compares the ObjectB place() reads with the one
    // the rewire tracked (misread=).
    class reference_tracker {
    public:
        static constexpr size_t kDepth = 1024;
        static int mode();   // 0 off, 1 on, 2 verify

        reference_tracker();
        ~reference_tracker();
        reference_tracker(const reference_tracker&) = delete;
        reference_tracker& operator=(const reference_tracker&) = delete;
        reference_tracker(reference_tracker&&) noexcept;
        reference_tracker& operator=(reference_tracker&&) noexcept;

        // The rewire: forget everything, then each probe's ObjectB in the
        // hold order with its root (0 / null for none) and the root's pose.
        void clear();
        void add(uint32_t object, uint32_t root, const float* root_pose);
        size_t roots() const;
        uint32_t root(size_t r) const;
        size_t objects() const;
        uint32_t object(size_t i) const;
        uint32_t generation(size_t r) const;

        // Root r's pose as the world has it now.
        void observe(size_t r, const float* pose);
        // The capture's tail: u16 roots, u8 flags, the generations, and in
        // verify the references' u16 count (the caller appends their
        // positions: positions()).
        void append(std::vector<uint8_t>& out) const;
        bool positions() const;
        // A localized (portable) record's tail: every root kReferenceKeep.
        void append_keep(std::vector<uint8_t>& out) const;

        // A restore, after every root was observed: the section's shape
        // checked (its extent, the root and reference counts; false: nothing
        // may be written) and each root's generation looked up - an unknown
        // one is refused and keeps the live pose.  Then pending(r): the pose
        // to write at root r (null: none); refuse_missing(r) when the root's
        // entity is gone (kept too); wrote(r) once written; carried(i, xyz)
        // the record's position of reference i (verify), compared(i, equal)
        // its verdict.
        bool check(const uint8_t* section, size_t size, std::string& error);
        const float* pending(size_t r) const;
        void refuse_missing(size_t r);
        void wrote(size_t r);
        bool carried(size_t i, uint32_t xyz[3]) const;
        void compared(bool equal);
        // The roots written since the last call, for the bodied check at the
        // next place (window births are destroyed by then).
        bool take_written(size_t r);
        void bodied(const char* name);
        // Verify: place() read reference i's block's ObjectB as `read`.
        void read_object(size_t i, uint32_t read);

        // "refs=<mode>/<roots> moved= written= remade= truncated= unmade=
        // bodied= verified= differing= refused= misread=", the first fault
        // of unmade / refused / misread named after them.  Every count but
        // moved, written, remade and verified is a fault.
        std::string line() const;

    private:
        static constexpr size_t kNone = static_cast<size_t>(-1);
        struct root_state {
            uint32_t id = 0;
            std::vector<float> poses;            // 16 floats a pose, oldest first
            std::vector<uint32_t> generations;   // each pose's, increasing, never given twice
            size_t at = 0;                       // the pose the world is at
            uint32_t next = 1;                   // the generation the next new pose takes
            size_t pending = kNone;              // the pose to write (an index)
            bool written = false;
        };
        void fault(const std::string& what);
        std::vector<root_state> roots_;
        std::vector<uint32_t> objects_;
        std::vector<uint8_t> carried_;   // the checked record's reference positions (verify)
        uint64_t moved_ = 0, written_ = 0, remade_ = 0, truncated_ = 0, unmade_ = 0, bodied_ = 0, verified_ = 0,
                 differing_ = 0, refused_ = 0, misread_ = 0;
        std::string bodied_first_, fault_first_;
    };

}
