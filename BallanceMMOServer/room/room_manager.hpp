#ifndef BALLANCEMMOSERVER_ROOM_MANAGER_HPP
#define BALLANCEMMOSERVER_ROOM_MANAGER_HPP
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <net/transport.hpp>
#include "../../BallanceMMOCommon/include/entity/room.hpp"

// Pure room bookkeeping for the collision-overhaul room system
// (docs/rooms-and-sessions-protocol.md). It owns no networking: every mutation
// returns an error_code plus, for removals, a description of the side effects so
// the server can send the right room_event / room_state messages under its own
// state mutex. Player identity is the bmmo::net::connection, matching the server.
namespace bmmo {
    struct server_room {
        struct member {
            bmmo::net::connection id = bmmo::net::kInvalidConnection;
            bool ready = false;
            bool vote = false;   // voted to restart the running session; cleared by every start and end
        };

        uint32_t id = 0;
        std::string name;
        bmmo::net::connection host = bmmo::net::kInvalidConnection;
        room::phase phase = room::phase::Lobby;
        room::mode mode = room::mode::Shadow;
        // The session's map (room::map_set): the host's setting (SetMap), or
        // - none set - the host's own map as a Start found it, which is that
        // session's only (map_taken) and goes when it ends.
        bmmo::map map{};
        bool map_taken = false;
        uint16_t capacity = 8;
        std::vector<member> members;  // in join order; the host is one of them

        member* find_member(bmmo::net::connection c) {
            for (auto& m : members)
                if (m.id == c) return &m;
            return nullptr;
        }

        bool all_ready() const {
            for (const auto& m : members)
                if (!m.ready) return false;
            return !members.empty();
        }
    };

    class room_manager {
    public:
        uint32_t max_rooms = 64;
        uint16_t max_members = 8;

        // Description of what a removal (leave / kick / disconnect / close) did,
        // so the server can notify the right people.
        struct removal_result {
            bool was_member = false;
            uint32_t room = 0;
            bool room_closed = false;                            // room no longer exists
            bmmo::net::connection new_host = bmmo::net::kInvalidConnection;  // set if host changed
            std::vector<bmmo::net::connection> remaining;          // members still in the room
        };

        uint32_t room_of(bmmo::net::connection c) const {
            auto it = client_room_.find(c);
            return it == client_room_.end() ? 0 : it->second;
        }

        const server_room* find(uint32_t id) const {
            auto it = rooms_.find(id);
            return it == rooms_.end() ? nullptr : &it->second;
        }
        server_room* find(uint32_t id) {
            auto it = rooms_.find(id);
            return it == rooms_.end() ? nullptr : &it->second;
        }

        const std::map<uint32_t, server_room>& rooms() const { return rooms_; }

        room::error_code create(bmmo::net::connection c, const std::string& name, uint32_t& out_id) {
            if (room_of(c) != 0) return room::error_code::AlreadyInRoom;
            if (rooms_.size() >= max_rooms) return room::error_code::ServerBusy;
            const uint32_t id = next_id_++;
            server_room r;
            r.id = id;
            r.name = name;
            r.host = c;
            r.capacity = max_members;
            r.members.push_back({c, false});
            rooms_.emplace(id, std::move(r));
            client_room_[c] = id;
            out_id = id;
            return room::error_code::None;
        }

        room::error_code join(bmmo::net::connection c, uint32_t id) {
            if (room_of(c) != 0) return room::error_code::AlreadyInRoom;
            auto* r = find(id);
            if (!r) return room::error_code::NotFound;
            if (r->members.size() >= r->capacity) return room::error_code::Full;
            r->members.push_back({c, false});
            client_room_[c] = id;
            return room::error_code::None;
        }

        room::error_code set_ready(bmmo::net::connection c, bool ready) {
            const uint32_t id = room_of(c);
            if (id == 0) return room::error_code::NotInRoom;
            auto* r = find(id);
            auto* m = r ? r->find_member(c) : nullptr;
            if (!m) return room::error_code::NotInRoom;
            m->ready = ready;
            return room::error_code::None;
        }

        // A member's vote to restart its room's running session.
        room::error_code set_vote(bmmo::net::connection c, bool vote) {
            const uint32_t id = room_of(c);
            auto* r = id != 0 ? find(id) : nullptr;
            auto* m = r ? r->find_member(c) : nullptr;
            if (!m) return room::error_code::NotInRoom;
            if (r->phase != room::phase::Running) return room::error_code::NoSession;
            m->vote = vote;
            return room::error_code::None;
        }

        // Whether c may restart its room's session now: its host, a session
        // running, and the vote of every member in that session (in_session
        // says who is; the server's to know).
        template <typename InSession>
        room::error_code can_restart(bmmo::net::connection c, InSession&& in_session) const {
            const uint32_t id = room_of(c);
            const auto* r = id != 0 ? find(id) : nullptr;
            if (!r) return room::error_code::NotInRoom;
            if (r->host != c) return room::error_code::NotHost;
            if (r->phase != room::phase::Running) return room::error_code::NoSession;
            for (const auto& m : r->members)
                if (in_session(m.id) && !m.vote) return room::error_code::NotReady;
            return room::error_code::None;
        }

