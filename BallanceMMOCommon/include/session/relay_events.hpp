#pragma once

// Protocol 33 (docs/relay-protocol-design.md 4.9, the user's decisions of
// 2026-11-04): the lifecycle messages - SessionEvent, SessionMember - ride the
// relay datagrams from the first that carries them until the recipient's
// acknowledgement passes that datagram's tick, as its frames do.  On the wire
// an event is compact: what differs per event (~13 B), its pose from the
// datagram's pose table, its recipe from the session's recipe dictionary;
// every other message, and an event the compact form cannot hold, is raw - the
// bytes its reliable copy carries.  The floats are copied bit for bit: the
// body every world makes from them must be the owner's.

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "../entity/session.hpp"
#include "../message/message_utils.hpp"
#include "../message/session_event_msg.hpp"

namespace bmmo::session {
    // The head byte: bits 0-2 the event type (kRelayEventRaw: a raw message),
    // bits 3-5 the state (EVENT_STATE_*), bit 6 an origin byte follows.
    inline constexpr uint8_t kRelayEventRaw = 7;
    inline constexpr uint8_t kRelayEventStateShift = 3;
    inline constexpr uint8_t kRelayEventOrigin = 1u << 6;
    inline constexpr size_t kRelayPoseBytes = sizeof(float) * 12;

    // One message of the events section.  A compact one keeps the reliable
    // copy's bytes too (`raw`): the server falls back to them where a
    // datagram's tick puts the event out of the compact form's reach.
    struct relay_event {
        uint8_t kind = kRelayEventRaw;   // event_type, or kRelayEventRaw
        uint8_t state = 0;
        uint32_t player = 0;
        uint16_t life = 0;
        uint32_t tick = 0, origin_tick = 0;
        uint8_t ball_type = 0, flags = 0;
        uint8_t join_order = 255;        // Trafo (protocol 39): session_event_msg::join_order
        float position[3] = {};
        float rotation[9] = {};
        int32_t sector = 0;
        uint8_t recipe = 0;              // Physicalize: its id in the session's dictionary
        std::string raw;

        bool compact() const { return kind != kRelayEventRaw; }
        // The bytes of a Physicalize's pose, its key in a datagram's table.
        std::string pose() const {
            std::string out(kRelayPoseBytes, '\0');
            std::memcpy(out.data(), position, sizeof(position));
            std::memcpy(out.data() + sizeof(position), rotation, sizeof(rotation));
            return out;
        }
        bool operator==(const relay_event& o) const {
            return kind == o.kind && state == o.state && player == o.player && life == o.life && tick == o.tick
                && origin_tick == o.origin_tick && ball_type == o.ball_type && flags == o.flags
                && join_order == o.join_order && std::memcmp(position, o.position, sizeof(position)) == 0
                && std::memcmp(rotation, o.rotation, sizeof(rotation)) == 0 && sector == o.sector
                && recipe == o.recipe && (compact() || raw == o.raw);
        }
    };

    // A recipe's bytes as the Physicalize writes them: the dictionary's key
    // and its wire form.
    inline std::string recipe_bytes(const ball_recipe& r) {
        std::stringstream raw;
        bmmo::session_event_msg::write_recipe(r, raw);
        return raw.str();
    }

    // The server's numbering of the recipes, one per session: an id on first
    // use, up to 255 of them (a 256th recipe's events go raw).
    class recipe_numbering {
    public:
        std::optional<uint8_t> id_of(const ball_recipe& r) {
            const std::string bytes = recipe_bytes(r);
            for (size_t i = 0; i < bytes_.size(); ++i)
                if (bytes_[i] == bytes) return static_cast<uint8_t>(i);
            if (bytes_.size() >= 255) return std::nullopt;
            bytes_.push_back(bytes);
            recipes_.push_back(r);
            return static_cast<uint8_t>(bytes_.size() - 1);
        }
        const ball_recipe& recipe(uint8_t id) const { return recipes_.at(id); }
        size_t size(uint8_t id) const { return bytes_.at(id).size(); }
        void clear() { bytes_.clear(); recipes_.clear(); }
    private:
        std::vector<std::string> bytes_;
        std::vector<ball_recipe> recipes_;
    };

    // A message as the server sends it reliably, for the relay: a
    // SessionEvent of a compact type in its fields, anything else raw.
    inline relay_event relay_event_of(const std::string& bytes, recipe_numbering& recipes) {
        relay_event e;
        e.raw = bytes;
        bmmo::session_event_msg m;
        m.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (bytes.size() < sizeof(bmmo::opcode) || !m.deserialize()) return e;
        if (m.type != event_type::Physicalize && m.type != event_type::Unphysicalize && m.type != event_type::Sector
                && m.type != event_type::Finish && m.type != event_type::Trafo)
            return e;
        if ((m.state & ~EVENT_STATE_KNOWN) || static_cast<uint32_t>(m.tick - m.origin_tick) > 255) return e;
        if (m.type == event_type::Physicalize) {
            const auto id = recipes.id_of(m.recipe);
            if (!id) return e;
            e.recipe = *id;
        }
        e.kind = static_cast<uint8_t>(m.type);
        e.state = m.state;
        e.player = m.player;
        e.life = m.life;
        e.tick = m.tick;
        e.origin_tick = m.origin_tick;
        e.ball_type = m.ball_type;
        e.flags = m.flags;
        e.join_order = m.join_order;
        std::memcpy(e.position, m.position, sizeof(e.position));
        std::memcpy(e.rotation, m.rotation, sizeof(e.rotation));
        e.sector = m.sector;
        return e;
    }

    // Whether a datagram of `tick` can carry `e` compact (its tick within an
    // i16 of the datagram's).
    inline bool relay_compact_at(const relay_event& e, uint32_t tick) {
        const int32_t d = static_cast<int32_t>(e.tick - tick);
        return e.compact() && d >= INT16_MIN && d <= INT16_MAX;
    }

    // An event's bytes in a datagram of `tick` (its pose and recipe aside).
    inline size_t relay_event_bytes(const relay_event& e, uint32_t tick) {
        if (!relay_compact_at(e, tick)) return 1 + sizeof(uint16_t) + e.raw.size();
        size_t bytes = 1 + 4 + 2 + 2 + (e.origin_tick != e.tick ? 1 : 0);
        switch (static_cast<event_type>(e.kind)) {
            case event_type::Physicalize: return bytes + 4;
            case event_type::Sector: return bytes + sizeof(int32_t);
            case event_type::Trafo: return bytes + 3 + sizeof(e.position);
            default: return bytes;
        }
    }

    // A compact event back into the message its reliable copy is; false
    // when its recipe is not in `recipes` (the client's dictionary).
    inline bool event_msg_of(const relay_event& e, uint32_t session, const std::map<uint8_t, ball_recipe>& recipes,
                             bmmo::session_event_msg& out) {
        if (!e.compact()) return false;
        out.session = session;
        out.player = e.player;
        out.tick = e.tick;
        out.type = static_cast<event_type>(e.kind);
        out.origin_tick = e.origin_tick;
        out.state = e.state;
        out.life = e.life;
        out.ball_type = e.ball_type;
        out.flags = e.flags;
        out.join_order = e.join_order;
        std::memcpy(out.position, e.position, sizeof(out.position));
        std::memcpy(out.rotation, e.rotation, sizeof(out.rotation));
        out.sector = e.sector;
        if (out.type == event_type::Physicalize) {
            const auto it = recipes.find(e.recipe);
            if (it == recipes.end()) return false;
            out.recipe = it->second;
        }
        return true;
    }
}
