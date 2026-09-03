#include "headless_engine.hpp"
#include "null_managers.hpp"

#include "CKAll.h"
#include "CKIpionManager.h"
#include "InterfaceManager.h"

#include "bmmo_headless_plugin_registry.h"

// Ballanced Player (unmodified sources compiled into the sim library).
#include "GameConfig.h"
bool EditScript(CKLevel* level, const CGameConfig& config, const char* resolvedFile);

#include <physics/physics_state.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

namespace bmmo::sim {
    namespace {
        std::mutex g_startup_mutex;
        int g_startup_references = 0;

        bool register_static_plugins(std::string& error) {
            CKPluginManager* manager = CKGetPluginManager();
            if (!manager) {
                error = "CKStartUp did not create the plugin manager";
                return false;
            }
            const auto add = [&](const char* name, int (*count)(), CKPluginInfo* (*info)(int),
                                 CKDataReader* (*reader)(int), void (*declarations)(XObjectDeclarationArray*)) {
                const CKERROR result = manager->RegisterStaticPlugin(
                    const_cast<CKSTRING>(name), count, info, reader, declarations);
                if (result != CK_OK && result != CKERR_ALREADYPRESENT) {
                    error = std::string("failed to register static plugin ") + name
                          + ": " + std::to_string(result);
                    return false;
                }
                return true;
            };
            if (!add("BallanceMMO.NullInput", nullptr, null_input_manager_plugin_info, nullptr, nullptr)
                    || !add("BallanceMMO.NullSound", nullptr, null_sound_manager_plugin_info, nullptr, nullptr))
                return false;
            for (const auto& plugin: kHeadlessStaticPlugins)
                if (!add(plugin.name, plugin.get_info_count, plugin.get_info,
                         plugin.get_reader, plugin.register_declarations))
                    return false;
            return true;
        }

        bool acquire_startup(std::string& error) {
            std::lock_guard lock(g_startup_mutex);
            if (g_startup_references == 0) {
                const CKERROR result = CKStartUp();
                if (result != CK_OK) {
                    error = "CKStartUp failed: " + std::to_string(result);
                    return false;
                }
                if (!register_static_plugins(error)) {
                    CKShutdown();
                    return false;
                }
            }
            ++g_startup_references;
            return true;
        }

        void release_startup() {
            std::lock_guard lock(g_startup_mutex);
            if (g_startup_references > 0 && --g_startup_references == 0) CKShutdown();
        }

        int find_render_engine(CKPluginManager* manager) {
            const int count = manager->GetPluginCount(CKPLUGIN_RENDERENGINE_DLL);
            for (int index = 0; index < count; ++index) {
                CKPluginEntry* entry = manager->GetPluginInfo(CKPLUGIN_RENDERENGINE_DLL, index);
                if (!entry) continue;
                CKPluginDll* dll = manager->GetPluginDllInfo(entry->m_PluginDllIndex);
                const char* name = dll && dll->m_DllFileName.CStr() ? dll->m_DllFileName.CStr() : "";
                if (std::string_view(name).find("CK2_3D") != std::string_view::npos) return index;
            }
            return count > 0 ? 0 : -1;
        }

        void add_path(CKPathManager* paths, int category, const std::filesystem::path& path) {
            std::error_code ignored;
            if (!std::filesystem::is_directory(path, ignored)) return;
            XString value(path.string().c_str());
            paths->AddPath(category, value);
        }

        // Scripts drive the Player through TT_InterfaceManager commands
        // (screen mode changes, quit...).  Headless: acknowledge and ignore.
        int player_command_handler(InterfaceManager*, const TTPlayerCommand& command, void* argument) {
            if (auto* engine = static_cast<headless_engine*>(argument))
                engine->log("ignored player command type " + std::to_string(command.type));
            return 1;
        }

