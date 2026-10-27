#pragma once

// Host support, the CK half (docs/session-client-core-design.md 2.4; step
// E9): platform code both hosts of the shared session-client core need
// identically, templated on a duck-typed physics backend (physics_direct on
// the headless client, physics_view on the retail mod).  The CK-free half -
// the local birth log, the event log's formats and bind_session_world - is
// session/session_host_world.hpp, which this header includes.
//
//   * matrix_from_pose: an entity's world matrix from a Physicalize's pose;
//   * make_mirror_body: a mirror's body posed, physicalized and kicked
//     (platform make_mirror);
//   * navigation_leaves, attach_mirror_navigation, attach_own_replica: the
//     shared navigation for a mirror, and the own ball's replica (design
//     9.6);
//   * manager_activations: every activation of Gameplay_SectorManager the
//     live frames made, recorded for the replay to re-issue (plan doc 10),
//     and of the trafo and keep-local scripts (steps P, A1);
//     session_script_selection, the script replay's selection of the
//     session core's world;
//   * live_cells_guard: the three data-array cells kept across a rollback
//     (platform keep_live_cells);
//   * put_back_birth: a destroyed mechanism body's entity put back where
//     its live Physicalize found it (bind_session_world's put_back);
//   * explosion_pieces: the trafo explosion pieces' entity names (step P:
//     what reportable_wakeup leaves out).
//
// Moved from the headless client (session_client.cpp at 0ea2e05) at E9,
// verbatim but for the backend's methods in place of the free calls; used
// by the headless client only until the retail host (R steps).

#include <session/client_core.hpp>
#include <session/resync_rules.hpp>
#include <session/session_host_world.hpp>
#include <session/spawn_impulse.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "CKAll.h"

#include "navigation_graph.hpp"
#include "script_replay.hpp"

namespace bmmo::game {