        // The host's settings for the session its room will start: its map
        // and its kind.  Only while the room waits - a running session keeps
        // what it started with.
        room::error_code set_map(bmmo::net::connection c, const bmmo::map& map) {
            auto err = room::error_code::None;
            if (auto* r = waiting_room_of_host(c, err)) {
                r->map = map;
                r->map_taken = false;
            }
            return err;
        }

        room::error_code set_mode(bmmo::net::connection c, room::mode mode) {
            auto err = room::error_code::None;
            if (auto* r = waiting_room_of_host(c, err)) r->mode = mode;
            return err;
        }

        // Validates a start request and, on success, marks the room Running.
        // Map consistency and physics availability are checked by the caller,
        // which owns the per-player map data; here we only enforce host + ready.
        room::error_code start(bmmo::net::connection c, room::mode mode) {
            const uint32_t id = room_of(c);
            if (id == 0) return room::error_code::NotInRoom;
            auto* r = find(id);
            if (!r) return room::error_code::NotInRoom;
            if (r->host != c) return room::error_code::NotHost;
            if (!r->all_ready()) return room::error_code::NotReady;
            r->phase = room::phase::Running;
            r->mode = mode;
            for (auto& m : r->members) m.vote = false;   // the votes were the last session's
            return room::error_code::None;
        }

        // Ends the running session and returns the room to the lobby (host action
        // distinct from Close). Everyone is un-readied so a new Start is deliberate.
        room::error_code end_session(bmmo::net::connection c) {
            const uint32_t id = room_of(c);
            if (id == 0) return room::error_code::NotInRoom;
            auto* r = find(id);
            if (!r) return room::error_code::NotInRoom;
            if (r->host != c) return room::error_code::NotHost;
            back_to_lobby(*r);
            return room::error_code::None;
        }

        // The running session ended for a reason other than a host request
        // (server failure, world mismatch): back to the lobby, ready cleared.
        void reset_session(uint32_t id) {
            if (auto* r = find(id)) back_to_lobby(*r);
        }

        room::error_code kick(bmmo::net::connection host, bmmo::net::connection target,
                              removal_result& out) {
            const uint32_t id = room_of(host);
            if (id == 0) return room::error_code::NotInRoom;
            auto* r = find(id);
            if (!r) return room::error_code::NotInRoom;
            if (r->host != host) return room::error_code::NotHost;
            if (room_of(target) != id) return room::error_code::NotFound;
            out = remove_member(target);
            return room::error_code::None;
        }

        // Host closes the room: everyone is removed and the room destroyed.
        // Returns the members that were in it (host last is irrelevant here).
        room::error_code close(bmmo::net::connection host, std::vector<bmmo::net::connection>& members_out,
                               uint32_t& room_out) {
            const uint32_t id = room_of(host);
            if (id == 0) return room::error_code::NotInRoom;
            auto* r = find(id);
            if (!r) return room::error_code::NotInRoom;
            if (r->host != host) return room::error_code::NotHost;
            room_out = id;
            for (const auto& m : r->members) {
                members_out.push_back(m.id);
                client_room_.erase(m.id);
            }
            rooms_.erase(id);
            return room::error_code::None;
        }

        // Removes a client from whatever room it is in (voluntary leave or
        // disconnect). Safe to call for a client not in any room.
        removal_result leave(bmmo::net::connection c) {
            return remove_member(c);
        }

    private:
        // A session ended: everyone un-readied so a new Start is deliberate,
        // and a map that session took from its host forgotten.
        static void back_to_lobby(server_room& r) {
            r.phase = room::phase::Lobby;
            for (auto& m : r.members) m.ready = m.vote = false;
            if (r.map_taken) {
                r.map = {};
                r.map_taken = false;
            }
        }

        // The room c hosts while it waits in its lobby, or why there is none.
        server_room* waiting_room_of_host(bmmo::net::connection c, room::error_code& err) {
            const uint32_t id = room_of(c);
            auto* r = id != 0 ? find(id) : nullptr;
            if (!r) err = room::error_code::NotInRoom;
            else if (r->host != c) err = room::error_code::NotHost;
            else if (r->phase != room::phase::Lobby) err = room::error_code::SessionRunning;
            return err == room::error_code::None ? r : nullptr;
        }

        removal_result remove_member(bmmo::net::connection c) {
            removal_result res;
            const uint32_t id = room_of(c);
            if (id == 0) return res;
            auto* r = find(id);
            if (!r) { client_room_.erase(c); return res; }
            res.was_member = true;
            res.room = id;
            r->members.erase(std::remove_if(r->members.begin(), r->members.end(),
                    [c](const server_room::member& m) { return m.id == c; }), r->members.end());
            client_room_.erase(c);
            if (r->members.empty()) {
                res.room_closed = true;
                rooms_.erase(id);
                return res;
            }
            if (r->host == c) {
                // migrate to the next member in join order
                r->host = r->members.front().id;
                res.new_host = r->host;
            }
            for (const auto& m : r->members) res.remaining.push_back(m.id);
            return res;
        }

        std::map<uint32_t, server_room> rooms_;
        std::unordered_map<bmmo::net::connection, uint32_t> client_room_;
        uint32_t next_id_ = 1;
    };
}

#endif // BALLANCEMMOSERVER_ROOM_MANAGER_HPP
