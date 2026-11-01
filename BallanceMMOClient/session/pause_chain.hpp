#pragma once

// FULL step LGM (R7 part one): the retail pause chain's two session edits as
// the core host's own (session/core_host.cpp, a file-scope instance), so the
// core host no longer borrows the legacy path's BallanceMMOClient members
// pause_clock_ / pause_scripts_ (physics_session_client.cpp keeps its own
// copies until LGD deletes that path).  The pause chain is retail's alone: the
// headless client and the server's world have no pause menu, so this is no
// shared helper.  The rules, the checks and the log lines are the legacy
// copies' (BallanceMMOClient::pause_clock_* and pause_scripts_*), moved as
// they are:
//
// 1. The clock: Event_handler's "Pause Level" / "Unpause Level" chains each
//    write the physics time factor through a Set Physics Globals block (0 on
//    pause, 2.0 on unpause).  During a session both are rewritten to the
//    factor the run already has, so the menu's write is a no-op and the world
//    keeps stepping; the retail values go back when the session ends.
//
//    Unit conversion: the block feeds its "Physic Time Factor" input straight
//    to CKIpionManager::SetTimeFactor, which scales it by 0.001
//    (CKIpionManager.cpp:848) - the script value is 2 during normal play,
//    while get_clock() reports the scaled engine value (0.002).
//
// 2. The scripts: the same two chains stop and restart the level's gameplay
//    scripts ("Pause Level" deactivates Gameplay_Events and Gameplay_Ingame,
//    "Unpause Level" re-activates both).  The world they stop is exactly what
//    the session keeps simulating (the BallManager death test lives in
//    Gameplay_Ingame), so the "Script" target parameter of each of those
//    Deactivate / Activate Script blocks is emptied (a null CK_ID: Deactivate
//    Script hands null to CKScene::DeActivate, which does nothing, and
//    Activate Script skips a null target), the chain itself untouched.  A graph
//    that does not match - a missing chain, a target input with no usable
//    source, a target parameter an input other than ours also reads, more
//    targets than the table holds, no target at all - is refused as a whole and
//    logged as an error.  Gameplay_Tutorial is not in this set (a core session
//    keeps the tutorial running through the ESC menu overlay,
//    session/menu_overlay.hpp).
//
// Every method that writes re-resolves a recorded block in the live graph
// (prototype, parent chain, input type and size): the context recycles object
// ids when a level is torn down, so an id alone is not enough to write through.

#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CKAll.h"
#include "BML/ScriptHelper.h"

namespace bmmo::retail {
    class pause_chain {
    public:
        // A line to log (without the host's "Physics session: " prefix) and its
        // severity: 0 info, 1 warning, 2 error.
        using log_fn = std::function<void(int severity, const std::string& line)>;

        static constexpr float kTimeFactorScriptScale = 1000.0f;
        static constexpr int kScriptWrites = 8;

        // ---- the clock ----

        // Resolved from the level's Event_handler (`event_handler`, the
        // host's lookup of that script, IBML::GetScriptByName as before),
        // which exists by the time the session anchors.  False when either
        // block is missing.
        bool clock_resolve(CKBehavior* event_handler, const log_fn& log) {
            if (!event_handler) return false;
            static const char* kChains[2] = {"Pause Level", "Unpause Level"};
            bool resolved = true;
            for (int i = 0; i < 2; ++i) {
                clock_[i] = {};
                CKBehavior* chain = find_first(event_handler, kChains[i], false);
                CKBehavior* block = chain ? find_first(chain, "Set Physics Globals", true) : nullptr;
                auto* input = block ? block->GetInputParameter(1) : nullptr;
                CKParameter* param = input ? input->GetRealSource() : nullptr;
                if (!param) {
                    resolved = false;
                    continue;
                }
                clock_[i].behavior = block->GetID();
                float value = 0.0f;
                param->GetValue(&value);
                clock_[i].retail = value;
                clock_[i].applied = value;
            }
            if (resolved && log)
                log(0, std::format("pause chain time factors: pause={:.4f} unpause={:.4f}", clock_[0].retail,
                                   clock_[1].retail));
            return resolved;
        }

