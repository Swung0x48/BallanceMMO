#ifndef BALLANCEMMOSERVER_CAMERA_HEADING_HPP
#define BALLANCEMMOSERVER_CAMERA_HEADING_HPP
// A player's camera in a physics session: one base and its quarter turns
// (protocol 30, docs/relay-protocol-design.md 4.7).
//
// The camera rows of an input frame are the direction reference of the
// ball's forces.  The retail camera only ever turns by 90 degrees about the
// vertical axis, but its script computes each turn with sines and cosines, so
// one heading comes in several bit patterns (up to 1.3e-5 apart).  The owner
// snaps what it observes (heading_snap) to the exact quarter turns of its
// first camera, the base: a quarter turn swaps two components and negates
// one, exact on every host.  Every world then uses the same rows, and on the
// wire a frame's camera is a 3-bit heading code against the stream's base
// (camera_bases); a frame no quarter turn reaches goes inline and is the
// base from its tick on (another map's orientation, say).
//
// CK-free, shared by the server, the headless client and the retail mod.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "../entity/session.hpp"

namespace bmmo::session {
    // Bit for bit: == would call -0.0 and +0.0 the same and the frame a
    // client rebuilds from a heading code must be the bytes the server
    // applied.
    inline bool same_camera(const input_frame& a, const input_frame& b) {
        return std::memcmp(a.cam_right, b.cam_right, sizeof(a.cam_right)) == 0
            && std::memcmp(a.cam_up, b.cam_up, sizeof(a.cam_up)) == 0
            && std::memcmp(a.cam_dir, b.cam_dir, sizeof(a.cam_dir)) == 0;
    }

    // A camera basis, the three rows of an input frame bit for bit.
    using camera_rows = std::array<float, 9>;
    inline camera_rows camera_of(const input_frame& f) {
        camera_rows rows;
        std::memcpy(rows.data(), f.cam_right, sizeof(f.cam_right));
        std::memcpy(rows.data() + 3, f.cam_up, sizeof(f.cam_up));
        std::memcpy(rows.data() + 6, f.cam_dir, sizeof(f.cam_dir));
        return rows;
    }
    inline void set_camera(input_frame& f, const camera_rows& rows) {
        std::memcpy(f.cam_right, rows.data(), sizeof(f.cam_right));
        std::memcpy(f.cam_up, rows.data() + 3, sizeof(f.cam_up));
        std::memcpy(f.cam_dir, rows.data() + 6, sizeof(f.cam_dir));
    }
    inline bool same_rows(const camera_rows& a, const camera_rows& b) {
        return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
    }
    // No camera yet (the rows before the first one was seen).
    inline bool zero_camera(const camera_rows& rows) {
        return same_rows(rows, camera_rows{});
    }
    // `k` quarter turns about the vertical axis: each row (x, y, z) becomes
    // (-z, y, x) per turn.
    inline camera_rows quarter_turns(camera_rows rows, int k) {
        for (int turn = 0; turn < (k & 3); ++turn)
            for (int r = 0; r < 9; r += 3) {
                const float x = rows[r];
                rows[r] = -rows[r + 2];
                rows[r + 2] = x;
            }
        return rows;
    }

    // A frame's camera on the wire: 0-3 quarter turns of the stream's base,
    // kHeadingNone the zero rows, kHeadingInline the rows follow (and are the
    // base from that tick on).  It rides the top bits of the ball_type byte.
    constexpr uint8_t kHeadingNone = 4;
    constexpr uint8_t kHeadingInline = 7;
    constexpr int kHeadingShift = 5;
    constexpr uint8_t kBallTypeMask = 0x1F;
    inline bool valid_heading(uint8_t code) { return code < 5 || code == kHeadingInline; }

