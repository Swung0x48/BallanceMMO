#pragma once

// The level's fall handler for free props, in every world at one tick (FULL
// step PRX; world-agreement-gaps-design.md "As built (step INV)" finding 1,
// the user's (b) of 2026-10-30: every world removes a fallen prop by the
// original logic).
//
// Gameplay_Ingame/DepthTest (started by Init Ingame -> get maxDepth at the
// level start, running as long as Gameplay_Ingame runs): an Op takes the
// lowest DepthTestCubes y (get maxDepth) minus 200; a Group Iterator walks
// the group "DepthTest" (the level's free props, P_Box_MF / P_Ball_*_MF) one
// member per frame, forever; Test (y < threshold) -> Physicalize
// (Unphysicalize) -> Hide -> Set Position (0,0,0).  In a session the block
// cannot be kept as it is:
//   * every client's body guard (engine change #6) refuses its
//     Unphysicalize, so only the server's world removed a fallen prop;
//   * letting it through would still remove the prop at a per-world tick:
//     the iterator's phase is the count of live frames each world played
//     since its level start, and Gameplay_Ingame is not replayed
//     (script_replay::select takes no satellite), so a rollback neither
//     restores the phase nor removes the prop again.
// So the block's Test is pinned (its threshold input fed a session constant
// that no y can be below - the N2 pin's way, game/sector_deactivate_pin.hpp)
// and every world - the server's, the headless client's, the retail core's -
// runs the block's own rule from here instead, at one point of the tick:
// before the tick's behaviour pass, after its births and the union's pass
// and the trafo frame (live: session_client_core::pre_step after
// trafo_frame; replay: the core's before_replayed_tick; the server:
// physics_world::tick after trafo_frame).  For every member of the block's
// group with a body, the block's predicate on the body's y as the step left
// it (the entity's y, which physics_RT writes from the same pose after each
// step, only pre-filters); true -> the body removed (the block's
// Unphysicalize: delete_silently, bmmo::physics::unphysicalize, which the
// guard does not see), the entity hidden (Hide, hierarchy off) and put at
// (0,0,0) (Set Position, no referential, children moved).  The decision is a
// function of the world's state at the start of the tick, so every world
// that agrees on the bodies removes the prop at the same tick, and a replay
// that re-steps the tick removes it again by itself (no record needed: the
// state is the record).  What changes from the retail block: the iterator's
// one-member-per-frame poll (a removal up to N-1 frames after the crossing,
// N = the group's size) becomes every member every tick - a removal at the
// first tick after the crossing, in every world.
//
// A rewind across a removal: the core's restore revives the prop (the
// hosts' revivable names the group's members: rollback_world::revive_body,
// gap L's mechanism half) and the replay removes it again at its tick; the
// entity this rule hid is shown again when it has a body again (a revived
// prop whose corrected history does not cross).
//
// Only the public CK2 API and duck-typed physics backends (physics_direct,
// physics_view or a pointer to one: get_body_state, unphysicalize): the
// header compiles in the x86 client target (Virtools SDK) and against
// Ballanced.

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <type_traits>

#include "CKAll.h"

#include <physics/physics_rt_api.h>

#include "menu_driver.hpp"

namespace bmmo::game {
    class depth_test_rule {
    public:
        static constexpr const char* kConstantName = "BMMO_DepthTestOff";
        // A member whose entity stands this far above the threshold is not
        // looked up (the entity is the body's pose after the last step; the
        // body's y decides).
        static constexpr float kPrefilter = 50.0f;

