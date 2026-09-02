#pragma once

// Headless instance of the original game: the statically linked Ballanced
// engine with the NULL rasterizer, a silent sound manager and a scriptable
// input manager.  It loads base.cmo and runs the retail scripts exactly like
// the Player does, one fixed 1/66 s behaviour frame per tick().
//
// Every method must be called from the thread that created the engine.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class CKContext;
class CKTimeManager;
class CKRenderContext;
class CKIpionManager;
class CKBeObject;
class CKDataArray;

namespace bmmo::sim {
    inline constexpr double kTickRate = 66.0;
    inline constexpr float kFixedDeltaMs = 1000.0f / 66.0f;

    struct engine_options {
        std::filesystem::path game_root;      // directory containing base.cmo
        std::string composition = "base.cmo";
        int width = 1024;
        int height = 768;
        bool verbose = false;
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
        // Mirrors Menu_Start: CurrentLevel[0,0] = level, "Load Level" -> Level.
        bool load_level(int level, std::string& error);
        bool send_message(const std::string& message, CKBeObject* target, std::string& error);

        // Scriptable keyboard for the null input manager (CKKEY_* codes).
        void set_key(unsigned key, bool down);
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

        engine_options options_;
        CKContext* context_ = nullptr;
        CKTimeManager* time_ = nullptr;
        CKRenderContext* render_ = nullptr;
        void* game_info_ = nullptr;
        uint64_t ticks_ = 0;
        bool startup_acquired_ = false;
    };
}
