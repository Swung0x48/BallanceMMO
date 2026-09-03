#pragma once

// The generator behind the Virtools "Random" building block once BallanceMMO
// hooks it (design 9.10).  The retail block calls the C runtime's rand(),
// whose algorithm and call history differ between the game (MSVCRT.dll) and
// the headless engine (UCRT, glibc, bionic), so the trafo explosion pieces it
// feeds could never replay.  This is the Microsoft runtime's generator
// (state = state * 214013 + 2531011, value = bits 16..30, RAND_MAX 32767) so
// the numbers keep the retail look, but the state is ours: both engines seed
// it at the session anchor and the server saves/restores it per world.
// Integer arithmetic only, so every platform draws the same sequence.

#include <cstdint>

namespace bmmo::physics {
    struct deterministic_random {
        static constexpr int32_t kMax = 32767;   // the Microsoft runtime's RAND_MAX

        uint32_t state = 1;

        void reset(int32_t seed) { state = static_cast<uint32_t>(seed); }

        int32_t next() {
            state = state * 214013u + 2531011u;
            return static_cast<int32_t>((state >> 16) & 0x7fffu);
        }
    };
}
