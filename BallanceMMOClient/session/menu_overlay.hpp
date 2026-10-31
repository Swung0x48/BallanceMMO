#pragma once

// FULL step ESCX (roadmap section 4 item 6, RK3E; the user, 2026-10-30): in a
// core session the ESC menu is an overlay.  It opens, closes and works as in
// vanilla play, and the game behind it keeps running as if it were not open -
// the world, the level's scripts (the tutorial included), the points timer,
// the own ball's physics.  The own ball takes no keys while it is open; that
// half is the core host's existing mute (core_host.cpp, host.input_muted:
// IsIngame && IsPaused, recorded and sent as neutral frames) and is not here.
//
// What the session already did before this step: the Event_handler's "Pause
// Level" / "Unpause Level" chains neither stop nor restart Gameplay_Events and
// Gameplay_Ingame (BallanceMMOClient::pause_scripts_resolve), and their time
// factor writes are pinned to the factor in use (pause_clock_apply).  Three
// pieces of the vanilla pause were still in force; this overlay takes them
// out, for a core session only (retail-only, host-local; the legacy path and
// offline play are untouched):
//
// 1. The ESC re-pause (RK3E's symptom).  Gameplay_Events' ESC Key Event
//    (Pressed -> Menu_Click -> BallNav deactivate -> "Pause Level") stays
//    live while the menu is open, because the session keeps the script
//    running; the press that closes the menu (Menu_Pause's own ESC ->
//    "Unpause Level") also fired it, and the menu paused again in the same
//    frame.  In vanilla play the script is stopped by the pause with the
//    block's "Key State" output still TRUE from the opening press, and Key
//    Event fires Pressed only on a FALSE -> down edge (KeyEvent.cpp), so the
//    closing press is ignored and the first release afterwards resets the
//    state.  The overlay reproduces exactly that: while the menu is open the
//    block's Key State is written TRUE every frame; when it closes nothing is
//    written any more, the block sees the key up (or the release) and goes
//    back to FALSE by itself, and the next press opens the menu again.
//    Released (output 1) fires on each such release while the menu is open;
//    the block is accepted only when that output leads nowhere.
// 2. The tutorial.  "Pause Level" deactivates Gameplay_Tutorial and "Unpause
//    Level" re-activates it (when the level's tutorial flags say so).  The
//    overlay re-points the Script input of those Deactivate / Activate Script
//    blocks at a session constant holding no script (CK_ID 0: Deactivate
//    Script hands null to CKScene::DeActivate, which does nothing; Activate
//    Script skips a null target), so the tutorial keeps running.  A
//    re-pointed input, not an emptied parameter: whatever else reads the
//    retail parameter keeps its value.
// 3. The points timer.  Gameplay_Energy's Switch On Message pauses its
//    TT_Timer on "Pause Level" and plays it on "Unpause Level" (the menu's
//    Back / ESC sends that to All_Gameplay).  The overlay re-points those two
//    message inputs at a session constant holding a message type nobody
//    sends, so the timer is neither paused nor resumed by the menu; the
//    level's own "Counter inactive" / "Counter active" keep working.  The
//    same for every other gameplay script (on All_Gameplay) that listens:
//    Gameplay_Blitz's lightning on a level with a thunderstorm (cosmetic;
//    its Random block is not one the session patches).  The census logged
//    at the anchor names every remaining listener (Event_handler = the
//    menu, Sound_Manager = the menu's audio).
//
// Each piece is found structurally at the anchor and is independent: one
// that does not match is reported and left out, the others still apply.
// Everything is handed back at the session's end, the ESC hold only once the
// menu has closed when the session ends with it open (end() / release()).
// The never-sent message type stays registered for the process (the message
// manager has no removal; local numbering, compared nowhere).
// Not touched: the menu's own work - the cursor, End / Start Music, the
// Unpause chain's Gameplay_Refresh activation (key bindings; its tutorial
// time-factor write is held by step DX2), BallNav activate / deactivate (the
// own replica drives the ball).  A key the menu reads reaches the menu only:
// step ESCK's key mask (menu_keys.hpp) hides it from the level's scripts.
//
// Retail-only (the Virtools SDK of the mod).  File-scope state of
// core_host.cpp: BallanceMMOClient keeps its layout.

#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "CKAll.h"

namespace bmmo::retail {
    class menu_overlay {
    public:
        static constexpr const char* kNoScriptName = "BMMO_MenuOverlay_NoScript";
        static constexpr const char* kNoMessageName = "BMMO_MenuOverlay_NoMessage";
        static constexpr const char* kNoMessageType = "BMMO menu overlay (never sent)";