        // `factor`: the engine's scaled factor (get_clock()).
        void clock_apply(CKContext* context, float factor) {
            const float script = factor * kTimeFactorScriptScale;
            for (auto& write: clock_) {
                if (!write.behavior || write.applied == script) continue;
                CKParameter* param = clock_parameter(context, write.behavior);
                if (!param) continue;
                float value = script;
                param->SetValue(&value, sizeof(value));
                write.applied = script;
            }
        }

        void clock_restore(CKContext* context) {
            for (auto& write: clock_) {
                if (!write.behavior) continue;
                CKParameter* param = clock_parameter(context, write.behavior);
                if (param && write.applied != write.retail) {
                    float value = write.retail;
                    param->SetValue(&value, sizeof(value));
                }
                write = {};
            }
        }

        // ---- the scripts ----

        // Resolves and applies (the legacy pause_scripts_resolve).  Every
        // level (re)entry brings a new graph, so this runs at each anchor.
        bool scripts_resolve(CKContext* context, CKBehavior* event_handler, const log_fn& log) {
            count_ = 0;
            const auto refuse = [&](const std::string& why) {
                count_ = 0;
                failed_ = true;
                if (log)
                    log(2, "the pause menu's script edits were NOT neutralized (" + why
                               + "): the world will stop while the menu is open");
                return false;
            };
            if (!event_handler) return refuse("Event_handler is missing");
            static const char* kChains[2] = {"Pause Level", "Unpause Level"};
            std::vector<std::pair<const char*, CKBehavior*>> blocks;
            for (const char* chain_name: kChains) {
                CKBehavior* chain = find_first(event_handler, chain_name, false);
                if (!chain) return refuse(std::string(chain_name) + " is missing");
                collect_blocks(chain_name, chain, blocks);
            }
            // A target parameter can be shared (the retail level feeds
            // Gameplay_Events to the Pause chain's Deactivate Script and to the
            // Unpause chain's Activate Script through ONE parameter object):
            // emptying it is safe only when every reader is one of the inputs
            // this edit neutralizes.
            struct script_target {
                CKBehavior* block;
                const char* chain;
                int input;
                CKParameter* param;
                CK_ID retail;
                const char* name;
            };
            std::vector<script_target> candidates;
            for (const auto& [chain_name, block]: blocks) {
                const int inputs = block->GetInputParameterCount();
                for (int i = 0; i < inputs; ++i) {
                    CKParameterIn* input = block->GetInputParameter(i);
                    // Activate Script's input 0 is its "Reset ?" flag, so an
                    // input is a target when its type says so.
                    if (!input || input->GetGUID() != CKPGUID_SCRIPT) continue;
                    CKParameter* param = input->GetRealSource();
                    if (!param || param->GetDataSize() != static_cast<int>(sizeof(CK_ID)))
                        return refuse(std::format("{}/{} input {} has no usable target parameter", chain_name,
                                                  block->GetName() ? block->GetName() : "?", i));
                    CK_ID retail = 0;
                    param->GetValue(&retail);
                    auto* target = CKBehavior::Cast(block->GetInputParameterObject(i));
                    if (!target || !is_world_script(target->GetName())) continue;
                    candidates.push_back({block, chain_name, i, param, retail, target->GetName()});
                }
            }
            if (candidates.empty()) return refuse("no pause chain block targets the world scripts");
            bool seen[kWorldScriptCount] = {};
            std::string list;
            for (const auto& candidate: candidates) {
                const auto [readers, readers_text] = parameter_readers(context, candidate.param);
                int mine = 0;
                for (const auto& other: candidates)
                    if (other.param == candidate.param) ++mine;
                if (readers != mine)
                    return refuse(std::format("{}/{}'s target {} is read by {} inputs ({})", candidate.chain,
                                              candidate.block->GetName() ? candidate.block->GetName() : "?",
                                              candidate.name, readers, readers_text));
                if (count_ >= kScriptWrites) return refuse(std::format("more than {} target inputs", kScriptWrites));
                script_write& write = scripts_[count_++];
                write.block = candidate.block->GetID();
                write.chain = candidate.chain;
                write.input = candidate.input;
                write.retail = candidate.retail;
                write.applied = false;
                for (int s = 0; s < kWorldScriptCount; ++s)
                    if (std::strcmp(kWorldScripts[s], candidate.name) == 0) seen[s] = true;
                list += std::format("{}{}/{} -> {}", list.empty() ? "" : ", ", candidate.chain,
                                    candidate.block->GetName() ? candidate.block->GetName() : "?", candidate.name);
            }
            for (int s = 0; s < kWorldScriptCount; ++s)
                if (!seen[s] && log) log(1, std::string("no pause chain block targets ") + kWorldScripts[s]);
            if (log) log(0, "pause menu script edits neutralized: " + list);
            failed_ = false;
            scripts_apply(context, log);
            return true;
        }