        CKERROR log_redirect(CKUICallbackStruct& data, void* argument) {
            auto* engine = static_cast<headless_engine*>(argument);
            if (!engine) return CK_OK;
            if (data.Reason == CKUIM_OUTTOCONSOLE || data.Reason == CKUIM_OUTTOINFOBAR
                    || data.Reason == CKUIM_DEBUGMESSAGESEND) {
                // The retail navigation blocks complain every frame about the
                // parked retail ball (design 8.3); everything else is worth a line.
                if (data.ConsoleString && std::strncmp(data.ConsoleString, "You must Physicalize Ball_", 26) != 0)
                    engine->log(std::string("[CK] ") + data.ConsoleString);
            }
            return CK_OK;
        }
    }

    std::unique_ptr<headless_engine> headless_engine::create(const engine_options& options,
                                                             std::string& error) {
        error.clear();
        std::error_code fs_error;
        const auto root = std::filesystem::weakly_canonical(options.game_root, fs_error);
        if (fs_error || !std::filesystem::is_regular_file(root / options.composition, fs_error)) {
            error = "game root does not contain " + options.composition + ": " + options.game_root.string();
            return nullptr;
        }
        std::unique_ptr<headless_engine> engine(new headless_engine);
        engine->options_ = options;
        engine->options_.game_root = root;
        // The retail scripts use paths relative to the Player's working
        // directory (<root>/Bin): "..\\Database.tdb", "..\\Sounds\\...".
        {
            std::error_code cwd_error;
            const auto bin = root / "Bin";
            std::filesystem::current_path(std::filesystem::is_directory(bin, cwd_error) ? bin : root, cwd_error);
            if (cwd_error) engine->log("warning: could not change the working directory: " + cwd_error.message());
        }
        if (!acquire_startup(error)) return nullptr;
        engine->startup_acquired_ = true;

        CKPluginManager* plugins = CKGetPluginManager();
        const int render_engine = find_render_engine(plugins);
        if (render_engine < 0) {
            error = "no static render engine plugin is registered";
            return nullptr;
        }
        const CKERROR created = CKCreateContext(&engine->context_, nullptr, render_engine, 0);
        if (created != CK_OK || !engine->context_) {
            error = "CKCreateContext failed: " + std::to_string(created);
            return nullptr;
        }
        CKContext* context = engine->context_;
        context->SetVirtoolsVersion(CK_VIRTOOLS_DEV, 0x2000043);
        context->SetInterfaceMode(FALSE, log_redirect, engine.get());

        engine->time_ = context->GetTimeManager();
        if (!engine->time_ || !context->GetRenderManager() || !context->GetMessageManager()
                || !context->GetManagerByGuid(INPUT_MANAGER_GUID)
                || !context->GetManagerByGuid(SOUND_MANAGER_GUID)
                || !CKIpionManager::GetManager(context)) {
            error = "headless CKContext is missing a required manager";
            return nullptr;
        }

        if (auto* interface_manager = InterfaceManager::GetManager(context))
            interface_manager->SetPlayerCommandHandler(player_command_handler, engine.get());

        // Paths: mirror the Player (composition directory for data, plus the
        // conventional Textures and Sounds folders).
        CKPathManager* paths = context->GetPathManager();
        add_path(paths, DATA_PATH_IDX, root);
        add_path(paths, DATA_PATH_IDX, root / "3D Entities");
        add_path(paths, BITMAP_PATH_IDX, root / "Textures");
        add_path(paths, BITMAP_PATH_IDX, root);
        add_path(paths, SOUND_PATH_IDX, root / "Sounds");
        add_path(paths, SOUND_PATH_IDX, root);

        CKRenderManager* render_manager = context->GetRenderManager();
        if (render_manager->GetRenderDriverCount() > 0) {
            CKRECT rect{0, 0, options.width, options.height};
            engine->render_ = render_manager->CreateRenderContext(nullptr, 0, &rect, FALSE, 32);
        }
        if (!engine->render_) engine->log("warning: no headless render context; running without one");

        engine->time_->SetTimeScaleFactor(1.0f);
        engine->time_->SetMinimumDeltaTime(kFixedDeltaMs);
        engine->time_->SetMaximumDeltaTime(kFixedDeltaMs);
        return engine;
    }

