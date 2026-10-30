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
        savestate_size_hint_ = 0;
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
                                      const float angular[3], bool wake, std::string& error,
                                      bool recheck) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        char text[256] = {};
        const auto beam = recheck ? api_->set_body_state_recheck : api_->set_body_state;
        if (!beam(manager_, entity_name, position, rotation, linear, angular,
                  wake ? 1 : 0, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    // API v10 (design 9.25): sleep state and contact recheck decided by the
    // caller.  The bridge also restores the core's leapfrog delta here, which
    // the plain beam zeroes - see physics_state.cpp.
    bool physics_view::set_body_state(const char* entity_name, const double position[3],
                                      const double rotation[4], const float linear[3],
                                      const float angular[3], bmmo::physics::wake_mode mode,
                                      bool recheck, std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_body_state_ex) {
            error = "the physics_RT bridge lacks set_body_state_ex";
            return false;
        }
        char text[256] = {};
        if (!api_->set_body_state_ex(manager_, entity_name, position, rotation, linear, angular,
                                     static_cast<int32_t>(mode), recheck ? 1 : 0, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::get_core_extra(const char* entity_name, bmmo_physics_core_extra& out,
                                      std::string& error) const {
        error.clear();
        out = bmmo_physics_core_extra{};
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->get_core_extra) {
            error = "the physics_RT bridge lacks get_core_extra";
            return false;
        }
        char text[256] = {};
        if (!api_->get_core_extra(manager_, entity_name, &out, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    bool physics_view::set_core_extra(const char* entity_name, const bmmo_physics_core_extra& in,
                                      std::string& error) const {
        error.clear();
        if (!available()) {
            error = "physics bridge is not initialized";
            return false;
        }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_core_extra) {
            error = "the physics_RT bridge lacks set_core_extra";
            return false;
        }
        char text[256] = {};
        if (!api_->set_core_extra(manager_, entity_name, &in, text, sizeof(text))) {
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

    bool physics_view::get_clock_state(bmmo_physics_clock_state& out, std::string& error) const {
        error.clear();
        out = bmmo_physics_clock_state{};
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->get_clock_state) {
            error = "the physics_RT bridge lacks get_clock_state";
            return false;
        }
        char text[256] = {};
        if (!api_->get_clock_state(manager_, &out, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::set_clock_state(const bmmo_physics_clock_state& in, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_clock_state) {
            error = "the physics_RT bridge lacks set_clock_state";
            return false;
        }
        char text[256] = {};
        if (!api_->set_clock_state(manager_, &in, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::force_block_get(uint32_t behavior_id, bmmo_physics_force_block& out, std::string& error) const {
        error.clear();
        out = bmmo_physics_force_block{};
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->force_block_get) {
            error = "the physics_RT bridge lacks force_block_get";
            return false;
        }
        char text[256] = {};
        if (!api_->force_block_get(manager_, behavior_id, &out, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::force_block_set(uint32_t behavior_id, const bmmo_physics_force_block& in, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->force_block_set) {
            error = "the physics_RT bridge lacks force_block_set";
            return false;
        }
        char text[256] = {};
        if (!api_->force_block_set(manager_, behavior_id, &in, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    // v27 (step RLC): a joint block's constraint image.
    bool physics_view::joint_block_get(uint32_t behavior_id, bmmo_physics_joint_image& out, std::string& error) const {
        error.clear();
        out.size = 0;
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->joint_block_get) {
            error = "the physics_RT bridge lacks joint_block_get";
            return false;
        }
        char text[256] = {};
        if (!api_->joint_block_get(manager_, behavior_id, &out, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::joint_block_set(uint32_t behavior_id, const bmmo_physics_joint_image& in, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->joint_block_set) {
            error = "the physics_RT bridge lacks joint_block_set";
            return false;
        }
        char text[256] = {};
        if (!api_->joint_block_set(manager_, behavior_id, &in, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::joint_blocks_order(const std::vector<uint32_t>& behavior_ids, const std::vector<int16_t>& orders,
                                          uint32_t& moved, std::string& error) const {
        error.clear();
        moved = 0;
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->joint_blocks_order) {
            error = "the physics_RT bridge lacks joint_blocks_order";
            return false;
        }
        if (orders.size() != 2 * behavior_ids.size()) { error = "two places per block"; return false; }
        char text[256] = {};
        if (!api_->joint_blocks_order(manager_, behavior_ids.data(), orders.data(), static_cast<uint32_t>(behavior_ids.size()),
                                      &moved, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    // v23: the PreSimulate queue.
    bool physics_view::callbacks_get(std::vector<bmmo_physics_callback>& out, std::string& error) const {
        error.clear();
        out.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->callbacks_get) {
            error = "the physics_RT bridge lacks callbacks_get";
            return false;
        }
        char text[256] = {};
        out.resize(16);
        int32_t total = api_->callbacks_get(manager_, out.data(), static_cast<int32_t>(out.size()), text, sizeof(text));
        if (total > static_cast<int32_t>(out.size())) {
            out.resize(static_cast<size_t>(total));
            total = api_->callbacks_get(manager_, out.data(), static_cast<int32_t>(out.size()), text, sizeof(text));
        }
        if (total < 0) { out.clear(); error = text; return false; }
        out.resize(std::min(static_cast<size_t>(total), out.size()));
        return true;
    }

    int physics_view::callbacks_rebuild(const std::vector<uint32_t>& scope, const std::vector<bmmo_physics_callback>& target,
                                        std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->callbacks_rebuild) {
            error = "the physics_RT bridge lacks callbacks_rebuild";
            return -1;
        }
        char text[256] = {};
        const int32_t r = api_->callbacks_rebuild(manager_, scope.data(), static_cast<int32_t>(scope.size()), target.data(),
                                                  static_cast<int32_t>(target.size()), text, sizeof(text));
        if (r < 0) error = text;
        return r;
    }

    int physics_view::callbacks_hold(const std::vector<uint32_t>& executed, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->callbacks_hold) {
            error = "the physics_RT bridge lacks callbacks_hold";
            return -1;
        }
        char text[256] = {};
        const int32_t r = api_->callbacks_hold(manager_, executed.data(), static_cast<int32_t>(executed.size()), text, sizeof(text));
        if (r < 0) error = text;
        return r;
    }

    int physics_view::callbacks_unhold(std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->callbacks_unhold) {
            error = "the physics_RT bridge lacks callbacks_unhold";
            return -1;
        }
        char text[256] = {};
        const int32_t r = api_->callbacks_unhold(manager_, text, sizeof(text));
        if (r < 0) error = text;
        return r;
    }

    bool physics_view::callback_scope_set(int mode, const std::vector<uint32_t>& scope, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->callback_scope_set) {
            error = "the physics_RT bridge lacks callback_scope_set";
            return false;
        }
        char text[256] = {};
        if (!api_->callback_scope_set(manager_, mode, scope.data(), static_cast<int32_t>(scope.size()), text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    int physics_view::contacts_get(const char* entity, bmmo_physics_contact* out, int max, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->contacts_get) {
            error = "the physics_RT bridge lacks contacts_get";
            return -1;
        }
        char text[256] = {};
        const int n = api_->contacts_get(manager_, entity, out, max, text, sizeof(text));
        if (n < 0) error = text;
        return n;
    }

    int physics_view::contacts_set(const char* entity, const bmmo_physics_contact* in, int count, uint32_t flags, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->contacts_set) {
            error = "the physics_RT bridge lacks contacts_set";
            return -1;
        }
        char text[256] = {};
        const int made = api_->contacts_set(manager_, entity, in, count, flags, text, sizeof(text));
        if (text[0]) error = text;
        return made;
    }

    bool physics_view::body_recheck(const char* entity, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->body_recheck) {
            error = "the physics_RT bridge lacks body_recheck";
            return false;
        }
        char text[256] = {};
        const bool ok = api_->body_recheck(manager_, entity, text, sizeof(text)) > 0;
        if (text[0]) error = text;
        return ok;
    }

    bool physics_view::set_body_matrix(const char* entity, const float matrix[16], bool simulated, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_body_matrix) {
            error = "the physics_RT bridge lacks set_body_matrix";
            return false;
        }
        char text[256] = {};
        const bool ok = api_->set_body_matrix(manager_, entity, matrix, simulated ? 1 : 0, text, sizeof(text)) > 0;
        if (text[0]) error = text;
        return ok;
    }

    bool physics_view::savestate_get(std::vector<uint8_t>& out, std::string& error) const {
        error.clear();
        out.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->savestate_get) {
            error = "the physics_RT bridge lacks savestate_get";
            return false;
        }
        char text[512] = {};
        // The size is asked with a NULL buffer, the blob fetched into a buffer
        // of that size.  The last blob's size (plus a margin) stands in for the
        // query, so the usual call captures once: the bridge returns the full
        // size whatever the buffer held, and only a blob that outgrew the guess
        // is fetched again (capture is deterministic, the second call returns
        // the same size).
        uint32_t capacity = savestate_size_hint_ ? savestate_size_hint_ + (savestate_size_hint_ >> 3) + 4096u : 0u;
        for (int attempt = 0; attempt < 2; ++attempt) {
            int32_t need = 0;
            if (capacity == 0) {
                need = api_->savestate_get(manager_, nullptr, 0, text, sizeof(text));
            } else {
                out.resize(capacity);
                need = api_->savestate_get(manager_, out.data(), capacity, text, sizeof(text));
            }
            if (need < 0) {
                error = text[0] ? text : "savestate_get failed";
                out.clear();
                return false;
            }
            if (need == 0) {
                error = "the physics_RT bridge returned an empty savestate";
                out.clear();
                return false;
            }
            if (capacity != 0 && static_cast<uint32_t>(need) <= capacity) {
                out.resize(static_cast<size_t>(need));
                savestate_size_hint_ = static_cast<uint32_t>(need);
                return true;
            }
            capacity = static_cast<uint32_t>(need);
        }
        error = "the savestate changed size between two captures";
        out.clear();
        return false;
    }

    bool physics_view::savestate_set(const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                                     std::vector<std::string>* contacts_dropped, std::vector<std::string>* unknown_bodies) const {
        error.clear();
        if (contacts_dropped) contacts_dropped->clear();
        if (unknown_bodies) unknown_bodies->clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->savestate_set) {
            error = "the physics_RT bridge lacks savestate_set";
            return false;
        }
        if (!data || size == 0 || size > 0xFFFFFFFFull) {
            error = "no savestate to restore";
            return false;
        }
        char text[512] = {};
        // 64 bytes a name: room for every body a level has; the bridge fails
        // the restore rather than truncate a list that does not fit.
        static thread_local std::vector<char> dropped(16384), unknown(32768);
        dropped[0] = unknown[0] = '\0';
        const int32_t ok = api_->savestate_set(manager_, data, static_cast<uint32_t>(size), flags, text, sizeof(text),
                                               dropped.data(), static_cast<uint32_t>(dropped.size()), unknown.data(),
                                               static_cast<uint32_t>(unknown.size()));
        const auto split_into = [](const char* joined, std::vector<std::string>* out) {
            if (!out) return;
            std::string name;
            for (const char* p = joined; *p; ++p) {
                if (*p == ';') { if (!name.empty()) out->push_back(name); name.clear(); }
                else name += *p;
            }
            if (!name.empty()) out->push_back(name);
        };
        split_into(dropped.data(), contacts_dropped);
        split_into(unknown.data(), unknown_bodies);
        if (!ok) {
            error = text[0] ? text : "savestate_set failed";
            return false;
        }
        return true;
    }

    bool physics_view::savestate_set_travelling(const uint8_t* data, size_t size, uint32_t flags,
                                                const std::vector<std::pair<std::string, std::string>>& renames,
                                                const std::vector<std::string>& excluded_prefixes, std::string& error,
                                                std::vector<std::string>* contacts_dropped,
                                                std::vector<std::string>* unknown_bodies, std::string* summary) const {
        error.clear();
        if (contacts_dropped) contacts_dropped->clear();
        if (unknown_bodies) unknown_bodies->clear();
        if (summary) summary->clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->savestate_set_travelling) {
            error = "the physics_RT bridge lacks savestate_set_travelling";
            return false;
        }
        if (!data || size == 0 || size > 0xFFFFFFFFull) {
            error = "no savestate to restore";
            return false;
        }
        std::string rename_text, excluded_text;
        for (const auto& [from, to]: renames) rename_text += (rename_text.empty() ? "" : ";") + from + "=" + to;
        for (const auto& prefix: excluded_prefixes) excluded_text += (excluded_text.empty() ? "" : ";") + prefix;
        char text[512] = {};
        char summary_text[1024] = {};
        static thread_local std::vector<char> dropped(16384), unknown(32768);
        dropped[0] = unknown[0] = '\0';
        const int32_t ok = api_->savestate_set_travelling(manager_, data, static_cast<uint32_t>(size), flags, rename_text.c_str(),
                                                          excluded_text.c_str(), text, sizeof(text), dropped.data(),
                                                          static_cast<uint32_t>(dropped.size()), unknown.data(),
                                                          static_cast<uint32_t>(unknown.size()), summary_text,
                                                          sizeof(summary_text));
        const auto split_into = [](const char* joined, std::vector<std::string>* out) {
            if (!out) return;
            std::string name;
            for (const char* p = joined; *p; ++p) {
                if (*p == ';') { if (!name.empty()) out->push_back(name); name.clear(); }
                else name += *p;
            }
            if (!name.empty()) out->push_back(name);
        };
        split_into(dropped.data(), contacts_dropped);
        split_into(unknown.data(), unknown_bodies);
        if (summary) *summary = summary_text;
        if (!ok) {
            error = text[0] ? text : "savestate_set_travelling failed";
            return false;
        }
        return true;
    }

    bool physics_view::set_event_log_muted(bool muted, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_event_log_muted) {
            error = "the physics_RT bridge lacks set_event_log_muted";
            return false;
        }
        char text[256] = {};
        if (!api_->set_event_log_muted(manager_, muted ? 1 : 0, text, sizeof(text))) { error = text; return false; }
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

    bool physics_view::get_physics_globals(bmmo_physics_globals& out, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->get_physics_globals) {
            error = "the physics_RT bridge lacks get_physics_globals";
            return false;
        }
        char text[256] = {};
        if (!api_->get_physics_globals(manager_, &out, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::set_physics_globals(const bmmo_physics_globals& in, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_physics_globals) {
            error = "the physics_RT bridge lacks set_physics_globals";
            return false;
        }
        char text[256] = {};
        if (!api_->set_physics_globals(manager_, &in, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::savestate_globals(const uint8_t* data, size_t size, bmmo_physics_globals& out,
                                         std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->savestate_globals) {
            error = "the physics_RT bridge lacks savestate_globals";
            return false;
        }
        if (!data || size > 0xffffffffu) { error = "no savestate data"; return false; }
        char text[256] = {};
        if (!api_->savestate_globals(data, static_cast<uint32_t>(size), &out, text, sizeof(text))) { error = text; return false; }
        return true;
    }

    bool physics_view::hull_template(const char* surface, const char* mesh, bmmo_physics_hull_template& out,
                                     std::string& error) const {
        error.clear();
        out = {};
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->hull_template) {
            error = "the physics_RT bridge lacks hull_template";
            return false;
        }
        char text[256] = {};
        if (!api_->hull_template(manager_, surface, mesh, &out, text, sizeof(text))) { error = text; return false; }
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
        // Both generators: IVP's own (idle since engine change #5) and the
        // one behind the hooked Random block.
        api_->set_random_seed(seed);
        if (api_->random_reset) api_->random_reset(seed);
        return true;
    }

    bool physics_view::push_impulse(const char* entity_name, const float direction_ws[3], float speed,
                                    uint32_t behavior_id, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->push_impulse) {
            error = "the physics_RT bridge lacks push_impulse";
            return false;
        }
        char text[256] = {};
        if (!api_->push_impulse(manager_, entity_name, direction_ws, speed, behavior_id, text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    int physics_view::install_random_block(std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->install_random_block) {
            error = "the physics_RT bridge lacks install_random_block";
            return -1;
        }
        const int patched = api_->install_random_block(context_);
        if (patched < 0) error = "the Random block prototype is not registered";
        return patched;
    }

    int32_t physics_view::random_next(std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->random_next) {
            error = "the physics_RT bridge lacks random_next";
            return -1;
        }
        return api_->random_next();
    }

    bool physics_view::set_clock_guard(bool enable, float time_factor, uint32_t behavior_id,
                                       uint32_t pause_behavior_id, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_clock_guard) {
            error = "the physics_RT bridge lacks set_clock_guard";
            return false;
        }
        char text[256] = {};
        if (!api_->set_clock_guard(manager_, enable ? 1 : 0, time_factor, behavior_id, pause_behavior_id,
                                   text, sizeof(text))) {
            error = text;
            return false;
        }
        return true;
    }

    // ---- bridge API v21 / v22: the trafo sets (step T4) ----

    bool physics_view::has_body(const char* entity_name) const {
        if (!available() || !entity_name || !entity_name[0]) return false;
        bmmo_physics_body_state state{};
        std::string error;
        return get_body_state(entity_name, state, error);
    }

    bool physics_view::trafo_set_register(uint32_t player, const uint32_t frames[3], const uint32_t* explosions,
                                          int explosion_count, bmmo_physics_trafo_set_report& out, std::string& error) const {
        error.clear();
        out = bmmo_physics_trafo_set_report{};
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->trafo_set_register) {
            error = "the physics_RT bridge lacks trafo_set_register";
            return false;
        }
        char text[512] = {};
        if (!api_->trafo_set_register(manager_, player, frames, explosions, explosion_count, &out, text, sizeof(text))) {
            error = text[0] ? text : "trafo_set_register failed";
            return false;
        }
        return true;
    }

    bool physics_view::trafo_set_unregister(uint32_t player, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->trafo_set_unregister) {
            error = "the physics_RT bridge lacks trafo_set_unregister";
            return false;
        }
        char text[256] = {};
        if (!api_->trafo_set_unregister(manager_, player, text, sizeof(text))) {
            error = text[0] ? text : "trafo_set_unregister failed";
            return false;
        }
        return true;
    }

    void physics_view::trafo_set_seed(uint32_t frame_id, int32_t seed) const {
        if (!available() || api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->trafo_set_seed) return;
        api_->trafo_set_seed(manager_, frame_id, seed);
    }

    int physics_view::trafo_set_unmake(uint32_t frame_id, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return -1; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->trafo_set_unmake) {
            error = "the physics_RT bridge lacks trafo_set_unmake";
            return -1;
        }
        char text[256] = {};
        const int removed = api_->trafo_set_unmake(manager_, frame_id, text, sizeof(text));
        if (removed < 0) error = text[0] ? text : "trafo_set_unmake failed";
        return removed;
    }

    bool physics_view::trafo_set_filter(bool enable, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->trafo_set_filter) {
            error = "the physics_RT bridge lacks trafo_set_filter";
            return false;
        }
        char text[256] = {};
        if (!api_->trafo_set_filter(manager_, enable ? 1 : 0, text, sizeof(text))) {
            error = text[0] ? text : "trafo_set_filter failed";
            return false;
        }
        return true;
    }

    bool physics_view::airship_zone_set(bool enable, const char* own_group, bmmo::physics::airship_zone_box& out,
                                        std::string& error) const {
        error.clear();
        out = bmmo::physics::airship_zone_box{};
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->airship_zone_set) {
            error = "the physics_RT bridge lacks airship_zone_set";
            return false;
        }
        char text[256] = {};
        double box[6] = {};
        const int32_t r = api_->airship_zone_set(manager_, enable ? 1 : 0, own_group ? own_group : "", box, text, sizeof(text));
        if (r < 0) {
            error = text[0] ? text : "airship_zone_set failed";
            return false;
        }
        out.active = r == 1;
        for (int k = 0; k < 3; ++k) {
            out.min[k] = box[k];
            out.max[k] = box[3 + k];
        }
        return true;
    }

    int physics_view::airship_zone_get(double box[6]) const {
        if (!available() || api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->airship_zone_get) return -1;
        return api_->airship_zone_get(manager_, box);
    }

    bool physics_view::spawn_ghost_set(bool enable, bool respawns, double radius, bmmo::physics::spawn_ghost_info& out,
                                       std::string& error) const {
        error.clear();
        out = bmmo::physics::spawn_ghost_info{};
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->spawn_ghost_set) {
            error = "the physics_RT bridge lacks spawn_ghost_set";
            return false;
        }
        char text[256] = {};
        double info[7] = {};
        const int32_t r = api_->spawn_ghost_set(manager_, enable ? 1 : 0, respawns ? 1 : 0, radius, info, text, sizeof(text));
        if (r < 0) {
            error = text[0] ? text : "spawn_ghost_set failed";
            return false;
        }
        out.active = r == 1;
        out.areas = static_cast<uint32_t>(info[0]);
        out.radius = info[1];
        out.solidified = static_cast<uint64_t>(info[2]);
        out.rechecks = static_cast<uint64_t>(info[3]);
        out.overlap_waits = static_cast<uint64_t>(info[4]);
        out.births = static_cast<uint64_t>(info[5]);
        out.resumed = static_cast<uint64_t>(info[6]);
        return true;
    }

    int physics_view::spawn_ghost_state(const char* entity_name) const {
        if (!available() || api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->spawn_ghost_state) return 0;
        return api_->spawn_ghost_state(manager_, entity_name);
    }

    bool physics_view::spawn_ghost_birth(const char* group) const {
        if (!available() || api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->spawn_ghost_birth) return false;
        return api_->spawn_ghost_birth(manager_, group) == 1;
    }

    int physics_view::own_group_queue(const char* entity_name, const char* group, uint32_t behavior_id,
                                      std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return 0; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->own_group_queue) {
            error = "the physics_RT bridge lacks own_group_queue";
            return 0;
        }
        char text[256] = {};
        const int r = api_->own_group_queue(manager_, entity_name, group, behavior_id, text, sizeof(text));
        if (r == 0) error = text;
        return r;
    }

    int physics_view::airship_zone_contains(const char* entity_name) const {
        if (!available() || api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->airship_zone_contains) return 0;
        return api_->airship_zone_contains(manager_, entity_name);
    }

    bool physics_view::set_controller_alias(int32_t (*alias)(void* user, const char* name, char* out, uint32_t out_size),
                                            void* user, std::string& error) const {
        error.clear();
        if (!available()) { error = "physics bridge is not initialized"; return false; }
        if (api_->struct_size < sizeof(bmmo_physics_api_v2) || !api_->set_controller_alias) {
            error = "the physics_RT bridge lacks set_controller_alias";
            return false;
        }
        api_->set_controller_alias(manager_, alias, user);
        return true;
    }
}
