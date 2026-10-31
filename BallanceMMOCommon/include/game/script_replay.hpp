#pragma once

// The mechanism scripts' state across a rollback (docs/lockstep-without-snapshots.md,
// "The script replay").
//
// A rollback re-simulates the physics of the ticks it rewinds across, and
// nothing else: the Virtools scripts that drive a mechanism on a timer - the
// swing's and the sandbag's Delayer that toggles a SetPhysicsForce every
// 32 frames, the balloon's Sequencer behind its sliders and springs - ran once,
// live, and the replay keeps their forces at whatever phase the toggle had
// when the rewind started.  Measured on the solo probe: 0.93 m on the swing
// from one depth-12 rewind, and the balloon chain and the swing are most of
// the mechanism rollbacks a networked session still takes.
//
// So the scripts travel with the rollback records: every tick the client
// captures the runtime state of the scripts that drive mechanisms, a restore
// puts the anchor's state back, and every replayed tick executes those scripts
// before its physics step - the frame order the live engine has.  A script's
// state here is what the engine itself changes between frames: the runtime
// activation flags of every block, which inputs and outputs are active, and
// the values of its local and output parameters (a Delayer keeps its elapsed
// time in output 0, a Sequencer its position).  The graph - blocks, links,
// input parameters - is not state.  What CK cannot carry is the IVP object a
// physics block owns: the force block keeps a raw pointer to its controller in
// a local parameter, so pointer-typed parameters are left alone and the
// force block's controller is captured and re-created through a hook the
// physics bridge provides.
//
// The sensors' reference.  In the retail graph every TT Scaleable Proximity
// in a mechanism script measures Ball_Pos_Frame, not the ball; that frame is
// a CHILD of the ball entity (parent=Ball_Stone, traced on the replay), so it
// follows the ball's restored and replayed body without any script.  That is
// still what a session with WORLD_PROXIMITY_UNION clear runs (a client's
// sensors watch its own ball only, G-c).  Under the bit (protocol 16,
// docs/world-agreement-gaps-design.md §2.2-§2.5, steps G2-G4) every world
// rewires the mechanism blocks to private frames BMMO_Prox_<owner>_<n> and
// their ActiveBall reads to private copies CurrentLevel#BMMOId_<owner>_<n>
// (game/proximity_union.hpp), which the host puts on the nearest ball with a
// body (own or mirror; session/proximity_rules.hpp) before the lifecycle
// changes of every live and every replayed tick; what a tick without a
// candidate keeps (the frames' positions, the by-name copies' entities)
// travels in the record's hold section (v6 bit 2, hold_hooks below), so a
// replay re-derives or restores the sensors' input without any script
// either.  The selection has room for
// "satellite" blocks of other scripts, should a level drive a sensors'
// reference from a script; the arena needs none, and the one candidate
// measured (Gameplay_Ingame's TT Set Dynamic Position with Ball_Pos_Frame as
// its Object, which moves something else after the frame) executed on its own
// tore the balloon bridge apart (7.9 m), so none is selected.
//
// Public CK API only (GetFlags/ModifyFlags, the IO Activate, parameter raw
// bytes, CKBehavior::Execute), because the same code runs against the retail
// CK2.dll inside the mod and against the reimplementation inside the headless
// clients.  Known limits: a link's activation delay (a per-link countdown the
// SDK does not expose) is not captured; a script that becomes active inside
// the replayed window, or that the game told to reset inside it (a sector
// activation resets the new sector's scripts, a death resets every module of
// the sector - resets_pending / restore's kept_live), is left as it is live and
// not executed (its bodies are on the rollback's birth branch).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <game/script_record_reuse.hpp>
#include <physics/callback_queue_plan.hpp>
#include <session/resync_rules.hpp>
#include <session/trafo_input.hpp>

#include "CKAll.h"

namespace bmmo::game {
    // What the SetPhysicsForce block holds outside CK: its IVP controller, the
    // point it pushes at and the force, in the block target's object space.
    struct force_block_state {
        bool exists = false;
        double position[3] = {};
        double force[3] = {};
    };

    // Supplied by the physics bridge (the controller class lives there).
    struct script_replay_hooks {
        std::function<bool(CKBehavior* block, force_block_state& out)> capture_force;
        std::function<bool(CKBehavior* block, const force_block_state& state)> restore_force;
        // Step RLC (bridge API v27): a joint block's constraint as an image
        // (the bridge's own bytes; empty when the block holds none), and one
        // made again from an image on a block that holds none.  Without them
        // the joint part of a record is v2's presence byte alone.  `order`
        // (step RLC's review): the constraint's place in its two cores'
        // controller lists among the controllers of its key
        // (bmmo_physics_joint_image::order) - what the blob cannot tell
        // apart; order_joints puts the given blocks' constraints back in
        // the places named (the lists changed, -1 with a message).
        std::function<bool(CKBehavior* block, std::vector<uint8_t>& image, std::array<int16_t, 2>& order)> capture_joint;
        std::function<bool(CKBehavior* block, const std::vector<uint8_t>& image)> restore_joint;
        std::function<int(const std::vector<std::pair<CKBehavior*, std::array<int16_t, 2>>>& blocks, std::string& error)>
            order_joints;
    };

    // One PreSimulate queue entry (bridge API v23, rollback-engine-gaps
    // design 2.2.3, gap Q): the block that queued it, its root script, its
    // slot, and whether it is one of the bridge's own callbacks.  A queued
    // retail entry has no state of its own beyond these (design 1.1).
    struct queued_callback {
        uint32_t behavior = 0;
        uint32_t owner_script = 0;
        uint8_t slot = 0;
        bool bmmo = false;
    };
    // Supplied by the binding (session/rollback_binding_scripts.hpp) from
    // the host's physics backend: the whole queue, and the rebuild of the
    // entries of `scope`'s scripts to `target` (returns the entries removed,
    // -1 with a message: refused, nothing changed).
    struct queue_hooks {
        std::function<bool(std::vector<queued_callback>& out)> capture;
        std::function<int(const std::vector<CK_ID>& scope, const std::vector<queued_callback>& target, std::string& error)> restore;
    };
    // Step G3 (docs/world-agreement-gaps-design.md 4.2): the proximity
    // union's hold section of the record (game/proximity_union.hpp capture /
    // restore), from the binding of a world under WORLD_PROXIMITY_UNION.
    struct hold_hooks {
        std::function<void(std::vector<uint8_t>& out)> capture;
        std::function<bool(const uint8_t* data, size_t size, std::string& error)> restore;
    };
    // Step RK2 (docs/record-format-gaps-design.md 11 Q4, the user's decision
    // of 2026-10-24: "the written cells are recorded with the other cells"):
    // one column of the record's written-cell section - every row of a
    // constant int / float column a selected script writes
    // (game/array_reads.hpp written_cells decides which).
    struct written_column {
        CK_ID array = 0;
        int column = -1;
    };

    class script_replay {
    public:
        static constexpr uint32_t kMagic = 0x52534d42u;   // "BMSR"
        // v2: the joint blocks' handle presence, a graph's in-flight delayed
        // links, the three data-array cells the sector manager and the modules
        // read (plan doc 10).
        static constexpr uint32_t kVersion = 2;
        // Step RLC: in a local record (every version) a joint block's handle
        // byte is 0 (none), 1 (one) or kJointImaged (one, and a u32 hash of
        // its image and its two i16 core-list places follow - the joint
        // ledger below); the portable record keeps 0 / 1.  The record is
        // this process's own, so no version.
        // v3 (docs/trafo-replication-design.md 4.5, step T1): the v2 record,
        // byte for byte after the version word, then the trafo sets' scripts
        // keyed by id - written only when a trafo part is selected, so every
        // record without trafo sets stays v2:
        //   u32 trafo_count, per script: u32 id, u8 present, block (as v2)
        // present: 0 no such script, 1 a block follows, 2 (kTrafoDormant,
        // step B6RC / RC4) the script holds its set's built state and no
        // block follows.  The record is this process's own (the wire and the
        // journals carry the portable record), so the value needs no version.
        // A selected trafo script the record does not list (its set was made
        // after the record) is restored to its set's built state; a recorded
        // one that no longer exists fails the restore
        // (session/trafo_input.hpp plan_trafo_record_restore).
        static constexpr uint32_t kVersionTrafo = 3;
        // v4 / v5 (rollback-engine-gaps design 2.2.3, gap Q, step B7EG): the
        // v2 / v3 record byte for byte after the version word, then the
        // PreSimulate queue's entries whose block belongs to a recorded
        // script (the selection's or a trafo set's), in queue order:
        //   u16 count, per entry: u32 behavior, u8 slot
        // Written only when the binding passes the queue hooks (the core
        // hosts); the legacy retail path's records stay v2 byte for byte.
        // A restore rebuilds those scripts' entries to the record's (the
        // block state and its queued entry come back together), so a
        // replayed Create or WakeUp edge queues exactly what the live frame
        // queued instead of a copy next to the live one.
        static constexpr uint32_t kVersionQueue = 4;
        static constexpr uint32_t kVersionTrafoQueue = 5;
        // v6 (docs/world-agreement-gaps-design.md 4.2, step G3): a sections
        // word after the version replaces the version-encoded combination,
        //   u32 sections: bit 0 the trafo list (as v3), bit 1 the queue
        //   section (as v4), bit 2 the proximity union's hold section
        // then the v2 body byte for byte, then each section present in that
        // order.  The hold section (game/proximity_union.hpp capture):
        //   u16 frames, per frame f32 x, y, z; u16 by-name copies, per copy
        //   u32 the held entity's id (this process's).
        // Written only when the binding passes the hold hooks (a world
        // under WORLD_PROXIMITY_UNION), so every other record stays v2-v5
        // byte for byte.  A v6 word with an unknown bit is refused.
        // Bit 3 (step RK2, record-format-gaps design 11 Q4): the written-cell
        // section, after the hold section - every row of each column the
        // binding names (game/array_reads.hpp written_cells: the int / float
        // columns a selected script writes, today the fans' sound-slot
        // column P_Modul18_ActiveSounds[*][1]):
        //   u16 columns, per column: u32 array id (this process's), u32
        //   column, u32 rows, rows x u32 value (the element's bytes)
        // Written only when the binding passes the columns (a core host), so
        // a record without them is the record it was; a record with them is
        // v6 even without the hold section.  Local only: the portable record
        // (the wire, the journals) carries no such section, so a localized
        // record has none and its restore keeps the live column.  The diff
        // does not compare it (the decision: "no check compares it").
        static constexpr uint32_t kVersionSections = 6;
        static constexpr uint32_t kSectionTrafo = 1u << 0;
        static constexpr uint32_t kSectionQueue = 1u << 1;
        static constexpr uint32_t kSectionHold = 1u << 2;
        static constexpr uint32_t kSectionCells = 1u << 3;
        static constexpr uint32_t kSectionsKnown = kSectionTrafo | kSectionQueue | kSectionHold | kSectionCells;
        static bool known_version(uint32_t v) { return v >= kVersion && v <= kVersionSections; }
        static bool has_trafo(uint32_t v) { return v == kVersionTrafo || v == kVersionTrafoQueue; }
        static bool has_queue(uint32_t v) { return v == kVersionQueue || v == kVersionTrafoQueue; }
        // The sections of a record whose version word was read at `pos`:
        // v2-v5 from the version, v6 from its word (read here).  False: an
        // unknown version, a truncated word or an unknown bit.
        static bool read_sections(const std::vector<uint8_t>& in, size_t& pos, uint32_t version, uint32_t& sections) {
            sections = 0;
            if (!known_version(version)) return false;
            if (version == kVersionSections) {
                if (!get_u32(in, pos, sections)) return false;
                return (sections & ~kSectionsKnown) == 0;
            }
            if (has_trafo(version)) sections |= kSectionTrafo;
            if (has_queue(version)) sections |= kSectionQueue;
            return true;
        }
        // The hold section's extent (its counts say how long it is).
        static bool skip_hold(const std::vector<uint8_t>& in, size_t& pos) {
            uint16_t frames = 0, names = 0;
            if (!get_u16(in, pos, frames) || pos + static_cast<size_t>(frames) * 12 > in.size()) return false;
            pos += static_cast<size_t>(frames) * 12;
            if (!get_u16(in, pos, names) || pos + static_cast<size_t>(names) * 4 > in.size()) return false;
            pos += static_cast<size_t>(names) * 4;
            return true;
        }
        // The written-cell section in this process (step RK2), diagnostic
        // only (as queue_stats): records captured with it and the columns
        // and rows they carried; restores that wrote it back, the rows
        // written and the columns left alone.  The headless client prints
        // them in its cells: line, SimTool in its savestate-check summary.
        struct written_cells_counters {
            uint64_t captured = 0, captured_columns = 0, captured_rows = 0, restores = 0, rows = 0, skipped = 0;
        };
        static written_cells_counters& written_cells_stats() {
            static written_cells_counters counters;
            return counters;
        }
        // The written-cell section's extent (step RK2).
        static bool skip_written(const std::vector<uint8_t>& in, size_t& pos) {
            uint16_t columns = 0;
            if (!get_u16(in, pos, columns)) return false;
            for (uint16_t i = 0; i < columns; ++i) {
                uint32_t array = 0, column = 0, rows = 0;
                if (!get_u32(in, pos, array) || !get_u32(in, pos, column) || !get_u32(in, pos, rows)
                        || pos + static_cast<size_t>(rows) * 4 > in.size())
                    return false;
                pos += static_cast<size_t>(rows) * 4;
            }
            return true;
        }
        // The written-cell section of the columns `cells` names, as they
        // stand.  A column that is gone, or no longer int / float, is written
        // with 0 rows (the restore then leaves it alone and counts it).
        static void put_written(CKContext* context, const std::vector<written_column>& cells, std::vector<uint8_t>& out) {
            put_u16(out, static_cast<uint16_t>(cells.size()));
            ++written_cells_stats().captured;
            written_cells_stats().captured_columns += cells.size();
            for (const written_column& w: cells) {
                auto* a = CKDataArray::Cast(context->GetObject(w.array));
                const bool ok = a && w.column >= 0 && w.column < a->GetColumnCount()
                             && (a->GetColumnType(w.column) == CKARRAYTYPE_INT || a->GetColumnType(w.column) == CKARRAYTYPE_FLOAT);
                const int rows = ok ? a->GetRowCount() : 0;
                put_u32(out, static_cast<uint32_t>(w.array));
                put_u32(out, static_cast<uint32_t>(w.column));
                put_u32(out, static_cast<uint32_t>(rows));
                written_cells_stats().captured_rows += static_cast<uint64_t>(rows);
                for (int r = 0; r < rows; ++r) {
                    uint32_t value = 0;
                    a->GetElementValue(r, w.column, &value);
                    put_u32(out, value);
                }
            }
        }
        // The section back into the world (step RK2): a column whose array
        // is gone, whose type changed or whose row count differs from the
        // record's is left as it stands and counted (written_cells_skipped).
        static bool restore_written(CKContext* context, const std::vector<uint8_t>& in, size_t pos, size_t end) {
            uint16_t columns = 0;
            if (!get_u16(in, pos, columns)) return false;
            ++written_cells_stats().restores;
            for (uint16_t i = 0; i < columns; ++i) {
                uint32_t array = 0, column = 0, rows = 0;
                if (!get_u32(in, pos, array) || !get_u32(in, pos, column) || !get_u32(in, pos, rows)
                        || pos + static_cast<size_t>(rows) * 4 > end)
                    return false;
                auto* a = CKDataArray::Cast(context->GetObject(static_cast<CK_ID>(array)));
                const int c = static_cast<int>(column);
                const bool ok = a && c >= 0 && c < a->GetColumnCount() && rows > 0
                             && (a->GetColumnType(c) == CKARRAYTYPE_INT || a->GetColumnType(c) == CKARRAYTYPE_FLOAT)
                             && a->GetRowCount() == static_cast<int>(rows);
                if (!ok) {
                    ++last_restore().written_cells_skipped;
                    ++written_cells_stats().skipped;
                    pos += static_cast<size_t>(rows) * 4;
                    continue;
                }
                for (uint32_t r = 0; r < rows; ++r) {
                    uint32_t value = 0;
                    get_u32(in, pos, value);
                    a->SetElementValue(static_cast<int>(r), c, &value, sizeof(value));
                }
                last_restore().written_cells_rows += rows;
                written_cells_stats().rows += rows;
            }
            return true;
        }
        // The queue section's counters (the core report's cbq field).
        struct queue_counters {
            uint64_t captured = 0, rebuilt = 0, removed = 0, rebuild_fail = 0, held = 0, holds = 0, unhold_clamped = 0,
                     no_record = 0, hold_fail = 0;
        };
        static queue_counters& queue_stats() {
            static queue_counters counters;
            return counters;
        }
        // The flags a restore writes: the engine's per-frame activation
        // bookkeeping.  Everything else in the flag word describes the graph.
        static constexpr CKDWORD kRuntimeFlags = CKBEHAVIOR_ACTIVE | CKBEHAVIOR_ACTIVATENEXTFRAME
                                               | CKBEHAVIOR_DEACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME
                                               | CKBEHAVIOR_LAUNCHEDONCE;

        static const CKGUID& force_block_guid() {
            static const CKGUID guid(0x56e20c57, 0xb926068);   // SetPhysicsForce (physics_RT/Behaviors/PhysicsForce.cpp)
            return guid;
        }
        static const CKGUID& dynamic_position_guid() {
            static const CKGUID guid(0xfd4755f, 0x7de22dc8);   // TT Set Dynamic Position (TT_Toolbox_RT/Behaviors/SetDynamicPosition.cpp)
            return guid;
        }

        // What travels with the records: the mechanism scripts, and the
        // satellite blocks (see the top of the file).
        struct selection {
            std::vector<CK_ID> scripts;
            std::vector<CK_ID> satellites;
            bool empty() const { return scripts.empty() && satellites.empty(); }
            void clear() { scripts.clear(); satellites.clear(); }
        };
        // The trafo sets' part of a record (step T1, game/trafo_sets.hpp):
        // the scripts the trafo frame runs, in its order (sets by player id,
        // the copied order inside a set), and each one's built state - its
        // block bytes right after the set was built.  Kept beside the
        // selection, never in it: `selection` is held by the legacy retail
        // state (physics_session_state::script_ids), whose layout stays as it
        // is until R7 (session-core design 3.2).
        struct trafo_part {
            std::vector<CK_ID> scripts;
            std::map<CK_ID, std::vector<uint8_t>> built;
            bool empty() const { return scripts.empty(); }

            // RC4, the dormant-script encoding (rewind-cost design RC4, trafo
            // design 9.4): per script of `scripts`, whether it is KNOWN to
            // hold its built state.  A cache over the world, never state:
            // only a capture that found the script's bytes equal to its built
            // bytes, or a restore that wrote them, sets an entry; everything
            // that can change the script - the trafo frame's execution and
            // next-frame flags (execute_trafo), a trafo input's flags
            // (run_trafo), a remake's execution, a restore of other bytes -
            // clears it, and an entry nobody set is unknown (the script is
            // walked).  Rebuilding the part (a set added, removed or
            // reselected) starts it empty.  A wrong "unknown" costs a walk; a
            // wrong "known" would restore the built state over a changed
            // script, so the capture also checks the root's activation words
            // against the built bytes (dormant_guard_trips counts the misses).
            mutable std::vector<uint8_t> dormant;
            size_t index_of(CK_ID id) const {
                for (size_t i = 0; i < scripts.size(); ++i)
                    if (scripts[i] == id) return i;
                return scripts.size();
            }
            bool is_dormant(size_t i) const { return i < dormant.size() && dormant[i] != 0; }
            void set_dormant(size_t i, bool known) const {
                if (dormant.size() != scripts.size()) dormant.assign(scripts.size(), 0);
                if (i < dormant.size()) dormant[i] = known ? 1 : 0;
            }
            void touch(CK_ID id) const { set_dormant(index_of(id), false); }
            void forget_all() const { dormant.clear(); }
        };
        // Record v3's per-script byte for a trafo script in its set's built
        // state (RC4): no block follows, and the restore writes the built
        // bytes.  A capture writes it exactly when the script's bytes equal
        // the built ones (known from the ledger or found by the walk), so two
        // records of equal states are equal byte for byte.
        static constexpr uint8_t kTrafoDormant = 2;
        // What the dormant encoding did since the last reset (RC4): scripts
        // written as one byte from the ledger, walked and found built (the
        // ledger healed), walked and written as a block, and ledger entries
        // the root check refused (an unhooked writer: a bug to find).  One
        // process-wide set of plain counters, diagnostic only: captures and
        // restores run on the game thread (a tool that ran worlds on several
        // threads would race on them; nothing decides on them).  The headless
        // client prints them in its trafo_dormancy: line.
        struct dormancy_counters {
            uint64_t from_ledger = 0, healed = 0, walked = 0, guard_trips = 0, restored_built = 0;
        };
        static dormancy_counters& dormancy() {
            static dormancy_counters counters;
            return counters;
        }
        static bool is_force_block(CKBehavior* beh) {
            return beh && beh->IsUsingFunction() && beh->GetPrototypeGuid() == force_block_guid();
        }
        // The blocks that keep an IVP constraint or spring in local 0 (Set
        // Physics Ball Joint, Hinge, Slider, Spring): the handle is a pointer
        // and never written, but whether the block HAS one is state - a
        // restore to an anchor at which the block had none deletes the live
        // one through the block's own reset callback, so a body destroyed at
        // the restore carries no joint and the replayed Create makes a new one
        // from nothing (Hinge's Create would otherwise delete a freed pointer).
        static bool is_joint_block(CKBehavior* beh) {
            if (!beh || !beh->IsUsingFunction()) return false;
            const CKGUID g = beh->GetPrototypeGuid();
            return g == CKGUID(0x5e624f0a, 0x35160450) || g == CKGUID(0x41cd3653, 0x0de60c1d)
                || g == CKGUID(0x2973360e, 0x23d31aa7) || g == CKGUID(0x24a06a3a, 0x07100fce);
        }
        static void* joint_handle(CKBehavior* beh) {
            void* handle = nullptr;
            if (beh->GetLocalParameterCount() > 0) beh->GetLocalParameterValue(0, &handle);
            return handle;
        }

