#include "null_managers.hpp"

#include "CKAll.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace bmmo::sim {
    namespace {
        constexpr const char* kNullInputName = "BallanceMMO Null Input Manager";
        constexpr const char* kNullSoundName = "BallanceMMO Null Sound Manager";

        struct key_name { CKDWORD code; const char* name; };
        // DirectInput scan codes for the keys the retail scripts reference.
        constexpr key_name kKeyNames[] = {
            {CKKEY_ESCAPE, "Esc"}, {CKKEY_RETURN, "Enter"}, {CKKEY_SPACE, "Space"},
            {CKKEY_UP, "Up"}, {CKKEY_DOWN, "Down"}, {CKKEY_LEFT, "Left"}, {CKKEY_RIGHT, "Right"},
            {CKKEY_LSHIFT, "Shift"}, {CKKEY_RSHIFT, "Right Shift"}, {CKKEY_LCONTROL, "Ctrl"},
            {CKKEY_RCONTROL, "Right Ctrl"}, {CKKEY_TAB, "Tab"}, {CKKEY_BACK, "Backspace"},
            {CKKEY_F1, "F1"}, {CKKEY_F2, "F2"}, {CKKEY_F3, "F3"}, {CKKEY_F4, "F4"},
            {CKKEY_W, "W"}, {CKKEY_A, "A"}, {CKKEY_S, "S"}, {CKKEY_D, "D"},
        };

        class null_input_manager final : public CKInputManager {
        public:
            explicit null_input_manager(CKContext* context)
                : CKInputManager(context, const_cast<CKSTRING>(kNullInputName)) {
                std::memset(keys_, 0, sizeof(keys_));
                std::memset(previous_, 0, sizeof(previous_));
                std::memset(toggled_, 0, sizeof(toggled_));
                context->RegisterNewManager(this);
            }

            // The keyboard buffer holds the same KS_IDLE / KS_PRESSED /
            // KS_RELEASED bytes as the retail manager, so recorded retail
            // states can be replayed verbatim.  PreProcess derives the press
            // edges; PostProcess retires KS_RELEASED after its one frame.
            CKDWORD GetValidFunctionsMask() override {
                return CKMANAGER_FUNC_PreProcess | CKMANAGER_FUNC_PostProcess;
            }
            CKERROR PreProcess() override {
                for (int key = 0; key < 256; ++key) {
                    toggled_[key] = (keys_[key] & KS_PRESSED) && !(previous_[key] & KS_PRESSED);
                    previous_[key] = keys_[key];
                }
                return CK_OK;
            }
            CKERROR PostProcess() override {
                for (auto& key: keys_)
                    if (key == KS_RELEASED) key = KS_IDLE;
                return CK_OK;
            }

            void EnableKeyboardRepetition(CKBOOL enable) override { repetition_ = enable; }
            CKBOOL IsKeyboardRepetitionEnabled() override { return repetition_; }
            CKBOOL IsKeyDown(CKDWORD key, CKDWORD* stamp) override {
                if (stamp) *stamp = 0;
                return key < 256 && (keys_[key] & KS_PRESSED) ? TRUE : FALSE;
            }
            CKBOOL IsKeyUp(CKDWORD key) override { return !IsKeyDown(key, nullptr); }
            CKBOOL IsKeyToggled(CKDWORD key, CKDWORD* stamp) override {
                if (stamp) *stamp = 0;
                return key < 256 && toggled_[key] ? TRUE : FALSE;
            }
            int GetKeyName(CKDWORD key, char* name) override {
                if (!name) return 0;
                for (const auto& entry: kKeyNames)
                    if (entry.code == key) {
                        std::strcpy(name, entry.name);
                        return static_cast<int>(std::strlen(name)) + 1;
                    }
                const int written = std::snprintf(name, 32, "Key%u", static_cast<unsigned>(key));
                return written > 0 ? written + 1 : 0;
            }
            CKDWORD GetKeyFromName(CKSTRING name) override {
                if (!name) return 0;
                for (const auto& entry: kKeyNames)
                    if (_stricmp(entry.name, name) == 0) return entry.code;
                if (std::strncmp(name, "Key", 3) == 0)
                    return static_cast<CKDWORD>(std::strtoul(name + 3, nullptr, 10));
                return 0;
            }
            unsigned char* GetKeyboardState() override { return keys_; }
            CKBOOL IsKeyboardAttached() override { return TRUE; }
            int GetNumberOfKeyInBuffer() override { return 0; }
            int GetKeyFromBuffer(int, CKDWORD& key, CKDWORD* stamp) override {
                key = 0;
                if (stamp) *stamp = 0;
                return 0;
            }
            CKBOOL IsMouseButtonDown(CK_MOUSEBUTTON) override { return FALSE; }
            CKBOOL IsMouseClicked(CK_MOUSEBUTTON) override { return FALSE; }
            CKBOOL IsMouseToggled(CK_MOUSEBUTTON) override { return FALSE; }
            void GetMouseButtonsState(CKBYTE states[4]) override { std::memset(states, 0, 4); }
            void GetMousePosition(Vx2DVector& position, CKBOOL) override { position.Set(0.0f, 0.0f); }
            void GetMouseRelativePosition(VxVector& position) override { position.Set(0.0f, 0.0f, 0.0f); }
            CKBOOL IsMouseAttached() override { return FALSE; }
            CKBOOL IsJoystickAttached(int) override { return FALSE; }
            void GetJoystickPosition(int, VxVector* position) override { if (position) position->Set(0.0f, 0.0f, 0.0f); }
            void GetJoystickRotation(int, VxVector* rotation) override { if (rotation) rotation->Set(0.0f, 0.0f, 0.0f); }
            void GetJoystickSliders(int, Vx2DVector* position) override { if (position) position->Set(0.0f, 0.0f); }
            void GetJoystickPointOfViewAngle(int, float* angle) override { if (angle) *angle = -1.0f; }
            CKDWORD GetJoystickButtonsState(int) override { return 0; }
            CKBOOL IsJoystickButtonDown(int, int) override { return FALSE; }
            void Pause(CKBOOL pause) override { paused_ = pause; }
            void ShowCursor(CKBOOL show) override { cursor_ = show; }
            CKBOOL GetCursorVisibility() override { return cursor_; }
            VXCURSOR_POINTER GetSystemCursor() override { return VXCURSOR_NORMALSELECT; }
            void SetSystemCursor(VXCURSOR_POINTER) override {}

        private:
            unsigned char keys_[256];
            unsigned char previous_[256];
            bool toggled_[256];
            CKBOOL repetition_ = FALSE;
            CKBOOL paused_ = FALSE;
            CKBOOL cursor_ = FALSE;
        };

        struct silent_source {
            CKWaveFormat format{};
            CK_WAVESOUND_TYPE type = CK_WAVESOUND_BACKGROUND;
            CKWaveSoundSettings settings{};
            CKWaveSound3DSettings settings_3d{};
            std::vector<CKBYTE> bytes;
            int play_position = 0;
            CKBOOL playing = FALSE;
        };

        class null_sound_manager final : public CKSoundManager {
        public:
            explicit null_sound_manager(CKContext* context)
                : CKSoundManager(context, const_cast<CKSTRING>(kNullSoundName)) {
                context->RegisterNewManager(this);
            }

            CK_SOUNDMANAGER_CAPS GetCaps() override { return static_cast<CK_SOUNDMANAGER_CAPS>(0); }
            void* CreateSource(CK_WAVESOUND_TYPE flags, CKWaveFormat* format, CKDWORD bytes, CKBOOL) override {
                if (!format || bytes == 0 || bytes > 256u * 1024u * 1024u) return nullptr;
                auto* source = new (std::nothrow) silent_source;
                if (!source) return nullptr;
                source->format = *format;
                source->type = flags;
                try {
                    source->bytes.resize(bytes);
                } catch (...) {
                    delete source;
                    return nullptr;
                }
                return source;
            }
            void* DuplicateSource(void* raw) override {
                auto* source = static_cast<silent_source*>(raw);
                return source ? new (std::nothrow) silent_source(*source) : nullptr;
            }
            void ReleaseSource(void* source) override { delete static_cast<silent_source*>(source); }
            void Play(CKWaveSound* sound, void* raw, CKBOOL) override {
                // CKWaveSound::PlayMinion plays a duplicated sound by passing
                // sound == NULL and the SoundMinion wrapper (64 bytes) as the
                // source argument; the manager's own handle is inside it.
                // Treating the wrapper as a silent_source wrote past its end
                // and corrupted the heap (found by AddressSanitizer).
                if (!sound && raw) raw = static_cast<SoundMinion*>(raw)->m_Source;
                if (auto* source = static_cast<silent_source*>(raw)) source->playing = TRUE;
            }
            void Pause(CKWaveSound*, void* raw) override { InternalPause(raw); }
            void SetPlayPosition(void* raw, int position) override {
                if (auto* source = static_cast<silent_source*>(raw))
                    source->play_position = std::clamp(position, 0, static_cast<int>(source->bytes.size()));
            }
            int GetPlayPosition(void* raw) override {
                auto* source = static_cast<silent_source*>(raw);
                return source ? source->play_position : 0;
            }
            CKBOOL IsPlaying(void* raw) override {
                auto* source = static_cast<silent_source*>(raw);
                return source ? source->playing : FALSE;
            }
            CKERROR SetWaveFormat(void* raw, CKWaveFormat& format) override {
                auto* source = static_cast<silent_source*>(raw);
                if (!source) return CKERR_INVALIDPARAMETER;
                source->format = format;
                return CK_OK;
            }
            CKERROR GetWaveFormat(void* raw, CKWaveFormat& format) override {
                auto* source = static_cast<silent_source*>(raw);
                if (!source) return CKERR_INVALIDPARAMETER;
                format = source->format;
                return CK_OK;
            }
            int GetWaveSize(void* raw) override {
                auto* source = static_cast<silent_source*>(raw);
                return source ? static_cast<int>(source->bytes.size()) : 0;
            }
            CKERROR Lock(void* raw, CKDWORD write_cursor, CKDWORD count, void** ptr1, CKDWORD* bytes1,
                         void** ptr2, CKDWORD* bytes2, CK_WAVESOUND_LOCKMODE) override {
                auto* source = static_cast<silent_source*>(raw);
                if (!source || !ptr1 || !bytes1) return CKERR_INVALIDPARAMETER;
                const CKDWORD size = static_cast<CKDWORD>(source->bytes.size());
                if (write_cursor > size) return CKERR_INVALIDPARAMETER;
                const CKDWORD first = std::min(count, size - write_cursor);
                *ptr1 = source->bytes.data() + write_cursor;
                *bytes1 = first;
                if (ptr2) *ptr2 = first < count ? source->bytes.data() : nullptr;
                if (bytes2) *bytes2 = first < count ? std::min(count - first, size) : 0;
                return CK_OK;
            }
            CKERROR Unlock(void*, void*, CKDWORD, void*, CKDWORD) override { return CK_OK; }
            void SetType(void* raw, CK_WAVESOUND_TYPE type) override {
                if (auto* source = static_cast<silent_source*>(raw)) source->type = type;
            }
            CK_WAVESOUND_TYPE GetType(void* raw) override {
                auto* source = static_cast<silent_source*>(raw);
                return source ? source->type : CK_WAVESOUND_BACKGROUND;
            }
            void UpdateSettings(void* raw, CK_SOUNDMANAGER_CAPS, CKWaveSoundSettings& settings, CKBOOL set) override {
                auto* source = static_cast<silent_source*>(raw);
                if (!source) return;
                if (set) source->settings = settings;
                else settings = source->settings;
            }
            void Update3DSettings(void* raw, CK_SOUNDMANAGER_CAPS, CKWaveSound3DSettings& settings, CKBOOL set) override {
                auto* source = static_cast<silent_source*>(raw);
                if (!source) return;
                if (set) source->settings_3d = settings;
                else settings = source->settings_3d;
            }
            void UpdateListenerSettings(CK_SOUNDMANAGER_CAPS, CKListenerSettings& settings, CKBOOL set) override {
                if (set) listener_ = settings;
                else settings = listener_;
            }
            CKBOOL IsInitialized() override { return TRUE; }

        protected:
            void InternalPause(void* raw) override {
                if (auto* source = static_cast<silent_source*>(raw)) source->playing = FALSE;
            }
            void InternalPlay(void* raw, CKBOOL) override {
                if (auto* source = static_cast<silent_source*>(raw)) source->playing = TRUE;
            }

        private:
            CKListenerSettings listener_{};
        };

        // The retail scripts store keyboard keys as the "Keyboard Key"
        // parameter type that the input manager plugin registers.
        const CKGUID kGetMousePositionOp(0x6ea0201, 0x680e3a62);
        const CKGUID kGetMouseXOp(0x53c51abe, 0xeba68de);
        const CKGUID kGetMouseYOp(0x27af3c9f, 0xdbc4eb3);

        int key_string_function(CKParameter* param, char* value, CKBOOL read_from_string) {
            if (!param) return 0;
            auto* manager = static_cast<CKInputManager*>(
                param->GetCKContext()->GetManagerByGuid(INPUT_MANAGER_GUID));
            if (!manager) return 0;
            if (read_from_string) {
                if (!value) return 0;
                CKDWORD key = value[0] ? manager->GetKeyFromName(value) : 0;
                param->SetValue(&key);
                return 0;
            }
            CKDWORD key = 0;
            param->GetValue(&key, FALSE);
            const int length = manager->GetKeyName(key, value);
            return length > 1 ? length : 0;
        }

        void mouse_position_op(CKContext*, CKParameterOut* result, CKParameterIn*, CKParameterIn*) {
            if (result) *static_cast<Vx2DVector*>(result->GetWriteDataPtr()) = Vx2DVector(0.0f, 0.0f);
        }
        void mouse_axis_op(CKContext*, CKParameterOut* result, CKParameterIn*, CKParameterIn*) {
            if (result) *static_cast<int*>(result->GetWriteDataPtr()) = 0;
        }

        CKERROR create_null_input(CKContext* context) {
            CKParameterTypeDesc desc;
            desc.TypeName = const_cast<CKSTRING>("Keyboard Key");
            desc.Guid = CKPGUID_KEY;
            desc.DerivedFrom = CKPGUID_INT;
            desc.Valid = TRUE;
            desc.DefaultSize = 4;
            desc.StringFunction = key_string_function;
            CKParameterManager* parameters = context->GetParameterManager();
            parameters->RegisterParameterType(&desc);
            parameters->RegisterOperationType(kGetMousePositionOp, const_cast<CKSTRING>("Get Mouse Position"));
            parameters->RegisterOperationType(kGetMouseXOp, const_cast<CKSTRING>("Get Mouse X"));
            parameters->RegisterOperationType(kGetMouseYOp, const_cast<CKSTRING>("Get Mouse Y"));
            parameters->RegisterOperationFunction(kGetMouseXOp, CKPGUID_INT, CKPGUID_NONE, CKPGUID_NONE, mouse_axis_op);
            parameters->RegisterOperationFunction(kGetMouseYOp, CKPGUID_INT, CKPGUID_NONE, CKPGUID_NONE, mouse_axis_op);
            parameters->RegisterOperationFunction(kGetMousePositionOp, CKPGUID_2DVECTOR, CKPGUID_NONE, CKPGUID_NONE, mouse_position_op);
            parameters->RegisterOperationFunction(kGetMousePositionOp, CKPGUID_2DVECTOR, CKPGUID_BOOL, CKPGUID_NONE, mouse_position_op);
            new null_input_manager(context);
            return CK_OK;
        }
        CKERROR remove_null_input(CKContext* context) {
            delete static_cast<null_input_manager*>(
                context->GetManagerByName(const_cast<CKSTRING>(kNullInputName)));
            CKParameterManager* parameters = context->GetParameterManager();
            parameters->UnRegisterParameterType(CKPGUID_KEY);
            parameters->UnRegisterOperationType(kGetMousePositionOp);
            parameters->UnRegisterOperationType(kGetMouseXOp);
            parameters->UnRegisterOperationType(kGetMouseYOp);
            return CK_OK;
        }
        CKERROR create_null_sound(CKContext* context) {
            new null_sound_manager(context);
            return CK_OK;
        }
        CKERROR remove_null_sound(CKContext* context) {
            delete static_cast<null_sound_manager*>(
                context->GetManagerByName(const_cast<CKSTRING>(kNullSoundName)));
            return CK_OK;
        }
    }

    CKPluginInfo* null_input_manager_plugin_info(int) {
        static CKPluginInfo info;
        info.m_Author = const_cast<CKSTRING>("BallanceMMO");
        info.m_Description = const_cast<CKSTRING>("Scriptable headless keyboard");
        info.m_Extension = const_cast<CKSTRING>("");
        info.m_Type = CKPLUGIN_MANAGER_DLL;
        info.m_Version = 1;
        info.m_InitInstanceFct = create_null_input;
        info.m_ExitInstanceFct = remove_null_input;
        info.m_GUID = INPUT_MANAGER_GUID;
        info.m_Summary = const_cast<CKSTRING>("Null Input Manager");
        return &info;
    }

    CKPluginInfo* null_sound_manager_plugin_info(int) {
        static CKPluginInfo info;
        info.m_Author = const_cast<CKSTRING>("BallanceMMO");
        info.m_Description = const_cast<CKSTRING>("Silent headless sound manager");
        info.m_Extension = const_cast<CKSTRING>("");
        info.m_Type = CKPLUGIN_MANAGER_DLL;
        info.m_Version = 1;
        info.m_InitInstanceFct = create_null_sound;
        info.m_ExitInstanceFct = remove_null_sound;
        info.m_GUID = SOUND_MANAGER_GUID;
        info.m_Summary = const_cast<CKSTRING>("Null Sound Manager");
        return &info;
    }

    unsigned char* null_input_keyboard_state(CKContext* context) {
        if (!context) return nullptr;
        auto* manager = static_cast<null_input_manager*>(
            context->GetManagerByName(const_cast<CKSTRING>(kNullInputName)));
        return manager ? manager->GetKeyboardState() : nullptr;
    }
}