    // The owner's snap: rows within kHeadingTolerance of a quarter turn of
    // the base become that turn exactly; others (the first camera, a turn the
    // base does not reach) become the base.  The retail turns are 1.3e-5 off
    // at most; the nearest other orientation a map could have is far beyond.
    constexpr float kHeadingTolerance = 1e-3f;
    class heading_snap {
    public:
        camera_rows apply(const camera_rows& seen) {
            if (zero_camera(seen)) return seen;
            if (base_)
                for (int k = 0; k < 4; ++k)
                    if (const camera_rows turned = quarter_turns(*base_, k); within_tolerance(turned, seen)) return turned;
            base_ = seen;
            return seen;
        }
        void reset() { base_.reset(); }

    private:
        static bool within_tolerance(const camera_rows& a, const camera_rows& b) {
            for (size_t i = 0; i < a.size(); ++i)
                if (!(std::fabs(a[i] - b[i]) <= kHeadingTolerance)) return false;
            return true;
        }
        std::optional<camera_rows> base_;
    };

    // One stream's bases by tick (a player's frames, either end of a wire):
    // what a heading code means at a tick.  The encoder records a base when a
    // frame goes inline; the decoder records the inline frames and the bases
    // a message attaches.  Lossless whatever the rows: a frame no turn of the
    // base reproduces bit for bit goes inline.
    class camera_bases {
    public:
        struct base {
            uint32_t since = 0;    // the tick it is the base from
            camera_rows rows{};
        };

        // The base in force at `tick`, if any.
        const base* at(uint32_t tick) const {
            const base* found = nullptr;
            for (const auto& b: changes_)
                if (static_cast<int32_t>(b.since - tick) <= 0) found = &b;
            return found;
        }
        // The code of `rows` at `tick` (encoder; a frame's code is the same
        // however often it is asked).
        uint8_t encode(uint32_t tick, const camera_rows& rows) {
            if (zero_camera(rows)) return kHeadingNone;
            if (const base* b = at(tick)) {
                if (b->since == tick && same_rows(b->rows, rows)) return kHeadingInline;
                if (b->since != tick)
                    for (int k = 0; k < 4; ++k)
                        if (same_rows(quarter_turns(b->rows, k), rows)) return static_cast<uint8_t>(k);
            }
            set(tick, rows);
            return kHeadingInline;
        }
        // The rows of a non-inline code at `tick` (decoder); false without a
        // base for a turn.
        bool decode(uint32_t tick, uint8_t code, camera_rows& rows) const {
            if (code == kHeadingNone) {
                rows = {};
                return true;
            }
            const base* b = code < 4 ? at(tick) : nullptr;
            if (!b) return false;
            rows = quarter_turns(b->rows, code);
            return true;
        }
        // `rows` is the base from `since` on (until a later recorded base).
        void set(uint32_t since, const camera_rows& rows) {
            auto it = std::find_if(changes_.begin(), changes_.end(),
                                   [since](const base& b) { return static_cast<int32_t>(b.since - since) >= 0; });
            if (it != changes_.end() && it->since == since) it->rows = rows;
            else changes_.insert(it, base{since, rows});
        }
        // Forget what no tick from `tick` on needs: every base before the one
        // in force at `tick`.
        void forget_before(uint32_t tick) {
            size_t keep = 0;
            while (keep + 1 < changes_.size() && static_cast<int32_t>(changes_[keep + 1].since - tick) <= 0) ++keep;
            changes_.erase(changes_.begin(), changes_.begin() + static_cast<std::ptrdiff_t>(keep));
        }
        // Forget the bases from `tick` on (a numbering that replaces them).
        void forget_from(uint32_t tick) {
            changes_.erase(std::remove_if(changes_.begin(), changes_.end(),
                                          [tick](const base& b) { return static_cast<int32_t>(b.since - tick) >= 0; }),
                           changes_.end());
        }
        void clear() { changes_.clear(); }
        size_t size() const { return changes_.size(); }

    private:
        std::vector<base> changes_;   // by since, ascending
    };

    // Bytes a message spends attaching a base: since u32 and the rows.
    constexpr size_t kCameraBaseBytes = sizeof(uint32_t) + sizeof(camera_rows);
}

#endif //BALLANCEMMOSERVER_CAMERA_HEADING_HPP
