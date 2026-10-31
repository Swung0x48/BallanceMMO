#pragma once

// FULL step ESCK (the user, 2026-10-31: while the overlay menu is open a key
// operates the menu and nothing else).  ESCX (menu_overlay.hpp) keeps the
// level's scripts running while the ESC menu is open in a core session, so a
// key pressed in the menu also reached them as with no menu: Enter on the
// menu's Back reached a tutorial text waiting for RETURN (Level 1) - and
// released the own ball when that text was the one that does.  This mask
// makes every keyboard read of the level's scripts see no key while the menu
// is open; the menu's own scripts (Menu_Pause and its sub-menus, all owned by
// All_Menu) read the keyboard unchanged.  Core session only, retail-only,
// host-local (no wire or record change: the own ball's input is the core's
// mute, unchanged, and the own replica reads the keyboard buffer directly,
// not through these blocks); the legacy path and offline play never see it.
//
// How.  The level's keyboard readers are building blocks (a census of every
// root script on Levels 1-13: Key Event and one Key Waiter, in Gameplay_Tutorial,
// Gameplay_Ingame, Gameplay_Events, Gameplay_Energy and Debug_Info; no module
// script reads a key).  Each such block outside the menus gets its per-block
// function (CKBehavior::SetFunction; the precedent is physics_state.cpp's
// Random-block patch) replaced by `wrapper`, which runs the block's own
// function with the block's keys reading up in the input manager's keyboard
// buffer (the bytes IsKeyDown reads), and puts the bytes back right after: the
// block takes its own no-key path, nothing is re-implemented.  A Key Waiter in
// its any-key mode (ReadKey: the key buffer, not the state bytes) is not run
// while the menu is open or a key is latched - its no-key path only waits.
//
// Keys held when the menu opens: the blocks see them released at the open (a
// Key Event whose Key State was TRUE fires Released once, as for a real
// release).  Keys held when the menu closes stay hidden from these blocks until
// they are let go (a per-key latch): the key that closes the menu - Enter on
// Back, ESC - is still down for a few frames after the close, and without the
// latch its first unmasked frame would be a fresh press into the level (the
// tutorial's RETURN, the level end's "3 keys").  A press after the release is
// seen as usual.  A key held across the whole menu period therefore needs a
// new press after the close; the own ball is not affected (it reads the buffer
// itself and takes keys again from the mute's end, as ESCX built it).
//
// Exempt: ESCX's ESC block of Gameplay_Events (its Key State hold is what keeps
// the closing ESC from re-pausing; ESCX's hold rules unchanged).  Every wrapped
// block is checked every frame (a level reset may give a block its prototype's
// function back) and handed back at the session's end - once the menu has
// closed and the latch is empty when the session ends with them (end() /
// release()).  File-scope state of core_host.cpp: BallanceMMOClient keeps its
// layout.

#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "CKAll.h"

namespace bmmo::retail {
    class menu_key_mask {
    public:
        using open_test = std::function<bool()>;

        ~menu_key_mask() {
            if (active_ == this) active_ = nullptr;
        }

