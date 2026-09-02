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
        const bmmo_physics_api_v1* api = module.entry(BMMO_PHYSICS_API_VERSION);
        if (!api || api->struct_size < sizeof(bmmo_physics_api_v1)
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
