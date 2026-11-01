#pragma once

// FULL step AHN: the wait panel of the retail non-blocking anchor hold
// (session-client-core-design.md 2.3 "nonblocking_hold", As built (step AHN)).
//
// While the session waits at its anchor the world is frozen (the behaviour
// loop does not run) but the level keeps rendering; this panel is drawn in
// that render's ImGui frame (BML+'s context) - the user's chosen first
// version (.local-tools/work/AHN/imgui_wait_mockup.svg): a window centred
// near the top, semi-transparent dark with a gold border; "BallanceMMO" and
// "Level N" in its title band; the current state line with a live
// countdown and a progress bar; every member with a check mark once it has
// confirmed (AHNP: the grey previous-state and keys lines removed, the user's
// mockup v2, .local-tools/work/AHN/imgui_wait_mockup_v2.svg). English only;
// gone at the start (nothing draws it outside the hold).
//
// Display only: every decision stays the core's.  The panel reads what the
// host is told - the core's start-handshake notices (hooks.notice), the
// server's SessionStartStatus (counts, the missing members' names in its
// text; only one the core showed - step SAXC: a status after this member's
// own start is stale) and a SessionStartAt the core took (its notice came
// while it was handled) - and the SessionStart's member list.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

namespace bmmo::retail {
    class hold_panel {
    public:
        using clock_type = std::chrono::steady_clock;

        enum class mark : uint8_t { pending, confirmed, refused };
        struct member {
            std::string name;
            bool own = false;
            mark state = mark::pending;
        };

        // A SessionStart: the level and the members (own first in its join
        // order, as the SessionStart lists them).
        void reset(int level, std::vector<member> members) {
            *this = hold_panel{};
            level_ = level;
            members_ = std::move(members);
        }

        // The anchor: the hold begins with the anchor's own line.
        void armed(int level, const std::string& line, clock_type::time_point now) {
            level_ = level;
            line_ = {};
            set_line(line, now);
        }

        // One start-handshake line shown to the player (the core's notice or
        // the server's status text, which reaches the panel as the core's
        // notice of it).
        void notice(const std::string& text, clock_type::time_point now) {
            set_line(text, now);
            if (starts_with(text, "all players confirmed") || starts_with(text, "new start time")
                    || starts_with(text, "another player has started")) {
                for (auto& m: members_)
                    if (m.state != mark::refused) m.state = mark::confirmed;
            } else if (starts_with(text, "waiting for the other players to confirm the start.")) {
                // This member's own scheduled assignment came and was
                // acknowledged (client_core on_session_assign).
                for (auto& m: members_)
                    if (m.own && m.state == mark::pending) m.state = mark::confirmed;
            }
        }

        // A SessionStartStatus (before the core's notice of its text): the
        // counts and the members it names as missing or refused.
        void status(uint8_t phase, uint8_t confirmed, uint8_t total, const std::string& text) {
            confirmed_ = confirmed;
            total_ = total;
            have_counts_ = total > 0;
            std::string names;
            bool refused = false;
            if (phase == 0 || phase == 1) {   // waiting_assign / waiting_start: "...; missing: a, b)"
                const auto at = text.find("missing: ");
                if (at != std::string::npos) {
                    names = text.substr(at + 9);
                    if (!names.empty() && names.back() == ')') names.pop_back();
                }
            } else if (phase == 2) {          // postponed: "start postponed (k of 3): a, b did not confirm ..."
                const auto from = text.find("): ");
                const auto to = text.find(" did not confirm");
                if (from != std::string::npos && to != std::string::npos && to > from + 3)
                    names = text.substr(from + 3, to - from - 3);
            } else if (phase == 3) {          // refused: "a, b refused: ..."
                const auto to = text.find(" refused:");
                if (to != std::string::npos) names = text.substr(0, to);
                refused = true;
            }
            if (names.empty()) return;
            const auto named = split_names(names);
            for (auto& m: members_) {
                const bool in = std::find(named.begin(), named.end(), m.name) != named.end();
                if (refused) {
                    if (in) m.state = mark::refused;
                } else if (m.state != mark::refused) {
                    m.state = in ? mark::pending : mark::confirmed;
                }
            }
        }

        // A SessionStartAt the core took (its notice came while it was
        // handled): the precise instant of the start, on this clock.
        void start_instant(clock_type::time_point instant) {
            line_.countdown = true;
            line_.instant = instant;
        }

        void draw(clock_type::time_point now, int ping_ms) const;

    private:
        struct state_line {
            std::string text;        // shown, without its "starting in about N ms"
            bool countdown = false;
            clock_type::time_point from{}, instant{};
        };

        static bool starts_with(const std::string& text, const char* prefix) {
            return text.rfind(prefix, 0) == 0;
        }

