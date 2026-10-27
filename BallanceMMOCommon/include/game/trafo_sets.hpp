#pragma once

// The per-player trafo piece sets (docs/trafo-replication-design.md 2.3,
// 2.4, 2.5, 2.7, 2.8, 4.5, 4.7; step T1): host support for the trafo as a
// lockstep input, the CK half.  The CK-free half - the event's flags, the
// seed, the server's ledger, the input log - is session/trafo_input.hpp.
//
//   * quiesce_originals: the level's own three piece sets taken to the state
//     every world shares at the anchor (no body, no wind controller, no
//     listener, no script running or pending), logged with what it removed;
//   * build_trafo_set: one member's copy of all three piece types and the 8
//     scripts that drive them (explosions, resets, wind, collision sound),
//     owned by an inactive BMMO_TrafoOwner_<p>, each explosion placed by its
//     own BMMO_TrafoRef_<T>_<p>, checked against invariants I0-I7 (fail
//     closed, the offending object named) and logged with a digest of its
//     reference map (names, not ids);
//   * trafo_hold: the originals' activations cancelled after every live frame
//     and reported (kind, type, position) - the explosion predicted from the
//     Trafo Manager's 1000 ms Delayer (decision 9.5, option E) when it
//     starts, the activation itself otherwise - and the effect checks
//     (trafo_effect_check: no original piece with a body, no original script
//     active);
//   * run_trafo: one input applied - the referential and the seed of an
//     explosion, the next-frame flags of the explosion or the reset;
//   * the trafo list (trafo_registry::part) and the trafo frame that runs it
//     (trafo_frame), sets by player id, the copied order inside;
//   * unmake_type_set / remake_type_set: the reconcile's two halves (4.7);
//   * retire_trafo_sets, teardown_trafo_sets: when sets go (2.4).
//
// Public CK2 API only, like sector_activation.hpp: the header compiles
// against the Virtools SDK (the retail mod) and against the Ballanced engine
// (the headless hosts, SimTool).  What only the physics bridge can do
// (bodies, the hooked Random and Set Position blocks, the guard's exemption
// list, the filter's piece registry, the seeds) goes through
// trafo_set_hooks, which each host fills from its own bridge (physics_direct
// on the headless side, physics_view in the mod).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <physics/physics_rt_api.h>
#include <session/trafo_input.hpp>

#include "CKAll.h"

#include "menu_driver.hpp"
#include "script_replay.hpp"

namespace bmmo::game {

    // ---- names (design 2.4)

    // The originals' names by piece type (0 wood, 1 stone, 2 paper).
    inline std::string trafo_frame_name(uint8_t type) {
        return std::string("Ball_") + bmmo::session::trafo_type_name(type) + "Pieces_Frame";
    }
    // The pieces' group (the explosion's and the reset's group loop); the
    // wood and paper materials share the name (stone has two materials,
    // "_Inside" and "_Outside": the materials are taken from the pieces'
    // meshes, trafo_piece_materials).
    inline std::string trafo_pieces_name(uint8_t type) {
        return std::string("Ball_") + bmmo::session::trafo_type_name(type) + "_Pieces";
    }
    inline std::string trafo_explosion_name(uint8_t type) {
        return std::string("Ball_Explosion_") + bmmo::session::trafo_type_name(type);
    }
    inline std::string trafo_reset_name(uint8_t type) {
        return std::string("Ball_ResetPieces_") + bmmo::session::trafo_type_name(type);
    }
    constexpr const char* kTrafoWindName = "Ball_PaperWind";
    constexpr const char* kTrafoCollSoundName = "Ball_CollSound_Pieces";
    constexpr const char* kTrafoScriptOwnerName = "All_Balls";
    // A copy's name: the original's, then "_Trafo<player>".
    inline std::string trafo_copy_suffix(uint32_t player) { return "_Trafo" + std::to_string(player); }
    inline std::string trafo_owner_name(uint32_t player) { return "BMMO_TrafoOwner_" + std::to_string(player); }
    inline std::string trafo_ref_name(uint8_t type, uint32_t player) {
        return std::string("BMMO_TrafoRef_") + bmmo::session::trafo_type_name(type) + "_" + std::to_string(player);
    }

    // The bridge's side (physics_state.hpp trafo_set_*, bridge API v21) and
    // the bodies, as the host reaches them.
    struct trafo_set_hooks {
        std::function<bool(const std::string& entity)> has_body;
        std::function<bool(const std::string& entity, std::string& error)> unphysicalize;
        // The originals' restore (physics_state.hpp restore_explosion_pieces).
        std::function<void()> restore_originals;
        std::function<bool(uint32_t player, const uint32_t frames[3], const std::vector<uint32_t>& explosions,
                           bmmo_physics_trafo_set_report& out, std::string& error)> register_set;
        std::function<bool(uint32_t player, std::string& error)> unregister_set;
        std::function<void(uint32_t frame, int32_t seed)> seed;
        std::function<int(uint32_t frame, std::string& error)> unmake;
        // The force blocks' controllers (capture_force / restore_force).
        script_replay_hooks forces;
        std::function<void(const std::string& line)> log;
    };

    // One member's set (design 2.4).  Every id is this world's.
    struct trafo_type_set {
        CK_ID frame = 0, ref = 0, group = 0, explosion = 0, reset = 0;
        std::vector<CK_ID> pieces, materials;
        bool final = false;   // the set's TRAFO_FINAL reset of this type was applied
    };
    struct trafo_set {
        uint32_t player = 0;
        CK_ID owner = 0;
        trafo_type_set types[bmmo::session::kTrafoTypes];
        CK_ID wind = 0, collsound = 0;
        std::vector<CK_ID> scripts;    // the 8, in All_Balls' order
        // Every force block of the scripts, in their order and walk order:
        // the k-th names its controller "trafo:<player>/<k>" in a savestate
        // (trafo_controller_alias), the same in every world.
        std::vector<CK_ID> force_blocks;
        std::vector<CK_ID> objects;    // every object the builder made: what destroy removes
        std::map<CK_ID, std::vector<uint8_t>> built;   // each script's block bytes right after the build
        bool selected = true;
        uint32_t final_tick = 0;       // the newest TRAFO_FINAL reset's applied tick
        // Retirement (retire_trafo_sets): the set has been inert at every
        // retirement check since `inert_since` (each live tick's, after the
        // frame); reset when a check finds it live again.
        bool inert = false;
        uint32_t inert_since = 0;
        bool deselected = false;
        uint32_t deselected_at = 0;
        std::string digest;            // the reference map's digest (names, not ids)
        std::string value_digest;      // the built values' digest (trafo_value_digest: names, not ids)
    };

    // The level reference (design 2.4 step 6, as built at T1's review): the
    // 8 original scripts' runtime values - every block's non-opaque locals
    // and outputs and its active inputs and outputs - as the level file has
    // them, captured once per level when the scripts first exist, before any
    // original explosion (capture_trafo_reference; the host's job: the
    // server and the headless client at their world's level load, the retail
    // host when the level is loaded).  CopyObjects copies the originals'
    // CURRENT values, which are the last run's: a world whose originals
    // exploded before the anchor (a retail player who played the level, or
    // anyone after the level's own uses) built copies with other outputs,
    // other fed locals, another script input state - measured at T1's
    // review: a paper explosion before the anchor left 11 runtime values
    // different (the Random outputs, Group Iterator's and Identity's
    // outputs, W?ndOn?, the Paper explosion's In 0), and a wood or stone one
    // left PhysicsCollDetection's Entity output naming the floor, which
    // failed I2.  The builder writes the reference into every copy, so every
    // world's copies start from the level file's values, whatever ran before.
    struct trafo_reference {
        struct value {
            uint8_t kind = 0;              // 0 skipped (opaque, empty), 1 bytes, 2 an object reference
            std::vector<uint8_t> bytes;    // kind 1
            CK_ID object = 0;              // kind 2: an original's or an outside object's id (0: none)
        };
        struct block {
            std::string name;
            uint32_t in_mask = 0, out_mask = 0;
            std::vector<value> locals, outs;
        };
        std::map<std::string, std::vector<block>> scripts;   // original script name -> its blocks in walk order
        bool empty() const { return scripts.empty(); }
    };

    // The registry: a value type each host keeps (the server per room, the
    // headless client, the retail host's file-scope static).  `part()` is the
    // trafo list with the built states: what the trafo frame runs and record
    // v3 carries (script_replay::trafo_part); it is rebuilt whenever the
    // selected sets change.
    class trafo_registry {
    public:
        std::vector<trafo_set>& sets() { return sets_; }
        const std::vector<trafo_set>& sets() const { return sets_; }
        trafo_set* find(uint32_t player) {
            for (auto& s: sets_) if (s.player == player) return &s;
            return nullptr;
        }
        const trafo_set* find(uint32_t player) const {
            for (const auto& s: sets_) if (s.player == player) return &s;
            return nullptr;
        }
        const script_replay::trafo_part& part() const { return part_; }
        // The selected sets' scripts in player-id order, their built states.
        void rebuild_part() {
            std::sort(sets_.begin(), sets_.end(), [](const trafo_set& a, const trafo_set& b) { return a.player < b.player; });
            part_ = {};
            force_names_.clear();
            for (const auto& s: sets_)
                for (size_t k = 0; k < s.force_blocks.size(); ++k)
                    force_names_[s.force_blocks[k]] = "trafo:" + std::to_string(s.player) + "/" + std::to_string(k);
            for (const auto& s: sets_) {
                if (!s.selected) continue;
                for (CK_ID id: s.scripts) {
                    part_.scripts.push_back(id);
                    const auto b = s.built.find(id);
                    if (b != s.built.end()) part_.built[id] = b->second;
                }
            }
        }
        void add(trafo_set set) {
            sets_.push_back(std::move(set));
            rebuild_part();
        }
        void remove(uint32_t player) {
            sets_.erase(std::remove_if(sets_.begin(), sets_.end(), [player](const trafo_set& s) { return s.player == player; }),
                        sets_.end());
            rebuild_part();
        }
        void clear() {
            sets_.clear();
            part_ = {};
            force_names_.clear();
        }
        // The world-independent name of a force block of a set, or null
        // (trafo_controller_alias).
        const std::string* force_name(CK_ID block) const {
            const auto it = force_names_.find(block);
            return it == force_names_.end() ? nullptr : &it->second;
        }
        // The level reference every build writes into its copies (one per
        // level: a new level's load replaces it, before any set is built).
        const trafo_reference& reference() const { return reference_; }
        void set_reference(trafo_reference reference) { reference_ = std::move(reference); }

    private:
        std::vector<trafo_set> sets_;
        script_replay::trafo_part part_;
        trafo_reference reference_;
        std::map<CK_ID, std::string> force_names_;   // every set's force blocks, selected or not
    };

    // The savestate's controller alias over a registry (physics_savestate.hpp
    // set_controller_alias; step T3's fix-up): a set's force controller,
    // "rt:force/<block id>" in this world, is "trafo:<player>/<k>" in every
    // world - the k-th force block of that player's copies.  Every other
    // name is kept.
    inline bool trafo_controller_alias(const trafo_registry& registry, const char* name, std::string& alias) {
        static constexpr char kPrefix[] = "rt:force/";
        if (!name || std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0) return false;
        char* end = nullptr;
        const unsigned long id = std::strtoul(name + sizeof(kPrefix) - 1, &end, 10);
        if (!end || *end) return false;
        const std::string* found = registry.force_name(static_cast<CK_ID>(id));
        if (!found) return false;
        alias = *found;
        return true;
    }

