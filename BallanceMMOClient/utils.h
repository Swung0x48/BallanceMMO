#pragma once

#include <cstdint>
#include <steam/steamnetworkingtypes.h>
#include "bml_includes.h"

class utils {
private:
    IBML* bml_;

public:
    utils(IBML* bml);

    static void md5_from_file(const std::string& path, uint8_t* result);
    static uint32_t get_system_dpi();

    inline HWND get_main_window() const { return static_cast<HWND>(bml_->GetCKContext()->GetMainWindow()); }

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

    static float distance_to_line_segment(const VxVector& begin, const VxVector& end, const VxVector& point);

    // only creates the thread on windows < 10
    static std::thread create_named_thread(const std::wstring& name, std::function<void()>&& func);
};