        static std::vector<std::string> split_names(const std::string& names) {
            std::vector<std::string> out;
            size_t at = 0;
            while (at <= names.size()) {
                const auto comma = names.find(", ", at);
                out.push_back(names.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                if (comma == std::string::npos) break;
                at = comma + 2;
            }
            return out;
        }

        // "x - starting in about N ms." / "x; starting in about N ms": the
        // countdown runs from now to now + N (a SessionStartAt the core took
        // replaces the instant with its own, start_instant).
        void set_line(const std::string& text, clock_type::time_point now) {
            state_line next;
            std::string shown = text;
            while (!shown.empty() && (shown.back() == '.' || shown.back() == ' ')) shown.pop_back();
            const auto at = shown.find("starting in about ");
            if (at != std::string::npos) {
                long long ms = 0;
                if (std::sscanf(shown.c_str() + at + 18, "%lld", &ms) == 1) {
                    next.countdown = true;
                    next.from = now;
                    next.instant = now + std::chrono::milliseconds(std::max(0LL, ms));
                }
                shown = shown.substr(0, at);
                while (!shown.empty() && (shown.back() == ' ' || shown.back() == '-' || shown.back() == ';'))
                    shown.pop_back();
            } else if (starts_with(shown, "waiting for the other players to confirm the start")) {
                shown = "waiting for the other players to confirm the start";
            }
            if (!shown.empty()) shown[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(shown[0])));
            next.text = shown;
            line_ = next;
        }

        int level_ = 0;
        std::vector<member> members_;
        state_line line_;
        uint8_t confirmed_ = 0, total_ = 0;
        bool have_counts_ = false;
    };