    headless_engine::~headless_engine() {
        if (context_) {
            context_->Reset();
            context_->ClearAll();
            if (render_) {
                if (auto* render_manager = context_->GetRenderManager())
                    render_manager->DestroyRenderContext(render_);
                render_ = nullptr;
            }
            CKCloseContext(context_);
            context_ = nullptr;
        }
        delete static_cast<CGameInfo*>(game_info_);
        game_info_ = nullptr;
        if (startup_acquired_) release_startup();
    }

    void headless_engine::log(const std::string& text) const {
        if (options_.log) options_.log(text);
        else if (options_.verbose) std::fprintf(stderr, "%s\n", text.c_str());
    }

    bool headless_engine::load_composition(std::string& error) {
        error.clear();
        CKPathManager* paths = context_->GetPathManager();
        XString resolved(options_.composition.c_str());
        if (paths->ResolveFileName(resolved, DATA_PATH_IDX) != CK_OK) {
            error = "failed to resolve composition " + options_.composition;
            return false;
        }
        context_->Reset();
        context_->ClearAll();

        CKFile* file = context_->CreateCKFile();
        if (!file) {
            error = "CreateCKFile failed";
            return false;
        }
        CKERROR result = file->OpenFile(resolved.Str(),
            static_cast<CK_LOAD_FLAGS>(CK_LOAD_DEFAULT | CK_LOAD_CHECKDEPENDENCIES));
        if (result != CK_OK) {
            std::string missing;
            if (result == CKERR_PLUGINSMISSING) {
                if (const auto* dependencies = file->GetMissingPlugins()) {
                    for (const auto* it = dependencies->Begin(); it != dependencies->End(); ++it) {
                        for (int guid = 0; guid < it->m_Guids.Size(); ++guid) {
                            if (it->ValidGuids.IsSet(guid)) continue;
                            char buffer[64];
                            std::snprintf(buffer, sizeof(buffer), " [%d:%08x-%08x]",
                                it->m_PluginCategory, it->m_Guids[guid].d1, it->m_Guids[guid].d2);
                            missing += buffer;
                        }
                    }
                }
            }
            context_->DeleteCKFile(file);
            error = "OpenFile failed for " + std::string(resolved.CStr()) + ": "
                  + std::to_string(result) + missing;
            return false;
        }
        CKObjectArray* objects = CreateCKObjectArray();
        result = file->LoadFileData(objects);
        context_->DeleteCKFile(file);
        DeleteCKObjectArray(objects);
        if (result != CK_OK) {
            error = "LoadFileData failed: " + std::to_string(result);
            return false;
        }
        return finish_load(resolved.CStr(), error);
    }

