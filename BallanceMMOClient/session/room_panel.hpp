#pragma once

// The ImGui room panel of the collision-overhaul room system
// (docs/rooms-and-sessions-protocol.md): the graphical front end for
// everything the /mmo room subcommands do - listing the open rooms, creating
// and joining one, the member roster with its ready flags, and the host's
// kick / close / start controls.
//
// Display and intent only: draw() never touches the network or the engine.
// It renders the room_state the client already holds and returns at most one
// action per call; BallanceMMOClient::OnProcess turns that action into the
// matching room_request. Outcomes (Accepted / Denied), timeouts and room
// events come back in through notify() and are shown in the feedback lines.
//
// open()/close()/toggle()/notify()/reset() may run on the network thread
// while draw() runs on the game thread, so every piece of UI state sits
// behind the panel's mutex. English/ASCII only, like the rest of the mod's
// in-game text. Drawn in BML+'s ImGui frame; the panel never calls
// NewFrame/Render itself.

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <message/room_state_msg.hpp>

namespace bmmo::retail {
    class room_panel {
    public:
        // One thing the user asked the panel to do this frame. valid=false
        // means "nothing". type names the room_request; room is the target
        // room (Join / Leave / Kick / Close / Start), target the kicked
        // member (Kick), mode the session kind (Start), name the room name
        // (Create, possibly empty).
        struct action {
            bool valid = false;
            bmmo::room::action type = bmmo::room::action::List;
            uint32_t room = 0;
            uint32_t target = 0;
            bmmo::room::mode mode = bmmo::room::mode::Physics;
            std::string name;
        };

        room_panel() = default;
        room_panel(const room_panel&) = delete;
        room_panel& operator=(const room_panel&) = delete;

        void open() {
            std::lock_guard lk(mtx_);
            visible_ = true;
        }

        void close() {
            std::lock_guard lk(mtx_);
            visible_ = false;
        }

        void toggle() {
            std::lock_guard lk(mtx_);
            visible_ = !visible_;
        }

        bool visible() const {
            std::lock_guard lk(mtx_);
            return visible_;
        }

        // One feedback line: a request outcome, a timeout, or a room event.
        // error=true draws it red, otherwise green.
        void notify(const std::string& text, bool error = false) {
            std::lock_guard lk(mtx_);
            if (feedback_.size() >= MAX_FEEDBACK)
                feedback_.pop_front();
            feedback_.push_back({text, error});
        }

        // Back to the freshly-connected state: closed, no feedback, the
        // create name cleared, the start mode back at its default.
        void reset() {
            std::lock_guard lk(mtx_);
            visible_ = false;
            feedback_.clear();
            create_name_[0] = '\0';
            start_mode_ = bmmo::room::mode::Physics;
        }

        // Game thread, inside BML+'s ImGui frame. Renders the panel when it
        // is visible and returns at most one action.
        action draw(const bmmo::room_state_msg& state, uint32_t own_id,
                    const std::vector<bmmo::room::action>& pending);

    private:
        struct feedback_line {
            std::string text;
            bool error = false;
        };

        static constexpr size_t MAX_FEEDBACK = 16;    // remembered
        static constexpr size_t SHOWN_FEEDBACK = 6;   // on screen at once

        static const char* action_name(bmmo::room::action a) {
            using room_action = bmmo::room::action;
            switch (a) {
                case room_action::List: return "list";
                case room_action::Create: return "create";
                case room_action::Join: return "join";
                case room_action::Leave: return "leave";
                case room_action::Ready: return "ready";
                case room_action::Unready: return "unready";
                case room_action::Start: return "start";
                case room_action::Kick: return "kick";
                case room_action::Close: return "close";
            }
            return "?";
        }

        static const char* mode_name(bmmo::room::mode m) {
            return m == bmmo::room::mode::Physics ? "physics" : "shadow";
        }

        static const char* phase_name(bmmo::room::phase p) {
            return p == bmmo::room::phase::Running ? "running" : "lobby";
        }

        // Short ASCII label for a member's map: the level number for an
        // original level, a short hash for a custom map (the panel is not
        // handed the server's map-name table).
        static std::string map_label(const bmmo::map& m) {
            switch (m.type) {
                case bmmo::map_type::OriginalLevel:
                    if (m.level >= 1 && m.level <= 13)
                        return "Level " + std::to_string(m.level);
                    return "Original " + std::to_string(m.level);
                case bmmo::map_type::CustomMap: {
                    const std::string hash = m.get_hash_string();
                    if (hash == std::string(sizeof(m.md5) * 2, '0'))
                        return "N/A";
                    return hash.substr(0, 12) + "..";
                }
                default:
                    return "-";
            }
        }

        mutable std::mutex mtx_;
        bool visible_ = false;                       // guarded by mtx_
        std::deque<feedback_line> feedback_;         // guarded by mtx_
        // +1 for the NUL; ImGui enforces the wire's 32-byte name cap.
        char create_name_[bmmo::room::MAX_ROOM_NAME + 1] = {};  // guarded by mtx_
        bmmo::room::mode start_mode_ = bmmo::room::mode::Physics;  // guarded by mtx_
    };

