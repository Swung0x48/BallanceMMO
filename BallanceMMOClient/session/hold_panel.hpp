#pragma once

// FULL step AHN: the wait panel of the retail non-blocking anchor hold
// (session-client-core-design.md 2.3 "nonblocking_hold", As built (step AHN)).
//
// While the session waits at its anchor the world is frozen (the behaviour
// loop does not run) but the level keeps rendering; this panel is drawn in
// that render's ImGui frame (BML+'s context) - the user's chosen first
// version (.local-tools/work/AHN/imgui_wait_mockup.svg): a window centred
// near the top, semi-transparent dark with a gold border; "BallanceMMO" and
// "Level N · M players · lag X ms"; the current state line with a live
// countdown and a progress bar; every member with a check mark once it has
// confirmed; a separator; the previous state line and the keys line in grey.
// English only; gone at the start (nothing draws it outside the hold).
//
// Display only: every decision stays the core's.  The panel reads what the
// host is told - the core's start-handshake notices (hooks.notice), the
// server's SessionStartStatus (counts, the missing members' names in its
// text) and a SessionStartAt the core took (its notice came while it was
// handled) - and the SessionStart's member list.

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
            previous_.clear();
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
            if (!line_.text.empty()) previous_ = line_.text;
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
        std::string previous_;
        uint8_t confirmed_ = 0, total_ = 0;
        bool have_counts_ = false;
    };

    inline void hold_panel::draw(clock_type::time_point now, int ping_ms) const {
        const ImGuiIO& io = ImGui::GetIO();
        // 480 px up to a 1280 px wide screen (the mockup's), larger above it;
        // never smaller (ahn_r2: at 800 x 600 a shrunk panel overlapped its
        // own lines - BML+'s font does not shrink with it).
        const float scale = std::clamp(io.DisplaySize.x / 1280.0f, 1.0f, 2.0f);
        const float width = 480.0f * scale;
        const ImVec4 gold(0.91f, 0.70f, 0.29f, 0.85f);
        const ImVec4 title(0.953f, 0.831f, 0.541f, 1.0f);
        const ImVec4 grey(0.541f, 0.588f, 0.702f, 1.0f);
        const ImVec4 text(0.914f, 0.933f, 0.973f, 1.0f);
        const ImVec4 amber(1.0f, 0.827f, 0.416f, 1.0f);
        const ImVec4 green(0.373f, 0.824f, 0.478f, 1.0f);
        const ImVec4 red(0.95f, 0.36f, 0.33f, 1.0f);

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.064f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.078f, 0.102f, 0.149f, 0.86f));
        ImGui::PushStyleColor(ImGuiCol_Border, gold);
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.227f, 0.271f, 0.376f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.169f, 0.200f, 0.275f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.91f, 0.70f, 0.29f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 8.0f * scale));
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##bmmo_anchor_hold", nullptr, flags)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetWindowPos();
            const float line_h = ImGui::GetTextLineHeightWithSpacing();
            // The right edge of the content, window-local (SameLine's offset).
            const float right = width - ImGui::GetStyle().WindowPadding.x;
            // The title band.
            draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + line_h + 10.0f * scale),
                                ImGui::GetColorU32(ImVec4(0.165f, 0.208f, 0.314f, 0.95f)), 7.0f * scale,
                                ImDrawFlags_RoundCornersTop);
            ImGui::TextColored(title, "BallanceMMO");
            char header[128];
            std::snprintf(header, sizeof(header), "Level %d \xC2\xB7 %zu player%s \xC2\xB7 lag %d ms", level_,
                          members_.size(), members_.size() == 1 ? "" : "s", ping_ms);
            const float header_w = ImGui::CalcTextSize(header).x;
            ImGui::SameLine(right - header_w);
            ImGui::TextColored(grey, "%s", header);
            ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));

            // The current state line, its live countdown and the progress bar.
            float fraction = 0.0f;
            if (line_.countdown) {
                const double left_ms = std::max(0.0, std::chrono::duration<double, std::milli>(line_.instant - now).count());
                const double span_ms = std::chrono::duration<double, std::milli>(line_.instant - line_.from).count();
                fraction = span_ms > 0.0 ? static_cast<float>(1.0 - left_ms / span_ms) : 1.0f;
                const std::string line = line_.text + " - starting in about";
                char ms[32];
                std::snprintf(ms, sizeof(ms), "%lld ms", static_cast<long long>(left_ms + 0.5));
                const float ms_w = ImGui::CalcTextSize(ms).x;
                const float room = right - ImGui::GetStyle().WindowPadding.x - ms_w - 12.0f * scale;
                if (ImGui::CalcTextSize(line.c_str()).x <= room) {
                    ImGui::TextColored(text, "%s", line.c_str());
                    ImGui::SameLine(right - ms_w);
                } else {
                    // A long line (a postponement's): wrapped, the number below it.
                    ImGui::PushStyleColor(ImGuiCol_Text, text);
                    ImGui::TextWrapped("%s", line.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SetCursorPosX(right - ms_w);
                }
                ImGui::TextColored(amber, "%s", ms);
            } else {
                size_t confirmed = 0;
                for (const auto& m: members_) confirmed += m.state == mark::confirmed ? 1 : 0;
                const size_t total = have_counts_ ? total_ : members_.size();
                if (have_counts_) confirmed = confirmed_;
                fraction = total ? static_cast<float>(confirmed) / static_cast<float>(total) : 0.0f;
                ImGui::PushStyleColor(ImGuiCol_Text, text);
                if (line_.text == "Waiting for the other players to confirm the start")
                    ImGui::TextWrapped("%s (%zu/%zu)", line_.text.c_str(), confirmed, total);
                else
                    ImGui::TextWrapped("%s", line_.text.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-1.0f, 10.0f * scale), "");
            ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));

            // The members, two columns, a check mark once confirmed.
            const float column = (width - 28.0f * scale) * 0.5f;
            const float mark_w = ImGui::GetFontSize();
            for (size_t i = 0; i < members_.size(); ++i) {
                const auto& m = members_[i];
                if (i % 2 == 1) ImGui::SameLine(14.0f * scale + column);
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
            ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, grey);
            if (!previous_.empty()) ImGui::TextWrapped("Earlier: %s", previous_.c_str());
            ImGui::TextWrapped("Keys are ignored until the start. The level begins for everyone at the same instant.");
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
    }
}