        // The core session's anchor: wraps every keyboard-reading block whose
        // root script is not a menu's (owner All_Menu), `exempt` (ESCX's ESC
        // block) left out.  Returns the lines to log, "ok " / "error ".
        std::vector<std::string> resolve(CKContext* context, open_test menu_open, CK_ID exempt) {
            restore(context);
            roots_.clear();
            closes_ = latch_total_ = rewraps_ = 0;
            if (!context) return {"error keys: no context"};
            menu_open_ = std::move(menu_open);
            std::vector<std::string> lines;
            std::map<std::string, std::string> wrapped_by_root, menus, unmasked;
            int menu_blocks = 0, exempted = 0;
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            for (int b = 0; b < count; ++b) {
                auto* block = CKBehavior::Cast(context->GetObject(ids[b]));
                if (!block || !block->IsUsingFunction()) continue;
                const char* prototype = block->GetPrototypeName();
                if (!prototype) continue;
                const kind k = kind_of(block, prototype);
                const bool reader = k != kind::none || is_other_reader(prototype);
                if (!reader) continue;
                CKBehavior* root = root_of(block);
                CKBeObject* owner = root ? root->GetOwner() : nullptr;
                const std::string root_name = root && root->GetName() ? root->GetName() : "?";
                if (owner && owner->GetName() && std::strcmp(owner->GetName(), kMenuOwner) == 0) {
                    ++menu_blocks;
                    add(menus, root_name, prototype);
                    continue;
                }
                if (block->GetID() == exempt) {
                    ++exempted;
                    continue;
                }
                if (k == kind::none) {
                    add(unmasked, root_name, prototype);
                    continue;
                }
                CKBEHAVIORFCT original = block->GetFunction();
                if (original == &wrapper) original = prototype_function(block);
                if (!original) {
                    add(unmasked, root_name, std::string(prototype) + " (no function)");
                    continue;
                }
                const int root_index = root_slot(root_name);
                wrapped_.push_back({block->GetID(), original, k, root_index});
                index_[block->GetID()] = wrapped_.size() - 1;
                block->SetFunction(&wrapper);
                ++roots_[root_index].blocks;
                add(wrapped_by_root, root_name,
                    k == kind::any_key ? std::string(prototype) + " (any key)" : std::string(prototype));
            }
            active_ = this;
            lines.push_back(std::format("ok keys: {} block(s) of the level's scripts read no key while the menu is open: {}",
                                        wrapped_.size(), join(wrapped_by_root)));
            lines.push_back(std::format("ok keys: left to the menus: {} block(s) ({}); exempt: {} (ESCX's ESC block #{})",
                                        menu_blocks, join(menus), exempted, exempt));
            if (!unmasked.empty())
                lines.push_back("error keys: keyboard readers outside the menus left unmasked: " + join(unmasked));
            return lines;
        }

        // Every frame of a core session: the open/close transition and the
        // latch kept, every wrapped block still wrapped.
        void apply(CKContext* context) {
            if (!context) return;
            keep(context);
            const unsigned char* state = keyboard(context);
            observe(menu_open_ && menu_open_(), state);
            prune(state);
        }

        // Hands every wrapped block its own function back.
        void restore(CKContext* context) {
            if (context) {
                for (const auto& w: wrapped_) {
                    auto* block = CKBehavior::Cast(context->GetObject(w.block));
                    if (block && block->GetFunction() == &wrapper) block->SetFunction(w.original);
                }
            }
            wrapped_.clear();
            index_.clear();
            latched_.fill(false);
            latched_count_ = 0;
            was_open_ = false;
            pending_ = false;
            if (active_ == this) active_ = nullptr;
        }

        // The session's end (core_host_end_local): handed back at once unless
        // the menu is still open or a key is still latched; then the mask
        // stays until both are over (release()).  True when it stays.
        bool end(CKContext* context) {
            if (context) {
                const unsigned char* state = keyboard(context);
                observe(menu_open_ && menu_open_(), state);
                prune(state);
            }
            if (!wrapped_.empty() && ((menu_open_ && menu_open_()) || latched_count_ > 0)) {
                pending_ = true;
                return true;
            }
            restore(context);
            return false;
        }

        // Outside a core session, every frame: after a session that ended
        // with the menu open or a key latched, true on the frame it lets go.
        bool release(CKContext* context) {
            if (!pending_) return false;
            apply(context);
            if ((menu_open_ && menu_open_()) || latched_count_ > 0) return false;
            restore(context);
            return true;
        }

        // The automation's 'pausechain' (core session) and the end-of-session
        // log: per root script, wrapped blocks / frames a block had one of
        // its keys down and read it up / any-key waits skipped.
        std::string status() const {
            std::string per_root;
            for (const auto& r: roots_) {
                if (r.blocks == 0) continue;
                per_root += std::format("{}{}={}/{}/{}", per_root.empty() ? "" : ",", r.name, r.blocks, r.hidden, r.skipped);
            }
            return std::format("keys wrapped={} open={} closes={} latched={} latch_total={} rewraps={} hidden=[{}]",
                               wrapped_.size(), was_open_ ? 1 : 0, closes_, latched_count_, latch_total_, rewraps_,
                               per_root.empty() ? "-" : per_root);
        }

