#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <steam/steamnetworkingtypes.h>
#include "bml_includes.h"

class utils {
private:
    IBML* bml_;
    // written once from the game thread, read from every other thread
    mutable std::atomic<std::thread::id> game_thread_id_{};
    // Engine values other threads need but must not query themselves: the main
    // window handle never changes, and the render size is refreshed once per
    // frame by the game thread (see refresh_engine_cache).
    mutable std::atomic<HWND> main_window_{};
    mutable std::atomic_int render_width_ = 1024, render_height_ = 768;

public:
    utils(IBML* bml);

    static void md5_from_file(const std::string& path, uint8_t* result);
    static uint32_t get_system_dpi();

    // Cached rather than queried through CKContext, so worker threads can ask
    // for the window without touching the engine.
    inline HWND get_main_window() const { return main_window_.load(); }

    // Game thread only; call once the render context exists and then per frame.
    void refresh_engine_cache() const {
        if (!main_window_.load())
            main_window_ = static_cast<HWND>(bml_->GetCKContext()->GetMainWindow());
        if (auto* rc = bml_->GetRenderContext()) {
            render_width_ = rc->GetWidth();
            render_height_ = rc->GetHeight();
        }
    }

    bool is_foreground_window() const;
    void flash_window() const;

    int split_lines(std::string& text, float max_width, float font_size, int font_weight) const;

    static const char* get_system_font();

    // input: desired font size on BallanceBug's screen
    // window size: 1024x768; dpi: 119
    int get_display_font_size(float size) const;

    // blocks as it uses this_thread::sleep
    void display_important_notification(std::string text, float font_size, int line_count, int weight = 700, float y_pos = 0.4f) const;

    static void cleanup_old_crash_dumps();

    static std::string pretty_percentage(float value);
    static std::string pretty_bytes(float bytes);
    static std::string pretty_status(const SteamNetConnectionRealTimeStatus_t& status);

    inline void schedule_sync_call(std::function<void()>&& func) const {
        bml_->AddTimer(CKDWORD(0), [func = std::move(func)] { func(); });
    }

    // Call once from a BML callback (OnLoad) to record which thread the game
    // engine runs its callbacks on; run_on_game_thread() needs it to tell
    // "already there" from "must be scheduled".
    inline void set_game_thread() const { game_thread_id_ = std::this_thread::get_id(); }
    inline bool on_game_thread() const { return std::this_thread::get_id() == game_thread_id_.load(); }

    // Runs func on the game thread: inline when we are already on it, through
    // the timer queue otherwise. CK objects, sprites and BML gameplay state
    // must only be touched there - the network thread would race the renderer.
    // Scheduled calls keep their relative order, so a create queued before a
    // destroy still happens first.
    inline void run_on_game_thread(std::function<void()>&& func) const {
        if (on_game_thread())
            func();
        else
            schedule_sync_call(std::move(func));
    }

    static float distance_to_line_segment(const VxVector& begin, const VxVector& end, const VxVector& point);

    // only creates the thread on windows < 10
    static std::thread create_named_thread(const std::wstring& name, std::function<void()>&& func);
};
