#include "physics_view.hpp"

#include "bml_includes.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

namespace bmmo::physics {
    namespace {
        const CKGUID kPhysicsManagerGuid(0x6BED328B, 0x141F5148);

        std::string sha256_of_file(const std::string& path) {
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

        // The module that contains `address`: the manager's vtable lives in
        // whichever physics_RT the engine loaded.
        struct module_info {
            std::string path;
            bmmo_physics_api_fn entry = nullptr;
        };

        bool locate_module(const void* address, module_info& out) {
#ifdef _WIN32
            HMODULE module = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCSTR>(address), &module) || !module)
                return false;
            char path[MAX_PATH] = {};
            if (!GetModuleFileNameA(module, path, MAX_PATH)) return false;
            out.path = path;
            out.entry = reinterpret_cast<bmmo_physics_api_fn>(
                reinterpret_cast<void*>(GetProcAddress(module, BMMO_PHYSICS_API_SYMBOL)));
            return true;
#else
            Dl_info info{};
            if (!dladdr(address, &info) || !info.dli_fname) return false;
            out.path = info.dli_fname;
            if (void* handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD)) {
                out.entry = reinterpret_cast<bmmo_physics_api_fn>(dlsym(handle, BMMO_PHYSICS_API_SYMBOL));
                dlclose(handle);
            }
            return true;
#endif
        }
    }

    bool physics_view::initialize(CKContext* context, std::string& error) {
        error.clear();
        shutdown();
        if (!context) {
            error = "no CKContext";
            return false;
        }
        CKBaseManager* manager = context->GetManagerByGuid(kPhysicsManagerGuid);
        if (!manager) {
            error = "the physics manager is not registered";
            return false;
        }
        module_info module;
        if (!locate_module(*reinterpret_cast<void* const*>(manager), module)) {
            error = "cannot locate the physics plugin module";
            return false;
        }
        dll_path_ = module.path;
        dll_sha256_ = sha256_of_file(module.path);
        if (!module.entry) {
            error = "the loaded physics_RT has no BallanceMMO bridge (retail DLL?): " + module.path;
            return false;
        }
        const bmmo_physics_api_v2* api = module.entry(BMMO_PHYSICS_API_VERSION);
        if (!api || api->struct_size < sizeof(bmmo_physics_api_v2)
                || api->api_version != BMMO_PHYSICS_API_VERSION) {
            error = "the physics_RT bridge does not provide API version "
                  + std::to_string(BMMO_PHYSICS_API_VERSION);
            return false;
        }
        api_ = api;
        build_id_ = api->build_id ? api->build_id : "";
        context_ = context;
        manager_ = manager;
        return true;
    }

    void physics_view::shutdown() {
        context_ = nullptr;
        manager_ = nullptr;
        api_ = nullptr;
        build_id_.clear();
    }

    bool physics_view::capture(world_hash& out, std::string& error) const {
        error.clear();
        out = {};
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        bmmo_physics_world_hash raw{};
        if (!api_->capture_world_hash(manager_, &raw, text, sizeof(text))) {
            error = text;
            return false;
        }
        out.hash = raw.hash;
        out.pose = raw.pose;
        out.cores = raw.cores;
        out.ivp_time = raw.ivp_time;
        out.ivp_seed = raw.ivp_seed;
        out.delta_time_ms = raw.delta_time_ms;
        out.physics_delta_time = raw.physics_delta_time;
        out.time_factor = raw.time_factor;
        out.surfaces = raw.surfaces;
        std::snprintf(out.probe_name, sizeof(out.probe_name), "%s", raw.probe_name);
        for (int k = 0; k < 3; ++k) {
            out.probe_position[k] = raw.probe_position[k];
            out.probe_speed[k] = raw.probe_speed[k];
            out.probe_rot_speed[k] = raw.probe_rot_speed[k];
        }
        out.next_movement_check = raw.next_movement_check;
        out.time_of_last_psi = raw.time_of_last_psi;
        out.time_of_next_psi = raw.time_of_next_psi;
        return true;
    }

    bool physics_view::reset_session_clock(int seed, std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        if (!api_->reset_session_clock(manager_, seed, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    std::string physics_view::describe_movable_objects() const {
        if (!available()) return {};
        std::string text(4096, char{});
        const int32_t length = api_->describe_movable_objects(manager_, text.data(),
                                                              static_cast<uint32_t>(text.size()));
        text.resize(length > 0 && static_cast<size_t>(length) < text.size() ? static_cast<size_t>(length)
                                                                              : text.size() - 1);
        return text;
    }

    std::string physics_view::describe_physics_objects() const {
        if (!available()) return {};
        std::string text(16384, char{});
        const int32_t length = api_->describe_physics_objects(manager_, text.data(),
                                                              static_cast<uint32_t>(text.size()));
        text.resize(length > 0 && static_cast<size_t>(length) < text.size() ? static_cast<size_t>(length)
                                                                              : text.size() - 1);
        return text;
    }

    std::string physics_view::drain_event_log() const {
        if (!available()) return {};
        std::string text(262144, char{});
        const int32_t length = api_->drain_event_log(manager_, text.data(), static_cast<uint32_t>(text.size()));
        text.resize(length > 0 && static_cast<size_t>(length) < text.size() ? static_cast<size_t>(length)
                                                                              : text.size() - 1);
        return text;
    }

    std::string physics_view::describe_cores_exact() const {
        if (!available()) return {};
        std::string text(65536, char{});
        const int32_t length = api_->describe_cores_exact(manager_, text.data(), static_cast<uint32_t>(text.size()));
        text.resize(length > 0 && static_cast<size_t>(length) < text.size() ? static_cast<size_t>(length)
                                                                              : text.size() - 1);
        return text;
    }

    std::vector<bmmo_physics_body_state> physics_view::list_bodies() const {
        if (!available()) return {};
        std::vector<bmmo_physics_body_state> bodies(64);
        int32_t total = api_->list_bodies(manager_, bodies.data(), static_cast<int32_t>(bodies.size()));
        if (total > static_cast<int32_t>(bodies.size())) {
            bodies.resize(static_cast<size_t>(total));
            total = api_->list_bodies(manager_, bodies.data(), static_cast<int32_t>(bodies.size()));
        }
        bodies.resize(total > 0 ? std::min(static_cast<size_t>(total), bodies.size()) : 0);
        return bodies;
    }

    bool physics_view::get_body_state(const char* entity_name, bmmo_physics_body_state& out,
                                      std::string& error) const {
        error.clear();
        out = {};
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        if (!api_->get_body_state(manager_, entity_name, &out, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::set_body_state(const char* entity_name, const double position[3],
                                      const double rotation[4], const float linear[3],
                                      const float angular[3], bool wake, std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        if (!api_->set_body_state(manager_, entity_name, position, rotation, linear, angular,
                                  wake ? 1 : 0, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::physicalize(const char* entity_name, const bmmo_physics_ball_recipe& recipe,
                                   const char* collision_group, std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        if (!api_->physicalize(manager_, entity_name, &recipe, collision_group, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::set_body_group(const char* entity_name, const char* collision_group, std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_body_group) {
            error = "the physics_RT bridge lacks set_body_group";
            return false;
        }
        char text[256] = {};
        if (!api_->set_body_group(manager_, entity_name, collision_group, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::navigation_create(const char* ball_entity, const char* direction_ref_entity, uint32_t behavior_id,
                                         const float (*directions)[3], int leaf_count, float force_value,
                                         std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->navigation_create) {
            error = "the physics_RT bridge lacks navigation_create";
            return false;
        }
        char text[256] = {};
        if (!api_->navigation_create(manager_, ball_entity, direction_ref_entity, behavior_id, directions, leaf_count,
                                     force_value, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::navigation_input(const char* ball_entity, uint8_t keys, const float right[3], const float up[3],
                                        const float dir[3], bool active, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->navigation_input) {
            error = "the physics_RT bridge lacks navigation_input";
            return false;
        }
        char text[256] = {};
        if (!api_->navigation_input(manager_, ball_entity, keys, right, up, dir, active ? 1 : 0, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::navigation_set_ball(const char* ball_entity, const char* new_ball_entity, float force_value,
                                           std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->navigation_set_ball) {
            error = "the physics_RT bridge lacks navigation_set_ball";
            return false;
        }
        char text[256] = {};
        if (!api_->navigation_set_ball(manager_, ball_entity, new_ball_entity, force_value, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::navigation_destroy(const char* ball_entity, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->navigation_destroy) {
            error = "the physics_RT bridge lacks navigation_destroy";
            return false;
        }
        char text[256] = {};
        if (!api_->navigation_destroy(manager_, ball_entity, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::set_body_guard(bool enable, const char* except_entity, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_body_guard) {
            error = "the physics_RT bridge lacks set_body_guard";
            return false;
        }
        char text[256] = {};
        if (!api_->set_body_guard(manager_, enable ? 1 : 0, except_entity, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::get_clock(float& time_factor, float& physics_delta, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->get_clock) {
            error = "the physics_RT bridge lacks get_clock";
            return false;
        }
        char text[256] = {};
        if (!api_->get_clock(manager_, &time_factor, &physics_delta, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::step_physics(float delta_ms, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->step_physics) {
            error = "the physics_RT bridge lacks step_physics";
            return false;
        }
        char text[256] = {};
        if (!api_->step_physics(manager_, delta_ms, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::navigation_poll(const char* ball_entity, bool enable, const int* key_codes,
                                       const uint32_t* key_blocks, int count, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->navigation_poll) {
            error = "the physics_RT bridge lacks navigation_poll";
            return false;
        }
        char text[256] = {};
        if (!api_->navigation_poll(manager_, ball_entity, enable ? 1 : 0, key_codes, key_blocks, count, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::navigation_get_state(const char* ball_entity, bmmo_physics_nav_state& out, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->navigation_get_state) {
            error = "the physics_RT bridge lacks navigation_get_state";
            return false;
        }
        char text[256] = {};
        if (!api_->navigation_get_state(manager_, ball_entity, &out, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::navigation_set_state(const char* ball_entity, const bmmo_physics_nav_state& state,
                                            std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->navigation_set_state) {
            error = "the physics_RT bridge lacks navigation_set_state";
            return false;
        }
        char text[256] = {};
        if (!api_->navigation_set_state(manager_, ball_entity, &state, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::unphysicalize(const char* entity_name, std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        if (!api_->unphysicalize(manager_, entity_name, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::install_player_collision_filter(const char* player_group_prefix,
                                                       std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        if (!api_->install_player_collision_filter(manager_, player_group_prefix, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::reset_random(int seed, std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        api_->set_random_seed(seed);
        return true;
    }
}