        // What the last restore did beyond writing bytes: joints reset to
        // their anchor absence, joints the anchor had and the live world has
        // lost (nothing can make those), delayed links re-armed, links that
        // could not be (their output has a plain link too), live in-flight
        // links the anchor did not have (the SDK cannot clear a graph's list).
        struct restore_counters {
            uint32_t joints_reset = 0, joints_missing = 0, links_rearmed = 0, links_unrestorable = 0, links_live_pending = 0;
            uint32_t cells_written = 0;
            // Step RK2: the written-cell section's rows written back, and its
            // columns left alone (array gone, type or row count changed).
            uint32_t written_cells_rows = 0, written_cells_skipped = 0;
            uint32_t joints_released = 0;   // handles nulled because the rollback destroyed their body (release_joints)
            uint32_t trafo_built = 0;       // trafo scripts the record did not list, put to their set's built state (v3)
            uint32_t strings_resized = 0;   // string parameters written back at another length (step RO1, resize_strings)
            // Step RLC: constraints made again from the record's image (the
            // window shut them down), and live ones replaced by it (the window
            // shut down and re-made them: another image).
            uint32_t joints_recreated = 0, joints_replaced = 0;
            // Step RLC's review: core lists whose constraints the restore put
            // back in the record's order (a constraint made again lands last).
            uint32_t joints_reordered = 0;
        };
        static restore_counters& last_restore() {
            static restore_counters counters;
            return counters;
        }

