#pragma once

// The coordinator's wire (docs/canned-tests-plan.md, step T1b): one TCP
// connection per process of a coordinated run, frames of
//     u32 size (of what follows) | u8 op | body
// in little-endian.  A process's transport calls go to the coordinator as ops
// tagged with the sending thread's rank and a sequence (the coordinator applies
// one step's ops in the order process, rank, sequence); the coordinator answers
// with what the network delivers and the time.  Header-only.

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

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
        // coordinator -> process
        welcome = 64,   // index u32
        deliver,        // local u32, received_us i64, bytes str
        status,         // local u32, state i32, old_state i32, reason i32, debug str, description str
        link,           // local u32, ping_ms i32
        advance,        // now_us i64
        stop,
    };

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

    inline bool write_frame(asio::ip::tcp::socket& socket, op o, const std::string& body) {
        std::string frame;
        const auto size = static_cast<uint32_t>(1 + body.size());
        frame.append(reinterpret_cast<const char*>(&size), sizeof(size));
        frame.push_back(static_cast<char>(o));
        frame.append(body);
        std::error_code ec;
        asio::write(socket, asio::buffer(frame), ec);
        return !ec;
    }

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