    inline room_panel::action room_panel::draw(const bmmo::room_state_msg& state, uint32_t own_id,
                                               const std::vector<bmmo::room::action>& pending) {
        using room_action = bmmo::room::action;
        using room_mode = bmmo::room::mode;
        std::lock_guard lk(mtx_);

        action result;
        // At most one action per frame: the first click wins, the rest wait
        // for their own frame.
        const auto submit = [&result](action a) {
            if (!result.valid) result = std::move(a);
        };
        const auto is_pending = [&pending](room_action type) {
            return std::find(pending.begin(), pending.end(), type) != pending.end();
        };

        if (!visible_)
            return result;

        const ImVec4 amber(1.0f, 0.827f, 0.416f, 1.0f);
        const ImVec4 green(0.373f, 0.824f, 0.478f, 1.0f);
        const ImVec4 red(0.95f, 0.36f, 0.33f, 1.0f);
        const ImVec4 grey(0.66f, 0.63f, 0.58f, 1.0f);

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Always);
        // Auto-height, capped so a long roster scrolls inside the window
        // instead of growing past the screen.
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 180.0f),
                                            ImVec2(FLT_MAX, io.DisplaySize.y * 0.85f));
        constexpr int STYLE_COLORS = 18;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.050f, 0.044f, 0.038f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.86f, 0.66f, 0.30f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.17f, 0.14f, 0.10f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.24f, 0.19f, 0.11f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, ImVec4(0.12f, 0.10f, 0.07f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.92f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.11f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.24f, 0.19f, 0.11f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.32f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.19f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.31f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.45f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.24f, 0.19f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.42f, 0.31f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.62f, 0.45f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(0.13f, 0.11f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ImVec4(0.42f, 0.32f, 0.17f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0.25f, 0.20f, 0.12f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        bool open = visible_;
        if (ImGui::Begin("BallanceMMO Rooms###bmmo_room_panel", &open, ImGuiWindowFlags_NoSavedSettings)) {
            // The feedback lines: outcomes, timeouts and room events, newest
            // at the bottom.
            if (!feedback_.empty()) {
                const size_t skip = feedback_.size() > SHOWN_FEEDBACK ? feedback_.size() - SHOWN_FEEDBACK : 0;
                for (size_t i = skip; i < feedback_.size(); ++i) {
                    const auto& line = feedback_[i];
                    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x);
                    ImGui::TextColored(line.error ? red : green, "%s", line.text.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::Separator();
            }

            // What the server still owes us an answer for (the client drops
            // a request after 5 s, and tells us through notify()).
            if (!pending.empty()) {
                std::string names;
                for (size_t i = 0; i < pending.size(); ++i) {
                    if (i > 0) names += ", ";
                    names += action_name(pending[i]);
                }
                ImGui::TextColored(amber, "Waiting for the server: %s (requests time out after 5 s).",
                                   names.c_str());
                ImGui::Separator();
            }

            const bool in_room = state.own_room != 0;

            if (!in_room) {
                // --- Create -------------------------------------------------
                ImGui::SeparatorText("Create a room");
                ImGui::Text("Name:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(280.0f);
                const bool enter = ImGui::InputTextWithHint("##room_name", "room name (optional)",
                                                            create_name_, sizeof(create_name_),
                                                            ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                ImGui::BeginDisabled(is_pending(room_action::Create));
                const bool clicked = ImGui::Button("Create");
                ImGui::EndDisabled();
                if ((clicked || enter) && !is_pending(room_action::Create)) {
                    action a;
                    a.valid = true;
                    a.type = room_action::Create;
                    a.name = create_name_;
                    submit(std::move(a));
                }
            } else {
                // --- Own room -----------------------------------------------
                const auto own_room_it = std::find_if(state.rooms.begin(), state.rooms.end(),
                        [&](const bmmo::room::room_info& r) { return r.id == state.own_room; });
                const auto self_it = std::find_if(state.members.begin(), state.members.end(),
                        [&](const bmmo::room::room_member& m) { return m.id == own_id; });
                const bool own_is_host = self_it != state.members.end() && self_it->is_host;
                const bool own_ready = self_it != state.members.end() && self_it->ready;

                if (own_room_it != state.rooms.end()) {
                    ImGui::SeparatorText("Your room");
                    ImGui::Text("Room #%u \"%s\" - %s, %s", state.own_room, own_room_it->name.c_str(),
                                phase_name(own_room_it->room_phase), mode_name(own_room_it->room_mode));
                } else {
                    ImGui::SeparatorText("Your room");
                    ImGui::Text("Room #%u", state.own_room);
                }

                const size_t ready_count = static_cast<size_t>(std::count_if(
                        state.members.begin(), state.members.end(),
                        [](const bmmo::room::room_member& m) { return m.ready; }));
                ImGui::TextColored(grey, "%zu/%zu ready", ready_count, state.members.size());

                if (ImGui::BeginTable("##room_members", 5,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("##kick", ImGuiTableColumnFlags_WidthFixed, 56.0f);
                    ImGui::TableHeadersRow();
                    for (const auto& m: state.members) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(m.name.c_str());
                        if (m.id == own_id) {
                            ImGui::SameLine();
                            ImGui::TextColored(grey, "(you)");
                        }
                        ImGui::TableNextColumn();
                        const std::string map = map_label(m.map);
                        ImGui::TextUnformatted(map.c_str());
                        ImGui::TableNextColumn();
                        if (m.ready)
                            ImGui::TextColored(green, "Ready");
                        else
                            ImGui::TextColored(grey, "Not ready");
                        ImGui::TableNextColumn();
                        if (m.is_host)
                            ImGui::TextColored(amber, "Host");
                        ImGui::TableNextColumn();
                        if (own_is_host && m.id != own_id) {
                            char kick_id[32];
                            std::snprintf(kick_id, sizeof(kick_id), "Kick##%u", m.id);
                            ImGui::BeginDisabled(is_pending(room_action::Kick));
                            if (ImGui::SmallButton(kick_id)) {
                                action a;
                                a.valid = true;
                                a.type = room_action::Kick;
                                a.room = state.own_room;
                                a.target = m.id;
                                submit(std::move(a));
                            }
                            ImGui::EndDisabled();
                        }
                    }
                    ImGui::EndTable();
                }

                // Ready toggle + leave (+ the host's close).
                ImGui::BeginDisabled(is_pending(room_action::Ready) || is_pending(room_action::Unready));
                if (ImGui::Button(own_ready ? "Unready" : "Ready")) {
                    action a;
                    a.valid = true;
                    a.type = own_ready ? room_action::Unready : room_action::Ready;
                    a.room = state.own_room;
                    submit(std::move(a));
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(is_pending(room_action::Leave));
                if (ImGui::Button("Leave room")) {
                    action a;
                    a.valid = true;
                    a.type = room_action::Leave;
                    a.room = state.own_room;
                    submit(std::move(a));
                }
                ImGui::EndDisabled();
                if (own_is_host) {
                    ImGui::SameLine();
                    ImGui::BeginDisabled(is_pending(room_action::Close));
                    if (ImGui::Button("Close room")) {
                        action a;
                        a.valid = true;
                        a.type = room_action::Close;
                        a.room = state.own_room;
                        submit(std::move(a));
                    }
                    ImGui::EndDisabled();

                    // The start row: one line with the Shadow/Physics
                    // segmented control (the selected half in the active
                    // color; Physics is the default) and the Start button.
                    ImGui::Text("Start:");
                    ImGui::SameLine();
                    const ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                    const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
                    const bool shadow = start_mode_ == room_mode::Shadow;
                    if (shadow) ImGui::PushStyleColor(ImGuiCol_Button, active);
                    if (ImGui::SmallButton("Shadow")) start_mode_ = room_mode::Shadow;
                    if (shadow) ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, inner);
                    if (!shadow) ImGui::PushStyleColor(ImGuiCol_Button, active);
                    if (ImGui::SmallButton("Physics")) start_mode_ = room_mode::Physics;
                    if (!shadow) ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(is_pending(room_action::Start));
                    if (ImGui::Button("Start")) {
                        action a;
                        a.valid = true;
                        a.type = room_action::Start;
                        a.room = state.own_room;
                        a.mode = start_mode_;
                        submit(std::move(a));
                    }
                    ImGui::EndDisabled();
                }
            }

            // --- The open rooms -------------------------------------------
            ImGui::SeparatorText("Open rooms");
            ImGui::BeginDisabled(is_pending(room_action::List));
            if (ImGui::Button("Refresh")) {
                action a;
                a.valid = true;
                a.type = room_action::List;
                submit(std::move(a));
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(grey, "%zu room%s open", state.rooms.size(),
                               state.rooms.size() == 1 ? "" : "s");

            if (state.rooms.empty()) {
                ImGui::TextColored(grey, "No rooms are open.%s",
                                   in_room ? "" : " Create one above.");
            } else if (ImGui::BeginTable("##room_list", 6,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 48.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed, 56.0f);
                ImGui::TableHeadersRow();
                for (const auto& r: state.rooms) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("#%u", r.id);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.name.empty() ? "-" : r.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", r.member_count, r.capacity);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(phase_name(r.room_phase));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(mode_name(r.room_mode));
                    ImGui::TableNextColumn();
                    if (!in_room) {
                        const bool full = r.member_count >= r.capacity;
                        char join_id[32];
                        std::snprintf(join_id, sizeof(join_id), "Join##%u", r.id);
                        ImGui::BeginDisabled(is_pending(room_action::Join) || full);
                        if (ImGui::SmallButton(join_id)) {
                            action a;
                            a.valid = true;
                            a.type = room_action::Join;
                            a.room = r.id;
                            submit(std::move(a));
                        }
                        ImGui::EndDisabled();
                        if (full && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("This room is full.");
                    } else if (r.id == state.own_room) {
                        ImGui::TextColored(grey, "yours");
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(STYLE_COLORS);
        // The window's close button is another way to close the panel.
        if (!open)
            visible_ = false;
        return result;
    }
}
