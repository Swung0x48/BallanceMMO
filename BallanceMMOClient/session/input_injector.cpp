#include "input_injector.hpp"

#include "bml_includes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <MinHook.h>

namespace bmmo::session {
    namespace {
        // CKBaseManager vtable: 0 dtor, 1 SaveData, 2 LoadData, 3 PreClearAll,
        // 4 PostClearAll, 5 PreProcess (see the Virtools SDK CKBaseManager.h).
        constexpr size_t kPreProcessSlot = 5;
        using pre_process_fn = long(__fastcall*)(void* self, void* edx);
    }

    input_injector& input_injector::instance() {
        static input_injector injector;
        return injector;
    }

    long __fastcall input_injector::pre_process_detour(void* self, void* edx) {
        auto& injector = instance();
        const long result = reinterpret_cast<pre_process_fn>(injector.original_)(self, edx);
        post_input_callback callback;
        {
            std::lock_guard lk(injector.mutex_);
            callback = injector.callback_;
        }
        if (callback) {
            auto* manager = static_cast<CKInputManager*>(self);
            if (unsigned char* state = manager->GetKeyboardState()) callback(state);
        }
        return result;
    }

    bool input_injector::install(CKContext* context, std::string& error) {
        error.clear();
        if (installed_) return true;
        if (!context) {
            error = "no CKContext";
            return false;
        }
        auto* manager = context->GetManagerByGuid(INPUT_MANAGER_GUID);
        if (!manager) {
            error = "the retail input manager is unavailable";
            return false;
        }
        void** vtable = *reinterpret_cast<void***>(manager);
        target_ = vtable[kPreProcessSlot];
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            error = std::string("MH_Initialize failed: ") + MH_StatusToString(init);
            return false;
        }
        const MH_STATUS created = MH_CreateHook(
            target_, reinterpret_cast<void*>(&input_injector::pre_process_detour), &original_);
        if (created != MH_OK) {
            error = std::string("MH_CreateHook failed: ") + MH_StatusToString(created);
            return false;
        }
        const MH_STATUS enabled = MH_EnableHook(target_);
        if (enabled != MH_OK) {
            MH_RemoveHook(target_);
            error = std::string("MH_EnableHook failed: ") + MH_StatusToString(enabled);
            return false;
        }
        installed_ = true;
        return true;
    }

    void input_injector::uninstall() {
        if (!installed_) return;
        MH_DisableHook(target_);
        MH_RemoveHook(target_);
        installed_ = false;
        target_ = nullptr;
        original_ = nullptr;
        std::lock_guard lk(mutex_);
        callback_ = nullptr;
    }

    void input_injector::set_callback(post_input_callback callback) {
        std::lock_guard lk(mutex_);
        callback_ = std::move(callback);
    }
}
