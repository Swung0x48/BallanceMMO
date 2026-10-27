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
// The sensors' reference.  Every TT Scaleable Proximity in a mechanism script
// measures Ball_Pos_Frame, not the ball; that frame is a CHILD of the ball
// entity (parent=Ball_Stone, traced on the replay), so it follows the ball's
// restored and replayed body without any script.  The selection has room for
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

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
    };

    class script_replay {
    public:
        static constexpr uint32_t kMagic = 0x52534d42u;   // "BMSR"
        // v2: the joint blocks' handle presence, a graph's in-flight delayed
        // links, the three data-array cells the sector manager and the modules
        // read (plan doc 10).
        static constexpr uint32_t kVersion = 2;
        // v3 (docs/trafo-replication-design.md 4.5, step T1): the v2 record,
        // byte for byte after the version word, then the trafo sets' scripts
        // keyed by id - written only when a trafo part is selected, so every
        // record without trafo sets stays v2:
        //   u32 trafo_count, per script: u32 id, u8 present, block (as v2)
        // A selected trafo script the record does not list (its set was made
        // after the record) is restored to its set's built state; a recorded
        // one that no longer exists fails the restore
        // (session/trafo_input.hpp plan_trafo_record_restore).
        static constexpr uint32_t kVersionTrafo = 3;
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
        };
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
            uint32_t joints_released = 0;   // handles nulled because the rollback destroyed their body (release_joints)
            uint32_t trafo_built = 0;       // trafo scripts the record did not list, put to their set's built state (v3)
        };
        static restore_counters& last_restore() {
            static restore_counters counters;
            return counters;
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

        // The scripts that travel with the records, in the order the behaviour
        // manager runs their owners: priority first (highest first), then by
        // id, which is the same on every call.  Mechanism scripts do not read
        // each other's state, so the order only has to be stable.  `trafo`
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

        // The runtime state of the scripts, as bytes.  Deterministic for a
        // deterministic world, so two captures of the same tick compare
        // byte for byte.
        static bool capture(CKContext* context, const selection& sel, const script_replay_hooks& hooks,
                            std::vector<uint8_t>& out, const trafo_part* trafo = nullptr) {
            out.clear();
            if (!context) return false;
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
            put_u32(out, kMagic);
            put_u32(out, v3 ? kVersionTrafo : kVersion);
            for (const auto* list: {&sel.scripts, &sel.satellites}) {
                put_u32(out, static_cast<uint32_t>(list->size()));
                for (CK_ID id: *list) {
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    put_u32(out, static_cast<uint32_t>(id));
                    put_u8(out, beh ? 1 : 0);
                    if (beh) capture_behavior(beh, hooks, out);
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
                for (CK_ID id: trafo->scripts) {
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    put_u32(out, static_cast<uint32_t>(id));
                    put_u8(out, beh ? 1 : 0);
                    if (beh) capture_behavior(beh, hooks, out);
                }
            }
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
                            const trafo_part* trafo = nullptr) {
            if (error) error->clear();
            if (replayed) replayed->clear();
            last_restore() = restore_counters{};
            if (!context) return fail(error, "no context");
            size_t pos = 0;
            uint32_t magic = 0, version = 0;
            if (!get_u32(in, pos, magic) || magic != kMagic) return fail(error, "not a script snapshot");
            if (!get_u32(in, pos, version) || (version != kVersion && version != kVersionTrafo))
                return fail(error, "snapshot version");
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
                    if (!present) continue;            // the capture had no such script; nothing to write
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
            if (version == kVersionTrafo) {
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
                    if (!restore_behavior(beh, hooks, in, pos, error, true)) return false;
                }
            }
            if (pos != in.size()) return fail(error, "trailing bytes");
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
                    ++last_restore().trafo_built;
                }
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
        static int execute_frame(CKContext* context, const selection& sel, float delta_ms,
                                 const std::vector<CK_ID>* replayed = nullptr, const std::vector<CK_ID>* trafo = nullptr) {
            if (!context) return 0;
            int executed = 0;
            if (trafo) executed += execute_scripts(context, *trafo, delta_ms);
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
            executed += execute_scripts(context, replayed ? *replayed : sel.scripts, delta_ms);
            return executed;
        }

        // One behaviour frame of a list of root scripts: the next-frame flags
        // of every one first, then every active one in the list's order (the
        // selection's frame above, and the trafo frame of game/trafo_sets.hpp).
        static int execute_scripts(CKContext* context, const std::vector<CK_ID>& scripts, float delta_ms) {
            if (!context) return 0;
            int executed = 0;
            for (CK_ID id: scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh) continue;
                const CKDWORD flags = beh->GetFlags();
                if (flags & (CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME)) {
                    beh->Activate((flags & CKBEHAVIOR_ACTIVATENEXTFRAME) != 0, (flags & CKBEHAVIOR_RESETNEXTFRAME) != 0);
                    beh->ModifyFlags(0, CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME);
                }
            }
            for (CK_ID id: scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh || !beh->IsActive()) continue;
                beh->Execute(delta_ms);
                ++executed;
            }
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
            void note(const std::string& what) {
                if (logical_count > 12) return;
                detail += (detail.empty() ? "" : "; ") + what;
            }
        };
        static difference diff(CKContext* context, const selection& sel, const std::vector<uint8_t>& a,
                               const std::vector<uint8_t>& b) {
            difference d;
            if (a == b) return d;
            d.any = true;
            size_t pa = 0, pb = 0;
            uint32_t ma = 0, mb = 0, va = 0, vb = 0;
            if (!get_u32(a, pa, ma) || !get_u32(b, pb, mb) || !get_u32(a, pa, va) || !get_u32(b, pb, vb)
                    || ma != mb || va != vb) {
                d.logical = true; ++d.logical_count; d.where = "header";
                return d;
            }
            const bool v3 = va == kVersionTrafo;
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
                    if (!diff_behavior(beh, script_name, a, pa, b, pb, d)) return d;
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
                    if (!get_u32(a, pa, ia) || !get_u32(b, pb, ib) || !get_u8(a, pa, qa) || !get_u8(b, pb, qb) || ia != ib || qa != qb) {
                        d.logical = true; ++d.logical_count;
                        if (d.where.empty()) d.where = "trafo list";
                        return d;
                    }
                    if (!qa) continue;
                    auto* beh = context ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(ia))) : nullptr;
                    const std::string script_name = beh && beh->GetName() ? beh->GetName() : std::to_string(ia);
                    if (!diff_behavior(beh, script_name, a, pa, b, pb, d)) return d;
                }
            }
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
        static constexpr uint32_t kPortableMagic = 0x50534d42u;   // "BMSP"
        static constexpr uint32_t kPortableVersion = 1;
        static constexpr uint32_t kPortableVersionTrafo = 2;

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
            uint32_t magic = 0, version = 0, count = 0;
            if (!get_u32(record, a, magic) || magic != kMagic || !get_u32(record, a, version)
                    || (version != kVersion && version != kVersionTrafo) || !get_u32(record, a, count))
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
                                     portable_report* report = nullptr, const trafo_part* trafo = nullptr) {
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
            const bool v2 = trafo != nullptr;
            put_u32(out, kPortableMagic);
            put_u32(out, v2 ? kPortableVersionTrafo : kPortableVersion);
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
                             const trafo_part* trafo = nullptr) {
            out.clear();
            if (error) error->clear();
            if (!context) return fail(error, "no context");
            if (!sel.satellites.empty()) return fail(error, "satellite blocks are not portable");
            portable_report scratch;
            portable_report& r = report ? *report : scratch;
            // The portable scripts by key: where each one's block starts.
            size_t pos = 0;
            uint32_t magic = 0, version = 0, count = 0;
            if (!get_u32(portable, pos, magic) || magic != kPortableMagic) return fail(error, "not a portable script record");
            if (!get_u32(portable, pos, version) || (version != kPortableVersion && version != kPortableVersionTrafo))
                return fail(error, "portable record version");
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
            if (version == kPortableVersionTrafo) {
                size_t at = cells_at + static_cast<size_t>(kCellCount) * 5;
                if (at > portable.size()) return fail(error, "truncated portable cells");
                pos = at;
                uint32_t trafo_count = 0;
                if (!get_u32(portable, pos, trafo_count)) return fail(error, "truncated portable trafo scripts");
                if (!read_entries(trafo_count, trafo_entries)) return false;
                end_at = pos;
                if (!trafo) return fail(error, "the portable record carries a trafo part and this world has no trafo list");
            }
            const bool v3 = version == kPortableVersionTrafo;
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
            put_u32(out, v3 ? kVersionTrafo : kVersion);
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
                    if (!localize_behavior(context, beh, portable, at, out, error, r, name, place)) {
                        out.clear();
                        if (error) *error = place + ": " + *error;
                        return false;
                    }
                }
                for (const entry& e: trafo_entries)
                    if (!e.used) {
                        out.clear();
                        if (error) *error = "the local trafo list has no script " + e.owner + "/" + e.name;
                        return false;
                    }
                pos = end_at;
            }
            if (pos != portable.size()) { out.clear(); return fail(error, "trailing bytes in the portable record"); }
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
                                  const std::vector<uint8_t>& b, size_t& pb, difference& d) {
            const std::string block = script + " / " + (beh && beh->GetName() ? beh->GetName() : "?");
            uint32_t xa = 0, xb = 0;
            // id, flags, inputs, outputs
            static const char* const kNames[4] = {"id", "flags", "inputs", "outputs"};
            for (int k = 0; k < 4; ++k) {
                if (!get_u32(a, pa, xa) || !get_u32(b, pb, xb)) { d.logical = true; ++d.logical_count; if (d.where.empty()) d.where = block + ": truncated"; return false; }
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
                if (ha != hb) {
                    d.logical = true; ++d.logical_count;
                    const std::string what = block + ": joint handle " + std::to_string(ha) + " vs " + std::to_string(hb);
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
                if (!diff_behavior(child, script, a, pa, b, pb, d)) return false;
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
            // did not see: leave the value alone rather than write past it.
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
            if (parameter && !is_opaque(parameter) && parameter->GetDataSize() == static_cast<int>(size))
                if (void* buffer = parameter->GetWriteDataPtr()) std::memcpy(buffer, in.data() + pos, size);
            pos += size;
            return true;
        }

        static void capture_behavior(CKBehavior* beh, const script_replay_hooks& hooks, std::vector<uint8_t>& out) {
            put_u32(out, static_cast<uint32_t>(beh->GetID()));
            put_activation(beh, out);
            const int locals = beh->GetLocalParameterCount();
            put_u16(out, static_cast<uint16_t>(locals));
            for (int i = 0; i < locals; ++i) capture_parameter(beh->GetLocalParameter(i), out);
            const int outs = beh->GetOutputParameterCount();
            put_u16(out, static_cast<uint16_t>(outs));
            for (int i = 0; i < outs; ++i) capture_parameter(beh->GetOutputParameter(i), out);
            put_force_and_joint(beh, hooks, out);
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
        static void put_force_and_joint(CKBehavior* beh, const script_replay_hooks& hooks, std::vector<uint8_t>& out) {
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
                put_u8(out, joint_handle(beh) ? 1 : 0);
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
            put_force_and_joint(beh, hooks, out);
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
            if (!get_u8(in, pos, has) || (has && !skip(in, pos, 1))) return false;
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
                if (!get_u8(in, pos, had_handle)) return fail(error, "truncated joint");
                if (write && is_joint_block(beh)) {
                    const bool has_handle = joint_handle(beh) != nullptr;
                    if (!had_handle && has_handle) {
                        // The block's own reset: deletes its constraint or
                        // spring while both cores still exist and nulls the
                        // handle, as physics_RT's own ResetPhysicsBehaviorHandles does.
                        beh->CallCallbackFunction(CKM_BEHAVIORRESET);
                        ++last_restore().joints_reset;
                    } else if (had_handle && !has_handle) {
                        ++last_restore().joints_missing;
                    }
                }
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
