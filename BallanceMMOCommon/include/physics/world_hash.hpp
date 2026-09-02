#pragma once

// Physics world hashing shared by the client (retail physics_RT.dll view) and
// the headless server (Ballanced physics_RT).  Both sides feed exactly the
// same fields in the same order, so equal hashes mean equal IVP state.

#include <cstddef>
#include <cstdint>

namespace bmmo::physics {
    struct fnv1a64 {
        uint64_t value = 14695981039346656037ULL;
        void feed(const void* data, size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; ++i) {
                value ^= bytes[i];
                value *= 1099511628211ULL;
            }
        }
        template <class T>
        void feed(const T& v) { feed(&v, sizeof(v)); }
    };

    struct world_hash {
        uint64_t hash = 0;       // environment clock + every movable core
        uint64_t pose = 0;       // movable cores only, without absolute times
        uint64_t surfaces = 0;   // order-independent signature of every body's collision surface
        int cores = 0;
        // Probe: the game ball when simulated (name starts with "Ball_"), else
        // the first simulated core; lets a replay show the size of a divergence.
        char probe_name[32] = {};
        double probe_position[3] = {};
        float probe_speed[3] = {};
        float probe_rot_speed[3] = {};
        double ivp_time = 0.0;
        int ivp_seed = 0;
        float delta_time_ms = 0.0f;
        float physics_delta_time = 0.0f;
        float time_factor = 0.0f;
    };

    // Per-core state in the canonical order (matches BallanceTAS's CoreState).
    struct core_state {
        double position[3];
        double q_last_psi[4];
        double q_next_psi[4];
        float speed[3];
        float rot_speed[3];
        float speed_change[3];
        float rot_speed_change[3];
        float delta_psis[3];
        uint8_t movement_state;
        float i_delta_time;
        double time_of_last_psi;
    };

    struct environment_state {
        double current_time;
        double time_of_next_psi;
        double time_of_last_psi;
        short next_movement_check;
        int ivp_seed;
        float delta_time_ms;
        float physics_delta_time;
    };

    inline void feed_environment(fnv1a64& hasher, const environment_state& e) {
        hasher.feed(e.current_time);
        hasher.feed(e.time_of_next_psi);
        hasher.feed(e.time_of_last_psi);
        hasher.feed(e.next_movement_check);
        hasher.feed(e.ivp_seed);
        hasher.feed(e.delta_time_ms);
        hasher.feed(e.physics_delta_time);
    }

    // Everything about a core except its absolute PSI time stamp, so two
    // worlds started at different wall-clock ticks can still be compared.
    inline void feed_core_pose(fnv1a64& hasher, const core_state& c) {
        hasher.feed(c.position, sizeof(c.position));
        hasher.feed(c.q_last_psi, sizeof(c.q_last_psi));
        hasher.feed(c.q_next_psi, sizeof(c.q_next_psi));
        hasher.feed(c.speed, sizeof(c.speed));
        hasher.feed(c.rot_speed, sizeof(c.rot_speed));
        hasher.feed(c.speed_change, sizeof(c.speed_change));
        hasher.feed(c.rot_speed_change, sizeof(c.rot_speed_change));
        hasher.feed(c.delta_psis, sizeof(c.delta_psis));
        hasher.feed(c.movement_state);
        hasher.feed(c.i_delta_time);
    }

    inline void feed_core(fnv1a64& hasher, const core_state& c) {
        hasher.feed(c.position, sizeof(c.position));
        hasher.feed(c.q_last_psi, sizeof(c.q_last_psi));
        hasher.feed(c.q_next_psi, sizeof(c.q_next_psi));
        hasher.feed(c.speed, sizeof(c.speed));
        hasher.feed(c.rot_speed, sizeof(c.rot_speed));
        hasher.feed(c.speed_change, sizeof(c.speed_change));
        hasher.feed(c.rot_speed_change, sizeof(c.rot_speed_change));
        hasher.feed(c.delta_psis, sizeof(c.delta_psis));
        hasher.feed(c.movement_state);
        hasher.feed(c.i_delta_time);
        hasher.feed(c.time_of_last_psi);
    }
}