        // Finds the block, pins its Test and names the group's members.
        // Empty when pinned, otherwise why not (nothing changed then: the
        // block runs as the level has it).  Call restore() first when a
        // previous pin may still be in place.
        std::string apply(CKContext* context) {
            clear();
            if (!context) return "no context";
            CKBehavior* ingame = find_root_script(context, "Gameplay_Ingame");
            if (!ingame) return "Gameplay_Ingame is missing";
            CKBehavior* graph = nullptr;
            for (int i = 0; i < ingame->GetSubBehaviorCount(); ++i) {
                CKBehavior* sub = ingame->GetSubBehavior(i);
                if (!sub || !sub->GetName() || std::strcmp(sub->GetName(), "DepthTest") != 0) continue;
                if (graph) return "Gameplay_Ingame has more than one 'DepthTest'";
                graph = sub;
            }
            if (!graph) return "Gameplay_Ingame has no 'DepthTest'";
            CKBehavior *test = nullptr, *physicalize = nullptr, *iterator = nullptr, *threshold_op = nullptr;
            for (int i = 0; i < graph->GetSubBehaviorCount(); ++i) {
                CKBehavior* sub = graph->GetSubBehavior(i);
                const char* prototype = sub && sub->IsUsingFunction() ? sub->GetPrototypeName() : nullptr;
                if (!prototype) continue;
                CKBehavior** slot = std::strcmp(prototype, "Test") == 0 ? &test
                                  : std::strcmp(prototype, "Physicalize") == 0 ? &physicalize
                                  : std::strcmp(prototype, "Group Iterator") == 0 ? &iterator : nullptr;
                if (!slot) continue;
                if (*slot) return std::string("'DepthTest' has more than one ") + prototype;
                *slot = sub;
            }
            if (!test || !physicalize || !iterator) return "'DepthTest' lacks its Test, Physicalize or Group Iterator";
            bool unphysicalizes = false;
            for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
                CKBehaviorLink* link = graph->GetSubBehaviorLink(i);
                CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                if (!from || !to) continue;
                // The graph's own input: the Op it starts is the threshold's.
                if (from->GetOwner() == graph && from->GetType() == CK_BEHAVIORIO_IN && to->GetOwner() != graph) {
                    if (threshold_op) return "'DepthTest''s input starts more than one block";
                    threshold_op = CKBehavior::Cast(to->GetOwner());
                }
                // Test's True -> Physicalize's Unphysicalize: the block this
                // rule replaces.
                if (from->GetOwner() == test && to->GetOwner() == physicalize && test->GetOutput(0) == from
                    && physicalize->GetInput(1) == to)
                    unphysicalizes = true;
            }
            if (!unphysicalizes) return "'DepthTest''s Test does not lead to an Unphysicalize";
            CKParameterOut* threshold = threshold_op ? threshold_op->GetOutputParameter(0) : nullptr;
            if (!threshold || threshold->GetGUID() != CKPGUID_FLOAT) return "'DepthTest' has no float threshold Op";
            // Test reads the Op's result directly or through a local the
            // result is copied into (a destination of the output: the
            // level's DepthTest reads its local "res").
            const auto reads_threshold = [threshold](CKParameter* source) {
                if (!source) return false;
                if (source == threshold) return true;
                for (int d = 0; d < threshold->GetDestinationCount(); ++d)
                    if (threshold->GetDestination(d) == source) return true;
                return false;
            };
            int input = -1;
            for (int k = 1; k <= 2; ++k) {
                CKParameterIn* in = test->GetInputParameter(k);
                if (in && reads_threshold(in->GetRealSource())) {
                    if (input >= 0) return "both of Test's operands read the threshold";
                    input = k;
                }
            }
            if (input < 0) {
                std::string what = "Test does not read the threshold Op (" + describe(threshold_op) + "'s result";
                for (int k = 1; k <= 2; ++k) {
                    CKParameterIn* in = test->GetInputParameter(k);
                    CKParameter* source = in ? in->GetRealSource() : nullptr;
                    what += std::string("; operand ") + std::to_string(k) + " reads "
                          + (source && source->GetName() ? source->GetName() : "?") + " of "
                          + describe(source ? CKBehavior::Cast(source->GetOwner()) : nullptr);
                }
                return what + ")";
            }
            int op = 0;
            CKParameterIn* op_in = test->GetInputParameter(0);
            CKParameter* op_source = op_in ? op_in->GetRealSource() : nullptr;
            if (!op_source || op_source->GetDataSize() != static_cast<int>(sizeof(int)) || op_source->GetValue(&op, FALSE) != CK_OK)
                return "Test's comparison operator cannot be read";
            // y below the threshold: A < B / A <= B with the threshold as B,
            // A > B / A >= B with it as A (CompOperator: 3 <, 4 <=, 5 >, 6 >=).
            if (input == 2 && (op == 3 || op == 4)) inclusive_ = op == 4;
            else if (input == 1 && (op == 5 || op == 6)) inclusive_ = op == 6;
            else return "Test compares the threshold with operator " + std::to_string(op) + " as operand "
                        + std::to_string(input) + ", not 'y below it'";
            CKParameterIn* pinned = test->GetInputParameter(input);
            // The session's constant: no finite y is below -FLT_MAX, so the
            // block's True never fires.
            auto* constant = context->CreateCKParameterLocal(const_cast<CKSTRING>(kConstantName), CKPGUID_FLOAT, TRUE);
            if (!constant) return "could not create the session's constant";
            const float never = -FLT_MAX;
            constant->SetValue(&never, sizeof(never));
            CKParameter* real = pinned->GetRealSource();
            CKParameterIn* shared = pinned->GetSharedSource();
            CKParameter* direct = pinned->GetDirectSource();
            if (pinned->SetDirectSource(constant) != CK_OK) {
                context->DestroyObject(constant);
                return "Test's threshold input refused the session's constant";
            }
            graph_ = graph->GetID();
            test_ = test->GetID();
            iterator_ = iterator->GetID();
            input_ = input;
            // The value Test compares: what its input read (the Op's result
            // or the local it is copied into).
            threshold_ = real ? real->GetID() : threshold->GetID();
            retail_source_ = direct ? direct->GetID() : 0;
            retail_shared_ = shared ? shared->GetID() : 0;
            retail_real_ = real ? real->GetID() : 0;
            constant_ = constant->GetID();
            if (CKGroup* group = CKGroup::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("DepthTest"), CKCID_GROUP, nullptr)))
                name_members(group);
            where_ = "Gameplay_Ingame/DepthTest: Test's " + std::string(input == 2 ? "B" : "A") + " (threshold, "
                   + (inclusive_ ? "<=" : "<") + ") pinned; the group 'DepthTest' holds " + std::to_string(members_.size())
                   + " member(s)" + (duplicates_.empty() ? "" : "; name(s) held twice: " + duplicates_);
            return {};
        }

        bool pinned() const { return constant_ != 0; }
        const std::string& where() const { return where_; }
        uint64_t removals() const { return removals_; }
        uint64_t shown_again() const { return shown_again_; }
        // A member of the block's group (the hosts' revivable: a prop this
        // rule removed inside a rewound window is made again at the
        // restore, and the replay removes it at its tick).
        bool member(const std::string& name) const { return members_.count(name) != 0; }

        // The frame guard's keep: the input back onto the constant when
        // something handed it back to the source it had.  True when it
        // re-pinned.
        bool keep(CKContext* context) {
            CKParameterIn* in = pinned_input(context);
            auto* constant = constant_ ? CKParameter::Cast(context->GetObject(constant_)) : nullptr;
            if (!in || !constant) return false;
            CKParameter* current = in->GetRealSource();
            if (current == constant) return false;
            if (!current || current->GetID() != retail_real_) return false;
            return in->SetDirectSource(constant) == CK_OK;
        }

        // Hands the input back and destroys the constant (kept when the
        // input still reads it and could not be handed back).  False when
        // the input was left on the constant.  The entities this rule hid
        // stay as they are (the level's exit or reload owns them).
        bool restore(CKContext* context) {
            bool ok = true;
            if (context && constant_) {
                CKParameterIn* in = pinned_input(context);
                auto* constant = CKParameter::Cast(context->GetObject(constant_));
                bool referenced = false;
                if (in && constant && in->GetRealSource() == constant) {
                    referenced = true;
                    if (retail_shared_) {
                        auto* other = CKParameterIn::Cast(context->GetObject(retail_shared_));
                        if (other && in->ShareSourceWith(other) == CK_OK) referenced = false;
                    } else if (auto* source = CKParameter::Cast(context->GetObject(retail_source_))) {
                        if (in->SetDirectSource(source) == CK_OK) referenced = false;
                    }
                }
                if (constant && !referenced) context->DestroyObject(constant);
                ok = !referenced;
            }
            clear();
            return ok;
        }

        // The block's rule for this tick (see the header comment): every
        // member of its group with a body below the threshold removed,
        // hidden and put at (0,0,0); an entity removed earlier that has a
        // body again (a rewind's revive) shown.  Nothing before the block
        // has run (its threshold and group are the block's own Ops'
        // outputs).  `log` takes one line per removal (and the threshold's
        // first reading).
        template <class Backend>
        void step(CKContext* context, const Backend& backend, uint32_t tick,
                  const std::function<void(const std::string&)>& log) {
            if (!context || !constant_) return;
            auto* graph = CKBehavior::Cast(context->GetObject(graph_));
            if (!graph) return;
            // The threshold: the block's Op output, read while the graph is
            // active (before its first activation its Ops have not run).
            // Latched (PRX review): once read, the rule runs until restore()
            // / apply() whatever the graph's activity now - Gameplay_Ingame
            // is not replayed, so its activity at a replayed tick is today's,
            // and a Level Reset's re-activation leaves it inactive in one
            // world for some frames while the other worlds run the rule; the
            // threshold is the level's (the Op recomputes the same value).
            float threshold = 0.0f;
            bool read = false;
            if (graph->IsActive()) {
                auto* threshold_param = CKParameter::Cast(context->GetObject(threshold_));
                read = threshold_param && threshold_param->GetValue(&threshold, FALSE) == CK_OK;
            }
            if (!read) {
                if (!have_threshold_) return;   // not started yet
                threshold = threshold_value_;
            }
            CKGroup* group = block_group(context);
            if (!group) return;
            if (!have_threshold_ || threshold != threshold_value_ || group->GetID() != group_) {
                have_threshold_ = true;
                threshold_value_ = threshold;
                group_ = group->GetID();
                name_members(group);
                if (log) {
                    char text[200];
                    std::snprintf(text, sizeof(text), "depth test: threshold %.3f (%a), group %s with %d member(s), from tick %u",
                                  static_cast<double>(threshold), static_cast<double>(threshold),
                                  group->GetName() ? group->GetName() : "?", group->GetObjectCount(), tick);
                    log(text);
                    if (!duplicates_.empty())
                        log("depth test: member name(s) " + duplicates_ + " held by more than one entity: the rule acts on the body of the name");
                }
            }
            const auto& b = backend_of(backend);
            for (int i = 0; i < group->GetObjectCount(); ++i) {
                auto* entity = CK3dEntity::Cast(group->GetObject(i));
                const char* name = entity ? entity->GetName() : nullptr;
                if (!name || !*name) continue;
                const bool ours = removed_.count(entity->GetID()) != 0;
                if (!ours) {
                    VxVector position;
                    entity->GetPosition(&position, nullptr);
                    if (!(position.y < threshold + kPrefilter)) continue;
                }
                bmmo_physics_body_state body{};
                std::string error;
                if (!b.get_body_state(name, body, error)) continue;   // no body
                if (ours) {
                    // Made again by a rewind's restore: shown again; the
                    // replay's rule takes it away again if it still falls.
                    removed_.erase(entity->GetID());
                    entity->Show(CKSHOW);
                    ++shown_again_;
                }
                const float y = static_cast<float>(body.position[1]);
                if (!(inclusive_ ? y <= threshold : y < threshold)) continue;
                if (!b.unphysicalize(name, error)) {
                    if (log && failures_++ < 8) log(std::string("depth test: ") + name + " could not be removed: " + error);
                    continue;
                }
                entity->Show(CKHIDE);
                VxVector origin(0.0f, 0.0f, 0.0f);
                entity->SetPosition(&origin, nullptr, FALSE);
                removed_.insert(entity->GetID());
                ++removals_;
                if (log && removal_logs_++ < 32) {   // a rewind across it removes it again: the first 32
                    char text[240];
                    std::snprintf(text, sizeof(text), "depth test: %s removed at tick %u (body y %.3f below %.3f; removal %llu)",
                                  name, tick, static_cast<double>(y), static_cast<double>(threshold),
                                  static_cast<unsigned long long>(removals_));
                    log(text);
                }
            }
        }

    private:
        template <class B> static decltype(auto) backend_of(const B& b) {
            if constexpr (std::is_pointer_v<B>) return (*b);
            else return (b);
        }

        static std::string describe(CKBehavior* block) {
            if (!block) return "?";
            const char* name = block->GetName();
            return std::string(name ? name : "?") + " #" + std::to_string(static_cast<unsigned long>(block->GetID()));
        }

        void clear() {
            graph_ = test_ = iterator_ = threshold_ = group_ = constant_ = 0;
            retail_source_ = retail_shared_ = retail_real_ = 0;
            input_ = -1;
            inclusive_ = false;
            have_threshold_ = false;
            threshold_value_ = 0.0f;
            members_.clear();
            removed_.clear();
            where_.clear();
            duplicates_.clear();
            removals_ = shown_again_ = 0;
            failures_ = removal_logs_ = 0;
        }

        CKParameterIn* pinned_input(CKContext* context) const {
            auto* test = context && test_ ? CKBehavior::Cast(context->GetObject(test_)) : nullptr;
            return test && input_ > 0 ? test->GetInputParameter(input_) : nullptr;
        }

        // The group the block's iterator walks (its Group input, the Op
        // that names "DepthTest"); by that name while the Op has not run.
        CKGroup* block_group(CKContext* context) const {
            auto* iterator = CKBehavior::Cast(context->GetObject(iterator_));
            CKParameterIn* in = iterator ? iterator->GetInputParameter(0) : nullptr;
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            CK_ID id = 0;
            if (source && source->GetDataSize() == static_cast<int>(sizeof(CK_ID)) && source->GetValue(&id, FALSE) == CK_OK && id)
                if (CKGroup* group = CKGroup::Cast(context->GetObject(id))) return group;
            return CKGroup::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("DepthTest"), CKCID_GROUP, nullptr));
        }

        // The members' names; a name two members share goes to duplicates_
        // (the physics API resolves a body by name: the rule would act on
        // that name's body for both; no level has one, PRX's census).
        void name_members(CKGroup* group) {
            members_.clear();
            duplicates_.clear();
            for (int i = 0; i < group->GetObjectCount(); ++i) {
                CKBeObject* object = group->GetObject(i);
                if (!object || !object->GetName() || !*object->GetName()) continue;
                if (!members_.insert(object->GetName()).second)
                    duplicates_ += (duplicates_.empty() ? "" : ", ") + std::string(object->GetName());
            }
        }

        CK_ID graph_ = 0, test_ = 0, iterator_ = 0, threshold_ = 0, group_ = 0, constant_ = 0;
        CK_ID retail_source_ = 0, retail_shared_ = 0, retail_real_ = 0;
        int input_ = -1;
        bool inclusive_ = false;
        bool have_threshold_ = false;
        float threshold_value_ = 0.0f;
        std::set<std::string> members_;
        std::set<CK_ID> removed_;
        std::string where_, duplicates_;
        uint64_t removals_ = 0, shown_again_ = 0;
        uint32_t failures_ = 0, removal_logs_ = 0;
    };
}
