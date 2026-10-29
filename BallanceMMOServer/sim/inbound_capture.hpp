#ifndef BALLANCEMMO_SIM_INBOUND_CAPTURE_HPP
#define BALLANCEMMO_SIM_INBOUND_CAPTURE_HPP

// Inbound capture and replay for the headless session client
// (docs/session-client-core-design.md 5.2 (5), step E0).
//
// --capture-inbound <file> writes, in the order the client meets them, every
// input from outside the process that the session flow reads:
//
//   I  a pass of the main loop: its index and how many steps (load_step()
//      and frame() calls) ran before it - the frame index every record
//      after it belongs to;
//   M  a message the client handled, its bytes as the network gave them;
//   S  a connection state change the GNS callback delivered;
//   C  a wall-clock read, by the site that made it (wall_site) - the hold
//      deadline, the resync limiter, journal_ms, the pacer's schedule, ...
//
// --replay-inbound <file> runs the same loop with no network and no sleeping:
// each poll hands on_message() the messages of its pass, each clock read
// returns the value captured at its site, and nothing is sent.  The world is
// a function of those inputs, so the replay must write the same journal as
// the live run, byte for byte; a read the live run did not make (a different
// site, a pass with a different step count) is a divergence and stops the
// replay at once, naming the record.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <session/clock_site.hpp>
#include <session/io_queue.hpp>

namespace bmmo::sim {

    // Every place the session flow reads the wall clock: the shared enum
    // (session/clock_site.hpp), so a read the shared core makes names its
    // site the same way.  Its values are part of the file format.
    using wall_site = bmmo::session::clock_site;

    inline const char* wall_site_name(wall_site site) { return bmmo::session::clock_site_name(site); }

    class inbound_capture {
    public:
        enum class tag : uint8_t { iteration = 'I', message = 'M', clock = 'C', connection = 'S', end = 'E' };
        static constexpr uint32_t kVersion = 1;

        inbound_capture() = default;
        inbound_capture(const inbound_capture&) = delete;
        inbound_capture& operator=(const inbound_capture&) = delete;
        ~inbound_capture() { close(); }

        bool capturing() const { return file_.is_open(); }
        bool replaying() const { return replay_; }

        // ------------------------------------------------------------ capture
        // `clock_den`: the steady clock's ticks per second, so a replay on a
        // clock of another period refuses the file instead of misreading it.
        // `poster` (step SIO): the host's I/O strand - the records are
        // handed to it once per pass (iteration) and written there with no
        // flush; empty: written on this thread.
        bool open_capture(const std::string& path, const std::string& command_line, int64_t clock_den,
                          std::string& error, bmmo::session::io_poster poster = {}) {
            if (!file_.open(path, std::move(poster), kMaxQueued, error, 1 << 20)) {
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

        void iteration(uint64_t index, uint64_t steps) {
            if (!capturing()) return;
            // The previous pass's records go to the writer here: one append
            // per pass, not one per clock read.
            hand_over();
            put_u8(static_cast<uint8_t>(tag::iteration));
            put_u64(index);
            put_u64(steps);
        }

        void message(const void* data, uint32_t size) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::message));
            put_u32(size);
            put_bytes(data, size);
            ++messages_;
            if (pending_.size() >= kHandOverBytes) hand_over();
        }

