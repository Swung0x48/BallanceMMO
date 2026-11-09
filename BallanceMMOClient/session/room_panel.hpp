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
// matching room_request. Outcomes, timeouts and room events use BML's message
// area rather than a second event log inside this window.
//
// open()/close()/toggle()/reset() may run on the network thread while draw()
// runs on the game thread, so every piece of UI state sits behind the panel's
// mutex. English/ASCII only, like the rest of the mod's in-game text. Drawn in
// BML+'s ImGui frame; the panel never calls NewFrame/Render itself.

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <message/room_state_msg.hpp>
#include <message/login_accepted_v4_msg.hpp>

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

        void set_server_info(const std::optional<bmmo::login_server_info>& info) {
            std::lock_guard lk(mtx_);
            server_info_ = info;
        }

        // Back to the freshly-connected state: closed, the create name
        // cleared, the start mode back at its default.
        void reset() {
            std::lock_guard lk(mtx_);
            visible_ = false;
            create_name_[0] = '\0';
            start_mode_ = bmmo::room::mode::Physics;
            server_info_.reset();
        }

        // Game thread, inside BML+'s ImGui frame. Renders the panel when it
        // is visible and returns at most one action.
        // in_session: this client runs a physics session (see the Ready button).
        action draw(const bmmo::room_state_msg& state, uint32_t own_id,
                    const std::vector<bmmo::room::action>& pending, bool connected, bool in_session);

    private:
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
        std::optional<bmmo::login_server_info> server_info_;  // LoginAcceptedV4's; guarded by mtx_
        // +1 for the NUL; ImGui enforces the wire's 32-byte name cap.
        char create_name_[bmmo::room::MAX_ROOM_NAME + 1] = {};  // guarded by mtx_
        bmmo::room::mode start_mode_ = bmmo::room::mode::Physics;  // guarded by mtx_
    };

    inline room_panel::action room_panel::draw(const bmmo::room_state_msg& state, uint32_t own_id,
                                               const std::vector<bmmo::room::action>& pending, bool connected,
                                               bool in_session) {
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
        const ImVec4 grey(0.66f, 0.63f, 0.58f, 1.0f);

        const ImGuiIO& io = ImGui::GetIO();
        const float ui_scale = std::clamp(io.DisplaySize.y / 768.0f * 1.2f, 1.0f, 3.0f);
        const float window_width = std::min((connected ? 620.0f : 420.0f) * ui_scale, io.DisplaySize.x * 0.92f);
        ImGui::SetNextWindowSize(ImVec2(window_width, 0.0f), ImGuiCond_Always);
        // Auto-height, capped so a long roster scrolls inside the window
        // instead of growing past the screen. Disconnected needs only its notice.
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * ui_scale, (connected ? 180.0f : 70.0f) * ui_scale),
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
            if (!connected) {
                ImGui::TextColored(amber, "Disconnected");
                ImGui::TextWrapped("Connect to a server to use this panel.");
                ImGui::End();
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(STYLE_COLORS);
                if (!open) visible_ = false;
                return result;
            }
            if (!pending.empty()) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                const float radius = ImGui::GetFontSize() * 0.35f;
                const float title_height = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
                const ImVec2 center(ImGui::GetWindowPos().x + ImGui::GetWindowWidth()
                                        - title_height - ImGui::GetStyle().WindowPadding.x - radius,
                                    ImGui::GetWindowPos().y + title_height * 0.5f);
                const float angle = static_cast<float>(ImGui::GetTime() * 4.0);
                const ImVec4 faint(amber.x, amber.y, amber.z, 0.25f);
                draw_list->AddCircle(center, radius, ImGui::GetColorU32(faint), 16, ui_scale);
                draw_list->PathArcTo(center, radius, angle, angle + 4.71f, 16);
                draw_list->PathStroke(ImGui::GetColorU32(amber), false, 1.2f * ui_scale);
            }
            if (server_info_) {
                const auto& info = *server_info_;
                ImGui::TextColored(amber, "Server %s", info.version.c_str());
                if (info.physics_available)
                    ImGui::TextColored(green, "Physics sessions: available (protocol %u)", info.physics_protocol);
                else {
                    ImGui::PushStyleColor(ImGuiCol_Text, amber);
                    ImGui::TextWrapped("Physics sessions: not available - %s", info.physics_unavailable_reason.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::TextWrapped("Rooms: %s, up to %u players; %u physics room%s at a time",
                                   info.rooms_enabled ? "on" : "off", info.maximum_members,
                                   info.maximum_physics_rooms, info.maximum_physics_rooms == 1 ? "" : "s");
            }
            const bool in_room = state.own_room != 0;

            if (!in_room) {
                // --- Create -------------------------------------------------
                ImGui::SeparatorText("Create a room");
                ImGui::Text("Name:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(280.0f * ui_scale);
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
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f * ui_scale);
                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 60.0f * ui_scale);
                    ImGui::TableSetupColumn("##kick", ImGuiTableColumnFlags_WidthFixed, 56.0f * ui_scale);
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

                // Ready toggle + leave (+ the host's close).  While the room's
                // physics session runs without us (we joined late, finished,
                // or the start left us behind) the button is "Join session":
                // the server takes a Ready of such a member into the session.
                const bool join_session = !in_session && own_room_it != state.rooms.end()
                        && own_room_it->room_phase == bmmo::room::phase::Running
                        && own_room_it->room_mode == room_mode::Physics;
                ImGui::BeginDisabled(is_pending(room_action::Ready) || is_pending(room_action::Unready));
                if (ImGui::Button(join_session ? "Join session" : own_ready ? "Unready" : "Ready")) {
                    action a;
                    a.valid = true;
                    a.type = (own_ready && !join_session) ? room_action::Unready : room_action::Ready;
                    a.room = state.own_room;
                    submit(std::move(a));
                    if (join_session) visible_ = false;   // the session starts; the panel goes away
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

                    // The mode selector has its own row: two Selectables in a
                    // two-column table, then the Start button below them.
                    ImGui::Text("Mode:");
                    const bool shadow = start_mode_ == room_mode::Shadow;
                    if (ImGui::BeginTable("##start_mode", 2,
                            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV
                                | ImGuiTableFlags_SizingStretchSame)) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable("Shadow", shadow, 0, ImVec2(0.0f, ImGui::GetFrameHeight())))
                            start_mode_ = room_mode::Shadow;
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable("Physics", !shadow, 0, ImVec2(0.0f, ImGui::GetFrameHeight())))
                            start_mode_ = room_mode::Physics;
                        ImGui::EndTable();
                    }
                    // Every member ready, or no start: the server refuses it too
                    // (action::Start), and a member that is not ready must not be
                    // able to hold the start.
                    const bool all_ready = ready_count == state.members.size();
                    ImGui::BeginDisabled(is_pending(room_action::Start) || !all_ready);
                    if (ImGui::Button("Start")) {
                        action a;
                        a.valid = true;
                        a.type = room_action::Start;
                        a.room = state.own_room;
                        a.mode = start_mode_;
                        submit(std::move(a));
                    }
                    ImGui::EndDisabled();
                    if (!all_ready) {
                        ImGui::SameLine();
                        ImGui::TextColored(grey, "waiting for %zu member(s) to be ready",
                                           state.members.size() - ready_count);
                    }
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
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 48.0f * ui_scale);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 64.0f * ui_scale);
                ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 64.0f * ui_scale);
                ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 64.0f * ui_scale);
                ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed, 56.0f * ui_scale);
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
