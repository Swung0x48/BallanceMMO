#pragma once

// The ImGui room panel of the collision-overhaul room system
// (docs/rooms-and-sessions-protocol.md): the graphical front end for
// everything the /mmo room subcommands do - listing the open rooms, creating
// and joining one, the member roster with its ready flags, and the host's
// kick / close / start controls.
//
// A drawer on the left or right screen edge.  Its tab is always there, from
// the game's start and offline too (the panel then offers to connect):
// a click on it (or "/mmo panel", or the panel's own close button)
// unfolds and folds the panel, and a drag carries the tab, which docks to the
// nearer edge when let go.  The tab stays inside a safe range clear of the
// screen's top and bottom, so it can never be left off screen or under the
// game's own HUD; the range is re-applied every frame, so a resolution change
// cannot strand it either.  "visible" below means unfolded.
//
// The panel is as wide as its widest row needs in BML+'s font and no wider
// (the window auto-resizes; a list's fitted columns take their text's width,
// its name column what is left), so it covers no more of the game than it
// has to.  Few words, with room around them.  Among the rooms the top is the
// server's: a status line (physics with a check or a cross; its version, its
// protocol or why it has none in the tooltip) over two bars, the players
// online and the physics rooms running against their limits.  In a room the
// top is the room's: its name in the title bar, its status ("Waiting (Level
// 3, Physics)"), its host's settings while it waits - Set Map and the kind of
// session - and a bar of the members ready.  While its session runs the
// bar and the members' marks count the votes to restart it instead, and the
// buttons are the vote and - the host's, once all have voted - Restart.  A member is a
// row with a check once ready and a crown for the host, a room a row with its
// players; the refresh and the kick are drawn buttons.  The button to press
// next (Connect, Ready, Start) has a row of its own and is lit; Close is red.
// What a mark or a disabled button means is in its tooltip, not in a line of
// its own.
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
#include <cmath>
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
        // room (Join / Leave / Kick / Close / Start / SetMap / SetMode),
        // target the kicked member (Kick), mode the session kind (Start /
        // SetMode), name the room name (Create, possibly empty).  SetMap's
        // map is the sender's to fill in: the level the game is in.
        struct action {
            bool valid = false;
            bmmo::room::action type = bmmo::room::action::List;
            uint32_t room = 0;
            uint32_t target = 0;
            bmmo::room::mode mode = bmmo::room::mode::Physics;
            std::string name;
            // Not a room request: the offline panel's Connect asks for the
            // server list.
            bool connect = false;
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

        // Where the tab is docked: the screen edge, and how far down its safe
        // range it sits (0 = top, 1 = bottom).  A fraction, so it means the
        // same at every resolution.
        struct dock {
            bool right = true;
            float position = 0.3f;
        };

        void set_dock(dock d) {
            std::lock_guard lk(mtx_);
            dock_ = {d.right, std::clamp(d.position, 0.0f, 1.0f)};
        }

        // The dock the player just dragged the tab to, once (for the config).
        std::optional<dock> take_moved_dock() {
            std::lock_guard lk(mtx_);
            if (!moved_) return std::nullopt;
            moved_ = false;
            return dock_;
        }

        // Back to the freshly-connected state: folded, the create name
        // cleared.  The dock stays.
        void reset() {
            std::lock_guard lk(mtx_);
            visible_ = false;
            create_name_[0] = '\0';
            server_info_.reset();
        }

        // Game thread, inside BML+'s ImGui frame. Renders the tab, and the
        // panel while it is unfolded, and returns at most one action.
        // in_session: this client runs a physics session (see the Ready button).
        // online: the players on the server, this one included.
        action draw(const bmmo::room_state_msg& state, uint32_t own_id,
                    const std::vector<bmmo::room::action>& pending, bool connected, bool in_session,
                    size_t online);

    private:
        // The tab's safe range: the share of the screen's height kept clear
        // above it (BML+'s own lines) and below it (the game's HUD, the
        // message area and the command bar).
        static constexpr float kSafeTop = 0.10f, kSafeBottom = 0.18f;
        static constexpr float kSlideSeconds = 0.15f;

        // The tab at `pos`: a click folds / unfolds, a drag carries it.
        void draw_tab(const ImVec2& pos, const ImVec2& size, float safe_top, float safe_span);

        static const char* mode_name(bmmo::room::mode m) {
            return m == bmmo::room::mode::Physics ? "physics" : "shadow";
        }

        static const char* mode_title(bmmo::room::mode m) {
            return m == bmmo::room::mode::Physics ? "Physics" : "Shadow";
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
        // The drawer; guarded by mtx_ like the rest.
        dock dock_;
        float shown_ = 0.0f;          // the slide: 0 folded .. 1 unfolded
        ImVec2 panel_size_;           // the last frame's: the slide's length, and the fit on screen
        ImVec2 grab_;                 // where in the tab the mouse holds it
        bool dragging_ = false;
        bool reopen_ = false;         // the panel was unfolded when the drag began
        bool moved_ = false;          // a drag ended: the dock is to be saved
    };

    inline void room_panel::draw_tab(const ImVec2& pos, const ImVec2& size, float safe_top, float safe_span) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##bmmo_room_tab", nullptr, flags)) {
            const bool clicked = ImGui::InvisibleButton("##tab", size);
            const bool held = ImGui::IsItemActive();
            const bool hot = held || ImGui::IsItemHovered();
            if (ImGui::IsItemActivated())
                grab_ = ImVec2(io.MousePos.x - pos.x, io.MousePos.y - pos.y);
            if (clicked && !dragging_)
                visible_ = !visible_;
            if (held && !dragging_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                dragging_ = true;
                reopen_ = visible_;   // the panel folds away while its tab is carried
                visible_ = false;
            }
            if (dragging_) {
                // Carried: never out of the safe range, and docked to the
                // nearer edge when let go (draw() places it from dock_).
                if (safe_span > 0.0f)
                    dock_.position = std::clamp((io.MousePos.y - grab_.y - safe_top) / safe_span, 0.0f, 1.0f);
                dock_.right = pos.x + size.x * 0.5f > io.DisplaySize.x * 0.5f;
                if (!held) {
                    dragging_ = false;
                    visible_ = reopen_;
                    moved_ = true;
                }
            }
            if (hot && !held)
                ImGui::SetTooltip("BallanceMMO rooms\nClick to %s, drag to move", visible_ ? "fold" : "unfold");

            // Rounded on the side away from its edge; all around while carried.
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const ImVec2 max(pos.x + size.x, pos.y + size.y);
            const float font = ImGui::GetFontSize();
            const ImDrawFlags corners = dragging_ ? ImDrawFlags_RoundCornersAll
                : dock_.right ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight;
            const ImVec4 fill = hot ? ImVec4(0.24f, 0.19f, 0.11f, 1.0f) : ImVec4(0.12f, 0.10f, 0.07f, 0.85f);
            const ImU32 gold = ImGui::GetColorU32(ImVec4(0.86f, 0.66f, 0.30f, 0.90f));
            const ImU32 amber = ImGui::GetColorU32(ImVec4(1.0f, 0.827f, 0.416f, 1.0f));
            draw_list->AddRectFilled(pos, max, ImGui::GetColorU32(fill), 7.0f, corners);
            draw_list->AddRect(pos, max, gold, 7.0f, corners, 1.5f);
            // "MMO" down the tab, then a chevron pointing where a click moves the panel.
            const float center = pos.x + size.x * 0.5f;
            float y = pos.y + (size.y - font * 4.2f) * 0.5f;
            for (const char* letter: {"M", "M", "O"}) {
                draw_list->AddText(ImVec2(center - ImGui::CalcTextSize(letter).x * 0.5f, y), amber, letter);
                y += font;
            }
            y += font * 0.7f;
            const float arm = (dock_.right == visible_ ? 1.0f : -1.0f) * font * 0.22f;
            const ImVec2 chevron[3] = {ImVec2(center - arm, y - font * 0.36f), ImVec2(center + arm, y),
                                       ImVec2(center - arm, y + font * 0.36f)};
            draw_list->AddPolyline(chevron, 3, hot ? amber : gold, ImDrawFlags_None, 2.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    inline room_panel::action room_panel::draw(const bmmo::room_state_msg& state, uint32_t own_id,
                                               const std::vector<bmmo::room::action>& pending, bool connected,
                                               bool in_session, size_t online) {
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

        const ImVec4 amber(1.0f, 0.827f, 0.416f, 1.0f);
        const ImVec4 green(0.373f, 0.824f, 0.478f, 1.0f);
        const ImVec4 grey(0.66f, 0.63f, 0.58f, 1.0f);

        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 display = io.DisplaySize;
        const float ui_scale = std::clamp(display.y / 768.0f * 1.2f, 1.0f, 3.0f);
        // The tab, sized by BML+'s font and never too small to hit, and the
        // range its top edge may take.
        const float font = ImGui::GetFontSize();
        const ImVec2 tab(std::max(font * 1.7f, 26.0f), std::max(font * 5.6f, 88.0f));
        const float safe_top = display.y * kSafeTop;
        const float safe_span = std::max(0.0f, display.y * (1.0f - kSafeBottom) - tab.y - safe_top);
        const float tab_y = safe_top + dock_.position * safe_span;
        // The panel slides out of its edge and pushes the tab ahead of it.
        const float step = io.DeltaTime / kSlideSeconds;
        shown_ = visible_ ? std::min(1.0f, shown_ + step) : std::max(0.0f, shown_ - step);
        const float out = panel_size_.x * shown_ * shown_ * (3.0f - 2.0f * shown_);
        const float tab_x = dragging_ ? std::clamp(io.MousePos.x - grab_.x, 0.0f, std::max(0.0f, display.x - tab.x))
                          : dock_.right ? display.x - out - tab.x : out;
        draw_tab(ImVec2(tab_x, tab_y), tab, safe_top, safe_span);
        if (shown_ <= 0.0f)
            return result;

        // The panel's top is level with the tab's where it fits on the screen;
        // the side on its edge is the one held (the pivot), whatever width
        // this frame's content turns out to need.
        const float edge = display.y * 0.04f;
        ImGui::SetNextWindowPos(ImVec2(dock_.right ? display.x + panel_size_.x - out : out - panel_size_.x,
                                       std::clamp(tab_y, edge, std::max(edge, display.y - edge - panel_size_.y))),
                                ImGuiCond_Always, ImVec2(dock_.right ? 1.0f : 0.0f, 0.0f));
        // Where this player stands: in which room, as what.
        const bool in_room = connected && state.own_room != 0;
        const auto own_room_it = std::find_if(state.rooms.begin(), state.rooms.end(),
                [&](const bmmo::room::room_info& r) { return in_room && r.id == state.own_room; });
        const bool listed = own_room_it != state.rooms.end();
        const bool running = listed && own_room_it->room_phase == bmmo::room::phase::Running;
        const auto self_it = std::find_if(state.members.begin(), state.members.end(),
                [&](const bmmo::room::room_member& m) { return m.id == own_id; });
        const bool own_is_host = self_it != state.members.end() && self_it->is_host;
        const bool own_ready = self_it != state.members.end() && self_it->ready;
        const size_t ready_count = static_cast<size_t>(std::count_if(
                state.members.begin(), state.members.end(),
                [](const bmmo::room::room_member& m) { return m.ready; }));
        const bool all_ready = ready_count == state.members.size();
        // While a session runs the count that matters is the votes to restart
        // it, of the members in the session: one in the room but out of its
        // session (it joined the room late, or finished) has no vote.
        const size_t session_count = static_cast<size_t>(std::count_if(
                state.members.begin(), state.members.end(),
                [](const bmmo::room::room_member& m) { return m.in_session; }));
        const size_t vote_count = static_cast<size_t>(std::count_if(
                state.members.begin(), state.members.end(),
                [](const bmmo::room::room_member& m) { return m.in_session && m.vote; }));
        const bool all_voted = vote_count == session_count;
        const bool own_vote = self_it != state.members.end() && self_it->vote;
        const bool own_in_session = self_it != state.members.end() && self_it->in_session;
        // The room's settings, its host's to change while it waits: the map
        // its session will run (none until a Set Map or the first start - the
        // room is then "in the lobby") and the kind of session.
        const bmmo::map room_map = in_room ? state.own_map : bmmo::map{};
        const bool has_map = bmmo::room::map_set(room_map);
        const room_mode kind = listed ? own_room_it->room_mode : room_mode::Physics;
        // Where the room stands: "Waiting (Lobby)", "Waiting (Level 3,
        // Physics)", "Running (Level 2, Shadow)".
        char status[96];
        if (has_map)
            std::snprintf(status, sizeof(status), "%s (%s, %s)", running ? "Running" : "Waiting",
                          map_label(room_map).c_str(), mode_title(kind));
        else
            std::snprintf(status, sizeof(status), "%s (%s)", running ? "Running" : "Waiting",
                          running ? mode_title(kind) : "Lobby");
        // The title says where the panel is: offline, among the rooms, or in
        // one (that room's number and name).
        char title[160];
        if (!connected)
            std::snprintf(title, sizeof(title), "BallanceMMO###bmmo_room_panel");
        else if (!in_room)
            std::snprintf(title, sizeof(title), "Rooms###bmmo_room_panel");
        else
            std::snprintf(title, sizeof(title), "#%u %s###bmmo_room_panel", state.own_room,
                          listed ? own_room_it->name.c_str() : "");

        // Sized to its content: at least the offline notice's line, at most
        // what leaves the tab on screen, and capped in height so a long
        // roster scrolls inside the window.  Its spacing is its own, measured
        // in the font (BML+'s is tight for a panel this narrow) and in whole
        // pixels: a fraction of one makes the auto-resize come up short and
        // show a scrollbar.
        const auto em = [font](float n) { return std::round(font * n); };
        const ImVec2 padding(em(0.7f), em(0.55f));
        // Wide enough for the title beside its close button, too.
        const float min_width = std::max(ImGui::CalcTextSize("Not connected").x,
                                         ImGui::CalcTextSize(title, nullptr, true).x) + padding.x * 2.0f + font * 2.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f),
                                            ImVec2(std::max(min_width, display.x * 0.92f - tab.x), display.y * 0.85f));
        const ImVec4 button(0.24f, 0.19f, 0.12f, 1.0f);
        const ImVec4 button_hovered(0.42f, 0.31f, 0.15f, 1.0f);
        const ImVec4 accent(0.62f, 0.45f, 0.20f, 1.0f);
        const ImVec4 accent_hovered(0.74f, 0.54f, 0.24f, 1.0f);
        const ImVec4 frame(0.13f, 0.11f, 0.08f, 1.0f);
        constexpr int STYLE_COLORS = 13;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.050f, 0.044f, 0.038f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.86f, 0.66f, 0.30f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.17f, 0.14f, 0.10f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.24f, 0.19f, 0.11f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.92f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.24f, 0.19f, 0.11f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.32f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, button);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.03f));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0.25f, 0.20f, 0.12f, 1.0f));
        constexpr int STYLE_VARS = 7;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em(0.45f), em(0.4f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(0.5f), em(0.2f)));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(em(0.5f), em(0.25f)));
        // Held by its edge: the tab moves it, and its close button folds it.
        bool open = true;
        constexpr ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
        // A list without a grid: a row per entry, the name taking the room
        // the fitted columns leave.
        constexpr ImGuiTableFlags list_flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_SizingStretchProp;
        const bool drawn = ImGui::Begin(title, &open, panel_flags);
        panel_size_ = ImGui::GetWindowSize();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const float stroke = std::max(1.5f, font * 0.08f);

        // Marks drawn in the font's box at the cursor, in place of words;
        // each says what it is in its tooltip.
        // framed: on a row of buttons, where text sits lower by the frame's padding.
        const auto mark = [&](const char* tip, bool framed = false) {
            const float lead = framed ? ImGui::GetStyle().FramePadding.y : 0.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(font, font + lead * 2.0f));
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return ImVec2(at.x, at.y + lead);
        };
        const auto at_em = [font](const ImVec2& at, float x, float y) { return ImVec2(at.x + font * x, at.y + font * y); };
        const ImU32 red = ImGui::GetColorU32(ImVec4(0.95f, 0.42f, 0.38f, 1.0f));
        const auto check = [&](const ImVec2& at) {
            const ImVec2 line[3] = {at_em(at, 0.15f, 0.55f), at_em(at, 0.40f, 0.80f), at_em(at, 0.85f, 0.22f)};
            draw_list->AddPolyline(line, 3, ImGui::GetColorU32(green), ImDrawFlags_None, stroke);
        };
        const auto cross = [&](const ImVec2& at) {
            draw_list->AddLine(at_em(at, 0.25f, 0.27f), at_em(at, 0.75f, 0.77f), red, stroke);
            draw_list->AddLine(at_em(at, 0.25f, 0.77f), at_em(at, 0.75f, 0.27f), red, stroke);
        };
        // A check once ready, a ring until then.
        const auto tick = [&](bool on, const char* yes, const char* no) {
            const ImVec2 at = mark(on ? yes : no);
            if (on)
                check(at);
            else
                draw_list->AddCircle(at_em(at, 0.5f, 0.5f), font * 0.26f, ImGui::GetColorU32(grey), 12, stroke * 0.75f);
        };
        // The host's crown.
        const auto host_mark = [&] {
            const ImVec2 at = mark("Host");
            const ImVec2 crown[7] = {at_em(at, 0.12f, 0.80f), at_em(at, 0.12f, 0.30f), at_em(at, 0.32f, 0.52f),
                                     at_em(at, 0.50f, 0.22f), at_em(at, 0.68f, 0.52f), at_em(at, 0.88f, 0.30f),
                                     at_em(at, 0.88f, 0.80f)};
            draw_list->AddConcavePolyFilled(crown, 7, ImGui::GetColorU32(amber));
        };
        // A square button whose label is drawn over it by the caller.
        const auto icon_button = [&](const char* id, float side, const char* tip) {
            const bool clicked = ImGui::Button(id, ImVec2(side, side));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
            return clicked;
        };
        // A key button - the thing to do next - has a row of its own, is
        // taller than the rest, and is lit while it is that thing.
        const auto key_button = [&](const char* label, float width, bool lit) {
            ImGui::PushStyleColor(ImGuiCol_Button, lit ? accent : button);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lit ? accent_hovered : button_hovered);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(0.5f), em(0.4f)));
            const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            return clicked;
        };
        // The width such a row and the bars share: two bars side by side.
        const float bar_width = ImGui::CalcTextSize("players: 00/00").x + em(0.5f) * 2.0f;
        // In a room, the status line's width too - with the longer kind's
        // name, so that a change of kind does not change the panel's width.
        const float kind_slack = (has_map || running) && kind == room_mode::Shadow
                ? ImGui::CalcTextSize("Physics").x - ImGui::CalcTextSize("Shadow").x : 0.0f;
        // The host's Set Map sits at the end of that line while the room waits.
        const bool settings = own_is_host && !running;
        const float set_map_width = ImGui::CalcTextSize("Set Map").x + em(0.5f) * 2.0f;
        const float row_width = std::max(bar_width * 2.0f + em(0.45f),
                                         in_room ? ImGui::CalcTextSize("Room status: ").x + ImGui::CalcTextSize(status).x
                                                       + kind_slack + (settings ? em(0.45f) + set_map_width : 0.0f)
                                                 : 0.0f);
        const auto item_center = [] {
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            return ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        };
        // Air between the panel's groups.
        const auto section_gap = [&] { ImGui::Dummy(ImVec2(0.0f, em(0.15f))); };

        if (drawn && !connected) {
            // The way in, a row of its own as wide as the panel at its
            // narrowest: the server list, which takes the screen, so the
            // panel folds.
            ImGui::TextColored(amber, "Not connected");
            if (key_button("Connect", min_width - padding.x * 2.0f, true)) {
                result.connect = true;
                visible_ = false;
            }
        } else if (drawn) {
            if (!pending.empty()) {
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

            // --- At a glance: the server, or in a room the room ----------
            // The server: what it offers, each with a check or a cross (the
            // version, the protocol or the reason in the tooltip), and how
            // full it is as two bars.  The room: where it stands, the kind of
            // settings its host has while it waits (so up here and not among
            // the buttons).
            const auto offer = [&](const char* label, bool on) {
                ImGui::BeginGroup();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine(0.0f, em(0.2f));
                const ImVec2 at = mark(nullptr, true);
                if (on) check(at); else cross(at);
                ImGui::EndGroup();
            };
            const auto bar = [&](const char* label, size_t value, size_t limit, float width, const ImVec4& fill,
                                 const char* tip) {
                char text[48];
                std::snprintf(text, sizeof(text), "%s: %zu/%zu", label, value, limit);
                const ImVec2 size(width, ImGui::GetFrameHeight());
                const ImVec2 lo = ImGui::GetCursorScreenPos(), hi(lo.x + size.x, lo.y + size.y);
                ImGui::Dummy(size);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                draw_list->AddRectFilled(lo, hi, ImGui::GetColorU32(frame), 4.0f);
                // The filled share, from the left: square where it ends short of the bar's end.
                const float share = limit ? std::min(1.0f, static_cast<float>(value) / static_cast<float>(limit)) : 0.0f;
                if (share > 0.0f)
                    draw_list->AddRectFilled(lo, ImVec2(lo.x + std::max(size.x * share, 4.0f), hi.y),
                                             ImGui::GetColorU32(fill), 4.0f,
                                             share < 1.0f ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersAll);
                draw_list->AddText(ImVec2(lo.x + (size.x - ImGui::CalcTextSize(text).x) * 0.5f,
                                          lo.y + ImGui::GetStyle().FramePadding.y),
                                   ImGui::GetColorU32(ImGuiCol_Text), text);
            };
            if (!in_room) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(grey, "Server status:");
                if (server_info_) {
                    const auto& info = *server_info_;
                    ImGui::SameLine();
                    offer("Physics", info.physics_available);
                    if (ImGui::IsItemHovered()) {
                        if (info.physics_available)
                            ImGui::SetTooltip("Server %s\nPhysics protocol %u", info.version.c_str(),
                                              info.physics_protocol);
                        else
                            ImGui::SetTooltip("Server %s\nNo physics sessions: %s", info.version.c_str(),
                                              info.physics_unavailable_reason.c_str());
                    }
                    if (!info.rooms_enabled) {
                        ImGui::SameLine();
                        offer("Rooms", false);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rooms are off on this server");
                    }
                } else {
                    ImGui::SameLine();
                    ImGui::Text("%zu online", online);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(is_pending(room_action::List));
                if (icon_button("##refresh", ImGui::GetFrameHeight(), "Refresh")) {
                    action a;
                    a.valid = true;
                    a.type = room_action::List;
                    submit(std::move(a));
                }
                ImGui::EndDisabled();
                {
                    // A turning arrow: most of a circle and a head at its end.
                    const ImVec2 c = item_center();
                    const float r = ImGui::GetFrameHeight() * 0.26f, end = 5.5f;
                    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
                    draw_list->PathArcTo(c, r, 0.9f, end, 16);
                    draw_list->PathStroke(ink, ImDrawFlags_None, stroke * 0.75f);
                    const ImVec2 out(std::cos(end), std::sin(end)), along(-std::sin(end), std::cos(end));
                    const ImVec2 tip(c.x + out.x * r, c.y + out.y * r);
                    draw_list->AddTriangleFilled(ImVec2(tip.x + out.x * r * 0.5f, tip.y + out.y * r * 0.5f),
                                                 ImVec2(tip.x - out.x * r * 0.5f, tip.y - out.y * r * 0.5f),
                                                 ImVec2(tip.x + along.x * r * 0.8f, tip.y + along.y * r * 0.8f), ink);
                }
                if (server_info_) {
                    const auto& info = *server_info_;
                    // The physics rooms in use: the listed rooms whose physics session runs.
                    const auto in_use = std::count_if(state.rooms.begin(), state.rooms.end(),
                            [](const bmmo::room::room_info& r) {
                                return r.room_phase == bmmo::room::phase::Running && r.room_mode == room_mode::Physics;
                            });
                    bar("players", online, info.maximum_members, bar_width, accent,
                        "Players online, against a room's size");
                    ImGui::SameLine();
                    bar("rooms", static_cast<size_t>(in_use), info.maximum_physics_rooms, bar_width, accent,
                        "Physics rooms running, against the server's limit");
                }
                section_gap();
            } else {
                if (settings) ImGui::AlignTextToFramePadding();
                ImGui::TextColored(grey, "Room status:");
                ImGui::SameLine();
                if (running)
                    ImGui::TextColored(green, "%s", status);
                else
                    ImGui::TextUnformatted(status);
                if (settings) {
                    // The host's settings while the room waits.  Set Map, at
                    // the end of the line it changes, takes the level this
                    // game is in now (none outside one); under it the kind
                    // of session, two joined buttons, the room's lit.
                    const float half = std::floor(row_width * 0.5f);
                    ImGui::SameLine(padding.x + row_width - set_map_width);
                    ImGui::BeginDisabled(is_pending(room_action::SetMap));
                    if (ImGui::Button("Set Map", ImVec2(set_map_width, 0.0f))) {
                        action a;
                        a.valid = true;
                        a.type = room_action::SetMap;
                        a.room = state.own_room;
                        submit(std::move(a));
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Start on the level you are in now");
                    const auto kind_button = [&](const char* label, room_mode mode, float width) {
                        const bool chosen = kind == mode;
                        ImGui::PushStyleColor(ImGuiCol_Button, chosen ? accent : frame);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chosen ? accent : button_hovered);
                        if (ImGui::Button(label, ImVec2(width, 0.0f)) && !chosen) {
                            action a;
                            a.valid = true;
                            a.type = room_action::SetMode;
                            a.room = state.own_room;
                            a.mode = mode;
                            submit(std::move(a));
                        }
                        ImGui::PopStyleColor(2);
                    };
                    ImGui::BeginDisabled(is_pending(room_action::SetMode));
                    kind_button("Shadow", room_mode::Shadow, half);
                    ImGui::SameLine(0.0f, 0.0f);
                    kind_button("Physics", room_mode::Physics, row_width - half);
                    ImGui::EndDisabled();
                }
                section_gap();
            }


            if (!in_room) {
                // --- Create: the name and the button, one row -------------
                ImGui::SetNextItemWidth(ImGui::CalcTextSize("Room name").x * 1.8f + ImGui::GetStyle().FramePadding.x * 2.0f);
                const bool enter = ImGui::InputTextWithHint("##room_name", "Room name", create_name_,
                                                            sizeof(create_name_), ImGuiInputTextFlags_EnterReturnsTrue);
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
                // --- Own room: who is in it and how many are ready, then what to do
                ImGui::Separator();
                const ImVec4 agreed(0.16f, 0.52f, 0.28f, 1.0f);
                if (running)
                    bar("votes", vote_count, session_count, row_width, agreed, "Votes to restart, of those in the session");
                else
                    bar("ready", ready_count, state.members.size(), row_width, agreed, "Members ready");

                // A member a row: the ready mark, the name (this player's row
                // tinted), the host's crown, its map if another; the host's kick at the
                // end, only where there is someone for this host to kick.
                const bool kick_column = own_is_host && state.members.size() > 1;
                if (ImGui::BeginTable("##room_members", kick_column ? 3 : 2, list_flags)) {
                    ImGui::TableSetupColumn("##player", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("##map", ImGuiTableColumnFlags_WidthFixed);
                    if (kick_column) ImGui::TableSetupColumn("##kick", ImGuiTableColumnFlags_WidthFixed);
                    for (const auto& m: state.members) {
                        ImGui::TableNextRow();
                        if (m.id == own_id)
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                   ImGui::GetColorU32(ImVec4(amber.x, amber.y, amber.z, 0.10f)));
                        ImGui::TableNextColumn();
                        if (running && !m.in_session) {
                            // In the room, out of its session: a dash, no vote.
                            const ImVec2 at = mark("Not in the session");
                            draw_list->AddLine(at_em(at, 0.28f, 0.52f), at_em(at, 0.72f, 0.52f), ImGui::GetColorU32(grey),
                                               stroke * 0.75f);
                        } else if (running)
                            tick(m.vote, "Voted to restart", "No vote");
                        else
                            tick(m.ready, "Ready", "Not ready");
                        ImGui::SameLine();
                        ImGui::TextUnformatted(m.name.c_str());
                        if (m.is_host) {
                            ImGui::SameLine();
                            host_mark();
                        }
                        ImGui::TableNextColumn();
                        // The member's map only where it is not the room's.
                        if (has_map && m.map != room_map) {
                            const std::string map = map_label(m.map);
                            ImGui::TextColored(amber, "%s", map == "-" ? "no level" : map.c_str());
                        }
                        if (kick_column && m.id != own_id) {
                            ImGui::TableNextColumn();
                            char kick_id[32];
                            std::snprintf(kick_id, sizeof(kick_id), "##kick%u", m.id);
                            ImGui::BeginDisabled(is_pending(room_action::Kick));
                            if (icon_button(kick_id, font, "Kick")) {
                                action a;
                                a.valid = true;
                                a.type = room_action::Kick;
                                a.room = state.own_room;
                                a.target = m.id;
                                submit(std::move(a));
                            }
                            ImGui::EndDisabled();
                            cross(ImGui::GetItemRectMin());
                        }
                    }
                    ImGui::EndTable();
                }

                section_gap();
                // Ready toggle + leave (+ the host's close).  While the room's
                // physics session runs without us (we joined late, finished,
                // or the start left us behind) the button is "Join session":
                // the server takes a Ready of such a member into the session.
                // Lit while it is the thing left to do.  A running session
                // has nothing else to ready and nothing to start: those two
                // buttons are the waiting room's.
                const bool join_session = !in_session && running && own_room_it->room_mode == room_mode::Physics;
                if (!running || join_session) {
                    ImGui::BeginDisabled(is_pending(room_action::Ready) || is_pending(room_action::Unready));
                    if (key_button(join_session ? "Join session" : own_ready ? "Unready" : "Ready", row_width,
                                   join_session || !own_ready)) {
                        action a;
                        a.valid = true;
                        a.type = (own_ready && !join_session) ? room_action::Unready : room_action::Ready;
                        a.room = state.own_room;
                        submit(std::move(a));
                        if (join_session) visible_ = false;   // the session starts; the panel folds away
                    }
                    ImGui::EndDisabled();
                }
                if (running) {
                    // A running session is restarted by its host once every
                    // member in it has voted for that: the vote - a session
                    // member's - and the host's Restart, lit when the votes
                    // are in.
                    if (own_in_session) {
                        ImGui::BeginDisabled(is_pending(room_action::VoteRestart) || is_pending(room_action::UnvoteRestart));
                        if (key_button(own_vote ? "Unvote" : "Vote restart", row_width, false)) {
                            action a;
                            a.valid = true;
                            a.type = own_vote ? room_action::UnvoteRestart : room_action::VoteRestart;
                            a.room = state.own_room;
                            submit(std::move(a));
                        }
                        ImGui::EndDisabled();
                    }
                    if (own_is_host) {
                        ImGui::BeginDisabled(is_pending(room_action::Restart) || !all_voted);
                        if (key_button("Restart", row_width, all_voted)) {
                            action a;
                            a.valid = true;
                            a.type = room_action::Restart;
                            a.room = state.own_room;
                            submit(std::move(a));
                        }
                        ImGui::EndDisabled();
                        if (!all_voted && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Waiting for %zu vote(s)", session_count - vote_count);
                    }
                }
                if (own_is_host && !running) {
                    // The host's Start, lit once every member is ready.
                    // Until then there is no start: the server refuses it too
                    // (action::Start), and a member that is not ready must
                    // not be able to hold the start.
                    ImGui::BeginDisabled(is_pending(room_action::Start) || !all_ready);
                    if (key_button("Start", row_width, all_ready)) {
                        action a;
                        a.valid = true;
                        a.type = room_action::Start;
                        a.room = state.own_room;
                        a.mode = kind;
                        submit(std::move(a));
                    }
                    ImGui::EndDisabled();
                    if (!all_ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Waiting for %zu member(s) to be ready", state.members.size() - ready_count);
                }
                section_gap();
                // The ways out fill the last row: Leave, and beside it the
                // host's Close, red because it ends the room for everyone.
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float way_out = own_is_host ? std::floor((row_width - gap) * 0.5f) : row_width;
                ImGui::BeginDisabled(is_pending(room_action::Leave));
                if (ImGui::Button("Leave", ImVec2(way_out, 0.0f))) {
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
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.17f, 0.15f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.23f, 0.20f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.29f, 0.25f, 1.0f));
                    if (ImGui::Button("Close", ImVec2(row_width - gap - way_out, 0.0f))) {
                        action a;
                        a.valid = true;
                        a.type = room_action::Close;
                        a.room = state.own_room;
                        submit(std::move(a));
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Close the room for everyone");
                }
            }

            // --- The rooms to join; in a room, only if there are others ----
            const size_t others = state.rooms.size() - (own_room_it != state.rooms.end() ? 1 : 0);
            if (others > 0) ImGui::Separator();
            if (others == 0) {
                if (!in_room) ImGui::TextColored(grey, "No rooms yet");
            } else if (ImGui::BeginTable("##room_list", in_room ? 3 : 4, list_flags)) {
                ImGui::TableSetupColumn("##room", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##players", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed);
                if (!in_room) ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed);
                for (const auto& r: state.rooms) {
                    if (in_room && r.id == state.own_room) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("#%u %s", r.id, r.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(grey, "%u/%u", r.member_count, r.capacity);
                    ImGui::TableNextColumn();
                    // Only a running room has a state worth a word.
                    if (r.room_phase == bmmo::room::phase::Running)
                        ImGui::TextColored(green, "%s", mode_name(r.room_mode));
                    if (in_room) continue;
                    ImGui::TableNextColumn();
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
                        ImGui::SetTooltip("This room is full");
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(STYLE_VARS);
        ImGui::PopStyleColor(STYLE_COLORS);
        // The window's close button is another way to fold the panel.
        if (!open)
            visible_ = false;
        return result;
    }
}
