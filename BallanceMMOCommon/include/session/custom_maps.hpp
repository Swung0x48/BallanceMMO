#pragma once

// Step CMAP: the maps a physics session may run.  An original level is known by its slot's canonical md5
// (entity/map.hpp); a custom map (BML+'s custom-map loading: type CustomMap, the md5 of the file the client
// loaded, the level slot it was loaded into) is accepted when the server holds a file with that md5 in its
// custom-map directory (physics.custom_maps, default <game_root>/ModLoader/Maps - where BML+ lists them).  The
// server's world then loads that file into the same slot (headless_engine's map_file).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "../entity/map.hpp"
#include "../utility/md5.hpp"

namespace bmmo::session {
    struct custom_map_file {
        std::filesystem::path path;
        uint8_t md5[16] = {};
    };

    // The md5s of an earlier scan, by path: a file whose size and modification time are unchanged is not read
    // again (the server scans at each custom-map session start, under its state lock).
    struct custom_map_md5_cache {
        struct entry {
            std::uintmax_t size = 0;
            std::filesystem::file_time_type mtime{};
            uint8_t md5[16] = {};
        };
        std::map<std::filesystem::path, entry> entries;
    };

    // Every *.nmo / *.cmo under `dir` (any case; BML+'s map menu descends 8 levels by default), sorted by path.
    // A directory that does not exist gives none.  With `cache`, unchanged files reuse their md5 and the cache
    // keeps only the files this scan found.
    inline std::vector<custom_map_file> scan_custom_maps(const std::filesystem::path& dir,
                                                         custom_map_md5_cache* cache = nullptr, int max_depth = 8) {
        std::vector<custom_map_file> found;
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
            if (cache) cache->entries.clear();
            return found;
        }
        custom_map_md5_cache next;
        std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > max_depth) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".nmo" && ext != ".cmo") continue;
            custom_map_file f;
            f.path = it->path();
            if (cache) {
                std::error_code stat_ec;
                custom_map_md5_cache::entry e;
                e.size = it->file_size(stat_ec);
                if (!stat_ec) e.mtime = it->last_write_time(stat_ec);
                if (!stat_ec) {
                    const auto old = cache->entries.find(f.path);
                    if (old != cache->entries.end() && old->second.size == e.size && old->second.mtime == e.mtime) {
                        std::memcpy(f.md5, old->second.md5, sizeof(f.md5));
                    } else if (!bmmo::md5::of_file(f.path, f.md5)) {
                        continue;
                    }
                    std::memcpy(e.md5, f.md5, sizeof(e.md5));
                    next.entries[f.path] = e;
                    found.push_back(f);
                    continue;
                }
            }
            if (bmmo::md5::of_file(f.path, f.md5)) found.push_back(f);
        }
        if (cache) cache->entries = std::move(next.entries);
        std::sort(found.begin(), found.end(), [](const custom_map_file& a, const custom_map_file& b) { return a.path < b.path; });
        return found;
    }

    inline const custom_map_file* find_custom_map(const std::vector<custom_map_file>& known, const uint8_t md5[16]) {
        for (const auto& f: known)
            if (std::memcmp(f.md5, md5, sizeof(f.md5)) == 0) return &f;
        return nullptr;
    }

    // Whether a physics session may run `m`: an original level (its slot's md5), or a custom map the server
    // has (then `file` is it).  `reason` says why not.
    inline bool physics_map_accepted(const bmmo::map& m, const std::vector<custom_map_file>& known,
                                     std::filesystem::path& file, std::string& reason) {
        file.clear();
        if (m.level < 1 || m.level > 13) {
            reason = "physics sessions need a level slot 1..13 (level " + std::to_string(m.level) + ")";
            return false;
        }
        if (m.is_original_level()) return true;
        if (m.type != bmmo::map_type::CustomMap) {
            reason = "physics sessions need an original level or a custom map the server has";
            return false;
        }
        if (const auto* f = find_custom_map(known, m.md5)) {
            file = f->path;
            return true;
        }
        reason = "the server does not have this custom map (md5 " + m.get_hash_string() + ")";
        return false;
    }
}