    inline VxMatrix matrix_from_pose(const float position[3], const float rotation[9]) {
        VxMatrix matrix;
        matrix.SetIdentity();
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) matrix[r][k] = rotation[r * 3 + k];
        matrix[3][0] = position[0];
        matrix[3][1] = position[1];
        matrix[3][2] = position[2];
        matrix[3][3] = 1.0f;
        return matrix;
    }

    inline CKDataArray* session_data_array(CKContext* context, const char* name) {
        return CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>(name), CKCID_DATAARRAY, nullptr));
    }

    // A mirror's body (platform make_mirror): the entity posed as the
    // birth says, physicalized with its recipe in the player's group, and
    // kicked with the spawn impulse the spec names.
    template <class Backend>
    bmmo::session::mirror_made make_mirror_body(Backend& backend, CK3dEntity* entity, const bmmo::session::mirror_spec& spec) {
        bmmo::session::mirror_made made;
        entity->SetWorldMatrix(matrix_from_pose(spec.position, spec.rotation));
        if (!backend.physicalize(entity->GetName(), spec.recipe, spec.group.c_str(), made.error))
            return made;
        made.made = true;
        if (spec.kick)
            made.kicked = backend.push_impulse(entity->GetName(), bmmo::session::kSpawnDirectionTable[spec.kick_index],
                                               spec.kick_strength, 0, made.kick_error);
        return made;
    }

    // The level's navigation leaves by leaf index: their directions and,
    // for the own replica, the key codes and Key Event blocks it polls.
    // Returns the leaf count.
    inline int navigation_leaves(const navigation_graph& graph, float directions[8][3], int* key_codes = nullptr,
                                 uint32_t* key_blocks = nullptr) {
        int count = 0;
        for (const auto& leaf: graph.leaves) {
            if (leaf.index < 0 || leaf.index >= 8) continue;
            directions[leaf.index][0] = leaf.direction.x;
            directions[leaf.index][1] = leaf.direction.y;
            directions[leaf.index][2] = leaf.direction.z;
            if (key_codes) key_codes[leaf.index] = leaf.key;
            if (key_blocks) key_blocks[leaf.index] = leaf.key_block;
            count = std::max(count, leaf.index + 1);
        }
        return count;
    }

    // A mirror's navigation (platform attach_mirror_navigation): the shared
    // navigation with the level's leaf directions, the ball row's force and
    // the player's camera reference.  Unavailable while the graph is not
    // valid (the host checks first that it is known).
    template <class Backend>
    bmmo::session::mirror_navigation attach_mirror_navigation(Backend& backend, const navigation_graph& graph,
                                                              const std::string& entity, uint32_t player, float force,
                                                              std::string& error) {
        if (!graph.valid()) return bmmo::session::mirror_navigation::unavailable;
        float directions[8][3] = {};
        const int count = navigation_leaves(graph, directions);
        const std::string cam_ref = "CamRef_BMMO_" + std::to_string(player);
        if (backend.navigation_create(entity.c_str(), cam_ref.c_str(), graph.ball_navigation, directions, count, force,
                                      error))
            return bmmo::session::mirror_navigation::attached;
        return bmmo::session::mirror_navigation::failed;
    }

    // The own ball's navigation replica (design 9.6): the own ball driven
    // by the replica in polling mode.  `entity` is the name it was last
    // attached to, kept after it is gone (platform own_replica_name).
    struct own_replica {
        bool active = false;
        std::string entity;
        int key_codes[8] = {};
        uint32_t key_blocks[8] = {};
        int key_count = 0;
    };
    enum class own_replica_attach : uint8_t { unavailable, moved, created, failed };

    // The replica moved to `ball_name` when one exists (the old one
    // destroyed if that fails), else created on it, polling the leaves'
    // keys; `failed` with `error` when the create or the polling failed.
    template <class Backend>
    own_replica_attach attach_own_replica(Backend& backend, const navigation_graph& graph, own_replica& replica,
                                          const std::string& ball_name, float force, std::string& error) {
        if (!graph.valid() || ball_name.empty()) return own_replica_attach::unavailable;
        if (replica.active && replica.entity != ball_name) {
            if (backend.navigation_set_ball(replica.entity.c_str(), ball_name.c_str(), force, error)) {
                replica.entity = ball_name;
                return own_replica_attach::moved;
            }
            backend.navigation_destroy(replica.entity.c_str(), error);
            replica.active = false;
        }
        float directions[8][3] = {};
        const int count = navigation_leaves(graph, directions, replica.key_codes, replica.key_blocks);
        replica.key_count = count;
        if (!backend.navigation_create(ball_name.c_str(), "CamRef_BMMO_self", graph.ball_navigation, directions, count,
                                       force, error)
                || !backend.navigation_poll(ball_name.c_str(), true, replica.key_codes, replica.key_blocks, count, error))
            return own_replica_attach::failed;
        replica.active = true;
        replica.entity = ball_name;
        return own_replica_attach::created;
    }

    // Every activation of Gameplay_SectorManager the live frames made, by
    // the tick whose frame consumed it: the union's own passes (before the
    // frame) and the ones scripts outside the replay asked for (Init
    // Ingame at the level start, Event_handler), seen as next-frame flags
    // after their frame.  The replay re-issues them at that tick, before
    // the tick's scripts, with the two IngameParameter cells as they were
    // (plan doc 10).
    //
    // Two slots per tick (step B1, session_host_world.hpp activation_slots):
    // the frame's pending activation (record_pending) and the union's pass
    // (record, from the live pass; set_pass / forget_pass, from a replay
    // that re-derives the union), re-issued pending first, then the pass.
    using manager_activation = bmmo::session::manager_activation;
    //
    // The same rule for the selected scripts that only scripts outside the
    // selection activate (script_replay::trafo_activations: the trafo
    // scripts, step P - Gameplay_Ingame's Trafo Manager activates
    // Ball_Explosion_<type> and Ball_ResetPieces_<type>, with Reset): every
    // pending activation of one of them after a live frame is recorded for
    // the frame that consumes it, reset flag included, and re-issued with the
    // manager's (replay), before that tick's scripts.  A re-issue the
    // replayed scripts make themselves as well (the explosion activating the
    // wind) sets the same flags twice: idempotent.
    //
    // The explosion's placement.  Ball_Explosion_<type> opens with "Set
    // Position (0,0,0, Referential = Ball_Pos_Frame)" on its pieces frame
    // (wrapped by the bridge: restore_explosion_pieces first, which puts
    // Ball_Pos_Frame's local matrix back to identity under its parent), so
    // the pieces start from Ball_Pos_Frame's parent - the ball entity the
    // trafo unphysicalized, where Gameplay_Ingame's Trafo Manager left it.
    // Neither that entity's matrix nor Ball_Pos_Frame's parent is replayed
    // state: by the time a rewind runs, the Trafo Manager has made the new
    // ball current and moved Ball_Pos_Frame onto it (measured, headless
    // 1183:24: every wood piece of a replayed explosion 1.16 m off its birth
    // record in y, the new ball's Physicalize height against the old ball's
    // in the trafo).  So the referential's world matrix is recorded with the
    // explosion's activation (record_pending: the parent's, the one the
    // wrapped Set Position reads, or the frame's own without a parent), and
    // inside a replay (replay's `inside`) Ball_Pos_Frame is taken off its
    // parent and put there for that tick's scripts; it goes back - parent and
    // local matrix as they were - at the next replayed tick's re-issue or when
    // the rewinds end (end_override, from live_cells_guard).
    //
    // Step T3 (trafo-replication design 4.3): with the trafo as an input
    // (WORLD_TRAFO_INPUTS) the originals are not selected
    // (session_script_selection), so session_script_activations names no
    // trafo script and neither the activations nor the referential override
    // above ever act: the sets' scripts are activated by the trafo inputs,
    // which the core re-applies in every replayed tick, and each explosion
    // is placed by its type set's own referential.  They stay for a session
    // without the option (the step P path: the legacy retail mod, a core host
    // without the capability), until R7.
    struct script_activation {
        CK_ID script = 0;
        bool reset = false;
    };
    class manager_activations {
    public:
        void clear() {
            slots_.clear();
            scripts_by_tick_.clear();
            referential_by_tick_.clear();
            scripts_reissued_ = 0;
            referentials_placed_ = 0;
            override_ = {};
        }
        // The union's live pass of `tick` (platform record_activation), with
        // the cells as run_sector left them: the tick's pass slot.
        void record(CKContext* context, uint32_t tick, bool reset) { slots_.set_pass(tick, read(context, reset)); }
        // A pass a replay re-derived for `tick` (platform set_union_pass):
        // what run_sector writes - activate `sector`, deactivate nothing,
        // reset - in the pass slot, bit for bit the live record of that pass.
        void set_pass(uint32_t tick, int sector) {
            manager_activation a;
            a.reset = true;
            a.activate = sector;
            a.deactivate = 0;
            slots_.set_pass(tick, a);
        }
        // A live pass the re-derivation did not make at `tick`; the pending
        // slot stays.  True when there was one.
        bool forget_pass(uint32_t tick) { return slots_.forget_pass(tick); }
        // The manager's pending activations of the ticks from..to dropped
        // (platform forget_pending_activations, step A5); the pass slots and
        // the scripts' activations stay.
        size_t forget_pending(uint32_t from, uint32_t to) { return slots_.forget_pending(from, to); }
        const bmmo::session::activation_slots& slots() const { return slots_; }
        // Every record at a tick >= `tick` dropped - both slots, the script
        // activations and the explosion referentials (a resync's re-assign,
        // step A3, G-a).  Returns the ticks the manager's slots dropped.
        size_t forget_from(uint32_t tick) {
            const size_t n = slots_.forget_from(tick);
            scripts_by_tick_.erase(scripts_by_tick_.lower_bound(tick), scripts_by_tick_.end());
            referential_by_tick_.erase(referential_by_tick_.lower_bound(tick), referential_by_tick_.end());
            return n;
        }
        // After a live frame (platform record_pending_activation, for the
        // frame of `tick` that consumes it): the manager's pending activation
        // (a script outside the replay asked for it) and every pending
        // activation of `scripts` (session_script_activations).
        void record_pending(CKContext* context, CK_ID manager_id, const std::vector<CK_ID>& scripts, uint32_t tick) {
            if (!context) return;
            if (auto* manager = manager_id ? CKBehavior::Cast(context->GetObject(manager_id)) : nullptr;
                    manager && (manager->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME))
                slots_.record_pending(tick, read(context, (manager->GetFlags() & CKBEHAVIOR_RESETNEXTFRAME) != 0));
            std::vector<script_activation> pending;
            for (CK_ID id: scripts) {
                auto* script = CKBehavior::Cast(context->GetObject(id));
                if (!script || !(script->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME)) continue;
                pending.push_back({id, (script->GetFlags() & CKBEHAVIOR_RESETNEXTFRAME) != 0});
                // An explosion: where its pieces start (see above).
                if (script->GetName() && std::strncmp(script->GetName(), "Ball_Explosion_", 15) == 0)
                    if (CK3dEntity* frame = pos_frame(context)) {
                        CK3dEntity* parent = frame->GetParent();
                        referential_by_tick_[tick] = parent ? parent->GetWorldMatrix() : frame->GetWorldMatrix();
                    }
            }
            if (pending.empty()) return;
            scripts_by_tick_[tick] = std::move(pending);
            while (!scripts_by_tick_.empty() && scripts_by_tick_.begin()->first + 4096 < tick)
                scripts_by_tick_.erase(scripts_by_tick_.begin());
            while (!referential_by_tick_.empty() && referential_by_tick_.begin()->first + 4096 < tick)
                referential_by_tick_.erase(referential_by_tick_.begin());
        }
        // The activations recorded for `tick` re-issued: the manager's (each
        // slot's cells, then the manager activated, reset as recorded:
        // pending first, then the pass) and the scripts'.  `inside`: a
        // replayed tick (not the live frame a rewind's end re-issues for),
        // where an explosion's referential is put where the live one was
        // (above).  Returns the manager's last one re-issued, nullptr when
        // none was recorded or the array or the manager is missing.
        const manager_activation* replay(CKContext* context, CK_ID manager_id, uint32_t tick, bool inside = false) {
            end_override(context);
            CKScene* scene = context ? context->GetCurrentScene() : nullptr;
            if (const auto r = referential_by_tick_.find(tick); inside && r != referential_by_tick_.end())
                if (CK3dEntity* frame = pos_frame(context)) {
                    override_.frame = frame->GetID();
                    CK3dEntity* parent = frame->GetParent();
                    override_.parent = parent ? parent->GetID() : 0;
                    override_.local = frame->GetLocalMatrix();
                    override_.active = true;
                    if (parent) frame->SetParent(nullptr, TRUE);
                    frame->SetWorldMatrix(r->second);
                    ++referentials_placed_;
                }
            if (const auto s = scripts_by_tick_.find(tick); s != scripts_by_tick_.end() && scene) {
                for (const script_activation& a: s->second)
                    if (auto* script = CKBehavior::Cast(context->GetObject(a.script))) {
                        scene->Activate(script, a.reset ? TRUE : FALSE);
                        ++scripts_reissued_;
                    }
            }
            const std::vector<const manager_activation*> due = slots_.at(tick);
            if (due.empty()) return nullptr;
            CKDataArray* parameters = session_data_array(context, "IngameParameter");
            CKBehavior* manager = manager_id ? CKBehavior::Cast(context->GetObject(manager_id)) : nullptr;
            if (!parameters || !manager) return nullptr;
            for (const manager_activation* a: due) {
                int activate = a->activate, deactivate = a->deactivate;
                parameters->SetElementValue(0, 1, &activate, sizeof(int));
                parameters->SetElementValue(0, 2, &deactivate, sizeof(int));
                if (scene) scene->Activate(manager, a->reset ? TRUE : FALSE);
            }
            return due.back();
        }
        // Ball_Pos_Frame back on the parent it had, with the local matrix
        // it had, after a replayed explosion's tick (above).  Idempotent.
        void end_override(CKContext* context) {
            if (!override_.active) return;
            override_.active = false;
            auto* frame = context ? CK3dEntity::Cast(context->GetObject(override_.frame)) : nullptr;
            if (!frame) return;
            auto* parent = override_.parent ? CK3dEntity::Cast(context->GetObject(override_.parent)) : nullptr;
            if (parent) frame->SetParent(parent, TRUE);
            frame->SetLocalMatrix(override_.local);
        }
        // How many script activations replay() re-issued, and explosion
        // referentials it placed (diagnostics).
        uint64_t scripts_reissued() const { return scripts_reissued_; }
        uint64_t referentials_placed() const { return referentials_placed_; }

    private:
        static CK3dEntity* pos_frame(CKContext* context) {
            return CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>("Ball_Pos_Frame"),
                                                                           CKCID_3DENTITY, nullptr));
        }
        // The manager's activation as the cells stand now.
        static manager_activation read(CKContext* context, bool reset) {
            manager_activation a;
            a.reset = reset;
            if (CKDataArray* parameters = session_data_array(context, "IngameParameter")) {
                parameters->GetElementValue(0, 1, &a.activate);
                parameters->GetElementValue(0, 2, &a.deactivate);
            }
            return a;
        }
        bmmo::session::activation_slots slots_;
        std::map<uint32_t, std::vector<script_activation>> scripts_by_tick_;
        std::map<uint32_t, VxMatrix> referential_by_tick_;
        uint64_t scripts_reissued_ = 0, referentials_placed_ = 0;
        struct {
            bool active = false;
            CK_ID frame = 0, parent = 0;
            VxMatrix local;
        } override_;
    };

    // The script replay's selection of the session core's world, both hosts
    // (step P): the mechanism scripts, the sector manager and the trafo
    // scripts.  The legacy retail rollback calls script_replay::select
    // without the trafo scripts.  `trafo_inputs` (a session with
    // WORLD_TRAFO_INPUTS, trafo-replication design 4.3/4.5): the originals
    // never run from the anchor on and are not selected; the sets' scripts
    // travel beside the selection as the trafo part (game/trafo_sets.hpp
    // trafo_registry::part).
    inline script_replay::selection session_script_selection(CKContext* context, bool trafo_inputs = false) {
        return script_replay::select(context, !trafo_inputs);
    }

    // The platform's localize_scripts (step A3, resync design 2.6.4 step 4),
    // one implementation for both hosts: the server's portable record as a
    // local v2 record of `sel`, the core's keep-local rule taking those
    // scripts from `anchor` (the synthetic anchor's own record).
    // `trafo` (step T3): the world's trafo part (a session with the trafo as
    // an input), whose scripts the server's portable record (version 2)
    // carries by name; null without one.
    inline bool localize_session_scripts(CKContext* context, const script_replay::selection& sel,
                                         const std::vector<uint8_t>& portable, const std::vector<uint8_t>* anchor,
                                         std::vector<uint8_t>& out, std::string& error,
                                         bmmo::session::resync_script_report& report,
                                         const script_replay::trafo_part* trafo = nullptr) {
        script_replay::portable_report r;
        r.lists = true;
        const script_replay::keep_local_fn keep = [](const std::string& owner, const std::string& script) {
            return bmmo::session::keep_local_script(owner, script);
        };
        const bool ok = script_replay::localize(context, sel, portable, anchor, keep, out, &error, &r, trafo);
        report.scripts = r.scripts;
        report.kept_local = r.kept_local;
        report.objects_kept = r.objects_kept;
        report.objects_resolved = r.objects_resolved;
        report.unportable = r.unportable;
        report.ambiguous = r.ambiguous;
        report.first_unportable = r.unportable_list.empty() ? std::string() : r.unportable_list.front();
        return ok;
    }

    // The tick each script a resync's re-assign forgets was told to reset
    // at (a host's script_reset_ticks): ticks >= `tick` dropped (step A3).
    inline size_t forget_reset_ticks_from(std::map<CK_ID, uint32_t>& reset_ticks, uint32_t tick) {
        size_t n = 0;
        for (auto it = reset_ticks.begin(); it != reset_ticks.end();) {
            if (static_cast<int32_t>(it->second - tick) >= 0) {
                it = reset_ticks.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        return n;
    }
    // The scripts of that selection whose pending activations the host
    // records and the replay re-issues (manager_activations::record_pending):
    // kept by the host beside the selection.  The trafo scripts
    // (script_replay::trafo_activations, step P), and the keep-local scripts
    // (session/resync_rules.hpp keep_local_script; resync design 2.8, step
    // A1): a resync re-derivation that forgets a live sector pass (the
    // server's union already had the sector at S) would otherwise never
    // activate the balloon's scripts, which the resync keeps from the
    // client's own anchor, where they never ran.  The manager's own replayed
    // walk activates them through the same next-frame flags (Activate Script,
    // CKScene::Activate), so where both re-issue it the flags are set twice:
    // idempotent.
    inline std::vector<CK_ID> session_script_activations(CKContext* context, const script_replay::selection& sel) {
        std::vector<CK_ID> out = script_replay::trafo_activations(context, sel);
        if (!context) return out;
        for (CK_ID id: sel.scripts) {
            auto* script = CKBehavior::Cast(context->GetObject(id));
            if (!script || std::find(out.begin(), out.end(), id) != out.end()) continue;
            CKBeObject* owner = script->GetOwner();
            if (bmmo::session::keep_local_script(owner && owner->GetName() ? owner->GetName() : "",
                                                 script->GetName() ? script->GetName() : ""))
                out.push_back(id);
        }
        return out;
    }

    // The three data-array cells the replay rewinds and re-writes (the
    // record's, then the replayed activations') while the LIVE chains that
    // wrote them - Gameplay_Events' crossing, the death chain - are not
    // replayed: kept across a rewind, so the live world ends the frame
    // with the values its own chains left, and only the replay sees the
    // anchor's (the next live pass writes them anyway; this keeps the
    // engine's end-of-replay script check honest about them).  `keep`
    // false (--park-births): nothing kept.  A rollback in its scope
    // re-issues, at its end, the activation of the frame about to run
    // (`reissue`).
    class live_cells_guard {
    public:
        live_cells_guard(CKContext* context, const bmmo::session::session_client_core& core, bool keep,
                         std::function<void(uint32_t tick)> reissue, manager_activations* activations = nullptr)
            : context_(context), core_(core), rollbacks_before_(core.rollback_stats().rollbacks), reissue_(std::move(reissue)),
              activations_(activations) {
            if (!keep) return;
            for (int i = 0; i < 3; ++i) have_[i] = read_cell(i, values_[i]);
        }
        ~live_cells_guard() {
            // A replayed explosion's referential, if the window ended on its
            // tick (manager_activations::end_override).
            if (activations_) activations_->end_override(context_);
            // Adopt mode (resync design 4.6, step B1): a rewind that
            // re-derived the union replayed the passes at the ticks they now
            // have, so the cells it ended with are the right ones - the phase
            // a module branches on, the sector the pass wrote (the live
            // chain's own crossing is a stamp already: the frame that saw it
            // polled it), the deactivate the pass cleared.  The live values
            // are the passes' of the other history: not written back.  The
            // core re-reads the activate cell afterwards (sector_cell_adopted).
            if (!core_.last_rewind_rederived())
                for (int i = 0; i < 3; ++i) if (have_[i]) write_cell(i, values_[i]);
            // A rewind that ended at `now` replaced the live world with the
            // replayed one, which never saw an activation a script outside
            // the replay asked for during the live frame `now` (Init Ingame
            // at the level start, Event_handler): pending as next-frame
            // flags in the live record of `now`, absent from the replayed
            // state.  Re-issued for the frame about to run, as the replay
            // re-issues them inside its window.
            if (core_.rollback_stats().rollbacks != rollbacks_before_ && reissue_) reissue_(core_.current_tick() + 1);
        }
        live_cells_guard(const live_cells_guard&) = delete;
        live_cells_guard& operator=(const live_cells_guard&) = delete;

    private:
        static const char* cell_array(int i) { return i == 0 ? "CurrentLevel" : "IngameParameter"; }
        static int cell_column(int i) { return i == 0 ? 4 : i; }
        bool read_cell(int i, int& value) {
            CKDataArray* a = session_data_array(context_, cell_array(i));
            return a && a->GetElementValue(0, cell_column(i), &value);
        }
        void write_cell(int i, int value) {
            if (CKDataArray* a = session_data_array(context_, cell_array(i))) a->SetElementValue(0, cell_column(i), &value, sizeof(int));
        }

        CKContext* context_;
        const bmmo::session::session_client_core& core_;
        uint64_t rollbacks_before_ = 0;
        std::function<void(uint32_t tick)> reissue_;
        manager_activations* activations_ = nullptr;
        int values_[3] = {};
        bool have_[3] = {};
    };

    // A mechanism body destroyed at a restore (bind_session_world's
    // put_back): its entity back at the matrix its live Physicalize found -
    // the LOCAL one for a parented entity (the replayed manager's Set World
    // Matrix on the parent then recomputes the same world matrix), the
    // world one otherwise.  (The scene's initial conditions are NOT it:
    // they are the level file's poses, before the manager placed the
    // module - measured: a box 900 m off, the ball fell to its death.)
    inline void put_back_birth(CKContext* context, const std::string& entity, const bmmo::session::local_birth& birth) {
        if (auto* ck = CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(entity.c_str()),
                                                                              CKCID_3DENTITY, nullptr))) {
            VxMatrix matrix;
            const bool local = birth.has_parent && birth.has_local && ck->GetParent();
            const float* source = local ? birth.local_matrix : birth.matrix;
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) matrix[r][k] = source[r * 4 + k];
            if (local) ck->SetLocalMatrix(matrix);
            else ck->SetWorldMatrix(matrix);
        }
    }

    // The trafo explosion pieces (step P): every entity under the three
    // Ball_*Pieces_Frame hierarchies of Balls.nmo, the bodies
    // Ball_Explosion_<type> physicalizes and pushes and Ball_ResetPieces_<type>
    // gives back (physics_state.cpp restore_explosion_pieces walks the same
    // frames).  They share the "Ball_" prefix with the balls but are rigid
    // bodies like any mechanism, so both hosts track them, and the replay
    // re-runs the scripts that make, push and take them (session_script_
    // selection), so a rollback destroys and re-makes them like any
    // mechanism born inside its window.  What they are told apart for: no
    // other world has them (the server and the other clients run no
    // explosion of this client's trafo), so their Physicalize's wake-up is
    // not a BodyRevived (reportable_wakeup).  With the trafo as an input
    // (step T3) every world runs every set's scripts: the hosts add every
    // set's pieces (game/trafo_sets.hpp add_trafo_set_pieces), which are no
    // BodyRevived because every world makes them.
    inline std::set<std::string> explosion_pieces(CKContext* context) {
        std::set<std::string> names;
        if (!context) return names;
        static const char* const kFrames[] = {"Ball_WoodPieces_Frame", "Ball_StonePieces_Frame", "Ball_PaperPieces_Frame"};
        for (const char* frame_name: kFrames) {
            auto* frame = CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(frame_name),
                                                                                 CKCID_3DENTITY, nullptr));
            if (!frame) continue;
            for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child))
                if (child->GetName()) names.insert(child->GetName());
        }
        return names;
    }

}
