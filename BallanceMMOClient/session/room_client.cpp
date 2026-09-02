// Client-side handling of the collision-overhaul room system
// (docs/rooms-and-sessions-protocol.md). The room_request messages are sent
// from the /mmo room subcommand in BallanceMMOClient.cpp; here we render the
// room_state roster / list and the room_event notifications.
#include "../BallanceMMOClient.h"

#include <format>

// Network thread. Stores the latest room state; prints the list only when the
// user asked for it (so the frequent background pushes stay silent).
void BallanceMMOClient::handle_room_state(bmmo::room_state_msg msg) {
    bool print = false;
    {
        std::lock_guard lk(room_state_mtx_);
        last_room_state_ = std::move(msg);
        if (room_list_requested_) {
            room_list_requested_ = false;
            print = true;
        }
    }
    if (print)
        print_room_list();
}

void BallanceMMOClient::print_room_list() {
    std::lock_guard lk(room_state_mtx_);
    const auto& rooms = last_room_state_.rooms;
    if (rooms.empty()) {
        SendIngameMessage("No rooms are open. Use /mmo room create [name] to open one.");
        return;
    }
    SendIngameMessage(std::format("{} room{} open:", rooms.size(), rooms.size() == 1 ? "" : "s"));
    for (const auto& r : rooms) {
        const char* mode = (r.mode == bmmo::room::mode::Physics) ? "physics" : "shadow";
        const char* phase = (r.phase == bmmo::room::phase::Running) ? "running" : "lobby";
        SendIngameMessage(std::format("  #{} \"{}\" - {}/{} [{}, {}] host: {}",
                r.id, r.name, r.member_count, r.capacity, mode, phase, get_username(r.host)));
    }
}

void BallanceMMOClient::print_room_status() {
    std::lock_guard lk(room_state_mtx_);
    if (last_room_state_.own_room == 0) {
        SendIngameMessage("You are not in a room. Use /mmo room list or /mmo room create.");
        return;
    }
    const auto& members = last_room_state_.members;
    SendIngameMessage(std::format("You are in room #{} ({} member{}):",
            last_room_state_.own_room, members.size(), members.size() == 1 ? "" : "s"));
    for (const auto& m : members) {
        SendIngameMessage(std::format("  {}{}{}", m.name,
                m.is_host ? " [host]" : "", m.ready ? " [ready]" : ""));
    }
}

// Network thread. One human-readable line per room event.
void BallanceMMOClient::handle_room_event(const bmmo::room_event_msg& msg) {
    using event_type = bmmo::room::event_type;
    switch (msg.type) {
        case event_type::RequestAccepted:
            // The authoritative room_state that follows shows the result; keep
            // the ack quiet for actions with an obvious visible effect.
            break;
        case event_type::RequestDenied:
            SendIngameMessage(std::format("Room request denied: {}.",
                    bmmo::room::error_string(msg.error)), bmmo::ansi::BrightRed);
            break;
        case event_type::PlayerJoined:
            SendIngameMessage(std::format("{} joined the room.", get_username(msg.actor)),
                    bmmo::ansi::BrightGreen);
            break;
        case event_type::PlayerLeft:
            SendIngameMessage(std::format("{} left the room.", get_username(msg.subject)));
            break;
        case event_type::HostChanged:
            SendIngameMessage(std::format("{} is now the room host.", get_username(msg.actor)),
                    bmmo::ansi::BrightYellow);
            break;
        case event_type::Kicked:
            SendIngameMessage("You were removed from the room.", bmmo::ansi::BrightRed);
            break;
        case event_type::RoomClosed:
            SendIngameMessage("The room was closed.", bmmo::ansi::BrightYellow);
            break;
        case event_type::ReadyChanged:
            // The refreshed roster already reflects the change; stay quiet.
            break;
        case event_type::SessionStarting:
            SendIngameMessage("The room session is starting.", bmmo::ansi::BrightGreen);
            break;
        case event_type::SessionEnded:
            SendIngameMessage(std::format("The room session ended{}{}.",
                    msg.reason.empty() ? "" : ": ", msg.reason), bmmo::ansi::BrightYellow);
            break;
        default:
            break;
    }
}