        void clock(wall_site site, int64_t value) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::clock));
            put_u8(static_cast<uint8_t>(site));
            put_i64(value);
        }

        void connection(int32_t state, uint32_t handle, const std::string& end_debug) {
            if (!capturing()) return;
            put_u8(static_cast<uint8_t>(tag::connection));
            put_u32(static_cast<uint32_t>(state));
            put_u32(handle);
            put_string32(end_debug);
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
            uint32_t version = 0;
            int64_t den = 0;
            if (!get_bytes(magic, 4) || std::memcmp(magic, "BMIC", 4) != 0) { error = path + " is not an inbound capture"; return false; }
            if (!get_u32(version) || version != kVersion) { error = path + ": capture version " + std::to_string(version)
                                                                   + ", this client reads " + std::to_string(kVersion); return false; }
            if (!get_i64(den) || den != clock_den) { error = path + ": captured with a clock of " + std::to_string(den)
                                                            + " ticks per second, this one has " + std::to_string(clock_den); return false; }
            if (!get_string32(command_line_)) { error = path + ": truncated header"; return false; }
            replay_ = true;
            return true;
        }

        const std::string& command_line() const { return command_line_; }

        // A FREE site (the headless --replay-free-cost-clock, step B5RC,
        // landed at B5RX): the
        // replay reads its own clock there, and the capture's reads at that
        // site are passed over wherever the replay meets them.  For a
        // paired replay whose rewinds differ from the capture's (another
        // rule, another build's clock reads): the rewind cost's reads
        // (rewind_*) decide nothing but the cap.  Off by default: every read
        // is taken in order.
        static_assert(static_cast<size_t>(wall_site::count_) <= 64, "the free-site mask is one uint64_t");
        void set_free(wall_site site) { free_ |= uint64_t(1) << static_cast<unsigned>(site); }
        bool is_free(wall_site site) const { return (free_ >> static_cast<unsigned>(site)) & 1u; }
        uint64_t free_skipped() const { return free_skipped_; }

        // The next record's tag; tag::end at the end of the data too.
        tag peek() const {
            const size_t at = next_pos();
            if (at >= data_.size()) return tag::end;
            return static_cast<tag>(data_[at]);
        }
        bool complete() const { return next_pos() < data_.size() && peek() == tag::end; }
        bool exhausted() const { return next_pos() >= data_.size() || peek() == tag::end; }

        bool take_iteration(uint64_t& index, uint64_t& steps) {
            skip_free();
            if (peek() != tag::iteration || pos_ >= data_.size()) return false;
            const size_t at = pos_++;
            if (get_u64(index) && get_u64(steps)) { ++records_; return true; }
            pos_ = at;
            return false;
        }

        bool take_message(std::string& bytes) {
            skip_free();
            if (peek() != tag::message || pos_ >= data_.size()) return false;
            const size_t at = pos_++;
            uint32_t size = 0;
            if (get_u32(size) && pos_ + size <= data_.size()) {
                bytes.assign(reinterpret_cast<const char*>(data_.data() + pos_), size);
                pos_ += size;
                ++records_;
                ++messages_;
                return true;
            }
            pos_ = at;
            return false;
        }

        bool take_connection(int32_t& state, uint32_t& handle, std::string& end_debug) {
            skip_free();
            if (peek() != tag::connection || pos_ >= data_.size()) return false;
            const size_t at = pos_++;
            uint32_t s = 0;
            if (get_u32(s) && get_u32(handle) && get_string32(end_debug)) {
                state = static_cast<int32_t>(s);
                ++records_;
                return true;
            }
            pos_ = at;
            return false;
        }

        // The clock read at `site`: false, with `error` saying what the
        // capture holds instead, when the next record is anything else.
        bool take_clock(wall_site site, int64_t& value, std::string& error) {
            skip_free();
            if (peek() != tag::clock || pos_ + 10 > data_.size()) {
                error = std::string("expected a clock read at ") + wall_site_name(site) + ", the capture has " + describe_next();
                return false;
            }
            const auto captured = static_cast<wall_site>(data_[pos_ + 1]);
            if (captured != site) {
                error = std::string("expected a clock read at ") + wall_site_name(site) + ", the capture has one at "
                      + wall_site_name(captured);
                return false;
            }
            pos_ += 2;
            get_i64(value);
            ++records_;
            return true;
        }

        std::string describe_next() const {
            const size_t at = next_pos();
            switch (peek()) {
            case tag::iteration: return "a loop pass";
            case tag::message: return "a message";
            case tag::clock:
                return at + 1 < data_.size()
                    ? std::string("a clock read at ") + wall_site_name(static_cast<wall_site>(data_[at + 1]))
                    : std::string("a truncated clock read");
            case tag::connection: return "a connection state change";
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
            while (free_ && at + 10 <= data_.size() && static_cast<tag>(data_[at]) == tag::clock
                   && is_free(static_cast<wall_site>(data_[at + 1])))
                at += 10;
            return at;
        }
        void skip_free() {
            while (free_ && pos_ + 10 <= data_.size() && static_cast<tag>(data_[pos_]) == tag::clock
                   && is_free(static_cast<wall_site>(data_[pos_ + 1]))) {
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
        bmmo::session::io_queue file_;
        std::string pending_;
        bool write_failed_ = false;
        bool replay_ = false;
        std::vector<uint8_t> data_;
        size_t pos_ = 0;
        uint64_t records_ = 0;
        uint64_t free_ = 0;            // set_free's sites, one bit each
        uint64_t free_skipped_ = 0;    // captured reads at a free site passed over
        uint64_t messages_ = 0;
        std::string command_line_;
    };

}

#endif
