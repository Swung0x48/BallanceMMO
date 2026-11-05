#pragma once

// A process's capture: every input from outside that its session flow reads,
// in the order it read them (docs/canned-tests-plan.md 3.1, seam C3; the
// headless client's inbound capture of step E0, moved here at step K3).  The
// seams write it (session/capture_seams.hpp: a recording clock and
// transport); their replaying twins hand it back, and a read the live run did
// not make is a divergence.
//
//   I  a pass of the host's loop: its index and how many steps ran before it
//   C  a clock read, by site (clock_site.hpp): the value as read
//   N  a message (v2): the connection, the transport's receive time, the bytes
//   T  a connection state change (v2): handle, state, old state, end reason,
//      the end text, the description
//   P  a connection's status read (v2): handle, whether it was known, ping
//      and the rest of net::connection_status
//   L  a read of the transport's clock (v2)
//   M  a message (v1: bytes only; read as connection 0, receive time 0)
//   S  a connection state change (v1: state, handle, end text)
//   E  the end marker (a capture without one was cut short)
//   Q  a take from a host's queue (v4, the retail mod, K7d): the take's site
//      (a u8 the host numbers), how many entries, each entry's bytes - every
//      take is written, an empty one too, so a replay takes at the same place
//   K  a frame's keyboard (v4, K7d): the frame's number, the state's bytes
//   O  a session message sent (v4, K7d): the send mode, the bytes
//
// File: "BMIC", u32 version, i64 clock ticks per second, str32 command line,
// then the records (u8 tag, little-endian fields).  Version 2 adds N, T, P and
// L and writes them in place of M and S; a version 1 file reads unchanged.
// Version 3 (step T3), the same records: the headless client's login uuid
// seed is a clock read (clock_site::login_uuid) - a replay of an older
// capture takes that site from its own clock.  Version 4 (K7d) adds Q, K and
// O; the older records are unchanged.
// CK-free and header-only.

#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../net/transport.hpp"
#include "clock_site.hpp"
#include "io_queue.hpp"

namespace bmmo::session {

    class capture_file {
    public:
        enum class tag : uint8_t {
            iteration = 'I', clock = 'C', message = 'N', status = 'T', link_status = 'P', link_clock = 'L',
            message_v1 = 'M', status_v1 = 'S', end = 'E', take = 'Q', keys = 'K', sent = 'O',
        };
        static constexpr uint32_t kVersion = 4;

        // A message as captured (its bytes owned).
        struct message_record {
            net::connection conn = net::kInvalidConnection;
            net::microseconds received = 0;
            std::string bytes;
        };

        capture_file() = default;
        capture_file(const capture_file&) = delete;
        capture_file& operator=(const capture_file&) = delete;
        ~capture_file() { close(); }

        bool capturing() const { return file_.is_open(); }
        bool replaying() const { return replay_; }

        // ------------------------------------------------------------ capture
        // `clock_den`: the steady clock's ticks per second, so a replay on a
        // clock of another period refuses the file instead of misreading it.
        // `poster` (step SIO): the host's I/O strand - the records are
        // handed to it once per pass (iteration) and written there; empty:
        // written on this thread.  `flush` (step IOF): the strand's flush
        // mode (record = once per pass handed over).
        bool open_capture(const std::string& path, const std::string& command_line, int64_t clock_den,
                          std::string& error, io_poster poster = {}, io_flush flush = io_flush::batch) {
            if (!file_.open(path, std::move(poster), kMaxQueued, error, 1 << 20, flush)) {
                error = "cannot write " + path;
                return false;
            }
            put_bytes("BMIC", 4);
            put_u32(kVersion);
            put_i64(clock_den);
            put_string32(command_line);
            hand_over();
            return true;
        }

        // The retail mod's open (K7d, step IOX's rule: no file-system call on
        // its game thread): the parent directory, `prepare` (the prune) and
        // the file are made on the writer strand; a failure shows later as
        // write_failed().
        bool open_capture_deferred(const std::string& path, const std::string& command_line, int64_t clock_den,
                                   io_poster poster, std::function<void()> prepare, std::string& error) {
            if (!file_.open_deferred(path, std::move(poster), kMaxQueued, error, io_flush::batch, true, std::move(prepare)))
                return false;
            put_bytes("BMIC", 4);
            put_u32(kVersion);
            put_i64(clock_den);
            put_string32(command_line);
            hand_over();
            return true;
        }