        // Finds the three pieces and re-points 2 and 3.  Returns the lines
        // to log, each prefixed "ok " or "error ".
        std::vector<std::string> resolve(CKContext* context) {
            std::vector<std::string> lines;
            restore(context);
            if (!context) return {"error no context"};
            CKMessageManager* messages = context->GetMessageManager();
            if (!messages) return {"error no message manager"};
            pause_type_ = messages->AddMessageType(const_cast<CKSTRING>("Pause Level"));
            unpause_type_ = messages->AddMessageType(const_cast<CKSTRING>("Unpause Level"));
            lines.push_back(resolve_esc(context));
            lines.push_back(resolve_tutorial(context));
            lines.push_back(resolve_timer(context, messages));
            lines.push_back("ok census: " + census(context));
            return lines;
        }

        // Every frame of a core session: keeps 2 and 3 re-pointed (a level
        // reset may hand an input back) and, while the menu is open, holds
        // the ESC block's Key State at TRUE.
        void apply(CKContext* context, bool menu_open) {
            if (!context) return;
            keep(context, tutorial_, tutorial_constant_);
            keep(context, timer_, timer_constant_);
            if (menu_open && !was_open_) ++openings_;
            was_open_ = menu_open;
            if (menu_open) hold_esc(context);
        }

        // Hands every re-pointed input back and drops the constants.  The
        // ESC block needs nothing (its own next frame resets Key State).
        void restore(CKContext* context) {
            if (context) {
                hand_back(context, tutorial_, tutorial_constant_);
                hand_back(context, timer_, timer_constant_);
            }
            tutorial_.clear();
            timer_.clear();
            tutorial_constant_ = timer_constant_ = 0;
            esc_block_ = 0;
            esc_pending_ = false;
            was_open_ = false;
        }

        // The session's end (core_host_end_local).  Pieces 2 and 3 are
        // handed back at once: the Unpause chain's Gameplay_Tutorial
        // activation (#2193) has Reset FALSE, so on a tutorial that never
        // stopped it changes nothing, and Play on a running points timer
        // changes nothing either.  Piece 1 cannot go yet when the session
        // ends with the menu open: the session kept Gameplay_Events running,
        // so the closing ESC would see the Key Event's Key State FALSE, fire
        // Pressed -> "Pause Level" through the handed-back chains, and open
        // the menu again in the frame Menu_Pause's own ESC closes it (RK3E's
        // symptom, once).  The hold then stays until the menu closes
        // (release(), called every frame outside a core session); after
        // that the block resets itself as in-session, and the next ESC is
        // the vanilla pause (Gameplay_Events stopped by the restored chain).
        // Returns true when the hold is kept.
        bool end(CKContext* context, bool menu_open) {
            const CK_ID esc = esc_block_;
            restore(context);
            if (!menu_open || !esc) return false;
            esc_block_ = esc;
            esc_pending_ = true;
            return true;
        }

        // After a session that ended with the menu open, every frame:
        // holds the ESC block's Key State while the menu stays open; true on
        // the frame it lets go (the menu closed or the level was left).
        bool release(CKContext* context, bool menu_open) {
            if (!esc_pending_) return false;
            if (menu_open && context) {
                hold_esc(context);
                return false;
            }
            esc_pending_ = false;
            esc_block_ = 0;
            return true;
        }

        // Piece 1's block (0 when it was not found): step ESCK's key mask
        // (menu_keys.hpp) leaves it to the hold above.
        CK_ID esc_block() const { return esc_block_; }

        // The automation's read ('pausechain', appended in a core session).
        std::string status(CKContext* context) const {
            const auto pointed = [context](const std::vector<redirect>& list, CK_ID constant) {
                int on = 0;
                for (const auto& r: list) {
                    CKParameterIn* in = input_of(context, r);
                    if (in && constant && in->GetRealSource() && in->GetRealSource()->GetID() == constant) ++on;
                }
                return std::format("{}/{}", on, list.size());
            };
            CKBOOL esc_state = FALSE;
            if (auto* block = context && esc_block_ ? CKBehavior::Cast(context->GetObject(esc_block_)) : nullptr)
                if (CKParameterOut* state = block->GetOutputParameter(0)) state->GetValue(&esc_state, FALSE);
            return std::format("overlay esc={} esc_state={} esc_writes={} openings={} tutorial={} timer={}",
                               esc_block_ ? std::to_string(esc_block_) : std::string("none"), esc_state ? 1 : 0,
                               esc_writes_, openings_, pointed(tutorial_, tutorial_constant_),
                               pointed(timer_, timer_constant_));
        }