    namespace trafo_detail {
        inline const CKGUID& activate_script_guid() {
            static const CKGUID guid(0x4c7e7bc3, 0x0b693155);   // Activate Script (Narratives)
            return guid;
        }
        inline const CKGUID& coll_detection_guid() {
            static const CKGUID guid(0x7435488d, 0x201d1188);   // PhysicsCollDetection (physics_RT)
            return guid;
        }
        template <typename F> void walk(CKBehavior* beh, F&& fn) {
            if (!beh) return;
            fn(beh);
            if (beh->IsUsingFunction()) return;
            const int children = beh->GetSubBehaviorCount();
            for (int i = 0; i < children; ++i) walk(beh->GetSubBehavior(i), fn);
        }
        inline CK3dEntity* entity(CKContext* context, const std::string& name) {
            return CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
        }
        inline CKObject* object_of_class(CKContext* context, const std::string& name, CK_CLASSID cid) {
            return context->GetObjectByNameAndClass(const_cast<CKSTRING>(name.c_str()), cid, nullptr);
        }
        // The original root script of All_Balls of that name (the copies
        // share the prefix; the originals' owner is All_Balls).
        inline CKBehavior* original_script(CKContext* context, const std::string& name) {
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                CKBehavior* beh = as_behavior(context->GetObject(ids[i]));
                if (!beh || beh->GetParent() || !beh->GetName() || name != beh->GetName()) continue;
                CKBeObject* owner = beh->GetOwner();
                if (owner && owner->GetName() && std::strcmp(owner->GetName(), kTrafoScriptOwnerName) == 0) return beh;
            }
            return nullptr;
        }
        // The 8 originals in All_Balls' script order (the order the behaviour
        // manager runs them in), or fewer when a level lacks one.
        inline std::vector<CKBehavior*> original_scripts(CKContext* context) {
            std::vector<CKBehavior*> out;
            std::vector<std::string> names;
            for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
                names.push_back(trafo_explosion_name(t));
                names.push_back(trafo_reset_name(t));
            }
            names.emplace_back(kTrafoWindName);
            names.emplace_back(kTrafoCollSoundName);
            auto* owner = CKBeObject::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(kTrafoScriptOwnerName),
                                                                                  CKCID_BEOBJECT, nullptr));
            if (owner) {
                for (int i = 0; i < owner->GetScriptCount(); ++i) {
                    CKBehavior* script = owner->GetScript(i);
                    if (script && script->GetName() && std::find(names.begin(), names.end(), script->GetName()) != names.end())
                        out.push_back(script);
                }
            }
            if (out.size() == names.size()) return out;
            out.clear();   // no owner list to go by: by name, in the list's order
            for (const auto& n: names)
                if (CKBehavior* s = original_script(context, n)) out.push_back(s);
            return out;
        }
        inline void null_pointer_locals(CKBehavior* beh, int& nulled) {
            const int locals = beh->GetLocalParameterCount();
            for (int i = 0; i < locals; ++i) {
                CKParameterLocal* p = beh->GetLocalParameter(i);
                if (!p || p->GetGUID() != CKPGUID_POINTER) continue;
                void* value = nullptr;
                if (p->GetDataSize() != static_cast<int>(sizeof(void*))) continue;
                std::memcpy(&value, p->GetReadDataPtr(FALSE), sizeof(void*));
                if (!value) continue;
                void* none = nullptr;
                if (void* buffer = p->GetWriteDataPtr()) std::memcpy(buffer, &none, sizeof(void*));
                ++nulled;
            }
        }
        inline void* pointer_local(CKBehavior* beh, int index = 0) {
            void* value = nullptr;
            if (beh->GetLocalParameterCount() > index) beh->GetLocalParameterValue(index, &value);
            return value;
        }
        inline CKBehavior* root_of(CKObject* object) {
            CKBehavior* beh = nullptr;
            if (!object) return nullptr;
            if (object->GetClassID() == CKCID_BEHAVIOR) beh = static_cast<CKBehavior*>(object);
            else if (CKIsChildClassOf(object, CKCID_PARAMETER) || CKIsChildClassOf(object, CKCID_PARAMETEROUT)
                     || CKIsChildClassOf(object, CKCID_PARAMETERLOCAL) || CKIsChildClassOf(object, CKCID_PARAMETERIN)) {
                CKObject* owner = nullptr;
                if (CKIsChildClassOf(object, CKCID_PARAMETERIN)) owner = static_cast<CKParameterIn*>(object)->GetOwner();
                else owner = static_cast<CKParameter*>(object)->GetOwner();
                if (owner && CKIsChildClassOf(owner, CKCID_PARAMETEROPERATION))
                    owner = static_cast<CKParameterOperation*>(owner)->GetOwner();
                if (owner && owner->GetClassID() == CKCID_BEHAVIOR) beh = static_cast<CKBehavior*>(owner);
            }
            while (beh && beh->GetParent()) beh = beh->GetParent();
            return beh;
        }
        // FNV-1a 64 over text.
        inline uint64_t fnv64(const std::string& text, uint64_t h = 1469598103934665603ull) {
            for (unsigned char c: text) {
                h ^= c;
                h *= 1099511628211ull;
            }
            return h;
        }
        // A name with this player's suffixes taken out, for the digest.
        inline std::string normalized(const std::string& name, uint32_t player) {
            std::string out = name;
            const std::string suffix = trafo_copy_suffix(player);
            for (size_t at; (at = out.find(suffix)) != std::string::npos;) out.replace(at, suffix.size(), "_Trafo#");
            const std::string tail = "_" + std::to_string(player);
            if (out.rfind("BMMO_Trafo", 0) == 0 && out.size() > tail.size()
                    && out.compare(out.size() - tail.size(), tail.size(), tail) == 0)
                out.replace(out.size() - tail.size(), tail.size(), "_#");
            return out;
        }
        inline std::string name_of(CKObject* o) { return o && o->GetName() ? o->GetName() : std::string(); }
    }

    // The materials a piece frame's pieces are drawn with (every mesh of
    // every piece), in first-seen order: what the reset's fade and Restore
    // IC act on (stone has two, Ball_Stone_Pieces_Inside / _Outside).
    inline std::vector<CKMaterial*> trafo_piece_materials(CK3dEntity* frame) {
        std::vector<CKMaterial*> out;
        if (!frame) return out;
        const auto add_mesh = [&out](CKMesh* mesh) {
            if (!mesh) return;
            for (int m = 0; m < mesh->GetMaterialCount(); ++m)
                if (CKMaterial* material = mesh->GetMaterial(m); material && std::find(out.begin(), out.end(), material) == out.end())
                    out.push_back(material);
        };
        for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child)) {
            for (int m = 0; m < child->GetMeshCount(); ++m) add_mesh(child->GetMesh(m));
            add_mesh(child->GetCurrentMesh());
        }
        return out;
    }

    // ---- the quiesce (design 2.3, "When the hold is armed")
    struct trafo_quiesce_report {
        int bodies = 0, controllers = 0, scripts = 0, listeners = 0, materials = 0;
        std::string text() const {
            char line[200];
            std::snprintf(line, sizeof(line), "trafo quiesce: %d bodies, %d controllers, %d scripts (%d listeners, %d materials)",
                          bodies, controllers, scripts, listeners, materials);
            return line;
        }
    };
    // The originals taken to the state every world has at the anchor: every
    // original piece body unphysicalized, the original wind's controllers
    // deleted (force hook, `exists = false`, which NULLs local 0), the
    // original collision sound's listeners dropped (its blocks' reset), the
    // 8 originals' next-frame flags cleared and the scripts deactivated, the
    // three materials back at their initial conditions (a fade the anchor
    // interrupted), then restore_explosion_pieces.  The same function in
    // every world; it removes nothing in the server's and the headless
    // worlds (a non-zero count there is a bug).
    inline trafo_quiesce_report quiesce_originals(CKContext* context, const trafo_set_hooks& hooks) {
        trafo_quiesce_report r;
        if (!context) return r;
        CKScene* scene = context->GetCurrentScene();
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            CK3dEntity* frame = trafo_detail::entity(context, trafo_frame_name(t));
            if (!frame) continue;
            for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child)) {
                const std::string name = trafo_detail::name_of(child);
                std::string error;
                if (hooks.has_body && hooks.has_body(name) && hooks.unphysicalize && hooks.unphysicalize(name, error)) ++r.bodies;
            }
        }
        for (CKBehavior* script: trafo_detail::original_scripts(context)) {
            trafo_detail::walk(script, [&](CKBehavior* beh) {
                if (script_replay::is_force_block(beh) && hooks.forces.capture_force) {
                    force_block_state state;
                    if (hooks.forces.capture_force(beh, state) && state.exists) {
                        force_block_state none;
                        if (hooks.forces.restore_force && hooks.forces.restore_force(beh, none)) ++r.controllers;
                    }
                }
                if (beh->IsUsingFunction() && beh->GetPrototypeGuid() == trafo_detail::coll_detection_guid()
                        && trafo_detail::pointer_local(beh)) {
                    beh->CallCallbackFunction(CKM_BEHAVIORRESET);
                    ++r.listeners;
                }
            });
            const CKDWORD pending = CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME;
            if (script->IsActive() || (script->GetFlags() & pending)) ++r.scripts;
            script->ModifyFlags(0, pending);
            if (script->IsActive()) script->Activate(FALSE, FALSE);
        }
        if (scene)
            for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t)
                for (CKMaterial* material: trafo_piece_materials(trafo_detail::entity(context, trafo_frame_name(t)))) {
                    if (!material->IsInScene(scene)) continue;
                    if (CKStateChunk* chunk = scene->GetObjectInitialValue(material)) {
                        CKReadObjectState(material, chunk);
                        ++r.materials;
                    }
                }
        if (hooks.restore_originals) hooks.restore_originals();
        return r;
    }

    // ---- the builder (design 2.4)
    struct trafo_build_result {
        bool ok = false;
        std::string error;               // the failed invariant and the object it names
        std::vector<std::string> lines;  // the build's log: list, extras, invariants, digest
        int extras_destroyed = 0;
    };

    namespace trafo_detail {
        // What CopyObjects makes of the list, and what it must not touch:
        // the copies share every reference outside the list (Ball_Pos_Frame,
        // sounds, textures), remap every one inside (targets, groups,
        // materials, the wind's targets, the cross-script W?ndOn? link, the
        // collision sound's targets), keep the hierarchy among the copies,
        // and pull nothing in: no BeObject's scripts (All_Balls' others), no
        // texture, no membership of the originals' other groups.
        inline void copy_dependencies(CKDependencies& deps) {
            CKCopyDefaultClassDependencies(deps, CK_DEPENDENCIES_COPY);
            deps.ModifyOptions(CKCID_OBJECT, 0, CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME);
            deps.ModifyOptions(CKCID_BEOBJECT, 0, CK_DEPENDENCIES_COPY_BEOBJECT_SCRIPTS | CK_DEPENDENCIES_COPY_BEOBJECT_GROUPS);
            deps.ModifyOptions(CKCID_MESH, CK_DEPENDENCIES_COPY_MESH_MATERIAL, 0);
            deps.ModifyOptions(CKCID_MATERIAL, 0, CK_DEPENDENCIES_COPY_MATERIAL_TEXTURE);
            deps.ModifyOptions(CKCID_3DENTITY, CK_DEPENDENCIES_COPY_3DENTITY_MESH | CK_DEPENDENCIES_COPY_3DENTITY_CHILDREN,
                               CK_DEPENDENCIES_COPY_3DENTITY_ANIMATIONS);
            deps.ModifyOptions(CKCID_GROUP, CK_DEPENDENCIES_COPY_GROUP_OBJECTS, 0);
            deps.m_Flags = CK_DEPENDENCIES_CUSTOM;
        }
        inline std::set<CK_ID> all_object_ids(CKContext* context) {
            std::set<CK_ID> ids;
            const XObjectPointerArray& all = context->GetObjectListByType(CKCID_OBJECT, TRUE);
            for (int i = 0; i < all.Size(); ++i)
                if (CKObject* o = all[i]) ids.insert(o->GetID());
            return ids;
        }
        inline const char* kind_of(CKObject* o) {
            if (!o) return "?";
            if (o->GetClassID() == CKCID_BEHAVIOR) return "script";
            if (CKIsChildClassOf(o, CKCID_MESH)) return "mesh";
            if (CKIsChildClassOf(o, CKCID_MATERIAL)) return "material";
            if (CKIsChildClassOf(o, CKCID_TEXTURE)) return "texture";
            if (CKIsChildClassOf(o, CKCID_GROUP)) return "group";
            if (CKIsChildClassOf(o, CKCID_3DENTITY)) return "entity";
            if (CKIsChildClassOf(o, CKCID_BEOBJECT)) return "beobject";
            return "other";
        }
        // A new object the copy made that is a part of a copied script (a
        // block, a parameter, an IO, a link, an operation): owned, not an
        // extra.  Everything else new is a root kind: a root script or a
        // BeObject (entities, meshes, materials, textures, groups).
        inline bool root_kind(CKObject* o) {
            if (!o) return false;
            if (o->GetClassID() == CKCID_BEHAVIOR) return static_cast<CKBehavior*>(o)->GetParent() == nullptr;
            return CKIsChildClassOf(o, CKCID_BEOBJECT) != FALSE;
        }
    }

    namespace trafo_detail {
        inline uint32_t io_mask(CKBehavior* beh, bool inputs) {
            uint32_t mask = 0;
            const int n = inputs ? beh->GetInputCount() : beh->GetOutputCount();
            for (int i = 0; i < n && i < 32; ++i)
                if (inputs ? beh->IsInputActive(i) : beh->IsOutputActive(i)) mask |= 1u << i;
            return mask;
        }
        // An object-typed parameter (its value is one CK_ID).
        inline bool object_parameter(CKParameter* p) {
            return p && p->GetParameterClassID() && p->GetDataSize() == static_cast<int>(sizeof(CK_ID));
        }
        inline trafo_reference::value reference_value(CKParameter* p) {
            trafo_reference::value v;
            if (!p || script_replay::opaque_parameter(p)) return v;
            const int size = p->GetDataSize();
            const void* data = size > 0 ? p->GetReadDataPtr(FALSE) : nullptr;
            if (!data) return v;
            if (object_parameter(p)) {
                v.kind = 2;
                std::memcpy(&v.object, data, sizeof(CK_ID));
                return v;
            }
            v.kind = 1;
            v.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            return v;
        }
    }

    // The level reference of the 8 originals (see trafo_reference).  Call it
    // when the level's scripts first exist and before any original
    // explosion.  False (and `out` empty) while the level has fewer than the
    // 8 scripts, or when one is running.
    inline bool capture_trafo_reference(CKContext* context, trafo_reference& out, std::string& error) {
        using namespace trafo_detail;
        out = {};
        error.clear();
        if (!context) return (error = "no context", false);
        const std::vector<CKBehavior*> scripts = original_scripts(context);
        if (scripts.size() != 8) return (error = "the level has " + std::to_string(scripts.size()) + " of the 8 trafo scripts", false);
        trafo_reference ref;
        for (CKBehavior* script: scripts) {
            if (script->IsActive()) return (error = name_of(script) + " is running", false);
            auto& blocks = ref.scripts[name_of(script)];
            walk(script, [&](CKBehavior* beh) {
                trafo_reference::block b;
                b.name = name_of(beh);
                b.in_mask = io_mask(beh, true);
                b.out_mask = io_mask(beh, false);
                for (int i = 0; i < beh->GetLocalParameterCount(); ++i) b.locals.push_back(reference_value(beh->GetLocalParameter(i)));
                for (int i = 0; i < beh->GetOutputParameterCount(); ++i) b.outs.push_back(reference_value(beh->GetOutputParameter(i)));
                blocks.push_back(std::move(b));
            });
        }
        out = std::move(ref);
        return true;
    }

    // The reference written into one copied script (the builder, right
    // after the copy): an object reference is mapped the way CopyObjects
    // maps it (an original in the list to its copy, anything else kept).
    // Values are written through the buffer, never with SetValue (no
    // propagation into linked locals, script_replay's restore rule).
    // `rewritten` counts the values and IO flags that differed.  False, with
    // the place, when the copy's graph does not match the reference.
    inline bool apply_trafo_reference(CKContext* context, const trafo_reference& ref, const std::string& original_name,
                                      CKBehavior* copy, const std::map<CK_ID, CK_ID>& copy_of, int& rewritten, std::string& error) {
        using namespace trafo_detail;
        const auto it = ref.scripts.find(original_name);
        if (it == ref.scripts.end()) return (error = "the level reference has no " + original_name, false);
        const auto& blocks = it->second;
        size_t k = 0;
        bool ok = true;
        walk(copy, [&](CKBehavior* beh) {
            if (!ok) return;
            const size_t index = k++;
            if (index >= blocks.size() || (index > 0 && blocks[index].name != name_of(beh))
                    || static_cast<int>(blocks[index].locals.size()) != beh->GetLocalParameterCount()
                    || static_cast<int>(blocks[index].outs.size()) != beh->GetOutputParameterCount()) {
                ok = false;
                error = "the copy of " + original_name + " does not match the level reference at block " + std::to_string(index)
                      + " (" + name_of(beh) + ")";
                return;
            }
            const auto& b = blocks[index];
            const auto put = [&](CKParameter* p, const trafo_reference::value& v, const char* what, int i) {
                if (!ok || !p || v.kind == 0 || script_replay::opaque_parameter(p)) return;
                void* buffer = p->GetWriteDataPtr();
                const int size = p->GetDataSize();
                if (!buffer) return;
                if (v.kind == 2) {
                    if (!object_parameter(p)) {
                        ok = false;
                        error = original_name + " block " + std::to_string(index) + " " + what + std::to_string(i) + " is no object reference now";
                        return;
                    }
                    CK_ID id = v.object;
                    if (const auto m = copy_of.find(id); m != copy_of.end()) id = m->second;
                    else if (id && !context->GetObject(id)) id = 0;   // a referenced object that is gone: none
                    if (std::memcmp(buffer, &id, sizeof(CK_ID)) != 0) {
                        std::memcpy(buffer, &id, sizeof(CK_ID));
                        ++rewritten;
                    }
                    return;
                }
                if (size != static_cast<int>(v.bytes.size())) {
                    ok = false;
                    error = original_name + " block " + std::to_string(index) + " (" + name_of(beh) + ") " + what + std::to_string(i)
                          + " has " + std::to_string(size) + " bytes, the level reference " + std::to_string(v.bytes.size());
                    return;
                }
                if (size > 0 && std::memcmp(buffer, v.bytes.data(), static_cast<size_t>(size)) != 0) {
                    std::memcpy(buffer, v.bytes.data(), static_cast<size_t>(size));
                    ++rewritten;
                }
            };
            for (int i = 0; i < beh->GetLocalParameterCount(); ++i) put(beh->GetLocalParameter(i), b.locals[static_cast<size_t>(i)], "local", i);
            for (int i = 0; i < beh->GetOutputParameterCount(); ++i) put(beh->GetOutputParameter(i), b.outs[static_cast<size_t>(i)], "out", i);
            for (int i = 0; i < beh->GetInputCount() && i < 32; ++i) {
                const bool want = (b.in_mask >> i) & 1u;
                if ((beh->IsInputActive(i) != FALSE) != want) {
                    beh->ActivateInput(i, want ? TRUE : FALSE);
                    ++rewritten;
                }
            }
            for (int i = 0; i < beh->GetOutputCount() && i < 32; ++i) {
                const bool want = (b.out_mask >> i) & 1u;
                if ((beh->IsOutputActive(i) != FALSE) != want) {
                    beh->ActivateOutput(i, want ? TRUE : FALSE);
                    ++rewritten;
                }
            }
        });
        if (ok && k != blocks.size()) {
            error = "the copy of " + original_name + " has " + std::to_string(k) + " blocks, the level reference " + std::to_string(blocks.size());
            return false;
        }
        return ok;
    }

    // Below the builder.
    inline std::vector<std::string> trafo_reference_map(CKContext* context, const std::vector<CK_ID>& scripts, uint32_t player);
    inline std::string trafo_value_digest(CKContext* context, const std::vector<CK_ID>& scripts, uint32_t player);
    inline std::string trafo_reference_digest(const std::vector<std::string>& lines);

    // One member's set (design 2.4 steps 0-8, invariants I0-I7).  Call it
    // after quiesce_originals, outside any replay.  Fail closed: on a failed
    // invariant every object the call made is destroyed again and the result
    // names the invariant and the object.
    inline trafo_build_result build_trafo_set(CKContext* context, trafo_registry& registry, uint32_t player,
                                              const trafo_set_hooks& hooks) {
        using namespace trafo_detail;
        trafo_build_result result;
        const auto line = [&result](const std::string& text) { result.lines.push_back(text); };
        const auto failed = [&](const std::string& why) {
            result.ok = false;
            result.error = why;
            line("FAIL " + why);
            return result;
        };
        if (!context) return failed("no context");
        if (registry.find(player)) return failed("player " + std::to_string(player) + " has a set already");
        if (registry.reference().empty())
            return failed("no level reference of the trafo scripts (capture_trafo_reference when the level is loaded)");
        CKScene* scene = context->GetCurrentScene();
        CKLevel* level = context->GetCurrentLevel();
        if (!scene) return failed("no current scene");
        const std::string suffix = trafo_copy_suffix(player);

        // ---- the list
        struct type_list { CK3dEntity* frame = nullptr; CKGroup* group = nullptr; std::vector<CKMaterial*> materials; std::vector<CK3dEntity*> pieces; };
        type_list originals[bmmo::session::kTrafoTypes];
        std::vector<CKBehavior*> scripts = original_scripts(context);
        if (scripts.size() != 8) return failed("the level has " + std::to_string(scripts.size()) + " of the 8 trafo scripts");
        std::vector<CKObject*> list;
        std::set<CK_ID> meshes_seen;
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            type_list& o = originals[t];
            o.frame = entity(context, trafo_frame_name(t));
            o.group = CKGroup::Cast(object_of_class(context, trafo_pieces_name(t), CKCID_GROUP));
            if (!o.frame) return failed("no " + trafo_frame_name(t));
            if (!o.group) return failed("no group " + trafo_pieces_name(t));
            o.materials = trafo_piece_materials(o.frame);
            if (o.materials.empty()) return failed("the pieces of " + trafo_frame_name(t) + " have no material");
            list.push_back(o.frame);
            for (CK3dEntity* child = o.frame->HierarchyParser(nullptr); child; child = o.frame->HierarchyParser(child)) {
                o.pieces.push_back(child);
                list.push_back(child);
                for (int m = 0; m < child->GetMeshCount(); ++m)
                    if (CKMesh* mesh = child->GetMesh(m); mesh && meshes_seen.insert(mesh->GetID()).second) list.push_back(mesh);
                if (CKMesh* mesh = child->GetCurrentMesh(); mesh && meshes_seen.insert(mesh->GetID()).second) list.push_back(mesh);
            }
            list.push_back(o.group);
            for (CKMaterial* m: o.materials) list.push_back(m);
        }
        for (CKBehavior* s: scripts) list.push_back(s);
        {   // a material two types share is listed once
            std::set<CK_ID> seen;
            list.erase(std::remove_if(list.begin(), list.end(), [&seen](CKObject* o) { return !seen.insert(o->GetID()).second; }),
                       list.end());
        }
        std::map<std::string, int> listed_kinds;
        for (CKObject* o: list) ++listed_kinds[kind_of(o)];
        {
            std::string kinds;
            for (const auto& [k, n]: listed_kinds) kinds += " " + k + "=" + std::to_string(n);
            line("trafo set " + std::to_string(player) + ": list of " + std::to_string(list.size()) + ":" + kinds);
        }
        // I7 (before the copy: what the copy inherits)
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t)
            for (CK3dEntity* p: originals[t].pieces)
                if (hooks.has_body && hooks.has_body(name_of(p))) return failed("I7: original piece " + name_of(p) + " has a body");
        for (CKBehavior* s: scripts) {
            if (s->IsActive() || (s->GetFlags() & (CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME)))
                return failed("I7: original script " + name_of(s) + " is active or pending");
            std::string bad;
            walk(s, [&](CKBehavior* beh) {
                if (script_replay::is_force_block(beh) && pointer_local(beh)) bad = name_of(s) + "/" + name_of(beh);
            });
            if (!bad.empty()) return failed("I7: original force block " + bad + " holds a controller");
        }

        // ---- the copy
        const std::set<CK_ID> before = all_object_ids(context);
        XObjectArray sources;
        for (CKObject* o: list) sources.PushBack(o->GetID());
        CKDependencies deps;
        copy_dependencies(deps);
        const XObjectArray& copied = context->CopyObjects(sources, &deps, CK_OBJECTCREATION_NONAMECHECK, nullptr);
        // Fail closed from here on: before the renaming the copies carry the
        // originals' exact names, so one left behind would answer every later
        // lookup by name (the quiesce, the hold, has_body, the next build's
        // I4).  Every root object the copy made is destroyed again.
        const auto undo_copy = [&](const std::string& why) {
            std::vector<CK_ID> made;
            for (CK_ID id: all_object_ids(context))
                if (!before.count(id) && root_kind(context->GetObject(id))) made.push_back(id);
            for (CK_ID id: made)   // a copied script leaves the owner the copy gave it first, as step 0 does
                if (CKBehavior* b = CKBehavior::Cast(context->GetObject(id)))
                    if (CKBeObject* owner = b->GetOwner()) owner->RemoveScript(id);
            if (!made.empty()) {
                CKDependencies none;
                context->DestroyObjects(made.data(), static_cast<int>(made.size()), 0, &none);
            }
            return failed(why + " (" + std::to_string(made.size()) + " made object(s) destroyed)");
        };
        std::map<CK_ID, CK_ID> copy_of;   // original -> copy
        if (copied.Size() != sources.Size()) return undo_copy("I0: the copy returned " + std::to_string(copied.Size()) + " of "
                                                              + std::to_string(sources.Size()) + " objects");
        for (int i = 0; i < sources.Size(); ++i) {
            if (!copied[i] || copied[i] == sources[i]) return undo_copy("I0: " + name_of(context->GetObject(sources[i])) + " was not copied");
            copy_of[sources[i]] = copied[i];
        }
        const auto copy = [&](CKObject* original) { return original ? context->GetObject(copy_of[original->GetID()]) : nullptr; };
        trafo_set set;
        set.player = player;
        std::set<CK_ID> mine;   // every copy of the list
        for (const auto& [o, c]: copy_of) mine.insert(c);
        // Step 5 first (before any owner change, whose DETACH callbacks would
        // delete what a copied pointer points at): no borrowed controller,
        // listener or joint - every pointer local of every copied block NULL.
        int nulled = 0;
        for (CKBehavior* s: scripts)
            walk(CKBehavior::Cast(copy(s)), [&](CKBehavior* beh) { null_pointer_locals(beh, nulled); });
        // Step 6's values: the level file's, not the last run's (see
        // trafo_reference) - before I2 reads the copies' references.
        int rewritten = 0;
        for (CKBehavior* s: scripts) {
            std::string why;
            if (!apply_trafo_reference(context, registry.reference(), name_of(s), CKBehavior::Cast(copy(s)), copy_of, rewritten, why))
                return undo_copy("I0: " + why);
        }
        line("level reference: " + std::to_string(rewritten) + " runtime value(s) and IO flag(s) put back to the level file's");
        // Step 0: the extras out - new root objects that are not copies of
        // the list (an All_Balls copy and its other scripts, a texture, a
        // material the meshes pulled in), destroyed before anything goes into
        // the scene.
        {
            std::vector<CK_ID> extras;
            std::string names;
            for (CK_ID id: all_object_ids(context)) {
                if (before.count(id) || mine.count(id)) continue;
                CKObject* o = context->GetObject(id);
                if (!root_kind(o)) continue;
                extras.push_back(id);
                if (result.extras_destroyed < 12) names += " " + std::string(kind_of(o)) + ":" + name_of(o);
                ++result.extras_destroyed;
            }
            for (CKBehavior* s: scripts) {   // the copies leave whatever owner the copy gave them
                CKBehavior* c = CKBehavior::Cast(copy(s));
                if (CKBeObject* owner = c->GetOwner()) owner->RemoveScript(c->GetID());
            }
            if (!extras.empty()) {
                CKDependencies none;
                context->DestroyObjects(extras.data(), static_cast<int>(extras.size()), 0, &none);
            }
            line("I0: copied " + std::to_string(copy_of.size()) + " objects, " + std::to_string(result.extras_destroyed)
                 + " extra(s) destroyed" + names + ", " + std::to_string(nulled) + " pointer local(s) nulled");
            // The copies, by kind, against the list.
            std::map<std::string, int> kinds;
            for (CK_ID id: mine) ++kinds[kind_of(context->GetObject(id))];
            if (kinds != listed_kinds) return undo_copy("I0: the copies' kinds differ from the list's");
            int leftovers = 0;
            for (CK_ID id: all_object_ids(context))
                if (!before.count(id) && !mine.count(id) && root_kind(context->GetObject(id))) ++leftovers;
            if (leftovers) return undo_copy("I0: " + std::to_string(leftovers) + " extra root object(s) survived the destroy");
        }
        const auto destroy_made = [&]() {
            std::vector<CK_ID> ids(mine.begin(), mine.end());
            if (set.owner) ids.push_back(set.owner);
            for (auto& ts: set.types) if (ts.ref) ids.push_back(ts.ref);
            CKDependencies none;
            context->DestroyObjects(ids.data(), static_cast<int>(ids.size()), 0, &none);
        };
        const auto abort = [&](const std::string& why) {
            destroy_made();
            return failed(why);
        };

        // Names (I4's rule): every copy is "<original>_Trafo<p>".
        for (const auto& [o, c]: copy_of) {
            CKObject* original = context->GetObject(o);
            CKObject* made = context->GetObject(c);
            const std::string name = name_of(original) + suffix;
            made->SetName(const_cast<CKSTRING>(name.c_str()));
        }

        // Step 1-2: the scripts' owner, inactive, and the referentials.
        auto* owner = CK3dEntity::Cast(context->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>(trafo_owner_name(player).c_str())));
        if (!owner) return abort("cannot create " + trafo_owner_name(player));
        set.owner = owner->GetID();
        if (level) level->AddObject(owner);
        scene->AddObjectToScene(owner, FALSE);
        if (scene->IsObjectActive(owner)) scene->DeActivate(owner);
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            auto* ref = CK3dEntity::Cast(context->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>(trafo_ref_name(t, player).c_str())));
            if (!ref) return abort("cannot create " + trafo_ref_name(t, player));
            set.types[t].ref = ref->GetID();
            if (level) level->AddObject(ref);
            scene->AddObjectToScene(ref, FALSE);
        }
        for (CKBehavior* s: scripts) {
            CKBehavior* c = CKBehavior::Cast(copy(s));
            const CKERROR err = owner->AddScript(c);
            if (err != CK_OK) return abort("AddScript " + name_of(c) + " failed (" + std::to_string(err) + ")");
            set.scripts.push_back(c->GetID());
        }
        // Every copy in the current scene (the IC restore and ActivateScript
        // need it), no copied script running or pending, the owner inactive.
        for (CK_ID id: mine) {
            auto* so = CKSceneObject::Cast(context->GetObject(id));
            if (so && !so->IsInScene(scene)) scene->AddObjectToScene(so, FALSE);
        }
        for (CK_ID id: set.scripts) {
            CKBehavior* c = CKBehavior::Cast(context->GetObject(id));
            c->ModifyFlags(0, CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME | CKBEHAVIOR_DEACTIVATENEXTFRAME);
            if (c->IsActive()) c->Activate(FALSE, FALSE);
            if (!c->IsInScene(scene)) scene->AddObjectToScene(c, FALSE);
        }
        if (scene->IsObjectActive(owner)) scene->DeActivate(owner);

        // The set's parts by type.
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            trafo_type_set& ts = set.types[t];
            ts.frame = copy_of[originals[t].frame->GetID()];
            ts.group = copy_of[originals[t].group->GetID()];
            for (CKMaterial* m: originals[t].materials) ts.materials.push_back(copy_of[m->GetID()]);
            for (CK3dEntity* p: originals[t].pieces) ts.pieces.push_back(copy_of[p->GetID()]);
        }
        for (CKBehavior* s: scripts) {
            const std::string n = name_of(s);
            const CK_ID c = copy_of[s->GetID()];
            for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
                if (n == trafo_explosion_name(t)) set.types[t].explosion = c;
                if (n == trafo_reset_name(t)) set.types[t].reset = c;
            }
            if (n == kTrafoWindName) set.wind = c;
            if (n == kTrafoCollSoundName) set.collsound = c;
        }

        // Step 3: no owner wake-ups.  Step 4: each explosion's referential.
        int awake_off = 0, referentials = 0;
        for (CK_ID id: set.scripts)
            walk(CKBehavior::Cast(context->GetObject(id)), [&](CKBehavior* beh) {
                if (!beh->IsUsingFunction() || beh->GetPrototypeGuid() != activate_script_guid()) return;
                CKBOOL awake = FALSE;
                beh->SetLocalParameterValue(0, &awake);
                ++awake_off;
            });
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            CKBehavior* explosion = CKBehavior::Cast(context->GetObject(set.types[t].explosion));
            bool placed = false;
            for (int i = 0; i < explosion->GetLocalParameterCount(); ++i) {
                CKParameterLocal* p = explosion->GetLocalParameter(i);
                if (!p || !p->GetName() || std::strcmp(p->GetName(), "Referential") != 0) continue;
                CK_ID ref = set.types[t].ref;
                p->SetValue(&ref);
                placed = true;
                ++referentials;
            }
            if (!placed) return abort("step 4: " + name_of(explosion) + " has no Referential local");
        }
        line("steps 3-4: " + std::to_string(awake_off) + " Activate Script block(s) with Awake Object FALSE, "
             + std::to_string(referentials) + " referential(s) placed");

        // Step 7: the bridge.
        {
            uint32_t frames[3] = {set.types[0].frame, set.types[1].frame, set.types[2].frame};
            std::vector<uint32_t> explosions;
            for (const auto& ts: set.types) explosions.push_back(ts.explosion);
            bmmo_physics_trafo_set_report bridge{};
            std::string error;
            if (!hooks.register_set || !hooks.register_set(player, frames, explosions, bridge, error))
                return abort("I5: the bridge refused the set: " + error);
            size_t pieces = 0;
            for (const auto& ts: set.types) pieces += ts.pieces.size();
            line("I5: bridge randoms=" + std::to_string(bridge.randoms) + " set_positions=" + std::to_string(bridge.set_positions)
                 + " exempt=" + std::to_string(bridge.exempt) + " pieces=" + std::to_string(bridge.pieces));
            if (bridge.randoms != 5 || bridge.set_positions != 3 || bridge.exempt != static_cast<int>(3 + pieces)
                    || bridge.pieces != static_cast<int>(pieces)) {
                std::string ignored;
                if (hooks.unregister_set) hooks.unregister_set(player, ignored);
                return abort("I5: the bridge reports " + std::to_string(bridge.randoms) + " Random, " + std::to_string(bridge.set_positions)
                             + " Set Position, " + std::to_string(bridge.exempt) + " exempt, " + std::to_string(bridge.pieces)
                             + " pieces (expected 5, 3, " + std::to_string(3 + pieces) + ", " + std::to_string(pieces) + ")");
            }
        }
        const auto unregister_and_abort = [&](const std::string& why) {
            std::string ignored;
            if (hooks.unregister_set) hooks.unregister_set(player, ignored);
            return abort(why);
        };

        // Step 6: initial conditions - every copied frame, piece, group and
        // material saved as it stands (flat, exact, opaque).
        int ics = 0;
        for (CK_ID id: mine) {
            CKObject* o = context->GetObject(id);
            if (!o || o->GetClassID() == CKCID_BEHAVIOR || CKIsChildClassOf(o, CKCID_MESH)) continue;
            auto* so = CKSceneObject::Cast(o);
            if (!so) continue;
            scene->SetObjectInitialValue(so, CKSaveObjectState(o));
            ++ics;
        }

        // ---- invariants I1-I4, I6, I7 (I0 and I5 above)
        for (CK_ID id: mine) {
            auto* so = CKSceneObject::Cast(context->GetObject(id));
            if (so && !so->IsInScene(scene)) return unregister_and_abort("I1: " + name_of(so) + " is not in the current scene");
        }
        if (!owner->IsInScene(scene)) return unregister_and_abort("I1: the owner is not in the current scene");
        if (scene->IsObjectActive(owner) || owner->IsActiveInCurrentScene())
            return unregister_and_abort("I1: the owner is active in the current scene");
        for (CK_ID id: set.scripts) {
            CKBehavior* c = CKBehavior::Cast(context->GetObject(id));
            if (c->GetOwner() != owner) return unregister_and_abort("I1: " + name_of(c) + " is not owned by " + trafo_owner_name(player));
        }
        // I2: every object reference resolves inside the set or to the
        // allow-list; every parameter link stays inside the set's scripts.
        std::set<CK_ID> allowed = mine;
        allowed.insert(set.owner);
        for (const auto& ts: set.types) allowed.insert(ts.ref);
        std::set<CK_ID> set_scripts(set.scripts.begin(), set.scripts.end());
        std::string violation;
        const auto check_value = [&](CKParameter* p, const std::string& place) {
            if (!p || !violation.empty() || !p->GetParameterClassID() || p->GetDataSize() != static_cast<int>(sizeof(CK_ID))) return;
            CK_ID id = 0;
            std::memcpy(&id, p->GetReadDataPtr(FALSE), sizeof(id));
            CKObject* o = id ? context->GetObject(id) : nullptr;
            if (!o) return;
            if (allowed.count(o->GetID())) return;
            if (o->GetClassID() == CKCID_BEHAVIOR && set_scripts.count(root_of(o) ? root_of(o)->GetID() : 0)) return;
            if (CKIsChildClassOf(o, CKCID_SOUND) || CKIsChildClassOf(o, CKCID_TEXTURE)) return;
            violation = place + " refers to " + name_of(o) + " (outside the set)";
        };
        for (CK_ID id: set.scripts) {
            CKBehavior* script = CKBehavior::Cast(context->GetObject(id));
            walk(script, [&](CKBehavior* beh) {
                const std::string place = name_of(script) + "/" + name_of(beh);
                for (int i = 0; i < beh->GetLocalParameterCount(); ++i)
                    check_value(beh->GetLocalParameter(i), place + ":local" + std::to_string(i));
                for (int i = 0; i < beh->GetOutputParameterCount(); ++i) {
                    CKParameterOut* out = beh->GetOutputParameter(i);
                    check_value(out, place + ":out" + std::to_string(i));
                    if (!out || !violation.empty()) continue;
                    for (int d = 0; d < out->GetDestinationCount(); ++d) {
                        CKParameter* dest = out->GetDestination(d);
                        CKBehavior* root = root_of(dest);
                        if (dest && (!root || !set_scripts.count(root->GetID())))
                            violation = place + ":out" + std::to_string(i) + " feeds " + name_of(dest) + " of "
                                      + (root ? name_of(root) : std::string("no script"));
                    }
                }
                const auto check_in = [&](CKParameterIn* in, const std::string& where) {
                    if (!in || !violation.empty()) return;
                    CKParameter* source = in->GetRealSource();
                    if (!source) return;
                    check_value(source, where);
                    CKBehavior* root = root_of(source);
                    if (!root || set_scripts.count(root->GetID()) || !violation.empty()) return;
                    // A parameter of another script: allowed when it holds an
                    // object of the allow-list (a sound Balls_Init keeps).
                    CKObject* value = nullptr;
                    if (source->GetParameterClassID() && source->GetDataSize() == static_cast<int>(sizeof(CK_ID))) {
                        CK_ID id = 0;
                        std::memcpy(&id, source->GetReadDataPtr(FALSE), sizeof(id));
                        value = id ? context->GetObject(id) : nullptr;
                    }
                    if (value && (CKIsChildClassOf(value, CKCID_SOUND) || CKIsChildClassOf(value, CKCID_TEXTURE))) return;
                    violation = where + " reads " + name_of(source) + " of " + name_of(root) + " (outside the set)";
                };
                for (int i = 0; i < beh->GetInputParameterCount(); ++i) check_in(beh->GetInputParameter(i), place + ":in" + std::to_string(i));
                check_in(beh->GetTargetParameter(), place + ":target");
            });
            if (!violation.empty()) return unregister_and_abort("I2: " + violation);
        }
        // I3: no copied Activate Script wakes its owner.
        for (CK_ID id: set.scripts) {
            std::string bad;
            walk(CKBehavior::Cast(context->GetObject(id)), [&](CKBehavior* beh) {
                if (!beh->IsUsingFunction() || beh->GetPrototypeGuid() != activate_script_guid()) return;
                CKBOOL awake = FALSE;
                beh->GetLocalParameterValue(0, &awake);
                if (awake) bad = name_of(beh);
            });
            if (!bad.empty()) return unregister_and_abort("I3: " + name_of(context->GetObject(id)) + "/" + bad + " has Awake Object TRUE");
        }
        // I4: the names, and every copy found by its name and class.
        for (const auto& [o, c]: copy_of) {
            CKObject* original = context->GetObject(o);
            CKObject* made = context->GetObject(c);
            const std::string name = name_of(original) + suffix;
            if (name_of(made) != name) return unregister_and_abort("I4: " + name_of(made) + " is not named " + name);
            CKObject* found = context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()), made->GetClassID(), nullptr);
            if (found != made) return unregister_and_abort("I4: " + name + " (class " + std::to_string(made->GetClassID())
                                                           + ") finds " + (found ? "another object" : "nothing"));
        }
        // I6: every copied piece's local matrix bit for bit the original's.
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t)
            for (size_t i = 0; i < originals[t].pieces.size(); ++i) {
                CK3dEntity* o = originals[t].pieces[i];
                CK3dEntity* c = CK3dEntity::Cast(context->GetObject(set.types[t].pieces[i]));
                const VxMatrix& a = o->GetLocalMatrix();
                const VxMatrix& b = c->GetLocalMatrix();
                if (std::memcmp(&a, &b, sizeof(VxMatrix)) != 0) return unregister_and_abort("I6: " + name_of(c) + "'s local matrix differs from " + name_of(o) + "'s");
                const VxMatrix& wa = o->GetWorldMatrix();
                const VxMatrix& wb = c->GetWorldMatrix();
                if (std::memcmp(&wa, &wb, sizeof(VxMatrix)) != 0) return unregister_and_abort("I6: " + name_of(c) + "'s world matrix differs from " + name_of(o) + "'s");
            }
        // I7: every original and copied force block without a controller,
        // no copied script running.
        for (CK_ID id: set.scripts) {
            CKBehavior* c = CKBehavior::Cast(context->GetObject(id));
            std::string bad;
            walk(c, [&](CKBehavior* beh) { if (script_replay::is_force_block(beh) && pointer_local(beh)) bad = name_of(beh); });
            if (!bad.empty()) return unregister_and_abort("I7: copied force block " + name_of(c) + "/" + bad + " holds a controller");
            if (c->IsActive() || (c->GetFlags() & (CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME)))
                return unregister_and_abort("I7: " + name_of(c) + " is active or pending after the build");
        }
        line("I1-I4, I6, I7 hold; " + std::to_string(ics) + " initial condition(s) saved");

        // The digest of the reference map (names, not ids, structure, not
        // runtime values): the same in every world that built the same set,
        // retail or headless (design 7.4).
        set.digest = trafo_reference_digest(trafo_reference_map(context, set.scripts, player));
        line("reference map digest " + set.digest);
        set.value_digest = trafo_value_digest(context, set.scripts, player);
        line("built value digest " + set.value_digest);

        // Step 6's built script state, after everything above.
        for (CK_ID id: set.scripts) script_replay::capture_script_block(CKBehavior::Cast(context->GetObject(id)), hooks.forces, set.built[id]);
        // The force blocks, for the savestate's world-independent names.
        for (CK_ID id: set.scripts)
            trafo_detail::walk(CKBehavior::Cast(context->GetObject(id)), [&](CKBehavior* beh) {
                if (script_replay::is_force_block(beh)) set.force_blocks.push_back(beh->GetID());
            });
        set.objects.assign(mine.begin(), mine.end());
        set.objects.push_back(set.owner);
        for (const auto& ts: set.types) set.objects.push_back(ts.ref);
        registry.add(std::move(set));
        result.ok = true;
        return result;
    }

    // The reference map of a set's scripts (design 2.4, "the builder also
    // logs a digest of the reference map (names, not ids)"): its structure,
    // not its runtime values - every input's source parameter by its place
    // (script / block walk index and name / kind and index), every output's
    // destinations by place, and the object a constant local holds (a local
    // no output writes).  The same in every world that built the same set
    // from the same level file, whatever the originals ran before the
    // anchor (an output's or a fed local's value is the last run's).
    inline std::vector<std::string> trafo_reference_map(CKContext* context, const std::vector<CK_ID>& scripts, uint32_t player) {
        using namespace trafo_detail;
        std::vector<std::string> out;
        std::map<CK_ID, std::string> place_of;
        std::set<CK_ID> fed;
        std::vector<std::pair<CKBehavior*, std::string>> blocks;
        for (CK_ID id: scripts) {
            CKBehavior* script = CKBehavior::Cast(context->GetObject(id));
            int k = 0;
            walk(script, [&](CKBehavior* beh) {
                const std::string place = normalized(name_of(script), player) + "/" + std::to_string(k++) + ":" + normalized(name_of(beh), player);
                blocks.emplace_back(beh, place);
                for (int i = 0; i < beh->GetLocalParameterCount(); ++i)
                    if (CKParameterLocal* p = beh->GetLocalParameter(i)) place_of[p->GetID()] = place + ":local" + std::to_string(i);
                for (int i = 0; i < beh->GetOutputParameterCount(); ++i)
                    if (CKParameterOut* p = beh->GetOutputParameter(i)) {
                        place_of[p->GetID()] = place + ":out" + std::to_string(i);
                        for (int d = 0; d < p->GetDestinationCount(); ++d)
                            if (CKParameter* dest = p->GetDestination(d)) fed.insert(dest->GetID());
                    }
            });
        }
        const auto where = [&](CKParameter* p) -> std::string {
            if (!p) return "-";
            const auto it = place_of.find(p->GetID());
            if (it != place_of.end()) return it->second;
            CKBehavior* root = root_of(p);
            return "outside:" + normalized(name_of(root), player) + ":" + name_of(p);
        };
        const auto object_of = [&](CKParameter* p) -> CKObject* {
            if (!p || !p->GetParameterClassID() || p->GetDataSize() != static_cast<int>(sizeof(CK_ID))) return nullptr;
            CK_ID ref = 0;
            std::memcpy(&ref, p->GetReadDataPtr(FALSE), sizeof(ref));
            return ref ? context->GetObject(ref) : nullptr;
        };
        for (const auto& [beh, place]: blocks) {
            for (int i = 0; i < beh->GetLocalParameterCount(); ++i) {
                CKParameterLocal* p = beh->GetLocalParameter(i);
                if (!p || fed.count(p->GetID())) continue;
                if (CKObject* o = object_of(p)) out.push_back(place + ":local" + std::to_string(i) + " = " + normalized(name_of(o), player));
            }
            for (int i = 0; i < beh->GetOutputParameterCount(); ++i) {
                CKParameterOut* p = beh->GetOutputParameter(i);
                if (!p) continue;
                for (int d = 0; d < p->GetDestinationCount(); ++d)
                    out.push_back(place + ":out" + std::to_string(i) + " -> " + where(p->GetDestination(d)));
            }
            const auto input = [&](CKParameterIn* in, const std::string& at) {
                if (!in) return;
                CKParameter* source = in->GetRealSource();
                if (!source) return;
                std::string line = at + " <- " + where(source);
                const bool constant = source->GetClassID() == CKCID_PARAMETERLOCAL && !fed.count(source->GetID());
                if (CKObject* o = constant ? object_of(source) : nullptr)
                    line += " = " + normalized(name_of(o), player);
                out.push_back(line);
            };
            for (int i = 0; i < beh->GetInputParameterCount(); ++i) input(beh->GetInputParameter(i), place + ":in" + std::to_string(i));
            input(beh->GetTargetParameter(), place + ":target");
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    inline std::vector<std::string> trafo_reference_map(CKContext* context, const trafo_set& set) {
        return trafo_reference_map(context, set.scripts, set.player);
    }
    inline std::string trafo_reference_digest(const std::vector<std::string>& lines) {
        uint64_t h = 1469598103934665603ull;
        for (const auto& l: lines) h = trafo_detail::fnv64(l + "\n", h);
        char digest[64];
        std::snprintf(digest, sizeof(digest), "%016llx/%zu", static_cast<unsigned long long>(h), lines.size());
        return digest;
    }
    // The built values' lines (script / walk index : block, every
    // non-opaque local and output - an object reference by its normalized
    // name - and the IO flags): the same in every world that built the set
    // from the same level reference, whatever its originals ran before.
    inline std::vector<std::string> trafo_value_lines(CKContext* context, const std::vector<CK_ID>& scripts, uint32_t player) {
        using namespace trafo_detail;
        std::vector<std::string> out;
        for (CK_ID id: scripts) {
            CKBehavior* script = CKBehavior::Cast(context->GetObject(id));
            const std::string sname = normalized(name_of(script), player);
            int k = 0;
            walk(script, [&](CKBehavior* beh) {
                const std::string place = sname + "/" + std::to_string(k++) + ":" + normalized(name_of(beh), player);
                const auto value = [&](CKParameter* p) -> std::string {
                    const trafo_reference::value v = reference_value(p);
                    if (v.kind == 0) return "-";
                    if (v.kind == 2) {
                        CKObject* o = v.object ? context->GetObject(v.object) : nullptr;
                        return "obj:" + (o ? normalized(name_of(o), player) : std::string(v.object ? "?" : "0"));
                    }
                    std::string hex;
                    char b[4];
                    for (uint8_t c: v.bytes) {
                        std::snprintf(b, sizeof(b), "%02x", c);
                        hex += b;
                    }
                    return hex;
                };
                for (int i = 0; i < beh->GetLocalParameterCount(); ++i)
                    out.push_back(place + ":local" + std::to_string(i) + " " + value(beh->GetLocalParameter(i)));
                for (int i = 0; i < beh->GetOutputParameterCount(); ++i)
                    out.push_back(place + ":out" + std::to_string(i) + " " + value(beh->GetOutputParameter(i)));
                out.push_back(place + ":io " + std::to_string(io_mask(beh, true)) + "/" + std::to_string(io_mask(beh, false)));
            });
        }
        return out;
    }
    inline std::string trafo_value_digest(CKContext* context, const std::vector<CK_ID>& scripts, uint32_t player) {
        return trafo_reference_digest(trafo_value_lines(context, scripts, player));
    }

    // ---- running an input (design 2.5)

    // One trafo input applied: an explosion places its type set's
    // referential (identity rotation, translation = `position`, the event's
    // exact floats), records the seed for its frame and sets the explosion's
    // next-frame flags (vanilla's Activate Script with Reset); a reset sets
    // the reset's.  Nothing runs here: the trafo frame does.
    inline bool run_trafo(CKContext* context, const trafo_registry& registry, const trafo_set_hooks& hooks, uint32_t player,
                          uint8_t flags, uint8_t type, const float position[3], int32_t seed, std::string& error) {
        error.clear();
        const trafo_set* set = registry.find(player);
        if (!set || type >= bmmo::session::kTrafoTypes) {
            error = set ? "piece type " + std::to_string(type) : "no trafo set for player " + std::to_string(player);
            return false;
        }
        const trafo_type_set& ts = set->types[type];
        const bool reset = bmmo::session::trafo_is_reset(flags);
        if (!reset) {
            auto* ref = CK3dEntity::Cast(context->GetObject(ts.ref));
            if (!ref) {
                error = "the referential of player " + std::to_string(player) + " is gone";
                return false;
            }
            VxMatrix m;
            m.SetIdentity();
            m[3][0] = position[0];
            m[3][1] = position[1];
            m[3][2] = position[2];
            ref->SetWorldMatrix(m);
            if (hooks.seed) hooks.seed(ts.frame, seed);
        }
        CKBehavior* script = CKBehavior::Cast(context->GetObject(reset ? ts.reset : ts.explosion));
        if (!script) {
            error = std::string("the ") + (reset ? "reset" : "explosion") + " script of player " + std::to_string(player) + " is gone";
            return false;
        }
        script->ModifyFlags(CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME, CKBEHAVIOR_DEACTIVATENEXTFRAME);
        return true;
    }

    // The trafo frame (design 2.5): every selected set's scripts, in player
    // id order and the copied order inside, executed once - next-frame flags
    // first, then every active one - with the fixed frame delta.  Once per
    // tick, after the tick's inputs and the sector union's pass, before the
    // behaviour pass and the step.  Returns the scripts executed.
    inline int trafo_frame(CKContext* context, const trafo_registry& registry, float delta_ms) {
        return script_replay::execute_scripts(context, registry.part().scripts, delta_ms);
    }

    // The effect checks (design 2.3): no body named after an original piece
    // exists, no original script is active.  Cheap; every world, every tick
    // from the anchor on.  `first` names the first leak.
    struct trafo_effect {
        int bodies = 0, scripts = 0;
        std::string first;
        int leaks() const { return bodies + scripts; }
    };

    // ---- the hold (design 2.3, decision 9.5 option E)
    struct trafo_hold_report {
        uint8_t flags = 0;       // TRAFO_EXPLODE / TRAFO_RESET
        uint8_t type = 0;
        float position[3] = {};
        // Option E: the explosion's frame, known from the Trafo Manager's
        // 1000 ms Delayer when it starts; 0 for a cancelled activation (the
        // core stamps tick + 1 + lead).
        uint32_t predicted_tick = 0;
        const char* source = "";  // "delayer" or "activation"
    };

    class trafo_hold {
    public:
        // Armed from the anchor on (design 2.3).  `delta_ms` is the fixed
        // frame delta the Delayer adds every frame (1000/66).
        void arm(CKContext* context, float delta_ms) {
            disarm();
            if (!context) return;
            delta_ms_ = delta_ms;
            for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
                if (CKBehavior* s = trafo_detail::original_script(context, trafo_explosion_name(t))) explosions_[t] = s->GetID();
                if (CKBehavior* s = trafo_detail::original_script(context, trafo_reset_name(t))) resets_[t] = s->GetID();
                if (CK3dEntity* frame = trafo_detail::entity(context, trafo_frame_name(t)))
                    for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child))
                        original_pieces_.push_back(trafo_detail::name_of(child));
            }
            for (CKBehavior* s: trafo_detail::original_scripts(context)) original_scripts_.push_back(s->GetID());
            // The Trafo Manager's Delayer whose Out reaches "start Explosion".
            if (CKBehavior* ingame = find_root_script(context, "Gameplay_Ingame")) {
                CKBehavior* manager = nullptr;
                for (int i = 0; i < ingame->GetSubBehaviorCount() && !manager; ++i)
                    if (CKBehavior* b = ingame->GetSubBehavior(i); b && b->GetName() && std::strcmp(b->GetName(), "Trafo Manager") == 0)
                        manager = b;
                if (manager) {
                    for (int i = 0; i < manager->GetSubBehaviorLinkCount(); ++i) {
                        CKBehaviorLink* link = manager->GetSubBehaviorLink(i);
                        CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                        CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                        CKBehavior* a = from ? from->GetOwner() : nullptr;
                        CKBehavior* b = to ? to->GetOwner() : nullptr;
                        if (!a || !b || !a->GetName() || !b->GetName()) continue;
                        if (std::strcmp(b->GetName(), "start Explosion") == 0 && std::strcmp(a->GetName(), "Delayer") == 0
                                && a->GetParent() == manager) {
                            delayer_ = a->GetID();
                            float wait = 0.0f;
                            a->GetInputParameterValue(0, &wait);
                            delayer_frames_ = frames_until(wait);
                            delayer_active_ = a->IsActive() != FALSE;
                            a->GetOutputParameterValue(0, &delayer_elapsed_);
                        }
                    }
                }
            }
            armed_ = true;
        }
        void disarm() { *this = trafo_hold{}; }
        bool armed() const { return armed_; }
        bool predicts() const { return delayer_ != 0 && delayer_frames_ > 0; }
        uint32_t delayer_frames() const { return delayer_frames_; }

        // After the live frame of `tick`.  `option_e`: the session's lead
        // allows stamping the predicted frame (lead <= 66).  Returns the
        // inputs to stamp, in order.
        std::vector<trafo_hold_report> poll(CKContext* context, uint32_t tick, bool option_e) {
            std::vector<trafo_hold_report> out;
            if (!armed_ || !context) return out;
            // Option E: the Delayer started in this frame - its transition
            // from inactive to active since the previous poll, or a restart
            // (its elapsed time went back).  Detected from the transition,
            // not from the elapsed value equalling one delta: a host whose
            // frame delta is not bit-exactly delta_ms would never match it.
            bool started = false;
            if (delayer_) {
                CKBehavior* d = CKBehavior::Cast(context->GetObject(delayer_));
                float elapsed = 0.0f;
                if (d) d->GetOutputParameterValue(0, &elapsed);
                const bool active = d && d->IsActive();
                started = active && (!delayer_active_ || elapsed < delayer_elapsed_);
                delayer_active_ = active;
                delayer_elapsed_ = elapsed;
            }
            if (option_e && delayer_frames_ > 0) {
                if (started) {
                    trafo_hold_report r;
                    r.flags = bmmo::session::TRAFO_EXPLODE;
                    if (old_ball(context, r.type, r.position)) {
                        // The Delayer fires in its n-th execution, frame
                        // tick + n - 1; start Explosion's Activate Script
                        // then sets the flags the explosion's frame consumes.
                        r.predicted_tick = tick + delayer_frames_;
                        r.source = "delayer";
                        expected_[r.type] = r.predicted_tick;
                        ++predicted_;
                        out.push_back(r);
                    }
                }
            }
            for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
                for (int kind = 0; kind < 2; ++kind) {
                    CKBehavior* s = CKBehavior::Cast(context->GetObject(kind ? resets_[t] : explosions_[t]));
                    if (!s || !(s->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME)) continue;
                    s->ModifyFlags(0, CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME);
                    ++cancelled_;
                    if (!kind) {
                        // The explosion's frame is tick + 1: predicted from
                        // its Delayer, it is stamped already.
                        const auto e = expected_.find(t);
                        if (e != expected_.end() && e->second == tick + 1) {
                            expected_.erase(e);
                            ++confirmed_;
                            // Where the activation's capture would have put it
                            // (diagnostics: option E's position is the same).
                            pos_frame_referential(context, confirmed_position_);
                            continue;
                        }
                        if (e != expected_.end()) {
                            ++mispredicted_;
                            expected_.erase(e);
                        }
                    }
                    trafo_hold_report r;
                    r.flags = kind ? bmmo::session::TRAFO_RESET : bmmo::session::TRAFO_EXPLODE;
                    r.type = t;
                    if (!kind) pos_frame_referential(context, r.position);
                    r.source = "activation";
                    out.push_back(r);
                }
            }
            return out;
        }
        // The effect checks with the originals found at `arm` (every tick:
        // no lookup by name).
        trafo_effect effect_check(CKContext* context, const trafo_set_hooks& hooks) const {
            trafo_effect e;
            if (!context) return e;
            for (const std::string& name: original_pieces_)
                if (hooks.has_body && hooks.has_body(name)) {
                    ++e.bodies;
                    if (e.first.empty()) e.first = "body " + name;
                }
            for (CK_ID id: original_scripts_)
                if (CKBehavior* s = CKBehavior::Cast(context->GetObject(id)); s && s->IsActive()) {
                    ++e.scripts;
                    if (e.first.empty()) e.first = "script " + trafo_detail::name_of(s);
                }
            return e;
        }
        uint64_t cancelled() const { return cancelled_; }
        uint64_t predicted() const { return predicted_; }
        uint64_t confirmed() const { return confirmed_; }
        uint64_t mispredicted() const { return mispredicted_; }
        const float* confirmed_position() const { return confirmed_position_; }

        // The Delayer's execution count to reach `wait` ms, summing the
        // fixed delta in float the way TimerMini does (Logics/Behaviors/
        // TimerMini.cpp: elapsed += DeltaTime; fires when elapsed >= wait).
        uint32_t frames_until(float wait) const {
            if (!(delta_ms_ > 0.0f)) return 0;
            float elapsed = 0.0f;
            for (uint32_t n = 1; n < 100000; ++n) {
                elapsed = elapsed + delta_ms_;
                if (elapsed >= wait) return n;
            }
            return 0;
        }

        // The explosion's position: the wrapped Set Position's referential is
        // Ball_Pos_Frame, whose local matrix it makes the identity under its
        // parent - so the parent's world translation, or the frame's own
        // without a parent (design 2.3).
        static bool pos_frame_referential(CKContext* context, float out[3]) {
            CK3dEntity* frame = trafo_detail::entity(context, "Ball_Pos_Frame");
            if (!frame) return false;
            CK3dEntity* parent = frame->GetParent();
            const VxMatrix& m = parent ? parent->GetWorldMatrix() : frame->GetWorldMatrix();
            out[0] = m[3][0];
            out[1] = m[3][1];
            out[2] = m[3][2];
            return true;
        }

    private:
        // The ball the trafo is about to explode (the current ball: its
        // type, and the explosion's position).
        static bool old_ball(CKContext* context, uint8_t& type, float position[3]) {
            CK3dEntity* frame = trafo_detail::entity(context, "Ball_Pos_Frame");
            CK3dEntity* ball = frame ? frame->GetParent() : nullptr;
            const std::string name = trafo_detail::name_of(ball);
            bool known = false;
            for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t)
                if (name == std::string("Ball_") + bmmo::session::trafo_type_name(t)) { type = t; known = true; }
            return known && pos_frame_referential(context, position);
        }

        bool armed_ = false;
        float delta_ms_ = 0.0f;
        CK_ID explosions_[bmmo::session::kTrafoTypes] = {}, resets_[bmmo::session::kTrafoTypes] = {};
        CK_ID delayer_ = 0;
        uint32_t delayer_frames_ = 0;
        bool delayer_active_ = false;      // the Delayer's state after the previous poll (start detection)
        float delayer_elapsed_ = 0.0f;
        std::vector<CK_ID> original_scripts_;
        std::vector<std::string> original_pieces_;
        std::map<uint8_t, uint32_t> expected_;   // type -> the predicted explosion frame
        uint64_t cancelled_ = 0, predicted_ = 0, confirmed_ = 0, mispredicted_ = 0;
        float confirmed_position_[3] = {};
    };

    // Without a hold to cache them: the originals looked up now.
    inline trafo_effect trafo_effect_check(CKContext* context, const trafo_set_hooks& hooks) {
        trafo_effect e;
        if (!context) return e;
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            CK3dEntity* frame = trafo_detail::entity(context, trafo_frame_name(t));
            if (!frame) continue;
            for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child)) {
                const std::string name = trafo_detail::name_of(child);
                if (hooks.has_body && hooks.has_body(name)) {
                    ++e.bodies;
                    if (e.first.empty()) e.first = "body " + name;
                }
            }
        }
        for (CKBehavior* s: trafo_detail::original_scripts(context))
            if (s->IsActive()) {
                ++e.scripts;
                if (e.first.empty()) e.first = "script " + trafo_detail::name_of(s);
            }
        return e;
    }

    // ---- the reconcile's halves (design 4.7)

    // A type set back to its built state: its pieces unphysicalized through
    // the bridge's per-set restore, for paper its wind's controllers deleted,
    // its explosion and reset (and for paper the wind and the collision
    // sound) put to their built script state.  Never physicalizes anything.
    inline bool unmake_type_set(CKContext* context, const trafo_registry& registry, const trafo_set_hooks& hooks, uint32_t player,
                                uint8_t type, int& bodies_removed, std::string& error) {
        error.clear();
        bodies_removed = 0;
        const trafo_set* set = registry.find(player);
        if (!set || type >= bmmo::session::kTrafoTypes) {
            error = "no trafo set for player " + std::to_string(player);
            return false;
        }
        const trafo_type_set& ts = set->types[type];
        const bool paper = type == 2;
        if (paper && set->wind)
            trafo_detail::walk(CKBehavior::Cast(context->GetObject(set->wind)), [&](CKBehavior* beh) {
                if (!script_replay::is_force_block(beh) || !hooks.forces.restore_force) return;
                force_block_state none;
                hooks.forces.restore_force(beh, none);
            });
        if (!hooks.unmake) {
            error = "no unmake hook";
            return false;
        }
        bodies_removed = hooks.unmake(ts.frame, error);
        if (bodies_removed < 0) return false;
        std::vector<CK_ID> scripts = {ts.explosion, ts.reset};
        if (paper) {
            scripts.push_back(set->wind);
            scripts.push_back(set->collsound);
        }
        for (CK_ID id: scripts) {
            const auto built = set->built.find(id);
            auto* beh = CKBehavior::Cast(context->GetObject(id));
            if (!beh || built == set->built.end()) {
                error = "a script of player " + std::to_string(player) + " has no built state";
                return false;
            }
            if (!script_replay::restore_script_block(beh, hooks.forces, built->second, &error)) return false;
        }
        return true;
    }

    // An explosion made again from the ledger (position, seed): run_trafo,
    // then that type set's explosion alone executed once.  The pieces get the
    // event's draws (mass, friction); what the explosion queued is
    // overwritten by the rows the caller writes next.
    inline bool remake_type_set(CKContext* context, const trafo_registry& registry, const trafo_set_hooks& hooks, uint32_t player,
                                uint8_t type, const float position[3], int32_t seed, float delta_ms, std::string& error) {
        if (!run_trafo(context, registry, hooks, player, bmmo::session::TRAFO_EXPLODE, type, position, seed, error)) return false;
        const trafo_set* set = registry.find(player);
        script_replay::execute_scripts(context, {set->types[type].explosion}, delta_ms);
        return true;
    }

    // ---- when sets go (design 2.4)

    // Whether a set is inert: no body, no active script, no next-frame flags,
    // no controller.  As built at T3: the collision sound
    // (Ball_CollSound_Pieces) does not count as an active script.  It is
    // physics-inert (T1's S1), and whether it ever stops is the sound
    // manager's: after a stone explosion and then a paper one its paper
    // branch's Wave Player stays active for good in a world whose sound
    // manager is the null one (the server's, the headless client's; SimTool
    // L2: stone 300, paper 650, finals 1326 - still active at 1750), where a
    // real one (retail) would end the sound.  Counted, a set with that
    // history never retired, and the decision would hang on a sound device
    // that differs between worlds.  Its listeners and controllers are
    // checked like every script's (its own blocks have no controller).
    inline bool trafo_set_inert(CKContext* context, const trafo_set& set, const trafo_set_hooks& hooks) {
        for (const auto& ts: set.types)
            for (CK_ID id: ts.pieces)
                if (hooks.has_body && hooks.has_body(trafo_detail::name_of(context->GetObject(id)))) return false;
        for (CK_ID id: set.scripts) {
            CKBehavior* s = CKBehavior::Cast(context->GetObject(id));
            if (!s) continue;
            if (id != set.collsound
                    && (s->IsActive() || (s->GetFlags() & (CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME))))
                return false;
            bool controller = false;
            trafo_detail::walk(s, [&](CKBehavior* beh) {
                if (script_replay::is_force_block(beh) && trafo_detail::pointer_local(beh)) controller = true;
            });
            if (controller) return false;
        }
        return true;
    }

    // A TRAFO_FINAL reset of (player, type) applied at `tick`.
    inline void trafo_set_final(trafo_registry& registry, uint32_t player, uint8_t type, uint32_t tick) {
        trafo_set* set = registry.find(player);
        if (!set || type >= bmmo::session::kTrafoTypes) return;
        set->types[type].final = true;
        set->final_tick = bmmo::session::trafo_tick_max(set->final_tick, tick);
    }

    inline void destroy_trafo_set_objects(CKContext* context, const trafo_set& set) {
        std::vector<CK_ID> ids;
        for (CK_ID id: set.objects)
            if (context->GetObject(id)) ids.push_back(id);
        CKDependencies none;
        if (!ids.empty()) context->DestroyObjects(ids.data(), static_cast<int>(ids.size()), 0, &none);
    }

    // Retirement, two stages: a set whose three types have their final
    // reset, whose newest final reset is older than `history` ticks and
    // that has been inert for `history` ticks is deselected (out of the
    // trafo list, the filter registry and the exemption list); a
    // deselected set is destroyed `history` ticks later
    // (`destroy_at_deselect`: at once - the server, which has no rollback).
    // Outside a replay, once per live tick.  Returns what it did, one line
    // each.
    //
    // Inert for the whole history (fix-up of step T3, review 1): the
    // pieces keep their bodies until the final reset's R+132 and its
    // scripts run until then, past R + `history` (96); a set deselected
    // the first tick it is inert could be rewound to an anchor where it was
    // not - its pieces revived by the restore, its scripts neither restored
    // nor run by the replay (a deselected set is out of the trafo list), so
    // the De Physicalize at R+132 never ran again and the pieces stayed,
    // outside the filter registry, until the destroy deleted them under
    // live bodies.  Every anchor a rewind can reach is within `history`
    // ticks of now, so a set inert since now - `history` is inert at all
    // of them.
    inline std::vector<std::string> retire_trafo_sets(CKContext* context, trafo_registry& registry, const trafo_set_hooks& hooks,
                                                      uint32_t now, uint32_t history, bool destroy_at_deselect) {
        std::vector<std::string> lines;
        bool changed = false;
        for (trafo_set& set: registry.sets()) {
            if (!set.selected) continue;
            if (!(set.types[0].final && set.types[1].final && set.types[2].final)) continue;
            if (!trafo_set_inert(context, set, hooks)) {
                set.inert = false;
                continue;
            }
            if (!set.inert) {
                set.inert = true;
                set.inert_since = now;
            }
            if (static_cast<int32_t>(now - (set.final_tick + history)) < 0) continue;
            if (static_cast<int32_t>(now - (set.inert_since + history)) < 0) continue;
            std::string error;
            if (hooks.unregister_set) hooks.unregister_set(set.player, error);
            set.selected = false;
            set.deselected = true;
            set.deselected_at = now;
            changed = true;
            lines.push_back("trafo set " + std::to_string(set.player) + " deselected at " + std::to_string(now));
        }
        std::vector<uint32_t> gone;
        for (const trafo_set& set: registry.sets()) {
            if (!set.deselected) continue;
            if (!destroy_at_deselect && static_cast<int32_t>(now - (set.deselected_at + history)) < 0) continue;
            destroy_trafo_set_objects(context, set);
            gone.push_back(set.player);
            lines.push_back("trafo set " + std::to_string(set.player) + " destroyed at " + std::to_string(now));
        }
        for (uint32_t p: gone) registry.remove(p);
        if (changed && gone.empty()) registry.rebuild_part();
        return lines;
    }

    // A player seen again before its retired set was destroyed: the set is
    // selected again (design 2.4).  False when there is no such set.
    inline bool reselect_trafo_set(trafo_registry& registry, const trafo_set_hooks& hooks, uint32_t player, std::string& error) {
        trafo_set* set = registry.find(player);
        if (!set || set->selected) return false;
        uint32_t frames[3] = {set->types[0].frame, set->types[1].frame, set->types[2].frame};
        std::vector<uint32_t> explosions;
        for (const auto& ts: set->types) explosions.push_back(ts.explosion);
        bmmo_physics_trafo_set_report report{};
        if (hooks.register_set && !hooks.register_set(player, frames, explosions, report, error)) return false;
        set->selected = true;
        set->deselected = false;
        set->inert = false;
        for (auto& ts: set->types) ts.final = false;
        registry.rebuild_part();
        return true;
    }

    // How a world comes to hold `player`'s set - at the server's add_player,
    // before any of the player's inputs is applied, wherever a client host
    // needs one (design 2.4).  One rule for every world, whose result does
    // not depend on how far a retirement got in that world (T2's review: the
    // server destroys a set at its deselect, a client history_ticks later,
    // so a player back inside that span would otherwise find a fresh set in
    // one world and a reselected one, its scripts as they finished, in
    // another):
    //   * a selected set is kept as it is (a fade in progress, its final
    //     marks: a set marked final is retired by every world on the same
    //     rule, and the player's input after that finds no selected set);
    //   * a retired set not destroyed yet (only a client: the server never
    //     holds one) is selected again AND put back to what a fresh build
    //     is: every type set unmade (its scripts to their built state; the
    //     set is inert, so no body or controller goes), every copied
    //     object's initial condition - the build saved it (step 6) - read
    //     back, the final marks and the final tick cleared;
    //   * otherwise a fresh build.
    enum class trafo_ensure : uint8_t { kept, reselected, built };
    struct trafo_ensure_result {
        bool ok = false;
        trafo_ensure how = trafo_ensure::kept;
        std::string error;
        std::vector<std::string> lines;   // a build's lines (build_trafo_set), or what a reselect did
    };
    inline trafo_ensure_result ensure_trafo_set(CKContext* context, trafo_registry& registry, uint32_t player,
                                                const trafo_set_hooks& hooks) {
        trafo_ensure_result r;
        if (!context) {
            r.error = "no context";
            return r;
        }
        if (trafo_set* held = registry.find(player)) {
            if (held->selected) {
                r.ok = true;
                r.how = trafo_ensure::kept;
                return r;
            }
            r.how = trafo_ensure::reselected;
            if (!trafo_set_inert(context, *held, hooks)) {
                r.error = "the retired set of player " + std::to_string(player) + " is not inert";
                return r;
            }
            if (!reselect_trafo_set(registry, hooks, player, r.error)) {
                if (r.error.empty()) r.error = "the set of player " + std::to_string(player) + " was not selected again";
                return r;
            }
            int removed = 0;
            for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t)
                if (!unmake_type_set(context, registry, hooks, player, t, removed, r.error)) return r;
            trafo_set* set = registry.find(player);
            int ics = 0;
            if (CKScene* scene = context->GetCurrentScene())
                for (CK_ID id: set->objects) {
                    CKObject* o = context->GetObject(id);
                    if (!o || o->GetClassID() == CKCID_BEHAVIOR || CKIsChildClassOf(o, CKCID_MESH)) continue;
                    auto* so = CKSceneObject::Cast(o);
                    if (!so) continue;
                    if (CKStateChunk* chunk = scene->GetObjectInitialValue(so)) {
                        CKReadObjectState(o, chunk);
                        ++ics;
                    }
                }
            set->final_tick = 0;
            r.lines.push_back("selected again and put back to its built state (" + std::to_string(ics) + " initial conditions read back)");
            r.ok = true;
            return r;
        }
        auto built = build_trafo_set(context, registry, player, hooks);
        r.how = trafo_ensure::built;
        r.ok = built.ok;
        r.error = std::move(built.error);
        r.lines = std::move(built.lines);
        return r;
    }

    // Every set's piece entities added to `names` (step T3: design 4.3,
    // `explosion_pieces` over every set - with the trafo as an input the
    // pieces of every set are no BodyRevived and are revivable at a restore,
    // like the originals were under step P).
    inline void add_trafo_set_pieces(CKContext* context, const trafo_registry& registry, std::set<std::string>& names) {
        if (!context) return;
        for (const trafo_set& set: registry.sets())
            for (const auto& ts: set.types)
                for (CK_ID id: ts.pieces)
                    if (CKObject* o = context->GetObject(id); o && o->GetName()) names.insert(o->GetName());
    }

    // Session end: every live piece unphysicalized, the winds' controllers
    // deleted, the sets unregistered and destroyed.  Returns the bodies removed.
    inline int teardown_trafo_sets(CKContext* context, trafo_registry& registry, const trafo_set_hooks& hooks) {
        int bodies = 0;
        if (context)
            for (const trafo_set& set: registry.sets()) {
                if (set.wind)
                    trafo_detail::walk(CKBehavior::Cast(context->GetObject(set.wind)), [&](CKBehavior* beh) {
                        if (!script_replay::is_force_block(beh) || !hooks.forces.restore_force) return;
                        force_block_state none;
                        hooks.forces.restore_force(beh, none);
                    });
                for (const auto& ts: set.types) {
                    std::string error;
                    if (hooks.unmake && set.selected) {
                        const int n = hooks.unmake(ts.frame, error);
                        if (n > 0) bodies += n;
                    } else {
                        for (CK_ID id: ts.pieces) {
                            const std::string name = trafo_detail::name_of(context->GetObject(id));
                            if (hooks.has_body && hooks.has_body(name) && hooks.unphysicalize && hooks.unphysicalize(name, error)) ++bodies;
                        }
                    }
                }
                std::string error;
                if (hooks.unregister_set && set.selected) hooks.unregister_set(set.player, error);
                destroy_trafo_set_objects(context, set);
            }
        registry.clear();
        return bodies;
    }
}
