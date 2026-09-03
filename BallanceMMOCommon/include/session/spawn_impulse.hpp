#pragma once

// Spawn impulse of a physics session (design 9.10).  Every player physicalizes
// its ball at the level's retail resetpoint (spawn and respawn alike) and the
// ball is kicked sideways right away, so balls that appear in the same tick at
// the same point move apart instead of resting inside each other.  The server,
// the player and every mirror derive the kick's direction from the same
// integers, so the very same impulse is applied on every side without being
// exchanged: the SessionStart seed, the player's join order and the tick of
// the Physicalize event.  The speed comes from SessionStart (spawn_impulse,
// metres per second, 0 = no kick: a solo session stays bit-exact with a solo
// recording).

#include <cstdint>

namespace bmmo::session {
    constexpr uint32_t kSpawnDirections = 64;

    // Unit vectors in the world XZ plane (Y up) along the golden-angle
    // sequence: any two entries are at least 2.9 degrees apart, so players
    // with different join orders never share a direction.  Float literals,
    // the same bits on every platform (no trigonometry at run time).
    inline constexpr float kSpawnDirectionTable[kSpawnDirections][3] = {
        {1.0f, 0.0f, 0.0f},
        {-0.737368878f, 0.0f, 0.675490294f},
        {0.0874257247f, 0.0f, -0.996171041f},
        {0.608438861f, 0.0f, 0.793600751f},
        {-0.984713485f, 0.0f, -0.17418195f},
        {0.843755295f, 0.0f, -0.536728053f},
        {-0.259604305f, 0.0f, 0.965715074f},
        {-0.460907025f, 0.0f, -0.887448429f},
        {0.939321296f, 0.0f, 0.343038631f},
        {-0.924345556f, 0.0f, 0.381556408f},
        {0.423845995f, 0.0f, -0.905734273f},
        {0.299283864f, 0.0f, 0.95416412f},
        {-0.86521121f, 0.0f, -0.501407581f},
        {0.976675774f, 0.0f, -0.214719429f},
        {-0.575129429f, 0.0f, 0.81806243f},
        {-0.12851069f, 0.0f, -0.991708124f},
        {0.764648995f, 0.0f, 0.644446983f},
        {-0.999146054f, 0.0f, 0.0413178262f},
        {0.708829414f, 0.0f, -0.705379941f},
        {-0.0461914459f, 0.0f, 0.998932605f},
        {-0.640709145f, 0.0f, -0.767783688f},
        {0.991069413f, 0.0f, 0.133346988f},
        {-0.820858337f, 0.0f, 0.57113185f},
        {0.219481369f, 0.0f, -0.975616691f},
        {0.497180875f, 0.0f, 0.86764692f},
        {-0.952692777f, 0.0f, -0.30393498f},
        {0.907791134f, 0.0f, -0.419422529f},
        {-0.386061082f, 0.0f, 0.92247322f},
        {-0.338452279f, 0.0f, -0.940983557f},
        {0.885189437f, 0.0f, 0.46523076f},
        {-0.966970005f, 0.0f, 0.25489019f},
        {0.540837738f, 0.0f, -0.841126947f},
        {0.169376173f, 0.0f, 0.985551476f},
        {-0.790623175f, 0.0f, -0.612303026f},
        {0.996585674f, 0.0f, -0.082565086f},
        {-0.679079346f, 0.0f, 0.734064875f},
        {0.00487827716f, 0.0f, -0.999988101f},
        {0.671885167f, 0.0f, 0.740655333f},
        {-0.995732701f, 0.0f, -0.0922842829f},
        {0.796559442f, 0.0f, -0.604560217f},
        {-0.178983583f, 0.0f, 0.983852061f},
        {-0.532605594f, 0.0f, -0.846363563f},
        {0.964437162f, 0.0f, 0.264312242f},
        {-0.889686302f, 0.0f, 0.456572321f},
        {0.347616819f, 0.0f, -0.937636682f},
        {0.377042655f, 0.0f, 0.926195895f},
        {-0.903655857f, 0.0f, -0.428259375f},
        {0.955612756f, 0.0f, -0.294625626f},
        {-0.505622355f, 0.0f, 0.86275491f},
        {-0.209952379f, 0.0f, -0.977711613f},
        {0.815247055f, 0.0f, 0.579113321f},
        {-0.992323234f, 0.0f, 0.123671334f},
        {0.648169484f, 0.0f, -0.761496106f},
        {0.0364432232f, 0.0f, 0.999335725f},
        {-0.701913682f, 0.0f, -0.712262019f},
        {0.998695385f, 0.0f, 0.0510639664f},
        {-0.770900109f, 0.0f, 0.63695606f},
        {0.138180112f, 0.0f, -0.990407117f},
        {0.56712068f, 0.0f, 0.823634709f},
        {-0.974534392f, 0.0f, -0.224238086f},
        {0.870061982f, 0.0f, -0.492942337f},
        {-0.308578863f, 0.0f, 0.951198762f},
        {-0.414989082f, 0.0f, -0.909826391f},
        {0.92057893f, 0.0f, 0.390556569f},
    };

    // Pseudo-random rotation of the table per (seed, tick): players that spawn
    // in the same tick share the offset and differ by join order.  A
    // splitmix-style integer hash, no floating point.
    inline uint32_t spawn_direction_index(int32_t seed, uint8_t join_order, uint32_t tick) {
        uint32_t h = static_cast<uint32_t>(seed) * 0x9E3779B1u;
        h ^= tick + 0x7F4A7C15u + (h << 6) + (h >> 2);
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;
        return (static_cast<uint32_t>(join_order) + (h & (kSpawnDirections - 1))) & (kSpawnDirections - 1);
    }

    inline const float* spawn_direction(int32_t seed, uint8_t join_order, uint32_t tick) {
        return kSpawnDirectionTable[spawn_direction_index(seed, join_order, tick)];
    }
}
