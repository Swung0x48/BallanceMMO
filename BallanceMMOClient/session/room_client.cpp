// Client-side handling of the collision-overhaul room system
// (docs/rooms-and-sessions-protocol.md). The room_request messages are sent
// from the /mmo room subcommand in BallanceMMOClient.cpp; here we render the
// room_state roster / list and the room_event notifications.
#include "../BallanceMMOClient.h"
#include "session_journal_client.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <vector>

namespace {
    // How a subcommand is spelled in the messages we print back to the user.
    const char* room_action_name(bmmo::room::action action) {
        using action_type = bmmo::room::action;
        switch (action) {
            case action_type::List: return "list";
            case action_type::Create: return "create";
            case action_type::Join: return "join";
            case action_type::Leave: return "leave";
            case action_type::Ready: return "ready";
            case action_type::Unready: return "unready";
            case action_type::Start: return "start";
            case action_type::Kick: return "kick";
            case action_type::Close: return "close";
        }
        return "?";
    }
}

// Called right after a room_request goes out, so its outcome can name the
// command that caused it.
void BallanceMMOClient::push_room_request(const pending_room_request& request) {
    std::lock_guard lk(room_state_mtx_);
    // A server that never answers must not make this grow without bound; the
    // oldest entry is the one whose outcome we have clearly missed.
    if (pending_room_requests_.size() >= MAX_PENDING_ROOM_REQUESTS)
        pending_room_requests_.pop_front();
    auto entry = request;
    entry.sent = std::chrono::steady_clock::now();
    pending_room_requests_.push_back(entry);
}

// Game thread, every frame. Reports the commands whose outcome never arrived
// (a server older than protocol 1.3 answers neither Ready/Unready, List nor
// Close) and drops them, so the next command's outcome is not read as theirs.
void BallanceMMOClient::process_room_requests() {
    std::vector<pending_room_request> timed_out;
    {
        std::lock_guard lk(room_state_mtx_);
        const auto now = std::chrono::steady_clock::now();
        while (!pending_room_requests_.empty()
                && now - pending_room_requests_.front().sent > ROOM_REQUEST_TIMEOUT) {
            timed_out.push_back(pending_room_requests_.front());
            pending_room_requests_.pop_front();
        }
    }
    for (const auto& request: timed_out)
        SendIngameMessage(std::format("Error: the server did not answer \"/mmo room {}\".",
                room_action_name(request.action)), bmmo::ansi::BrightRed);
}

std::optional<BallanceMMOClient::pending_room_request> BallanceMMOClient::pop_room_request() {
    std::lock_guard lk(room_state_mtx_);
    if (pending_room_requests_.empty())
        return std::nullopt;
    auto request = pending_room_requests_.front();
    pending_room_requests_.pop_front();
    return request;
}

void BallanceMMOClient::reset_room_state() {
    std::lock_guard lk(room_state_mtx_);
    last_room_state_ = {};
    pending_room_requests_.clear();
    room_list_requested_ = room_status_requested_ = false;
}

