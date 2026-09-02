#pragma once

// Deterministic keyboard injection for the running game (BallanceTAS
// technique): hook CKInputManager::PreProcess through its vtable and, after
// the retail manager has polled DirectInput, overwrite the keyboard state the
// scripts will read during this behaviour frame.  Used by test automation
// (held keys) and by record playback (full 256-byte state per tick).

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class CKContext;

namespace bmmo::session {
    class input_injector {
    public:
        // Callback runs on the game thread right after the retail input poll.
        // `state` is the manager's live 256-byte DirectInput keyboard buffer.
        using post_input_callback = std::function<void(unsigned char* state)>;

        static input_injector& instance();

        bool install(CKContext* context, std::string& error);
        void uninstall();
        bool installed() const { return installed_; }

        void set_callback(post_input_callback callback);

    private:
        input_injector() = default;
        static long __fastcall pre_process_detour(void* self, void* edx);

        bool installed_ = false;
        void* target_ = nullptr;
        void* original_ = nullptr;
        std::mutex mutex_;
        post_input_callback callback_;
    };
}
