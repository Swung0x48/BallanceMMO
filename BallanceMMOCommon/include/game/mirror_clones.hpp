#pragma once

// One mirror clone per player and ball type, reused across lives (FULL step
// BMX; world-agreement-gaps-design.md "As built (step INV)" finding 2 and
// "As built (step BMX)", the user's "Shared helper + audit" of 2026-10-30).
//
// The rule, for every world: a player's ball of one type is mirrored by ONE
// CK entity, copied once from the level's ball of that type ("Ball_Paper"),
// and every life of that player with that type - a respawn, a trafo back to
// the type, a beam's Unphysicalize + Physicalize pair - gives that same
// entity a new body.  The entity goes when the player leaves the world (the
// server's remove_player) or when the world goes (the engine's context; the
// headless client's engine reboot).
//
// Why it matters (INV finding 2): the place point of the proximity union
// runs before the tick's lifecycle events in every world, so at a same-tick
// Unphysicalize + Physicalize pair a slot cell names the entity the pair
// re-bodies only if the pair keeps the entity.  The server (clone_ball, its
// p.balls map) and retail (game_objects' spirit balls, one per player and
// type) always did; the headless client looked its clone up by the name
// "<ball>_BMMO_<player>", which CK's unique-name copy never gives (the copy
// of the name already taken comes out with the trailing number counted up),
// so it made a new entity per life and its cell named the old, bodiless one
// for a tick.
//
//   * mirror_clones: the rule as an owner of the clones (the server's
//     physics_world and the headless client).  Keyed by (player, type),
//     never by name; the CK_ID is kept, not the pointer, and resolved on
//     every use together with the name the copy came out with: CK hands a
//     freed id to the next object it creates (CKObjectManager's free list),
//     so an id alone could name a level object once the clone was destroyed
//     behind the helper's back.  A clone that is gone - its id resolves to
//     nothing or to an entity of another name - is made again, counted.
//   * mirror_entity_contract: the same rule as a check, for a host that
//     owns its entities otherwise (retail's spirit balls): the first entity
//     used for (player, type) in a session is the one every later life must
//     use; a different one (another id, or the same id under another name:
//     a re-made entity that got the freed id back) is counted and reported
//     by the host.
//
// Only the public CK2 API: the header compiles in the x86 client target
// (Virtools SDK) and against Ballanced.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include "CKAll.h"

namespace bmmo::game {
    // The tag between the level ball's name and the player in a mirror
    // clone's name: CopyObject appends "<tag><player>" to the source's name
    // (unique-named, so the trailing number may come out counted up).
    inline constexpr const char* kMirrorNameTag = "_BMMO_";

    inline bool is_mirror_name(const std::string& name) {
        return name.find(kMirrorNameTag) != std::string::npos;
    }

    // `entity`'s name, "" for none.
    inline std::string entity_name(CKObject* entity) {
        const char* name = entity ? entity->GetName() : nullptr;
        return name ? std::string(name) : std::string();
    }

    class mirror_clones {
    public:
        struct result {
            CK3dEntity* entity = nullptr;
            bool made = false;       // copied now (the first life of this player and type)
            bool remade = false;     // copied again: the earlier clone was gone (counted)
        };

        // The clone that mirrors `player`'s ball of `ball_type`: the one made
        // before, else a copy of the level's entity named `source_name`
        // (shared mesh, no scripts: a bare physics body).  Null and `error`
        // when the level has no such ball or the copy fails.
        result get(CKContext* context, uint32_t player, uint8_t ball_type, const std::string& source_name,
                   std::string& error) {
            result r;
            if (!context) { error = "no context"; return r; }
            const key k{player, ball_type};
            bool lost = false;
            if (const auto it = clones_.find(k); it != clones_.end()) {
                if (auto* existing = resolve(context, it->second)) {
                    r.entity = existing;
                    ++reuses_;
                    return r;
                }
                clones_.erase(it);
                lost = true;
            }
            auto* source = CK3dEntity::Cast(context->GetObjectByNameAndClass(
                    const_cast<CKSTRING>(source_name.c_str()), CKCID_3DOBJECT, nullptr));
            if (!source) { error = "ball entity " + source_name + " not found"; return r; }
            CKDependencies dependencies;
            dependencies.Resize(40);
            dependencies.Fill(0);
            dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
            dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
            const std::string suffix = kMirrorNameTag + std::to_string(player);
            auto* clone = CK3dEntity::Cast(context->CopyObject(source, &dependencies, const_cast<CKSTRING>(suffix.c_str())));
            if (!clone) { error = "CopyObject failed for " + source_name; return r; }
            clones_[k] = clone_ref{clone->GetID(), entity_name(clone)};
            ++made_;
            if (lost) ++remade_;
            r.entity = clone;
            r.made = true;
            r.remade = lost;
            return r;
        }

