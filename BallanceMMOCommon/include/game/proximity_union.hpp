#pragma once

// The proximity union's CK side (docs/world-agreement-gaps-design.md §2.3,
// part B; built at step G1).  Public CK2 API only, like sector_placement.hpp:
// it compiles against the retail Virtools SDK and Ballanced.
//
// Every "TT Scaleable Proximity" block whose ObjectA (input 1) resolves to
// Ball_Pos_Frame is rewired to a private frame (a 3D entity with no body), and
// every "Get Cell" of CurrentLevel's ActiveBall column inside a script with a
// rewired block reads a private one-row copy of CurrentLevel instead.  Before
// the behaviour pass of a tick the frames and the copies' cells are put on a
// ball of the candidate set:
//
//   * place_legacy - the server's path with the world option's bit clear
//     (the code that was physics_world's rewire_proximity_probes /
//     update_proximity_probes / rewire_ball_identity_reads /
//     update_ball_identity_reads, moved here unchanged at step G1): the
//     nearest player clone by full 3D squared distance, the parked retail
//     ball's Ball_Pos_Frame when there is none; an identity copy names the
//     player nearest to ANY ObjectB of its script, the retail entity of its
//     type where the script tests the name, its clone otherwise.
//   * place - §2.2's rule under the bit (the server from step G2, the
//     clients from G3): the nearest candidate under the block's own metric
//     (session/proximity_rules.hpp), the lowest player id on a tie, the
//     frame held where it is when there is no candidate; an identity copy
//     takes its gating probe's choice; a body-kind Get Cell's latched output
//     is moved off a ball that has no body any more.
//
// Names are the same in every world (§2.3): a frame is
// BMMO_Prox_<owner>_<n>, an identity copy CurrentLevel#BMMOId_<owner>_<n>,
// n the block's position in the pre-order walk of its root script in
// sub-behaviour index order (the root is 0); BMMO_NoBall (union rules only)
// is one body-less entity far below the level.  The objects are still CREATED
// in behaviour-list order, frames first, then the copies, as the server
// always made them, so the CK ids every later object gets are what they were
// before step G1.
//
// signature() is §2.3's FNV-1a 64 with the byte layout of SimTool's
// --probe-census (BallanceMMOServer/sim/probe_census.hpp, step G0, whose
// table in the design's "As built (step WG0)" is the reference): per block
// of the class in (owner, script, walk) order str frame, str ObjectB, f32
// Distance, i32 Check Axis, i32 Use Outputs, f32 exactness min, f32
// exactness max; per identity read of the class in the same order str copy,
// u8 kind (1 by-name, 0 body), str gating frame ("" unless exactly one);
// str the entity CurrentLevel[0][ActiveBall] names; the reset point as
// 3 x f32.  A str is a u32 length and its bytes, little-endian.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "CKAll.h"

#include <session/mechanism_units.hpp>
#include <session/proximity_rules.hpp>
#include <session/resync_rules.hpp>

#include "navigation_graph.hpp"
#include "script_replay.hpp"

namespace bmmo::game {

    using bmmo::session::probe_candidate;

    enum class probe_class : uint8_t {
        mechanism,   // clients: the root script is in the session selection and not keep_local (§2.3)
        all          // the server: every block of the structural test
    };