    private:
        struct redirect {
            CK_ID block = 0;
            int input = -1;
            CK_ID prior_direct = 0;
            CK_ID prior_shared = 0;
            CK_ID prior_real = 0;
        };

        // Writes the ESC block's Key State TRUE when it reads FALSE.
        void hold_esc(CKContext* context) {
            if (!context || !esc_block_) return;
            auto* block = CKBehavior::Cast(context->GetObject(esc_block_));
            if (!block || !block->IsUsingFunction() || !block->GetPrototypeName()
                    || std::strcmp(block->GetPrototypeName(), "Key Event") != 0)
                return;
            CKParameterOut* state = block->GetOutputParameter(0);
            if (!state || state->GetDataSize() != static_cast<int>(sizeof(CKBOOL))) return;
            CKBOOL value = FALSE;
            state->GetValue(&value, FALSE);
            if (value) return;
            const CKBOOL held = TRUE;
            state->SetValue(&held, sizeof(held));
            ++esc_writes_;
        }

        static CKParameterIn* input_of(CKContext* context, const redirect& r) {
            auto* block = context ? CKBehavior::Cast(context->GetObject(r.block)) : nullptr;
            return block && r.input >= 0 && r.input < block->GetInputParameterCount() ? block->GetInputParameter(r.input)
                                                                                     : nullptr;
        }

        static CKBehavior* find_graph(CKBehavior* behavior, const char* name) {
            const int count = behavior ? behavior->GetSubBehaviorCount() : 0;
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = behavior->GetSubBehavior(i);
                if (!sub) continue;
                if (!sub->IsUsingFunction() && sub->GetName() && std::strcmp(sub->GetName(), name) == 0) return sub;
                if (CKBehavior* found = find_graph(sub, name)) return found;
            }
            return nullptr;
        }

        static void collect(CKBehavior* behavior, const char* prototype, std::vector<CKBehavior*>& out) {
            const int count = behavior ? behavior->GetSubBehaviorCount() : 0;
            for (int i = 0; i < count; ++i) {
                CKBehavior* sub = behavior->GetSubBehavior(i);
                if (!sub) continue;
                if (sub->IsUsingFunction()) {
                    if (sub->GetPrototypeName() && std::strcmp(sub->GetPrototypeName(), prototype) == 0) out.push_back(sub);
                } else {
                    collect(sub, prototype, out);
                }
            }
        }

        static CKBehavior* root_of(CKBehavior* behavior) {
            while (behavior && behavior->GetParent()) behavior = behavior->GetParent();
            return behavior;
        }

        static bool read_int(CKParameterIn* in, int& value) {
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            if (!source || source->GetDataSize() != static_cast<int>(sizeof(int))) return false;
            return source->GetValue(&value, FALSE) == CK_OK;
        }

        // Pressed (output 0) of `key` leads, along links inside its graph and
        // within a few blocks, to a Send Message of "Pause Level".
        bool reaches_pause(CKBehavior* key) const {
            CKBehavior* graph = key ? key->GetParent() : nullptr;
            if (!graph) return false;
            std::vector<CKBehaviorIO*> frontier{key->GetOutput(0)};
            std::vector<CKBehavior*> seen{key};
            for (int depth = 0; depth < 12 && !frontier.empty(); ++depth) {
                std::vector<CKBehaviorIO*> next;
                for (int l = 0; l < graph->GetSubBehaviorLinkCount(); ++l) {
                    CKBehaviorLink* link = graph->GetSubBehaviorLink(l);
                    CKBehaviorIO* from = link ? link->GetInBehaviorIO() : nullptr;
                    CKBehaviorIO* to = link ? link->GetOutBehaviorIO() : nullptr;
                    if (!from || !to) continue;
                    bool hit = false;
                    for (CKBehaviorIO* io: frontier) hit = hit || io == from;
                    if (!hit) continue;
                    auto* target = CKBehavior::Cast(to->GetOwner());
                    if (!target || target == graph) continue;
                    bool known = false;
                    for (CKBehavior* b: seen) known = known || b == target;
                    if (known) continue;
                    seen.push_back(target);
                    if (target->IsUsingFunction() && target->GetPrototypeName()
                            && std::strcmp(target->GetPrototypeName(), "Send Message") == 0) {
                        int type = -1;
                        if (target->GetInputParameterCount() > 0 && read_int(target->GetInputParameter(0), type)
                                && type == pause_type_)
                            return true;
                    }
                    for (int o = 0; o < target->GetOutputCount(); ++o) next.push_back(target->GetOutput(o));
                }
                frontier.swap(next);
            }
            return false;
        }

