#pragma once

// The coordinator's wire (docs/canned-tests-plan.md, steps T1b, T1e): one TCP
// connection per process of a coordinated run, frames of
//     u32 size (of what follows) | u8 op | body
// in little-endian.  A process's transport calls go to the coordinator as ops
// tagged with the sending thread's rank, a sequence and the process's virtual
// time (the coordinator applies a round's ops in the order time, process, rank,
// sequence); the coordinator answers with what the network delivers by the
// round's end - each with its time - and the round's end, up to which the
// process runs on its own.  Header-only.

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#if defined(__ANDROID__) && defined(B0)
// Bionic's <sys/ioctl.h>, through asio, defines the termios baud rate B0;
// VxMath's VxIntersect.h names a parameter B0.
#undef B0
#endif

namespace bmmo::net::coord {

    static_assert(std::endian::native == std::endian::little, "the frames are written as the memory holds them");

    enum class op : uint8_t {
        // process -> coordinator
        hello = 1,   // role u8 (0 the server, 1 a client), name str
        listen,      // tagged; port u16
        connect,     // tagged; local u32, address str
        accept,      // tagged; local u32
        close,       // tagged; local u32, reason i32, debug str, linger u8
        send,        // tagged; local u32, flags i32, bytes str
        set_name,    // tagged; local u32, name str
        set_lag,     // tagged; direction u8, ms i32
        set_loss,    // tagged; direction u8, percent f32
        idle,        // has_wake u8, wake_us i64
        bye,         // at_us i64: the process is going (its connections close then)
        link_lag,    // tagged; direction u8, delta_ms i32, dur_ms i32: this end's latency moved (step LF:
                     // a step for good when dur_ms is 0, else a spike for what is sent in the next dur_ms)
        link_dark,   // tagged; direction u8, ms i32: this end blacked out that long (step LF)
        // coordinator -> process
        welcome = 64,   // index u32
        deliver,        // local u32, at_us i64, bytes str
        status,         // local u32, at_us i64, state i32, reason i32, debug str, ping_ms i32 (-1: none)
        grant,          // until_us i64: run on your own up to it
        stop,
        ping,           // local u32, at_us i64, ping_ms i32: a connected connection's ping from then (step LF: a
                        // latency step moved it; GNS measures it all along, a status change carries it otherwise)
    };
    // A tagged op's tag: rank u8, sequence u64, at_us i64.
    constexpr size_t kTagSize = 1 + 8 + 8;

    class body_out {
    public:
        template <class T>
        body_out& put(T v) {
            static_assert(std::is_trivially_copyable_v<T>);
            bytes_.append(reinterpret_cast<const char*>(&v), sizeof(v));
            return *this;
        }
        body_out& str(const std::string& s) {
            put(static_cast<uint32_t>(s.size()));
            bytes_.append(s);
            return *this;
        }
        const std::string& bytes() const { return bytes_; }

    private:
        std::string bytes_;
    };

    class body_in {
    public:
        explicit body_in(const std::string& bytes) : bytes_(bytes) {}
        template <class T>
        T get() {
            T v{};
            if (pos_ + sizeof(T) > bytes_.size()) { ok_ = false; return v; }
            std::memcpy(&v, bytes_.data() + pos_, sizeof(T));
            pos_ += sizeof(T);
            return v;
        }
        std::string str() {
            const auto n = get<uint32_t>();
            if (!ok_ || pos_ + n > bytes_.size()) { ok_ = false; return {}; }
            std::string s = bytes_.substr(pos_, n);
            pos_ += n;
            return s;
        }
        bool ok() const { return ok_; }

    private:
        const std::string& bytes_;
        size_t pos_ = 0;
        bool ok_ = true;
    };

    // Frames go out in batches: appended to a buffer, written at once.
    inline void append_frame(std::string& out, op o, const std::string& body) {
        const auto size = static_cast<uint32_t>(1 + body.size());
        out.append(reinterpret_cast<const char*>(&size), sizeof(size));
        out.push_back(static_cast<char>(o));
        out.append(body);
    }
    inline bool write_all(asio::ip::tcp::socket& socket, std::string& out) {
        std::error_code ec;
        if (!out.empty()) asio::write(socket, asio::buffer(out), ec);
        out.clear();
        return !ec;
    }
    inline bool write_frame(asio::ip::tcp::socket& socket, op o, const std::string& body) {
        std::string frame;
        append_frame(frame, o, body);
        return write_all(socket, frame);
    }

    // Reads frames through a buffer: one read takes whatever has arrived.
    class frame_reader {
    public:
        explicit frame_reader(asio::ip::tcp::socket& socket) : socket_(socket) {}
        bool next(op& o, std::string& body) {
            uint32_t size = 0;
            if (!fill(sizeof(size))) return false;
            std::memcpy(&size, buffer_.data() + pos_, sizeof(size));
            if (size == 0 || size > (64u << 20) || !fill(sizeof(size) + size)) return false;
            o = static_cast<op>(static_cast<uint8_t>(buffer_[pos_ + sizeof(size)]));
            body.assign(buffer_, pos_ + sizeof(size) + 1, size - 1);
            pos_ += sizeof(size) + size;
            return true;
        }

    private:
        bool fill(size_t n) {   // at least n bytes from pos_
            if (buffer_.size() - pos_ >= n) return true;
            buffer_.erase(0, pos_);
            pos_ = 0;
            char chunk[65536];
            while (buffer_.size() < n) {
                std::error_code ec;
                const size_t got = socket_.read_some(asio::buffer(chunk), ec);
                if (ec) return false;
                buffer_.append(chunk, got);
            }
            return true;
        }
        asio::ip::tcp::socket& socket_;
        std::string buffer_;
        size_t pos_ = 0;
    };

    inline bool read_frame(asio::ip::tcp::socket& socket, op& o, std::string& body) {
        uint32_t size = 0;
        std::error_code ec;
        asio::read(socket, asio::buffer(&size, sizeof(size)), ec);
        if (ec || size == 0 || size > (64u << 20)) return false;
        std::string frame(size, '\0');
        asio::read(socket, asio::buffer(frame.data(), frame.size()), ec);
        if (ec) return false;
        o = static_cast<op>(static_cast<uint8_t>(frame[0]));
        body.assign(frame, 1, std::string::npos);
        return true;
    }

}
