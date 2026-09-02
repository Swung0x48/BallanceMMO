#include "retail_physics_view.hpp"

#include "bml_includes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "retail_physics_rt_layout.hpp"

#include <openssl/evp.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace bmmo::physics {
    namespace {
        // Retail physics_RT.dll (Ballance 1.13, 512000 bytes).  The RVAs come
        // from BallanceTAS (src/Game/physics_RT.cpp) and are only used when
        // the loaded DLL hashes to this exact build.
        constexpr const char* kRetailPhysicsSha256 =
            "e72e4afcfa5c33a7d3d27776137f8c997b3c52d89d8a8a4745f1ca21e45893ec";
        constexpr size_t kRetailIvpSeedRva = 0x685B4;
        constexpr size_t kRetailQhSeedRva = 0x70BF0;
        constexpr int kEnvironmentMagic = 123456;
        constexpr int kMovementCheckCount = 10;

        const CKGUID kPhysicsManagerGuid(0x6BED328B, 0x141F5148);

        std::string sha256_of_file(const std::wstring& path) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) return {};
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            if (!ctx) return {};
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
            std::vector<char> buffer(1 << 16);
            while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || stream.gcount() > 0)
                EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(stream.gcount()));
            unsigned char digest[32];
            unsigned int length = 0;
            EVP_DigestFinal_ex(ctx, digest, &length);
            EVP_MD_CTX_free(ctx);
            static const char* hex = "0123456789abcdef";
            std::string out;
            for (unsigned i = 0; i < length; ++i) {
                out.push_back(hex[digest[i] >> 4]);
                out.push_back(hex[digest[i] & 0xF]);
            }
            return out;
        }

        HMODULE module_containing(const void* address) {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(address), &module))
                return nullptr;
            return module;
        }

        CKIpionManager* as_manager(void* manager) { return static_cast<CKIpionManager*>(manager); }
    }

    bool retail_physics_view::initialize(CKContext* context, std::string& error) {
        error.clear();
        shutdown();
        if (!context) {
            error = "no CKContext";
            return false;
        }
        auto* manager = static_cast<CKIpionManager*>(context->GetManagerByGuid(kPhysicsManagerGuid));
        if (!manager) {
            error = "the physics manager is not registered";
            return false;
        }
        IVP_Environment* environment = manager->GetEnvironment();
        if (!environment) {
            error = "physics environment does not exist yet";
            return false;
        }
        // Same structural probes as BallanceTAS ValidatePhysicsLayout.
        if (environment->environment_magic_number != kEnvironmentMagic) {
            error = "IVP environment probe failed (magic number)";
            return false;
        }
        if (std::fabs(environment->delta_PSI_time - 1.0 / 66.0) > 1.0e-3) {
            error = "IVP environment probe failed (PSI rate)";
            return false;
        }
        const float factor = manager->GetPhysicsTimeFactor();
        if (!(factor > 0.0005f && factor < 0.02f)) {
            error = "physics manager probe failed (time factor)";
            return false;
        }
        if (manager->m_MovableObjects.len() < 0 || manager->m_MovableObjects.len() > 65535) {
            error = "physics manager probe failed (movable list)";
            return false;
        }

        HMODULE module = module_containing(*reinterpret_cast<void* const*>(manager));
        wchar_t path[MAX_PATH] = {};
        if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) {
            error = "cannot locate the physics DLL module";
            return false;
        }
        dll_sha256_ = sha256_of_file(path);
        if (dll_sha256_ == kRetailPhysicsSha256) {
            auto* base = reinterpret_cast<std::byte*>(module);
            ivp_seed_ = reinterpret_cast<int*>(base + kRetailIvpSeedRva);
            qh_seed_ = reinterpret_cast<int*>(base + kRetailQhSeedRva);
        }
        context_ = context;
        manager_ = manager;
        return true;
    }

    void retail_physics_view::shutdown() {
        context_ = nullptr;
        manager_ = nullptr;
        ivp_seed_ = nullptr;
        qh_seed_ = nullptr;
        time_factor_offset_ = 0;
        dll_sha256_.clear();
    }

    bool retail_physics_view::capture(world_hash& out, std::string& error) const {
        error.clear();
        out = {};
        auto* manager = as_manager(manager_);
        IVP_Environment* environment = manager ? manager->GetEnvironment() : nullptr;
        if (!environment || environment->environment_magic_number != kEnvironmentMagic) {
            error = "physics view is not initialized or the environment disappeared";
            return false;
        }
        environment_state env{};
        env.current_time = environment->current_time.get_seconds();
        env.time_of_next_psi = environment->time_of_next_psi.get_seconds();
        env.time_of_last_psi = environment->time_of_last_psi.get_seconds();
        env.next_movement_check = environment->next_movement_check;
        env.ivp_seed = ivp_seed_ ? *ivp_seed_ : 0;
        env.delta_time_ms = manager->GetDeltaTime();
        env.physics_delta_time = manager->GetPhysicsDeltaTime();
        fnv1a64 hasher;
        feed_environment(hasher, env);

        std::vector<const IVP_Core*> seen;
        const int count = manager->m_MovableObjects.len();
        for (int i = 0; i < count; ++i) {
            IVP_Real_Object* object = manager->m_MovableObjects.element_at(i);
            const IVP_Core* core = object ? object->get_core() : nullptr;
            if (!core) continue;
            bool duplicate = false;
            for (const auto* known: seen) duplicate |= known == core;
            if (duplicate) continue;
            seen.push_back(core);
            core_state c{};
            for (int k = 0; k < 3; ++k) c.position[k] = core->pos_world_f_core_last_psi.k[k];
            c.q_last_psi[0] = core->q_world_f_core_last_psi.x;
            c.q_last_psi[1] = core->q_world_f_core_last_psi.y;
            c.q_last_psi[2] = core->q_world_f_core_last_psi.z;
            c.q_last_psi[3] = core->q_world_f_core_last_psi.w;
            c.q_next_psi[0] = core->q_world_f_core_next_psi.x;
            c.q_next_psi[1] = core->q_world_f_core_next_psi.y;
            c.q_next_psi[2] = core->q_world_f_core_next_psi.z;
            c.q_next_psi[3] = core->q_world_f_core_next_psi.w;
            for (int k = 0; k < 3; ++k) {
                c.speed[k] = core->speed.k[k];
                c.rot_speed[k] = core->rot_speed.k[k];
                c.speed_change[k] = core->speed_change.k[k];
                c.rot_speed_change[k] = core->rot_speed_change.k[k];
                c.delta_psis[k] = core->delta_world_f_core_psis.k[k];
            }
            c.movement_state = static_cast<uint8_t>(core->movement_state);
            c.i_delta_time = core->i_delta_time;
            c.time_of_last_psi = core->time_of_last_psi.get_seconds();
            feed_core(hasher, c);
        }
        out.hash = hasher.value;
        out.cores = static_cast<int>(seen.size());
        out.ivp_time = env.current_time;
        out.ivp_seed = env.ivp_seed;
        out.delta_time_ms = env.delta_time_ms;
        out.physics_delta_time = env.physics_delta_time;
        out.time_factor = manager->GetPhysicsTimeFactor();
        return true;
    }

    bool retail_physics_view::reset_session_clock(int seed, std::string& error) const {
        error.clear();
        auto* manager = as_manager(manager_);
        IVP_Environment* environment = manager ? manager->GetEnvironment() : nullptr;
        if (!environment || environment->environment_magic_number != kEnvironmentMagic) {
            error = "physics view is not initialized or the environment disappeared";
            return false;
        }
        // BallanceTAS GameInterface::ResetPhysicsTime, plus the movement-check
        // phase, the RNG cursor and the smoothed CK delta.
        environment->time_manager->base_time = IVP_Time{};
        environment->current_time = IVP_Time{};
        environment->time_of_last_psi = IVP_Time{};
        environment->time_of_next_psi = IVP_Time{} + environment->delta_PSI_time;
        environment->next_movement_check = kMovementCheckCount;
        manager->SetDeltaTime(1000.0f / 66.0f);
        manager->SetPhysicsDeltaTime((1000.0f / 66.0f) * manager->GetPhysicsTimeFactor());
        if (!ivp_seed_) {
            error = "RNG cursor is not addressable for this physics DLL";
            return false;
        }
        *ivp_seed_ = seed == 0 ? 1 : seed;
        return true;
    }

    bool retail_physics_view::reset_random(int seed, std::string& error) const {
        error.clear();
        if (!ivp_seed_ || !qh_seed_) {
            error = "the loaded physics DLL is not the recognised retail build; RNG reset unavailable";
            return false;
        }
        *ivp_seed_ = seed == 0 ? 1 : seed;
        *qh_seed_ = seed < 1 ? 1 : seed;
        return true;
    }
}