        static bool output_linked(CKBehavior* block, int output) {
            CKBehavior* graph = block ? block->GetParent() : nullptr;
            CKBehaviorIO* io = block && output < block->GetOutputCount() ? block->GetOutput(output) : nullptr;
            if (!graph || !io) return false;
            for (int l = 0; l < graph->GetSubBehaviorLinkCount(); ++l) {
                CKBehaviorLink* link = graph->GetSubBehaviorLink(l);
                if (link && link->GetInBehaviorIO() == io) return true;
            }
            return false;
        }

        std::string resolve_esc(CKContext* context) {
            CKBehavior* events = bmmo_root(context, "Gameplay_Events");
            if (!events) return "error esc: Gameplay_Events is missing";
            std::vector<CKBehavior*> keys;
            collect(events, "Key Event", keys);
            // The level has more than one Key Event on ESC (the level end's
            // "3 keys" - ESC, Space or Enter - is another): the one is the
            // block whose Pressed leads to the "Pause Level" message.
            std::vector<CKBehavior*> esc;
            int on_esc = 0;
            for (CKBehavior* key: keys) {
                for (int i = 0; i < key->GetInputParameterCount(); ++i) {
                    int code = 0;
                    if (read_int(key->GetInputParameter(i), code) && code == CKKEY_ESCAPE) {
                        ++on_esc;
                        if (reaches_pause(key)) esc.push_back(key);
                        break;
                    }
                }
            }
            if (esc.size() != 1)
                return std::format("error esc: Gameplay_Events has {} Key Event(s) on ESC leading to \"Pause Level\" "
                                   "(of {} on ESC, {} in all), not one", esc.size(), on_esc, keys.size());
            CKBehavior* key = esc.front();
            if (key->GetInputParameterCount() != 1)
                return std::format("error esc: the ESC Key Event #{} waits for {} keys", key->GetID(),
                                   key->GetInputParameterCount());
            if (output_linked(key, 1))
                return std::format("error esc: the ESC Key Event #{}'s Released is linked", key->GetID());
            CKParameterOut* state = key->GetOutputParameter(0);
            if (!state || state->GetDataSize() != static_cast<int>(sizeof(CKBOOL)))
                return std::format("error esc: the ESC Key Event #{} has no Key State output", key->GetID());
            if (state->GetDestinationCount() != 0)
                return std::format("error esc: the ESC Key Event #{}'s Key State feeds {} parameter(s)", key->GetID(),
                                   state->GetDestinationCount());
            esc_block_ = key->GetID();
            return std::format("ok esc: Gameplay_Events/{} Key Event #{} (Pressed -> \"Pause Level\"): Key State held "
                               "while the menu is open",
                               key->GetParent() && key->GetParent()->GetName() ? key->GetParent()->GetName() : "?",
                               key->GetID());
        }

        // Re-points one input at `constant`, recording what it read.
        static bool point(CKBehavior* block, int input, CKParameter* constant, std::vector<redirect>& out) {
            CKParameterIn* in = block->GetInputParameter(input);
            if (!in || !constant) return false;
            redirect r;
            r.block = block->GetID();
            r.input = input;
            CKParameter* real = in->GetRealSource();
            CKParameterIn* shared = in->GetSharedSource();
            CKParameter* direct = in->GetDirectSource();
            r.prior_real = real ? real->GetID() : 0;
            r.prior_shared = shared ? shared->GetID() : 0;
            r.prior_direct = direct ? direct->GetID() : 0;
            if (in->SetDirectSource(constant) != CK_OK) return false;
            out.push_back(r);
            return true;
        }

        static void keep(CKContext* context, std::vector<redirect>& list, CK_ID constant_id) {
            auto* constant = constant_id ? CKParameter::Cast(context->GetObject(constant_id)) : nullptr;
            if (!constant) return;
            for (const auto& r: list) {
                CKParameterIn* in = input_of(context, r);
                CKParameter* current = in ? in->GetRealSource() : nullptr;
                if (!current || current == constant || current->GetID() != r.prior_real) continue;
                in->SetDirectSource(constant);
            }
        }

