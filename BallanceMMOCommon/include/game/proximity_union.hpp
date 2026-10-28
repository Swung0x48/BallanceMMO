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
                    char name[128] = {};
                    balls->GetElementStringValue(row, 0, name, static_cast<int>(sizeof(name)));
                    name[sizeof(name) - 1] = 0;
                    o.ball_names.push_back(name);
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
            if (!probes_.empty() || !identities_.empty() || no_ball_) release(context);
            clear();
            which_ = which;
            options_ = options;
            lines_.clear();
            std::string error = rewire_probes(context, sel);
            if (error.empty()) error = rewire_identities(context);
            if (error.empty() && options_.union_rules) error = make_no_ball(context);
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
            }
            // Frames: the nearest under the block's own metric, or the hold.
            std::vector<int> choice(probes_.size(), -1);
            for (size_t i = 0; i < probes_.size(); ++i) {
                const probe& p = probes_[i];
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
            std::vector<size_t> by_name;
            for (size_t i: identity_order_)
                if (identities_[i].by_name) by_name.push_back(i);
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
            std::vector<size_t> by_name;
            for (size_t i: identity_order_)
                if (identities_[i].by_name) by_name.push_back(i);
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
            std::vector<size_t> by_name;
            for (size_t i: identity_order_)
                if (identities_[i].by_name) by_name.push_back(i);
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
            std::vector<size_t> by_name;
            for (size_t i: identity_order_)
                if (identities_[i].by_name) by_name.push_back(i);
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

        uint64_t signature(probe_class which = probe_class::mechanism) const {
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
            return f.h;
        }

        // The sources back (direct or shared, whichever was set), then the
        // frames, the copies, their parameter locals and BMMO_NoBall
        // destroyed.  Returns how many inputs were put back.  Safe to call
        // twice and on blocks that are gone (a placement copy released first).
        int release(CKContext* context) {
            int restored = 0;
            if (context) {
                for (auto it = identities_.rbegin(); it != identities_.rend(); ++it) {
                    if (auto* block = CKBehavior::Cast(context->GetObject(it->block)))
                        if (CKParameterIn* target = block->GetTargetParameter())
                            restored += put_back(context, target, it->prior_direct, it->prior_shared) ? 1 : 0;
                    destroy(context, it->parameter);
                    destroy(context, it->array);
                }
                for (auto it = probes_.rbegin(); it != probes_.rend(); ++it) {
                    if (auto* block = CKBehavior::Cast(context->GetObject(it->block)))
                        if (CKParameterIn* in = block->GetInputParameterCount() > 1 ? block->GetInputParameter(1) : nullptr)
                            restored += put_back(context, in, it->prior_direct, it->prior_shared) ? 1 : 0;
                    destroy(context, it->parameter);
                    destroy(context, it->frame);
                }
                destroy(context, no_ball_);
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

    private:
        struct probe {
            CK_ID block = 0, frame = 0, parameter = 0, root = 0;
            CK_ID prior_direct = 0, prior_shared = 0;
            std::string owner, script, frame_name, object_b;
            int walk = 0;
            float distance = 0.0f, exact_min = 0.0f, exact_max = 0.0f;
            int axes = -1, use_outputs = -1, barycenter = -1;
            bool mechanism = false;
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
                else if (options_.union_rules)
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
            auto* entity = CK3dEntity::Cast(context->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>("BMMO_NoBall")));
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
                return std::tie(probes_[a].owner, probes_[a].script, probes_[a].walk)
                       < std::tie(probes_[b].owner, probes_[b].script, probes_[b].walk);
            });
            identity_order_.resize(identities_.size());
            for (size_t i = 0; i < identities_.size(); ++i) identity_order_[i] = i;
            std::sort(identity_order_.begin(), identity_order_.end(), [this](size_t a, size_t b) {
                return std::tie(identities_[a].owner, identities_[a].script, identities_[a].walk)
                       < std::tie(identities_[b].owner, identities_[b].script, identities_[b].walk);
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
    };

}