    private:
        static constexpr const char* kMenuOwner = "All_Menu";

        enum class kind { none, key_event, key_waiter, any_key };

        struct wrapped_block {
            CK_ID block = 0;
            CKBEHAVIORFCT original = nullptr;
            kind type = kind::none;
            int root = 0;
        };

        struct root_counts {
            std::string name;
            int blocks = 0;
            unsigned long long hidden = 0;
            unsigned long long skipped = 0;
        };

        static kind kind_of(CKBehavior* block, const char* prototype) {
            if (std::strcmp(prototype, "Key Event") == 0) return kind::key_event;
            if (std::strcmp(prototype, "Key Waiter") == 0)
                return block->GetInputParameterCount() > 0 ? kind::key_waiter : kind::any_key;
            return kind::none;
        }

        // Other prototypes that read the keyboard (the census found them in
        // the menus only); outside the menus they are reported, not wrapped.
        static bool is_other_reader(const char* prototype) {
            static const char* const kOthers[] = {"TT_Key Waiter", "Keyboard", "Switch On Key", "Input String",
                                                  "TT InputString", "Keyboard Mapper", "Keyboard Controller"};
            for (const char* other: kOthers)
                if (std::strcmp(prototype, other) == 0) return true;
            return false;
        }

        // A key input's value (ESCX's read_int): only a 4-byte parameter is
        // read, so an input of another type (a custom map's or a mod's block)
        // can never write past `key`; a code outside 1..255 is no key.
        static bool read_key(CKParameterIn* in, int& key) {
            CKParameter* source = in ? in->GetRealSource() : nullptr;
            if (!source || source->GetDataSize() != static_cast<int>(sizeof(int))) return false;
            if (source->GetValue(&key, FALSE) != CK_OK) return false;
            return key > 0 && key < 256;
        }

        static CKBEHAVIORFCT prototype_function(CKBehavior* block) {
            CKBehaviorPrototype* proto = block ? block->GetPrototype() : nullptr;
            CKBEHAVIORFCT f = proto ? proto->GetFunction() : nullptr;
            return f == &wrapper ? nullptr : f;
        }

        static CKBehavior* root_of(CKBehavior* behavior) {
            while (behavior && behavior->GetParent()) behavior = behavior->GetParent();
            return behavior;
        }

        static void add(std::map<std::string, std::string>& out, const std::string& root, const std::string& prototype) {
            std::string& list = out[root];
            list += list.empty() ? prototype : "+" + prototype;
        }

        static std::string join(const std::map<std::string, std::string>& by_root) {
            if (by_root.empty()) return "-";
            std::string out;
            for (const auto& [root, list]: by_root) {
                // "Key Event+Key Event+..." condensed to counts
                std::map<std::string, int> counts;
                size_t start = 0;
                while (start <= list.size()) {
                    const size_t plus = list.find('+', start);
                    const std::string item = list.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
                    if (!item.empty()) ++counts[item];
                    if (plus == std::string::npos) break;
                    start = plus + 1;
                }
                std::string items;
                for (const auto& [item, n]: counts) items += std::format("{}{} x{}", items.empty() ? "" : ", ", item, n);
                out += std::format("{}{} ({})", out.empty() ? "" : "; ", root, items);
            }
            return out;
        }

        int root_slot(const std::string& name) {
            for (size_t i = 0; i < roots_.size(); ++i)
                if (roots_[i].name == name) return static_cast<int>(i);
            roots_.push_back({name});
            return static_cast<int>(roots_.size() - 1);
        }

        static unsigned char* keyboard(CKContext* context) {
            auto* input = context ? static_cast<CKInputManager*>(context->GetManagerByGuid(INPUT_MANAGER_GUID)) : nullptr;
            return input ? input->GetKeyboardState() : nullptr;
        }

