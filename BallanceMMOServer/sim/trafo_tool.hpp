#pragma once

// SimTool's trafo set modes (docs/trafo-replication-design.md 7.2, step T1):
// the per-player piece sets of game/trafo_sets.hpp in a free run or in the
// savestate check's world, driven by trafo inputs given on the command line
// (--explode-player, --reset-player), with the diagnostics the checks S0-S13
// read: the builder's log and invariants (--describe-trafo), every piece's
// exact state per tick (--trafo-track), the hold and the effect checks
// (--trafo-hold), the PreSimulate queue (--trafo-presim), the pieces'
// contacts and mindists (--trafo-contacts), the hulls (--trafo-surfaces) and
// the record v3 checks (--trafo-record-check).
//
// Free-run order of one tick, the order every world has (design 2.5): the
// tick's inputs (run_trafo), the trafo frame, the engine's frame (behaviour
// pass, step); after it the hold and the effect checks.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <game/script_replay.hpp>
#include <game/trafo_sets.hpp>
#include <physics/physics_direct.hpp>
#include <physics/physics_state.hpp>
#include <session/resync_rules.hpp>
#include <session/trafo_input.hpp>

#include "CKAll.h"
#include "CKIpionManager.h"

namespace bmmo::sim {

    struct trafo_tool_options {
        std::vector<uint32_t> players;     // the sets to build (--trafo-sets N: 1..N; --trafo-players a,b,..)
        bool reverse = false;              // --trafo-reverse: built in descending id order (S3)
        long long anchor = -1;             // --trafo-anchor TICK: the quiesce and the builds (free run)
        uint32_t session_seed = 1;         // --trafo-seed S: trafo_seed's session seed
        struct input {
            uint32_t player = 0;
            uint8_t type = 0;
            uint8_t flags = 0;
            long long tick = 0;
            float position[3] = {};
        };
        std::vector<input> inputs;         // --explode-player / --reset-player
        struct original {
            uint8_t type = 0;
            long long tick = 0;
            float position[3] = {};
            int32_t seed = 0;
        };
        std::vector<original> originals;   // --explode-original (S1's control: the original set, seeded, placed)
        bool describe = false;             // --describe-trafo
        bool hold = false;                 // --trafo-hold: the hold and the effect checks, every tick from the anchor
        bool hold_slip = false;            // --trafo-hold-slip: --activate lands after the hold (test switch)
        bool option_e = true;              // --trafo-no-option-e: the hold path only
        int filter = -1;                   // --trafo-filter on|off
        std::vector<std::pair<std::string, uint8_t>> track;   // --trafo-track ID|orig TYPE
        bool presim = false;               // --trafo-presim
        bool colldump = false;             // --trafo-colldump: every set's PhysicsCollDetection blocks, per tick on change (S8)
        struct poke { uint32_t player = 0; long long tick = 0; };
        std::vector<poke> pokes;           // --trafo-poke-create ID TICK: S8's positive control (see poke_create)
        bool built_params = false;         // --trafo-built-params: every built script's locals and outputs, by place
        bool contacts = false;             // --trafo-contacts
        bool surfaces = false;             // --trafo-surfaces
        bool no_collsound = false;         // --trafo-no-collsound: the sets' collision sound left out of the trafo list
        bool record_check = false;         // --trafo-record-check
        // The reconcile's halves and when sets go (design 2.4, 4.7):
        // --trafo-unmake ID TYPE TICK, --trafo-remake ID TYPE TICK X Y Z SEED_TICK
        // (the seed of an explosion applied at SEED_TICK), --trafo-final ID TICK
        // (a leaver's three final resets), --trafo-retire (retirement checked
        // after every frame, history 96), --trafo-teardown TICK (session end).
        struct unmake { uint32_t player = 0; uint8_t type = 0; long long tick = 0; bool remake = false;
                        float position[3] = {}; long long seed_tick = 0; };
        std::vector<unmake> unmakes;
        bool retire = false;
        long long teardown = -1;
        bool any() const {
            return !players.empty() || !originals.empty() || hold || describe;
        }
    };

    inline bool trafo_type_of(const std::string& text, uint8_t& type) {
        if (text == "wood") type = 0;
        else if (text == "stone") type = 1;
        else if (text == "paper") type = 2;
        else return false;
        return true;
    }