        static void hand_back(CKContext* context, std::vector<redirect>& list, CK_ID constant_id) {
            auto* constant = constant_id ? CKParameter::Cast(context->GetObject(constant_id)) : nullptr;
            bool referenced = false;
            for (const auto& r: list) {
                CKParameterIn* in = input_of(context, r);
                if (!in || !constant || in->GetRealSource() != constant) continue;
                bool back = false;
                if (r.prior_shared) {
                    auto* other = CKParameterIn::Cast(context->GetObject(r.prior_shared));
                    back = other && in->ShareSourceWith(other) == CK_OK;
                } else if (auto* source = r.prior_direct ? CKParameter::Cast(context->GetObject(r.prior_direct)) : nullptr) {
                    back = in->SetDirectSource(source) == CK_OK;
                }
                if (!back) referenced = true;
            }
            if (constant && !referenced) context->DestroyObject(constant);
        }

        std::string resolve_tutorial(CKContext* context) {
            CKBehavior* handler = bmmo_root(context, "Event_handler");
            if (!handler) return "error tutorial: Event_handler is missing";
            struct found {
                CKBehavior* block;
                int input;
                const char* chain;
            };
            std::vector<found> targets;
            for (const char* chain_name: {"Pause Level", "Unpause Level"}) {
                CKBehavior* chain = find_graph(handler, chain_name);
                if (!chain) return std::format("error tutorial: Event_handler has no '{}'", chain_name);
                std::vector<CKBehavior*> blocks;
                collect(chain, "Deactivate Script", blocks);
                collect(chain, "Activate Script", blocks);
                for (CKBehavior* block: blocks)
                    for (int i = 0; i < block->GetInputParameterCount(); ++i) {
                        CKParameterIn* in = block->GetInputParameter(i);
                        if (!in || in->GetGUID() != CKPGUID_SCRIPT) continue;
                        auto* target = CKBehavior::Cast(block->GetInputParameterObject(i));
                        if (target && target->GetName() && std::strcmp(target->GetName(), "Gameplay_Tutorial") == 0)
                            targets.push_back({block, i, chain_name});
                    }
            }
            if (targets.empty()) return "error tutorial: no pause chain block targets Gameplay_Tutorial";
            auto* constant = context->CreateCKParameterLocal(const_cast<CKSTRING>(kNoScriptName), CKPGUID_SCRIPT, TRUE);
            if (!constant || constant->GetDataSize() != static_cast<int>(sizeof(CK_ID))) {
                if (constant) context->DestroyObject(constant);
                return "error tutorial: could not create the session's empty Script";
            }
            const CK_ID none = 0;
            constant->SetValue(&none, sizeof(none));
            tutorial_constant_ = constant->GetID();
            std::string list;
            for (const auto& t: targets) {
                if (!point(t.block, t.input, constant, tutorial_)) {
                    std::string error = std::format("error tutorial: {}/{} #{} input {} refused the session's empty Script",
                                                    t.chain, t.block->GetPrototypeName(), t.block->GetID(), t.input);
                    hand_back(context, tutorial_, tutorial_constant_);
                    tutorial_.clear();
                    tutorial_constant_ = 0;
                    return error;
                }
                list += std::format("{}{}/{} #{}", list.empty() ? "" : ", ", t.chain, t.block->GetPrototypeName(),
                                    t.block->GetID());
            }
            return "ok tutorial: re-pointed at no script (the tutorial keeps running): " + list;
        }