    bool headless_engine::finish_load(const std::string& resolved_file, std::string& error) {
        if (auto* interface_manager = InterfaceManager::GetManager(context_)) {
            interface_manager->SetDriver(0);
            interface_manager->SetScreenMode(0);
            interface_manager->SetRookie(false);
            interface_manager->SetTaskSwitchEnabled(true);
            delete static_cast<CGameInfo*>(game_info_);
            auto* info = new CGameInfo;
            std::strncpy(info->path, ".", sizeof(info->path) - 1);
            std::strncpy(info->fileName, options_.composition.c_str(), sizeof(info->fileName) - 1);
            game_info_ = info;
            interface_manager->SetGameInfo(info);
        }
        CKLevel* level = context_->GetCurrentLevel();
        if (!level) {
            error = "composition has no CKLevel: " + resolved_file;
            return false;
        }
        // Same boot-script hotfixes as the Player the clients run.  Only
        // language and the opening skip are gameplay-visible; neither touches
        // the physics world.
        {
            CGameConfig config;
            config.langId = 1;
            config.skipOpening = true;
            config.applyHotfix = true;
            config.unlockFramerate = false;
            config.unlockWidescreen = false;
            config.unlockHighResolution = false;
            config.debug = false;
            config.rookie = false;
            if (!EditScript(level, config, resolved_file.c_str()))
                log("warning: boot script hotfixes were not fully applied");
        }
        if (render_) {
            level->AddRenderContext(render_, TRUE);
            const XObjectPointerArray cameras = context_->GetObjectListByType(CKCID_CAMERA, TRUE);
            if (cameras.Size() != 0) render_->AttachViewpointToCamera(static_cast<CKCamera*>(cameras[0]));
        }
        level->LaunchScene(nullptr);
        if (render_) render_->Render();
        const CKERROR played = context_->Play();
        if (played != CK_OK) {
            error = "CKContext::Play failed: " + std::to_string(played);
            return false;
        }
        // Deterministic "Random" block (design 9.10 / Part B): reroutes the
        // Random blocks inside the trafo explosion scripts once Balls.nmo
        // has been loaded by the boot scripts.  tick() repeats this while a
        // level is still loading; cheap (SetFunction only on a mismatch), and
        // reset_session_clock repeats it at every anchor anyway.
        const int patched = bmmo::physics::install_random_block(context_);
        if (patched >= 0) log("random block: " + std::to_string(patched) + " instances patched");
        else log("random block: no \"Random\" prototype registered");
        return true;
    }

    void headless_engine::drain_player_commands() {
        auto* interface_manager = InterfaceManager::GetManager(context_);
        if (!interface_manager) return;
        TTPlayerCommand command;
        while (interface_manager->PollPlayerCommand(command))
            log("ignored player command type " + std::to_string(command.type));
    }

    bool headless_engine::tick(std::string& error) {
        error.clear();
        if (!context_->IsPlaying()) {
            const CKERROR played = context_->Play();
            if (played != CK_OK) {
                error = "CKContext::Play failed: " + std::to_string(played);
                return false;
            }
        }
        time_->SetTimeScaleFactor(1.0f);
        time_->SetMinimumDeltaTime(kFixedDeltaMs);
        time_->SetMaximumDeltaTime(kFixedDeltaMs);
        // Re-patch the "Random" block instances while a level is still
        // loading (design 9.10): base.cmo's own instances are already caught
        // by finish_load, this catches whatever a level's file adds, before
        // its scripts get a chance to draw from it.  Stops once
        // Gameplay_Ingame is active so a long session does not keep scanning
        // every object every tick.
        if (!bmmo::game::script_active(context_, "Gameplay_Ingame")) {
            const int patched = bmmo::physics::install_random_block(context_);
            if (patched > 0) log("random block: " + std::to_string(patched) + " instances patched");
        }
        advance_level_request();
        const CKERROR processed = context_->Process();
        if (processed != CK_OK) {
            error = "CKContext::Process failed: " + std::to_string(processed);
            return false;
        }
        if (std::fabs(time_->GetLastDeltaTime() - kFixedDeltaMs) > 1.0e-4f) {
            error = "behaviour delta drifted from the fixed tick: "
                  + std::to_string(time_->GetLastDeltaTime());
            return false;
        }
        ++ticks_;
        drain_player_commands();
        return true;
    }