        // Keeps the edits applied: a frame that finds a recorded target back
        // in place writes it out again.  Only the exact retail target of a
        // recorded input is ever moved, and only to null.
        void scripts_apply(CKContext* context, const log_fn& log) {
            if (!count_) return;
            const CK_ID zero = 0;
            for (int i = 0; i < count_; ++i) {
                script_write& write = scripts_[i];
                CKParameter* param = script_parameter(context, write.block, write.chain, write.input);
                const CK_ID current = value_of(param);
                if (current == 0) {
                    write.applied = true;   // already neutral (this session, or the same graph reloaded)
                    continue;
                }
                if (current != write.retail) continue;
                param->SetValue(&zero, sizeof(CK_ID));
                if (!write.applied) {
                    write.applied = true;
                    if (log) log(0, std::format("pause chain target {} emptied (input {})", write.retail, write.input));
                }
            }
        }

        // Hands the retail targets back and drops the table.
        void scripts_restore(CKContext* context) {
            for (int i = 0; i < count_; ++i) {
                script_write& write = scripts_[i];
                CKParameter* param = script_parameter(context, write.block, write.chain, write.input);
                if (value_of(param) == 0 && write.retail) {
                    const CK_ID retail = write.retail;
                    param->SetValue(&retail, sizeof(CK_ID));
                }
                write = {};
            }
            count_ = 0;
            failed_ = false;
        }

        int scripts_count() const { return count_; }
        bool scripts_failed() const { return failed_; }

        // The 'pausechain' read's per-entry tail, the legacy line's:
        // " [<chain>/<prototype> input<N> retail=<id> now=<id>]" each.
        std::string scripts_entries(CKContext* context) const {
            std::string out;
            for (int i = 0; i < count_; ++i) {
                const script_write& write = scripts_[i];
                CKParameter* param = script_parameter(context, write.block, write.chain, write.input);
                auto* block = CKBehavior::Cast(context ? context->GetObject(write.block) : nullptr);
                const char* prototype = block && block->IsUsingFunction() ? block->GetPrototypeName() : "?";
                out += std::format(" [{}/{} input{} retail={} now={}]", write.chain ? write.chain : "?",
                                   prototype ? prototype : "?", write.input, write.retail, value_of(param));
            }
            return out;
        }

        // How many behavior inputs read one parameter, and which ones (the
        // shared-reader check; also the reason text the host's own refusals
        // name).
        static std::pair<int, std::string> parameter_readers(CKContext* context, CKParameter* param) {
            std::string text;
            int readers = 0;
            if (!context || !param) return {0, text};
            const int behaviors = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int b = 0; b < behaviors; ++b) {
                auto* other = CKBehavior::Cast(context->GetObject(ids[b]));
                const int inputs = other ? other->GetInputParameterCount() : 0;
                for (int k = 0; k < inputs; ++k) {
                    CKParameterIn* in = other->GetInputParameter(k);
                    if (!in || in->GetRealSource() != param) continue;
                    ++readers;
                    if (readers > 8) continue;
                    CKBehavior* owner = other->GetParent();
                    text += std::format("{}{}/{} input {}", text.empty() ? "" : ", ",
                                        owner && owner->GetName() ? owner->GetName() : "?",
                                        other->GetName() ? other->GetName() : "?", k);
                }
            }
            return {readers, text};
        }

