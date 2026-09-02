#pragma once

// Device-free replacements for the SDL input and sound managers.  They are
// registered as static plugins with the retail manager GUIDs so every script
// and building block that looks the managers up keeps working headlessly.

struct CKPluginInfo;
class CKContext;

namespace bmmo::sim {
    CKPluginInfo* null_input_manager_plugin_info(int index);
    CKPluginInfo* null_sound_manager_plugin_info(int index);

    // Keyboard state of the null input manager owned by `context`
    // (256 bytes, DirectInput layout: 0x80 = pressed).  Null when absent.
    unsigned char* null_input_keyboard_state(CKContext* context);
}
