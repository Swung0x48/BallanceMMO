#pragma once

// Headless instance of the original game: the statically linked Ballanced
// engine with the NULL rasterizer, a silent sound manager and a scriptable
// input manager.  It loads base.cmo and runs the retail scripts exactly like
// the Player does, one fixed 1/66 s behaviour frame per tick().  Render-only
// mesh work the Player does is cut where nothing reads it (step DWB: the
// vertex-cache pass), and the sound manager keeps no sample bytes.
//
// Every method must be called from the thread that created the engine.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <entity/session.hpp>
#include <game/menu_driver.hpp>
#include <game/script_record_reuse.hpp>

class CKContext;
class CKTimeManager;
class CKRenderContext;
class CKIpionManager;
class CKBeObject;
class CKDataArray;

namespace bmmo::sim {
    struct engine_options {
        std::filesystem::path game_root;      // directory containing base.cmo
        std::string composition = "base.cmo";
        int width = 1024;
        int height = 768;
        bool verbose = false;
        // Step CMAP: a custom map (a level file outside the slots, the arena) loaded into the requested slot
        // instead of the slot's own file - the slot still picks the ball, music and sky (game/custom_map.hpp).
        // Empty: the slot's file.  Fixed for the engine's life.
        std::filesystem::path map_file;
        std::function<void(const std::string&)> log;  // optional sink
    };

    class headless_engine {
    public:
        static std::unique_ptr<headless_engine> create(const engine_options& options,
                                                       std::string& error);
        ~headless_engine();
        headless_engine(const headless_engine&) = delete;
        headless_engine& operator=(const headless_engine&) = delete;

        // Loads the composition (base.cmo) the same way BallancePlayer does and
        // starts playing.  The retail Default Level script then boots the menu.
        bool load_composition(std::string& error);
        // Runs exactly one behaviour frame with the fixed delta.
        bool tick(std::string& error);
        // Same frame, executed block by block through the CK debug stepper;
        // `on_step` sees every executed script/behaviour (diagnostics only).
        bool tick_debug(const std::function<void(const std::string& script, const std::string& block,
                                                 const std::string& sub_block, int action)>& on_step,
                        std::string& error);
        // Starts a level through the retail menus (game/menu_driver.hpp);
        // advanced by tick() until the level is selected.  Query with
        // level_request().
        void request_level(int level);
        const bmmo::game::level_request& level_request() const { return level_request_; }
        // Direct path: CurrentLevel[0,0] = level, "Load Level" -> Level.  The
        // menus stay active; only for experiments.
        bool load_level(int level, std::string& error);
        bool send_message(const std::string& message, CKBeObject* target, std::string& error);

        // Scriptable keyboard for the null input manager (CKKEY_* codes,
        // KS_PRESSED / KS_RELEASED semantics like the retail manager).
        void set_key(unsigned key, bool down);
        // Verbatim 256-byte keyboard buffer (a recorded retail frame).
        void set_keyboard_state(const unsigned char* keys);
        void clear_keys();

        uint64_t ticks() const { return ticks_; }
        CKContext* context() const { return context_; }
        CKTimeManager* time_manager() const { return time_; }
        CKIpionManager* physics() const;
        CKBeObject* level_object() const;
        CKDataArray* data_array(const char* name) const;
        void log(const std::string& text) const;

    private:
        headless_engine() = default;
        bool finish_load(const std::string& resolved_file, std::string& error);
        void drain_player_commands();
        void advance_level_request();
        // Step CMAP: points Load LevelXX's file prefix at a copy of options_.map_file named as `level`'s slot
        // file (game/custom_map.hpp); false with `error` when it cannot.
        bool redirect_level_file(int level, std::string& error);

        engine_options options_;
        CKContext* context_ = nullptr;
        CKTimeManager* time_ = nullptr;
        CKRenderContext* render_ = nullptr;
        void* game_info_ = nullptr;
        uint64_t ticks_ = 0;
        bool startup_acquired_ = false;
        // Step DWB: BMMO_WORK_CUTS as this engine was made.  On, the Random
        // blocks are patched after every file load (the context's load hook,
        // headless_engine.cpp) and tick() no longer scans for Gameplay_Ingame;
        // off, tick() scans as before and there is no hook; verify, both, with
        // the scan's counts here (a patch it makes is one the hook missed).
        // A hook CK did not register (random_hook_ false) scans as off does.
        bmmo::game::script_reuse_mode work_cuts_ = bmmo::game::script_reuse_mode::on;
        bool random_hook_ = false;
        bool verify_scanning_ = false;
        uint64_t verify_scans_ = 0, verify_scan_patched_ = 0;
        bmmo::game::level_request level_request_;
        bool map_redirect_pending_ = false;   // step CMAP: a custom map's request not redirected yet
        int map_redirect_waited_ = 0;
    };
}