        // Step RLC (roadmap section 4 item 4, "a constraint shut down inside
        // the window"; rollback-engine-gaps design, As built (step RLC)).  A
        // joint block's constraint is made from the bodies' poses at its
        // Create and never changes afterwards; a Shutdown inside a rollback
        // window deletes it, and no pose at the anchor computes it again.
        // So every capture that sees a constraint names it in the record by
        // the hash of its image (kJointImaged, below) and keeps the image
        // here, per block, the most recently seen last; a restore to a tick
        // at which the block held a constraint the live world no longer has
        // (or holds another one) makes it again from that image, before the
        // blob, which then finds it among the core's controllers.  The
        // ledger is this process's: the images never travel (the portable
        // record carries the presence alone, and a localized record names no
        // image - its restore takes the block's latest one).
        static constexpr uint8_t kJointImaged = 2;   // the handle byte: present, a u32 image hash follows (local records)
        struct joint_ledger {
            struct entry {
                uint32_t hash = 0;
                std::vector<uint8_t> image;
            };
            static constexpr size_t kKeep = 8;   // images per block
            std::map<CK_ID, std::vector<entry>> blocks;
            void note(CK_ID id, uint32_t hash, const std::vector<uint8_t>& image) {
                std::vector<entry>& list = blocks[id];
                if (!list.empty() && list.back().hash == hash) return;
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i].hash != hash) continue;
                    entry e = std::move(list[i]);
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                    list.push_back(std::move(e));
                    return;
                }
                list.push_back(entry{hash, image});
                if (list.size() > kKeep) list.erase(list.begin());
            }
            // hash 0: the block's latest image (a record that names none).
            const entry* find(CK_ID id, uint32_t hash) const {
                const auto it = blocks.find(id);
                if (it == blocks.end() || it->second.empty()) return nullptr;
                if (!hash) return &it->second.back();
                for (const entry& e: it->second)
                    if (e.hash == hash) return &e;
                return nullptr;
            }
        };
        static joint_ledger& joints() {
            static joint_ledger ledger;
            return ledger;
        }
        struct joint_counters {
            uint64_t imaged = 0;       // captures that named a constraint by its image
            uint64_t recreated = 0;    // constraints made again at a restore
            uint64_t replaced = 0;     // live constraints replaced by the record's
            uint64_t missing = 0;      // the record had one, the restore could not make it
            uint64_t unmatched = 0;    // a live constraint with another image the ledger no longer holds the record's for (left)
            uint64_t reordered = 0;    // core lists put back in the record's constraint order
            uint64_t order_failed = 0; // restores refused because that order could not be put back
        };
        static joint_counters& joint_stats() {
            static joint_counters counters;
            return counters;
        }
        // SimTool's control (--joint-order-off): the restore leaves the
        // constraints' order as it finds it, as before the review.
        static bool& joint_order_off() {
            static bool off = false;
            return off;
        }
        // The joint blocks the restore under way found imaged in its record,
        // with their recorded places (restore_joint fills it, restore orders).
        static std::vector<std::pair<CKBehavior*, std::array<int16_t, 2>>>& joints_to_order() {
            static std::vector<std::pair<CKBehavior*, std::array<int16_t, 2>>> blocks;
            return blocks;
        }
        static uint32_t joint_image_hash(const std::vector<uint8_t>& image) {
            uint32_t h = 2166136261u;
            for (uint8_t b: image) { h ^= b; h *= 16777619u; }
            return h ? h : 1u;
        }
        // Step B2SX (record-format Q5 / A23, the user's option (a)): the
        // outputs the last restore turned back on to re-arm an in-flight
        // delayed link - the ones links_rearmed counts - as (block, output
        // index).  The record has such an output off (the capture takes it as
        // it stands, and the graph switched it off when it queued the link);
        // the restore switches it on so that the next frame queues the link
        // again.  A compare made right after a restore, before any frame ran
        // (a depth-0 rollback check), sees exactly these bits differ; it
        // ignores them through rearmed_output_masks() and diff's `masks`.
        static std::vector<std::pair<CK_ID, int>>& last_rearmed_outputs() {
            static std::vector<std::pair<CK_ID, int>> outputs;
            return outputs;
        }
        // The last restore's re-armed outputs as one bit mask per block (an
        // output index of 32 or more is not recorded and not re-armed).
        static std::map<CK_ID, uint32_t> rearmed_output_masks() {
            std::map<CK_ID, uint32_t> masks;
            for (const auto& [block, index]: last_rearmed_outputs())
                if (index >= 0 && index < 32) masks[block] |= 1u << index;
            return masks;
        }
        // Step RO1: whether a restore writes back a string parameter whose
        // length changed since the record (restore_parameter).  Off by
        // default - the legacy retail path keeps the old rule, the value left
        // alone - and switched on around its restore by a binding that asks
        // for it (session/rollback_binding_scripts.hpp resize_strings: the
        // core hosts and the tools).
        static bool& resize_strings() {
            static bool on = false;
            return on;
        }

        // Step B2SW (resync finding G1, rollback-engine gap F): the behaviour
        // manager's object list, as the live frame has it.  The live frame
        // (CKBehaviorManager::Execute) runs, and processes the next-frame
        // flags of, only the scripts whose owner is in m_BeObjects after the
        // frame's ManageObjectsActivity; an owner whose scripts all finished
        // left it (CKBeObject::ExecuteBehaviors -> RemoveObjectNextFrame), and
        // a finished script's scene flag CK_SCENEOBJECT_ACTIVE was cleared
        // (CKBehavior::CheckBehaviorActivity).  A replay's execute_frame calls
        // Execute directly, so it saw neither.  With this switch on:
        //   * restore re-registers every selected script it writes back active
        //     whose owner the live frame would not run (G1): the scene flag
        //     set first, then CKScene::Activate (exported by the retail CK2 as
        //     by the reimplementation: ActivateScript adds the owner for the
        //     next frame and, the scene flag being set, calls no
        //     CKM_BEHAVIORACTIVATESCRIPT callback), then the record's own
        //     next-frame flags put back over the ones Activate set;
        //   * execute_frame processes a selected script's next-frame flags
        //     only where the live frame did (F): its owner among those the
        //     live frame of the replayed tick ran (the host's recorded live
        //     order), or joining the list next frame (execute_scripts).  The
        //     trafo list is not judged: the trafo frame runs it directly,
        //     live too.
        // Off by default - the legacy retail path keeps the old rule - and
        // switched on around the restore and the replayed frame by a binding
        // that asks for it (session/rollback_binding_scripts.hpp
        // live_membership: the core hosts and the tools).
        static bool& live_membership() {
            static bool on = false;
            return on;
        }
        struct membership_counters {
            uint64_t reregistered = 0;   // restored-active scripts put back into the manager's list (G1)
            uint64_t flags_kept = 0;     // next-frame flags a replayed frame left as the live frame does (F)
            uint64_t unregistered = 0;   // step RLM: a registration taken back by a later restore that wrote its script
            uint64_t settled = 0;        // step RLM: a registration taken back once its script finished in the replay
            // Step FX1: the replayed frames' changes to an owner's pending
            // count (after the frame's scripts minus before): rises (adds) and
            // falls (removes, CKScene::ActivateObject(FALSE) ->
            // RemoveObjectNextFrame).  Their net per owner is taken back by
            // the next restore of the same live frame, kept (a script of the
            // owner that restore does not write is still active), or not
            // takeable (the owner's entry left the table); those three are
            // counted in units of the net.
            uint64_t replayed_adds = 0;
            uint64_t replayed_removes = 0;
            uint64_t replayed_taken_back = 0;
            uint64_t replayed_kept = 0;
            uint64_t replayed_unrecoverable = 0;
        };
        static membership_counters& membership() {
            static membership_counters counters;
            return counters;
        }
        // Whether the live frame would process `beh`'s next-frame flags and
        // run it: its owner is in the behaviour manager's list after the next
        // ManageObjectsActivity.  Read through the public members (the retail
        // SDK exports no lookup); the pending table is walked, not hashed, so
        // the mod's template code never depends on CK2.dll's hash function.
        static bool owner_in_frame(CKContext* context, CKBehavior* beh) {
            CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
            CKBehaviorManager* manager = context ? context->GetBehaviorManager() : nullptr;
            if (!owner || !manager) return true;   // nothing to judge by: as before
            const CK_ID id = owner->GetID();
            for (auto it = manager->m_BeObjectNextFrame.Begin(); it != manager->m_BeObjectNextFrame.End(); ++it)
                if (it.GetKey() == id) {
                    if (*it > 0) return true;
                    if (*it < 0) return false;
                    break;
                }
            const XObjectPointerArray& objects = manager->m_BeObjects;
            for (int i = 0; i < objects.Size(); ++i)
                if (objects[i] == static_cast<CKObject*>(owner)) return true;
            return false;
        }
        // Whether `beh`'s owner is to join the behaviour manager's list at the
        // next frame (a pending add, not yet applied).
        static bool owner_pending_add(CKContext* context, CKBehavior* beh) {
            CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
            CKBehaviorManager* manager = context ? context->GetBehaviorManager() : nullptr;
            if (!owner || !manager) return false;
            const CK_ID id = owner->GetID();
            for (auto it = manager->m_BeObjectNextFrame.Begin(); it != manager->m_BeObjectNextFrame.End(); ++it)
                if (it.GetKey() == id) return *it > 0;
            return false;
        }
        // Step RLM (the G1 membership leak): what reregister did, so that it
        // can be taken back.  A registration is the restore's own, not the
        // live frame's: the owner's pending add (+1 in m_BeObjectNextFrame,
        // which only the next live frame's ManageObjectsActivity consumes)
        // and the scene flag it set.  Nothing in a replay consumes either, so
        // without the ledger a later restore that wrote the script back
        // finished (SimTool's chained rounds; a host frame with two rewinds)
        // found the owner still pending, and a replay that ran the script to
        // its end left the owner pending for the live frame after it, which
        // had dropped it (CKBeObject::ExecuteBehaviors -> RemoveObjectNextFrame).
        // The ledger is the live frame's: a live frame in between (the time
        // manager's main tick count moved) consumed the pending adds, and the
        // scene flags are then live state - the entries are dropped, nothing
        // taken back.
        struct registration {
            CK_ID script = 0, owner = 0;
            bool pending = false;      // the owner's pending add is this registration's
            bool scene_flag = false;   // the script's scene flag was set by it
            bool ran = false;          // the script ran in the current replayed frame
        };
        // Step FX1: the net change the replayed frames of this live frame
        // made to an owner's pending count (CKScene::Activate of a script a
        // replayed graph activated: +1 in m_BeObjectNextFrame; a replayed
        // deactivation of the object, CKScene::ActivateObject(FALSE): -1).
        // Only a live frame's ManageObjectsActivity consumes the table, so a
        // second restore of the same live frame found the first replay's adds
        // still there and owner_pending_add admitted the owner in replayed
        // frames the live world had run without it (S2's V4b alive scene: a
        // next-frame flag of P_Modul_18_Particle Script consumed that the
        // live world kept).  Signed (review of step FX1): a replay that both
        // activated and deactivated an owner left the table where it found
        // it, and nothing may then be taken from the live frame's own count.
        struct replayed_add {
            CK_ID owner = 0;
            int adds = 0;   // the net change (in owner_pending_counts: the count itself)
        };
        struct registration_ledger {
            CKDWORD frame = 0;   // the live frame the entries belong to
            std::vector<registration> entries;
            std::vector<replayed_add> replayed;   // step FX1
            void clear() {
                entries.clear();
                replayed.clear();
            }
        };
        static registration_ledger& registrations() {
            static registration_ledger ledger;
            return ledger;
        }
        static CKDWORD live_frame_id(CKContext* context) {
            CKTimeManager* time = context ? context->GetTimeManager() : nullptr;
            return time ? time->GetMainTickCount() : 0;
        }
        // The ledger as of the current live frame: emptied when a live frame
        // ran since its entries were made.
        static registration_ledger& current_registrations(CKContext* context) {
            registration_ledger& ledger = registrations();
            const CKDWORD frame = live_frame_id(context);
            if (ledger.frame != frame) {
                ledger.clear();
                ledger.frame = frame;
            }
            return ledger;
        }
        static int pending_count(CKBehaviorManager* manager, CK_ID id) {
            if (!manager) return 0;
            for (auto it = manager->m_BeObjectNextFrame.Begin(); it != manager->m_BeObjectNextFrame.End(); ++it)
                if (it.GetKey() == id) return *it;
            return 0;
        }
        // Step FX1: writes `id`'s pending count (an entry already in the
        // table; walked, not hashed, as above).  False when the owner has no
        // entry (its object was deleted: SequenceDeleted removes it).
        static bool pending_set(CKBehaviorManager* manager, CK_ID id, int count) {
            if (!manager) return false;
            for (auto it = manager->m_BeObjectNextFrame.Begin(); it != manager->m_BeObjectNextFrame.End(); ++it)
                if (it.GetKey() == id) {
                    *it = count;
                    return true;
                }
            return false;
        }
        // Takes one pending add of `id` back (RemoveObjectNextFrame's count,
        // written through the public table: a count of 0 is no change).  Only
        // a positive count is touched: a cleared table has nothing of ours.
        static bool pending_take_back(CKBehaviorManager* manager, CK_ID id) {
            if (!manager) return false;
            for (auto it = manager->m_BeObjectNextFrame.Begin(); it != manager->m_BeObjectNextFrame.End(); ++it)
                if (it.GetKey() == id) {
                    if (*it <= 0) return false;
                    *it -= 1;
                    return true;
                }
            return false;
        }
        static void take_back(CKContext* context, const registration& r, bool scene_flag) {
            if (r.pending) pending_take_back(context->GetBehaviorManager(), r.owner);
            if (scene_flag && r.scene_flag)
                if (CKScene* scene = context->GetCurrentScene())
                    if (auto* beh = CKBehavior::Cast(context->GetObject(r.script)))
                        if (scene->GetObjectFlags(beh) & CK_SCENEOBJECT_ACTIVE) scene->ModifyObjectFlags(beh, 0, CK_SCENEOBJECT_ACTIVE);
        }
        // A restore that writes `written` back: the registrations an earlier
        // restore of this live frame made for them are taken back first
        // (the pending add and the scene flag), so the restore starts from
        // the live frame's membership; reregister then registers again what
        // this restore writes back active.  A script it does not write (the
        // v14 rule's kept live state) keeps its registration.
        // Whether a script of `owner` other than those in `skip` is active or
        // to be activated: the live frame keeps an owner in its list while
        // any of its scripts executes (CKBeObject::ExecuteBehaviors drops it
        // only when none did), so a registration's pending add is the
        // owner's, not one script's.
        static bool owner_keeps(CKContext* context, CK_ID owner_id, const std::vector<CKBehavior*>* skip = nullptr) {
            auto* owner = CKBeObject::Cast(context ? context->GetObject(owner_id) : nullptr);
            if (!owner) return false;
            const int count = owner->GetScriptCount();
            for (int i = 0; i < count; ++i) {
                CKBehavior* script = owner->GetScript(i);
                if (!script || !(script->GetFlags() & (CKBEHAVIOR_ACTIVE | CKBEHAVIOR_ACTIVATENEXTFRAME))) continue;
                if (skip && std::find(skip->begin(), skip->end(), script) != skip->end()) continue;
                return true;
            }
            return false;
        }
        static uint32_t take_back_registrations(CKContext* context, const std::vector<CKBehavior*>& written) {
            registration_ledger& ledger = current_registrations(context);
            uint32_t taken = 0;
            for (auto it = ledger.entries.begin(); it != ledger.entries.end();) {
                const bool rewritten = std::any_of(written.begin(), written.end(),
                                                   [&](CKBehavior* beh) { return beh && beh->GetID() == it->script; });
                if (!rewritten) { ++it; continue; }
                // A script of the same owner this restore does not write
                // (outside the selection, or the v14 rule's kept live state)
                // still active: the owner's pending add stays with the entry
                // (review of step RLM); only the scene flag is taken back.
                if (it->pending && owner_keeps(context, it->owner, &written)) {
                    take_back(context, registration{it->script, it->owner, false, it->scene_flag, false}, true);
                    it->scene_flag = false;
                    ++it;
                    ++taken;
                    continue;
                }
                take_back(context, *it, true);
                it = ledger.entries.erase(it);
                ++taken;
            }
            return taken;
        }
        // The end of a replayed frame (live_membership): a registered script
        // that did not run in it and is neither active nor to be activated
        // has finished - the live frame would have found nothing to execute
        // for its owner and dropped it (RemoveObjectNextFrame), so its
        // pending add is taken back.  The scene flag is left: the script's
        // own end (CheckBehaviorActivity) clears it as live.  A pending add
        // is the owner's: it is held while any script of the owner ran in
        // the frame or is active or to be activated (the live rule:
        // CKBeObject::ExecuteBehaviors drops an owner only when none of its
        // scripts executed; review of step RLM).
        static uint32_t settle_registrations(CKContext* context) {
            registration_ledger& ledger = current_registrations(context);
            uint32_t settled = 0;
            for (auto it = ledger.entries.begin(); it != ledger.entries.end();) {
                auto* beh = CKBehavior::Cast(context->GetObject(it->script));
                const bool live = it->pending ? owner_keeps(context, it->owner)
                                              : beh && (beh->GetFlags() & (CKBEHAVIOR_ACTIVE | CKBEHAVIOR_ACTIVATENEXTFRAME));
                if (it->ran || live) {
                    it->ran = false;
                    ++it;
                    continue;
                }
                take_back(context, *it, false);
                it = ledger.entries.erase(it);
                ++settled;
            }
            return settled;
        }
        // A script ran in the replayed frame: its own registration, and every
        // registration that holds its owner's pending add, count it.
        static void note_ran(CKBehavior* beh) {
            if (!beh) return;
            const CK_ID script = beh->GetID();
            CKBeObject* owner = beh->GetOwner();
            const CK_ID owner_id = owner ? owner->GetID() : 0;
            for (registration& r: registrations().entries)
                if (r.script == script || (r.pending && owner_id && r.owner == owner_id)) r.ran = true;
        }
        // Step FX1: the pending count of every selected script's owner, read
        // before a replayed frame's scripts run (one entry per owner).
        static std::vector<replayed_add> owner_pending_counts(CKContext* context, const selection& sel) {
            std::vector<replayed_add> counts;
            CKBehaviorManager* manager = context ? context->GetBehaviorManager() : nullptr;
            if (!manager) return counts;
            for (CK_ID id: sel.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
                if (!owner) continue;
                const CK_ID owner_id = owner->GetID();
                if (std::any_of(counts.begin(), counts.end(), [&](const replayed_add& c) { return c.owner == owner_id; })) continue;
                counts.push_back(replayed_add{owner_id, pending_count(manager, owner_id)});
            }
            return counts;
        }
        // Step FX1: after the replayed frame's scripts, every owner whose
        // pending count changed enters the change, rise or fall, in the
        // ledger's net (the restore's own re-registrations happen before the
        // frame and settle_registrations after this, so neither is counted
        // here).
        static void note_replayed_adds(CKContext* context, const std::vector<replayed_add>& before) {
            CKBehaviorManager* manager = context ? context->GetBehaviorManager() : nullptr;
            if (!manager || before.empty()) return;
            registration_ledger& ledger = current_registrations(context);
            for (const replayed_add& b: before) {
                const int delta = pending_count(manager, b.owner) - b.adds;
                if (delta == 0) continue;
                auto it = std::find_if(ledger.replayed.begin(), ledger.replayed.end(),
                                       [&](const replayed_add& r) { return r.owner == b.owner; });
                if (it == ledger.replayed.end()) ledger.replayed.push_back(replayed_add{b.owner, delta});
                else it->adds += delta;
                if (delta > 0) membership().replayed_adds += static_cast<uint64_t>(delta);
                else membership().replayed_removes += static_cast<uint64_t>(-delta);
            }
        }
        // Step FX1: a restore of the same live frame takes the earlier
        // replays' net change back before it registers anything (reregister
        // judges the table), so its replay starts from the live frame's
        // table.  The count is written as it is minus the net, with no floor
        // (review of step FX1): the ledger is emptied whenever a live frame
        // ran (current_registrations), so no ManageObjectsActivity consumed
        // the table since the changes were made, and a count at or below 0
        // is still the live frame's own (a removal it made) plus ours.  An
        // owner with a script this restore does not write still active (the
        // v14 rule's kept live state, a script outside the selection) keeps
        // its net: that script's state is the earlier replay's, and the live
        // frame must run its owner (RLM's rule for a registration's pending
        // add).  The last replay before the next live frame is not taken back
        // at all: its adds reach that live frame, the add of a script that
        // finished inside the replay included (RLM's settle_registrations
        // gives back only a restore's own registrations, not a replayed
        // activation's add; unchanged from before FX1).
        static uint32_t take_back_replayed_adds(CKContext* context, const std::vector<CKBehavior*>& written) {
            registration_ledger& ledger = current_registrations(context);
            CKBehaviorManager* manager = context->GetBehaviorManager();
            uint32_t taken = 0;
            for (auto it = ledger.replayed.begin(); it != ledger.replayed.end();) {
                if (it->adds == 0) {
                    it = ledger.replayed.erase(it);
                    continue;
                }
                const uint64_t units = static_cast<uint64_t>(it->adds < 0 ? -it->adds : it->adds);
                if (owner_keeps(context, it->owner, &written)) {
                    membership().replayed_kept += units;
                    ++it;
                    continue;
                }
                if (pending_set(manager, it->owner, pending_count(manager, it->owner) - it->adds)) {
                    membership().replayed_taken_back += units;
                    taken += static_cast<uint32_t>(units);
                } else {
                    membership().replayed_unrecoverable += units;
                }
                it = ledger.replayed.erase(it);
            }
            return taken;
        }
        // G1: a root script the restore wrote back active (or about to be
        // activated) whose owner the live frame would not run.  Returns true
        // when it changed anything.  What it changes is entered in the
        // ledger (step RLM).
        static bool reregister(CKContext* context, CKBehavior* beh) {
            CKScene* scene = context ? context->GetCurrentScene() : nullptr;
            if (!scene || !beh) return false;
            if (!(beh->GetFlags() & (CKBEHAVIOR_ACTIVE | CKBEHAVIOR_ACTIVATENEXTFRAME))) return false;
            registration entry;
            entry.script = beh->GetID();
            CKBeObject* owner = beh->GetOwner();
            entry.owner = owner ? owner->GetID() : 0;
            bool changed = false;
            if (!(scene->GetObjectFlags(beh) & CK_SCENEOBJECT_ACTIVE)) {
                scene->ModifyObjectFlags(beh, CK_SCENEOBJECT_ACTIVE, 0);
                entry.scene_flag = true;
                changed = true;
            }
            if (!owner_in_frame(context, beh)) {
                constexpr CKDWORD next = CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME;
                const CKDWORD recorded = beh->GetFlags() & next;
                CKBehaviorManager* manager = context->GetBehaviorManager();
                const int before = owner ? pending_count(manager, entry.owner) : 0;
                scene->Activate(beh, FALSE);
                beh->ModifyFlags(recorded, next & ~recorded);
                entry.pending = owner && pending_count(manager, entry.owner) == before + 1;
                changed = true;
            }
            if (entry.scene_flag || entry.pending) current_registrations(context).entries.push_back(entry);
            return changed;
        }

        // The joint blocks of the selected scripts that hold a handle on
        // `entity`: its body is their Target or their Object2 (input 0 of
        // all four).  IVP deletes a constraint or a spring with either of
        // its bodies (IVP_Constraint_Local / IVP_Actuator delete themselves
        // when a core or an anchor goes), so when the rollback destroys
        // `entity`'s body these handles point at freed memory: the block's
        // next reset, Create (Hinge deletes a handle it finds) or teardown
        // DETACH would delete it again.  The force blocks are not here: IVP
        // only detaches their controller (PhysicsControllerForce::
        // core_is_going_to_be_deleted_event), which the block still owns.
        static std::vector<CKBehavior*> joints_on(CKContext* context, const selection& sel, const std::string& entity) {
            std::vector<CKBehavior*> out;
            if (!context || entity.empty()) return out;
            const auto named = [&entity](CKObject* object) {
                return object && object->GetName() && entity == object->GetName();
            };
            for (CK_ID id: sel.scripts) {
                auto* script = CKBehavior::Cast(context->GetObject(id));
                if (!script) continue;
                walk(script, [&](CKBehavior* beh) {
                    if (!is_joint_block(beh) || !joint_handle(beh)) return;
                    if (named(beh->GetTarget()) || (beh->GetInputParameterCount() > 0 && named(beh->GetInputParameterObject(0))))
                        out.push_back(beh);
                });
            }
            return out;
        }
        // After the body is gone: the handles IVP freed with it are nulled,
        // never deleted - nothing in IVP changes, and the block is what a
        // block with no joint is (its next Create makes one).
        static uint32_t release_joints(const std::vector<CKBehavior*>& blocks) {
            uint32_t released = 0;
            for (CKBehavior* beh: blocks) {
                void* none = nullptr;
                if (!joint_handle(beh)) continue;
                beh->SetLocalParameterValue(0, &none);
                ++released;
            }
            last_restore().joints_released += released;
            return released;
        }

        // Whether a root script is one of the mechanisms': owned by a level
        // object named P_* or PE_* - the modules, the prop balls and boxes,
        // the balloon - except the collectibles (P_Extra_*), whose scripts
        // keep score, not bodies.  The checkpoints (PC_*), the start (PS_*)
        // and the resetpoints (PR_*) are not P_*.
        static bool is_mechanism_script(CKBehavior* beh) {
            if (!beh || beh->GetParent()) return false;
            CKBeObject* owner = beh->GetOwner();
            const char* name = owner && owner->GetName() ? owner->GetName() : "";
            if (std::strncmp(name, "PE_", 3) == 0) return true;
            if (std::strncmp(name, "P_", 2) != 0) return false;
            return std::strncmp(name, "P_Extra", 7) != 0;
        }

        // The trafo scripts of Balls.nmo (session-core design, step P): the
        // explosion that physicalizes, randomizes and pushes the pieces of the
        // type the ball was (Ball_Explosion_<type>), the reset that fades,
        // de-physicalizes and restores them (Ball_ResetPieces_<type>), and the
        // wind the paper explosion and its reset switch on and off over the
        // paper pieces' force blocks (Ball_PaperWind).  Gameplay_Ingame's
        // Trafo Manager activates the first two; the explosion and the reset
        // activate the wind.  Ball_CollSound_Pieces, which the explosion also
        // activates, plays sounds only (its collision listeners read IVP and
        // write nothing): not selected.
        static bool is_trafo_script(CKBehavior* beh) {
            if (!beh || beh->GetParent() || !beh->GetName()) return false;
            return bmmo::session::is_trafo_script_name(beh->GetName());   // session/resync_rules.hpp
        }

        // The scripts that travel with the records, priority first (highest
        // first), then by id, which is the same on every call.  This is NOT
        // the order the live frame runs them in (step RO1): the behaviour
        // manager runs its active objects in m_BeObjects order, priority
        // sorted by an unstable sort over an array appended in hash order,
        // and two scripts of one priority that both make bodies in a frame
        // make them in its order - which IVP's lists and the OV tree follow.
        // A replayed frame therefore runs them in the order the host recorded
        // from its own live frame (live_frame_order, session/script_order.hpp
        // order_by_live_rank); this order is the fallback for a script that
        // frame did not run, and the record's layout.  `trafo`
        // (the session core's selection, step P): the trafo scripts too, so a
        // rollback destroys the explosion pieces born inside its window and
        // the replayed explosion makes them again, draws included; their
        // activations are recorded and re-issued (trafo_activations).  The
        // legacy retail rollback selects without them.
        static selection select(CKContext* context, bool trafo = false) {
            selection out;
            std::vector<std::pair<int, CK_ID>> picked;
            if (!context) return out;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                auto* beh = CKBehavior::Cast(context->GetObject(ids[i]));
                if (!beh) continue;
                if (is_mechanism_script(beh)) {
                    picked.emplace_back(-beh->GetOwner()->GetPriority(), beh->GetID());
                    continue;
                }
                // Gameplay_SectorManager (plan doc 10): a sector activation is
                // an input with a known tick, and the replay re-issues it; the
                // manager then has to run inside the replayed frames - its
                // delay-4 walk, the modules' Activate Script with reset, the
                // Type 2/3 Physicalize - so it travels with the records like
                // the modules do.  As a script, not a satellite: the next-frame
                // flags the activation sets are processed for scripts only.
                if (!beh->GetParent() && beh->GetName() && std::strcmp(beh->GetName(), "Gameplay_SectorManager") == 0) {
                    picked.emplace_back(beh->GetOwner() ? -beh->GetOwner()->GetPriority() : 0, beh->GetID());
                    continue;
                }
                if (trafo && is_trafo_script(beh)) {
                    picked.emplace_back(beh->GetOwner() ? -beh->GetOwner()->GetPriority() : 0, beh->GetID());
                    continue;
                }
                // No satellites (see the top of the file).
            }
            std::sort(picked.begin(), picked.end());
            std::sort(out.satellites.begin(), out.satellites.end());
            out.scripts.reserve(picked.size());
            for (const auto& [priority, id]: picked) out.scripts.push_back(id);
            return out;
        }

        // The order the live frame just run ran the selection's scripts in
        // (step RO1, session/script_order.hpp): the behaviour manager's
        // active objects in m_BeObjects order (what CKBehaviorManager::Execute
        // walked, after the frame's ManageObjectsActivity; the next-frame
        // changes wait for the next frame's), each object's scripts in its
        // script array's order, the selected ones kept.  Read through the
        // public member, not GetObject (GetObjectA under windows.h in the
        // retail SDK, an export the header names by UNICODE).  Called after a
        // live frame only: a replay never runs the behaviour manager.
        static std::vector<uint32_t> live_frame_order(CKContext* context, const selection& sel) {
            std::vector<uint32_t> out;
            CKBehaviorManager* manager = context ? context->GetBehaviorManager() : nullptr;
            if (!manager || sel.scripts.empty()) return out;
            std::vector<CK_ID> selected(sel.scripts.begin(), sel.scripts.end());
            std::sort(selected.begin(), selected.end());
            const XObjectPointerArray& objects = manager->m_BeObjects;
            for (int i = 0; i < objects.Size(); ++i) {
                auto* owner = static_cast<CKBeObject*>(objects[i]);
                if (!owner) continue;
                const int count = owner->GetScriptCount();
                for (int k = 0; k < count; ++k) {
                    CKBehavior* script = owner->GetScript(k);
                    if (!script) continue;
                    const CK_ID id = script->GetID();
                    if (std::binary_search(selected.begin(), selected.end(), id)) out.push_back(static_cast<uint32_t>(id));
                }
            }
            return out;
        }

        // Step RO1: whether the 3D entity named `entity`, or a parent of it,
        // owns a script of the selection - a body of a module whose scripts
        // the replay runs (a module is one hierarchy under its _MF root,
        // sector-placement-design.md 2.2).  The guard of the rollback's
        // destroy of a window-born fixed body (session/rollback.hpp
        // remade_by_replay): only such a body is made again by the replay.
        // Every entity of that name is asked (copies share their source's
        // name until the placement renames them).
        static bool owned_by_selection(CKContext* context, const selection& sel, const std::string& entity) {
            if (!context || entity.empty()) return false;
            std::vector<CK_ID> owners;
            for (CK_ID id: sel.scripts)
                if (auto* script = CKBehavior::Cast(context->GetObject(id)))
                    if (CKBeObject* owner = script->GetOwner()) owners.push_back(owner->GetID());
            std::sort(owners.begin(), owners.end());
            CKSTRING name = const_cast<CKSTRING>(entity.c_str());
            for (CKObject* o = context->GetObjectByNameAndParentClass(name, CKCID_3DENTITY, nullptr); o;
                 o = context->GetObjectByNameAndParentClass(name, CKCID_3DENTITY, o))
                for (auto* at = static_cast<CK3dEntity*>(o); at; at = at->GetParent())
                    if (std::binary_search(owners.begin(), owners.end(), at->GetID())) return true;
            return false;
        }

        // The scripts among a selection's that only scripts outside the
        // selection activate (step P: the trafo scripts, select's `trafo`):
        // the host records each pending activation of these after a live
        // frame and the replay re-issues it, as it does the sector manager's
        // (game/session_host_support.hpp manager_activations).  A list the
        // host keeps beside the selection, not in it: `selection` is held by
        // the legacy retail state (physics_session_state::script_ids), whose
        // layout stays as it is until R7 (design 3.2).
        static std::vector<CK_ID> trafo_activations(CKContext* context, const selection& sel) {
            std::vector<CK_ID> out;
            if (!context) return out;
            for (CK_ID id: sel.scripts)
                if (is_trafo_script(CKBehavior::Cast(context->GetObject(id)))) out.push_back(id);
            return out;
        }

        struct stats {
            int scripts = 0, active = 0, satellites = 0, blocks = 0, parameters = 0, force_blocks = 0, opaque = 0;
        };
        // The selected scripts the live frame has told to reset: a sector
        // activation resets the new sector's scripts, a death resets every
        // module of the sector (Activate Script with Reset).  The record of
        // this tick carries the pending flags and is a fine anchor - the replay
        // performs the reset as the live frame would - but every record before
        // it holds the mid-flight state the reset tears down, and must not
        // come back over it: the adapter remembers the tick per script and a
        // restore anchored earlier keeps that script live (restore's
        // `kept_live`).
        static std::vector<CK_ID> resets_pending(CKContext* context, const selection& sel) {
            std::vector<CK_ID> out;
            if (!context) return out;
            for (CK_ID id: sel.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (beh && (beh->GetFlags() & CKBEHAVIOR_RESETNEXTFRAME)) out.push_back(id);
            }
            return out;
        }

        // Diagnostics: the force blocks of the selected scripts that hold a
        // controller, with the body each pushes - all of them, or only the
        // ones on `target`: "<script>/<block>@<body> f=(..) p=(..); ..."
        static std::string describe_force_blocks(CKContext* context, const selection& sel, const script_replay_hooks& hooks,
                                                 CK_ID target = 0) {
            std::string out;
            if (!context || !hooks.capture_force) return out;
            for (CK_ID id: sel.scripts) {
                auto* script = CKBehavior::Cast(context->GetObject(id));
                if (!script) continue;
                walk(script, [&](CKBehavior* beh) {
                    if (!is_force_block(beh)) return;
                    force_block_state state;
                    if (!hooks.capture_force(beh, state) || !state.exists) return;
                    CKObject* body = beh->GetTarget();
                    if (target && (!body || body->GetID() != target)) return;
                    char text[240];
                    std::snprintf(text, sizeof(text), "%s%s/%s@%s f=(%.3f,%.3f,%.3f) p=(%.3f,%.3f,%.3f)", out.empty() ? "" : "; ",
                                  script->GetName() ? script->GetName() : "?", beh->GetName() ? beh->GetName() : "?",
                                  body && body->GetName() ? body->GetName() : "-", state.force[0], state.force[1], state.force[2],
                                  state.position[0], state.position[1], state.position[2]);
                    out += text;
                });
            }
            return out;
        }

        static stats describe(CKContext* context, const selection& sel) {
            stats s;
            for (CK_ID id: sel.scripts) {
                auto* beh = context ? CKBehavior::Cast(context->GetObject(id)) : nullptr;
                if (!beh) continue;
                ++s.scripts;
                if (beh->IsActive()) ++s.active;
                count(beh, s);
            }
            for (CK_ID id: sel.satellites) {
                auto* beh = context ? CKBehavior::Cast(context->GetObject(id)) : nullptr;
                if (!beh) continue;
                ++s.satellites;
                count(beh, s);
            }
            return s;
        }
        static std::string names(CKContext* context, const selection& sel, size_t limit = 40) {
            std::string out;
            size_t n = 0;
            for (CK_ID id: sel.scripts) {
                auto* beh = context ? CKBehavior::Cast(context->GetObject(id)) : nullptr;
                if (!beh) continue;
                if (n++ >= limit) { out += " ..."; break; }
                if (!out.empty()) out += ", ";
                out += beh->GetName() ? beh->GetName() : "?";
                if (!beh->IsActive()) out += "(off)";
            }
            for (CK_ID id: sel.satellites) {
                auto* beh = context ? CKBehavior::Cast(context->GetObject(id)) : nullptr;
                if (!beh) continue;
                CKBehavior* root = beh;
                while (root->GetParent()) root = root->GetParent();
                out += (out.empty() ? "" : ", ") + std::string("satellite ") + (beh->GetName() ? beh->GetName() : "?")
                     + " of " + (root->GetName() ? root->GetName() : "?");
            }
            return out;
        }

        // Step OPC (game/script_record_reuse.hpp): the record of t - 1 a
        // capture may copy an inactive selected root's block from, the tick
        // being captured (verify's notes) and the mode.  The rollback engine
        // passes `previous` only when nothing wrote the scripts since it was
        // captured (session/rollback.hpp capture).
        struct reuse_request {
            const std::vector<uint8_t>* previous = nullptr;
            uint32_t tick = 0;
            script_reuse_mode mode = script_reuse_mode::off;
        };

        // The runtime state of the scripts, as bytes.  Deterministic for a
        // deterministic world, so two captures of the same tick compare
        // byte for byte.
        static bool capture(CKContext* context, const selection& sel, const script_replay_hooks& hooks,
                            std::vector<uint8_t>& out, const trafo_part* trafo = nullptr, const queue_hooks* queue = nullptr,
                            const hold_hooks* hold = nullptr, const std::vector<written_column>* cells = nullptr,
                            const reuse_request* reuse = nullptr) {
            out.clear();
            if (!context) return false;
            // Step OPC: the previous record's script list, indexed by id.
            const bool reusing = reuse && reuse->mode != script_reuse_mode::off;
            script_reuse_stats& reuse_stats = script_reuse_totals();
            record_index previous_index;
            bool have_previous = false;
            if (reusing) {
                ++reuse_stats.captures;
                if (reuse->previous) {
                    have_previous = index_record(*reuse->previous, previous_index);
                    if (have_previous) ++reuse_stats.with_previous;
                    else ++reuse_stats.index_failed;
                }
            }
            // The block of `id` in the previous record (present), or null.
            // Same order as the selection unless scripts joined it: the same
            // position first, then a search.
            const auto previous_block = [&](size_t k, CK_ID id, size_t& size) -> const uint8_t* {
                const record_index::script* s = nullptr;
                if (k < previous_index.scripts.size() && previous_index.scripts[k].id == id) {
                    s = &previous_index.scripts[k];
                } else {
                    for (const record_index::script& e: previous_index.scripts)
                        if (e.id == id) { s = &e; break; }
                }
                // id (4) and present (1), then the block.
                if (!s || s->end < s->begin + 5 || (*reuse->previous)[s->begin + 4] != 1) return nullptr;
                size = s->end - s->begin - 5;
                return reuse->previous->data() + s->begin + 5;
            };
            // Sized like this thread's previous record (a tick's scripts are
            // the last tick's): the bytes are appended without the vector
            // moving them as it grows (step TR3: every member's trafo set
            // makes the record some 64 KB).
            thread_local size_t last_size = 0;
            out.reserve(last_size + last_size / 8 + 64);
            struct remember {
                std::vector<uint8_t>& bytes;
                ~remember() { last_size = bytes.size(); }
            } remember_size{out};
            const bool v3 = trafo && !trafo->empty();
            const bool with_queue = queue && queue->capture;
            const bool with_hold = hold && hold->capture;   // step G3: record v6
            const bool with_cells = cells && !cells->empty();   // step RK2: record v6
            put_u32(out, kMagic);
            if (with_hold || with_cells) {
                put_u32(out, kVersionSections);
                put_u32(out, (v3 ? kSectionTrafo : 0u) | (with_queue ? kSectionQueue : 0u) | (with_hold ? kSectionHold : 0u)
                                 | (with_cells ? kSectionCells : 0u));
            } else {
                put_u32(out, with_queue ? (v3 ? kVersionTrafoQueue : kVersionQueue) : (v3 ? kVersionTrafo : kVersion));
            }
            for (const auto* list: {&sel.scripts, &sel.satellites}) {
                put_u32(out, static_cast<uint32_t>(list->size()));
                const bool roots = list == &sel.scripts;
                for (size_t k = 0; k < list->size(); ++k) {
                    const CK_ID id = (*list)[k];
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    put_u32(out, static_cast<uint32_t>(id));
                    put_u8(out, beh ? 1 : 0);
                    if (!beh) continue;
                    if (!reusing || !roots) {
                        capture_behavior(beh, hooks, out);
                        continue;
                    }
                    // Step OPC: an inactive root whose activation words equal
                    // record t - 1's takes that record's block - step OPC2:
                    // with its force and joint tails emitted fresh.
                    size_t size = 0;
                    const uint8_t* block = have_previous ? previous_block(k, id, size) : nullptr;
                    script_root_words recorded;
                    if (!block || !read_script_root_words(block, size, recorded)) {
                        ++reuse_stats.walked_unrecorded;
                        capture_behavior(beh, hooks, out);
                        continue;
                    }
                    if (!reusable_root(recorded, root_words_of(beh), static_cast<uint32_t>(kFrameFlags))) {
                        ++reuse_stats.walked;
                        capture_behavior(beh, hooks, out);
                        continue;
                    }
                    // Step OPC2: record t - 1's block walked in step with the
                    // live graph, its header and parameter bytes copied, its
                    // force and joint tails read from physics_RT now; any
                    // structure mismatch walks the root instead.
                    const size_t at = out.size();
                    const uint64_t imaged_before = joint_stats().imaged;
                    size_t pos = static_cast<size_t>(block - reuse->previous->data());
                    const size_t end = pos + size;
                    uint64_t tails = 0;
                    if (!copy_with_fresh_tails(beh, hooks, *reuse->previous, pos, out, tails) || pos != end) {
                        out.resize(at);
                        joint_stats().imaged = imaged_before;   // the walk below images them again
                        ++reuse_stats.walked_structure;
                        capture_behavior(beh, hooks, out);
                        continue;
                    }
                    ++reuse_stats.reused;
                    reuse_stats.tails += tails;
                    if (reuse->mode == script_reuse_mode::verify) {
                        // Walked as well and compared with the copy; the copy
                        // is recorded either way (a verify run records what on
                        // would).  The walk's joint images are the copy's
                        // (the ledger's note is idempotent on one hash); its
                        // `imaged` count is taken back.
                        ++reuse_stats.verified;
                        const uint64_t imaged_copy = joint_stats().imaged;
                        const size_t mid = out.size();
                        capture_behavior(beh, hooks, out);
                        joint_stats().imaged = imaged_copy;
                        const size_t copied = mid - at, fresh = out.size() - mid;
                        if (fresh != copied || !std::equal(out.begin() + static_cast<std::ptrdiff_t>(at),
                                                           out.begin() + static_cast<std::ptrdiff_t>(mid),
                                                           out.begin() + static_cast<std::ptrdiff_t>(mid))) {
                            const size_t where = first_difference(out.data() + mid, fresh, out.data() + at, copied);
                            reuse_stats.note("script_reuse mismatch: " + owner_name(beh) + " / "
                                             + (beh->GetName() ? beh->GetName() : "") + " (#" + std::to_string(id)
                                             + ") at tick " + std::to_string(reuse->tick) + ": a fresh walk ("
                                             + std::to_string(fresh) + " bytes) differs from the copy of record t - 1's block ("
                                             + std::to_string(copied) + " bytes, " + std::to_string(tails)
                                             + " physics tails fresh) at byte " + std::to_string(where) + "; recorded the copy");
                        }
                        out.resize(mid);
                    }
                }
            }
            // The data-array cells the manager and the modules read and the
            // activation chain writes: CurrentLevel[0][4] ("Activation Phase?",
            // the modules' first-frame branch), IngameParameter[0][1] and
            // [0][2] (the sector to activate and to deactivate).
            for (int c = 0; c < kCellCount; ++c) {
                int value = 0;
                const bool present = read_cell(context, c, value);
                put_u8(out, present ? 1 : 0);
                put_u32(out, static_cast<uint32_t>(value));
            }
            if (v3) {
                put_u32(out, static_cast<uint32_t>(trafo->scripts.size()));
                for (size_t i = 0; i < trafo->scripts.size(); ++i) {
                    const CK_ID id = trafo->scripts[i];
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    put_u32(out, static_cast<uint32_t>(id));
                    if (!beh) {
                        put_u8(out, 0);
                        trafo->set_dormant(i, false);
                        continue;
                    }
                    // RC4: a script known to hold its built state is one byte,
                    // its walk (parameters, force blocks, children) skipped.
                    const auto built = trafo->built.find(id);
                    const bool has_built = built != trafo->built.end();
                    if (has_built && trafo->is_dormant(i)) {
                        if (root_activation_is(beh, built->second)) {
                            put_u8(out, kTrafoDormant);
                            ++dormancy().from_ledger;
                            continue;
                        }
                        ++dormancy().guard_trips;
                        trafo->set_dormant(i, false);
                    }
                    put_u8(out, 1);
                    const size_t at = out.size();
                    capture_behavior(beh, hooks, out);
                    const bool as_built = has_built && out.size() - at == built->second.size()
                                       && std::equal(built->second.begin(), built->second.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
                    trafo->set_dormant(i, as_built);
                    if (as_built) {
                        out.resize(at - 1);
                        put_u8(out, kTrafoDormant);
                        ++dormancy().healed;
                    } else {
                        ++dormancy().walked;
                    }
                }
            }
            if (with_queue) {
                // v4 / v5: the queued entries of the recorded scripts (gap Q).
                std::vector<queued_callback> all;
                if (!queue->capture(all)) return false;
                // The filter is queue_plan::recorded_entries (close-out step
                // S3: Q-U1 runs it over a fake queue).
                std::vector<uint32_t> scripts(sel.scripts.begin(), sel.scripts.end());
                if (v3) scripts.insert(scripts.end(), trafo->scripts.begin(), trafo->scripts.end());
                const std::vector<const queued_callback*> kept =
                    bmmo::physics::queue_plan::recorded_entries(all, std::move(scripts));
                put_u16(out, static_cast<uint16_t>(kept.size()));
                for (const queued_callback* e: kept) {
                    put_u32(out, e->behavior);
                    put_u8(out, e->slot);
                }
                queue_stats().captured += kept.size();
            }
            if (with_hold) {
                // v6: the proximity union's hold section (step G3).
                std::vector<uint8_t> section;
                hold->capture(section);
                size_t check = 0;
                if (!skip_hold(section, check) || check != section.size()) return false;
                out.insert(out.end(), section.begin(), section.end());
            }
            // Step RK2: the written-cell section (decision Q4).
            if (with_cells) put_written(context, *cells, out);
            return true;
        }

        // RC4's guard: the root's runtime flags and active inputs and outputs
        // (put_activation) equal the first words of its built block (id,
        // flags, in mask, out mask).
        static bool root_activation_is(CKBehavior* beh, const std::vector<uint8_t>& built) {
            size_t pos = 0;
            uint32_t id = 0, flags = 0, in_mask = 0, out_mask = 0;
            if (!get_u32(built, pos, id) || !get_u32(built, pos, flags) || !get_u32(built, pos, in_mask) || !get_u32(built, pos, out_mask))
                return false;
            if (id != static_cast<uint32_t>(beh->GetID()) || (static_cast<uint32_t>(beh->GetFlags()) & kRuntimeFlags) != flags) return false;
            const int inputs = beh->GetInputCount(), outputs = beh->GetOutputCount();
            uint32_t ins = 0, outs = 0;
            for (int i = 0; i < inputs && i < 32; ++i)
                if (beh->IsInputActive(i)) ins |= 1u << i;
            for (int i = 0; i < outputs && i < 32; ++i)
                if (beh->IsOutputActive(i)) outs |= 1u << i;
            return ins == in_mask && outs == out_mask;
        }

        // Step OPC: a root's activation words as put_activation writes them.
        static script_root_words root_words_of(CKBehavior* beh) {
            script_root_words w;
            w.id = static_cast<uint32_t>(beh->GetID());
            w.flags = static_cast<uint32_t>(beh->GetFlags()) & kRuntimeFlags;
            const int inputs = beh->GetInputCount(), outputs = beh->GetOutputCount();
            for (int i = 0; i < inputs && i < 32; ++i)
                if (beh->IsInputActive(i)) w.in_mask |= 1u << i;
            for (int i = 0; i < outputs && i < 32; ++i)
                if (beh->IsOutputActive(i)) w.out_mask |= 1u << i;
            return w;
        }

        // Step OPC2 (OPC's review finding 1, finished): a reused root's block
        // with its force and joint tails emitted fresh.  Those tails' bytes
        // (put_force_and_joint: the force controller's existence, position
        // and force; the joint's image hash and its place among its core's
        // controllers of one priority and name) are read from physics_RT, not
        // from the script, and can change while the script is inactive with
        // unchanged words (another script's constraint shut down, the body
        // unphysicalized) - OPC's gate round therefore walked every root with
        // such a block (~80 % of them).  Here record t - 1's block (`in` from
        // `pos`, a v2 block as capture_behavior writes it) is walked in step
        // with the live graph: each block's id must be the live block's, its
        // activation words (16 bytes with the id) and both parameter sections
        // (their counts checked against the graph) are copied, its old force
        // and joint tails skipped and put_force_and_joint re-run (exactly as
        // capture_behavior runs it, so the joint ledger is fed the same way),
        // its child count and each child's presence checked, its delayed
        // links copied (their count checked).  So the only bytes not copied
        // are the ones read now, and the result equals a walk whenever the
        // script state is record t - 1's (OPC's rule; verify compares).
        // False on any structure mismatch, with `out` partly written: the
        // caller cuts it back and walks.  `tails` counts the force and joint
        // blocks whose tails were emitted fresh (a force block whose
        // capture_force hook fails, writing a bare 0, still counts: a
        // diagnostic counter of blocks, not of tail bytes).
        // The block layout is capture_behavior's, read here and by skip_tail /
        // skip_v2_block: a change to one must be made in all three, or every
        // copy falls back to the walk (structure= rises; output stays exact).
        static bool copy_with_fresh_tails(CKBehavior* beh, const script_replay_hooks& hooks, const std::vector<uint8_t>& in,
                                          size_t& pos, std::vector<uint8_t>& out, uint64_t& tails) {
            const auto copy_from = [&](size_t from) {
                out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(from), in.begin() + static_cast<std::ptrdiff_t>(pos));
            };
            const auto parameters = [&](int live) {
                size_t peek = pos;
                uint16_t n = 0;
                return get_u16(in, peek, n) && static_cast<int>(n) == live && skip_parameters_v2(in, pos);
            };
            const size_t head = pos;
            uint32_t id = 0;
            if (!get_u32(in, pos, id) || id != static_cast<uint32_t>(beh->GetID()) || !skip(in, pos, 12)) return false;
            if (!parameters(beh->GetLocalParameterCount()) || !parameters(beh->GetOutputParameterCount())) return false;
            copy_from(head);
            // The recorded tails (skip_tail's first half), replaced.
            uint8_t has_force = 0, has_joint = 0, handle = 0;
            if (!get_u8(in, pos, has_force) || (has_force && !skip(in, pos, 49))) return false;
            if (!get_u8(in, pos, has_joint)) return false;
            if (has_joint && (!get_u8(in, pos, handle) || (handle == kJointImaged && !skip(in, pos, 8)))) return false;
            const bool force = is_force_block(beh), joint = is_joint_block(beh);
            if (force || joint) {
                put_force_and_joint(beh, hooks, out, true);
                tails += (force ? 1u : 0u) + (joint ? 1u : 0u);
            } else {
                if (has_force || has_joint) return false;   // the record had a tail this block cannot have
                put_u8(out, 0);
                put_u8(out, 0);
            }
            uint16_t children = 0, delayed = 0;
            if (!get_u16(in, pos, children)) return false;
            if (beh->IsUsingFunction()) {
                // capture_behavior's u16 0, u16 0.
                if (children != 0 || !get_u16(in, pos, delayed) || delayed != 0) return false;
                put_u16(out, 0);
                put_u16(out, 0);
                return true;
            }
            const int live = beh->GetSubBehaviorCount();
            if (static_cast<int>(children) != live) return false;
            put_u16(out, children);
            for (int i = 0; i < live; ++i) {
                CKBehavior* child = beh->GetSubBehavior(i);
                uint8_t present = 0;
                if (!get_u8(in, pos, present) || present != (child ? 1 : 0)) return false;
                put_u8(out, present);
                if (child && !copy_with_fresh_tails(child, hooks, in, pos, out, tails)) return false;
            }
            const size_t links = pos;
            if (!get_u16(in, pos, delayed) || delayed != delayed_link_count(beh) || !skip(in, pos, 2u * delayed)) return false;
            copy_from(links);
            return true;
        }

        // Whether a record skips a parameter's bytes (is_opaque below): a
        // trafo set's level reference skips the same (game/trafo_sets.hpp).
        static bool opaque_parameter(CKParameter* parameter) { return is_opaque(parameter); }

        // One script's block bytes as a record carries them (a trafo set's
        // built state, game/trafo_sets.hpp).
        static void capture_script_block(CKBehavior* script, const script_replay_hooks& hooks, std::vector<uint8_t>& out) {
            out.clear();
            if (script) capture_behavior(script, hooks, out);
        }
        // Those bytes written back into the same script - every block, IO,
        // parameter and force or joint they name, as a restore writes a
        // recorded script.  Fails, naming the place, when they do not match
        // the graph.
        static bool restore_script_block(CKBehavior* script, const script_replay_hooks& hooks,
                                         const std::vector<uint8_t>& bytes, std::string* error) {
            if (error) error->clear();
            if (!script) return fail(error, "no script");
            size_t pos = 0;
            if (!restore_behavior(script, hooks, bytes, pos, error, true)) return false;
            return pos == bytes.size() || fail(error, "trailing bytes in a script block");
        }

        static constexpr int kCellCount = 3;
        static bool cell_of(int c, const char*& array, int& row, int& column) {
            static const struct { const char* array; int row, column; } kCells[kCellCount] = {
                {"CurrentLevel", 0, 4}, {"IngameParameter", 0, 1}, {"IngameParameter", 0, 2}};
            if (c < 0 || c >= kCellCount) return false;
            array = kCells[c].array; row = kCells[c].row; column = kCells[c].column;
            return true;
        }
        static CKDataArray* find_array(CKContext* context, const char* name) {
            return CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>(name), CKCID_DATAARRAY, nullptr));
        }
        static bool read_cell(CKContext* context, int c, int& value) {
            const char* array = nullptr; int row = 0, column = 0;
            if (!cell_of(c, array, row, column)) return false;
            CKDataArray* a = find_array(context, array);
            return a && a->GetElementValue(row, column, &value);
        }
        static bool write_cell(CKContext* context, int c, int value) {
            const char* array = nullptr; int row = 0, column = 0;
            if (!cell_of(c, array, row, column)) return false;
            CKDataArray* a = find_array(context, array);
            return a && a->SetElementValue(row, column, &value, sizeof(value));
        }

        // Writes a capture back.  The walk has to meet the same graph the
        // capture saw (ids and counts); any mismatch aborts the whole restore
        // and names the place, so a half-written graph never runs.
        //
        // Only a script that was ACTIVE at the captured tick is written (and
        // listed in `replayed`, which is what execute_frame then runs).  A
        // script that was not - one the sector manager activates inside the
        // window being replayed - keeps its live state, pending activation
        // and all: the anchor's state would put it back to sleep and nobody
        // in the replay would activate it again (measured: a module script
        // activated at tick 2480 never started after a rewind across it).
        // Its bodies are on the rollback's birth branch.  So does a script in
        // `kept_live`: one the game told to reset after the anchor (see
        // resets_pending) - the anchor's mid-flight state would come back over
        // a teardown in progress (measured: a death's sector reset crossed by
        // a rewind put the level-end balloons' lift back on bodies the reset
        // was rebuilding, and the bridge flew 1800 m).
        // `replay_activation` (plan doc 10): the caller re-issues every sector
        // activation inside the replayed window, so a script inactive at the
        // anchor IS written back inactive - the replayed manager activates it
        // again, with the reset, and it re-makes its bodies, joints and forces
        // the live way.  Without it the v14 rule holds: such a script keeps its
        // live state and is not executed.
        static bool restore(CKContext* context, const selection& sel, const script_replay_hooks& hooks,
                            const std::vector<uint8_t>& in, std::string* error, std::vector<CK_ID>* replayed = nullptr,
                            const std::vector<CK_ID>* kept_live = nullptr, bool replay_activation = false,
                            const trafo_part* trafo = nullptr, const queue_hooks* queue = nullptr,
                            const hold_hooks* hold = nullptr) {
            if (error) error->clear();
            if (replayed) replayed->clear();
            last_restore() = restore_counters{};
            last_rearmed_outputs().clear();
            joints_to_order().clear();
            if (!context) return fail(error, "no context");
            size_t pos = 0;
            uint32_t magic = 0, version = 0;
            if (!get_u32(in, pos, magic) || magic != kMagic) return fail(error, "not a script snapshot");
            uint32_t sections = 0;
            if (!get_u32(in, pos, version) || !read_sections(in, pos, version, sections))
                return fail(error, "snapshot version");
            std::vector<CKBehavior*> written;   // the root scripts written back (G1, live_membership)
            // Gap Q (design 2.2.3): a selected script the record has no
            // state for that exists now runs in the replay too; it has no
            // recorded entry, so its live entries are removed.
            std::vector<CK_ID> unrecorded;
            for (const auto* list: {&sel.scripts, &sel.satellites}) {
                const bool scripts = list == &sel.scripts;
                uint32_t count = 0;
                if (!get_u32(in, pos, count) || count != list->size()) return fail(error, "script count");
                for (CK_ID id: *list) {
                    uint32_t recorded = 0;
                    uint8_t present = 0;
                    if (!get_u32(in, pos, recorded) || recorded != static_cast<uint32_t>(id)) return fail(error, "script id");
                    if (!get_u8(in, pos, present)) return fail(error, "truncated");
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    if (!present) {                    // the capture had no such script; nothing to write
                        if (scripts && replay_activation && beh) unrecorded.push_back(id);
                        continue;
                    }
                    if (!beh) return fail(error, "a recorded script is gone");
                    // Peek at the root's recorded flags: id, then flags.
                    size_t peek = pos;
                    uint32_t peek_id = 0, flags = 0;
                    if (!get_u32(in, peek, peek_id) || !get_u32(in, peek, flags)) return fail(error, "truncated root");
                    const bool was_active = (flags & (CKBEHAVIOR_ACTIVE | CKBEHAVIOR_ACTIVATENEXTFRAME)) != 0;
                    const bool reset_since = kept_live && std::find(kept_live->begin(), kept_live->end(), id) != kept_live->end();
                    const bool write = !scripts || replay_activation || (was_active && !reset_since);
                    if (!restore_behavior(beh, hooks, in, pos, error, write)) return false;
                    if (write && scripts && replayed) replayed->push_back(id);
                    if (write && scripts) written.push_back(beh);
                }
            }
            for (int c = 0; c < kCellCount; ++c) {
                uint8_t present = 0;
                uint32_t value = 0;
                if (!get_u8(in, pos, present) || !get_u32(in, pos, value)) return fail(error, "truncated cells");
                if (present && replay_activation && write_cell(context, c, static_cast<int>(value))) ++last_restore().cells_written;
            }
            // Record v3: the trafo sets' scripts (design 4.5).  Every one is
            // written, active or not: the trafo frame runs them, and what
            // activates them is the trafo inputs the replay re-applies.
            std::vector<uint32_t> recorded;
            if (sections & kSectionTrafo) {
                uint32_t count = 0;
                if (!get_u32(in, pos, count)) return fail(error, "truncated trafo scripts");
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t id = 0;
                    uint8_t present = 0;
                    if (!get_u32(in, pos, id) || !get_u8(in, pos, present)) return fail(error, "truncated trafo script");
                    recorded.push_back(id);
                    if (!present) continue;
                    auto* beh = CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(id)));
                    if (!beh) return fail(error, "a recorded trafo script is gone");
                    const std::vector<uint8_t>* built = nullptr;
                    if (trafo)
                        if (const auto b = trafo->built.find(static_cast<CK_ID>(id)); b != trafo->built.end()) built = &b->second;
                    if (present == kTrafoDormant) {
                        // RC4: the set's built state, as the capture found it.
                        if (!built) return fail(error, "a dormant trafo script without a built state");
                        if (!restore_script_block(beh, hooks, *built, error)) return false;
                        trafo->set_dormant(trafo->index_of(static_cast<CK_ID>(id)), true);
                        ++dormancy().restored_built;
                        continue;
                    }
                    if (present != 1) return fail(error, "trafo script presence");
                    const size_t at = pos;
                    if (!restore_behavior(beh, hooks, in, pos, error, true)) return false;
                    if (trafo)
                        trafo->set_dormant(trafo->index_of(static_cast<CK_ID>(id)),
                                           built && pos - at == built->size()
                                               && std::equal(built->begin(), built->end(), in.begin() + static_cast<std::ptrdiff_t>(at)));
                }
            }
            // v4 / v5: the queued entries of the recorded scripts (gap Q).
            std::vector<queued_callback> queued;
            if (sections & kSectionQueue) {
                uint16_t count = 0;
                if (!get_u16(in, pos, count)) return fail(error, "truncated queue");
                queued.resize(count);
                for (queued_callback& e: queued)
                    if (!get_u32(in, pos, e.behavior) || !get_u8(in, pos, e.slot)) return fail(error, "truncated queue entry");
            }
            // v6: the proximity union's hold section (step G3), put back
            // below when the binding passes the hooks.
            const size_t hold_at = pos;
            if ((sections & kSectionHold) && !skip_hold(in, pos)) return fail(error, "truncated proximity hold section");
            const size_t hold_end = pos;
            // Step RK2: the written-cell section (decision Q4), written back
            // below with the three cells' rule (replay_activation).
            const size_t written_at = pos;
            if ((sections & kSectionCells) && !skip_written(in, pos)) return fail(error, "truncated written-cell section");
            if (pos != in.size()) return fail(error, "trailing bytes");
            // The frames and the by-name cells back at the anchor (design
            // 4.2): a tick of the window without a candidate reads the hold.
            if ((sections & kSectionHold) && hold && hold->restore) {
                std::string why;
                if (!hold->restore(in.data() + hold_at, hold_end - hold_at, why))
                    return fail(error, ("proximity hold: " + why).c_str());
            }
            if ((sections & kSectionCells) && replay_activation && !restore_written(context, in, written_at, pos))
                return fail(error, "written-cell section");
            if (trafo && !trafo->empty()) {
                std::vector<uint32_t> selected;
                for (CK_ID id: trafo->scripts) selected.push_back(static_cast<uint32_t>(id));
                const auto plan = bmmo::session::plan_trafo_record_restore(recorded, selected, [context](uint32_t id) {
                    return CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(id))) != nullptr;
                });
                for (uint32_t id: plan.to_built) {
                    const auto built = trafo->built.find(static_cast<CK_ID>(id));
                    auto* beh = CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(id)));
                    if (built == trafo->built.end() || !beh) return fail(error, "a trafo script without a built state");
                    if (!restore_script_block(beh, hooks, built->second, error)) return false;
                    trafo->set_dormant(trafo->index_of(static_cast<CK_ID>(id)), true);
                    ++last_restore().trafo_built;
                }
            }
            // Step RLC's review: the blob matches a core's controllers by
            // priority and name, which every constraint shares, so it keeps
            // the constraints in the order the live lists have them - and a
            // constraint made again above was appended to its cores' lists.
            // The record's places go back here, before the blob (whose
            // simulation units are built from the cores' lists).  Only when a
            // place differs: the blocks holding a constraint now, in one call.
            if (!joints_to_order().empty() && hooks.capture_joint && hooks.order_joints && !joint_order_off()) {
                std::vector<std::pair<CKBehavior*, std::array<int16_t, 2>>> held;
                bool differs = false;
                for (const auto& [beh, order]: joints_to_order()) {
                    std::vector<uint8_t> image;
                    std::array<int16_t, 2> live{-1, -1};
                    if (!hooks.capture_joint(beh, image, live) || image.empty()) continue;
                    held.emplace_back(beh, order);
                    if (live != order) differs = true;
                }
                if (differs) {
                    std::string why;
                    const int moved = hooks.order_joints(held, why);
                    if (moved < 0) {
                        ++joint_stats().order_failed;
                        if (error) *error = "joint order: " + why;
                        return false;
                    }
                    last_restore().joints_reordered += static_cast<uint32_t>(moved);
                    joint_stats().reordered += static_cast<uint64_t>(moved);
                }
            }
            // Gap Q (design 2.2.3): the PreSimulate entries of every script
            // this restore wrote - the selected roots written back and every
            // trafo script (written, or put to its built state) - are the
            // record's, in its order; a written script the record lists no
            // entry for has none, and neither has a selected script absent
            // from the record.  After the blocks: a force block restored
            // above with no controller and its entry queued comes back with
            // no controller and one entry, which runs when its body appears.
            if ((sections & kSectionQueue) && queue && queue->restore) {
                // The scope and the target are queue_plan's (close-out step
                // S3: Q-U1 / Q-U2 run them over a fake queue).
                std::vector<uint32_t> ids;
                for (CKBehavior* beh: written) ids.push_back(static_cast<uint32_t>(beh->GetID()));
                for (CK_ID id: unrecorded) ids.push_back(static_cast<uint32_t>(id));
                if (trafo)
                    for (CK_ID id: trafo->scripts)
                        if (CKBehavior::Cast(context->GetObject(id))) ids.push_back(static_cast<uint32_t>(id));
                const std::vector<uint32_t> in_scope = bmmo::physics::queue_plan::rebuild_scope(std::move(ids));
                std::vector<queued_callback> target;
                uint32_t gone = 0;
                const bool found = bmmo::physics::queue_plan::rebuild_target(
                    queued, in_scope,
                    [context](uint32_t block_id, uint32_t& owner_id) {
                        auto* block = CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(block_id)));
                        if (!block) return false;
                        CKBehavior* owner = block->GetOwnerScript();
                        owner_id = owner ? static_cast<uint32_t>(owner->GetID()) : 0;
                        return true;
                    },
                    target, gone);
                if (!found) {
                    ++queue_stats().rebuild_fail;
                    return fail(error, "a queued block of the record is gone");
                }
                const std::vector<CK_ID> scope(in_scope.begin(), in_scope.end());
                std::string why;
                const int removed = queue->restore(scope, target, why);
                if (removed < 0) {
                    ++queue_stats().rebuild_fail;
                    if (error) *error = "PreSimulate queue: " + why;
                    return false;
                }
                queue_stats().removed += static_cast<uint64_t>(removed);
                queue_stats().rebuilt += target.size();
            }
            // G1 (live_membership): what the restore made active runs in the
            // live frames after the replay too.  Step RLM: an earlier
            // restore's registrations of the scripts written here are taken
            // back first - one written back finished must not find its owner
            // still pending from a rewind of the same live frame.
            if (live_membership()) {
                // Step FX1: the earlier replays' pending adds first.
                take_back_replayed_adds(context, written);
                membership().unregistered += take_back_registrations(context, written);
                for (CKBehavior* beh: written)
                    if (reregister(context, beh)) ++membership().reregistered;
            }
            return true;
        }

        // One behaviour frame of the scripts, the way the behaviour manager
        // runs it: the next-frame activation flags first, then every active
        // script's graph.  Called before the physics step of a replayed tick.
        // `trafo` (step T1): the trafo list, run first - before the
        // satellites and the scripts - the live frame's order, where the
        // trafo frame runs before the engine's whole behaviour pass,
        // Gameplay_Ingame's satellites included (trafo-replication design
        // 2.5; sim/physics_world.cpp's frame hook).
        // `live_members` (gap F, under live_membership): the selected scripts
        // whose owner the live frame of this tick ran (the host's recorded
        // live order, session/script_order.hpp); null: judged by the
        // behaviour manager's list as it is now.
        static int execute_frame(CKContext* context, const selection& sel, float delta_ms,
                                 const std::vector<CK_ID>* replayed = nullptr, const trafo_part* trafo = nullptr,
                                 const std::vector<uint32_t>* live_members = nullptr) {
            if (!context) return 0;
            int executed = 0;
            // Step FX1: the owners' pending counts before the frame's scripts.
            const std::vector<replayed_add> pending_before =
                live_membership() ? owner_pending_counts(context, sel) : std::vector<replayed_add>{};
            if (trafo) executed += execute_trafo(context, *trafo, delta_ms);
            // The satellites next, as the live frame runs Gameplay_Ingame
            // before the modules' scripts.  A block is executed the way its
            // graph's iterator executes it, and only while the graph keeps it
            // active (a continuous block stays active between frames).
            for (CK_ID id: sel.satellites) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh || !beh->IsActive()) continue;
                beh->Execute(delta_ms);
                ++executed;
            }
            executed += execute_scripts(context, replayed ? *replayed : sel.scripts, delta_ms, live_membership(), live_members);
            // Step FX1: what the frame's scripts added to the pending table.
            if (live_membership()) note_replayed_adds(context, pending_before);
            // Step RLM: a registered script that finished before this frame
            // gives its owner's pending add back, as the live frame drops it.
            if (live_membership()) membership().settled += settle_registrations(context);
            return executed;
        }

        // One behaviour frame of a list of root scripts: the next-frame flags
        // of every one first, then every active one in the list's order (the
        // selection's frame above, and the trafo frame of game/trafo_sets.hpp).
        // `live_flags` (gap F, the selection's frame under live_membership):
        // the flags of a script whose owner the live frame would not run are
        // left set, as the live frame leaves them.  Which owners it ran: the
        // live order recorded for the replayed tick (`live_members`), plus
        // every owner the replay itself has put into the list for the next
        // frame (a replayed activation, a restore's re-registration); without
        // a recorded order, the list as it is now (owner_in_frame).
        // `ledger` (RC4, execute_trafo only: `scripts` is ledger->scripts):
        // every script whose flags the frame processed or that it executed
        // leaves the dormant ledger.
        static int execute_scripts(CKContext* context, const std::vector<CK_ID>& scripts, float delta_ms,
                                   bool live_flags = false, const std::vector<uint32_t>* live_members = nullptr,
                                   const trafo_part* ledger = nullptr) {
            if (!context) return 0;
            int executed = 0;
            for (size_t i = 0; i < scripts.size(); ++i) {
                const CK_ID id = scripts[i];
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh) continue;
                const CKDWORD flags = beh->GetFlags();
                if (ledger && (flags & kFrameFlags)) ledger->set_dormant(i, false);
                if (flags & (CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME)) {
                    if (live_flags) {
                        const bool member = live_members
                            ? std::find(live_members->begin(), live_members->end(), static_cast<uint32_t>(id)) != live_members->end()
                                  || owner_pending_add(context, beh)
                            : owner_in_frame(context, beh);
                        if (!member) {
                            ++membership().flags_kept;
                            continue;
                        }
                    }
                    beh->Activate((flags & CKBEHAVIOR_ACTIVATENEXTFRAME) != 0, (flags & CKBEHAVIOR_RESETNEXTFRAME) != 0);
                    beh->ModifyFlags(0, CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME);
                }
            }
            for (size_t i = 0; i < scripts.size(); ++i) {
                auto* beh = CKBehavior::Cast(context->GetObject(scripts[i]));
                if (!beh || !beh->IsActive()) continue;
                if (ledger) ledger->set_dormant(i, false);
                if (live_flags) note_ran(beh);   // step RLM: the registration ledger
                beh->Execute(delta_ms);
                ++executed;
            }
            return executed;
        }

        // The flags that make a frame touch a root script: active, or a
        // next-frame change pending.
        static constexpr CKDWORD kFrameFlags = CKBEHAVIOR_ACTIVE | CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME
                                             | CKBEHAVIOR_RESETNEXTFRAME;

        // The trafo frame's scripts (execute_scripts over the part's list)
        // with the dormant ledger kept (RC4): a script the frame processed or
        // executed leaves it, and so does one that ends the frame active or
        // with a next-frame change (another script of the list activated it).
        static int execute_trafo(CKContext* context, const trafo_part& part, float delta_ms) {
            if (!context) return 0;
            const int executed = execute_scripts(context, part.scripts, delta_ms, false, nullptr, &part);
            for (size_t i = 0; i < part.scripts.size(); ++i)
                if (part.is_dormant(i))
                    if (auto* beh = CKBehavior::Cast(context->GetObject(part.scripts[i])); beh && (beh->GetFlags() & kFrameFlags))
                        part.set_dormant(i, false);
            return executed;
        }

        // Where two snapshots of the same scripts first differ, and how:
        // "logical" (a flag, an IO, a force block, a parameter of up to four
        // bytes - a counter, a switch, an id) or "float" (a longer parameter,
        // which is the sensors' and timers' arithmetic following the physics
        // by an ulp).  Empty when equal.  The walk needs the live graph to
        // name things; it compares bytes only.
        struct difference {
            bool any = false;
            bool logical = false;
            std::string where;   // first difference: "<script> / <block>: <field>"
            std::string detail;  // every logical difference, up to a dozen, for a trace
            int logical_count = 0, float_count = 0;
            // Step RK2: where each record's body starts (after its header)
            // and its written-cell section starts (its size when it has none).
            size_t body_a = 0, body_b = 0, cells_a = 0, cells_b = 0;
            void note(const std::string& what) {
                if (logical_count > 12) return;
                detail += (detail.empty() ? "" : "; ") + what;
            }
        };
        // `masks` (step B2SX, Q5): per block, output bits left out of the
        // compare (rearmed_output_masks: what a restore turned back on);
        // `any` is then whether anything else differs.  Only a walk that
        // read both records to their ends with nothing counted is equal: a
        // difference the walk does not count (trailing bytes, a truncated
        // record) keeps `any`.
        static difference diff(CKContext* context, const selection& sel, const std::vector<uint8_t>& a,
                               const std::vector<uint8_t>& b, const std::map<CK_ID, uint32_t>* masks = nullptr) {
            bool complete = false;
            difference d = diff_walk(context, sel, a, b, masks, complete);
            if (masks && complete && d.logical_count == 0 && d.float_count == 0 && d.where.empty()) d.any = false;
            // Step RK2 (decision Q4, "no check compares it"): two records
            // equal but for their written-cell sections (and the header words
            // the section changes) are equal.
            if (d.any && complete && d.logical_count == 0 && d.float_count == 0 && d.where.empty()
                    && d.cells_a - d.body_a == d.cells_b - d.body_b
                    && std::equal(a.begin() + static_cast<std::ptrdiff_t>(d.body_a), a.begin() + static_cast<std::ptrdiff_t>(d.cells_a),
                                  b.begin() + static_cast<std::ptrdiff_t>(d.body_b)))
                d.any = false;
            return d;
        }
        static difference diff_walk(CKContext* context, const selection& sel, const std::vector<uint8_t>& a,
                                    const std::vector<uint8_t>& b, const std::map<CK_ID, uint32_t>* masks,
                                    bool& complete) {
            complete = false;
            difference d;
            if (a == b) return d;
            d.any = true;
            size_t pa = 0, pb = 0;
            uint32_t ma = 0, mb = 0, va = 0, vb = 0;
            uint32_t sa = 0, sb = 0;
            if (!get_u32(a, pa, ma) || !get_u32(b, pb, mb) || !get_u32(a, pa, va) || !get_u32(b, pb, vb)
                    || !read_sections(a, pa, va, sa) || !read_sections(b, pb, vb, sb) || ma != mb
                    || (sa & ~kSectionCells) != (sb & ~kSectionCells)
                    || (va != vb && !(sa & kSectionCells) == !(sb & kSectionCells))) {
                // Step RK2: the written-cell section alone may differ (a
                // localized record has none; a record with it is v6 where
                // the other may be v2-v5): not a header difference.
                d.logical = true; ++d.logical_count; d.where = "header";
                return d;
            }
            d.body_a = pa;
            d.body_b = pb;
            const bool v3 = (sa & kSectionTrafo) != 0;
            for (const auto* list: {&sel.scripts, &sel.satellites}) {
                uint32_t ca = 0, cb = 0;
                if (!get_u32(a, pa, ca) || !get_u32(b, pb, cb) || ca != cb) {
                    d.logical = true; ++d.logical_count; d.where = "list count";
                    return d;
                }
                for (CK_ID id: *list) {
                    uint32_t ia = 0, ib = 0;
                    uint8_t qa = 0, qb = 0;
                    if (!get_u32(a, pa, ia) || !get_u32(b, pb, ib) || !get_u8(a, pa, qa) || !get_u8(b, pb, qb) || ia != ib || qa != qb) {
                        d.logical = true; ++d.logical_count;
                        if (d.where.empty()) d.where = "script list";
                        return d;
                    }
                    if (!qa) continue;
                    auto* beh = context ? CKBehavior::Cast(context->GetObject(id)) : nullptr;
                    const std::string script_name = beh && beh->GetName() ? beh->GetName() : std::to_string(id);
                    if (!diff_behavior(beh, script_name, a, pa, b, pb, d, masks)) return d;
                }
            }
            for (int c = 0; c < kCellCount; ++c) {
                uint8_t qa = 0, qb = 0;
                uint32_t va = 0, vb = 0;
                if (!get_u8(a, pa, qa) || !get_u8(b, pb, qb) || !get_u32(a, pa, va) || !get_u32(b, pb, vb)) break;
                if (qa != qb || va != vb) {
                    d.logical = true; ++d.logical_count;
                    const char* array = nullptr; int row = 0, column = 0;
                    cell_of(c, array, row, column);
                    const std::string what = std::string("cell ") + (array ? array : "?") + "[" + std::to_string(row) + "][" + std::to_string(column)
                                           + "] " + std::to_string(va) + " vs " + std::to_string(vb);
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                }
            }
            if (v3) {
                uint32_t ca = 0, cb = 0;
                if (!get_u32(a, pa, ca) || !get_u32(b, pb, cb) || ca != cb) {
                    d.logical = true; ++d.logical_count;
                    if (d.where.empty()) d.where = "trafo list count";
                    return d;
                }
                for (uint32_t i = 0; i < ca; ++i) {
                    uint32_t ia = 0, ib = 0;
                    uint8_t qa = 0, qb = 0;
                    if (!get_u32(a, pa, ia) || !get_u32(b, pb, ib) || !get_u8(a, pa, qa) || !get_u8(b, pb, qb) || ia != ib
                            || (qa == 0) != (qb == 0)) {
                        d.logical = true; ++d.logical_count;
                        if (d.where.empty()) d.where = "trafo list";
                        return d;
                    }
                    auto* beh = context ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(ia))) : nullptr;
                    const std::string script_name = beh && beh->GetName() ? beh->GetName() : std::to_string(ia);
                    if (qa != qb) {
                        // RC4: one record has the script at its built state,
                        // the other a block (which is not the built state).
                        d.logical = true; ++d.logical_count;
                        const std::string what = "trafo " + script_name + ": " + (qa == kTrafoDormant ? "built" : "running")
                                               + " vs " + (qb == kTrafoDormant ? "built" : "running");
                        if (d.where.empty()) d.where = what;
                        d.note(what);
                        if ((qa == 1 && !skip_v2_block(a, pa)) || (qb == 1 && !skip_v2_block(b, pb))) return d;
                        continue;
                    }
                    if (!qa || qa == kTrafoDormant) continue;
                    if (!diff_behavior(beh, script_name, a, pa, b, pb, d, masks)) return d;
                }
            }
            if (sa & kSectionQueue) {
                // Gap Q: the recorded scripts' queued entries.
                const auto read = [](const std::vector<uint8_t>& r, size_t& p, std::string& text) {
                    uint16_t n = 0;
                    if (!get_u16(r, p, n)) return false;
                    for (uint16_t i = 0; i < n; ++i) {
                        uint32_t behavior = 0;
                        uint8_t slot = 0;
                        if (!get_u32(r, p, behavior) || !get_u8(r, p, slot)) return false;
                        text += (text.empty() ? "" : " ") + std::to_string(slot) + ":" + std::to_string(behavior);
                    }
                    return true;
                };
                std::string qa, qb;
                if (!read(a, pa, qa) || !read(b, pb, qb)) return d;
                if (qa != qb) {
                    d.logical = true; ++d.logical_count;
                    const std::string what = "PreSimulate queue " + (qa.empty() ? std::string("(none)") : qa) + " vs "
                                           + (qb.empty() ? std::string("(none)") : qb);
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                }
            }
            if (sa & kSectionHold) {
                // Step G3 (design 4.2): a held by-name entity (or a count)
                // that differs is a candidate-set difference - logical.  A
                // frame's position is a copy of its candidate's position, so
                // any correction of that ball moves it: float, like the
                // sensors' arithmetic (WG3's review; the design's "logical"
                // for frames made every correcting rollback count one).
                const size_t ha = pa, hb = pb;
                if (!skip_hold(a, pa) || !skip_hold(b, pb)) return d;
                if (pa - ha != pb - hb || !std::equal(a.begin() + static_cast<std::ptrdiff_t>(ha),
                                                      a.begin() + static_cast<std::ptrdiff_t>(pa),
                                                      b.begin() + static_cast<std::ptrdiff_t>(hb))) {
                    const size_t frames = static_cast<size_t>(a[ha] | (a[ha + 1] << 8));
                    const size_t names_at = 2 + frames * 12;   // the copies' u16 count, from the section start
                    std::string what;
                    bool logical = pa - ha != pb - hb;
                    const size_t n = std::min(pa - ha, pb - hb);
                    for (size_t i = 0; i < n; ++i) {
                        if (a[ha + i] == b[hb + i]) continue;
                        if (i < 2 || (i >= names_at && i < names_at + 2)) {
                            logical = true;
                            what = "proximity hold: the counts differ";
                        } else if (i < names_at) {
                            if (what.empty()) what = "proximity hold: frame " + std::to_string((i - 2) / 12) + " differs [float]";
                            continue;
                        } else {
                            logical = true;
                            what = "proximity hold: by-name copy " + std::to_string((i - names_at - 2) / 4) + " differs";
                        }
                        break;
                    }
                    if (what.empty()) what = "proximity hold: the counts differ";
                    if (logical) { d.logical = true; ++d.logical_count; d.note(what); }
                    else ++d.float_count;
                    if (d.where.empty()) d.where = what;
                }
            }
            // Step RK2: the written-cell section is skipped, never compared
            // (decision Q4: "no check compares it").
            d.cells_a = pa;
            d.cells_b = pb;
            if ((sa & kSectionCells) && !skip_written(a, pa)) return d;
            if ((sb & kSectionCells) && !skip_written(b, pb)) return d;
            complete = pa == a.size() && pb == b.size();
            return d;
        }

        // ---- The portable record (docs/resync-and-sector-timing-design.md
        // 2.4, step A1).  The v2 record above keys every script and every
        // block by CK_ID, and an object-typed parameter's bytes are a CK_ID,
        // a Message- or attribute-typed one's an index into a registry the
        // process filled in its own order: none of it means anything in
        // another engine (the server's, the headless client's, retail
        // CK2.dll).  The portable record carries the same payload keyed by
        // names:
        //
        //   u32 magic 'BMSP', u32 version
        //   u32 script_count
        //     per script: str owner_name, str script_name, u8 present
        //       block (pre-order, sub-behaviour index order), recursively:
        //         u32 runtime_flags & kRuntimeFlags, u32 in_mask, u32 out_mask
        //         u16 locals, then per parameter one of
        //             u8 0                       opaque or empty: not written
        //             u8 1, u32 size, bytes      a value
        //             u8 2, u32 class_id, str name   an object reference: the
        //                                        object's own class; 0 and "" =
        //                                        no object, a class and "" = an
        //                                        object without a name
        //             u8 3, u8 kind, str name    a registry index by name: kind 1
        //                                        a message type, 2 an attribute type
        //             u8 4, str owner, str name  a root script (a Script-typed
        //                                        reference), by the record's own
        //                                        key: module copies share script
        //                                        names (step A1, as built)
        //         u16 outputs, the same
        //         u8 has_force [u8 exists, 6 x f64]    as v2 (object space)
        //         u8 has_joint [u8 handle_present]     as v2
        //         u16 children, per child u8 present + block
        //         u16 delayed links, u16 countdown each as v2
        //   3 x (u8 present, u32 value)    the three cells, as v2
        //   version 2 only (protocol 11, trafo-replication design 4.5/4.7,
        //   step T2): the trafo sets' scripts, keyed like the selection's -
        //   u32 count, per script: str owner_name (BMMO_TrafoOwner_<p>), str
        //   script_name (<original>_Trafo<p>), u8 present, block.  Written
        //   whenever a trafo part is passed (the world option is on), an
        //   empty part included (count 0): every record of a session without
        //   the sets stays version 1, byte for byte.
        //
        // (str: u16 length, bytes.)  A script is keyed by (owner name, script
        // name) - unique in every level's selection (design 2.4) and checked
        // at capture; a block by its walk position, validated by every count
        // the walk meets.  localize() turns a portable record into a LOCAL v2
        // record for the selection of this world, which the existing restore
        // takes unchanged.
        //   version 3 (world-agreement-gaps design 4.2 / 4.4 / 2.9, step G3,
        //   protocol 16): a sections word after the version,
        //     u32 sections: bit 0 the trafo scripts (as version 2), bit 1 the
        //     proximity union's hold section
        //   then the version 1 body byte for byte, then the trafo scripts if
        //   bit 0, then u32 size + the hold section if bit 1, keyed by name
        //   (game/proximity_union.hpp capture_portable):
        //     u16 frames, per frame str frame name, f32 x, y, z;
        //     u16 by-name copies, per copy str copy name, str the held
        //     entity's name ("" none)
        //   Written only when the hold capture is passed (the server's world
        //   under WORLD_PROXIMITY_UNION), so every other record stays version
        //   1 / 2 byte for byte.  An unknown bit is refused.
        static constexpr uint32_t kPortableMagic = 0x50534d42u;   // "BMSP"
        static constexpr uint32_t kPortableVersion = 1;
        static constexpr uint32_t kPortableVersionTrafo = 2;
        static constexpr uint32_t kPortableVersionSections = 3;
        static constexpr uint32_t kPortableSectionTrafo = 1u << 0;
        static constexpr uint32_t kPortableSectionHold = 1u << 1;
        // The hold section's halves (step G3): the server's world writes it by
        // name, a client's localizes it to its own record's hold section
        // (game/proximity_union.hpp localize_hold).
        using portable_hold_capture = std::function<void(std::vector<uint8_t>& out)>;
        using portable_hold_localize =
            std::function<bool(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error)>;

        // What capture_portable and localize met.  The lists are filled only
        // when `lists` is set (the SimTool check); each entry names the
        // script, the block path and the parameter.
        struct portable_report {
            bool lists = false;
            uint32_t scripts = 0, blocks = 0, values = 0, objects = 0, registry = 0, opaque = 0;
            // capture: object references a name alone does not find again in
            // this world - no name, or the name (within the parameter's class,
            // the recorded class first) does not name exactly this one object;
            // and references to the server's private objects (a name
            // containing "BMMO"), which a client's world does not have.
            // localize: `ambiguous` counts the references whose name names
            // more than one object (or script) here - the local value is kept
            // rather than a creation-order pick.
            uint32_t unnamed = 0, ambiguous = 0, server_private = 0;
            // capture: values copied as bytes whose type is an index into a
            // process-local table or carries ids (a class id, a parameter
            // type, an ID, a structure - whose data is its members' CK_IDs):
            // the same-world byte check cannot see them, so they are listed.
            uint32_t index_like = 0;
            // localize: references kept because the local parameter already
            // holds an object of the recorded name, resolved by name, and
            // unportable (the name or registry entry is not in this world: the
            // local value is kept); keep-local scripts copied from the anchor.
            uint32_t objects_kept = 0, objects_resolved = 0, unportable = 0, kept_local = 0;
            // localize, an input (step G3, design 2.9's player-ball reference):
            // the names this world gives the capturing world's objects - the
            // server's clone of a player -> this world's ball of that player
            // (session/resync_rules.hpp resync_renames, the blob's own list).
            // A reference whose recorded name is a key is resolved by the
            // mapped name; `objects_renamed` counts them.
            std::vector<std::pair<std::string, std::string>> renames;
            uint32_t objects_renamed = 0;
            // localize: the hold section (version 3) - 0 none in the record,
            // 1 localized into the record, 2 left out (no localizer passed:
            // the restore keeps this world's own hold).
            uint8_t hold = 0;
            std::vector<std::string> registry_list, unnamed_list, ambiguous_list, server_private_list, unportable_list,
                index_like_list;
        };
        using keep_local_fn = std::function<bool(const std::string& owner, const std::string& script)>;

        // Where each script's entry of a v2 record starts (its id) and ends,
        // and where the part after the scripts (satellites, cells) starts.
        struct record_index {
            struct script { CK_ID id = 0; size_t begin = 0, end = 0; };
            std::vector<script> scripts;
            size_t tail = 0;
        };
        static bool index_record(const std::vector<uint8_t>& record, record_index& index) {
            index = {};
            size_t a = 0;
            uint32_t magic = 0, version = 0, count = 0, sections = 0;
            if (!get_u32(record, a, magic) || magic != kMagic || !get_u32(record, a, version)
                    || !read_sections(record, a, version, sections) || !get_u32(record, a, count))
                return false;
            for (uint32_t i = 0; i < count; ++i) {
                record_index::script s;
                s.begin = a;
                uint32_t id = 0;
                uint8_t present = 0;
                if (!get_u32(record, a, id) || !get_u8(record, a, present)) return false;
                if (present && !skip_v2_block(record, a)) return false;
                s.id = static_cast<CK_ID>(id);
                s.end = a;
                index.scripts.push_back(s);
            }
            index.tail = a;
            return true;
        }

        static bool capture_portable(CKContext* context, const selection& sel, const script_replay_hooks& hooks,
                                     std::vector<uint8_t>& out, std::string* error = nullptr,
                                     portable_report* report = nullptr, const trafo_part* trafo = nullptr,
                                     const portable_hold_capture* hold = nullptr) {
            out.clear();
            if (error) error->clear();
            if (!context) return fail(error, "no context");
            if (!sel.satellites.empty()) return fail(error, "satellite blocks are not portable");
            portable_report scratch;
            portable_report& r = report ? *report : scratch;
            // Version 2 whenever a trafo part is passed (the world option is
            // on), an empty one included: a world without a selected set says
            // so, and a world holding sets the record does not list fails
            // its localize instead of putting them to their built state.
            // Version 3 (step G3) whenever the hold capture is passed, with
            // the trafo scripts as its bit 0 under the same rule.
            const bool v2 = trafo != nullptr;
            const bool with_hold = hold && *hold;
            put_u32(out, kPortableMagic);
            if (with_hold) {
                put_u32(out, kPortableVersionSections);
                put_u32(out, (v2 ? kPortableSectionTrafo : 0u) | kPortableSectionHold);
            } else {
                put_u32(out, v2 ? kPortableVersionTrafo : kPortableVersion);
            }
            std::vector<std::pair<std::string, std::string>> keys;
            const auto put_scripts = [&](const std::vector<CK_ID>& ids) {
                put_u32(out, static_cast<uint32_t>(ids.size()));
                for (CK_ID id: ids) {
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    const std::string owner = owner_name(beh), name = beh && beh->GetName() ? beh->GetName() : "";
                    put_str(out, owner);
                    put_str(out, name);
                    put_u8(out, beh ? 1 : 0);
                    if (!beh) continue;
                    if (std::find(keys.begin(), keys.end(), std::make_pair(owner, name)) != keys.end()) {
                        out.clear();
                        if (error) *error = "two selected scripts share the key " + owner + "/" + name;
                        return false;
                    }
                    keys.emplace_back(owner, name);
                    ++r.scripts;
                    capture_portable_behavior(context, beh, hooks, out, r, name, owner + "/" + name);
                }
                return true;
            };
            if (!put_scripts(sel.scripts)) return false;
            for (int c = 0; c < kCellCount; ++c) {
                int value = 0;
                const bool present = read_cell(context, c, value);
                put_u8(out, present ? 1 : 0);
                put_u32(out, static_cast<uint32_t>(value));
            }
            if (v2 && !put_scripts(trafo->scripts)) return false;
            if (with_hold) {
                std::vector<uint8_t> section;
                (*hold)(section);
                put_u32(out, static_cast<uint32_t>(section.size()));
                out.insert(out.end(), section.begin(), section.end());
            }
            return true;
        }

        // A portable record as a local v2 record of `sel` (this world's
        // selection), touching no world state.  A value is copied; an object
        // reference keeps the local bytes when the local parameter already
        // holds an object of the recorded name (every constant target), and
        // is otherwise found by name (the recorded class first, never outside
        // the parameter's class); "no object" is 0; a registry entry is found
        // by name.  What this world lacks keeps the local value and counts as
        // unportable; a name of more than one object (or script) here keeps
        // the local value and counts as ambiguous.  The scripts `keep_local`
        // names are copied from `local_anchor` (a v2 record of this world's
        // `sel`) instead.  A key in one selection and not the other, or any
        // count that differs, fails the whole call with the place named:
        // half a record never runs.  A version 2 record's trafo scripts
        // (step T2) become record v3's trafo part of `trafo` (this world's
        // trafo list, design 4.5), keyed the same way and as strictly; a
        // version 1 record with a trafo part passed gives a record without
        // one (its restore puts the sets to their built state); a version 2
        // record without one fails.
        static bool localize(CKContext* context, const selection& sel, const std::vector<uint8_t>& portable,
                             const std::vector<uint8_t>* local_anchor, const keep_local_fn& keep_local,
                             std::vector<uint8_t>& out, std::string* error = nullptr, portable_report* report = nullptr,
                             const trafo_part* trafo = nullptr, const portable_hold_localize* hold = nullptr) {
            out.clear();
            if (error) error->clear();
            if (!context) return fail(error, "no context");
            if (!sel.satellites.empty()) return fail(error, "satellite blocks are not portable");
            portable_report scratch;
            portable_report& r = report ? *report : scratch;
            r.hold = 0;
            // The portable scripts by key: where each one's block starts.
            size_t pos = 0;
            uint32_t magic = 0, version = 0, count = 0, sections = 0;
            if (!get_u32(portable, pos, magic) || magic != kPortableMagic) return fail(error, "not a portable script record");
            if (!get_u32(portable, pos, version)
                    || (version != kPortableVersion && version != kPortableVersionTrafo && version != kPortableVersionSections))
                return fail(error, "portable record version");
            if (version == kPortableVersionSections) {
                if (!get_u32(portable, pos, sections)) return fail(error, "truncated portable record");
                if (sections & ~(kPortableSectionTrafo | kPortableSectionHold)) return fail(error, "portable record sections");
            } else if (version == kPortableVersionTrafo) {
                sections = kPortableSectionTrafo;
            }
            if (!get_u32(portable, pos, count)) return fail(error, "truncated portable record");
            struct entry { std::string owner, name; size_t at = 0; bool used = false; };
            const auto read_entries = [&](uint32_t n, std::vector<entry>& list) {
                for (uint32_t i = 0; i < n; ++i) {
                    entry e;
                    uint8_t present = 0;
                    if (!get_str(portable, pos, e.owner) || !get_str(portable, pos, e.name) || !get_u8(portable, pos, present))
                        return fail(error, "truncated portable script");
                    if (!present) continue;
                    e.at = pos;
                    if (!skip_portable_block(portable, pos)) {
                        if (error) *error = "truncated portable block of " + e.owner + "/" + e.name;
                        return false;
                    }
                    list.push_back(std::move(e));
                }
                return true;
            };
            std::vector<entry> entries;
            if (!read_entries(count, entries)) return false;
            const size_t cells_at = pos;
            // Version 2: the trafo scripts after the cells.
            std::vector<entry> trafo_entries;
            size_t end_at = portable.size();
            if (cells_at + static_cast<size_t>(kCellCount) * 5 > portable.size()) return fail(error, "truncated portable cells");
            end_at = cells_at + static_cast<size_t>(kCellCount) * 5;
            if (sections & kPortableSectionTrafo) {
                pos = end_at;
                uint32_t trafo_count = 0;
                if (!get_u32(portable, pos, trafo_count)) return fail(error, "truncated portable trafo scripts");
                if (!read_entries(trafo_count, trafo_entries)) return false;
                end_at = pos;
                if (!trafo) return fail(error, "the portable record carries a trafo part and this world has no trafo list");
            }
            // Version 3: the hold section (step G3), localized by the host's
            // union into this world's record order; without a localizer it is
            // left out and the restore keeps this world's own hold.
            std::vector<uint8_t> local_hold;
            if (sections & kPortableSectionHold) {
                pos = end_at;
                uint32_t size = 0;
                if (!get_u32(portable, pos, size) || pos + size > portable.size())
                    return fail(error, "truncated portable hold section");
                if (hold && *hold) {
                    std::string why;
                    if (!(*hold)(portable.data() + pos, size, local_hold, why))
                        return fail(error, ("proximity hold: " + why).c_str());
                    r.hold = 1;
                } else {
                    r.hold = 2;
                }
                end_at = pos + size;
            }
            const bool v3 = (sections & kPortableSectionTrafo) != 0;
            const bool with_hold = r.hold == 1;
            // The local anchor's scripts by id: where each one's entry starts
            // (the id) and ends.
            record_index anchored;
            if (local_anchor && !local_anchor->empty() && !index_record(*local_anchor, anchored))
                return fail(error, "the local anchor is not a script record");
            // The local keys must be unique too: two local scripts of one key
            // would take the portable entries in this world's order.
            std::vector<std::pair<std::string, std::string>> local_keys;
            for (CK_ID id: sel.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh) continue;
                auto key = std::make_pair(owner_name(beh), std::string(beh->GetName() ? beh->GetName() : ""));
                if (std::find(local_keys.begin(), local_keys.end(), key) != local_keys.end()) {
                    if (error) *error = "two local scripts share the key " + key.first + "/" + key.second;
                    return false;
                }
                local_keys.push_back(std::move(key));
            }
            put_u32(out, kMagic);
            if (with_hold) {
                put_u32(out, kVersionSections);
                put_u32(out, (v3 ? kSectionTrafo : 0u) | kSectionHold);
            } else {
                put_u32(out, v3 ? kVersionTrafo : kVersion);
            }
            put_u32(out, static_cast<uint32_t>(sel.scripts.size()));
            for (CK_ID id: sel.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh) {                      // as capture() writes a missing script
                    put_u32(out, static_cast<uint32_t>(id));
                    put_u8(out, 0);
                    continue;
                }
                const std::string owner = owner_name(beh), name = beh->GetName() ? beh->GetName() : "";
                auto e = std::find_if(entries.begin(), entries.end(),
                                      [&](const entry& x) { return !x.used && x.owner == owner && x.name == name; });
                if (e == entries.end()) {
                    out.clear();
                    if (error) *error = "the portable record has no script " + owner + "/" + name;
                    return false;
                }
                e->used = true;
                if (keep_local && keep_local(owner, name)) {
                    const auto k = std::find_if(anchored.scripts.begin(), anchored.scripts.end(),
                                                [id](const record_index::script& x) { return x.id == id; });
                    if (k == anchored.scripts.end()) {
                        out.clear();
                        if (error) *error = "keep-local script " + owner + "/" + name + " is not in the local anchor";
                        return false;
                    }
                    out.insert(out.end(), local_anchor->begin() + static_cast<std::ptrdiff_t>(k->begin),
                               local_anchor->begin() + static_cast<std::ptrdiff_t>(k->end));
                    ++r.kept_local;
                    continue;
                }
                put_u32(out, static_cast<uint32_t>(id));
                put_u8(out, 1);
                ++r.scripts;
                size_t at = e->at;
                const std::string place = owner + "/" + name;
                if (!localize_behavior(context, beh, portable, at, out, error, r, name, place)) {
                    out.clear();
                    if (error) *error = place + ": " + *error;
                    return false;
                }
            }
            for (const entry& e: entries)
                if (!e.used) {
                    out.clear();
                    if (error) *error = "the local selection has no script " + e.owner + "/" + e.name;
                    return false;
                }
            put_u32(out, 0);                     // satellites: none (checked above)
            pos = cells_at;
            for (int c = 0; c < kCellCount; ++c)
                if (!copy_bytes(portable, pos, 5, out)) { out.clear(); return fail(error, "truncated portable cells"); }
            if (v3) {
                // Record v3's trafo part: this world's trafo list in its
                // order, each script from the entry of its key.
                put_u32(out, static_cast<uint32_t>(trafo->scripts.size()));
                for (CK_ID id: trafo->scripts) {
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    put_u32(out, static_cast<uint32_t>(id));
                    if (!beh) { put_u8(out, 0); continue; }
                    const std::string owner = owner_name(beh), name = beh->GetName() ? beh->GetName() : "";
                    auto e = std::find_if(trafo_entries.begin(), trafo_entries.end(),
                                          [&](const entry& x) { return !x.used && x.owner == owner && x.name == name; });
                    if (e == trafo_entries.end()) {
                        out.clear();
                        if (error) *error = "the portable record has no trafo script " + owner + "/" + name;
                        return false;
                    }
                    e->used = true;
                    put_u8(out, 1);
                    ++r.scripts;
                    size_t at = e->at;
                    const std::string place = owner + "/" + name;
                    const size_t block_at = out.size();
                    if (!localize_behavior(context, beh, portable, at, out, error, r, name, place)) {
                        out.clear();
                        if (error) *error = place + ": " + *error;
                        return false;
                    }
                    // RC4: a block equal to the set's built state is written
                    // as the capture writes it (one byte), so the localized
                    // record of a state is the record a capture of it gives.
                    const auto built = trafo->built.find(id);
                    if (built != trafo->built.end() && out.size() - block_at == built->second.size()
                            && std::equal(built->second.begin(), built->second.end(), out.begin() + static_cast<std::ptrdiff_t>(block_at))) {
                        out.resize(block_at - 1);
                        put_u8(out, kTrafoDormant);
                    }
                }
                for (const entry& e: trafo_entries)
                    if (!e.used) {
                        out.clear();
                        if (error) *error = "the local trafo list has no script " + e.owner + "/" + e.name;
                        return false;
                    }
            }
            // end_at: past the cells, the trafo scripts and the hold section.
            if (end_at != portable.size()) { out.clear(); return fail(error, "trailing bytes in the portable record"); }
            // Record v6's hold section (no queue section: a localized record
            // never carried one).
            if (with_hold) out.insert(out.end(), local_hold.begin(), local_hold.end());
            return true;
        }

    private:
        // A parameter whose value is arithmetic on the world - a distance, a
        // time, a pose - rather than a decision: a replay that differs from
        // the live frames by an ulp changes these without the scripts having
        // decided anything differently.  Everything else (ints, bools, ids,
        // enums, strings) is logical.
        // Every block of a graph, the way count() visits them.
        template <typename F> static void walk(CKBehavior* beh, F&& fn) {
            fn(beh);
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i)) walk(child, fn);
        }

        // A parameter of up to four bytes as an unsigned integer, for the diff
        // detail (an id, a count, a bool, an enum).
        static std::string small_value(const std::vector<uint8_t>& v, size_t at, size_t n) {
            uint32_t x = 0;
            for (size_t k = 0; k < n && k < 4 && at + k < v.size(); ++k) x |= static_cast<uint32_t>(v[at + k]) << (8 * k);
            return std::to_string(x);
        }

        static bool is_float_parameter(CKParameter* p) {
            if (!p) return false;
            const CKGUID guid = p->GetGUID();
            return guid == CKPGUID_FLOAT || guid == CKPGUID_ANGLE || guid == CKPGUID_PERCENTAGE || guid == CKPGUID_TIME
                || guid == CKPGUID_VECTOR || guid == CKPGUID_2DVECTOR || guid == CKPGUID_QUATERNION
                || guid == CKPGUID_MATRIX || guid == CKPGUID_EULERANGLES || guid == CKPGUID_RECT || guid == CKPGUID_BOX
                || guid == CKPGUID_COLOR;
        }

        static bool diff_behavior(CKBehavior* beh, const std::string& script, const std::vector<uint8_t>& a, size_t& pa,
                                  const std::vector<uint8_t>& b, size_t& pb, difference& d,
                                  const std::map<CK_ID, uint32_t>* masks = nullptr) {
            const std::string block = script + " / " + (beh && beh->GetName() ? beh->GetName() : "?");
            uint32_t xa = 0, xb = 0;
            // id, flags, inputs, outputs
            static const char* const kNames[4] = {"id", "flags", "inputs", "outputs"};
            for (int k = 0; k < 4; ++k) {
                if (!get_u32(a, pa, xa) || !get_u32(b, pb, xb)) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": truncated"; return false; }
                if (k == 3 && masks && beh)   // step B2SX (Q5): the outputs a restore re-armed
                    if (const auto m = masks->find(beh->GetID()); m != masks->end()) { xa &= ~m->second; xb &= ~m->second; }
                if (xa != xb) {
                    d.logical = true; ++d.logical_count;
                    const std::string what = block + ": " + kNames[k] + " " + std::to_string(xa) + " vs " + std::to_string(xb);
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                    if (k == 0) return false;
                }
            }
            for (int which = 0; which < 2; ++which) {
                uint16_t na = 0, nb = 0;
                if (!get_u16(a, pa, na) || !get_u16(b, pb, nb) || na != nb) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": parameter count"; return false; }
                for (int i = 0; i < na; ++i) {
                    uint8_t ka = 0, kb = 0;
                    if (!get_u8(a, pa, ka) || !get_u8(b, pb, kb) || ka != kb) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": parameter kind"; return false; }
                    if (!ka) continue;
                    uint32_t sa = 0, sb = 0;
                    if (!get_u32(a, pa, sa) || !get_u32(b, pb, sb) || sa != sb || pa + sa > a.size() || pb + sb > b.size()) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": parameter size"; return false; }
                    if (std::memcmp(a.data() + pa, b.data() + pb, sa) != 0) {
                        CKParameter* p = nullptr;
                        if (beh) p = which == 0 ? static_cast<CKParameter*>(beh->GetLocalParameter(i)) : static_cast<CKParameter*>(beh->GetOutputParameter(i));
                        const bool logical = !is_float_parameter(p);
                        const bool was_logical = d.logical;
                        if (logical) { d.logical = true; ++d.logical_count; } else ++d.float_count;
                        const std::string what = block + ": " + (which == 0 ? "local " : "output ") + std::to_string(i)
                                               + (p && p->GetName() ? std::string(" (") + p->GetName() + ")" : std::string())
                                               + (logical ? " [logical]" : " [float]")
                                               + (logical && sa <= 4 ? " " + small_value(a, pa, sa) + " vs " + small_value(b, pb, sa) : std::string());
                        if (d.where.empty() || (logical && !was_logical)) d.where = what;
                        if (logical) d.note(what);
                    }
                    pa += sa; pb += sb;
                }
            }
            uint8_t fa = 0, fb = 0;
            if (!get_u8(a, pa, fa) || !get_u8(b, pb, fb) || fa != fb) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": force presence"; return false; }
            if (fa) {
                uint8_t ea = 0, eb = 0;
                if (!get_u8(a, pa, ea) || !get_u8(b, pb, eb)) return false;
                if (ea != eb) {
                    d.logical = true; ++d.logical_count;
                    const std::string what = block + ": force controller " + std::to_string(ea) + " vs " + std::to_string(eb);
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                }
                for (int k = 0; k < 6; ++k) {
                    double da = 0, db = 0;
                    if (!get_f64(a, pa, da) || !get_f64(b, pb, db)) return false;
                    if (da != db) { ++d.float_count; if (d.where.empty()) d.where = block + ": force vector [float]"; }
                }
            }
            uint8_t ja = 0, jb = 0;
            if (!get_u8(a, pa, ja) || !get_u8(b, pb, jb) || ja != jb) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": joint presence"; return false; }
            if (ja) {
                uint8_t ha = 0, hb = 0;
                if (!get_u8(a, pa, ha) || !get_u8(b, pb, hb)) return false;
                uint32_t ia = 0, ib = 0;
                uint16_t oa[2] = {0xffffu, 0xffffu}, ob[2] = {0xffffu, 0xffffu};
                if ((ha == kJointImaged && (!get_u32(a, pa, ia) || !get_u16(a, pa, oa[0]) || !get_u16(a, pa, oa[1])))
                    || (hb == kJointImaged && (!get_u32(b, pb, ib) || !get_u16(b, pb, ob[0]) || !get_u16(b, pb, ob[1]))))
                    return false;
                if ((ha != 0) != (hb != 0)) {
                    d.logical = true; ++d.logical_count;
                    const std::string what = block + ": joint handle " + std::to_string(ha != 0) + " vs " + std::to_string(hb != 0);
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                } else if (ia && ib && ia != ib) {
                    // Step RLC: two constraints made from different poses.
                    d.logical = true; ++d.logical_count;
                    const std::string what = block + ": joint image " + std::to_string(ia) + " vs " + std::to_string(ib);
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                } else if (ia && ib && (oa[0] != ob[0] || oa[1] != ob[1])) {
                    // Step RLC's review: the same constraint at another place
                    // among its cores' constraints (another solve order).
                    d.logical = true; ++d.logical_count;
                    const std::string what = block + ": joint order " + std::to_string(static_cast<int16_t>(oa[0])) + "/"
                        + std::to_string(static_cast<int16_t>(oa[1])) + " vs " + std::to_string(static_cast<int16_t>(ob[0])) + "/"
                        + std::to_string(static_cast<int16_t>(ob[1]));
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                }
            }
            uint16_t ca = 0, cb = 0;
            if (!get_u16(a, pa, ca) || !get_u16(b, pb, cb) || ca != cb) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": children"; return false; }
            for (int i = 0; i < ca; ++i) {
                uint8_t qa = 0, qb = 0;
                if (!get_u8(a, pa, qa) || !get_u8(b, pb, qb) || qa != qb) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": child presence"; return false; }
                if (!qa) continue;
                CKBehavior* child = beh ? beh->GetSubBehavior(i) : nullptr;
                if (!diff_behavior(child, script, a, pa, b, pb, d, masks)) return false;
            }
            uint16_t la = 0, lb = 0;
            if (!get_u16(a, pa, la) || !get_u16(b, pb, lb) || la != lb) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": delayed link count"; return false; }
            for (int i = 0; i < la; ++i) {
                uint16_t da = 0, db = 0;
                if (!get_u16(a, pa, da) || !get_u16(b, pb, db)) return false;
                if (da != db) {
                    d.logical = true; ++d.logical_count;
                    const std::string what = block + ": delayed link " + std::to_string(i) + " countdown " + std::to_string(da) + " vs " + std::to_string(db);
                    if (d.where.empty()) d.where = what;
                    d.note(what);
                }
            }
            return true;
        }

        // A parameter whose bytes are not state: a raw pointer into the
        // physics engine (the IVP_Handle of the physics blocks), or a
        // container of object pointers.
        static bool is_opaque(CKParameter* parameter) {
            if (!parameter) return true;
            const CKGUID guid = parameter->GetGUID();
            // A 2D curve (a block's progression curve) serializes with bytes
            // that differ from one process to the next; it is an input, never
            // state, and with it in the capture two runs' scripts never agree.
            return guid == CKPGUID_POINTER || guid == CKPGUID_OBJECTARRAY || guid == CKPGUID_VOIDBUF
                || guid == CKPGUID_2DCURVE;
        }

        static void count(CKBehavior* beh, stats& s) {
            ++s.blocks;
            if (is_force_block(beh)) ++s.force_blocks;
            const int locals = beh->GetLocalParameterCount();
            for (int i = 0; i < locals; ++i) {
                if (is_opaque(beh->GetLocalParameter(i))) ++s.opaque; else ++s.parameters;
            }
            const int outs = beh->GetOutputParameterCount();
            for (int i = 0; i < outs; ++i) {
                if (is_opaque(beh->GetOutputParameter(i))) ++s.opaque; else ++s.parameters;
            }
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i)) count(child, s);
        }

        static void capture_parameter(CKParameter* parameter, std::vector<uint8_t>& out) {
            if (is_opaque(parameter)) { put_u8(out, 0); return; }
            const int size = parameter->GetDataSize();
            const void* data = size > 0 ? parameter->GetReadDataPtr(FALSE) : nullptr;
            if (size <= 0 || !data) { put_u8(out, 0); return; }
            put_u8(out, 1);
            put_u32(out, static_cast<uint32_t>(size));
            const auto* bytes = static_cast<const uint8_t*>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        static bool restore_parameter(CKParameter* parameter, const std::vector<uint8_t>& in, size_t& pos, std::string* error) {
            uint8_t kind = 0;
            if (!get_u8(in, pos, kind)) return fail(error, "truncated parameter");
            if (kind == 0) return true;
            uint32_t size = 0;
            if (!get_u32(in, pos, size) || pos + size > in.size()) return fail(error, "truncated parameter bytes");
            // A size the parameter does not have now is a graph the capture
            // did not see: leave the value alone rather than write past it -
            // except a string, which changes length with what it holds (step
            // RO1, resize_parameter below).
            //
            // Written through the buffer, never with SetValue: on an output
            // parameter SetValue ends in DataChanged(), which copies the value
            // into every parameter linked FROM it (CKParameterOut's
            // destinations - a graph's local fed by a child block's output,
            // the way Ballance's module scripts keep their permission flags).
            // Those destinations have their own recorded values, written a
            // moment earlier or later in this walk; a propagated stale output
            // overwrote them (measured: P_Modul_18's fan came out of every
            // replay with TurnON_Permission 0 where the live frames had 1, from
            // the Identity block's parked output, and never lifted the paper
            // ball again).  The live frame propagates only when the block
            // executes, and the replayed frames do that themselves.
            if (parameter && !is_opaque(parameter)) {
                if (parameter->GetDataSize() == static_cast<int>(size)) {
                    if (void* buffer = parameter->GetWriteDataPtr()) std::memcpy(buffer, in.data() + pos, size);
                } else if (size > 0 && resize_strings() && parameter->GetGUID() == CKPGUID_STRING) {
                    if (resize_parameter(parameter, in.data() + pos, static_cast<int>(size))) ++last_restore().strings_resized;
                }
            }
            pos += size;
            return true;
        }

        // Step RO1: a parameter whose size changed since the record (a
        // string: the sector manager's Activate Sector pins and Groupname,
        // Get Key Row's Group Name, which change length with the sector a
        // pass activates) written back at the record's size, still without
        // propagation.  Only SetValue resizes, and CKParameterOut's ends in
        // DataChanged (every destination gets a copy - see above), so an
        // output's destination list is swapped out around the call (an
        // XSArray swap: no allocation, and the buffer is resized by CK's own
        // SetValue in CK's own heap) and swapped back.  The base
        // CKParameter::SetValue is not exported by the retail CK2.dll (only
        // reachable through the vtable), so a qualified call would not link
        // there.  A local or a plain parameter's SetValue does not propagate.
        // Any other class is left alone, as before.
        struct parameter_out_access : CKParameterOut {
            static XSObjectPointerArray& destinations(CKParameterOut* out) {
                return out->*(&parameter_out_access::m_Destinations);
            }
        };
        static bool resize_parameter(CKParameter* parameter, const void* data, int size) {
            const CK_CLASSID cid = parameter->GetClassID();
            if (cid == CKCID_PARAMETEROUT) {
                auto* out = static_cast<CKParameterOut*>(parameter);
                XSObjectPointerArray held;
                parameter_out_access::destinations(out).Swap(held);
                const CKERROR e = out->SetValue(data, size);
                parameter_out_access::destinations(out).Swap(held);
                return e == CK_OK;
            }
            if (cid == CKCID_PARAMETERLOCAL || cid == CKCID_PARAMETER) return parameter->SetValue(data, size) == CK_OK;
            return false;
        }

        // The v2 block layout.  Its other readers: skip_v2_block / skip_tail
        // (stepping over a block) and copy_with_fresh_tails (step OPC2's
        // reuse copy, which re-reads this layout field by field) - change
        // them together.
        static void capture_behavior(CKBehavior* beh, const script_replay_hooks& hooks, std::vector<uint8_t>& out) {
            put_u32(out, static_cast<uint32_t>(beh->GetID()));
            put_activation(beh, out);
            const int locals = beh->GetLocalParameterCount();
            put_u16(out, static_cast<uint16_t>(locals));
            for (int i = 0; i < locals; ++i) capture_parameter(beh->GetLocalParameter(i), out);
            const int outs = beh->GetOutputParameterCount();
            put_u16(out, static_cast<uint16_t>(outs));
            for (int i = 0; i < outs; ++i) capture_parameter(beh->GetOutputParameter(i), out);
            put_force_and_joint(beh, hooks, out, true);
            if (beh->IsUsingFunction()) { put_u16(out, 0); put_u16(out, 0); return; }
            const int children = beh->GetSubBehaviorCount();
            put_u16(out, static_cast<uint16_t>(children));
            for (int i = 0; i < children; ++i) {
                CKBehavior* child = beh->GetSubBehavior(i);
                put_u8(out, child ? 1 : 0);
                if (child) capture_behavior(child, hooks, out);
            }
            put_delayed_links(beh, out);
        }

        // The runtime flags and the active inputs and outputs (v2 and the
        // portable record alike).
        static void put_activation(CKBehavior* beh, std::vector<uint8_t>& out) {
            put_u32(out, static_cast<uint32_t>(beh->GetFlags()) & kRuntimeFlags);
            const int inputs = beh->GetInputCount(), outputs = beh->GetOutputCount();
            uint32_t in_mask = 0, out_mask = 0;
            // Through the behaviour's own accessors: the IO objects' Activate /
            // IsActive are inline in the retail SDK and not exported, these are.
            for (int i = 0; i < inputs && i < 32; ++i)
                if (beh->IsInputActive(i)) in_mask |= 1u << i;
            for (int i = 0; i < outputs && i < 32; ++i)
                if (beh->IsOutputActive(i)) out_mask |= 1u << i;
            put_u32(out, in_mask);
            put_u32(out, out_mask);
        }

        // The force block's controller and the joint block's handle presence.
        // `local`: this process's record, whose handle byte may name the
        // constraint's image (kJointImaged + u32 hash, step RLC); the
        // portable record keeps v2's presence byte.
        static void put_force_and_joint(CKBehavior* beh, const script_replay_hooks& hooks, std::vector<uint8_t>& out,
                                        bool local) {
            force_block_state force;
            if (is_force_block(beh) && hooks.capture_force && hooks.capture_force(beh, force)) {
                put_u8(out, 1);
                put_u8(out, force.exists ? 1 : 0);
                for (int k = 0; k < 3; ++k) put_f64(out, force.position[k]);
                for (int k = 0; k < 3; ++k) put_f64(out, force.force[k]);
            } else {
                put_u8(out, 0);
            }
            if (is_joint_block(beh)) {
                put_u8(out, 1);
                const bool held = joint_handle(beh) != nullptr;
                std::vector<uint8_t> image;
                std::array<int16_t, 2> order{-1, -1};
                if (held && local && hooks.capture_joint && hooks.capture_joint(beh, image, order) && !image.empty()) {
                    const uint32_t hash = joint_image_hash(image);
                    joints().note(beh->GetID(), hash, image);
                    ++joint_stats().imaged;
                    put_u8(out, kJointImaged);
                    put_u32(out, hash);
                    put_u16(out, static_cast<uint16_t>(order[0]));
                    put_u16(out, static_cast<uint16_t>(order[1]));
                } else {
                    put_u8(out, held ? 1 : 0);
                }
            } else {
                put_u8(out, 0);
            }
        }

        // The graph's delayed links (initial delay above one frame): a link in
        // flight between frames lives in the graph's own list with a
        // countdown, which the SDK does not expose - but the countdown is on
        // the link, and a link is in flight exactly when its delay is between
        // zero and its initial value (queued: set to the initial and counted
        // down each frame; fired or reset: zero or initial).
        static uint16_t delayed_link_count(CKBehavior* beh) {
            uint16_t delayed = 0;
            const int links = beh->IsUsingFunction() ? 0 : beh->GetSubBehaviorLinkCount();
            for (int i = 0; i < links; ++i)
                if (CKBehaviorLink* link = beh->GetSubBehaviorLink(i); link && link->GetInitialActivationDelay() > 1) ++delayed;
            return delayed;
        }
        static void put_delayed_links(CKBehavior* beh, std::vector<uint8_t>& out) {
            put_u16(out, delayed_link_count(beh));
            const int links = beh->GetSubBehaviorLinkCount();
            for (int i = 0; i < links; ++i) {
                CKBehaviorLink* link = beh->GetSubBehaviorLink(i);
                if (!link || link->GetInitialActivationDelay() <= 1) continue;
                put_u16(out, static_cast<uint16_t>(std::max(0, std::min(65535, link->GetActivationDelay()))));
            }
        }

        // ---- the portable record's walk (see capture_portable)

        static std::string owner_name(CKBehavior* beh) {
            CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
            return owner && owner->GetName() ? owner->GetName() : "";
        }
        // Registry-typed parameters: 1 a message type, 2 an attribute type.
        static int registry_kind(CKParameter* parameter) {
            const CKGUID guid = parameter->GetGUID();
            if (guid == CKPGUID_MESSAGE) return 1;
            if (guid == CKPGUID_ATTRIBUTE) return 2;
            return 0;
        }
        static const char* registry_name(CKContext* context, int kind, int index) {
            if (index < 0) return nullptr;
            if (kind == 1) {
                CKMessageManager* messages = context->GetMessageManager();
                return messages && index < messages->GetMessageTypeCount() ? messages->GetMessageTypeName(index) : nullptr;
            }
            CKAttributeManager* attributes = context->GetAttributeManager();
            return attributes && index < attributes->GetAttributeCount() ? attributes->GetAttributeNameByType(index) : nullptr;
        }
        static bool registry_index(CKContext* context, int kind, const std::string& name, int& index) {
            if (kind == 1) {
                CKMessageManager* messages = context->GetMessageManager();
                const int count = messages ? messages->GetMessageTypeCount() : 0;
                for (int i = 0; i < count; ++i)
                    if (const char* n = messages->GetMessageTypeName(i); n && name == n) { index = i; return true; }
                return false;
            }
            if (kind != 2) return false;
            CKAttributeManager* attributes = context->GetAttributeManager();
            if (!attributes) return false;
            index = attributes->GetAttributeTypeByName(const_cast<CKSTRING>(name.c_str()));
            return index >= 0;
        }
        // The object of that name a portable reference means here, among the
        // objects of the parameter's class (derived classes included): the
        // one of the recorded class, else the one of another class.
        // `matches` gets how many candidates the tier used has; anything but
        // 1 returns null - the first of several is this world's creation
        // order, not the reference.  An object outside the parameter's class
        // is never a candidate.
        static CKObject* find_by_name(CKContext* context, const std::string& name, CK_CLASSID parameter_class,
                                      CK_CLASSID recorded_class, int* matches = nullptr) {
            CKSTRING n = const_cast<CKSTRING>(name.c_str());
            CKObject* same = nullptr;
            CKObject* other = nullptr;
            int same_count = 0, other_count = 0;
            for (CKObject* o = context->GetObjectByNameAndParentClass(n, parameter_class, nullptr); o;
                 o = context->GetObjectByNameAndParentClass(n, parameter_class, o)) {
                if (o->GetClassID() == recorded_class) { if (!same) same = o; ++same_count; }
                else { if (!other) other = o; ++other_count; }
            }
            const int count = same_count ? same_count : other_count;
            if (matches) *matches = count;
            return count == 1 ? (same ? same : other) : nullptr;
        }
        // The root script of that key (owner name, script name); `matches`
        // gets how many there are (the key is unique in every level's
        // selection, but a reference may name any script).
        static CKBehavior* find_script(CKContext* context, const std::string& owner, const std::string& name, int* matches = nullptr) {
            CKSTRING n = const_cast<CKSTRING>(name.c_str());
            CKBehavior* found = nullptr;
            int count = 0;
            for (CKObject* o = context->GetObjectByNameAndParentClass(n, CKCID_BEHAVIOR, nullptr); o;
                 o = context->GetObjectByNameAndParentClass(n, CKCID_BEHAVIOR, o)) {
                auto* beh = CKBehavior::Cast(o);
                if (!beh || beh->GetParent() || owner_name(beh) != owner) continue;
                if (!found) found = beh;
                ++count;
            }
            if (matches) *matches = count;
            return found;
        }
        static std::string parameter_place(const std::string& block, bool output, int index, CKParameter* parameter) {
            return block + ": " + (output ? "output " : "local ") + std::to_string(index)
                 + (parameter && parameter->GetName() ? std::string(" (") + parameter->GetName() + ")" : std::string());
        }
        static void note(portable_report& r, std::vector<std::string>& list, const std::string& what) {
            if (r.lists) list.push_back(what);
        }

        static void capture_portable_parameter(CKContext* context, CKParameter* parameter, std::vector<uint8_t>& out,
                                               portable_report& r, const std::string& block, bool output, int index) {
            if (is_opaque(parameter)) { put_u8(out, 0); ++r.opaque; return; }
            const int size = parameter->GetDataSize();
            const void* data = size > 0 ? parameter->GetReadDataPtr(FALSE) : nullptr;
            if (size <= 0 || !data) { put_u8(out, 0); return; }
            const CK_CLASSID parameter_class = parameter->GetParameterClassID();
            if (parameter_class != 0 && size == static_cast<int>(sizeof(CK_ID))) {
                CK_ID id = 0;
                std::memcpy(&id, data, sizeof(id));
                CKObject* object = id ? context->GetObject(id) : nullptr;
                const std::string name = object && object->GetName() ? object->GetName() : "";
                if (auto* script = object && !name.empty() ? CKBehavior::Cast(object) : nullptr; script && !script->GetParent()) {
                    const std::string owner = owner_name(script);
                    put_u8(out, 4);
                    put_str(out, owner);
                    put_str(out, name);
                    ++r.objects;
                    if (owner.find("BMMO") != std::string::npos || name.find("BMMO") != std::string::npos) {
                        ++r.server_private;
                        note(r, r.server_private_list, parameter_place(block, output, index, parameter) + " -> " + owner + "/" + name);
                    }
                    int matches = 0;
                    if (find_script(context, owner, name, &matches) != script || matches != 1) {
                        ++r.ambiguous;
                        note(r, r.ambiguous_list, parameter_place(block, output, index, parameter) + " -> script " + owner + "/" + name);
                    }
                    return;
                }
                put_u8(out, 2);
                put_u32(out, object ? static_cast<uint32_t>(object->GetClassID()) : 0u);
                put_str(out, name);
                ++r.objects;
                if (object && name.empty()) {
                    ++r.unnamed;
                    note(r, r.unnamed_list, parameter_place(block, output, index, parameter));
                } else if (object) {
                    if (name.find("BMMO") != std::string::npos) {
                        ++r.server_private;
                        note(r, r.server_private_list, parameter_place(block, output, index, parameter) + " -> " + name);
                    }
                    if (find_by_name(context, name, parameter_class, object->GetClassID()) != object) {   // null unless unique
                        ++r.ambiguous;
                        note(r, r.ambiguous_list, parameter_place(block, output, index, parameter) + " -> " + name);
                    }
                }
                return;
            }
            if (const int kind = registry_kind(parameter); kind && size == 4) {
                int value = 0;
                std::memcpy(&value, data, sizeof(value));
                if (const char* name = registry_name(context, kind, value); name && *name) {
                    put_u8(out, 3);
                    put_u8(out, static_cast<uint8_t>(kind));
                    put_str(out, name);
                    ++r.registry;
                    if (r.lists)
                        note(r, r.registry_list, parameter_place(block, output, index, parameter) + (kind == 1 ? " message " : " attribute ")
                                                 + std::to_string(value) + " (" + name + ")");
                    return;
                }
            }
            if (const char* what = index_like_type(context, parameter)) {
                ++r.index_like;
                note(r, r.index_like_list, parameter_place(block, output, index, parameter) + " " + what);
            }
            put_u8(out, 1);
            put_u32(out, static_cast<uint32_t>(size));
            const auto* bytes = static_cast<const uint8_t*>(data);
            out.insert(out.end(), bytes, bytes + size);
            ++r.values;
        }
        // A value type whose bytes mean something only in this process: a
        // class id or a parameter type (registered in plugin load order), an
        // ID, a structure (its data is its member parameters' CK_IDs).
        static const char* index_like_type(CKContext* context, CKParameter* parameter) {
            const CKGUID guid = parameter->GetGUID();
            if (guid == CKPGUID_CLASSID) return "class id";
            if (guid == CKPGUID_PARAMETERTYPE) return "parameter type";
            if (guid == CKPGUID_ID) return "ID";
            CKParameterManager* pm = context->GetParameterManager();
            CKParameterTypeDesc* desc = pm ? pm->GetParameterTypeDescription(parameter->GetType()) : nullptr;
            if (desc && (desc->dwFlags & CKPARAMETERTYPE_STRUCT)) return "structure";
            return nullptr;
        }

        static std::string child_place(const portable_report& r, const std::string& block, int index, CKBehavior* child) {
            if (!r.lists) return std::string();
            return block + "/" + std::to_string(index) + ":" + (child->GetName() ? child->GetName() : "?");
        }

        static void capture_portable_behavior(CKContext* context, CKBehavior* beh, const script_replay_hooks& hooks,
                                              std::vector<uint8_t>& out, portable_report& r, const std::string& script,
                                              const std::string& block) {
            ++r.blocks;
            put_activation(beh, out);
            const int locals = beh->GetLocalParameterCount();
            put_u16(out, static_cast<uint16_t>(locals));
            for (int i = 0; i < locals; ++i) capture_portable_parameter(context, beh->GetLocalParameter(i), out, r, block, false, i);
            const int outs = beh->GetOutputParameterCount();
            put_u16(out, static_cast<uint16_t>(outs));
            for (int i = 0; i < outs; ++i) capture_portable_parameter(context, beh->GetOutputParameter(i), out, r, block, true, i);
            put_force_and_joint(beh, hooks, out, false);
            if (beh->IsUsingFunction()) { put_u16(out, 0); put_u16(out, 0); return; }
            const int children = beh->GetSubBehaviorCount();
            put_u16(out, static_cast<uint16_t>(children));
            for (int i = 0; i < children; ++i) {
                CKBehavior* child = beh->GetSubBehavior(i);
                put_u8(out, child ? 1 : 0);
                if (child) capture_portable_behavior(context, child, hooks, out, r, script, child_place(r, block, i, child));
            }
            put_delayed_links(beh, out);
        }

        // One portable parameter as the local v2 parameter (localize).
        static bool localize_parameter(CKContext* context, CKParameter* local, const std::vector<uint8_t>& in, size_t& pos,
                                       std::vector<uint8_t>& out, std::string* error, portable_report& r,
                                       const std::string& block, bool output, int index) {
            uint8_t kind = 0;
            if (!get_u8(in, pos, kind)) return fail(error, "truncated parameter");
            if (kind == 0) { put_u8(out, 0); return true; }
            if (kind == 1) {
                uint32_t size = 0;
                if (!get_u32(in, pos, size) || pos + size > in.size()) return fail(error, "truncated parameter bytes");
                put_u8(out, 1);
                put_u32(out, size);
                out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + size));
                pos += size;
                ++r.values;
                return true;
            }
            // A reference: the local parameter has to be one that holds it.
            const int size = local && !is_opaque(local) ? local->GetDataSize() : 0;
            const void* data = size == 4 ? local->GetReadDataPtr(FALSE) : nullptr;
            uint32_t local_value = 0;
            if (data) std::memcpy(&local_value, data, sizeof(local_value));
            const auto emit = [&out](uint32_t value) { put_u8(out, 1); put_u32(out, 4); put_u32(out, value); };
            if (kind == 2) {
                uint32_t recorded_class = 0;
                std::string name;
                if (!get_u32(in, pos, recorded_class) || !get_str(in, pos, name)) return fail(error, "truncated object reference");
                if (!data || local->GetParameterClassID() == 0) return fail(error, ("an object reference on a parameter that holds none at " + parameter_place(block, output, index, local)).c_str());
                ++r.objects;
                // Step G3: a reference to the capturing world's ball of a
                // player (the server's clone) means this world's ball of that
                // player (a latched Get Cell output, design 2.9).
                if (!name.empty())
                    for (const auto& [from, to]: r.renames)
                        if (from == name) {
                            name = to;
                            ++r.objects_renamed;
                            break;
                        }
                CKObject* current = local_value ? context->GetObject(static_cast<CK_ID>(local_value)) : nullptr;
                if (recorded_class == 0 && name.empty()) {          // no object: 0, never a stale local id
                    emit(0);
                    if (local_value) ++r.objects_resolved; else ++r.objects_kept;
                    return true;
                }
                // The local value is kept when it already holds an object of
                // the recorded name: a name names one object (capture counts
                // the exceptions as ambiguous), or the copies of one name are
                // interchangeable (every constant target).
                if (current && current->GetName() && name == current->GetName()) {
                    emit(local_value);
                    ++r.objects_kept;
                    return true;
                }
                int matches = 0;
                CKObject* found = name.empty() ? nullptr
                    : find_by_name(context, name, local->GetParameterClassID(), static_cast<CK_CLASSID>(recorded_class), &matches);
                if (found) {
                    emit(static_cast<uint32_t>(found->GetID()));
                    ++r.objects_resolved;
                } else if (matches > 1) {
                    emit(local_value);
                    ++r.ambiguous;
                    note(r, r.ambiguous_list, parameter_place(block, output, index, local) + " -> " + name + " ("
                                              + std::to_string(matches) + " objects)");
                } else {
                    emit(local_value);
                    ++r.unportable;
                    note(r, r.unportable_list, parameter_place(block, output, index, local) + " -> "
                                               + (name.empty() ? std::string("an object without a name") : name));
                }
                return true;
            }
            if (kind == 4) {
                std::string owner, name;
                if (!get_str(in, pos, owner) || !get_str(in, pos, name)) return fail(error, "truncated script reference");
                if (!data || local->GetParameterClassID() == 0) return fail(error, ("a script reference on a parameter that holds none at " + parameter_place(block, output, index, local)).c_str());
                ++r.objects;
                auto* current = local_value ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(local_value))) : nullptr;
                int matches = 0;
                if (current && !current->GetParent() && current->GetName() && name == current->GetName() && owner_name(current) == owner) {
                    emit(local_value);
                    ++r.objects_kept;
                } else if (CKBehavior* found = find_script(context, owner, name, &matches); found && matches == 1) {
                    emit(static_cast<uint32_t>(found->GetID()));
                    ++r.objects_resolved;
                } else if (matches > 1) {
                    emit(local_value);
                    ++r.ambiguous;
                    note(r, r.ambiguous_list, parameter_place(block, output, index, local) + " -> script " + owner + "/" + name
                                              + " (" + std::to_string(matches) + " scripts)");
                } else {
                    emit(local_value);
                    ++r.unportable;
                    note(r, r.unportable_list, parameter_place(block, output, index, local) + " -> script " + owner + "/" + name);
                }
                return true;
            }
            if (kind == 3) {
                uint8_t registry = 0;
                std::string name;
                if (!get_u8(in, pos, registry) || !get_str(in, pos, name)) return fail(error, "truncated registry entry");
                if (!data) return fail(error, ("a registry entry on a parameter that holds none at " + parameter_place(block, output, index, local)).c_str());
                ++r.registry;
                int value = 0;
                if (registry_index(context, registry, name, value)) {
                    emit(static_cast<uint32_t>(value));
                } else {
                    emit(local_value);
                    ++r.unportable;
                    note(r, r.unportable_list, parameter_place(block, output, index, local) + (registry == 1 ? " -> message " : " -> attribute ") + name);
                }
                return true;
            }
            return fail(error, "parameter kind");
        }

        // One portable block as the local v2 block (localize): the local id,
        // every count checked against the local graph.
        static bool localize_behavior(CKContext* context, CKBehavior* beh, const std::vector<uint8_t>& in, size_t& pos,
                                      std::vector<uint8_t>& out, std::string* error, portable_report& r,
                                      const std::string& script, const std::string& block) {
            const auto failed = [&](const char* why) {
                if (error) *error = std::string(why) + (r.lists ? " at " + block : std::string(" in block ") + (beh->GetName() ? beh->GetName() : "?"));
                return false;
            };
            put_u32(out, static_cast<uint32_t>(beh->GetID()));
            if (!copy_bytes(in, pos, 12, out)) return failed("truncated block");
            uint16_t locals = 0, outs = 0, children = 0, delayed = 0;
            if (!get_u16(in, pos, locals) || locals != beh->GetLocalParameterCount()) return failed("local parameter count");
            put_u16(out, locals);
            for (int i = 0; i < locals; ++i)
                if (!localize_parameter(context, beh->GetLocalParameter(i), in, pos, out, error, r, block, false, i)) return false;
            if (!get_u16(in, pos, outs) || outs != beh->GetOutputParameterCount()) return failed("output parameter count");
            put_u16(out, outs);
            for (int i = 0; i < outs; ++i)
                if (!localize_parameter(context, beh->GetOutputParameter(i), in, pos, out, error, r, block, true, i)) return false;
            uint8_t has_force = 0, has_joint = 0;
            if (!get_u8(in, pos, has_force)) return failed("truncated force");
            put_u8(out, has_force);
            if (has_force && !copy_bytes(in, pos, 49, out)) return failed("truncated force");
            if (!get_u8(in, pos, has_joint)) return failed("truncated joint");
            put_u8(out, has_joint);
            if (has_joint && !copy_bytes(in, pos, 1, out)) return failed("truncated joint");
            if (!get_u16(in, pos, children) || children != (beh->IsUsingFunction() ? 0 : beh->GetSubBehaviorCount()))
                return failed("sub-behaviour count");
            put_u16(out, children);
            for (int i = 0; i < children; ++i) {
                uint8_t present = 0;
                CKBehavior* child = beh->GetSubBehavior(i);
                if (!get_u8(in, pos, present) || present != (child ? 1 : 0)) return failed("sub-behaviour presence");
                put_u8(out, present);
                if (child && !localize_behavior(context, child, in, pos, out, error, r, script, child_place(r, block, i, child)))
                    return false;
            }
            if (!get_u16(in, pos, delayed) || delayed != delayed_link_count(beh)) return failed("delayed link count");
            put_u16(out, delayed);
            if (!copy_bytes(in, pos, 2u * delayed, out)) return failed("truncated links");
            ++r.blocks;
            return true;
        }

        // Past one block of a v2 record / of a portable record.
        static bool skip_parameters_v2(const std::vector<uint8_t>& in, size_t& pos) {
            uint16_t n = 0;
            if (!get_u16(in, pos, n)) return false;
            for (int i = 0; i < n; ++i) {
                uint8_t kind = 0;
                uint32_t size = 0;
                if (!get_u8(in, pos, kind)) return false;
                if (!kind) continue;
                if (!get_u32(in, pos, size) || pos + size > in.size()) return false;
                pos += size;
            }
            return true;
        }
        static bool skip_tail(const std::vector<uint8_t>& in, size_t& pos, bool portable) {
            uint8_t has = 0;
            if (!get_u8(in, pos, has) || (has && !skip(in, pos, 49))) return false;
            if (!get_u8(in, pos, has)) return false;
            if (has) {
                uint8_t handle = 0;
                if (!get_u8(in, pos, handle) || (handle == kJointImaged && !skip(in, pos, 8))) return false;
            }
            uint16_t children = 0, delayed = 0;
            if (!get_u16(in, pos, children)) return false;
            for (int i = 0; i < children; ++i) {
                uint8_t present = 0;
                if (!get_u8(in, pos, present)) return false;
                if (present && !(portable ? skip_portable_block(in, pos) : skip_v2_block(in, pos))) return false;
            }
            return get_u16(in, pos, delayed) && skip(in, pos, 2u * delayed);
        }
        static bool skip_v2_block(const std::vector<uint8_t>& in, size_t& pos) {
            return skip(in, pos, 16) && skip_parameters_v2(in, pos) && skip_parameters_v2(in, pos) && skip_tail(in, pos, false);
        }
        static bool skip_parameters_portable(const std::vector<uint8_t>& in, size_t& pos) {
            uint16_t n = 0;
            if (!get_u16(in, pos, n)) return false;
            for (int i = 0; i < n; ++i) {
                uint8_t kind = 0;
                uint32_t size = 0;
                std::string name;
                if (!get_u8(in, pos, kind)) return false;
                if (kind == 0) continue;
                if (kind == 1) {
                    if (!get_u32(in, pos, size) || !skip(in, pos, size)) return false;
                } else if (kind == 2) {
                    if (!get_u32(in, pos, size) || !get_str(in, pos, name)) return false;
                } else if (kind == 3) {
                    if (!skip(in, pos, 1) || !get_str(in, pos, name)) return false;
                } else if (kind == 4) {
                    if (!get_str(in, pos, name) || !get_str(in, pos, name)) return false;
                } else {
                    return false;
                }
            }
            return true;
        }
        static bool skip_portable_block(const std::vector<uint8_t>& in, size_t& pos) {
            return skip(in, pos, 12) && skip_parameters_portable(in, pos) && skip_parameters_portable(in, pos)
                && skip_tail(in, pos, true);
        }

        // The output IO a sub-link starts from, as (owner, output index).
        static bool link_source(CKBehaviorLink* link, CKBehavior*& owner, int& index) {
            CKBehaviorIO* io = link ? link->GetInBehaviorIO() : nullptr;
            owner = io ? io->GetOwner() : nullptr;
            if (!owner) return false;
            for (int k = 0; k < owner->GetOutputCount(); ++k)
                if (owner->GetOutput(k) == io) { index = k; return true; }
            return false;
        }
        // Whether every link out of that IO is delayed: re-activating the
        // output to re-arm one in-flight link must not fire a plain one.
        static bool all_links_delayed(CKBehavior* graph, CKBehaviorIO* io) {
            for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
                CKBehaviorLink* other = graph->GetSubBehaviorLink(i);
                if (other && other->GetInBehaviorIO() == io && other->GetInitialActivationDelay() == 0) return false;
            }
            return true;
        }

        // A joint block back to the record's constraint (before the blob,
        // which matches recorded controllers to live ones and makes none).
        static void restore_joint(CKBehavior* beh, const script_replay_hooks& hooks, bool had_handle, uint32_t image_hash,
                                  const std::array<int16_t, 2>& order) {
            const bool has_handle = joint_handle(beh) != nullptr;
            // Step RLC's review: put in order with the record's other imaged
            // constraints once every block is restored (restore, below).
            if (had_handle && image_hash) joints_to_order().emplace_back(beh, order);
            if (!had_handle && has_handle) {
                // The block's own reset: deletes its constraint or spring
                // while both cores still exist and nulls the handle, as
                // physics_RT's own ResetPhysicsBehaviorHandles does.
                beh->CallCallbackFunction(CKM_BEHAVIORRESET);
                ++last_restore().joints_reset;
                return;
            }
            if (!had_handle) return;
            if (has_handle) {
                // Step RLC: the same constraint, unless a Shutdown and a
                // Create inside the window made another one (another image).
                if (!image_hash || !hooks.capture_joint || !hooks.restore_joint) return;
                std::vector<uint8_t> live;
                std::array<int16_t, 2> live_order{-1, -1};
                if (!hooks.capture_joint(beh, live, live_order) || live.empty() || joint_image_hash(live) == image_hash) return;
                const joint_ledger::entry* recorded = joints().find(beh->GetID(), image_hash);
                if (!recorded) {
                    ++joint_stats().unmatched;
                    return;
                }
                const std::vector<uint8_t> image = recorded->image;
                beh->CallCallbackFunction(CKM_BEHAVIORRESET);
                if (hooks.restore_joint(beh, image)) {
                    ++last_restore().joints_replaced;
                    ++joint_stats().replaced;
                } else {
                    ++last_restore().joints_missing;
                    ++joint_stats().missing;
                }
                return;
            }
            // Step RLC: shut down inside the window - made again from its
            // image (a record naming none: the block's latest).
            const joint_ledger::entry* recorded = hooks.restore_joint ? joints().find(beh->GetID(), image_hash) : nullptr;
            if (recorded && hooks.restore_joint(beh, recorded->image)) {
                ++last_restore().joints_recreated;
                ++joint_stats().recreated;
            } else {
                ++last_restore().joints_missing;
                ++joint_stats().missing;
            }
        }

        static bool restore_behavior(CKBehavior* beh, const script_replay_hooks& hooks, const std::vector<uint8_t>& in,
                                     size_t& pos, std::string* error, bool write = true) {
            uint32_t id = 0, flags = 0, in_mask = 0, out_mask = 0;
            if (!get_u32(in, pos, id) || id != static_cast<uint32_t>(beh->GetID())) return fail(error, "block id");
            if (!get_u32(in, pos, flags) || !get_u32(in, pos, in_mask) || !get_u32(in, pos, out_mask))
                return fail(error, "truncated block");
            const int inputs = beh->GetInputCount(), outputs = beh->GetOutputCount();
            if (write) {
                beh->ModifyFlags(flags & kRuntimeFlags, kRuntimeFlags & ~flags);
                for (int i = 0; i < inputs && i < 32; ++i) beh->ActivateInput(i, ((in_mask >> i) & 1u) != 0);
                for (int i = 0; i < outputs && i < 32; ++i) beh->ActivateOutput(i, ((out_mask >> i) & 1u) != 0);
            }
            uint16_t locals = 0, outs = 0, children = 0;
            if (!get_u16(in, pos, locals) || locals != beh->GetLocalParameterCount()) return fail(error, "local parameter count");
            for (int i = 0; i < locals; ++i)
                if (!restore_parameter(write ? beh->GetLocalParameter(i) : nullptr, in, pos, error)) return false;
            if (!get_u16(in, pos, outs) || outs != beh->GetOutputParameterCount()) return fail(error, "output parameter count");
            for (int i = 0; i < outs; ++i)
                if (!restore_parameter(write ? beh->GetOutputParameter(i) : nullptr, in, pos, error)) return false;
            uint8_t has_force = 0;
            if (!get_u8(in, pos, has_force)) return fail(error, "truncated force");
            if (has_force) {
                force_block_state force;
                uint8_t exists = 0;
                if (!get_u8(in, pos, exists)) return fail(error, "truncated force");
                force.exists = exists != 0;
                for (int k = 0; k < 3; ++k) if (!get_f64(in, pos, force.position[k])) return fail(error, "truncated force");
                for (int k = 0; k < 3; ++k) if (!get_f64(in, pos, force.force[k])) return fail(error, "truncated force");
                if (write && hooks.restore_force) hooks.restore_force(beh, force);
            }
            uint8_t has_joint = 0;
            if (!get_u8(in, pos, has_joint)) return fail(error, "truncated joint");
            if (has_joint) {
                uint8_t had_handle = 0;
                uint32_t image_hash = 0;
                uint16_t places[2] = {0xffffu, 0xffffu};
                if (!get_u8(in, pos, had_handle)) return fail(error, "truncated joint");
                if (had_handle == kJointImaged
                    && (!get_u32(in, pos, image_hash) || !get_u16(in, pos, places[0]) || !get_u16(in, pos, places[1])))
                    return fail(error, "truncated joint");
                if (write && is_joint_block(beh))
                    restore_joint(beh, hooks, had_handle != 0, image_hash,
                                  {static_cast<int16_t>(places[0]), static_cast<int16_t>(places[1])});
            }
            if (!get_u16(in, pos, children)) return fail(error, "truncated children");
            const int now = beh->IsUsingFunction() ? 0 : beh->GetSubBehaviorCount();
            if (children != now) return fail(error, "sub-behaviour count");
            for (int i = 0; i < children; ++i) {
                uint8_t present = 0;
                if (!get_u8(in, pos, present)) return fail(error, "truncated child");
                CKBehavior* child = beh->GetSubBehavior(i);
                if (present != (child ? 1 : 0)) return fail(error, "sub-behaviour presence");
                if (child && !restore_behavior(child, hooks, in, pos, error, write)) return false;
            }
            uint16_t delayed = 0;
            if (!get_u16(in, pos, delayed)) return fail(error, "truncated links");
            uint16_t now_delayed = 0;
            const int links = beh->IsUsingFunction() ? 0 : beh->GetSubBehaviorLinkCount();
            for (int i = 0; i < links; ++i)
                if (CKBehaviorLink* link = beh->GetSubBehaviorLink(i); link && link->GetInitialActivationDelay() > 1) ++now_delayed;
            if (delayed != now_delayed) return fail(error, "delayed link count");
            for (int i = 0; i < links; ++i) {
                CKBehaviorLink* link = beh->GetSubBehaviorLink(i);
                if (!link || link->GetInitialActivationDelay() <= 1) continue;
                uint16_t recorded = 0;
                if (!get_u16(in, pos, recorded)) return fail(error, "truncated link");
                if (!write) continue;
                const int initial = link->GetInitialActivationDelay();
                const int live = link->GetActivationDelay();
                const bool pending_recorded = recorded > 0 && recorded < initial;
                const bool pending_live = live > 0 && live < initial;
                if (pending_recorded) {
                    // Re-armed: the countdown set to the anchor's, the source
                    // output active again; the next execution of this graph
                    // queues the link (keeping the countdown) and its end-of-
                    // frame check counts it down as the live frame did.
                    CKBehavior* owner = nullptr;
                    int index = 0;
                    if (all_links_delayed(beh, link->GetInBehaviorIO()) && link_source(link, owner, index)) {
                        link->SetActivationDelay(recorded);
                        owner->ActivateOutput(index, TRUE);
                        ++last_restore().links_rearmed;
                        last_rearmed_outputs().emplace_back(owner->GetID(), index);   // step B2SX (Q5)
                    } else {
                        ++last_restore().links_unrestorable;
                    }
                } else if (pending_live) {
                    // In flight live, not at the anchor: the graph's list is
                    // not reachable through the SDK, so it fires anyway.
                    ++last_restore().links_live_pending;
                }
            }
            return true;
        }

        static bool fail(std::string* error, const char* why) {
            if (error) *error = why;
            return false;
        }
        static void put_u8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
        static void put_u16(std::vector<uint8_t>& out, uint16_t v) {
            out.push_back(static_cast<uint8_t>(v & 0xff));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        static void put_u32(std::vector<uint8_t>& out, uint32_t v) {
            for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>((v >> (8 * k)) & 0xff));
        }
        static void put_f64(std::vector<uint8_t>& out, double v) {
            uint8_t bytes[8];
            std::memcpy(bytes, &v, 8);
            out.insert(out.end(), bytes, bytes + 8);
        }
        static bool get_u8(const std::vector<uint8_t>& in, size_t& pos, uint8_t& v) {
            if (pos + 1 > in.size()) return false;
            v = in[pos++];
            return true;
        }
        static bool get_u16(const std::vector<uint8_t>& in, size_t& pos, uint16_t& v) {
            if (pos + 2 > in.size()) return false;
            v = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
            pos += 2;
            return true;
        }
        static bool get_u32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& v) {
            if (pos + 4 > in.size()) return false;
            v = 0;
            for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(in[pos + k]) << (8 * k);
            pos += 4;
            return true;
        }
        static bool get_f64(const std::vector<uint8_t>& in, size_t& pos, double& v) {
            if (pos + 8 > in.size()) return false;
            std::memcpy(&v, in.data() + pos, 8);
            pos += 8;
            return true;
        }
        // The portable record's strings: u16 length, bytes.
        static void put_str(std::vector<uint8_t>& out, const std::string& s) {
            const size_t n = std::min<size_t>(s.size(), 0xffff);
            put_u16(out, static_cast<uint16_t>(n));
            out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        }
        static bool get_str(const std::vector<uint8_t>& in, size_t& pos, std::string& s) {
            uint16_t n = 0;
            if (!get_u16(in, pos, n) || pos + n > in.size()) return false;
            s.assign(reinterpret_cast<const char*>(in.data() + pos), n);
            pos += n;
            return true;
        }
        static bool skip(const std::vector<uint8_t>& in, size_t& pos, size_t n) {
            if (pos + n > in.size()) return false;
            pos += n;
            return true;
        }
        static bool copy_bytes(const std::vector<uint8_t>& in, size_t& pos, size_t n, std::vector<uint8_t>& out) {
            if (pos + n > in.size()) return false;
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + n));
            pos += n;
            return true;
        }
    };
}
