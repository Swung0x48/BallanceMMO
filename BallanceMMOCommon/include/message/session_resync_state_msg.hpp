#ifndef BALLANCEMMOSERVER_SESSION_RESYNC_STATE_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_RESYNC_STATE_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"
#include "../physics/physics_rt_api.h"
#include "../session/resync_state_codec.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bmmo {
    // server -> one client, reliable (protocol 10; resync design 2.3, step
    // A3): the server's state at the end of tick `tick` = base - 1, answering
    // the SessionAssign of `base` that carried ASSIGN_FLAG_STATE_FOLLOWS.  The
    // one state transfer of docs/roadmap.md: sent once, to the resynced member
    // only; the server never reads it back.
    //
    //   rows        every body the server's full snapshot of that tick would
    //               carry (the balls by owner, the movable mechanisms with
    //               their names), in the full snapshot's row format;
    //   clones      every physicalized player's ball as the server names it
    //               (its clone, "<ball row>_BMMO_<id>"): the client's blob
    //               restore maps each to its own entity for that player - the
    //               member's own ball, a mirror (the headless client's mirrors
    //               share the server's names, the retail spirit balls do not);
    //   navs        every player's ball navigation as the server's world has
    //               it (bmmo_physics_nav_state: the keys down, the force
    //               controllers alive and the force each was made with - its
    //               camera at that key's press, which no later input
    //               re-derives): the client's own ball and mirrors take it,
    //               or a held key keeps the anchor's force for good;
    //   union_state the sector union after the server's tick() of `tick`
    //               (sector_union::serialize);
    //   scripts     the portable script record of the session's selection
    //               (game/script_replay.hpp capture_portable);
    //   savestate   the server's IVP savestate of that tick (physics/
    //               physics_savestate.hpp), restored as a travelling blob.
    //   trafo_ledger (protocol 11; trafo-replication design 4.7) the three
    //               type sets of every set the server's world holds at S
    //               (bodies, the newest explosion and reset, the final
    //               bit and tick): the client builds the sets it lacks,
    //               marks the final ones, and the reconcile makes the body
    //               set of each type set equal to it before the rows and
    //               the portable record are written.  Empty unless the
    //               session has WORLD_TRAFO_INPUTS.
    //   ledger lives (protocol 21, step TRL) after the ledger: one u16 per
    //               ledger entry, the life of the set it describes (a set
    //               is (player, life)'s); always there since protocol 34.
    //   ledger join orders (protocol 39) after the lives: one u8 per entry,
    //               the join order of its life (trafo_ledger_entry::
    //               join_order), always there.
    //
    // Protocol 34: those sections are the state's BODY (body_bytes /
    // read_body_bytes, the layout above, with scripts and savestate raw and
    // the ledger lives always written), and the message carries it as one
    // zstd frame (session/resync_state_codec.hpp): plain (ref_kind 0), or
    // against the body of the last state this member decoded (ref_kind 1,
    // named by its tick and its frame's check), when the member's
    // SessionResync said it holds that one.  deserialize() reads the header
    // and the frame only; the core decodes the frame against its reference.
    // The two packed-size fields and CKPackData are gone.
    struct session_resync_state_msg : public serializable_message {
        static constexpr uint32_t kMaxBytesField = 4u << 20;   // a forged length is refused, never allocated

        uint32_t session = 0;
        uint32_t tick = 0;   // S = base - 1
        uint32_t base = 0;
        std::vector<session::body_state> rows;   // <= MAX_BODIES_PER_SNAPSHOT
        std::vector<std::pair<uint32_t, std::string>> clones;   // player -> the server's ball entity
        std::vector<std::pair<uint32_t, bmmo_physics_nav_state>> navs;   // player -> its ball's navigation
        std::vector<uint8_t> union_state;
        std::vector<uint8_t> scripts;
        std::vector<uint8_t> savestate;
        std::vector<session::trafo_ledger_entry> trafo_ledger;   // <= kMaxTrafoLedger
        // Three type sets per set; the sets are the members' and the
        // leavers' not yet retired (a leaver's goes about 230 ticks after
        // its final resets), so twice the member cap.  The server logs a
        // ledger cut at it.
        static constexpr size_t kMaxTrafoLedger = 3 * 2 * session::MAX_PLAYERS_PER_SESSION;

        // The wire (protocol 34): the body as one zstd frame.
        uint8_t ref_kind = session::RESYNC_REF_NONE;
        uint32_t ref_tick = 0, ref_check = 0;   // the reference's resync_ref_id (0 when plain)
        uint32_t raw_size = 0;                  // the decoded body's size
        std::vector<uint8_t> frame;

        session_resync_state_msg() : serializable_message(bmmo::SessionResyncState) {}

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            raw.write(reinterpret_cast<const char*>(&base), sizeof(base));
            raw.write(reinterpret_cast<const char*>(&ref_kind), sizeof(ref_kind));
            raw.write(reinterpret_cast<const char*>(&ref_tick), sizeof(ref_tick));
            raw.write(reinterpret_cast<const char*>(&ref_check), sizeof(ref_check));
            raw.write(reinterpret_cast<const char*>(&raw_size), sizeof(raw_size));
            write_bytes(raw, frame);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &tick)) return false;
            if (!message_utils::read_variable(raw, &base)) return false;
            if (!message_utils::read_variable(raw, &ref_kind) || ref_kind > session::RESYNC_REF_PREVIOUS) return false;
            if (!message_utils::read_variable(raw, &ref_tick)) return false;
            if (!message_utils::read_variable(raw, &ref_check)) return false;
            if (!message_utils::read_variable(raw, &raw_size) || raw_size > session::kMaxResyncRaw) return false;
            return read_bytes(raw, static_cast<std::streamoff>(raw.tellp()), frame);
        }

        // The body: the sections, as bytes (what both sides keep as a reference).
        std::string body_bytes() const {
            std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
            write_body(out);
            return out.str();
        }
        // The sections from a decoded body, which must be read to its end.
        bool read_body_bytes(const std::string& body) {
            // Written into, as a received message is (message_utils' string
            // reads bound a length by the put position).
            std::stringstream in(std::ios::in | std::ios::out | std::ios::binary);
            in.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!read_body(in, static_cast<std::streamoff>(body.size()))) return false;
            return in.peek() == std::char_traits<char>::eof();
        }

    private:
        void write_body(std::stringstream& raw) const {
            const uint16_t count = static_cast<uint16_t>(std::min<size_t>(rows.size(), session::MAX_BODIES_PER_SNAPSHOT));
            raw.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (uint16_t i = 0; i < count; ++i) {
                const auto& b = rows[i];
                const auto kind = static_cast<uint8_t>(b.kind);
                raw.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
                raw.write(reinterpret_cast<const char*>(&b.owner), sizeof(b.owner));
                if (b.kind == session::body_kind::Mechanism)
                    message_utils::write_string<uint16_t>(b.name.substr(0, session::MAX_NAME), raw);
                raw.write(reinterpret_cast<const char*>(b.position), sizeof(b.position));
                raw.write(reinterpret_cast<const char*>(b.rotation), sizeof(b.rotation));
                raw.write(reinterpret_cast<const char*>(b.linear), sizeof(b.linear));
                raw.write(reinterpret_cast<const char*>(b.angular), sizeof(b.angular));
                raw.write(reinterpret_cast<const char*>(&b.flags), sizeof(b.flags));
            }
            const uint16_t clone_count = static_cast<uint16_t>(std::min<size_t>(clones.size(), session::MAX_PLAYERS_PER_SESSION));
            raw.write(reinterpret_cast<const char*>(&clone_count), sizeof(clone_count));
            for (uint16_t i = 0; i < clone_count; ++i) {
                raw.write(reinterpret_cast<const char*>(&clones[i].first), sizeof(clones[i].first));
                message_utils::write_string<uint16_t>(clones[i].second.substr(0, session::MAX_NAME), raw);
            }
            const uint16_t nav_count = static_cast<uint16_t>(std::min<size_t>(navs.size(), session::MAX_PLAYERS_PER_SESSION));
            raw.write(reinterpret_cast<const char*>(&nav_count), sizeof(nav_count));
            for (uint16_t i = 0; i < nav_count; ++i) {
                const auto& [player, n] = navs[i];
                raw.write(reinterpret_cast<const char*>(&player), sizeof(player));
                const uint8_t bytes[4] = {n.active, n.key_mask, n.controller_mask, n.create_pending_mask};
                raw.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
                raw.write(reinterpret_cast<const char*>(n.force), sizeof(n.force));
            }
            write_bytes(raw, union_state);
            write_bytes(raw, scripts);
            write_bytes(raw, savestate);
            const uint16_t ledger_count = static_cast<uint16_t>(std::min<size_t>(trafo_ledger.size(), kMaxTrafoLedger));
            raw.write(reinterpret_cast<const char*>(&ledger_count), sizeof(ledger_count));
            for (uint16_t i = 0; i < ledger_count; ++i) {
                const auto& e = trafo_ledger[i];
                const uint8_t bits = static_cast<uint8_t>((e.has_explosion ? 1u : 0u) | (e.has_bodies ? 2u : 0u)
                                                          | (e.has_reset ? 4u : 0u) | (e.final ? 8u : 0u));
                raw.write(reinterpret_cast<const char*>(&e.player), sizeof(e.player));
                raw.write(reinterpret_cast<const char*>(&e.type), sizeof(e.type));
                raw.write(reinterpret_cast<const char*>(&bits), sizeof(bits));
                raw.write(reinterpret_cast<const char*>(&e.explode_applied), sizeof(e.explode_applied));
                raw.write(reinterpret_cast<const char*>(e.position), sizeof(e.position));
                raw.write(reinterpret_cast<const char*>(&e.reset_applied), sizeof(e.reset_applied));
                raw.write(reinterpret_cast<const char*>(&e.final_applied), sizeof(e.final_applied));
            }
            for (uint16_t i = 0; i < ledger_count; ++i)
                raw.write(reinterpret_cast<const char*>(&trafo_ledger[i].life), sizeof(trafo_ledger[i].life));
            for (uint16_t i = 0; i < ledger_count; ++i)
                raw.write(reinterpret_cast<const char*>(&trafo_ledger[i].join_order), sizeof(trafo_ledger[i].join_order));
        }

        bool read_body(std::stringstream& raw, std::streamoff end) {
            uint16_t count = 0;
            if (!message_utils::read_variable(raw, &count)) return false;
            if (count > session::MAX_BODIES_PER_SNAPSHOT) return false;
            rows.clear();
            rows.reserve(count);
            for (uint16_t i = 0; i < count; ++i) {
                session::body_state b;
                uint8_t kind = 0;
                if (!message_utils::read_variable(raw, &kind)) return false;
                if (kind > static_cast<uint8_t>(session::body_kind::Mechanism)) return false;
                b.kind = static_cast<session::body_kind>(kind);
                if (!message_utils::read_variable(raw, &b.owner)) return false;
                if (b.kind == session::body_kind::Mechanism) {
                    if (!message_utils::read_string<uint16_t>(raw, b.name)) return false;
                    if (b.name.size() > session::MAX_NAME) return false;
                }
                if (!raw.read(reinterpret_cast<char*>(b.position), sizeof(b.position))) return false;
                if (!raw.read(reinterpret_cast<char*>(b.rotation), sizeof(b.rotation))) return false;
                if (!raw.read(reinterpret_cast<char*>(b.linear), sizeof(b.linear))) return false;
                if (!raw.read(reinterpret_cast<char*>(b.angular), sizeof(b.angular))) return false;
                if (!message_utils::read_variable(raw, &b.flags)) return false;
                rows.push_back(std::move(b));
            }
            uint16_t clone_count = 0;
            if (!message_utils::read_variable(raw, &clone_count) || clone_count > session::MAX_PLAYERS_PER_SESSION) return false;
            clones.clear();
            for (uint16_t i = 0; i < clone_count; ++i) {
                std::pair<uint32_t, std::string> clone;
                if (!message_utils::read_variable(raw, &clone.first)) return false;
                if (!message_utils::read_string<uint16_t>(raw, clone.second) || clone.second.size() > session::MAX_NAME) return false;
                clones.push_back(std::move(clone));
            }
            uint16_t nav_count = 0;
            if (!message_utils::read_variable(raw, &nav_count) || nav_count > session::MAX_PLAYERS_PER_SESSION) return false;
            navs.clear();
            for (uint16_t i = 0; i < nav_count; ++i) {
                std::pair<uint32_t, bmmo_physics_nav_state> nav{};
                uint8_t bytes[4] = {};
                if (!message_utils::read_variable(raw, &nav.first)) return false;
                if (!raw.read(reinterpret_cast<char*>(bytes), sizeof(bytes))) return false;
                if (!raw.read(reinterpret_cast<char*>(nav.second.force), sizeof(nav.second.force))) return false;
                nav.second.active = bytes[0];
                nav.second.key_mask = bytes[1];
                nav.second.controller_mask = bytes[2];
                nav.second.create_pending_mask = bytes[3];
                navs.push_back(nav);
            }
            if (!read_bytes(raw, end, union_state) || !read_bytes(raw, end, scripts) || !read_bytes(raw, end, savestate))
                return false;
            uint16_t ledger_count = 0;
            if (!message_utils::read_variable(raw, &ledger_count) || ledger_count > kMaxTrafoLedger) return false;
            trafo_ledger.clear();
            for (uint16_t i = 0; i < ledger_count; ++i) {
                session::trafo_ledger_entry e;
                uint8_t bits = 0;
                if (!message_utils::read_variable(raw, &e.player)) return false;
                if (!message_utils::read_variable(raw, &e.type) || e.type > 2) return false;
                if (!message_utils::read_variable(raw, &bits) || (bits & ~15u) != 0) return false;
                if (!message_utils::read_variable(raw, &e.explode_applied)) return false;
                if (!raw.read(reinterpret_cast<char*>(e.position), sizeof(e.position))) return false;
                if (!message_utils::read_variable(raw, &e.reset_applied)) return false;
                if (!message_utils::read_variable(raw, &e.final_applied)) return false;
                e.has_explosion = (bits & 1u) != 0;
                e.has_bodies = (bits & 2u) != 0;
                e.has_reset = (bits & 4u) != 0;
                e.final = (bits & 8u) != 0;
                trafo_ledger.push_back(e);
            }
            // The entries' lives (protocol 21; always written since 34).
            for (auto& e: trafo_ledger)
                if (!message_utils::read_variable(raw, &e.life) || e.life == 0) return false;
            // The entries' join orders (protocol 39).
            for (auto& e: trafo_ledger)
                if (!message_utils::read_variable(raw, &e.join_order)) return false;
            return true;
        }

        static void write_bytes(std::ostream& raw, const std::vector<uint8_t>& bytes) {
            const uint32_t size = static_cast<uint32_t>(std::min<size_t>(bytes.size(), kMaxBytesField));
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            if (size) raw.write(reinterpret_cast<const char*>(bytes.data()), size);
        }
        // `end`: the stream's size, so a forged length is refused before it is allocated.
        static bool read_bytes(std::istream& raw, std::streamoff end, std::vector<uint8_t>& out) {
            uint32_t size = 0;
            if (!message_utils::read_variable(raw, &size)) return false;
            if (size > kMaxBytesField) return false;
            const auto remaining = end - static_cast<std::streamoff>(raw.tellg());
            if (remaining < 0 || static_cast<std::streamoff>(size) > remaining) return false;
            out.resize(size);
            if (size && !raw.read(reinterpret_cast<char*>(out.data()), size)) return false;
            return true;
        }
    };
}

#endif // BALLANCEMMOSERVER_SESSION_RESYNC_STATE_MSG_HPP
