#pragma once

// Step INT: the level intents' CK side, shared by the retail mod and the
// headless client (docs/realtime-design.md 'Built (player intents in the
// journal, step INT, 2026-11-15)').
//
//   send_reset_level / send_exit_level: the message the game's own menus
//     send ("Reset Level" / "Exit Level" to Level and All_Balls, as
//     Menu_Pause does), for BMMO's triggers (retail's restart, the
//     automation verbs, the headless test switches; recipe v4's re-drive);
//   level_intent_watch: after a live frame, whether that frame delivered
//     "Reset Level" to Level (whoever sent it: a menu, Menu_Dead, BMMO) and
//     whether the root script Menu_Dead is active (the last life lost).  The
//     core turns these into intent records (session_client_core
//     poll_level_intents).
//
// Only the public CK2 API: the header compiles against the Virtools SDK
// (client mod) and the Ballanced engine (headless).  The watch keeps the two
// objects' ids and checks each by name and class on every poll, finding it
// again when the check fails (a level load recycles ids): no full lookup on
// a normal frame.

#include <cstring>

#include "CKAll.h"

#include "menu_driver.hpp"

namespace bmmo::game {
    namespace detail {
        inline CKBeObject* level_object(CKContext* context, const char* name) {
            return static_cast<CKBeObject*>(
                context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name), CKCID_BEOBJECT, nullptr));
        }

        // Sends `message` to Level and All_Balls; how many took it.
        inline int send_level_message(CKContext* context, const char* message) {
            CKMessageManager* messages = context ? context->GetMessageManager() : nullptr;
            if (!messages) return 0;
            const CKMessageType type = messages->AddMessageType(const_cast<CKSTRING>(message));
            int sent = 0;
            for (const char* target: {"Level", "All_Balls"})
                if (CKBeObject* object = level_object(context, target); object && messages->SendMessageSingle(type, object))
                    ++sent;
            return sent;
        }
    }

    // Both Level and All_Balls exist (a send reaches both).
    inline bool level_objects_present(CKContext* context) {
        return context && detail::level_object(context, "Level") && detail::level_object(context, "All_Balls");
    }

    // "Reset Level" to Level and All_Balls: the pause menu's Restart Level
    // and a lost last life's restart.  The objects that took it (of 2).
    inline int send_reset_level(CKContext* context) { return detail::send_level_message(context, "Reset Level"); }

    // "Exit Level" to Level and All_Balls: the pause menu's Exit Level.
    inline int send_exit_level(CKContext* context) { return detail::send_level_message(context, "Exit Level"); }

    struct level_intents_seen {
        bool reset = false;       // the frame delivered "Reset Level" to Level
        bool menu_dead = false;   // Menu_Dead is active after it
    };

    class level_intent_watch {
    public:
        // After a live frame (behaviours and managers run).
        level_intents_seen poll(CKContext* context) {
            level_intents_seen out;
            if (!context) return out;
            if (CKBeObject* level = level_(context)) {
                CKMessageManager* messages = context->GetMessageManager();
                for (int i = 0; messages && i < level->GetLastFrameMessageCount(); ++i) {
                    CKMessage* message = level->GetLastFrameMessage(i);
                    const char* name = message ? messages->GetMessageTypeName(message->GetMsgType()) : nullptr;
                    if (name && std::strcmp(name, "Reset Level") == 0) { out.reset = true; break; }
                }
            }
            CKBehavior* dead = menu_dead_(context);
            out.menu_dead = dead && dead->IsActive();
            return out;
        }

    private:
        CKBeObject* level_(CKContext* context) {
            auto* object = level_id_ ? CKBeObject::Cast(context->GetObject(level_id_)) : nullptr;
            if (!object || !object->GetName() || std::strcmp(object->GetName(), "Level") != 0) {
                object = detail::level_object(context, "Level");
                level_id_ = object ? object->GetID() : 0;
            }
            return object;
        }
        CKBehavior* menu_dead_(CKContext* context) {
            auto* script = menu_dead_id_ ? as_behavior(context->GetObject(menu_dead_id_)) : nullptr;
            if (!script || script->GetParent() || !script->GetName() || std::strcmp(script->GetName(), "Menu_Dead") != 0) {
                script = find_root_script(context, "Menu_Dead");
                menu_dead_id_ = script ? script->GetID() : 0;
            }
            return script;
        }

        CK_ID level_id_ = 0;
        CK_ID menu_dead_id_ = 0;
    };
}