        void iteration(uint64_t index, uint64_t steps) {
            if (!capturing()) return;
            // The previous pass's records go to the writer here: one append
            // per pass, not one per clock read.
            hand_over();
            put_u8(static_cast<uint8_t>(tag::iteration));
            put_u64(index);
            put_u64(steps);
        }

        void clock(clock_site site, int64_t value) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::clock));
            put_u8(static_cast<uint8_t>(site));
            put_i64(value);
        }

        void message(const net::message& m) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::message));
            put_u32(m.conn);
            put_i64(m.received);
            put_u32(static_cast<uint32_t>(m.size));
            put_bytes(m.data, static_cast<size_t>(m.size));
            ++messages_;
            if (pending_.size() >= kHandOverBytes) hand_over();
        }

        void status(const net::status_change& c) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::status));
            put_u32(c.conn);
            put_u32(static_cast<uint32_t>(c.state));
            put_u32(static_cast<uint32_t>(c.old_state));
            put_u32(static_cast<uint32_t>(c.end_reason));
            put_string32(c.end_debug);
            put_string32(c.description);
        }

        void link_status(net::connection c, bool known, const net::connection_status& s) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::link_status));
            put_u32(c);
            put_u8(known ? 1 : 0);
            put_bytes(&s, sizeof(s));   // a plain struct of ints and floats, written and read by one build
        }

        void link_clock(net::microseconds value) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::link_clock));
            put_i64(value);
        }

        void take(uint8_t site, const std::vector<std::string>& entries) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::take));
            put_u8(site);
            put_u32(static_cast<uint32_t>(entries.size()));
            for (const auto& e: entries) put_string32(e);
            messages_ += entries.size();
            if (pending_.size() >= kHandOverBytes) hand_over();
        }

        void keys(uint64_t frame, const void* state, size_t size) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::keys));
            put_u64(frame);
            put_u32(static_cast<uint32_t>(size));
            put_bytes(state, size);
        }

        void sent(uint8_t mode, const std::string& bytes) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::sent));
            put_u8(mode);
            put_string32(bytes);
            if (pending_.size() >= kHandOverBytes) hand_over();
        }

        // The end marker: a replay that meets it knows the live run got here
        // (a capture without one was cut short by a crash or a kill).
        // A capture that failed to write (a full disk, or step SIO's writer
        // queue full) gets no end marker, so its replay says the live run was
        // cut short; the caller logs it.  The close is synchronous (the end
        // of the run): write_failed() is final after it.
        void close() {
            if (!capturing()) return;
            if (!write_failed_) put_u8(static_cast<uint8_t>(tag::end));
            hand_over();
            file_.close_now(std::chrono::milliseconds(60000));
            if (file_.failed()) write_failed_ = true;
        }

        uint64_t messages() const { return messages_; }
        bool write_failed() const { return write_failed_ || file_.failed(); }
        uint64_t peak_queued() const { return file_.peak_queued(); }

        // ------------------------------------------------------------- replay
        bool open_replay(const std::string& path, int64_t clock_den, std::string& error) {
            std::FILE* in = std::fopen(path.c_str(), "rb");
            if (!in) { error = "cannot read " + path; return false; }
            // Read in chunks to the end: no ftell, whose long is 32 bits on
            // Windows.
            data_.clear();
            const size_t kChunk = size_t(1) << 20;
            for (;;) {
                const size_t at = data_.size();
                data_.resize(at + kChunk);
                const size_t got = std::fread(data_.data() + at, 1, kChunk, in);
                data_.resize(at + got);
                if (got < kChunk) break;
            }
            const bool failed = std::ferror(in) != 0;
            std::fclose(in);
            if (failed) { error = "read error on " + path; return false; }
            pos_ = 0;
            char magic[4] = {};
            int64_t den = 0;
            if (!get_bytes(magic, 4) || std::memcmp(magic, "BMIC", 4) != 0) { error = path + " is not an inbound capture"; return false; }
            if (!get_u32(version_) || version_ < 1 || version_ > kVersion) {
                error = path + ": capture version " + std::to_string(version_) + ", this build reads 1.."
                      + std::to_string(kVersion);
                return false;
            }
            if (!get_i64(den) || den != clock_den) { error = path + ": captured with a clock of " + std::to_string(den)
                                                            + " ticks per second, this one has " + std::to_string(clock_den); return false; }
            if (!get_string32(command_line_)) { error = path + ": truncated header"; return false; }
            replay_ = true;
            return true;
        }

        const std::string& command_line() const { return command_line_; }
        uint32_t version() const { return version_; }

        // A FREE site (the headless --replay-free-cost-clock, step B5RC,
        // landed at B5RX): the replay reads its own clock there, and the
        // capture's reads at that site are passed over wherever the replay
        // meets them.  For a paired replay whose rewinds differ from the
        // capture's (another rule, another build's clock reads): the rewind
        // cost's reads (rewind_*) decide nothing but the cap.  Off by default:
        // every read is taken in order.
        static_assert(static_cast<size_t>(clock_site::count_) <= 256, "a site is a u8 in the file");
        void set_free(clock_site site) { free_.set(static_cast<size_t>(site)); }
        bool is_free(clock_site site) const { return free_.test(static_cast<size_t>(site)); }
        uint64_t free_skipped() const { return free_skipped_; }

        // The next record's tag; tag::end at the end of the data too.
        tag peek() const {
            const size_t at = next_pos();
            if (at >= data_.size()) return tag::end;
            return static_cast<tag>(data_[at]);
        }
        bool complete() const { return next_pos() < data_.size() && peek() == tag::end; }
        // Step K4b-ii: whether the next record is a clock read at `site` (the
        // server's replay tells a requested receive pass from the loop's own
        // by it).
        bool next_is_clock(clock_site site) const {
            const size_t at = next_pos();
            return peek() == tag::clock && at + 1 < data_.size() && static_cast<clock_site>(data_[at + 1]) == site;
        }
        bool exhausted() const { return next_pos() >= data_.size() || peek() == tag::end; }

        bool take_iteration(uint64_t& index, uint64_t& steps) {
            skip_free();
            if (peek() != tag::iteration || pos_ >= data_.size()) return false;
            const size_t at = pos_++;
            if (get_u64(index) && get_u64(steps)) { ++records_; return true; }
            pos_ = at;
            return false;
        }

        // The next record if it is a message (v1 or v2).
        bool take_message(message_record& out) {
            skip_free();
            const tag t = peek();
            if ((t != tag::message && t != tag::message_v1) || pos_ >= data_.size()) return false;
            const size_t at = pos_++;
            out = message_record{};
            uint32_t conn = 0, size = 0;
            bool ok = true;
            if (t == tag::message) ok = get_u32(conn) && get_i64(out.received);
            if (ok && get_u32(size) && pos_ + size <= data_.size()) {
                out.conn = conn;
                out.bytes.assign(reinterpret_cast<const char*>(data_.data() + pos_), size);
                pos_ += size;
                ++records_;
                ++messages_;
                return true;
            }
            pos_ = at;
            return false;
        }

        // The next record if it is a connection state change (v1 or v2).
        bool take_status(net::status_change& out) {
            skip_free();
            const tag t = peek();
            if ((t != tag::status && t != tag::status_v1) || pos_ >= data_.size()) return false;
            const size_t at = pos_++;
            out = net::status_change{};
            uint32_t state = 0, old_state = 0, reason = 0, conn = 0;
            const bool ok = t == tag::status
                ? get_u32(conn) && get_u32(state) && get_u32(old_state) && get_u32(reason)
                      && get_string32(out.end_debug) && get_string32(out.description)
                : get_u32(state) && get_u32(conn) && get_string32(out.end_debug);
            if (!ok) { pos_ = at; return false; }
            out.conn = conn;
            out.state = static_cast<net::connection_state>(static_cast<int32_t>(state));
            out.old_state = static_cast<net::connection_state>(static_cast<int32_t>(old_state));
            out.end_reason = static_cast<int32_t>(reason);
            ++records_;
            return true;
        }

        bool take_link_status(net::connection c, bool& known, net::connection_status& s, std::string& error) {
            skip_free();
            if (peek() != tag::link_status || pos_ + 1 + 4 + 1 + sizeof(s) > data_.size()) {
                error = "expected a connection status read, the capture has " + describe_next();
                return false;
            }
            const size_t at = pos_++;
            uint32_t conn = 0;
            uint8_t k = 0;
            get_u32(conn);
            get_bytes(&k, 1);
            get_bytes(&s, sizeof(s));
            if (conn != c) {
                pos_ = at;
                error = "expected a status read of connection " + std::to_string(c) + ", the capture's is of "
                      + std::to_string(conn);
                return false;
            }
            known = k != 0;
            ++records_;
            return true;
        }

        bool take_link_clock(net::microseconds& value, std::string& error) {
            skip_free();
            if (peek() != tag::link_clock || pos_ + 9 > data_.size()) {
                error = "expected a transport clock read, the capture has " + describe_next();
                return false;
            }
            ++pos_;
            get_i64(value);
            ++records_;
            return true;
        }

        // The clock read at `site`: false, with `error` saying what the
        // capture holds instead, when the next record is anything else.
        bool take_clock(clock_site site, int64_t& value, std::string& error) {
            skip_free();
            if (peek() != tag::clock || pos_ + 10 > data_.size()) {
                error = std::string("expected a clock read at ") + clock_site_name(site) + ", the capture has " + describe_next();
                return false;
            }
            const auto captured = static_cast<clock_site>(data_[pos_ + 1]);
            if (captured != site) {
                error = std::string("expected a clock read at ") + clock_site_name(site) + ", the capture has one at "
                      + clock_site_name(captured);
                return false;
            }
            pos_ += 2;
            get_i64(value);
            ++records_;
            return true;
        }

        // The take at `site`: false, with `error`, when the next record is
        // anything else.
        bool take_take(uint8_t site, std::vector<std::string>& out, std::string& error) {
            skip_free();
            if (peek() != tag::take || pos_ + 6 > data_.size()) {
                error = "expected a take at site " + std::to_string(site) + ", the capture has " + describe_next();
                return false;
            }
            if (data_[pos_ + 1] != site) {
                error = "expected a take at site " + std::to_string(site) + ", the capture has one at site "
                      + std::to_string(data_[pos_ + 1]);
                return false;
            }
            const size_t at = pos_;
            pos_ += 2;
            uint32_t count = 0;
            out.clear();
            bool ok = get_u32(count);
            for (uint32_t i = 0; ok && i < count; ++i) {
                out.emplace_back();
                ok = get_string32(out.back());
            }
            if (!ok) { pos_ = at; error = "a truncated take"; return false; }
            messages_ += count;
            ++records_;
            return true;
        }

        // The keyboard of `frame` when the next record holds it (a frame
        // whose keys did not change has none).
        bool take_keys(uint64_t frame, std::string& out) {
            skip_free();
            if (peek() != tag::keys || pos_ + 13 > data_.size()) return false;
            const size_t at = pos_++;
            uint64_t captured = 0;
            uint32_t size = 0;
            if (!get_u64(captured) || captured != frame || !get_u32(size) || pos_ + size > data_.size()) {
                pos_ = at;
                return false;
            }
            out.assign(reinterpret_cast<const char*>(data_.data() + pos_), size);
            pos_ += size;
            ++records_;
            return true;
        }

        // The next send, which must be this one.
        bool take_sent(uint8_t mode, const std::string& bytes, std::string& error) {
            skip_free();
            if (peek() != tag::sent || pos_ + 6 > data_.size()) {
                error = "expected a send, the capture has " + describe_next();
                return false;
            }
            const size_t at = pos_;
            pos_ += 1;
            uint8_t captured_mode = 0;
            std::string captured;
            if (!get_bytes(&captured_mode, 1) || !get_string32(captured)) {
                pos_ = at;
                error = "a truncated send";
                return false;
            }
            ++records_;
            if (captured_mode != mode || captured != bytes) {
                error = "the send differs from the capture's (" + std::to_string(bytes.size()) + " bytes, mode "
                      + std::to_string(mode) + "; the capture's " + std::to_string(captured.size()) + " bytes, mode "
                      + std::to_string(captured_mode) + ")";
                return false;
            }
            return true;
        }

        std::string describe_next() const {
            const size_t at = next_pos();
            switch (peek()) {
            case tag::iteration: return "a loop pass";
            case tag::message: case tag::message_v1: return "a message";
            case tag::clock:
                return at + 1 < data_.size()
                    ? std::string("a clock read at ") + clock_site_name(static_cast<clock_site>(data_[at + 1]))
                    : std::string("a truncated clock read");
            case tag::status: case tag::status_v1: return "a connection state change";
            case tag::link_status: return "a connection status read";
            case tag::link_clock: return "a transport clock read";
            case tag::take: return at + 1 < data_.size() ? "a take at site " + std::to_string(data_[at + 1]) : "a truncated take";
            case tag::keys: return "a frame's keyboard";
            case tag::sent: return "a send";
            case tag::end: return at >= data_.size() ? "the end of the data (no end marker)" : "the end marker";
            }
            return "an unknown record";
        }

        // Where the replay is, for a divergence report.
        std::string where() const {
            return "record " + std::to_string(records_) + " at byte " + std::to_string(pos_) + " of "
                 + std::to_string(data_.size());
        }

    private:
        // pos_, past any clock reads at a free site (each 10 bytes: tag,
        // site, i64).
        size_t next_pos() const {
            size_t at = pos_;
            while (free_.any() && at + 10 <= data_.size() && static_cast<tag>(data_[at]) == tag::clock
                   && is_free(static_cast<clock_site>(data_[at + 1])))
                at += 10;
            return at;
        }
        void skip_free() {
            while (free_.any() && pos_ + 10 <= data_.size() && static_cast<tag>(data_[pos_]) == tag::clock
                   && is_free(static_cast<clock_site>(data_[pos_ + 1]))) {
                pos_ += 10;
                ++free_skipped_;
            }
        }

        void put_bytes(const void* p, size_t n) {
            if (n && !write_failed_) pending_.append(static_cast<const char*>(p), n);
        }
        // Step SIO: the records so far to the writer.  A full writer queue
        // stops the capture as a failed write did (never blocks the tick).
        void hand_over() {
            if (pending_.empty()) return;
            if (!write_failed_ && !file_.append(std::move(pending_))) write_failed_ = true;
            pending_.clear();
        }
        void put_u8(uint8_t v) { put_bytes(&v, 1); }
        void put_u32(uint32_t v) {
            uint8_t b[4];
            for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
            put_bytes(b, 4);
        }
        void put_u64(uint64_t v) {
            uint8_t b[8];
            for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
            put_bytes(b, 8);
        }
        void put_i64(int64_t v) { put_u64(static_cast<uint64_t>(v)); }
        void put_string32(const std::string& s) {
            put_u32(static_cast<uint32_t>(s.size()));
            put_bytes(s.data(), s.size());
        }

        bool get_bytes(void* p, size_t n) {
            if (pos_ + n > data_.size()) return false;
            if (n) std::memcpy(p, data_.data() + pos_, n);
            pos_ += n;
            return true;
        }
        bool get_u32(uint32_t& v) {
            uint8_t b[4];
            if (!get_bytes(b, 4)) return false;
            v = 0;
            for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[i]) << (8 * i);
            return true;
        }
        bool get_u64(uint64_t& v) {
            uint8_t b[8];
            if (!get_bytes(b, 8)) return false;
            v = 0;
            for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (8 * i);
            return true;
        }
        bool get_i64(int64_t& v) {
            uint64_t u = 0;
            if (!get_u64(u)) return false;
            v = static_cast<int64_t>(u);
            return true;
        }
        bool get_string32(std::string& s) {
            uint32_t n = 0;
            if (!get_u32(n) || pos_ + n > data_.size()) return false;
            s.assign(reinterpret_cast<const char*>(data_.data() + pos_), n);
            pos_ += n;
            return true;
        }

        // Step SIO: the capture's writer (session/io_queue.hpp) and the
        // records of the current pass not handed to it yet.
        static constexpr uint64_t kMaxQueued = 64ull << 20;
        static constexpr size_t kHandOverBytes = 256u << 10;
        io_queue file_;
        std::string pending_;
        bool write_failed_ = false;
        bool replay_ = false;
        uint32_t version_ = 0;
        std::vector<uint8_t> data_;
        size_t pos_ = 0;
        uint64_t records_ = 0;
        std::bitset<256> free_;        // set_free's sites, one bit each
        uint64_t free_skipped_ = 0;    // captured reads at a free site passed over
        uint64_t messages_ = 0;
        std::string command_line_;
    };

}