    inline void hold_panel::draw(clock_type::time_point now, int ping_ms) const {
        const ImGuiIO& io = ImGui::GetIO();
        // The layout is measured in BML+'s font, whatever its size (AHNP:
        // ahn_r2's fixed 480 px was too narrow for it, so the title row and
        // the countdown overlapped): the panel is as wide as its widest row
        // needs, at least 480 px (scaled up above a 1280 px wide screen) and
        // at most 92 % of the screen; a row that still does not fit wraps.
        // AHNP (the user: "界面缩放可以大一些"): the panel's text is drawn larger than BML+'s font, scaled
        // with the screen's height (1.3x at 768 lines, 1.1x at 600), and every measure below follows it.
        const float font_scale = std::clamp(io.DisplaySize.y / 768.0f * 1.3f, 1.1f, 2.2f);
        const float scale = font_scale;
        const float pad = 14.0f * scale;
        const float gap = 24.0f * scale;
        // A warm near-black with a gold border, like the game's own message area and menus (the user: the dark
        // blue "有点突兀").
        const ImVec4 gold(0.86f, 0.66f, 0.30f, 0.90f);
        const ImVec4 title(0.96f, 0.82f, 0.50f, 1.0f);
        const ImVec4 grey(0.66f, 0.63f, 0.58f, 1.0f);
        const ImVec4 text(0.94f, 0.92f, 0.88f, 1.0f);
        const ImVec4 amber(1.0f, 0.827f, 0.416f, 1.0f);
        const ImVec4 green(0.373f, 0.824f, 0.478f, 1.0f);
        const ImVec4 red(0.95f, 0.36f, 0.33f, 1.0f);

        // What each row says, and how wide it is.
        const char* const name = "BallanceMMO";
        // The title band holds the name and the level only (AHNP: the user found "Level N · M players · ping X ms"
        // too crowded there); the member list shows the players, and the ping sits small at the panel's foot.
        char header[32];
        std::snprintf(header, sizeof(header), "Level %d", level_);
        char ping[32];
        std::snprintf(ping, sizeof(ping), "ping %d ms", ping_ms);
        // Measured before Begin, in the unscaled font: every width is multiplied by the panel's font scale.
        const auto measure = [font_scale](const char* s) { return ImGui::CalcTextSize(s).x * font_scale; };
        const float name_w = measure(name);
        const float header_w = measure(header);

        float fraction = 0.0f;
        std::string line;
        char ms[32] = "";
        if (line_.countdown) {
            const double left_ms = std::max(0.0, std::chrono::duration<double, std::milli>(line_.instant - now).count());
            const double span_ms = std::chrono::duration<double, std::milli>(line_.instant - line_.from).count();
            fraction = span_ms > 0.0 ? static_cast<float>(1.0 - left_ms / span_ms) : 1.0f;
            line = line_.text + " - starting in about";
            std::snprintf(ms, sizeof(ms), "%lld ms", static_cast<long long>(left_ms + 0.5));
        } else {
            size_t confirmed = 0;
            for (const auto& m: members_) confirmed += m.state == mark::confirmed ? 1 : 0;
            const size_t total = have_counts_ ? total_ : members_.size();
            if (have_counts_) confirmed = confirmed_;
            fraction = total ? static_cast<float>(confirmed) / static_cast<float>(total) : 0.0f;
            char counts[32];
            std::snprintf(counts, sizeof(counts), " (%zu/%zu)", confirmed, total);
            line = line_.text == "Waiting for the other players to confirm the start" ? line_.text + counts : line_.text;
        }
        const float line_w = measure(line.c_str());
        // The countdown's widest value keeps the panel from changing width as it counts down.
        const float ms_w = line_.countdown ? measure("0000 ms") : 0.0f;

        const float mark_w = ImGui::GetFontSize() * font_scale;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        float member_w = 0.0f;
        for (const auto& m: members_)
            member_w = std::max(member_w, mark_w + spacing + measure((m.name + (m.own ? " (you)" : "")).c_str()));

        float need = std::max(name_w + gap + header_w, line_w + (line_.countdown ? gap + ms_w : 0.0f));
        need = std::max(need, members_.size() > 1 ? 2.0f * member_w + gap : member_w);
        const float width = std::clamp(need + 2.0f * pad, 480.0f * scale, std::max(480.0f * scale, io.DisplaySize.x * 0.92f));
        const float content = width - 2.0f * pad;
        const float right = width - pad;   // the content's right edge, window-local (SameLine's offset)
        const bool header_fits = name_w + gap + header_w <= content;

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.064f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.050f, 0.044f, 0.038f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_Border, gold);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.22f, 0.19f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.86f, 0.66f, 0.30f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 8.0f * scale));
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##bmmo_anchor_hold", nullptr, flags)) {
            ImGui::SetWindowFontScale(font_scale);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetWindowPos();
            const float line_h = ImGui::GetTextLineHeightWithSpacing();
            // The title band: one row, or two when the header has to go under the name.
            const float band_h = (header_fits ? line_h : 2.0f * line_h) + 10.0f * scale;
            draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + band_h),
                                ImGui::GetColorU32(ImVec4(0.17f, 0.14f, 0.10f, 0.95f)), 7.0f * scale,
                                ImDrawFlags_RoundCornersTop);
            ImGui::TextColored(title, "%s", name);
            if (header_fits) ImGui::SameLine(right - header_w);
            ImGui::TextColored(grey, "%s", header);
            ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));

            // The current state line, its live countdown at the right, and the progress bar.
            ImGui::PushTextWrapPos(right);
            if (line_.countdown && line_w + gap + ms_w <= content) {
                ImGui::TextColored(text, "%s", line.c_str());
                ImGui::SameLine(right - ImGui::CalcTextSize(ms).x);
                ImGui::TextColored(amber, "%s", ms);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, text);
                ImGui::TextWrapped("%s", line.c_str());
                ImGui::PopStyleColor();
                if (line_.countdown) {
                    // A line too long for one row (a postponement's): the number under it, at the right.
                    ImGui::SetCursorPosX(right - ImGui::CalcTextSize(ms).x);
                    ImGui::TextColored(amber, "%s", ms);
                }
            }
            ImGui::PopTextWrapPos();
            ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-1.0f, 10.0f * scale), "");
            ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));

            // The members: two columns when two names fit side by side, else one; a check mark once
            // confirmed, a grey ring while waiting, a red cross if refused.
            const bool two_columns = members_.size() > 1 && 2.0f * member_w + gap <= content;
            const float second = pad + std::max(member_w + gap, content * 0.5f);
            for (size_t i = 0; i < members_.size(); ++i) {
                const auto& m = members_[i];
                if (two_columns && i % 2 == 1) ImGui::SameLine(second);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetFontSize();
                if (m.state == mark::confirmed) {
                    const ImVec2 pts[3] = {ImVec2(at.x + h * 0.15f, at.y + h * 0.55f), ImVec2(at.x + h * 0.40f, at.y + h * 0.80f),
                                           ImVec2(at.x + h * 0.85f, at.y + h * 0.22f)};
                    draw->AddPolyline(pts, 3, ImGui::GetColorU32(green), 2.2f * scale);
                } else if (m.state == mark::refused) {
                    draw->AddLine(ImVec2(at.x + h * 0.2f, at.y + h * 0.2f), ImVec2(at.x + h * 0.8f, at.y + h * 0.8f),
                                  ImGui::GetColorU32(red), 2.0f * scale);
                    draw->AddLine(ImVec2(at.x + h * 0.8f, at.y + h * 0.2f), ImVec2(at.x + h * 0.2f, at.y + h * 0.8f),
                                  ImGui::GetColorU32(red), 2.0f * scale);
                } else {
                    draw->AddCircle(ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.28f, ImGui::GetColorU32(grey), 12,
                                    1.5f * scale);
                }
                ImGui::Dummy(ImVec2(mark_w, h));
                ImGui::SameLine();
                ImGui::TextColored(m.state == mark::pending ? grey : ImVec4(0.812f, 0.839f, 0.902f, 1.0f), "%s%s",
                                   m.name.c_str(), m.own ? " (you)" : "");
            }
            // The foot: the ping, small and grey, at the right.
            ImGui::SetWindowFontScale(font_scale * 0.8f);
            ImGui::SetCursorPosX(right - ImGui::CalcTextSize(ping).x);
            ImGui::TextColored(grey, "%s", ping);
            ImGui::SetWindowFontScale(font_scale);
        }
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(4);
    }
}