        // The clone of (player, type) if one was made and still exists; null
        // otherwise (never makes one).
        CK3dEntity* find(CKContext* context, uint32_t player, uint8_t ball_type) const {
            if (!context) return nullptr;
            const auto it = clones_.find(key{player, ball_type});
            return it == clones_.end() ? nullptr : resolve(context, it->second);
        }

        // The player left the world: each of its clones handed to
        // `before_destroy` (the host takes its body out) and destroyed.
        // Only a world that never rewinds across the leave may call it (the
        // server); a client keeps a leaver's clones for the rest of its
        // session (records inside the rollback window name them).
        void release_player(CKContext* context, uint32_t player, const std::function<void(CK3dEntity*)>& before_destroy) {
            for (auto it = clones_.lower_bound(key{player, 0}); it != clones_.end() && it->first.first == player;) {
                if (auto* entity = context ? resolve(context, it->second) : nullptr) {
                    if (before_destroy) before_destroy(entity);
                    context->DestroyObject(entity);
                }
                it = clones_.erase(it);
            }
        }

        // The world's context goes (an engine reboot, the host's end): the
        // clones go with it, nothing is destroyed here.
        void clear() { clones_.clear(); }

        size_t size() const { return clones_.size(); }
        uint64_t made() const { return made_; }
        uint64_t reuses() const { return reuses_; }
        uint64_t remade() const { return remade_; }

    private:
        using key = std::pair<uint32_t, uint8_t>;
        // The clone's id and the name its copy came out with.
        struct clone_ref {
            CK_ID id = 0;
            std::string name;
        };
        // The clone `ref` names, null when it is gone: no object under its
        // id any more, or (the id freed and handed out again) an object of
        // another name.
        static CK3dEntity* resolve(CKContext* context, const clone_ref& ref) {
            auto* entity = CK3dEntity::Cast(context->GetObject(ref.id));
            return entity && entity_name(entity) == ref.name ? entity : nullptr;
        }
        std::map<key, clone_ref> clones_;
        uint64_t made_ = 0, reuses_ = 0, remade_ = 0;
    };

    // The rule as a check (retail: game_objects owns the spirit balls).
    class mirror_entity_contract {
    public:
        // True when `entity` (id and name) is the entity (player, type) used
        // before in this session, or the first one; false (counted) when a
        // different entity would be used - another id, or the same id under
        // another name (a destroyed spirit ball's freed id given to its
        // replacement) - and `previous` / `previous_name` are the earlier
        // one.  The new entity is kept as the reference from then on.
        bool check(uint32_t player, uint8_t ball_type, CK_ID entity, const std::string& name,
                   CK_ID* previous = nullptr, std::string* previous_name = nullptr) {
            auto [it, inserted] = entities_.try_emplace(std::make_pair(player, ball_type), entity, name);
            if (inserted || (it->second.first == entity && it->second.second == name)) return true;
            if (previous) *previous = it->second.first;
            if (previous_name) *previous_name = it->second.second;
            it->second = {entity, name};
            ++violations_;
            return false;
        }
        void clear() { entities_.clear(); }
        uint64_t violations() const { return violations_; }

    private:
        std::map<std::pair<uint32_t, uint8_t>, std::pair<CK_ID, std::string>> entities_;
        uint64_t violations_ = 0;
    };
}