        // Every Switch On Message of a script on All_Gameplay (the gameplay
        // scripts' owner) that waits for the pause messages: Gameplay_Energy's
        // points timer and, on a level with a thunderstorm, Gameplay_Blitz's
        // lightning (a Binary Memory that stops new flashes).
        std::string resolve_timer(CKContext* context, CKMessageManager* messages) {
            struct found {
                CKBehavior* block;
                std::vector<int> inputs;
            };
            std::vector<found> listeners;
            bool energy = false;
            const int behaviors = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int b = 0; b < behaviors; ++b) {
                auto* block = CKBehavior::Cast(context->GetObject(ids[b]));
                if (!block || !block->IsUsingFunction() || !block->GetPrototypeName()
                        || std::strcmp(block->GetPrototypeName(), "Switch On Message") != 0)
                    continue;
                CKBehavior* root = root_of(block);
                CKBeObject* owner = root ? root->GetOwner() : nullptr;
                if (!owner || !owner->GetName() || std::strcmp(owner->GetName(), "All_Gameplay") != 0) continue;
                found f{block, {}};
                for (int i = 0; i < block->GetInputParameterCount(); ++i) {
                    int type = -1;
                    if (read_int(block->GetInputParameter(i), type) && (type == pause_type_ || type == unpause_type_))
                        f.inputs.push_back(i);
                }
                if (f.inputs.empty()) continue;
                if (root->GetName() && std::strcmp(root->GetName(), "Gameplay_Energy") == 0) energy = true;
                listeners.push_back(f);
            }
            if (!energy) return "error timer: no Switch On Message of Gameplay_Energy waits for the pause messages";
            const int never = messages->AddMessageType(const_cast<CKSTRING>(kNoMessageType));
            if (never == pause_type_ || never == unpause_type_) return "error timer: no free message type";
            auto* constant = context->CreateCKParameterLocal(const_cast<CKSTRING>(kNoMessageName), CKPGUID_MESSAGE, TRUE);
            if (!constant || constant->GetDataSize() != static_cast<int>(sizeof(int))) {
                if (constant) context->DestroyObject(constant);
                return "error timer: could not create the session's message constant";
            }
            constant->SetValue(&never, sizeof(never));
            timer_constant_ = constant->GetID();
            std::string list;
            for (const auto& f: listeners) {
                std::string inputs;
                for (int i: f.inputs) {
                    if (!point(f.block, i, constant, timer_)) {
                        hand_back(context, timer_, timer_constant_);
                        timer_.clear();
                        timer_constant_ = 0;
                        return std::format("error timer: Switch On Message #{} input {} refused the session's constant",
                                           f.block->GetID(), i);
                    }
                    inputs += std::format("{}{}", inputs.empty() ? "" : ",", i);
                }
                CKBehavior* root = root_of(f.block);
                list += std::format("{}{}/Switch On Message #{} inputs {}", list.empty() ? "" : ", ",
                                    root && root->GetName() ? root->GetName() : "?", f.block->GetID(), inputs);
            }
            return std::format("ok timer: re-pointed at message type {} (never sent), the menu neither pauses nor "
                               "resumes them: {}", never, list);
        }

        // Who else hears the pause messages (logged once per anchor, read
        // only): every Wait Message / Switch On Message input on them.
        std::string census(CKContext* context) const {
            std::string out;
            int count = 0;
            const int behaviors = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int b = 0; b < behaviors; ++b) {
                auto* block = CKBehavior::Cast(context->GetObject(ids[b]));
                const char* prototype = block && block->IsUsingFunction() ? block->GetPrototypeName() : nullptr;
                if (!prototype
                        || (std::strcmp(prototype, "Wait Message") != 0 && std::strcmp(prototype, "Switch On Message") != 0))
                    continue;
                for (int i = 0; i < block->GetInputParameterCount(); ++i) {
                    CKParameterIn* in = block->GetInputParameter(i);
                    if (!in || in->GetGUID() != CKPGUID_MESSAGE) continue;
                    int type = -1;
                    CKParameter* source = in->GetRealSource();
                    if (!source || source->GetDataSize() != static_cast<int>(sizeof(int)) || source->GetValue(&type, FALSE) != CK_OK)
                        continue;
                    if (type != pause_type_ && type != unpause_type_) continue;
                    CKBehavior* root = root_of(block);
                    CKBeObject* owner = block->GetOwner();
                    out += std::format("{}{}@{}/{} #{} in{}={}", out.empty() ? "" : "; ",
                                       root && root->GetName() ? root->GetName() : "?",
                                       owner && owner->GetName() ? owner->GetName() : "?", prototype, block->GetID(), i,
                                       type == pause_type_ ? "Pause" : "Unpause");
                    ++count;
                }
            }
            return std::format("{} input(s) on Pause/Unpause Level: {}", count, out.empty() ? "-" : out);
        }

        static CKBehavior* bmmo_root(CKContext* context, const char* name) {
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int i = 0; i < count; ++i) {
                auto* behavior = CKBehavior::Cast(context->GetObject(ids[i]));
                if (!behavior || behavior->GetParent() || !behavior->GetName()) continue;
                if (std::strcmp(behavior->GetName(), name) == 0) return behavior;
            }
            return nullptr;
        }

        CK_ID esc_block_ = 0;
        std::vector<redirect> tutorial_;
        std::vector<redirect> timer_;
        CK_ID tutorial_constant_ = 0;
        CK_ID timer_constant_ = 0;
        int pause_type_ = -1;
        int unpause_type_ = -1;
        bool was_open_ = false;
        bool esc_pending_ = false;
        unsigned long long esc_writes_ = 0;
        unsigned long long openings_ = 0;
    };
}
