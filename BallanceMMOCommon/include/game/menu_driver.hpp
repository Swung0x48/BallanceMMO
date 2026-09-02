#pragma once

// Drives the retail menus the way a player does: by activating the very
// building blocks the menu buttons trigger (the technique BallanceTAS uses
// to leave the level menu).  Retail graphs, from base.cmo:
//
//   Menu_Main:  Main Menu."Button 1 pressed" -> Start -> Exit
//               (Start activates Menu_Start; Exit hides and deactivates Menu_Main)
//   Menu_Start: Start Menu."Button N pressed" -> Test -> Op -> Set Cell(CurrentLevel[0,0])
//               -> Send Message "Load Level" -> Hide -> Send Message "Menu_Load" -> Exit
//               (Exit hides every menu entity and deactivates Menu_Start)
//
// A level request enters Menu_Start at Op with Op.p1 = level - 1, skipping
// only the unlock test; everything after it is the retail path, so the
// menus end up hidden and inactive exactly as after a mouse click.
//
// Only the public CK2 API is used: the header compiles against the Virtools
// SDK (client mod) and against the Ballanced engine (headless server).

#include <cstring>
#include <string>

#include "CKAll.h"

namespace bmmo::game {
    inline CKBehavior* as_behavior(CKObject* object) {
        return object && object->GetClassID() == CKCID_BEHAVIOR ? static_cast<CKBehavior*>(object) : nullptr;
    }

    // Root script (no parent) with this exact name, or null.
    inline CKBehavior* find_root_script(CKContext* context, const char* name) {
        if (!context || !name) return nullptr;
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int i = 0; i < count; ++i) {
            CKBehavior* behavior = as_behavior(context->GetObject(ids[i]));
            if (!behavior || behavior->GetParent() || !behavior->GetName()) continue;
            if (std::strcmp(behavior->GetName(), name) == 0) return behavior;
        }
        return nullptr;
    }

    inline bool script_active(CKContext* context, const char* name) {
        CKBehavior* script = find_root_script(context, name);
        return script && script->IsActive();
    }

    // n-th direct sub-behaviour of `parent` with this name, or null.
    inline CKBehavior* find_sub_behavior(CKBehavior* parent, const char* name, int occurrence = 0) {
        if (!parent || !name) return nullptr;
        const int count = parent->GetSubBehaviorCount();
        for (int i = 0; i < count; ++i) {
            CKBehavior* sub = parent->GetSubBehavior(i);
            if (!sub || !sub->GetName() || std::strcmp(sub->GetName(), name) != 0) continue;
            if (occurrence-- == 0) return sub;
        }
        return nullptr;
    }

    // Activates input 0 of a block inside an active script; the engine runs
    // the block and follows its links during the next Process().
    inline void press_block(CKBehavior* block) {
        block->ActivateInput(0, TRUE);
        block->Activate(TRUE, FALSE);
    }

    // One level request, advanced by calling step() once per behaviour
    // frame before the engine processes it (the client's OnProcess, the
    // headless tick()).  From the main menu it takes three frames: press
    // Start, let Menu_Start initialise, press the level.
    struct level_request {
        enum class status { idle, waiting_for_menu, start_pressed, level_selected, failed };

        static constexpr int kMenuStartSettleFrames = 2;
        static constexpr int kTimeoutFrames = 600;

        int level = 0;
        int frames = 0;
        int menu_start_frames = 0;
        bool start_pressed = false;
        bool selected = false;
        std::string error;

        void begin(int requested_level) {
            *this = level_request{};
            level = requested_level;
        }
        bool pending() const { return level > 0 && !selected && error.empty(); }

        status step(CKContext* context) {
            if (level <= 0) return status::idle;
            if (selected) return status::level_selected;
            if (!error.empty()) return status::failed;
            ++frames;
            if (CKBehavior* menu_start = find_root_script(context, "Menu_Start");
                    menu_start && menu_start->IsActive()) {
                if (++menu_start_frames < kMenuStartSettleFrames) return status::start_pressed;
                CKBehavior* op = find_sub_behavior(menu_start, "Op");
                CKParameterIn* p1 = op && op->GetInputParameterCount() >= 2 ? op->GetInputParameter(0) : nullptr;
                CKParameter* index = p1 ? p1->GetRealSource() : nullptr;
                if (!index) {
                    error = "Menu_Start: the Op block feeding CurrentLevel was not found";
                    return status::failed;
                }
                int value = level - 1;
                index->SetValue(&value, sizeof(value));
                press_block(op);
                selected = true;
                return status::level_selected;
            }
            if (CKBehavior* menu_main = find_root_script(context, "Menu_Main");
                    menu_main && menu_main->IsActive() && !start_pressed) {
                CKBehavior* start = find_sub_behavior(menu_main, "Start");
                if (!start) {
                    error = "Menu_Main: the Start block was not found";
                    return status::failed;
                }
                press_block(start);
                start_pressed = true;
                return status::start_pressed;
            }
            if (frames > kTimeoutFrames) {
                error = start_pressed ? "Menu_Start never became active after pressing Start"
                                      : "neither Menu_Main nor Menu_Start is active";
                return status::failed;
            }
            return start_pressed ? status::start_pressed : status::waiting_for_menu;
        }
    };
}
