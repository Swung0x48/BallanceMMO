#pragma once

// The sensor rules of world agreement's proximity union, CK-free
// (docs/world-agreement-gaps-design.md §2.2, part A; built at step G1).
//
// A mechanism's "TT Scaleable Proximity" block measures, under the proximity
// union (the world option of step G2/G3), a per-block frame that every world
// puts on the same ball: the candidate nearest to the block's ObjectB under
// the block's OWN metric (its Check Axis, squared), the lowest player id on a
// tie.  These two functions are that rule; game/proximity_union.hpp applies
// it to the CK objects.  Header code compiled into the three targets (x64
// MSVC, x86 MSVC, GCC): plain float multiplies and adds in a fixed order, no
// VxMath, no container iteration order (§3 item 3; the unit test U1 pins the
// float results as hex constants).

#include <cstdint>
#include <string>
#include <vector>

namespace bmmo::session {

    // A candidate ball of the place point: its player and its position.
    struct probe_ball {
        uint32_t player = 0;
        float position[3] = {0.0f, 0.0f, 0.0f};
    };

    // A candidate as the core hands it to the host (§2.5): the player and the
    // name of the ball entity that has a body in this world.  CK-free, so the
    // client platform (session/client_platform.hpp) can carry it.
    struct probe_candidate {
        uint32_t player = 0;
        std::string entity;
        // Step Q7 (§2.10 "Binding"): the join order of the life of `player`
        // that the placed tick names - the mechanism slot this ball serves
        // under WORLD_PLAYER_MECHANISMS; -1 when unknown (no life holds the
        // tick) or when the world runs without the bit.
        int slot = -1;
    };

    // The Check Axis bits of TT Scaleable Proximity (SPX: A_X 1, A_Y 2, A_Z 4).
    constexpr uint8_t kProbeAxisX = 1u;
    constexpr uint8_t kProbeAxisY = 2u;
    constexpr uint8_t kProbeAxisZ = 4u;

    // The block's own metric (SPX:166-186): the difference b - a per axis,
    // z, then y, then x, each term d*d in float added in that order onto 0,
    // only the axes in `axes`.  Squared: the block compares squares.
    inline float probe_metric(const float a[3], const float b[3], uint8_t axes) {
        float sum = 0.0f;
        if (axes & kProbeAxisZ) {
            const float z = b[2] - a[2];
            const float zz = z * z;
            sum = sum + zz;
        }
        if (axes & kProbeAxisY) {
            const float y = b[1] - a[1];
            const float yy = y * y;
            sum = sum + yy;
        }
        if (axes & kProbeAxisX) {
            const float x = b[0] - a[0];
            const float xx = x * x;
            sum = sum + xx;
        }
        return sum;
    }

    // The index into `balls` (sorted by player id) of the smallest metric to
    // `reference`; the first (the lowest id) on a tie; -1 when `balls` is
    // empty.  The operands are as the block has them: ObjectA (the frame,
    // i.e. the ball) is posA, ObjectB (the reference) is posB, and SPX takes
    // posB - posA (the square makes the sign irrelevant to the bits).
    inline int probe_nearest(const std::vector<probe_ball>& balls, const float reference[3], uint8_t axes) {
        int best = -1;
        float best_metric = 0.0f;
        for (size_t i = 0; i < balls.size(); ++i) {
            const float m = probe_metric(balls[i].position, reference, axes);
            if (best < 0 || m < best_metric) {
                best = static_cast<int>(i);
                best_metric = m;
            }
        }
        return best;
    }

}