    bool headless_engine::tick_debug(
            const std::function<void(const std::string&, const std::string&, const std::string&, int)>& on_step,
            std::string& error) {
        error.clear();
        if (!context_->IsPlaying()) context_->Play();
        time_->SetTimeScaleFactor(1.0f);
        time_->SetMinimumDeltaTime(kFixedDeltaMs);
        time_->SetMaximumDeltaTime(kFixedDeltaMs);
        advance_level_request();
        const CKERROR started = context_->ProcessDebugStart(kFixedDeltaMs);
        if (started != CK_OK) {
            error = "ProcessDebugStart failed: " + std::to_string(started);
            return false;
        }
        const auto name_of = [](CKObject* object) -> std::string {
            return object && object->GetName() ? object->GetName() : "?";
        };
        while (context_->ProcessDebugStep()) {
            CKDebugContext* debug = context_->GetDebugContext();
            if (!debug || !debug->CurrentBehavior) continue;
            std::string block = name_of(debug->CurrentBehavior);
            if (debug->CurrentBehavior->IsUsingFunction()) {
                block += '{';
                block += debug->CurrentBehavior->GetPrototypeName() ? debug->CurrentBehavior->GetPrototypeName() : "?";
                block += '}';
            }
            on_step(name_of(debug->CurrentScript), block,
                    debug->SubBehavior ? name_of(debug->SubBehavior) : std::string(),
                    static_cast<int>(debug->CurrentBehaviorAction));
        }
        const CKERROR ended = context_->ProcessDebugEnd();
        if (ended != CK_OK) {
            error = "ProcessDebugEnd failed: " + std::to_string(ended);
            return false;
        }
        ++ticks_;
        drain_player_commands();
        return true;
    }

    CKBeObject* headless_engine::level_object() const {
        const int count = context_->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context_->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int i = 0; i < count; ++i) {
            auto* behavior = CKBehavior::Cast(context_->GetObject(ids[i]));
            if (behavior && behavior->GetName() && !behavior->GetParent()
                    && std::string_view(behavior->GetName()) == "Event_handler")
                return behavior->GetOwner();
        }
        return nullptr;
    }

    CKDataArray* headless_engine::data_array(const char* name) const {
        return CKDataArray::Cast(context_->GetObjectByNameAndClass(
            const_cast<CKSTRING>(name), CKCID_DATAARRAY, nullptr));
    }

    bool headless_engine::send_message(const std::string& message, CKBeObject* target, std::string& error) {
        error.clear();
        if (!target) {
            error = "message target is null";
            return false;
        }
        auto* messages = context_->GetMessageManager();
        const CKMessageType type = messages->AddMessageType(const_cast<CKSTRING>(message.c_str()));
        messages->SendMessageSingle(type, target, nullptr);
        return true;
    }

    void headless_engine::request_level(int level) {
        level_request_.begin(level);
        log("level " + std::to_string(level) + " requested through the menus");
    }

    void headless_engine::advance_level_request() {
        if (!level_request_.pending()) return;
        using status = bmmo::game::level_request::status;
        const bool was_pressed = level_request_.start_pressed;
        switch (level_request_.step(context_)) {
        case status::start_pressed:
            if (!was_pressed) log("menu: pressed Start in Menu_Main at tick " + std::to_string(ticks_));
            break;
        case status::level_selected:
            log("menu: selected level " + std::to_string(level_request_.level) + " in Menu_Start at tick "
                + std::to_string(ticks_));
            break;
        case status::failed:
            log("menu: level request failed: " + level_request_.error);
            break;
        default:
            break;
        }
    }

    bool headless_engine::load_level(int level, std::string& error) {
        error.clear();
        CKDataArray* current_level = data_array("CurrentLevel");
        if (!current_level) {
            error = "CurrentLevel array is unavailable (composition not booted yet?)";
            return false;
        }
        if (!current_level->SetElementValue(0, 0, &level, sizeof(level))) {
            error = "failed to write CurrentLevel[0,0]";
            return false;
        }
        CKBeObject* target = level_object();
        if (!target) {
            error = "Event_handler owner (Level) not found";
            return false;
        }
        return send_message("Load Level", target, error);
    }

    void headless_engine::set_key(unsigned key, bool down) {
        if (key >= 256) return;
        if (auto* state = null_input_keyboard_state(context_)) state[key] = down ? KS_PRESSED : KS_RELEASED;
    }

    void headless_engine::set_keyboard_state(const unsigned char* keys) {
        if (auto* state = null_input_keyboard_state(context_)) std::memcpy(state, keys, 256);
    }

    void headless_engine::clear_keys() {
        if (auto* state = null_input_keyboard_state(context_)) std::memset(state, 0, 256);
    }

    CKIpionManager* headless_engine::physics() const {
        return CKIpionManager::GetManager(context_);
    }
}