    // The bridge as the headless hosts reach it.
    inline bmmo::game::trafo_set_hooks make_trafo_hooks(CKIpionManager* physics) {
        bmmo::game::trafo_set_hooks hooks;
        hooks.has_body = [physics](const std::string& name) {
            CKContext* context = physics ? physics->m_Context : nullptr;
            auto* entity = context ? CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()),
                                                                                              CKCID_3DENTITY, nullptr))
                                   : nullptr;
            PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
            return object && object->m_RealObject;
        };
        hooks.unphysicalize = [physics](const std::string& name, std::string& error) {
            return bmmo::physics::unphysicalize(physics, name.c_str(), error);
        };
        hooks.restore_originals = [physics]() { bmmo::physics::restore_explosion_pieces(physics->m_Context); };
        hooks.register_set = [physics](uint32_t player, const uint32_t frames[3], const std::vector<uint32_t>& explosions,
                                       bmmo_physics_trafo_set_report& out, std::string& error) {
            return bmmo::physics::trafo_set_register(physics, player, frames, explosions.data(), static_cast<int>(explosions.size()),
                                                     out, error);
        };
        hooks.unregister_set = [physics](uint32_t player, std::string& error) {
            return bmmo::physics::trafo_set_unregister(physics, player, error);
        };
        hooks.seed = [physics](uint32_t frame, int32_t seed) { bmmo::physics::trafo_set_seed(physics, frame, seed); };
        hooks.unmake = [physics](uint32_t frame, std::string& error) { return bmmo::physics::trafo_set_unmake(physics, frame, error); };
        hooks.forces.capture_force = [physics](CKBehavior* block, bmmo::game::force_block_state& out) {
            bmmo_physics_force_block state{};
            std::string error;
            if (!bmmo::physics::force_block_get(physics, static_cast<uint32_t>(block->GetID()), state, error)) return false;
            out.exists = state.exists != 0;
            for (int k = 0; k < 3; ++k) { out.position[k] = state.position[k]; out.force[k] = state.force[k]; }
            return true;
        };
        hooks.forces.restore_force = [physics](CKBehavior* block, const bmmo::game::force_block_state& in) {
            bmmo_physics_force_block state{};
            state.exists = in.exists ? 1 : 0;
            for (int k = 0; k < 3; ++k) { state.position[k] = in.position[k]; state.force[k] = in.force[k]; }
            std::string error;
            return bmmo::physics::force_block_set(physics, static_cast<uint32_t>(block->GetID()), state, error);
        };
        hooks.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
        return hooks;
    }

    class trafo_tool {
    public:
        trafo_tool(const trafo_tool_options& options, CKIpionManager* physics, CKContext* context)
            : o_(options), physics_(physics), context_(context), hooks_(make_trafo_hooks(physics)) {}

        bmmo::game::trafo_registry& registry() { return registry_; }
        const bmmo::game::trafo_set_hooks& hooks() const { return hooks_; }
        const bmmo::session::trafo_input_log& log() const { return log_; }
        bool anchored() const { return anchored_; }
        uint64_t leaks() const { return leaks_; }
        int failures() const { return failures_; }

        // The quiesce and the builds (design 2.3, 2.4), then the filter
        // switch and the hold.  False when a build failed its invariants.
        // The level reference (game/trafo_sets.hpp trafo_reference): taken
        // when the level's 8 trafo scripts first exist, before any original
        // explosion - every free-run tick until it is (before_frame), or at
        // the anchor in the modes whose world never ran an original (the
        // drop scene, the savestate check).
        void capture_reference(long long tick) {
            if (!registry_.reference().empty()) return;
            bmmo::game::trafo_reference ref;
            std::string error;
            if (!bmmo::game::capture_trafo_reference(context_, ref, error)) return;
            size_t blocks = 0;
            for (const auto& [name, b]: ref.scripts) blocks += b.size();
            registry_.set_reference(std::move(ref));
            std::printf("trafo level reference captured at t=%lld: 8 scripts, %zu blocks\n", tick, blocks);
        }

        bool anchor(long long tick) {
            anchored_ = true;
            capture_reference(tick);
            const auto q = bmmo::game::quiesce_originals(context_, hooks_);
            std::printf("trafo anchor t=%lld %s\n", tick, q.text().c_str());
            std::vector<uint32_t> order = o_.players;
            std::sort(order.begin(), order.end());
            if (o_.reverse) std::reverse(order.begin(), order.end());
            for (uint32_t p: order) {
                const auto r = bmmo::game::build_trafo_set(context_, registry_, p, hooks_);
                for (const auto& l: r.lines)
                    if (o_.describe || !r.ok) std::printf("  %s\n", l.c_str());
                if (!r.ok) {
                    std::printf("trafo build FAILED for player %u: %s\n", p, r.error.c_str());
                    ++failures_;
                    return false;
                }
                const auto* set = registry_.find(p);
                std::printf("trafo set %u built: %zu objects, %zu scripts, digest %s, value digest %s\n", p,
                            set->objects.size(), set->scripts.size(), set->digest.c_str(), set->value_digest.c_str());
                if (o_.built_params) built_params(*set);
            }
            if (o_.no_collsound) strip_collsound();
            if (o_.describe) describe();
            if (o_.filter >= 0) {
                std::string error;
                if (!bmmo::physics::trafo_set_filter(physics_, o_.filter != 0, error)) std::printf("trafo filter: %s\n", error.c_str());
                std::printf("trafo filter %s\n", o_.filter ? "on" : "off");
            }
            if (o_.hold) {
                hold_.arm(context_, bmmo::sim::kFixedDeltaMs);
                std::printf("trafo hold armed: option E %s (Delayer %u frames)\n", hold_.predicts() ? "available" : "unavailable",
                            hold_.delayer_frames());
            }
            for (const auto& in: o_.inputs) {
                bmmo::session::trafo_input x;
                x.player = in.player;
                x.type = in.type;
                x.flags = in.flags;
                x.origin = x.applied = static_cast<uint32_t>(in.tick);
                for (int k = 0; k < 3; ++k) x.position[k] = in.position[k];
                if (!bmmo::session::trafo_is_reset(in.flags))
                    x.seed = bmmo::session::trafo_seed(o_.session_seed, in.player, x.applied, in.type);
                log_.add(x);
            }
            if (o_.record_check) record_check();
            return failures_ == 0;
        }

        // Before the engine's frame of `tick`: the inputs due, then the trafo
        // frame.  `frame`: run the trafo frame here (false when the caller's
        // world runs it through its frame hook).
        void before_frame(long long tick, bool frame = true) {
            capture_reference(tick);
            if (!anchored_ && o_.anchor >= 0 && tick == o_.anchor) anchor(tick);
            for (const auto& orig: o_.originals)
                if (orig.tick == tick) explode_original(orig, tick);
            if (!anchored_) return;
            // A remake executes its explosion once, as the reconcile does at
            // the restored tick S, whose replay then runs S+1's frame: that
            // tick's trafo frame is the remake's (the exercise has no other
            // set active then).
            bool remade = false;
            for (const auto& u: o_.unmakes)
                if (u.tick == tick) {
                    reconcile_half(u, tick);
                    remade = remade || u.remake;
                }
            if (o_.teardown >= 0 && tick == o_.teardown) teardown(tick);
            for (const auto& k: o_.pokes)
                if (k.tick == tick) poke_create(k.player, tick);
            apply_inputs(static_cast<uint32_t>(tick));
            if (frame && !remade) run_frame();
        }
        void reconcile_half(const trafo_tool_options::unmake& u, long long tick) {
            std::string error;
            if (!u.remake) {
                int removed = 0;
                const bool ok = bmmo::game::unmake_type_set(context_, registry_, hooks_, u.player, u.type, removed, error);
                std::printf("trafo unmake t=%lld player %u %s: %s, %d bodies removed%s\n", tick, u.player,
                            bmmo::session::trafo_type_name(u.type), ok ? "ok" : "FAILED", removed, error.empty() ? "" : (" (" + error + ")").c_str());
                if (!ok) ++failures_;
                return;
            }
            const int32_t s = bmmo::session::trafo_seed(o_.session_seed, u.player, static_cast<uint32_t>(u.seed_tick), u.type);
            const bool ok = bmmo::game::remake_type_set(context_, registry_, hooks_, u.player, u.type, u.position, s,
                                                        bmmo::sim::kFixedDeltaMs, error);
            int bodies = 0;
            if (const auto* set = registry_.find(u.player))
                for (CK_ID id: set->types[u.type].pieces)
                    if (hooks_.has_body(name(id))) ++bodies;
            std::printf("trafo remake t=%lld player %u %s seed %d: %s, %d bodies%s\n", tick, u.player,
                        bmmo::session::trafo_type_name(u.type), s, ok ? "ok" : "FAILED", bodies,
                        error.empty() ? "" : (" (" + error + ")").c_str());
            if (!ok) ++failures_;
        }
        void teardown(long long tick) {
            const int bodies = bmmo::game::teardown_trafo_sets(context_, registry_, hooks_);
            int left = 0;
            std::string names;
            const XObjectPointerArray& all = context_->GetObjectListByType(CKCID_OBJECT, TRUE);
            for (int i = 0; i < all.Size(); ++i)
                if (CKObject* o = all[i]; o && o->GetName() && is_set_name(o->GetName())) {
                    if (++left <= 6) names += std::string(" ") + o->GetName() + "(" + std::to_string(o->GetClassID()) + ")";
                }
            if (left) std::printf("trafo teardown: left:%s\n", names.c_str());
            std::printf("trafo teardown t=%lld: %d bodies removed, %d objects named _Trafo / BMMO_Trafo left; bridge: %s\n", tick,
                        bodies, left, bmmo::physics::describe_trafo_sets(physics_).c_str());
            if (left) ++failures_;
        }
        void apply_inputs(uint32_t tick, bool quiet = false) {
            for (const auto& in: log_.at(tick)) {
                std::string error;
                if (!bmmo::game::run_trafo(context_, registry_, hooks_, in.player, in.flags, in.type, in.position, in.seed, error)) {
                    std::printf("trafo input FAILED at %u: %s\n", tick, error.c_str());
                    ++failures_;
                    continue;
                }
                if (in.flags & bmmo::session::TRAFO_FINAL) bmmo::game::trafo_set_final(registry_, in.player, in.type, tick);
                if (quiet) continue;
                std::printf("trafo input t=%u player %u %s %s at (%.3f,%.3f,%.3f) seed %d\n", tick, in.player,
                            bmmo::session::trafo_type_name(in.type), bmmo::session::trafo_is_reset(in.flags) ? "reset" : "explode",
                            in.position[0], in.position[1], in.position[2], in.seed);
            }
        }
        void run_frame() { bmmo::game::trafo_frame(context_, registry_, bmmo::sim::kFixedDeltaMs); }

        // A post-frame activation (--activate with the hold armed): what a
        // script of this frame would have done, Activate Script's way.
        void post_frame_activate(CKBehavior* script) {
            if (CKScene* scene = context_->GetCurrentScene()) scene->Activate(script, TRUE);
        }

        // After the engine's frame of `tick`.
        void after_frame(long long tick, const std::vector<CKBehavior*>& activations) {
            if (!anchored_) return;
            if (!o_.hold_slip) for (CKBehavior* s: activations) post_frame_activate(s);
            if (o_.hold) {
                const uint64_t confirmed = hold_.confirmed();
                const auto reports = hold_.poll(context_, static_cast<uint32_t>(tick), o_.option_e);
                if (hold_.confirmed() != confirmed)
                    std::printf("trafo hold t=%lld the predicted explosion's activation cancelled; the activation-time position "
                                "(%a,%a,%a)\n", tick, static_cast<double>(hold_.confirmed_position()[0]),
                                static_cast<double>(hold_.confirmed_position()[1]), static_cast<double>(hold_.confirmed_position()[2]));
                for (const auto& r: reports)
                    std::printf("trafo hold t=%lld %s %s from %s at (%a,%a,%a)%s\n", tick,
                                bmmo::session::trafo_is_reset(r.flags) ? "reset" : "explode", bmmo::session::trafo_type_name(r.type),
                                r.source, static_cast<double>(r.position[0]), static_cast<double>(r.position[1]),
                                static_cast<double>(r.position[2]),
                                r.predicted_tick ? (" predicted for " + std::to_string(r.predicted_tick)).c_str() : "");
                const auto e = hold_.effect_check(context_, hooks_);
                if (e.leaks()) {
                    leaks_ += static_cast<uint64_t>(e.leaks());
                    std::printf("trafo LEAK t=%lld bodies=%d scripts=%d first: %s\n", tick, e.bodies, e.scripts, e.first.c_str());
                }
            }
            if (o_.hold_slip) for (CKBehavior* s: activations) post_frame_activate(s);
            if (o_.retire)
                for (const auto& line: bmmo::game::retire_trafo_sets(context_, registry_, hooks_, static_cast<uint32_t>(tick), 96, false))
                    std::printf("%s\n", line.c_str());
            if (!o_.track.empty()) track(tick);
            if (o_.presim) presim(tick);
            if (o_.colldump) colldump(tick);
            if (o_.contacts) contacts(tick);
            if (o_.surfaces) surfaces(tick);
            pose(tick);
        }

        int finish() {
            if (o_.hold)
                std::printf("trafo summary: hold cancelled=%llu predicted=%llu confirmed=%llu mispredicted=%llu "
                            "trafo_original_leaks=%llu\n",
                            static_cast<unsigned long long>(hold_.cancelled()), static_cast<unsigned long long>(hold_.predicted()),
                            static_cast<unsigned long long>(hold_.confirmed()), static_cast<unsigned long long>(hold_.mispredicted()),
                            static_cast<unsigned long long>(leaks_));
            if (o_.presim)
                std::printf("trafo summary: presim max non-BMMO entries %d, of them from _Trafo blocks %d (first at t=%lld: %s)\n",
                            presim_max_, presim_trafo_max_, presim_first_tick_, presim_first_.c_str());
            if (o_.contacts)
                std::printf("trafo summary: piece x keep-local contacts %d mindists %d (first at t=%lld: %s)\n", local_contacts_,
                            local_mindists_, local_first_tick_, local_first_.c_str());
            if (o_.describe) describe();
            std::printf("trafo summary: failures=%d\n", failures_);
            std::fflush(stdout);
            return failures_ ? 3 : 0;
        }

        void describe() {
            std::printf("trafo describe: %s\n", bmmo::physics::describe_trafo_sets(physics_).c_str());
            for (const auto& set: registry_.sets()) {
                std::printf("  set %u owner %s selected=%d scripts:", set.player,
                            bmmo::game::trafo_owner_name(set.player).c_str(), set.selected ? 1 : 0);
                for (CK_ID id: set.scripts) {
                    CKBehavior* s = CKBehavior::Cast(context_->GetObject(id));
                    std::printf(" %s%s", s && s->GetName() ? s->GetName() : "?", s && s->IsActive() ? "*" : "");
                }
                std::printf("\n");
                for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
                    const auto& ts = set.types[t];
                    int bodies = 0;
                    for (CK_ID id: ts.pieces)
                        if (hooks_.has_body(name(id))) ++bodies;
                    std::printf("    %s: frame %s, %zu pieces (%d with a body), ref %s\n", bmmo::session::trafo_type_name(t),
                                name(ts.frame).c_str(), ts.pieces.size(), bodies, name(ts.ref).c_str());
                }
                const auto map = bmmo::game::trafo_reference_map(context_, set);
                std::printf("    reference map (%zu):\n", map.size());
                for (const auto& l: map) std::printf("      %s\n", l.c_str());
            }
            std::fflush(stdout);
        }

    private:
        // A set's object: "<original>_Trafo<digits>" or "BMMO_Trafo...".
        static bool is_set_name(const char* name) {
            if (std::strncmp(name, "BMMO_Trafo", 10) == 0) return true;
            const char* at = std::strstr(name, "_Trafo");
            while (at) {
                const char* d = at + 6;
                if (*d >= '0' && *d <= '9') {
                    while (*d >= '0' && *d <= '9') ++d;
                    if (!*d) return true;
                }
                at = std::strstr(at + 1, "_Trafo");
            }
            return false;
        }
        std::string name(CK_ID id) const {
            CKObject* o = context_->GetObject(id);
            return o && o->GetName() ? o->GetName() : std::string();
        }
        void strip_collsound() {
            // Test switch (S1's second run): the sets' collision sound never
            // runs - out of the trafo list.
            for (auto& set: registry_.sets())
                set.scripts.erase(std::remove(set.scripts.begin(), set.scripts.end(), set.collsound), set.scripts.end());
            registry_.rebuild_part();
            std::printf("trafo: the collision sound is left out of the trafo list (test switch)\n");
        }

        void explode_original(const trafo_tool_options::original& orig, long long tick) {
            CKBehavior* explosion = bmmo::game::trafo_detail::original_script(context_, bmmo::game::trafo_explosion_name(orig.type));
            CK3dEntity* frame = bmmo::game::trafo_detail::entity(context_, bmmo::game::trafo_frame_name(orig.type));
            if (!explosion || !frame) {
                std::printf("explode-original: no original %s set\n", bmmo::session::trafo_type_name(orig.type));
                ++failures_;
                return;
            }
            const std::string ref_name = std::string("BMMO_TrafoRef_Test_") + bmmo::session::trafo_type_name(orig.type);
            CK3dEntity* ref = bmmo::game::trafo_detail::entity(context_, ref_name);
            if (!ref) {
                ref = CK3dEntity::Cast(context_->CreateObject(CKCID_3DENTITY, const_cast<CKSTRING>(ref_name.c_str())));
                if (CKLevel* level = context_->GetCurrentLevel()) level->AddObject(ref);
                if (CKScene* scene = context_->GetCurrentScene()) scene->AddObjectToScene(ref, FALSE);
            }
            VxMatrix m;
            m.SetIdentity();
            m[3][0] = orig.position[0];
            m[3][1] = orig.position[1];
            m[3][2] = orig.position[2];
            ref->SetWorldMatrix(m);
            for (int i = 0; i < explosion->GetLocalParameterCount(); ++i) {
                CKParameterLocal* p = explosion->GetLocalParameter(i);
                if (!p || !p->GetName() || std::strcmp(p->GetName(), "Referential") != 0) continue;
                CK_ID id = ref->GetID();
                p->SetValue(&id);
            }
            bmmo::physics::trafo_set_seed(physics_, frame->GetID(), orig.seed);
            explosion->Activate(TRUE, TRUE);
            std::printf("explode-original t=%lld %s at (%.3f,%.3f,%.3f) seed %d\n", tick, bmmo::session::trafo_type_name(orig.type),
                        orig.position[0], orig.position[1], orig.position[2], orig.seed);
        }

        // Every tracked type set's pieces, exact (hex floats), the player's
        // suffix taken out of the names so two runs compare by line.
        void track(long long tick) {
            for (const auto& [who, type]: o_.track) {
                std::vector<std::pair<std::string, std::string>> pieces;   // (entity, printed name)
                if (who == "orig") {
                    CK3dEntity* frame = bmmo::game::trafo_detail::entity(context_, bmmo::game::trafo_frame_name(type));
                    if (frame)
                        for (CK3dEntity* c = frame->HierarchyParser(nullptr); c; c = frame->HierarchyParser(c))
                            pieces.emplace_back(c->GetName(), "orig/" + std::string(c->GetName()));
                } else {
                    const auto* set = registry_.find(static_cast<uint32_t>(std::strtoul(who.c_str(), nullptr, 10)));
                    if (set)
                        for (CK_ID id: set->types[type].pieces) {
                            std::string n = name(id), printed = n;
                            const std::string suffix = bmmo::game::trafo_copy_suffix(set->player);
                            if (printed.size() > suffix.size() && printed.compare(printed.size() - suffix.size(), suffix.size(), suffix) == 0)
                                printed.resize(printed.size() - suffix.size());
                            pieces.emplace_back(n, who + "/" + printed);
                        }
                }
                for (const auto& [entity, printed]: pieces) {
                    bmmo::physics::body_state b;
                    std::string error;
                    if (!bmmo::physics::get_body_state(physics_, entity.c_str(), b, error)) {
                        // No body: where the entity is (a reset's TT Restore
                        // IC puts it back at its initial conditions).
                        auto* e = CK3dEntity::Cast(context_->GetObjectByNameAndParentClass(const_cast<CKSTRING>(entity.c_str()),
                                                                                           CKCID_3DENTITY, nullptr));
                        const VxMatrix& m = e ? e->GetWorldMatrix() : VxMatrix::Identity();
                        std::printf("piece t=%lld %s none ent=%a,%a,%a\n", tick, printed.c_str(), static_cast<double>(m[3][0]),
                                    static_cast<double>(m[3][1]), static_cast<double>(m[3][2]));
                        continue;
                    }
                    std::printf("piece t=%lld %s sim=%d pos=%a,%a,%a rot=%a,%a,%a,%a lin=%a,%a,%a ang=%a,%a,%a\n", tick,
                                printed.c_str(), b.simulated ? 1 : 0, b.position[0], b.position[1], b.position[2], b.rotation[0],
                                b.rotation[1], b.rotation[2], b.rotation[3], static_cast<double>(b.linear[0]),
                                static_cast<double>(b.linear[1]), static_cast<double>(b.linear[2]), static_cast<double>(b.angular[0]),
                                static_cast<double>(b.angular[1]), static_cast<double>(b.angular[2]));
                }
            }
        }

        void pose(long long tick) {
            bmmo::physics::world_hash h;
            std::string error;
            if (!bmmo::physics::capture_world_hash(physics_, h, error)) return;
            int bodies = 0;
            for (const auto& set: registry_.sets())
                for (const auto& ts: set.types)
                    for (CK_ID id: ts.pieces)
                        if (hooks_.has_body(name(id))) ++bodies;
            std::printf("trafo t=%lld pose=%016llx cores=%d piece_bodies=%d\n", tick, static_cast<unsigned long long>(h.pose), h.cores,
                        bodies);
        }

        void presim(long long tick) {
            const std::string q = bmmo::physics::describe_presimulate_queue(physics_);
            if (q != presim_last_) {
                std::printf("presim t=%lld %s\n", tick, q.c_str());
                presim_last_ = q;
            }
            // Non-BMMO entries (the savestate's check_callbacks compares
            // them), and those whose root script is a set's copy.
            int retail = 0, trafo = 0;
            std::string first;
            if (physics_->m_PreSimulateCallbacks)
                for (int slot = 0; slot < 3; ++slot)
                    for (int i = 0; i < physics_->m_PreSimulateCallbacks->m_Callbacks[slot].len(); ++i) {
                        PhysicsCallback* cb = physics_->m_PreSimulateCallbacks->m_Callbacks[slot].element_at(i);
                        if (!cb || cb->m_BehaviorID == 0) continue;
                        const char* type = typeid(*cb).name();
                        if (std::strstr(type, "navigation_callback") || std::strstr(type, "impulse_callback")
                                || std::strstr(type, "clock_guard_callback") || std::strstr(type, "tick_callback"))
                            continue;
                        ++retail;
                        CKBehavior* beh = cb->GetBehavior();
                        CKBehavior* root = beh;
                        while (root && root->GetParent()) root = root->GetParent();
                        if (root && root->GetName() && std::strstr(root->GetName(), "_Trafo")) {
                            ++trafo;
                            if (first.empty())
                                first = std::string(root->GetName()) + "/" + (beh && beh->GetName() ? beh->GetName() : "?") + " on "
                                      + (beh && beh->GetTarget() && beh->GetTarget()->GetName() ? beh->GetTarget()->GetName() : "-");
                        }
                    }
            presim_max_ = std::max(presim_max_, retail);
            presim_trafo_max_ = std::max(presim_trafo_max_, trafo);
            if (trafo && presim_first_.empty()) {
                presim_first_ = first;
                presim_first_tick_ = tick;
            }
        }

        // The sets' PhysicsCollDetection blocks (the CollSound copies'): per
        // block its target, whether local 0 (the listener) is set, whether
        // the target has a body, its two inputs and whether it is active -
        // printed when a block's line changes (S8's mechanism, design 9.2).
        void colldump(long long tick) {
            for (const auto& set: registry_.sets())
                for (CK_ID id: set.scripts) {
                    CKBehavior* script = CKBehavior::Cast(context_->GetObject(id));
                    int k = 0;
                    bmmo::game::trafo_detail::walk(script, [&](CKBehavior* beh) {
                        const int index = k++;
                        if (!beh->GetPrototypeName() || std::strcmp(beh->GetPrototypeName(), "PhysicsCollDetection") != 0) return;
                        CKObject* target = beh->GetTarget();
                        const std::string who = target && target->GetName() ? target->GetName() : "-";
                        char text[256];
                        std::snprintf(text, sizeof(text), "%s listener=%d body=%d create=%d stop=%d active=%d", who.c_str(),
                                      bmmo::game::trafo_detail::pointer_local(beh) ? 1 : 0, hooks_.has_body(who) ? 1 : 0,
                                      beh->IsInputActive(0) ? 1 : 0, beh->IsInputActive(1) ? 1 : 0, beh->IsActive() ? 1 : 0);
                        const std::string key = std::to_string(id) + "/" + std::to_string(index);
                        if (coll_last_[key] != text) {
                            coll_last_[key] = text;
                            std::printf("coll t=%lld %s/%d %s\n", tick, script && script->GetName() ? script->GetName() : "?", index, text);
                        }
                    });
                }
        }

        // S8's positive control: one PhysicsCollDetection block of the set's
        // CollSound copy whose listener is not set and whose target has no
        // body is given its Create and executed - the block's own code then
        // queues its PreSimulate callback, which stays queued while the
        // target has no body.  The sampler (--trafo-presim) must count it as
        // a _Trafo entry from this tick on.
        void poke_create(uint32_t player, long long tick) {
            const auto* set = registry_.find(player);
            CKBehavior* script = set ? CKBehavior::Cast(context_->GetObject(set->collsound)) : nullptr;
            CKBehavior* chosen = nullptr;
            bmmo::game::trafo_detail::walk(script, [&](CKBehavior* beh) {
                if (chosen || !beh->GetPrototypeName() || std::strcmp(beh->GetPrototypeName(), "PhysicsCollDetection") != 0) return;
                CKObject* target = beh->GetTarget();
                if (!target || !target->GetName() || hooks_.has_body(target->GetName())) return;
                if (bmmo::game::trafo_detail::pointer_local(beh)) return;
                chosen = beh;
            });
            if (!chosen) {
                std::printf("trafo poke t=%lld player %u: no idle PhysicsCollDetection block\n", tick, player);
                ++failures_;
                return;
            }
            chosen->ActivateInput(0, TRUE);
            const int result = chosen->Execute(bmmo::sim::kFixedDeltaMs);
            std::printf("trafo poke t=%lld player %u: Create executed on %s (target %s), result %d, listener=%d\n", tick, player,
                        chosen->GetName() ? chosen->GetName() : "?", chosen->GetTarget()->GetName(), result,
                        bmmo::game::trafo_detail::pointer_local(chosen) ? 1 : 0);
        }

        // Every built script's locals and outputs by place (script / walk
        // index : block), an object-typed value by its normalized name: what
        // differs between two worlds' built states (the built-state check).
        void built_params(const bmmo::game::trafo_set& set) {
            CKParameterManager* pm = context_->GetParameterManager();
            for (CK_ID id: set.scripts) {
                CKBehavior* script = CKBehavior::Cast(context_->GetObject(id));
                const std::string sname = bmmo::game::trafo_detail::normalized(bmmo::game::trafo_detail::name_of(script), set.player);
                int k = 0;
                bmmo::game::trafo_detail::walk(script, [&](CKBehavior* beh) {
                    const std::string place = sname + "/" + std::to_string(k++) + ":"
                                            + bmmo::game::trafo_detail::normalized(bmmo::game::trafo_detail::name_of(beh), set.player);
                    const auto value = [&](CKParameter* p) -> std::string {
                        if (!p) return "-";
                        const int size = p->GetDataSize();
                        const auto* data = static_cast<const unsigned char*>(p->GetReadDataPtr(FALSE));
                        if (size == static_cast<int>(sizeof(CK_ID)) && data && pm && pm->IsDerivedFrom(p->GetGUID(), CKPGUID_OBJECT)) {
                            CK_ID ref = 0;
                            std::memcpy(&ref, data, sizeof(ref));
                            CKObject* o = ref ? context_->GetObject(ref) : nullptr;
                            return "obj:" + (o ? bmmo::game::trafo_detail::normalized(bmmo::game::trafo_detail::name_of(o), set.player)
                                               : std::string(ref ? "?" : "0"));
                        }
                        std::string hex;
                        char b[4];
                        for (int i = 0; data && i < size; ++i) {
                            std::snprintf(b, sizeof(b), "%02x", data[i]);
                            hex += b;
                        }
                        return hex;
                    };
                    for (int i = 0; i < beh->GetLocalParameterCount(); ++i)
                        if (CKParameterLocal* l = beh->GetLocalParameter(i)) {
                            CKParameterType type = pm ? pm->ParameterGuidToType(l->GetGUID()) : -1;
                            CKParameterTypeDesc* desc = pm && type >= 0 ? pm->GetParameterTypeDescription(type) : nullptr;
                            std::printf("bp %s:local%d %s [%s:%s%s]\n", place.c_str(), i, value(l).c_str(),
                                        l->GetName() ? l->GetName() : "?", desc ? desc->TypeName.CStr() : "?",
                                        beh->IsUsingFunction() ? "" : " graph");
                        }
                    for (int i = 0; i < beh->GetOutputParameterCount(); ++i)
                        std::printf("bp %s:out%d %s\n", place.c_str(), i, value(beh->GetOutputParameter(i)).c_str());
                    std::printf("bp %s:io active=%d", place.c_str(), beh->IsActive() ? 1 : 0);
                    for (int i = 0; i < beh->GetInputCount(); ++i) std::printf(" in%d=%d", i, beh->IsInputActive(i) ? 1 : 0);
                    for (int i = 0; i < beh->GetOutputCount(); ++i) std::printf(" out%d=%d", i, beh->IsOutputActive(i) ? 1 : 0);
                    std::printf("\n");
                });
            }
        }

        // Every live piece's contacts and mindists by partner; a piece's
        // first sighting of a partner is printed, and one with a keep-local
        // body counted (S11).
        void contacts(long long tick) {
            for (const auto& set: registry_.sets())
                for (const auto& ts: set.types)
                    for (CK_ID id: ts.pieces) {
                        const std::string piece = name(id);
                        if (!hooks_.has_body(piece)) continue;
                        const std::string text = bmmo::physics::describe_contacts(physics_, piece.c_str());
                        // "fr=N{partner ...}{...} mind=M{partner ...}..."
                        const size_t mind = text.find(" mind=");
                        size_t at = 0;
                        while ((at = text.find('{', at)) != std::string::npos) {
                            const size_t end = text.find_first_of(" }", at + 1);
                            const std::string partner = text.substr(at + 1, end - at - 1);
                            const bool is_mind = mind != std::string::npos && at > mind;
                            const std::string key = piece + "|" + partner + (is_mind ? "|m" : "|f");
                            if (seen_.insert(key).second) {
                                std::printf("contact t=%lld %s %s %s\n", tick, piece.c_str(), partner.c_str(), is_mind ? "mind" : "fr");
                                if (bmmo::session::keep_local_body(partner)) {
                                    (is_mind ? local_mindists_ : local_contacts_)++;
                                    if (local_first_.empty()) {
                                        local_first_ = piece + " x " + partner + (is_mind ? " (mindist)" : " (contact)");
                                        local_first_tick_ = tick;
                                    }
                                }
                            }
                            at = end;
                        }
                    }
        }

        // Which pieces have a body and which hulls the manager has cached
        // (by the piece's mesh name), per type set, when it changes (S5).
        void surfaces(long long tick) {
            for (const auto& set: registry_.sets())
                for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
                    int bodies = 0, cached = 0;
                    uint64_t signature = 0;   // which hulls: a fresh compile changes it
                    for (CK_ID id: set.types[t].pieces) {
                        auto* e = CK3dEntity::Cast(context_->GetObject(id));
                        if (!e) continue;
                        if (hooks_.has_body(name(id))) ++bodies;
                        if (CKMesh* mesh = e->GetCurrentMesh(); mesh && mesh->GetName())
                            if (IVP_SurfaceManager* hull = physics_->GetCollisionSurface(mesh->GetName())) {
                                ++cached;
                                signature = signature * 1099511628211ull + reinterpret_cast<uintptr_t>(hull);
                            }
                    }
                    const std::string key = std::to_string(set.player) + "/" + std::to_string(t);
                    const std::pair<int, int> now{bodies, cached};
                    if (surfaces_[key] != now || hull_signature_[key] != signature) {
                        surfaces_[key] = now;
                        hull_signature_[key] = signature;
                        std::printf("surfaces t=%lld player %u %s bodies=%d hulls=%d hullsig=%016llx\n", tick, set.player,
                                    bmmo::session::trafo_type_name(t), bodies, cached, static_cast<unsigned long long>(signature));
                    }
                }
        }

        // Record v3 (design 7.1 "Record v3", run here because it needs CK):
        // (1) the v2 part of a v3 record is the v2 record byte for byte;
        // (2) a selected trafo script the record does not list is restored
        //     to its set's built state;
        // (3) a recorded trafo script that no longer exists fails the restore;
        // (4) a v2 record restored with a trafo part puts every trafo script
        //     to its built state.
        void record_check() {
            using bmmo::game::script_replay;
            const auto sel = script_replay::select(context_);
            const auto& forces = hooks_.forces;
            const auto check = [this](bool ok, const std::string& what) {
                std::printf("record-check %s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
                if (!ok) ++failures_;
            };
            std::vector<uint8_t> v2, v3;
            script_replay::capture(context_, sel, forces, v2);
            script_replay::capture(context_, sel, forces, v3, &registry_.part());
            const bool header = v2.size() >= 8 && v3.size() >= 8 && v3[4] == 3 && v2[4] == 2;
            const bool body = header && v3.size() > v2.size() && std::equal(v2.begin() + 8, v2.end(), v3.begin() + 8);
            check(header && body, "(1) the v3 record's mechanism part equals the v2 record (" + std::to_string(v2.size()) + " / "
                                  + std::to_string(v3.size()) + " bytes)");
            if (registry_.sets().size() < 2) {
                check(false, "(2)-(4) need two sets (--trafo-sets 2 or more)");
                return;
            }
            // (2): a record of the first set only; the others' scripts
            // perturbed; restored with every set selected.
            const uint32_t first = registry_.sets().front().player;
            script_replay::trafo_part only_first;
            for (CK_ID id: registry_.find(first)->scripts) {
                only_first.scripts.push_back(id);
                only_first.built[id] = registry_.find(first)->built.at(id);
            }
            std::vector<uint8_t> r1;
            script_replay::capture(context_, sel, forces, r1, &only_first);
            int perturbed = 0;
            for (const auto& set: registry_.sets()) {
                if (set.player == first) continue;
                for (CK_ID id: set.scripts) {
                    CKBehavior* s = CKBehavior::Cast(context_->GetObject(id));
                    s->ModifyFlags(CKBEHAVIOR_ACTIVATENEXTFRAME | CKBEHAVIOR_RESETNEXTFRAME, 0);
                    s->ActivateInput(0, TRUE);
                    ++perturbed;
                }
            }
            std::string error;
            const bool restored = script_replay::restore(context_, sel, forces, r1, &error, nullptr, nullptr, true, &registry_.part());
            bool built = restored;
            for (const auto& set: registry_.sets())
                for (CK_ID id: set.scripts) {
                    std::vector<uint8_t> now;
                    script_replay::capture_script_block(CKBehavior::Cast(context_->GetObject(id)), forces, now);
                    if (now != set.built.at(id)) built = false;
                }
            check(built, "(2) a record of set " + std::to_string(first) + " alone restored with every set selected: "
                         + std::to_string(perturbed) + " perturbed script(s) back at their built state, "
                         + std::to_string(script_replay::last_restore().trafo_built) + " put to built" + (error.empty() ? "" : " (" + error + ")"));
            // (4): a v2 record with the trafo part: every trafo script built.
            for (const auto& set: registry_.sets())
                for (CK_ID id: set.scripts) CKBehavior::Cast(context_->GetObject(id))->ModifyFlags(CKBEHAVIOR_ACTIVATENEXTFRAME, 0);
            const bool v2_ok = script_replay::restore(context_, sel, forces, v2, &error, nullptr, nullptr, true, &registry_.part());
            check(v2_ok && script_replay::last_restore().trafo_built == registry_.part().scripts.size(),
                  "(4) a v2 record restored with the trafo part: " + std::to_string(script_replay::last_restore().trafo_built) + " of "
                      + std::to_string(registry_.part().scripts.size()) + " scripts put to built" + (error.empty() ? "" : " (" + error + ")"));
            // (3): a record naming a set that is then destroyed.
            const uint32_t doomed = 999999u;
            const auto r = bmmo::game::build_trafo_set(context_, registry_, doomed, hooks_);
            if (!r.ok) {
                check(false, "(3) the extra set for the vanished-id check did not build: " + r.error);
                return;
            }
            std::vector<uint8_t> r3;
            script_replay::capture(context_, sel, forces, r3, &registry_.part());
            const bmmo::game::trafo_set gone = *registry_.find(doomed);
            hooks_.unregister_set(doomed, error);
            bmmo::game::destroy_trafo_set_objects(context_, gone);
            registry_.remove(doomed);
            const bool refused = !script_replay::restore(context_, sel, forces, r3, &error, nullptr, nullptr, true, &registry_.part());
            check(refused && error == "a recorded trafo script is gone",
                  "(3) a record naming a destroyed set is refused (" + error + ")");
            // Leave the sets as built for the rest of the run.
            for (const auto& set: registry_.sets())
                for (CK_ID id: set.scripts)
                    script_replay::restore_script_block(CKBehavior::Cast(context_->GetObject(id)), forces, set.built.at(id), &error);
        }

        trafo_tool_options o_;
        CKIpionManager* physics_ = nullptr;
        CKContext* context_ = nullptr;
        bmmo::game::trafo_set_hooks hooks_;
        bmmo::game::trafo_registry registry_;
        bmmo::game::trafo_hold hold_;
        bmmo::session::trafo_input_log log_;
        bool anchored_ = false;
        uint64_t leaks_ = 0;
        int failures_ = 0;
        std::map<std::string, std::string> coll_last_;
        std::string presim_last_, presim_first_;
        long long presim_first_tick_ = -1;
        int presim_max_ = 0, presim_trafo_max_ = 0;
        std::set<std::string> seen_;
        int local_contacts_ = 0, local_mindists_ = 0;
        std::string local_first_;
        long long local_first_tick_ = -1;
        std::map<std::string, std::pair<int, int>> surfaces_;
        std::map<std::string, uint64_t> hull_signature_;
    };
}