// Network thread. Stores the latest room state; prints the list or the roster
// only when a command asked for it (so the frequent background pushes stay
// silent).
void BallanceMMOClient::handle_room_state(bmmo::room_state_msg msg) {
    bool print_list = false, print_status = false;
    {
        std::lock_guard lk(room_state_mtx_);
        last_room_state_ = std::move(msg);
        print_list = std::exchange(room_list_requested_, false);
        print_status = std::exchange(room_status_requested_, false);
    }
    if (print_list)
        print_room_list();
    if (print_status)
        print_room_status();
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
        const char* mode = (r.room_mode == bmmo::room::mode::Physics) ? "physics" : "shadow";
        const char* phase = (r.room_phase == bmmo::room::phase::Running) ? "running" : "lobby";
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

// "X is ready (2/3 ready)." from the roster we already hold: the server pushes
// the refreshed room_state before the ReadyChanged events, so by now it carries
// the new flag. Empty if the player is not in our roster.
std::string BallanceMMOClient::room_ready_text(HSteamNetConnection player) {
    std::lock_guard lk(room_state_mtx_);
    const auto& members = last_room_state_.members;
    const auto member = std::ranges::find(members, player, &bmmo::room::room_member::id);
    if (member == members.end())
        return {};
    const auto ready = std::ranges::count_if(members, [](const auto& m) { return m.ready; });
    const bool self = (player == db_.get_client_id());
    return std::format("{} {}{} ready ({}/{} ready).", self ? "You" : member->name.c_str(),
            self ? "are" : "is", member->ready ? "" : " not", ready, members.size());
}

// The outcome of one of our own /mmo room subcommands: exactly one of these
// arrives for every request we send (protocol 1.3).
void BallanceMMOClient::handle_room_request_outcome(const bmmo::room_event_msg& msg, bool accepted) {
    using action = bmmo::room::action;
    const auto request = pop_room_request();
    if (!accepted) {
        if (request && request->action == action::List) {
            // no room_state is coming; do not print the old list on the next push
            std::lock_guard lk(room_state_mtx_);
            room_list_requested_ = false;
        }
        SendIngameMessage(std::format("Error: {} failed: {}{}{}.",
                request ? std::format("\"/mmo room {}\"", room_action_name(request->action))
                        : std::string{"the room request"},
                bmmo::room::error_string(msg.error),
                msg.reason.empty() ? "" : " - ", msg.reason), bmmo::ansi::BrightRed);
        return;
    }
    if (!request)  // we lost track of what this answers; the state push speaks for itself
        return;
    switch (request->action) {
        case action::List:
            break;  // print_room_list() already ran off the room_state
        case action::Create:
            SendIngameMessage(std::format("Created room #{}; you are the host. Use \"/mmo room ready\", "
                    "then \"/mmo room start physics\" once everyone is ready.", msg.room),
                    bmmo::ansi::BrightGreen);
            { std::lock_guard lk(room_state_mtx_); room_status_requested_ = true; }
            break;
        case action::Join:
            SendIngameMessage(std::format("Joined room #{}.", msg.room), bmmo::ansi::BrightGreen);
            { std::lock_guard lk(room_state_mtx_); room_status_requested_ = true; }
            break;
        case action::Leave:
            SendIngameMessage(std::format("Left room #{}.", msg.room), bmmo::ansi::BrightYellow);
            break;
        case action::Ready:
        case action::Unready: {
            auto text = room_ready_text(db_.get_client_id());
            SendIngameMessage(text.empty()
                    ? std::format("You are{} ready.", request->ready ? "" : " no longer")
                    : text, bmmo::ansi::BrightGreen);
            break;
        }
        case action::Start:
            SendIngameMessage(std::format("Starting a {} session in room #{}.",
                    request->mode == bmmo::room::mode::Physics ? "physics" : "shadow", msg.room),
                    bmmo::ansi::BrightGreen);
            break;
        case action::Kick:
            SendIngameMessage(std::format("Removed {} from room #{}.",
                    get_username(msg.subject), msg.room), bmmo::ansi::BrightYellow);
            break;
        case action::Close:
            SendIngameMessage(std::format("Closed room #{}.", msg.room), bmmo::ansi::BrightYellow);
            break;
    }
}

// Network thread. One human-readable line per room event.
void BallanceMMOClient::handle_room_event(const bmmo::room_event_msg& msg) {
    using event_type = bmmo::room::event_type;
    const auto own = db_.get_client_id();
    switch (msg.type) {
        case event_type::RequestAccepted:
        case event_type::RequestDenied:
            handle_room_request_outcome(msg, msg.type == event_type::RequestAccepted);
            break;
        case event_type::PlayerJoined:
            SendIngameMessage(std::format("{} joined the room.", get_username(msg.actor)),
                    bmmo::ansi::BrightGreen);
            break;
        case event_type::PlayerLeft:
            // The black box: this is the only unambiguous "left" signal a
            // remaining member gets (the leaver's relayed Unphysicalize is the
            // same message a living player sends when its ball dies), and the
            // server drops that player from the session world here, so a replay
            // of our file has to drop it too.
            utils_.run_on_game_thread([this, subject = msg.subject] {
                if (physics_session_.session == 0) return;
                bmmo::session::client_journal::instance().leave_player(
                        physics_session_.current_tick(), subject);
            });
            if (msg.actor == msg.subject)  // left on their own
                SendIngameMessage(std::format("{} left the room.", get_username(msg.subject)));
            else if (msg.actor != own)     // our own kick is reported by its outcome
                SendIngameMessage(std::format("{} was removed from the room by {}.",
                        get_username(msg.subject), get_username(msg.actor)), bmmo::ansi::BrightYellow);
            break;
        case event_type::HostChanged:
            SendIngameMessage(msg.actor == own
                    ? std::string{"You are now the room host."}
                    : std::format("{} is now the room host.", get_username(msg.actor)),
                    bmmo::ansi::BrightYellow);
            break;
        case event_type::Kicked:
            SendIngameMessage(std::format("You were removed from room #{} by {}.",
                    msg.room, get_username(msg.actor)), bmmo::ansi::BrightRed);
            break;
        case event_type::RoomClosed:
            if (msg.actor != own)  // the host's own close is reported by its outcome
                SendIngameMessage(std::format("Room #{} was closed by {}.",
                        msg.room, get_username(msg.actor)), bmmo::ansi::BrightYellow);
            break;
        case event_type::ReadyChanged: {
            if (msg.subject == own)  // our own toggle is reported by its outcome
                break;
            auto text = room_ready_text(msg.subject);
            if (!text.empty())
                SendIngameMessage(text);
            break;
        }
        case event_type::SessionStarting:
            if (msg.actor != own)  // the host's own start is reported by its outcome
                SendIngameMessage(std::format("{} started the room session.", get_username(msg.actor)),
                        bmmo::ansi::BrightGreen);
            break;
        case event_type::SessionEnded:
            SendIngameMessage(std::format("The room session ended{}{}.",
                    msg.reason.empty() ? "" : ": ", msg.reason), bmmo::ansi::BrightYellow);
            break;
        default:
            break;
    }
}
