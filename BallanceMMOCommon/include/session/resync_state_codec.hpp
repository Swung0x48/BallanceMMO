#pragma once

// Protocol 34: a resync or late-join state travels as one zstd frame of its
// body (session_resync_state_msg::body_bytes) - plain, or against the body of
// the last state this member decoded (a raw-content prefix, ZSTD_*_refPrefix),
// which both sides hold byte for byte: the server the body it last sent the
// member, the client the body it last decoded.  Measured on 586 states of the
// 2026-11-08 public test (zstd 1.5.7, level 3): plain 0.886x of the protocol-33
// size, against the previous state 0.343x (mean 109 KB -> about 39 KB); 1 ms to
// compress against zlib's 3.5 ms.  A session's start state as a second
// reference was measured too and is not used: on the states that lack a
// previous one it lost to plain (0.901x against 0.891x), and the rest of a
// state (the server's row numbering, its clones and ledger) is not something a
// client can reproduce.
//
// Exactness does not depend on the reference: the frame carries zstd's
// content checksum (XXH64 of the decoded body), checked by every decode, and a
// state names its reference by (tick, that reference's frame check).  A body
// that does not decode is refused and asked for again without a reference.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <zstd.h>

namespace bmmo::session {

    constexpr int kResyncZstdLevel = 3;
    // A body's largest decoded size: a forged raw_size is refused before any
    // allocation (the largest measured is 682 KB).
    constexpr uint32_t kMaxResyncRaw = 8u << 20;
    constexpr int kResyncMaxWindowLog = 24;

    // session_resync_state_msg::ref_kind
    enum resync_ref_kind : uint8_t {
        RESYNC_REF_NONE = 0,       // plain
        RESYNC_REF_PREVIOUS = 1,   // against the last state this member decoded
    };

    // A decoded state as its reference: its tick S and the frame check of the
    // frame it came in (zstd's content checksum, the frame's last 4 bytes).
    struct resync_ref_id {
        uint32_t tick = 0;
        uint32_t check = 0;
        bool held = false;
        bool operator==(const resync_ref_id&) const = default;
    };

    // A frame's check: its last 4 bytes, little-endian - the low 32 bits of
    // the XXH64 of its decoded content (ZSTD_c_checksumFlag is set).
    inline uint32_t resync_frame_check(const std::vector<uint8_t>& frame) {
        if (frame.size() < 4) return 0;
        const uint8_t* p = frame.data() + frame.size() - 4;
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16
             | static_cast<uint32_t>(p[3]) << 24;
    }

    // One compression and one decompression context, reused (each state
    // would otherwise allocate a context on the sim thread).
    class resync_codec {
    public:
        resync_codec() = default;
        ~resync_codec() {
            ZSTD_freeCCtx(c_);
            ZSTD_freeDCtx(d_);
        }
        resync_codec(const resync_codec&) = delete;
        resync_codec& operator=(const resync_codec&) = delete;

        // `raw` into one frame, against `prefix` when given.
        bool encode(const std::string& raw, const std::string* prefix, std::vector<uint8_t>& out) {
            if (!c_) c_ = ZSTD_createCCtx();
            if (!c_ || raw.size() > kMaxResyncRaw) return false;
            ZSTD_CCtx_reset(c_, ZSTD_reset_session_and_parameters);
            ZSTD_CCtx_setParameter(c_, ZSTD_c_compressionLevel, kResyncZstdLevel);
            ZSTD_CCtx_setParameter(c_, ZSTD_c_checksumFlag, 1);
            ZSTD_CCtx_setParameter(c_, ZSTD_c_contentSizeFlag, 1);
            // Set, not derived from the source size: zstd would shrink the
            // window below the prefix and match nothing in it.
            ZSTD_CCtx_setParameter(c_, ZSTD_c_windowLog, window_log((prefix ? prefix->size() : 0) + raw.size()));
            if (prefix && ZSTD_isError(ZSTD_CCtx_refPrefix(c_, prefix->data(), prefix->size()))) return false;
            out.resize(ZSTD_compressBound(raw.size()));
            const size_t n = ZSTD_compress2(c_, out.data(), out.size(), raw.data(), raw.size());
            if (ZSTD_isError(n)) return false;
            out.resize(n);
            return true;
        }

        // `frame` into `out`, against `prefix` when given: refused unless the
        // frame says it decodes to exactly `raw_size` bytes (at most
        // kMaxResyncRaw), all of it is one frame, and its checksum holds.
        bool decode(const std::vector<uint8_t>& frame, const std::string* prefix, uint32_t raw_size, std::string& out) {
            if (!d_) d_ = ZSTD_createDCtx();
            if (!d_ || raw_size > kMaxResyncRaw) return false;
            if (ZSTD_getFrameContentSize(frame.data(), frame.size()) != raw_size) return false;
            if (ZSTD_findFrameCompressedSize(frame.data(), frame.size()) != frame.size()) return false;
            ZSTD_DCtx_reset(d_, ZSTD_reset_session_and_parameters);
            ZSTD_DCtx_setParameter(d_, ZSTD_d_windowLogMax, kResyncMaxWindowLog);
            if (prefix && ZSTD_isError(ZSTD_DCtx_refPrefix(d_, prefix->data(), prefix->size()))) return false;
            out.resize(raw_size);
            const size_t n = ZSTD_decompressDCtx(d_, out.data(), out.size(), frame.data(), frame.size());
            return !ZSTD_isError(n) && n == raw_size;
        }

    private:
        static int window_log(size_t total) {
            int log = 21;
            while (log < kResyncMaxWindowLog && (size_t{1} << log) < total) ++log;
            return log;
        }

        ZSTD_CCtx* c_ = nullptr;
        ZSTD_DCtx* d_ = nullptr;
    };
}