        // The open -> closed transition latches every key then down (any
        // state bit: down, or released in this very frame).
        void observe(bool open, const unsigned char* state) {
            if (open) {
                was_open_ = true;
                return;
            }
            if (!was_open_) return;
            was_open_ = false;
            ++closes_;
            if (!state) return;
            for (int k = 0; k < 256; ++k)
                if (state[k] != KS_IDLE && !latched_[k]) {
                    latched_[k] = true;
                    ++latched_count_;
                    ++latch_total_;
                }
        }

        // A latched key is let go once it reads idle (fully up).
        void prune(const unsigned char* state) {
            if (!state || latched_count_ == 0) return;
            for (int k = 0; k < 256; ++k)
                if (latched_[k] && state[k] == KS_IDLE) {
                    latched_[k] = false;
                    --latched_count_;
                }
        }

        void keep(CKContext* context) {
            for (auto& w: wrapped_) {
                auto* block = CKBehavior::Cast(context->GetObject(w.block));
                if (!block || block->GetFunction() == &wrapper) continue;
                CKBEHAVIORFCT current = block->GetFunction();
                if (current) w.original = current;
                block->SetFunction(&wrapper);
                ++rewraps_;
            }
        }

        // Runs one wrapped block.
        int run(const wrapped_block& w, const CKBehaviorContext& context) {
            CKBehavior* block = context.Behavior;
            unsigned char* state = keyboard(context.Context);
            const bool open = menu_open_ && menu_open_();
            observe(open, state);
            prune(state);
            if (!state || (!open && latched_count_ == 0)) return w.original(context);
            root_counts& counts = roots_[w.root];
            if (w.type == kind::any_key) {
                // ReadKey reads the key buffer, which cannot be masked per key:
                // while the menu is open, and after the close while a key is
                // still latched (a close in the middle of a frame leaves the
                // closing press in this frame's buffer), the waiter does not
                // run - its own no-key path only waits.
                ++counts.skipped;
                return CKBR_ACTIVATENEXTFRAME;
            }
            int keys[8];
            unsigned char saved[8];
            int hidden = 0;
            const int inputs = block->GetInputParameterCount();
            for (int i = 0; i < inputs && hidden < 8; ++i) {
                int key = 0;
                if (!read_key(block->GetInputParameter(i), key)) continue;
                if (state[key] == KS_IDLE || !(open || latched_[key])) continue;
                bool known = false;
                for (int h = 0; h < hidden; ++h) known = known || keys[h] == key;
                if (known) continue;
                keys[hidden] = key;
                saved[hidden] = state[key];
                state[key] = KS_IDLE;
                ++hidden;
            }
            if (hidden == 0) return w.original(context);
            ++counts.hidden;
            const int result = w.original(context);
            for (int h = 0; h < hidden; ++h) state[keys[h]] = saved[h];
            return result;
        }

        static int wrapper(const CKBehaviorContext& context) {
            CKBehavior* block = context.Behavior;
            if (!block) return CKBR_OK;
            menu_key_mask* self = active_;
            if (self) {
                const auto it = self->index_.find(block->GetID());
                if (it != self->index_.end()) return self->run(self->wrapped_[it->second], context);
            }
            // Not (or no longer) one of ours: the block's prototype function.
            CKBEHAVIORFCT f = prototype_function(block);
            return f ? f(context) : CKBR_OK;
        }

        inline static menu_key_mask* active_ = nullptr;

        open_test menu_open_;
        std::vector<wrapped_block> wrapped_;
        std::unordered_map<CK_ID, size_t> index_;
        std::vector<root_counts> roots_;
        std::array<bool, 256> latched_{};
        int latched_count_ = 0;
        bool was_open_ = false;
        bool pending_ = false;
        unsigned long long closes_ = 0;
        unsigned long long latch_total_ = 0;
        unsigned long long rewraps_ = 0;
    };
}