    // The name of a parameter operation reading `value`, "" when none: the
    // rope bridge's name test is a parameter *operation* ("Get Name") feeding
    // a Test block, while a mechanism that works on the ball (the fan's
    // SetPhysicsForce, its box test) hands it to a behaviour.  Parameter links
    // live on the reading side (CKParameterIn::m_OutSource), so the consumers
    // are found by looking for readers, not through the producer's
    // destination list.  (Was physics_world.cpp's; moved unchanged, §2.3.)
    inline std::string proximity_operation_on(CKContext* context, CKParameter* value) {
        if (!context || !value) return {};
        const int count = context->GetObjectsCountByClassID(CKCID_PARAMETEROPERATION);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_PARAMETEROPERATION);
        for (int i = 0; i < count; ++i) {
            auto* operation = CKParameterOperation::Cast(context->GetObject(ids[i]));
            if (!operation) continue;
            for (CKParameterIn* in: {operation->GetInParameter1(), operation->GetInParameter2()})
                if (in && in->GetDirectSource() == value)
                    return operation->GetName() ? operation->GetName() : "?";
        }
        return {};
    }

    class proximity_union {
    public:
        struct rewire_options {
            // false: the server's path with the world option's bit clear (step
            // G1): the frames start at Ball_Pos_Frame's world matrix, there is
            // no BMMO_NoBall, a read is rewired whatever its gating probes and
            // a refused creation skips that block (as the server always did).
            // true: §2.3 under the bit: the frames start at the reset point,
            // BMMO_NoBall is made, a read without exactly one gating probe or
            // a block with Barycenter 1 refuses the whole rewire (all or
            // nothing: nothing stays rewired).
            bool union_rules = false;
            // The level's first reset point (ResetPoints row 0; the server's
            // physics_world::resetpoints().front()): the signature's reset
            // point and the union rules' initial frame position.
            float reset[3] = {0.0f, 0.0f, 0.0f};
            // The Physicalize_GameBall names in row order: a candidate's ball
            // type is the longest of them its entity name starts with (by-name
            // kind, union rules).
            std::vector<std::string> ball_names;
            // Step Q7 (§2.10, WORLD_PLAYER_MECHANISMS): the session's slot
            // count K; with the union rules and K >= 1 the rewire first copies
            // every per-player unit K - 1 times (slot 0 is the original) and
            // binds slot k's frames, identity cells and latches to the
            // candidate whose `slot` is k.  0: no units (G3's rules).
            uint8_t slots = 0;
        };
        // A ball of the bit-clear path, in player id order: the body (the
        // clone) and the retail entity of its type (null when unknown).
        struct legacy_ball {
            CK3dEntity* body = nullptr;
            CK3dEntity* named = nullptr;
        };

        // Optional: whether an entity has a physics body (place's consistency
        // check, §2.3 Place step 2).  Without it only the entity's existence
        // is checked.
        std::function<bool(CK3dEntity*)> has_body;

        // A client host's options under the bit (step G3): the union rules,
        // the level's first reset point and the Physicalize_GameBall names,
        // read from the level's arrays the way the server's world reads them
        // at its creation (physics_world: ResetPoints row 0's matrix
        // translation, else CurrentLevel[0][3]'s; the ball table's column 0),
        // so the signature's reset point is the server's.
        static rewire_options level_options(CKContext* context) {
            rewire_options o;
            o.union_rules = true;
            if (!context) return o;
            const auto array = [context](const char* name) {
                return CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>(name), CKCID_DATAARRAY));
            };
            bool have = false;
            if (CKDataArray* table = array("ResetPoints")) {
                VxMatrix m;
                for (int row = 0; row < table->GetRowCount() && !have; ++row)
                    if (table->GetElementValue(row, 0, &m)) {
                        o.reset[0] = m[3][0];
                        o.reset[1] = m[3][1];
                        o.reset[2] = m[3][2];
                        have = true;
                    }
            }
            if (!have)
                if (CKDataArray* current_level = array("CurrentLevel")) {
                    VxMatrix m;
                    if (current_level->GetElementValue(0, 3, &m)) {
                        o.reset[0] = m[3][0];
                        o.reset[1] = m[3][1];
                        o.reset[2] = m[3][2];
                    }
                }
            if (CKDataArray* balls = array("Physicalize_GameBall"))
                for (int row = 0; row < balls->GetRowCount(); ++row) {
                    // The STRING cell holds the string's pointer in both SDKs
                    // (sector_placement.hpp's read): GetElementStringValue
                    // takes a size in Ballanced only (step WG4, the retail
                    // host's build).  Only a STRING column is read so (any
                    // other type would be a value, not a pointer: the name
                    // stays empty and the probe signature refuses, closed),
                    // and the name is cut at 127 characters like the
                    // server's 128-byte read (physics_world.cpp).
                    std::string name;
                    if (balls->GetColumnType(0) == CKARRAYTYPE_STRING)
                        if (auto* cell = balls->GetElement(row, 0))
                            if (const char* text = reinterpret_cast<const char*>(*cell))
                                name = std::string(std::string_view(text).substr(0, 127));
                    o.ball_names.push_back(std::move(name));
                }
            return o;
        }

        // At the anchor, after the sector placement's apply.  Returns "" on
        // success.  `sel` decides the mechanism class (probe_class::mechanism
        // rewires only those blocks; with probe_class::all it only marks
        // them, and an empty selection can be given and classify() called
        // when the selection exists).  The log lines are in describe().
        std::string rewire(CKContext* context, probe_class which, const script_replay::selection& sel,
                           const rewire_options& options) {
            // Step G3 (WG1's note): a host that anchors again on the same
            // level (a retail session after another, a resync's re-anchor)
            // gives the previous rewire's sources back first - clear() alone
            // would forget them, and the structural test would then find no
            // Ball_Pos_Frame source and rewire nothing.
            if (!probes_.empty() || !identities_.empty() || no_ball_ || !units_.empty()) release(context);
            note_script_write();   // step OPC: the unit copies change the selected scripts' graphs
            clear();
            which_ = which;
            options_ = options;
            lines_.clear();
            context_ = context;
            // Step Q7 (§2.10 "The copy step"): the units copied before the
            // rewire, which then gives every slot its own frame and identity
            // copy (the copies' probes and reads are found by the structural
            // test like the originals, and named by slot).
            std::string error;
            if (options_.union_rules && options_.slots > 0) {
                error = copy_units(context, sel);
                lines_.insert(lines_.end(), unit_lines_.begin(), unit_lines_.end());
            }
            if (error.empty()) error = rewire_probes(context, sel);
            if (error.empty()) error = rewire_identities(context);
            if (error.empty() && options_.union_rules) error = make_no_ball(context);
            // §2.10: a slot's identity cell starts on BMMO_NoBall (a Test of a
            // slot that never had a ball is false).
            if (error.empty() && slots_)
                for (const identity& r: identities_)
                    if (r.slot >= 0)
                        if (auto* array = CKDataArray::Cast(context->GetObject(r.array)))
                            array->SetElementObject(0, r.column, context->GetObject(no_ball_));
            order();
            classify(sel);
            if (!error.empty()) {
                release(context);
                lines_.push_back("proximity union refused: " + error);
            }
            return error;
        }

        // Marks the mechanism class (§2.3: the root script is in `sel` and is
        // not keep_local).  The server rewires `all` before its selection is
        // made (the order of the CK objects it creates is kept) and calls this
        // afterwards; signature(mechanism) needs it.
        void classify(const script_replay::selection& sel) {
            const std::set<CK_ID> selected(sel.scripts.begin(), sel.scripts.end());
            std::set<CK_ID> roots;
            for (probe& p: probes_) {
                p.mechanism = selected.count(p.root) != 0 && !session::keep_local_script(p.owner, p.script);
                if (p.mechanism) roots.insert(p.root);
            }
            for (identity& r: identities_) r.mechanism = roots.count(r.root) != 0;
            // Step Q7: a unit is in the class with its root script.
            for (unit_rec& u: units_)
                u.mechanism = selected.count(u.root) != 0 && !session::keep_local_script(u.owner, u.script);
        }

        // ---- the bit-clear path (the server's today, moved) ----

        // Before the scripts of a tick run: each probe frame goes to the
        // player ball nearest to the block's ObjectB (the mechanism) by 3D
        // squared distance, so the retail "any ball in range" test fires for
        // whichever player gets there; with none, to Ball_Pos_Frame.  Then the
        // ActiveBall cell of every identity copy names the ball of the player
        // nearest to one of its script's mechanisms - the retail entity of
        // that player's ball type where the script tests the name
        // ("Ball_Stone"), the player's clone where it works on the ball itself
        // (the fan's SetPhysicsForce and box test); with no player around,
        // `fallback_identity` (the parked retail ball), which is what the
        // retail script would have read.
        void place_legacy(CKContext* context, const std::vector<legacy_ball>& balls, CKObject* fallback_identity) {
            if (!context) return;
            if (!probes_.empty()) {
                VxVector fallback(0.0f, 0.0f, 0.0f);
                if (auto* bpf = CK3dEntity::Cast(context->GetObject(ball_pos_frame_))) bpf->GetPosition(&fallback);
                for (const probe& p: probes_) {
                    auto* block = CKBehavior::Cast(context->GetObject(p.block));
                    auto* frame = CK3dEntity::Cast(context->GetObject(p.frame));
                    if (!block || !frame) continue;
                    VxVector target = fallback;
                    if (!balls.empty()) {
                        VxVector reference = fallback;
                        if (auto* object_b = CK3dEntity::Cast(block->GetInputParameterObject(2)))
                            object_b->GetPosition(&reference);
                        float best = 0.0f;
                        bool have = false;
                        for (const legacy_ball& ball: balls) {
                            VxVector position;
                            ball.body->GetPosition(&position);
                            const float distance = (position - reference).SquareMagnitude();
                            if (!have || distance < best) {
                                best = distance;
                                target = position;
                                have = true;
                            }
                        }
                    }
                    frame->SetPosition(&target);
                }
            }
            for (const identity& r: identities_) {
                auto* array = CKDataArray::Cast(context->GetObject(r.array));
                if (!array) continue;
                CKObject* active = fallback_identity;
                float best = 0.0f;
                bool have = false;
                for (const legacy_ball& ball: balls) {
                    CK3dEntity* entity = r.by_name ? ball.named : ball.body;
                    if (!entity) continue;
                    VxVector position;
                    ball.body->GetPosition(&position);
                    for (CK_ID reference: r.references) {
                        auto* mechanism = CK3dEntity::Cast(context->GetObject(reference));
                        if (!mechanism) continue;
                        VxVector origin;
                        mechanism->GetPosition(&origin);
                        const float distance = (position - origin).SquareMagnitude();
                        if (have && distance >= best) continue;
                        best = distance;
                        have = true;
                        active = entity;
                    }
                }
                array->SetElementObject(0, r.column, active);
            }
        }

        // ---- under the bit (§2.2; the server from G2, the clients from G3) ----

        // Before the behaviour pass of a tick: frames, identity cells and
        // latches from the candidates (the balls with a body at the end of
        // the previous tick, §2.2), in player id order.
        void place(CKContext* context, const std::vector<probe_candidate>& candidates) {
            if (!context) return;
            std::vector<probe_candidate> sorted(candidates);
            std::stable_sort(sorted.begin(), sorted.end(),
                             [](const probe_candidate& a, const probe_candidate& b) { return a.player < b.player; });
            std::vector<session::probe_ball> balls;
            std::vector<CK3dEntity*> entities;
            std::vector<int> slot_of;   // step Q7: each accepted candidate's slot
            for (const probe_candidate& c: sorted) {
                auto* entity = CK3dEntity::Cast(context->GetObjectByNameAndParentClass(
                    const_cast<CKSTRING>(c.entity.c_str()), CKCID_3DENTITY, nullptr));
                if (!entity || (has_body && !has_body(entity))) {
                    ++skipped_;
                    continue;
                }
                VxVector v;
                entity->GetPosition(&v);
                session::probe_ball b;
                b.player = c.player;
                b.position[0] = v.x;
                b.position[1] = v.y;
                b.position[2] = v.z;
                balls.push_back(b);
                entities.push_back(entity);
                slot_of.push_back(c.slot);
                if (slots_ && (c.slot < 0 || c.slot >= static_cast<int>(slots_))) ++unslotted_;
            }
            // Step Q7 (§2.10 "Binding"): slot k's candidate, an index into
            // balls / entities, -1 when slot k has none.
            const std::vector<int> bound = session::units::bind_slots(slot_of, slots_);
            const auto slot_ball = [&bound](int slot) {
                return slot >= 0 && slot < static_cast<int>(bound.size()) ? bound[static_cast<size_t>(slot)] : -1;
            };
            // Frames: the nearest under the block's own metric, or the hold;
            // a unit's frame goes to its slot's candidate, or holds (§2.10
            // "Frame").
            std::vector<int> choice(probes_.size(), -1);
            for (size_t i = 0; i < probes_.size(); ++i) {
                const probe& p = probes_[i];
                if (p.slot >= 0) {
                    const int k = slot_ball(p.slot);
                    choice[i] = k;
                    auto* frame = k < 0 ? nullptr : CK3dEntity::Cast(context->GetObject(p.frame));
                    if (!frame) continue;
                    const VxVector position(balls[static_cast<size_t>(k)].position[0], balls[static_cast<size_t>(k)].position[1],
                                            balls[static_cast<size_t>(k)].position[2]);
                    frame->SetPosition(&position);
                    continue;
                }
                if (balls.empty()) continue;
                auto* block = CKBehavior::Cast(context->GetObject(p.block));
                auto* frame = CK3dEntity::Cast(context->GetObject(p.frame));
                auto* object_b = block ? CK3dEntity::Cast(block->GetInputParameterObject(2)) : nullptr;
                if (!frame || !object_b) continue;
                VxVector r;
                object_b->GetPosition(&r);
                const float reference[3] = {r.x, r.y, r.z};
                const int k = session::probe_nearest(balls, reference, static_cast<uint8_t>(p.axes));
                choice[i] = k;
                if (k < 0) continue;
                const VxVector position(balls[k].position[0], balls[k].position[1], balls[k].position[2]);
                frame->SetPosition(&position);
            }
            // Identity cells: the gating probe's choice; by-name holds, body
            // kind names BMMO_NoBall when there is no candidate.
            CKObject* no_ball = context->GetObject(no_ball_);
            for (identity& r: identities_) {
                auto* array = CKDataArray::Cast(context->GetObject(r.array));
                if (r.slot >= 0) {
                    if (array) place_slot_identity(context, r, *array, slot_ball(r.slot), entities, no_ball);
                    continue;
                }
                if (!array || r.gating < 0) continue;
                const int k = choice[static_cast<size_t>(r.gating)];
                CKObject* value = nullptr;
                if (r.by_name) {
                    if (k < 0) continue;   // the hold
                    value = retail_entity_of(context, entities[static_cast<size_t>(k)]);
                    if (!value) continue;
                    r.held = value->GetID();
                } else {
                    value = k < 0 ? no_ball : entities[static_cast<size_t>(k)];
                }
                array->SetElementObject(0, r.column, value);
                // The latch (body kind, G-c7): an output naming no candidate's
                // ball is moved to the cell's value, through the block's own
                // path (it propagates as the block's execution would).
                if (r.by_name) continue;
                auto* block = CKBehavior::Cast(context->GetObject(r.block));
                CKParameterOut* out = block && block->GetOutputParameterCount() > 0 ? block->GetOutputParameter(0) : nullptr;
                if (!out) continue;
                // The raw id, not the object: id 0 (the Get Cell has not run
                // yet) names no entity and is left alone; an id whose object
                // is gone (a leaver's destroyed clone) names no candidate
                // and is moved, like any other (§2.2, H2c; step G2).
                CK_ID latched_id = 0;
                if (out->GetDataSize() != static_cast<int>(sizeof(CK_ID)) || out->GetValue(&latched_id, FALSE) != CK_OK
                    || latched_id == 0)
                    continue;
                CKObject* latched = context->GetObject(latched_id);
                const bool on_candidate =
                    latched && std::find(entities.begin(), entities.end(), latched) != entities.end();
                if (on_candidate || (latched && latched == value)) continue;
                block->SetOutputParameterObject(0, value);
                ++relatched_;
            }
        }

        // The hold section of the script record (§4.2): u16 frames, each
        // f32 x, y, z; u16 by-name copies, each u32 the held entity's id; in
        // (owner, script, walk) order.
        void capture(std::vector<uint8_t>& out) const {
            out.clear();
            CKContext* context = context_;
            const auto u16 = [&out](uint16_t v) {
                out.push_back(static_cast<uint8_t>(v));
                out.push_back(static_cast<uint8_t>(v >> 8));
            };
            const auto u32 = [&out](uint32_t v) {
                for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(v >> s));
            };
            const auto f32 = [&u32](float v) {
                uint32_t u;
                std::memcpy(&u, &v, 4);
                u32(u);
            };
            u16(static_cast<uint16_t>(probe_order_.size()));
            for (size_t i: probe_order_) {
                VxVector v(0.0f, 0.0f, 0.0f);
                if (auto* frame = context ? CK3dEntity::Cast(context->GetObject(probes_[i].frame)) : nullptr)
                    frame->GetPosition(&v);
                f32(v.x);
                f32(v.y);
                f32(v.z);
            }
            const std::vector<size_t> by_name = held_identities();
            u16(static_cast<uint16_t>(by_name.size()));
            for (size_t i: by_name) {
                CKObject* held = nullptr;
                if (auto* array = context ? CKDataArray::Cast(context->GetObject(identities_[i].array)) : nullptr)
                    held = array->GetElementObject(0, identities_[i].column);
                u32(held ? static_cast<uint32_t>(held->GetID()) : 0u);
            }
        }

        // The portable hold section (design 4.2 / 4.4, the portable record's
        // version 3, game/script_replay.hpp): the same hold keyed by name -
        // u16 frames, per frame str frame name, f32 x, y, z; u16 by-name
        // copies, per copy str copy name, str the held entity's name ("" none);
        // str = u16 length, bytes.  The server's world writes every block it
        // rewired (class all); a client takes its own by name.
        void capture_portable(std::vector<uint8_t>& out) const {
            out.clear();
            CKContext* context = context_;
            const auto u16 = [&out](uint16_t v) {
                out.push_back(static_cast<uint8_t>(v));
                out.push_back(static_cast<uint8_t>(v >> 8));
            };
            const auto str = [&](const std::string& s) {
                u16(static_cast<uint16_t>(s.size()));
                out.insert(out.end(), s.begin(), s.end());
            };
            const auto f32 = [&out](float v) {
                uint32_t u;
                std::memcpy(&u, &v, 4);
                for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(u >> s));
            };
            u16(static_cast<uint16_t>(probe_order_.size()));
            for (size_t i: probe_order_) {
                VxVector v(0.0f, 0.0f, 0.0f);
                if (auto* frame = context ? CK3dEntity::Cast(context->GetObject(probes_[i].frame)) : nullptr)
                    frame->GetPosition(&v);
                str(probes_[i].frame_name);
                f32(v.x);
                f32(v.y);
                f32(v.z);
            }
            const std::vector<size_t> by_name = held_identities();
            u16(static_cast<uint16_t>(by_name.size()));
            for (size_t i: by_name) {
                CKObject* held = nullptr;
                if (auto* array = context ? CKDataArray::Cast(context->GetObject(identities_[i].array)) : nullptr)
                    held = array->GetElementObject(0, identities_[i].column);
                str(identities_[i].copy_name);
                str(held && held->GetName() ? held->GetName() : "");
            }
        }

        // A portable hold section as this world's hold section (capture's
        // layout, this world's order): each frame and by-name copy of this
        // union from the entry of its name.  A frame or copy the section does
        // not name, or a held name this world does not have, refuses the
        // whole section (the signature compare at SessionReady makes both
        // impossible); the section's entries this world did not rewire (the
        // server's non-mechanism blocks) are skipped.  The held entity is
        // found the way place() finds it (the first 3D object of the name).
        bool localize_hold(CKContext* context, const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                           std::string& error) const {
            out.clear();
            size_t at = 0;
            const auto need = [&](size_t n) { return at + n <= size; };
            const auto u16 = [&](uint16_t& v) {
                if (!need(2)) return false;
                v = static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
                at += 2;
                return true;
            };
            const auto str = [&](std::string& s) {
                uint16_t n = 0;
                if (!u16(n) || !need(n)) return false;
                s.assign(reinterpret_cast<const char*>(data + at), n);
                at += n;
                return true;
            };
            std::map<std::string, size_t> frames_at;   // name -> the 12 bytes' offset
            std::map<std::string, std::string> held_by;
            uint16_t frames = 0, names = 0;
            if (!context || !u16(frames)) { error = "portable hold section truncated"; return false; }
            for (uint16_t f = 0; f < frames; ++f) {
                std::string name;
                if (!str(name) || !need(12)) { error = "portable hold section truncated"; return false; }
                if (!frames_at.emplace(name, at).second) { error = "portable hold section: frame " + name + " twice"; return false; }
                at += 12;
            }
            if (!u16(names)) { error = "portable hold section truncated"; return false; }
            for (uint16_t n = 0; n < names; ++n) {
                std::string copy, held;
                if (!str(copy) || !str(held)) { error = "portable hold section truncated"; return false; }
                if (!held_by.emplace(copy, held).second) { error = "portable hold section: copy " + copy + " twice"; return false; }
            }
            if (at != size) { error = "portable hold section: trailing bytes"; return false; }
            const auto put16 = [&out](uint16_t v) {
                out.push_back(static_cast<uint8_t>(v));
                out.push_back(static_cast<uint8_t>(v >> 8));
            };
            put16(static_cast<uint16_t>(probe_order_.size()));
            for (size_t i: probe_order_) {
                const auto it = frames_at.find(probes_[i].frame_name);
                if (it == frames_at.end()) { error = "the server's hold has no frame " + probes_[i].frame_name; out.clear(); return false; }
                out.insert(out.end(), data + it->second, data + it->second + 12);
            }
            const std::vector<size_t> by_name = held_identities();
            put16(static_cast<uint16_t>(by_name.size()));
            for (size_t i: by_name) {
                const auto it = held_by.find(identities_[i].copy_name);
                if (it == held_by.end()) { error = "the server's hold has no copy " + identities_[i].copy_name; out.clear(); return false; }
                uint32_t id = 0;
                if (!it->second.empty()) {
                    CKObject* held = context->GetObjectByNameAndClass(const_cast<CKSTRING>(it->second.c_str()), CKCID_3DOBJECT, nullptr);
                    if (!held) { error = "copy " + identities_[i].copy_name + " holds " + it->second + ", not in this world"; out.clear(); return false; }
                    id = static_cast<uint32_t>(held->GetID());
                }
                for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(id >> s));
            }
            return true;
        }

        bool restore(CKContext* context, const uint8_t* data, size_t size, std::string& error) {
            size_t at = 0;
            const auto need = [&](size_t n) { return at + n <= size; };
            const auto u16 = [&]() { uint16_t v = static_cast<uint16_t>(data[at] | (data[at + 1] << 8)); at += 2; return v; };
            const auto u32 = [&]() {
                uint32_t v = 0;
                for (int s = 0; s < 4; ++s) v |= static_cast<uint32_t>(data[at + s]) << (8 * s);
                at += 4;
                return v;
            };
            if (!context || !need(2)) {
                error = "proximity hold section truncated";
                return false;
            }
            const uint16_t frames = u16();
            if (frames != probe_order_.size() || !need(static_cast<size_t>(frames) * 12 + 2)) {
                error = "proximity hold section: " + std::to_string(frames) + " frames, this world has "
                        + std::to_string(probe_order_.size());
                return false;
            }
            std::vector<VxVector> positions(frames);
            for (uint16_t f = 0; f < frames; ++f) {
                float xyz[3];
                for (float& c: xyz) {
                    const uint32_t u = u32();
                    std::memcpy(&c, &u, 4);
                }
                positions[f] = VxVector(xyz[0], xyz[1], xyz[2]);
            }
            const std::vector<size_t> by_name = held_identities();
            const uint16_t names = u16();
            if (names != by_name.size() || !need(static_cast<size_t>(names) * 4) || at + names * 4u != size) {
                error = "proximity hold section: " + std::to_string(names) + " by-name copies, this world has "
                        + std::to_string(by_name.size());
                return false;
            }
            for (uint16_t f = 0; f < frames; ++f)
                if (auto* frame = CK3dEntity::Cast(context->GetObject(probes_[probe_order_[f]].frame)))
                    frame->SetPosition(&positions[f]);
            for (size_t n = 0; n < by_name.size(); ++n) {
                identity& r = identities_[by_name[n]];
                const CK_ID held = static_cast<CK_ID>(u32());
                if (auto* array = CKDataArray::Cast(context->GetObject(r.array)))
                    array->SetElementObject(0, r.column, held ? context->GetObject(held) : nullptr);
                r.held = held;
            }
            return true;
        }

        // `unit_guard` false gives the signature as it was before step F1
        // (SimTool's negative fixture compares the two; no world sends it).
        uint64_t signature(probe_class which = probe_class::mechanism, bool unit_guard = true) const {
            const bool mechanism = which == probe_class::mechanism;
            fnv f;
            for (size_t i: probe_order_) {
                const probe& p = probes_[i];
                if (mechanism && !p.mechanism) continue;
                f.str(p.frame_name);
                f.str(p.object_b);
                f.f32(p.distance);
                f.i32(p.axes);
                f.i32(p.use_outputs);
                f.f32(p.exact_min);
                f.f32(p.exact_max);
            }
            for (size_t i: identity_order_) {
                const identity& r = identities_[i];
                if (mechanism && !r.mechanism) continue;
                f.str(r.copy_name);
                const uint8_t kind = r.by_name ? 1 : 0;
                f.bytes(&kind, 1);
                std::vector<size_t> gating;
                for (size_t g: r.gating_all)
                    if (!mechanism || probes_[g].mechanism) gating.push_back(g);
                f.str(gating.size() == 1 ? probes_[gating.front()].frame_name : std::string());
            }
            f.str(initial_entity_);
            f.f32(options_.reset[0]);
            f.f32(options_.reset[1]);
            f.f32(options_.reset[2]);
            // Step Q7 (§5): under WORLD_PLAYER_MECHANISMS also K, the units'
            // shapes and every copy's structure (blocks, IO and parameter
            // counts, links and their delays by walk position; no CK id), so
            // a world whose copies differ cannot join.  Nothing is added
            // without units: G3's signatures are unchanged.
            if (slots_) {
                f.u32(slots_);
                for (const unit_rec& u: units_) {
                    if (mechanism && !u.mechanism) continue;
                    f.str(u.owner);
                    f.str(u.script);
                    f.i32(u.read_walk);
                    const uint8_t kind = static_cast<uint8_t>(u.kind);
                    f.bytes(&kind, 1);
                    for (uint64_t h: u.structure) f.bytes(&h, 8);
                }
                // Step F1 (UGRD, the unit-set guard): the units this world
                // copied outside the mechanism class, counted when there is
                // one.  A client copies only its mechanism scripts' units (0,
                // nothing written: every signature so far is unchanged); the
                // server copies every identity read's unit before its
                // selection exists (adaptation 4 of step WGQ7), so a level
                // whose identity read lies outside a mechanism script gives
                // the server K copies the clients lack - its anchor signature
                // then differs and the server refuses each member ("probe
                // mismatch").  signature(all) hashes every unit already.
                if (mechanism && unit_guard)
                    if (const uint32_t outside = static_cast<uint32_t>(non_mechanism_unit_count())) f.u32(outside);
            }
            return f.h;
        }

        // The sources back (direct or shared, whichever was set), then the
        // frames, the copies, their parameter locals and BMMO_NoBall
        // destroyed.  Returns how many inputs were put back.  Safe to call
        // twice and on blocks that are gone (a placement copy released first).
        int release(CKContext* context) {
            int restored = 0;
            if (context) {
                const std::map<CK_ID, std::vector<CK_ID>> outputs = copy_outputs(context);
                for (auto it = identities_.rbegin(); it != identities_.rend(); ++it) {
                    if (auto* block = CKBehavior::Cast(context->GetObject(it->block)))
                        if (CKParameterIn* target = block->GetTargetParameter())
                            restored += put_back(context, target, it->prior_direct, it->prior_shared) ? 1 : 0;
                    destroy(context, it->parameter);
                    // Step F1 (IDCP): the copy with the parameter outputs it
                    // owns, in one call (a CKParameterOut's PreDelete reads
                    // its owner, so the copy must outlive their PreDelete).
                    const auto owned = outputs.find(it->array);
                    destroy_with(context, it->array, owned == outputs.end() ? nullptr : &owned->second);
                }
                for (auto it = probes_.rbegin(); it != probes_.rend(); ++it) {
                    if (auto* block = CKBehavior::Cast(context->GetObject(it->block)))
                        if (CKParameterIn* in = block->GetInputParameterCount() > 1 ? block->GetInputParameter(1) : nullptr)
                            restored += put_back(context, in, it->prior_direct, it->prior_shared) ? 1 : 0;
                    destroy(context, it->parameter);
                    destroy(context, it->frame);
                }
                destroy(context, no_ball_);
                release_units(context);   // step Q7: the slot copies with their links, after the union's own objects
                note_script_write();      // step OPC: the selected scripts' graphs changed
            }
            clear();
            return restored;
        }

        std::string describe() const {
            std::string text;
            for (const std::string& line: lines_) text += (text.empty() ? "" : "\n") + line;
            return text;
        }
        const std::vector<std::string>& lines() const { return lines_; }
        size_t probe_count() const { return probes_.size(); }
        size_t identity_count() const { return identities_.size(); }
        size_t mechanism_probe_count() const {
            size_t n = 0;
            for (const probe& p: probes_) n += p.mechanism ? 1u : 0u;
            return n;
        }
        size_t mechanism_identity_count() const {
            size_t n = 0;
            for (const identity& r: identities_) n += r.mechanism ? 1u : 0u;
            return n;
        }
        bool union_rules() const { return options_.union_rules; }
        CK_ID no_ball() const { return no_ball_; }
        uint64_t skipped() const { return skipped_; }
        uint64_t relatched() const { return relatched_; }
        // Step Q7: K, the units, the copies made and the candidates whose
        // slot was outside [0, K) (the server's cap makes it impossible: 0).
        uint8_t slots() const { return slots_; }
        size_t unit_count() const { return units_.size(); }
        // Step F1 (UGRD): units outside the mechanism class (the server's
        // class-all copy step on a level breaking WG0's census; 0 on a
        // client by construction) - the signature's tail.
        size_t non_mechanism_unit_count() const {
            size_t n = 0;
            for (const unit_rec& u: units_) n += u.mechanism ? 0u : 1u;
            return n;
        }
        // The units' root scripts, in unit order (SimTool's UGRD fixture).
        std::vector<CK_ID> unit_roots() const {
            std::vector<CK_ID> out;
            for (const unit_rec& u: units_) out.push_back(u.root);
            return out;
        }
        size_t copy_count() const {
            size_t n = 0;
            for (const unit_rec& u: units_) n += u.copies.size();
            return n;
        }
        uint64_t unslotted() const { return unslotted_; }
        // The record bytes the copies add (each copy's blocks as the v2
        // record writes them, measured at the copy step; C1).
        size_t copy_record_bytes() const { return copy_record_bytes_; }
        // §4.3: a unit force block's controller, "rt:force/<block id>" in
        // this world, is "unit:<owner>/<n>/S<k>" in every world (slot 0
        // included).  False for every other name.
        bool controller_alias(const char* name, std::string& alias) const {
            static constexpr char kPrefix[] = "rt:force/";
            if (!name || force_alias_.empty() || std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0) return false;
            char* end = nullptr;
            const unsigned long id = std::strtoul(name + sizeof(kPrefix) - 1, &end, 10);
            if (!end || *end) return false;
            const auto it = force_alias_.find(static_cast<CK_ID>(id));
            if (it == force_alias_.end()) return false;
            alias = it->second;
            return true;
        }
        const std::map<CK_ID, std::string>& force_aliases() const { return force_alias_; }
        // Every block of every slot copy (V6's diagnostics).
        std::vector<CK_ID> copied_blocks() const {
            std::vector<CK_ID> out;
            for (const auto& [id, ref]: unit_blocks_)
                if (ref.slot > 0) out.push_back(id);
            return out;
        }
        // The copy step's listing (V6, Q10): one line per unit and per copy.
        const std::vector<std::string>& unit_lines() const { return unit_lines_; }
        // "player mechanisms: K slots, U units, C copies, +B record bytes" (C1).
        std::string units_summary() const {
            return "player mechanisms: " + std::to_string(slots_) + " slots, " + std::to_string(units_.size()) + " units, "
                   + std::to_string(copy_count()) + " copies, +" + std::to_string(copy_record_bytes_) + " record bytes";
        }

    private:
        struct probe {
            CK_ID block = 0, frame = 0, parameter = 0, root = 0;
            CK_ID prior_direct = 0, prior_shared = 0;
            std::string owner, script, frame_name, object_b;
            int walk = 0;
            float distance = 0.0f, exact_min = 0.0f, exact_max = 0.0f;
            int axes = -1, use_outputs = -1, barycenter = -1;
            bool mechanism = false;
            int slot = -1;   // step Q7: a unit's probe: its slot (walk = the original's pre-copy position)
        };
        struct identity {
            CK_ID block = 0, array = 0, parameter = 0, root = 0;
            CK_ID prior_direct = 0, prior_shared = 0;
            CK_ID held = 0;   // by-name: the entity the cell names (the hold)
            std::string owner, script, copy_name, operation;
            int walk = 0;
            int column = 1;
            bool by_name = false;
            bool mechanism = false;
            std::vector<CK_ID> references;   // the bit-clear path: every ObjectB of the script, in probe order
            std::vector<size_t> gating_all;  // the rewired probes with an output linked to its input
            int gating = -1;                 // union rules: the one gating probe
            int slot = -1;                   // step Q7: a unit's read: its slot (no hold, §2.10)
        };

        struct fnv {
            uint64_t h = 1469598103934665603ull;
            void bytes(const void* p, size_t n) {
                const auto* b = static_cast<const uint8_t*>(p);
                for (size_t i = 0; i < n; ++i) {
                    h ^= b[i];
                    h *= 1099511628211ull;
                }
            }
            void u32(uint32_t v) {
                const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                                      static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
                bytes(b, 4);
            }
            void i32(int v) { u32(static_cast<uint32_t>(v)); }
            void f32(float v) {
                uint32_t u;
                std::memcpy(&u, &v, 4);
                u32(u);
            }
            void str(const std::string& s) {
                u32(static_cast<uint32_t>(s.size()));
                bytes(s.data(), s.size());
            }
        };

        void clear() {
            probes_.clear();
            identities_.clear();
            probe_order_.clear();
            identity_order_.clear();
            walks_.clear();
            ball_pos_frame_ = 0;
            current_level_ = 0;
            no_ball_ = 0;
            initial_entity_.clear();
            skipped_ = 0;
            relatched_ = 0;
            unslotted_ = 0;
            units_.clear();
            unit_blocks_.clear();
            force_alias_.clear();
            unit_lines_.clear();
            slots_ = 0;
            copy_record_bytes_ = 0;
        }

        static CKBehavior* root_of(CKBehavior* block) {
            while (block->GetParent()) block = block->GetParent();
            return block;
        }
        static std::string owner_name(CKBehavior* root) {
            CKBeObject* owner = root ? root->GetOwner() : nullptr;
            return owner && owner->GetName() ? owner->GetName() : "";
        }
        template <typename F> static void walk(CKBehavior* beh, F&& fn) {
            fn(beh);
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i)) walk(child, fn);
        }
        int walk_of(CKBehavior* root, CK_ID block) {
            auto it = walks_.find(root->GetID());
            if (it == walks_.end()) {
                std::map<CK_ID, int> positions;
                int n = 0;
                walk(root, [&](CKBehavior* b) { positions[b->GetID()] = n++; });
                it = walks_.emplace(root->GetID(), std::move(positions)).first;
            }
            auto found = it->second.find(block);
            return found == it->second.end() ? -1 : found->second;
        }
        // Reads without update (no parameter operation runs).
        static CKObject* object_of(CKContext* context, CKParameter* source) {
            if (!source) return nullptr;
            CKParameterManager* pm = context->GetParameterManager();
            if (!pm || !pm->IsDerivedFrom(source->GetGUID(), CKPGUID_OBJECT)) return nullptr;
            return source->GetValueObject(FALSE);
        }
        template <typename T> static void read_input(CKBehavior* block, int index, T& value) {
            if (index >= block->GetInputParameterCount()) return;
            CKParameterIn* in = block->GetInputParameter(index);
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            T v{};
            if (source && source->GetDataSize() == static_cast<int>(sizeof(T)) && source->GetValue(&v, FALSE) == CK_OK)
                value = v;
        }
        template <typename T> static void read_local(CKBehavior* block, int index, T& value) {
            if (index >= block->GetLocalParameterCount()) return;
            CKParameterLocal* local = block->GetLocalParameter(index);
            T v{};
            if (local && local->GetDataSize() == static_cast<int>(sizeof(T)) && local->GetValue(&v, FALSE) == CK_OK)
                value = v;
        }
        static void destroy(CKContext* context, CK_ID id) {
            if (id && context->GetObject(id)) context->DestroyObject(id);
        }
        // Step F1 (IDCP, design "As built (step WGQ7)": two CKParameterOut
        // per identity copy survived its release, their owner pointer left
        // dangling).  They are owned by the copy but not taken with it when
        // it is destroyed alone; found here by owner, one pass over the
        // class for every copy at once.
        std::map<CK_ID, std::vector<CK_ID>> copy_outputs(CKContext* context) const {
            std::map<CK_ID, std::vector<CK_ID>> out;
            std::map<CKObject*, CK_ID> copies;
            for (const identity& r: identities_)
                if (CKObject* array = r.array ? context->GetObject(r.array) : nullptr) copies.emplace(array, r.array);
            if (copies.empty()) return out;
            const int count = context->GetObjectsCountByClassID(CKCID_PARAMETEROUT);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_PARAMETEROUT);
            for (int i = 0; i < count; ++i) {
                auto* parameter = static_cast<CKParameterOut*>(context->GetObject(ids[i]));
                if (!parameter) continue;
                const auto copy = copies.find(parameter->GetOwner());
                if (copy != copies.end()) out[copy->second].push_back(ids[i]);
            }
            return out;
        }
        static void destroy_with(CKContext* context, CK_ID id, const std::vector<CK_ID>* owned) {
            if (!id || !context->GetObject(id)) return;
            std::vector<CK_ID> batch{id};
            if (owned)
                for (CK_ID o: *owned)
                    if (context->GetObject(o)) batch.push_back(o);
            context->DestroyObjects(batch.data(), static_cast<int>(batch.size()));
        }
        static bool put_back(CKContext* context, CKParameterIn* in, CK_ID direct, CK_ID shared) {
            if (shared) {
                auto* source = static_cast<CKParameterIn*>(context->GetObject(shared));
                return source && in->ShareSourceWith(source) == CK_OK;
            }
            auto* source = static_cast<CKParameter*>(direct ? context->GetObject(direct) : nullptr);
            return in->SetDirectSource(source) == CK_OK;
        }
        CKObject* retail_entity_of(CKContext* context, CK3dEntity* ball) const {
            const char* name = ball && ball->GetName() ? ball->GetName() : "";
            const std::string* best = nullptr;
            for (const std::string& type: options_.ball_names)
                if (!type.empty() && std::strncmp(name, type.c_str(), type.size()) == 0
                    && (!best || type.size() > best->size()))
                    best = &type;
            if (!best) return nullptr;
            return context->GetObjectByNameAndClass(const_cast<CKSTRING>(best->c_str()), CKCID_3DOBJECT, nullptr);
        }

        // The structural test (§2.3 "Which blocks"; the server's, PW): the
        // prototype and ObjectA's real source holding Ball_Pos_Frame.  The
        // calls are the server's own, in its order (GetValueObject with its
        // default update), so the bit-clear path reads what it always read.
        std::string rewire_probes(CKContext* context, const script_replay::selection& sel) {
            context_ = context;
            if (CKObject* frame = context->GetObjectByNameAndParentClass(const_cast<CKSTRING>("Ball_Pos_Frame"),
                                                                         CKCID_3DENTITY, nullptr))
                ball_pos_frame_ = frame->GetID();
            if (!ball_pos_frame_) {
                lines_.push_back("Ball_Pos_Frame not found; proximity union disabled");
                return options_.union_rules ? "Ball_Pos_Frame not found" : "";
            }
            const VxMatrix origin = CK3dEntity::Cast(context->GetObject(ball_pos_frame_))->GetWorldMatrix();
            const std::set<CK_ID> selected(sel.scripts.begin(), sel.scripts.end());
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            std::vector<CK_ID> list(ids, ids + count);   // the creations below grow the class list
            for (CK_ID id: list) {
                CKBehavior* block = CKBehavior::Cast(context->GetObject(id));
                if (!block || std::string_view(behavior_prototype_name(block)) != "TT Scaleable Proximity") continue;
                CKParameterIn* in = block->GetInputParameter(1);   // ObjectA
                CKParameter* source = in ? in->GetRealSource() : nullptr;
                CKObject* object = source ? source->GetValueObject() : nullptr;
                if (!object || object->GetID() != ball_pos_frame_) continue;
                CKBehavior* root = root_of(block);
                probe p;
                p.block = block->GetID();
                p.root = root->GetID();
                p.owner = owner_name(root);
                p.script = root->GetName() ? root->GetName() : "";
                if (which_ == probe_class::mechanism
                    && (!selected.count(p.root) || session::keep_local_script(p.owner, p.script)))
                    continue;
                p.walk = walk_of(root, p.block);
                p.frame_name = "BMMO_Prox_" + p.owner + "_" + std::to_string(p.walk);
                if (const auto unit = unit_blocks_.find(p.block); unit != unit_blocks_.end()) {
                    // Step Q7 (§2.10 "Names"): a unit's probe, slot k, n the
                    // original's pre-copy walk position.
                    p.slot = unit->second.slot;
                    p.walk = unit->second.walk;
                    p.frame_name = "BMMO_Prox_" + p.owner + "_" + std::to_string(p.walk) + "_S" + std::to_string(p.slot);
                }
                if (CKObject* b = block->GetInputParameterCount() > 2
                                      ? object_of(context, block->GetInputParameter(2)->GetRealSource()) : nullptr)
                    p.object_b = b->GetName() ? b->GetName() : "";
                read_input(block, 0, p.distance);
                read_input(block, 3, p.barycenter);
                read_input(block, 4, p.exact_min);
                read_input(block, 5, p.exact_max);
                read_local(block, 2, p.use_outputs);
                read_local(block, 3, p.axes);
                if (options_.union_rules && p.barycenter == 1)
                    return "block " + p.owner + "/" + p.script + " #" + std::to_string(p.walk) + " uses Barycenter";
                p.prior_direct = in->GetDirectSource() ? in->GetDirectSource()->GetID() : 0;
                p.prior_shared = in->GetSharedSource() ? in->GetSharedSource()->GetID() : 0;
                auto* frame = CK3dEntity::Cast(context->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>(p.frame_name.c_str())));
                if (!frame) {
                    if (options_.union_rules) return "CreateObject failed for " + p.frame_name;
                    continue;
                }
                if (options_.union_rules) {
                    const VxVector start(options_.reset[0], options_.reset[1], options_.reset[2]);
                    frame->SetPosition(&start);
                } else {
                    frame->SetWorldMatrix(origin);
                }
                p.frame = frame->GetID();
                CKParameterLocal* parameter = context->CreateCKParameterLocal(const_cast<CKSTRING>(p.frame_name.c_str()),
                                                                              CKPGUID_3DENTITY, TRUE);
                if (!parameter) {
                    if (options_.union_rules) {
                        probes_.push_back(p);   // released with the rest
                        return "CreateCKParameterLocal failed for " + p.frame_name;
                    }
                    continue;
                }
                p.parameter = parameter->GetID();
                CK_ID frame_id = p.frame;
                parameter->SetValue(&frame_id, sizeof(frame_id));
                if (in->SetDirectSource(parameter) != CK_OK) {
                    if (options_.union_rules) {
                        probes_.push_back(p);
                        return "SetDirectSource failed for " + p.frame_name;
                    }
                    continue;
                }
                probes_.push_back(p);
            }
            lines_.push_back(std::to_string(probes_.size()) + " proximity blocks rewired to per-player frames");
            return {};
        }

        // Every Get Cell of CurrentLevel's ActiveBall column inside a script
        // with a rewired block gets a private one-row copy of the array.  The
        // calls are the server's own, in its order.
        std::string rewire_identities(CKContext* context) {
            auto* current_level = CKDataArray::Cast(
                context->GetObjectByNameAndClass(const_cast<CKSTRING>("CurrentLevel"), CKCID_DATAARRAY, nullptr));
            if (!current_level) return options_.union_rules ? "CurrentLevel not found" : "";
            current_level_ = current_level->GetID();
            int ball_column = -1;
            for (int c = 0; c < current_level->GetColumnCount(); ++c) {
                CKSTRING column = current_level->GetColumnName(c);
                if (column && std::string_view(column) == "ActiveBall") { ball_column = c; break; }
            }
            if (ball_column < 0) {
                lines_.push_back("CurrentLevel has no ActiveBall column; ball identity union disabled");
                return options_.union_rules ? "CurrentLevel has no ActiveBall column" : "";
            }
            if (CKObject* ball = current_level->GetElementObject(0, ball_column))
                initial_entity_ = ball->GetName() ? ball->GetName() : "";
            // The mechanisms (proximity ObjectB) each rewired script watches.
            std::unordered_map<CK_ID, std::vector<CK_ID>> mechanisms;
            std::map<CK_ID, size_t> probe_index;
            for (size_t i = 0; i < probes_.size(); ++i) {
                probe_index[probes_[i].block] = i;
                auto* block = CKBehavior::Cast(context->GetObject(probes_[i].block));
                if (!block) continue;
                auto* object_b = CK3dEntity::Cast(block->GetInputParameterObject(2));
                if (!object_b) continue;
                mechanisms[probes_[i].root].push_back(object_b->GetID());
            }
            std::map<std::string, int> rewired, untouched;   // for one summary line
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            std::vector<CK_ID> list(ids, ids + count);
            for (CK_ID id: list) {
                CKBehavior* block = CKBehavior::Cast(context->GetObject(id));
                if (!block || std::string_view(behavior_prototype_name(block)) != "Get Cell") continue;
                CKParameterIn* target = block->GetTargetParameter();
                CKParameter* array_source = target ? target->GetRealSource() : nullptr;
                CKObject* array = array_source ? array_source->GetValueObject() : nullptr;
                if (!array || array->GetID() != current_level_) continue;
                CKParameterIn* column_in = block->GetInputParameterCount() > 1 ? block->GetInputParameter(1) : nullptr;
                CKParameter* column_source = column_in ? column_in->GetRealSource() : nullptr;
                int column = -1;
                if (column_source) column_source->GetValue(&column);
                if (column != ball_column) continue;
                CKBehavior* root = root_of(block);
                const std::string script = root->GetName() ? root->GetName() : "?";
                auto watched = mechanisms.find(root->GetID());
                if (watched == mechanisms.end()) {
                    // Retail gameplay logic (Ball_Shadow, Sound_Manager, the
                    // Gameplay scripts): it acts on the retail ball and has
                    // to keep doing so.
                    ++untouched[script];
                    continue;
                }
                identity r;
                r.block = block->GetID();
                r.root = root->GetID();
                r.owner = owner_name(root);
                r.script = root->GetName() ? root->GetName() : "";
                r.walk = walk_of(root, r.block);
                r.copy_name = "CurrentLevel#BMMOId_" + r.owner + "_" + std::to_string(r.walk);
                if (const auto unit = unit_blocks_.find(r.block); unit != unit_blocks_.end()) {
                    r.slot = unit->second.slot;
                    r.walk = unit->second.walk;
                    r.copy_name = "CurrentLevel#BMMOId_" + r.owner + "_" + std::to_string(r.walk) + "_S" + std::to_string(r.slot);
                }
                r.column = ball_column;
                r.references = watched->second;
                // Gating probes: a rewired probe with an output linked
                // directly to an input of the Get Cell, in its graph.
                if (CKBehavior* graph = block->GetParent()) {
                    std::set<size_t> seen;
                    for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
                        CKBehaviorLink* link = graph->GetSubBehaviorLink(i);
                        CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                        CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                        if (!from || !to || to->GetOwner() != block || from->GetType() != CK_BEHAVIORIO_OUT
                            || to->GetType() != CK_BEHAVIORIO_IN || !from->GetOwner())
                            continue;
                        auto it = probe_index.find(from->GetOwner()->GetID());
                        if (it != probe_index.end() && seen.insert(it->second).second) r.gating_all.push_back(it->second);
                    }
                }
                if (r.gating_all.size() == 1) r.gating = static_cast<int>(r.gating_all.front());
                if (r.gating >= 0 && r.slot != probes_[static_cast<size_t>(r.gating)].slot)
                    return "identity read " + r.owner + "/" + r.script + " #" + std::to_string(r.walk) + " slot "
                           + std::to_string(r.slot) + " is gated by a probe of slot "
                           + std::to_string(probes_[static_cast<size_t>(r.gating)].slot);
                if (r.gating_all.size() == 1) {
                } else if (options_.union_rules)
                    return "identity read " + r.owner + "/" + r.script + " #" + std::to_string(r.walk) + " has "
                           + std::to_string(r.gating_all.size()) + " gating probes";
                // What the block's consumers want out of the cell decides which
                // object goes in it: a name test ("Ball_Stone") wants the
                // retail entity, anything else (P_Modul_18's fan pushes the
                // ball with SetPhysicsForce and tests its box) wants the body
                // that is actually there.
                r.operation = proximity_operation_on(context, block->GetOutputParameter(0));
                r.by_name = !r.operation.empty();
                CKDependencies dependencies;
                dependencies.Resize(CKCID_MAXCLASSID);
                dependencies.Fill(0);
                dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
                dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
                // The rows come along, the objects they name are shared.
                dependencies[CKCID_DATAARRAY] = CK_DEPENDENCIES_COPY_DATAARRAY_DATA;
                const std::string suffix = r.copy_name.substr(std::strlen("CurrentLevel"));
                auto* copy = CKDataArray::Cast(context->CopyObject(current_level, &dependencies,
                                                                   const_cast<CKSTRING>(suffix.c_str())));
                if (!copy) {
                    if (options_.union_rules) return "CopyObject failed for " + r.copy_name;
                    lines_.push_back("CopyObject failed for CurrentLevel; ball identity union disabled for " + script);
                    continue;
                }
                r.array = copy->GetID();
                if (copy->GetRowCount() == 0) copy->AddRow();
                r.prior_direct = target->GetDirectSource() ? target->GetDirectSource()->GetID() : 0;
                r.prior_shared = target->GetSharedSource() ? target->GetSharedSource()->GetID() : 0;
                CKParameterLocal* parameter = context->CreateCKParameterLocal(const_cast<CKSTRING>(r.copy_name.c_str()),
                                                                              CKPGUID_DATAARRAY, TRUE);
                if (!parameter) {
                    if (options_.union_rules) {
                        destroy(context, r.array);
                        return "CreateCKParameterLocal failed for " + r.copy_name;
                    }
                    continue;   // the copy stays, as the server always left it
                }
                r.parameter = parameter->GetID();
                CK_ID array_id = r.array;
                parameter->SetValue(&array_id, sizeof(array_id));
                if (target->SetDirectSource(parameter) != CK_OK) {
                    if (options_.union_rules) {
                        identities_.push_back(r);
                        return "SetDirectSource failed for " + r.copy_name;
                    }
                    continue;
                }
                if (r.by_name)
                    if (CKObject* held = copy->GetElementObject(0, ball_column)) r.held = held->GetID();
                identities_.push_back(r);
                ++rewired[script + (r.by_name ? " (" + r.operation + ")" : " (body)")];
            }
            const auto describe_counts = [](const std::map<std::string, int>& counts) {
                std::string text;
                for (const auto& [script, n]: counts) text += (text.empty() ? "" : ", ") + script + " x" + std::to_string(n);
                return text.empty() ? std::string("none") : text;
            };
            lines_.push_back(std::to_string(identities_.size()) + " ball identity reads rewired to per-player copies ("
                             + describe_counts(rewired) + "); left on the retail array: " + describe_counts(untouched));
            return {};
        }

        std::string make_no_ball(CKContext* context) {
            // A CK3dObject (step FANX, the user's option (a) of FANP): the class
            // a Get Cell's 3D Object output takes, so a body-kind read of a cell
            // holding BMMO_NoBall carries it instead of staying id 0 (H5's dead
            // fan slot), and a 3D Object reference to it resolves by name.
            auto* entity = CK3dEntity::Cast(context->CreateObject(CKCID_3DOBJECT, const_cast<CKSTRING>("BMMO_NoBall")));
            if (!entity) return "CreateObject failed for BMMO_NoBall";
            const VxVector far_below(0.0f, -100000.0f, 0.0f);
            entity->SetPosition(&far_below);
            no_ball_ = entity->GetID();
            return {};
        }

        // (owner, script, walk) order: the signature's and the hold section's.
        void order() {
            probe_order_.resize(probes_.size());
            for (size_t i = 0; i < probes_.size(); ++i) probe_order_[i] = i;
            std::sort(probe_order_.begin(), probe_order_.end(), [this](size_t a, size_t b) {
                return std::tie(probes_[a].owner, probes_[a].script, probes_[a].walk, probes_[a].slot)
                       < std::tie(probes_[b].owner, probes_[b].script, probes_[b].walk, probes_[b].slot);
            });
            identity_order_.resize(identities_.size());
            for (size_t i = 0; i < identities_.size(); ++i) identity_order_[i] = i;
            std::sort(identity_order_.begin(), identity_order_.end(), [this](size_t a, size_t b) {
                return std::tie(identities_[a].owner, identities_[a].script, identities_[a].walk, identities_[a].slot)
                       < std::tie(identities_[b].owner, identities_[b].script, identities_[b].walk, identities_[b].slot);
            });
        }

        probe_class which_ = probe_class::all;
        rewire_options options_;
        CKContext* context_ = nullptr;
        std::vector<probe> probes_;          // creation (behaviour list) order
        std::vector<identity> identities_;   // creation order
        std::vector<size_t> probe_order_, identity_order_;
        std::map<CK_ID, std::map<CK_ID, int>> walks_;
        CK_ID ball_pos_frame_ = 0, current_level_ = 0, no_ball_ = 0;
        std::string initial_entity_;
        std::vector<std::string> lines_;
        uint64_t skipped_ = 0, relatched_ = 0;
        // ---- step Q7: the per-player units (§2.10, part I) ----
        struct unit_copy {
            std::vector<CK_ID> roots;        // the copied graph, or the unit's blocks (walk order)
            std::vector<CK_ID> links;        // added to the parent graph: the inside copies, the fan-out and fan-in links
            std::vector<CK_ID> operations;   // a block unit's operations, copied into the parent graph
        };
        struct unit_rec {
            std::string owner, script;
            CK_ID root = 0, parent = 0;      // the root script; the graph the unit and its copies live in
            int read_walk = -1;
            session::units::shape kind = session::units::shape::none;
            bool mechanism = false;          // the clients' class (the root in the selection, not keep_local)
            std::vector<uint64_t> structure; // per slot, slot 0 first (the signature's)
            std::vector<unit_copy> copies;   // slots 1..K-1
        };
        struct slot_ref {
            int slot = -1;
            int walk = -1;   // the original's pre-copy walk position
        };
        // What the copy step plans per unit before it copies anything.
        struct unit_plan {
            CK_ID root = 0;
            std::string owner, script;
            int read_walk = -1;
            session::units::unit unit;
            const session::units::graph_model* model = nullptr;
            std::vector<uint32_t> blocks;    // every block a copy takes (pre-order per copy root)
        };
        std::vector<unit_rec> units_;
        std::map<CK_ID, slot_ref> unit_blocks_;   // every block of a unit and of its copies
        std::map<CK_ID, std::string> force_alias_;
        std::vector<std::string> unit_lines_;
        uint8_t slots_ = 0;
        uint64_t unslotted_ = 0;
        size_t copy_record_bytes_ = 0;

        // The by-name copies with a hold entry (§4.2): a slot's by-name cell
        // has none (§2.10: it is BMMO_NoBall or the candidate's type, a
        // function of the tick).
        std::vector<size_t> held_identities() const {
            std::vector<size_t> out;
            for (size_t i: identity_order_)
                if (identities_[i].by_name && identities_[i].slot < 0) out.push_back(i);
            return out;
        }

        // §2.10 "Identity cell" and "Latch, body kind" for one slot read: the
        // slot's candidate (index k into `entities`, -1 for none) - its ball
        // entity (body kind) or the retail entity of its type (by-name), or
        // BMMO_NoBall; never the hold.  A body-kind output that does not name
        // the slot's candidate is moved to the cell, through the block's own
        // path; id 0 (the block never ran) is left alone (step G2's rule).
        void place_slot_identity(CKContext* context, identity& r, CKDataArray& array, int k,
                                 const std::vector<CK3dEntity*>& entities, CKObject* no_ball) {
            CK3dEntity* candidate = k >= 0 ? entities[static_cast<size_t>(k)] : nullptr;
            CKObject* value = no_ball;
            if (candidate) {
                if (!r.by_name) value = candidate;
                else if (CKObject* named = retail_entity_of(context, candidate)) value = named;
            }
            array.SetElementObject(0, r.column, value);
            if (r.by_name) return;
            auto* block = CKBehavior::Cast(context->GetObject(r.block));
            CKParameterOut* out = block && block->GetOutputParameterCount() > 0 ? block->GetOutputParameter(0) : nullptr;
            if (!out) return;
            CK_ID latched_id = 0;
            if (out->GetDataSize() != static_cast<int>(sizeof(CK_ID)) || out->GetValue(&latched_id, FALSE) != CK_OK
                || latched_id == 0)
                return;
            CKObject* latched = context->GetObject(latched_id);
            if (latched && (latched == candidate || latched == value)) return;
            block->SetOutputParameterObject(0, value);
            ++relatched_;
        }

        template <typename T> static T* object_as(CKObject* o, CK_CLASSID cid) {
            return o && CKIsChildClassOf(o, cid) ? static_cast<T*>(o) : nullptr;
        }

        // The abstract graph of one root script (session/mechanism_units.hpp):
        // every behaviour with its prototype, parent, children, parameters and
        // pre-order walk position; every graph's links (with their initial
        // delays) and parameter operations.  Read only.
        session::units::graph_model extract_model(CKContext* context, CKBehavior* root) {
            namespace mu = session::units;
            mu::graph_model m;
            const auto id_of = [](CKObject* o) { return o ? static_cast<uint32_t>(o->GetID()) : 0u; };
            const auto add_in = [&](CKParameterIn* in) {
                if (!in) return 0u;
                mu::param p;
                p.id = id_of(in);
                p.source = id_of(in->GetRealSource());
                m.params[p.id] = p;
                return p.id;
            };
            const auto add_out = [&](CKParameterOut* out) {
                if (!out) return 0u;
                mu::param p;
                p.id = id_of(out);
                for (int i = 0; i < out->GetDestinationCount(); ++i)
                    if (CKParameter* d = out->GetDestination(i)) p.destinations.push_back(id_of(d));
                m.params[p.id] = p;
                return p.id;
            };
            const auto io_of = [](CKBehaviorIO* io) {
                mu::io_ref r;
                CKBehavior* owner = io ? io->GetOwner() : nullptr;
                if (!owner) return r;
                r.block = static_cast<uint32_t>(owner->GetID());
                r.input = io->GetType() == CK_BEHAVIORIO_IN;
                r.index = r.input ? owner->GetInputPosition(io) : owner->GetOutputPosition(io);
                return r;
            };
            walk(root, [&](CKBehavior* b) {
                mu::block blk;
                blk.id = id_of(b);
                blk.prototype = behavior_prototype_name(b);
                blk.parent = id_of(b->GetParent());
                blk.walk = walk_of(root, b->GetID());
                for (int i = 0; i < b->GetInputParameterCount(); ++i) blk.inputs.push_back(add_in(b->GetInputParameter(i)));
                for (int i = 0; i < b->GetOutputParameterCount(); ++i) blk.outputs.push_back(add_out(b->GetOutputParameter(i)));
                for (int i = 0; i < b->GetLocalParameterCount(); ++i) {
                    CKParameterLocal* local = b->GetLocalParameter(i);
                    if (!local) continue;
                    mu::param p;
                    p.id = id_of(local);
                    m.params[p.id] = p;
                    blk.locals.push_back(p.id);
                }
                blk.target = add_in(b->GetTargetParameter());
                if (!b->IsUsingFunction()) {
                    for (int i = 0; i < b->GetSubBehaviorCount(); ++i)
                        if (CKBehavior* child = b->GetSubBehavior(i)) blk.children.push_back(id_of(child));
                    for (int i = 0; i < b->GetSubBehaviorLinkCount(); ++i) {
                        CKBehaviorLink* l = b->GetSubBehaviorLink(i);
                        if (!l) continue;
                        mu::link link;
                        link.id = id_of(l);
                        link.graph = blk.id;
                        link.from = io_of(l->GetInBehaviorIO());
                        link.to = io_of(l->GetOutBehaviorIO());
                        link.delay = l->GetInitialActivationDelay();
                        m.links.push_back(link);
                    }
                    for (int i = 0; i < b->GetParameterOperationCount(); ++i) {
                        CKParameterOperation* op = b->GetParameterOperation(i);
                        if (!op) continue;
                        mu::operation o;
                        o.id = id_of(op);
                        o.graph = blk.id;
                        o.name = op->GetName() ? op->GetName() : "";
                        for (CKParameterIn* in: {op->GetInParameter1(), op->GetInParameter2()})
                            if (in) o.inputs.push_back(add_in(in));
                        o.output = add_out(op->GetOutParameter());
                        m.operations[o.id] = o;
                    }
                }
                m.blocks[blk.id] = blk;
            });
            return m;
        }

        // A unit's (or a copy's) structure, world-independent (§5: the
        // signature covers every copy's structure): per block of the copied
        // subtrees in pre-order its prototype, IO counts, parameter counts and
        // child count; then every link between those blocks or into or out of
        // them (`extra`: the parent graph's links of the unit - inside, fan-out
        // and fan-in), as (end, end, delay) with an end a position in that
        // pre-order or, outside it, the owner's pre-copy walk position in the
        // root (the originals the copies are linked to); sorted, so the order
        // of a link list does not matter; then the operations' names.
        uint64_t structure_of(CKContext* context, CK_ID root_id, const std::vector<CK_ID>& roots,
                              const std::vector<CK_ID>& extra, const std::vector<CK_ID>& operations) {
            fnv f;
            CKBehavior* root = CKBehavior::Cast(context->GetObject(root_id));
            std::map<CK_ID, int> position;
            std::vector<CKBehavior*> blocks;
            for (CK_ID id: roots)
                if (auto* b = CKBehavior::Cast(context->GetObject(id)))
                    walk(b, [&](CKBehavior* x) {
                        position[x->GetID()] = static_cast<int>(blocks.size());
                        blocks.push_back(x);
                    });
            for (CKBehavior* b: blocks) {
                f.str(behavior_prototype_name(b));
                f.i32(b->GetInputCount());
                f.i32(b->GetOutputCount());
                f.i32(b->GetInputParameterCount());
                f.i32(b->GetOutputParameterCount());
                f.i32(b->GetLocalParameterCount());
                f.i32(b->IsUsingFunction() ? -1 : b->GetSubBehaviorCount());
            }
            const auto end_of = [&](CKBehaviorIO* io) {
                CKBehavior* owner = io ? io->GetOwner() : nullptr;
                if (!owner) return std::tuple<int, int, int, int>(2, -1, 0, 0);
                const int in = io->GetType() == CK_BEHAVIORIO_IN ? 1 : 0;
                const int index = in ? owner->GetInputPosition(io) : owner->GetOutputPosition(io);
                const auto it = position.find(owner->GetID());
                if (it != position.end()) return std::tuple<int, int, int, int>(0, it->second, in, index);
                return std::tuple<int, int, int, int>(1, root ? walk_of(root, owner->GetID()) : -1, in, index);
            };
            std::vector<std::tuple<int, int, int, int, int, int, int, int, int>> links;
            const auto take = [&](CKBehaviorLink* l) {
                if (!l) return;
                const auto a = end_of(l->GetInBehaviorIO()), b = end_of(l->GetOutBehaviorIO());
                links.emplace_back(std::get<0>(a), std::get<1>(a), std::get<2>(a), std::get<3>(a), std::get<0>(b),
                                   std::get<1>(b), std::get<2>(b), std::get<3>(b), l->GetInitialActivationDelay());
            };
            for (CKBehavior* b: blocks)
                if (!b->IsUsingFunction())
                    for (int i = 0; i < b->GetSubBehaviorLinkCount(); ++i) take(b->GetSubBehaviorLink(i));
            for (CK_ID id: extra) take(object_as<CKBehaviorLink>(context->GetObject(id), CKCID_BEHAVIORLINK));
            std::sort(links.begin(), links.end());
            f.u32(static_cast<uint32_t>(links.size()));
            for (const auto& t: links) {
                f.i32(std::get<0>(t)); f.i32(std::get<1>(t)); f.i32(std::get<2>(t)); f.i32(std::get<3>(t));
                f.i32(std::get<4>(t)); f.i32(std::get<5>(t)); f.i32(std::get<6>(t)); f.i32(std::get<7>(t));
                f.i32(std::get<8>(t));
            }
            std::vector<std::string> names;
            for (CK_ID id: operations)
                if (auto* op = object_as<CKParameterOperation>(context->GetObject(id), CKCID_PARAMETEROPERATION))
                    names.push_back(op->GetName() ? op->GetName() : "");
            std::sort(names.begin(), names.end());
            for (const std::string& n: names) f.str(n);
            return f.h;
        }

        // A parameter of a unit block that holds a live engine object at the
        // anchor (a force controller's or a joint's handle): §2.10 step 2's
        // hot source.  A Pointer parameter whose bytes are not all zero.
        static bool hot_parameter(CKContext* context, CKParameter* p) {
            if (!p) return false;
            CKParameterManager* pm = context->GetParameterManager();
            if (!pm || p->GetGUID() != CKPGUID_POINTER) return false;
            const int size = p->GetDataSize();
            const auto* data = static_cast<const uint8_t*>(p->GetReadDataPtr(FALSE));
            if (!data || size <= 0) return false;
            for (int i = 0; i < size; ++i)
                if (data[i]) return true;
            return false;
        }

        // A graph's children in their execution order (BehaviorGraphData::
        // m_SubBehaviors: the graph runs its active children in this order,
        // CKBehavior::CheckIOsActivation), read and written through a
        // pointer to the protected member - the public API has no way to
        // keep it (AddSubBehavior sorts).  Both SDKs declare the same member.
        struct graph_access : CKBehavior {
            static BehaviorGraphData* CKBehavior::*member() { return &graph_access::m_GraphData; }
        };
        static std::vector<CKObject*> children_of(CKBehavior* graph) {
            std::vector<CKObject*> out;
            BehaviorGraphData* data = graph ? graph->*graph_access::member() : nullptr;
            if (!data) return out;
            for (int i = 0; i < data->m_SubBehaviors.Size(); ++i) out.push_back(data->m_SubBehaviors[i]);
            return out;
        }
        // The same objects in `order` (a permutation of the children: false,
        // and nothing changed, otherwise).
        static bool set_children(CKBehavior* graph, const std::vector<CKObject*>& order) {
            BehaviorGraphData* data = graph ? graph->*graph_access::member() : nullptr;
            if (!data || data->m_SubBehaviors.Size() != static_cast<int>(order.size())) return false;
            std::vector<CKObject*> now = children_of(graph), sorted = order;
            std::sort(now.begin(), now.end());
            std::sort(sorted.begin(), sorted.end());
            if (now != sorted) return false;
            for (int i = 0; i < data->m_SubBehaviors.Size(); ++i) data->m_SubBehaviors[i] = order[static_cast<size_t>(i)];
            return true;
        }

        // A behaviour and what it owns: its IOs, parameters (inputs, outputs,
        // locals, target) and, for a graph, its links and operations with
        // theirs.
        static void owned_objects(CKBehavior* b, XObjectArray& out) {
            const auto add = [&out](CKObject* o) { if (o) out.PushBack(o->GetID()); };
            add(b);
            for (int i = 0; i < b->GetInputCount(); ++i) add(b->GetInput(i));
            for (int i = 0; i < b->GetOutputCount(); ++i) add(b->GetOutput(i));
            for (int i = 0; i < b->GetInputParameterCount(); ++i) add(b->GetInputParameter(i));
            for (int i = 0; i < b->GetOutputParameterCount(); ++i) add(b->GetOutputParameter(i));
            for (int i = 0; i < b->GetLocalParameterCount(); ++i) add(b->GetLocalParameter(i));
            add(b->GetTargetParameter());
            if (b->IsUsingFunction()) return;
            for (int i = 0; i < b->GetSubBehaviorLinkCount(); ++i) add(b->GetSubBehaviorLink(i));
            for (int i = 0; i < b->GetParameterOperationCount(); ++i) {
                CKParameterOperation* op = b->GetParameterOperation(i);
                if (!op) continue;
                add(op);
                add(op->GetInParameter1());
                add(op->GetInParameter2());
                add(op->GetOutParameter());
            }
        }

        static bool pair_walk(CKBehavior* a, CKBehavior* b, std::map<CK_ID, CK_ID>& twin) {
            if (!a || !b) return false;
            twin[a->GetID()] = b->GetID();
            if (static_cast<bool>(a->IsUsingFunction()) != static_cast<bool>(b->IsUsingFunction())) return false;
            if (a->IsUsingFunction()) return true;
            const int n = a->GetSubBehaviorCount();
            if (n != b->GetSubBehaviorCount()) return false;
            for (int i = 0; i < n; ++i)
                if (!pair_walk(a->GetSubBehavior(i), b->GetSubBehavior(i), twin)) return false;
            return true;
        }

        void register_force(CKContext* context, CK_ID id, const std::string& owner, int walk_position, int slot) {
            auto* b = CKBehavior::Cast(context->GetObject(id));
            if (!b || !script_replay::is_force_block(b)) return;
            force_alias_[id] = "unit:" + owner + "/" + std::to_string(walk_position) + "/S" + std::to_string(slot);
        }

        // §2.10 "The copy step", steps 1-5 (all or nothing: a refusal is
        // returned and rewire's release destroys what was made).  The units
        // of every identity read of the class (the server's `all`; a
        // client's mechanism class), in (owner, script, walk) order; slot 0
        // is the original; for k = 1..K-1, unit by unit, the copy made with
        // CopyObjects (a behaviour copies its IO, parameters, sub-behaviours,
        // links and operations with it), added to the unit's graph, and
        // linked by the rules (inside, fan-out, fan-in; the original's
        // initial delay).
        std::string copy_units(CKContext* context, const script_replay::selection& sel) {
            namespace mu = session::units;
            slots_ = options_.slots;
            CKObject* ball_frame = context->GetObjectByNameAndParentClass(const_cast<CKSTRING>("Ball_Pos_Frame"), CKCID_3DENTITY, nullptr);
            auto* current_level = CKDataArray::Cast(
                context->GetObjectByNameAndClass(const_cast<CKSTRING>("CurrentLevel"), CKCID_DATAARRAY, nullptr));
            if (!ball_frame || !current_level) return "the copy step: Ball_Pos_Frame or CurrentLevel not found";
            int ball_column = -1;
            for (int c = 0; c < current_level->GetColumnCount(); ++c) {
                CKSTRING column = current_level->GetColumnName(c);
                if (column && std::string_view(column) == "ActiveBall") { ball_column = c; break; }
            }
            if (ball_column < 0) return "the copy step: CurrentLevel has no ActiveBall column";
            const std::set<CK_ID> selected(sel.scripts.begin(), sel.scripts.end());
            const auto mechanism_root = [&](CKBehavior* root) {
                return selected.count(root->GetID()) != 0 && !session::keep_local_script(owner_name(root), root->GetName() ? root->GetName() : "");
            };
            // The server (class all, before its selection exists) copies every
            // identity read's unit, a client only its mechanism scripts'. The
            // sets are equal by WG0's census (every identity read is in a
            // mechanism script; measured: L11 11 = 11 units, the arena 2 = 2),
            // NOT by construction.  signature(mechanism), the one the anchors
            // compare, skips a unit classify() leaves out of the class; since
            // step F1 (UGRD) it counts such units in its tail, so a level that
            // broke the census gives the server a signature no client has and
            // every member is refused ("probe mismatch") instead of the server
            // running K copies a client lacks.
            const auto in_class = [&](CKBehavior* root) {
                if (session::keep_local_script(owner_name(root), root->GetName() ? root->GetName() : "")) return false;
                return which_ == probe_class::all || mechanism_root(root);
            };
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            const std::vector<CK_ID> list(ids, ids + count);
            // Step 1: the structural probes of the class and the identity
            // reads of their scripts, each with its one gating probe (read
            // only: no parameter operation runs).
            std::set<CK_ID> structural, roots_with_probes;
            for (CK_ID id: list) {
                CKBehavior* b = CKBehavior::Cast(context->GetObject(id));
                if (!b || std::string_view(behavior_prototype_name(b)) != "TT Scaleable Proximity") continue;
                CKParameterIn* in = b->GetInputParameterCount() > 1 ? b->GetInputParameter(1) : nullptr;
                CKParameter* source = in ? in->GetRealSource() : nullptr;
                if (!source || object_of(context, source) != ball_frame) continue;
                CKBehavior* root = root_of(b);
                if (!in_class(root)) continue;
                structural.insert(id);
                roots_with_probes.insert(root->GetID());
            }
            std::vector<unit_plan> plans;
            for (CK_ID id: list) {
                CKBehavior* b = CKBehavior::Cast(context->GetObject(id));
                if (!b || std::string_view(behavior_prototype_name(b)) != "Get Cell") continue;
                CKParameterIn* target = b->GetTargetParameter();
                if (!target || object_of(context, target->GetRealSource()) != current_level) continue;
                CKParameterIn* column_in = b->GetInputParameterCount() > 1 ? b->GetInputParameter(1) : nullptr;
                CKParameter* column_source = column_in ? column_in->GetRealSource() : nullptr;
                int column = -1;
                if (!column_source || column_source->GetDataSize() != static_cast<int>(sizeof(int))
                    || column_source->GetValue(&column, FALSE) != CK_OK || column != ball_column)
                    continue;
                CKBehavior* root = root_of(b);
                if (!roots_with_probes.count(root->GetID())) continue;
                std::set<CK_ID> gating;
                if (CKBehavior* graph = b->GetParent())
                    for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
                        CKBehaviorLink* link = graph->GetSubBehaviorLink(i);
                        CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                        CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                        if (!from || !to || to->GetOwner() != b || from->GetType() != CK_BEHAVIORIO_OUT
                            || to->GetType() != CK_BEHAVIORIO_IN || !from->GetOwner())
                            continue;
                        if (structural.count(from->GetOwner()->GetID())) gating.insert(from->GetOwner()->GetID());
                    }
                unit_plan p;
                p.root = root->GetID();
                p.owner = owner_name(root);
                p.script = root->GetName() ? root->GetName() : "";
                p.read_walk = walk_of(root, id);   // caches the root's pre-copy walk
                if (gating.size() != 1)
                    return "the copy step: identity read " + p.owner + "/" + p.script + " #" + std::to_string(p.read_walk)
                           + " has " + std::to_string(gating.size()) + " gating probes";
                p.unit.read = static_cast<uint32_t>(id);
                p.unit.probe = static_cast<uint32_t>(*gating.begin());
                plans.push_back(p);
            }
            std::sort(plans.begin(), plans.end(), [](const unit_plan& a, const unit_plan& b) {
                return std::tie(a.owner, a.script, a.read_walk) < std::tie(b.owner, b.script, b.read_walk);
            });
            // Steps 1-2: the units by the rule, refused on a third shape, an
            // overlap or a hot source.
            std::map<CK_ID, mu::graph_model> models;
            std::set<uint32_t> taken;
            for (unit_plan& p: plans) {
                CKBehavior* root = CKBehavior::Cast(context->GetObject(p.root));
                auto model = models.find(p.root);
                if (model == models.end()) model = models.emplace(p.root, extract_model(context, root)).first;
                p.model = &model->second;
                const std::string where = p.owner + "/" + p.script + " #" + std::to_string(p.read_walk);
                p.unit = mu::find_unit(model->second, p.unit.read, p.unit.probe);
                unit_lines_.push_back("unit " + where + ": " + mu::describe(model->second, p.unit));
                if (!p.unit.error.empty()) return "the unit of identity read " + where + " is refused: " + p.unit.error;
                std::set<uint32_t> copied;
                for (uint32_t r: p.unit.copy_roots()) {
                    std::set<uint32_t> sub;
                    mu::detail::subtree(model->second, r, sub);
                    copied.insert(sub.begin(), sub.end());
                }
                p.blocks.assign(copied.begin(), copied.end());
                for (uint32_t b: copied) {
                    if (!taken.insert(b).second) return "the units overlap at " + where;
                    auto* beh = CKBehavior::Cast(context->GetObject(b));
                    if (!beh) return "the unit of " + where + ": a block is gone";
                    for (int i = 0; i < beh->GetLocalParameterCount(); ++i)
                        if (hot_parameter(context, beh->GetLocalParameter(i)))
                            return "the unit of " + where + " holds a live engine object (" + behavior_prototype_name(beh) + ")";
                    for (int i = 0; i < beh->GetOutputParameterCount(); ++i)
                        if (hot_parameter(context, beh->GetOutputParameter(i)))
                            return "the unit of " + where + " holds a live engine object (" + behavior_prototype_name(beh) + ")";
                }
            }
            // Slot 0: the originals' names and aliases, their structure.
            for (const unit_plan& p: plans) {
                CKBehavior* root = CKBehavior::Cast(context->GetObject(p.root));
                unit_rec rec;
                rec.owner = p.owner;
                rec.script = p.script;
                rec.root = p.root;
                rec.read_walk = p.read_walk;
                rec.kind = p.unit.kind;
                rec.mechanism = mechanism_root(root);
                if (p.unit.kind == mu::shape::graph) {
                    auto* graph = CKBehavior::Cast(context->GetObject(p.unit.graph));
                    rec.parent = graph && graph->GetParent() ? graph->GetParent()->GetID() : 0;
                } else {
                    rec.parent = p.unit.graph;
                }
                for (uint32_t b: p.blocks) {
                    const int n = walk_of(root, b);
                    unit_blocks_[b] = {0, n};
                    register_force(context, b, p.owner, n, 0);
                }
                const std::vector<uint32_t> unit_roots = p.unit.copy_roots();
                std::vector<CK_ID> roots(unit_roots.begin(), unit_roots.end());
                std::vector<CK_ID> extra, ops(p.unit.operations.begin(), p.unit.operations.end());
                if (p.unit.kind == mu::shape::blocks)
                    for (size_t i: p.unit.inside) extra.push_back(p.model->links[i].id);
                for (size_t i: p.unit.fan_out) extra.push_back(p.model->links[i].id);
                for (size_t i: p.unit.fan_in) extra.push_back(p.model->links[i].id);
                rec.structure.push_back(structure_of(context, p.root, roots, extra, ops));
                units_.push_back(rec);
            }
            // Step 3: the copies, slot by slot, unit by unit.
            CKDependencies dependencies;
            dependencies.Resize(CKCID_MAXCLASSID);
            dependencies.Fill(0);
            dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
            dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME;   // the originals' names, unchanged
            for (uint8_t k = 1; k < slots_; ++k)
                for (size_t u = 0; u < plans.size(); ++u) {
                    const std::string error = copy_unit(context, plans[u], units_[u], k, dependencies);
                    if (!error.empty()) return error;
                }
            lines_.push_back(units_summary());
            return {};
        }

        std::string copy_unit(CKContext* context, const unit_plan& p, unit_rec& rec, uint8_t k, CKDependencies& dependencies) {
            namespace mu = session::units;
            const mu::graph_model& m = *p.model;
            const mu::unit& u = p.unit;
            const std::string where = p.owner + "/" + p.script + " #" + std::to_string(p.read_walk) + " slot " + std::to_string(k);
            CKBehavior* parent = CKBehavior::Cast(context->GetObject(rec.parent));
            if (!parent) return "the copy step: no parent graph for " + where;
            const std::vector<uint32_t> roots = u.copy_roots();
            XObjectArray sources;
            for (uint32_t id: roots) sources.PushBack(id);
            std::vector<size_t> inside;
            if (u.kind == mu::shape::blocks)
                for (size_t i: u.inside) {
                    inside.push_back(i);
                    sources.PushBack(m.links[i].id);
                }
            for (uint32_t id: u.operations) sources.PushBack(id);
            const XObjectArray copied = context->CopyObjects(sources, &dependencies, CK_OBJECTCREATION_NONAMECHECK, nullptr);
            rec.copies.emplace_back();
            unit_copy& c = rec.copies.back();
            int at = 0;
            const auto next = [&]() { return at < copied.Size() ? copied[at++] : CK_ID(0); };
            for (size_t i = 0; i < roots.size(); ++i) c.roots.push_back(next());
            for (size_t i = 0; i < inside.size(); ++i) c.links.push_back(next());
            for (size_t i = 0; i < u.operations.size(); ++i) c.operations.push_back(next());
            for (CK_ID id: c.roots)
                if (!id) return "CopyObjects failed for " + where;
            for (CK_ID id: c.links)
                if (!id) return "CopyObjects failed for a link of " + where;
            for (CK_ID id: c.operations)
                if (!id) return "CopyObjects failed for an operation of " + where;
            // Into the unit's graph: the blocks, the inside links and the
            // operations.  AddSubBehavior sorts the graph's children by
            // priority, and CK's qsort is not stable: equal priorities - every
            // block of these scripts - come out permuted, and a graph runs its
            // children in that order.  §2.10 adds each copy AFTER the
            // existing sub-behaviours, so the children are put back in their
            // order before the add with the copy last (the same in every
            // world; release's RemoveSubBehavior then leaves the pre-copy
            // order).
            for (CK_ID id: c.roots) {
                const std::vector<CKObject*> order = children_of(parent);
                CKBehavior* copy = CKBehavior::Cast(context->GetObject(id));
                if (parent->AddSubBehavior(copy) != CK_OK) return "AddSubBehavior failed for " + where;
                std::vector<CKObject*> wanted = order;
                wanted.push_back(copy);
                if (!set_children(parent, wanted)) return "the copy of " + where + " could not be put after the graph's children";
            }
            for (CK_ID id: c.links)
                if (parent->AddSubBehaviorLink(object_as<CKBehaviorLink>(context->GetObject(id), CKCID_BEHAVIORLINK)) != CK_OK)
                    return "AddSubBehaviorLink failed for " + where;
            for (CK_ID id: c.operations)
                if (parent->AddParameterOperation(object_as<CKParameterOperation>(context->GetObject(id), CKCID_PARAMETEROPERATION))
                    != CK_OK)
                    return "AddParameterOperation failed for " + where;
            // The original -> copy correspondence, block by block.
            std::map<CK_ID, CK_ID> twin;
            for (size_t i = 0; i < roots.size(); ++i)
                if (!pair_walk(CKBehavior::Cast(context->GetObject(roots[i])), CKBehavior::Cast(context->GetObject(c.roots[i])), twin))
                    return "the copy of " + where + " does not have its original's blocks";
            // The fan links (§2.10 "Links and sources").
            const auto add_link = [&](const mu::link& l, CKBehaviorIO* from, CKBehaviorIO* to) -> std::string {
                auto* original = object_as<CKBehaviorLink>(context->GetObject(l.id), CKCID_BEHAVIORLINK);
                if (!original || !from || !to) return "a link end of " + where + " not found";
                auto* link = object_as<CKBehaviorLink>(
                    context->CreateObject(CKCID_BEHAVIORLINK, original->GetName(), CK_OBJECTCREATION_NONAMECHECK), CKCID_BEHAVIORLINK);
                if (!link) return "CreateObject failed for a link of " + where;
                c.links.push_back(link->GetID());
                link->SetInBehaviorIO(from);
                link->SetOutBehaviorIO(to);
                link->SetInitialActivationDelay(original->GetInitialActivationDelay());
                link->SetActivationDelay(original->GetInitialActivationDelay());
                if (parent->AddSubBehaviorLink(link) != CK_OK) return "AddSubBehaviorLink failed for " + where;
                return {};
            };
            const auto io = [&](const mu::io_ref& r) -> CKBehaviorIO* {
                const auto t = twin.find(r.block);
                auto* b = CKBehavior::Cast(context->GetObject(t == twin.end() ? CK_ID(0) : t->second));
                if (!b) return nullptr;
                return r.input ? b->GetInput(r.index) : b->GetOutput(r.index);
            };
            for (size_t i: u.fan_out) {
                const mu::link& l = m.links[i];
                auto* original = object_as<CKBehaviorLink>(context->GetObject(l.id), CKCID_BEHAVIORLINK);
                const std::string error = add_link(l, original ? original->GetInBehaviorIO() : nullptr, io(l.to));
                if (!error.empty()) return error;
            }
            for (size_t i: u.fan_in) {
                const mu::link& l = m.links[i];
                auto* original = object_as<CKBehaviorLink>(context->GetObject(l.id), CKCID_BEHAVIORLINK);
                const std::string error = add_link(l, io(l.from), original ? original->GetOutBehaviorIO() : nullptr);
                if (!error.empty()) return error;
            }
            // Names, aliases, and what each copy adds to a record (C1).
            CKBehavior* root = CKBehavior::Cast(context->GetObject(rec.root));
            for (const auto& [original, copy]: twin) {
                const auto it = unit_blocks_.find(original);
                const int n = it == unit_blocks_.end() ? walk_of(root, original) : it->second.walk;
                unit_blocks_[copy] = {k, n};
                register_force(context, copy, p.owner, n, k);
                // The copy's parameters hold its original's values at the
                // anchor (a world-local check; values are not in the
                // signature, whose pointers differ in size between builds).
                auto* a = CKBehavior::Cast(context->GetObject(original));
                auto* b = CKBehavior::Cast(context->GetObject(copy));
                if (!a || !b || a->GetLocalParameterCount() != b->GetLocalParameterCount())
                    return "the copy of " + where + " lost a block's locals";
                for (int i = 0; i < a->GetLocalParameterCount(); ++i) {
                    CKParameterLocal* la = a->GetLocalParameter(i);
                    CKParameterLocal* lb = b->GetLocalParameter(i);
                    if (!la || !lb || la->GetDataSize() != lb->GetDataSize()) return "the copy of " + where + " changed a local";
                    const void* da = la->GetReadDataPtr(FALSE);
                    const void* db = lb->GetReadDataPtr(FALSE);
                    if (la->GetDataSize() > 0 && da && db && std::memcmp(da, db, static_cast<size_t>(la->GetDataSize())) != 0
                        && la->GetGUID() != CKPGUID_POINTER && la->GetParameterClassID() == 0)
                        return "the copy of " + where + ": local " + std::to_string(i) + " of " + behavior_prototype_name(a)
                               + " differs from its original's";
                }
            }
            for (CK_ID id: c.roots) {
                std::vector<uint8_t> bytes;
                script_replay::capture_script_block(CKBehavior::Cast(context->GetObject(id)), script_replay_hooks{}, bytes);
                copy_record_bytes_ += bytes.size() + 1;   // + the child's presence byte
            }
            std::vector<CK_ID> copy_roots(c.roots.begin(), c.roots.end());
            const uint64_t structure = structure_of(context, rec.root, copy_roots, c.links, c.operations);
            rec.structure.push_back(structure);
            unit_lines_.push_back("copy " + where + ": " + std::to_string(twin.size()) + " blocks, "
                                  + std::to_string(c.links.size()) + " links, " + std::to_string(c.operations.size())
                                  + " operations" + (structure == rec.structure.front() ? "" : " - STRUCTURE DIFFERS"));
            if (structure != rec.structure.front())
                return "the copy of " + where + " differs from its original in structure (Q10)";
            return {};
        }

        // §2.10 step 6: the slot copies with their links and operations out
        // of the units' graphs and destroyed (after the union's own sources,
        // frames and identity copies).  Safe on a partial copy step.
        void release_units(CKContext* context) {
            for (auto u = units_.rbegin(); u != units_.rend(); ++u) {
                CKBehavior* parent = CKBehavior::Cast(context->GetObject(u->parent));
                for (auto c = u->copies.rbegin(); c != u->copies.rend(); ++c) {
                    for (CK_ID id: c->links) {
                        auto* link = object_as<CKBehaviorLink>(context->GetObject(id), CKCID_BEHAVIORLINK);
                        if (link && parent) parent->RemoveSubBehaviorLink(link);
                        destroy(context, id);
                    }
                    for (CK_ID id: c->operations) {
                        auto* op = object_as<CKParameterOperation>(context->GetObject(id), CKCID_PARAMETEROPERATION);
                        if (op && parent) parent->RemoveParameterOperation(op);
                        destroy(context, id);
                    }
                    for (CK_ID id: c->roots) {
                        auto* b = CKBehavior::Cast(context->GetObject(id));
                        if (b && parent) parent->RemoveSubBehavior(b);
                        destroy(context, id);
                    }
                }
            }
            units_.clear();
            unit_blocks_.clear();
            force_alias_.clear();
        }
    };

}
