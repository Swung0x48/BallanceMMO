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
//
// Step SPX (the user's decision of 2026-10-29: "启动时如果是幽灵球的话，那就
// 把间距和出生冲量去掉" - when balls start as spawn ghosts, drop the spacing
// and the spawn impulse): both existed only to pull coincident spawns apart,
// and a spawn ghost (below) passes through every other ball and turns solid
// only at the first PSI its core is outside every spawn area while it
// overlaps no other ball.  So while the spawn ghost covers
// every spawn birth the server puts 0 in both SessionStart fields
// (session_spawn_separation below) and every world spawns each ball at the
// resetpoint itself with no kick; the spacing table and the kick stay for a
// build with the ghost off.

#include <cstddef>
#include <cstdint>

namespace bmmo::session {
    constexpr uint32_t kSpawnDirections = 64;

    // Spawn spacing (used only with the spawn ghost off, step SPX): where a
    // member's ball is born relative to the resetpoint
    // when SessionStart carries a spacing (spawn_spacing, map units; 0 = every
    // ball at the resetpoint, the design 9.10 layout).  Balls born at one
    // point at one tick resolve their overlap by IVP's rescue pushes in an
    // order each world's object set gives the engine, and a rewind cannot
    // re-create "created coincident" (docs/lockstep-without-snapshots.md,
    // "The start pile"); born a diameter apart they never overlap.  The owner
    // applies the offset to its own ball before the kick and reports the
    // offset pose in its Physicalize event, so the server and every mirror
    // create the clone where the owner is - nothing else needs the spacing.
    // Join order 0 is at the resetpoint itself (a solo session is unchanged);
    // the rest spiral out on the resetpoint's lateral (right) and forward
    // (dir) axes in whole multiples of the spacing, 16 distinct cells.
    constexpr uint32_t kSpawnOffsets = 16;
    inline constexpr int kSpawnOffsetTable[kSpawnOffsets][2] = {   // {lateral, forward} in spacings
        {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1},
        {-1, -1}, {2, 0}, {-2, 0}, {0, 2}, {0, -2}, {2, 1}, {-2, 1}, {2, -1},
    };

    // The world-space offset of `join_order`'s spawn cell: `right` and `dir`
    // are the resetpoint's lateral and forward unit axes (the retail
    // resetpoint matrix's first and third rows).  Exact for whole multiples:
    // the same bits on every side.
    inline void spawn_offset(uint8_t join_order, float spacing, const float right[3], const float dir[3], float out[3]) {
        const int* cell = kSpawnOffsetTable[join_order % kSpawnOffsets];
        const float lateral = spacing * static_cast<float>(cell[0]);
        const float forward = spacing * static_cast<float>(cell[1]);
        for (int k = 0; k < 3; ++k) out[k] = lateral * right[k] + forward * dir[k];
    }

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

    // The spawn ghost (step SPGH, the user's rule of 2026-10-28: "开始的时候，
    // 在出生点区域是透明球（和未启用物理碰撞一样），然后远离出生点才物理化",
    // settled as "only ball-to-ball collision off"; physics_state.hpp
    // spawn_ghost_set).  A spawn area is a sphere of kSpawnGhostRadius
    // around the level start and around every resetpoint of the level; a
    // player ball is born a ghost (it passes through every other player
    // ball and is drawn translucent) and turns solid for the rest of its life
    // at the first PSI its core is outside every area while it overlaps no
    // other ball.  12 m was sized to hold every one of the 16 birth cells of
    // the 4.2 spacing (the farthest is sqrt(5) x 4.2 = 9.39 m out) with the
    // ball (2 m radius) inside; with the ghost on (step SPX) every ball is
    // born at the resetpoint itself.  A binary fraction: the same double
    // everywhere.
    constexpr double kSpawnGhostRadius = 12.0;
    // Whether every world runs the spawn ghost (protocol 15 on: the server's
    // world and both client hosts install it at the anchor).  A constant like
    // the switch below: every world runs one build.  false is the ghost-off
    // build - no rule installed, the configured spacing and impulse sent.
    constexpr bool kSpawnGhost = true;
    // Whether a respawn at a resetpoint is born a ghost as well (true), or
    // only a ball's first life (false): the one switch the report names for
    // the user (both are built behind it); every world runs one build, so the
    // switch is a constant, not a wire field.
    constexpr bool kSpawnGhostRespawns = true;

    // The spawn separation a session's SessionStart carries (step SPX, the
    // one rule; the server decides it once per session, every world reads
    // the two fields).  None when the spawn ghost covers every spawn birth -
    // the first life and the respawns alike (kSpawnGhostRespawns): the
    // fields are session-wide, so a build whose respawns are not ghosts keeps
    // the configured values for every spawn (its respawns at one point need
    // them).  A solo session has nothing to separate.  `members` is the
    // session's member count when it starts.
    // Flipping kSpawnGhostRespawns to false brings the spacing and the kick
    // back for first spawns too, which are still ghosts - against the user's
    // decision for them: reopen it with the user before building that.
    struct spawn_separation {
        float impulse = 0.0f;   // SessionStart::spawn_impulse, m/s
        float spacing = 0.0f;   // SessionStart::spawn_spacing, map units
    };
    constexpr bool kSpawnGhostCoversSpawns = kSpawnGhost && kSpawnGhostRespawns;
    inline spawn_separation session_spawn_separation(size_t members, float configured_impulse, float configured_spacing) {
        spawn_separation out;
        if (members > 1 && !kSpawnGhostCoversSpawns) {
            out.impulse = configured_impulse;
            out.spacing = configured_spacing;
        }
        return out;
    }
}
