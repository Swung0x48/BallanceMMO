#pragma once

// Step INT: a player's intents - a level reset asked for, the level left, the
// pause menu opened and closed, the last life lost - with the session tick
// each applies at and where it came from.  The client journals them (stat
// kind `intent`) and sends each to the server (SessionIntent, protocol 41),
// which journals what it received (`member_intent`), so a recipe for a
// public session can be built from the server's journal alone.  No world
// reads an intent: a reset's world-visible effects are the owner's own
// lifecycle events, which travel as before (docs/realtime-design.md 'Built
// (player intents in the journal, step INT, 2026-11-15)').

#include <cstddef>
#include <cstdint>

namespace bmmo::session {
    // The ids are the tables' order and never change meaning: a new one goes
    // at the end, before `count`.
    enum class intent_kind : uint8_t { reset_level = 0, exit_level, pause_enter, pause_leave, game_over, count };

    enum class intent_source : uint8_t {
        unknown = 0,
        pause_menu,        // the pause menu's Restart Level / Exit Level (the frame before was paused)
        dead_menu,         // Menu_Dead's restart / exit after the last life was lost
        finish_menu,       // the finish screen's exit (the own Finish was sent)
        restart_request,   // a RestartRequest from another player (arg: the requester's connection)
        countdown,         // a countdown's Go with restart_level (arg: the sender's connection)
        automation,        // an automation verb (restart, exitlevel)
        test_flag,         // a headless test switch (--reset-level-at, --exit-level-at)
        recipe,            // a re-drive recipe's intent (recipe v4)
        esc_key,           // a pause the ESC key opened
        restart_front,     // a pause BMMO's own restart opened (restart_current_level's ESC activation)
        debug_key,         // the debug build's key 5
        count
    };

    // No request tick / no argument.
    constexpr uint32_t INTENT_NONE = 0xFFFFFFFFu;

    constexpr const char* kIntentKindNames[] = {"reset_level", "exit_level", "pause_enter", "pause_leave", "game_over"};
    constexpr const char* kIntentSourceNames[] = {"unknown",    "pause_menu", "dead_menu",  "finish_menu",
                                                  "restart_request", "countdown", "automation", "test_flag",
                                                  "recipe",     "esc_key",    "restart_front", "debug_key"};
    static_assert(sizeof(kIntentKindNames) / sizeof(kIntentKindNames[0]) == static_cast<size_t>(intent_kind::count),
                  "one name per intent_kind");
    static_assert(sizeof(kIntentSourceNames) / sizeof(kIntentSourceNames[0]) == static_cast<size_t>(intent_source::count),
                  "one name per intent_source");

    inline const char* intent_kind_name(intent_kind kind) {
        return kind < intent_kind::count ? kIntentKindNames[static_cast<size_t>(kind)] : "?";
    }
    inline const char* intent_source_name(intent_source source) {
        return source < intent_source::count ? kIntentSourceNames[static_cast<size_t>(source)] : "?";
    }
}
