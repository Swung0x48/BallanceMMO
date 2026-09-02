#pragma once

// Read-mostly view of the retail physics_RT.dll world (Ballance 1.13, IVP 2.1).
//
// The manager container layout is private to the retail DLL; the IVP object
// layouts come from the pinned, unmodified IVP sources in the Ballanced
// submodule.  Every access is guarded by structural probes performed in
// initialize().  The only writes are the session clock/RNG reset that both
// the client and the headless server perform at the same tick (design 3.1).

#include <cstddef>
#include <cstdint>
#include <string>

#include <physics/world_hash.hpp>

class CKContext;

namespace bmmo::physics {
    class retail_physics_view {
    public:
        bool initialize(CKContext* context, std::string& error);
        void shutdown();
        bool available() const { return manager_ != nullptr; }

        // Hash of the IVP world after the last physics step; identical field
        // order on the server (sim/physics_state.cpp).
        bool capture(world_hash& out, std::string& error) const;

        // TAS-style physics clock reset plus RNG cursor and smoothed-delta
        // reset.  Returns false when the DLL is not the recognised retail
        // build (the RNG cursor cannot be addressed).
        bool reset_session_clock(int seed, std::string& error) const;
        bool reset_random(int seed, std::string& error) const;

        const std::string& dll_sha256() const { return dll_sha256_; }
        bool rng_addressable() const { return ivp_seed_ != nullptr; }

    private:
        CKContext* context_ = nullptr;
        void* manager_ = nullptr;
        std::string dll_sha256_;
        int* ivp_seed_ = nullptr;
        int* qh_seed_ = nullptr;
        size_t time_factor_offset_ = 0;
    };
}