    private:
        struct clock_write {
            CK_ID behavior = 0;
            float retail = 0.0f;
            float applied = 0.0f;
        };
        struct script_write {
            CK_ID block = 0;                 // the Deactivate/Activate Script block
            const char* chain = nullptr;     // the pause chain it belongs to
            int input = 0;                   // its "Script" input
            CK_ID retail = 0;                // the script it pointed at
            bool applied = false;
        };

        static constexpr const char* kWorldScripts[] = {"Gameplay_Events", "Gameplay_Ingame"};
        static constexpr int kWorldScriptCount = 2;

        clock_write clock_[2];   // [0] Pause Level, [1] Unpause Level
        script_write scripts_[kScriptWrites];
        int count_ = 0;
        bool failed_ = false;

        static bool is_world_script(const char* name) {
            if (!name) return false;
            for (const char* candidate: kWorldScripts)
                if (std::strcmp(name, candidate) == 0) return true;
            return false;
        }

        // BML's ScriptHelper::FindFirstBB, called exactly as the legacy copies
        // call it: the chain among the script's sub-behaviors, then the Set
        // Physics Globals block anywhere below that chain (`hierarchically`).
        static CKBehavior* find_first(CKBehavior* script, const char* name, bool hierarchically) {
            return ScriptHelper::FindFirstBB(script, name, hierarchically);
        }

        // The Deactivate Script / Activate Script blocks of one pause chain, in
        // graph order.  They are leaves; a group below a chain (the Unpause
        // Level chain has one) is walked into.
        static void collect_blocks(const char* chain, CKBehavior* behavior,
                                   std::vector<std::pair<const char*, CKBehavior*>>& out) {
            const int count = behavior ? behavior->GetSubBehaviorCount() : 0;
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = behavior->GetSubBehavior(i);
                if (!sub) continue;
                const char* prototype = sub->IsUsingFunction() ? sub->GetPrototypeName() : nullptr;
                if (prototype
                        && (std::strcmp(prototype, "Deactivate Script") == 0
                            || std::strcmp(prototype, "Activate Script") == 0)) {
                    out.emplace_back(chain, sub);
                    continue;
                }
                collect_blocks(chain, sub, out);
            }
        }

        // A recorded Set Physics Globals block's time factor parameter.
        static CKParameter* clock_parameter(CKContext* context, CK_ID behavior) {
            auto* block = CKBehavior::Cast(context ? context->GetObject(behavior) : nullptr);
            auto* input = block ? block->GetInputParameter(1) : nullptr;
            return input ? input->GetRealSource() : nullptr;
        }

        // A recorded entry, resolved back into the live graph: the block has to
        // still be a Deactivate/Activate Script block inside the chain it was
        // found in, holding a CK_ID-sized script parameter.  A level change
        // that leaves a stale table behind fails those checks and is skipped
        // until the next anchor resolves the new graph.
        static CKParameter* script_parameter(CKContext* context, CK_ID block_id, const char* chain, int input_index) {
            auto* block = CKBehavior::Cast(context ? context->GetObject(block_id) : nullptr);
            if (!block || !block->IsUsingFunction()) return nullptr;
            const char* prototype = block->GetPrototypeName();
            if (!prototype
                    || (std::strcmp(prototype, "Deactivate Script") != 0
                        && std::strcmp(prototype, "Activate Script") != 0))
                return nullptr;
            CKBehavior* parent = block->GetParent();
            const char* parent_name = parent ? parent->GetName() : nullptr;
            if (!chain || !parent_name || std::strcmp(parent_name, chain) != 0) return nullptr;
            CKParameterIn* input = block->GetInputParameter(input_index);
            if (!input || input->GetGUID() != CKPGUID_SCRIPT) return nullptr;
            CKParameter* param = input->GetRealSource();
            if (!param || param->GetDataSize() != static_cast<int>(sizeof(CK_ID))) return nullptr;
            return param;
        }

        static CK_ID value_of(CKParameter* param) {
            CK_ID value = 0;
            if (param) param->GetValue(&value);
            return value;
        }
    };
}
