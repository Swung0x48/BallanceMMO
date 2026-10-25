#pragma once

// The resync's rules that are decisions of the session core
// (docs/resync-and-sector-timing-design.md 2.4, 2.8; step A1), CK-free so
// that every world - the server's, the headless client's and the retail
// mod's - asks the same function, and the rule is unit-tested once.
//
// keep_local_script: the scripts a resync does NOT take from the server's
// portable script record (game/script_replay.hpp localize), but copies from
// the client's own anchor record instead, and whose pending activations the
// hosts record for the replay to re-issue (game/session_host_support.hpp
// session_script_activations):
//
//   * every script owned by an object whose name starts "PE_Balloon": the
//     level-end balloon and its airship are local to each player (the user's
//     exception (c), invariant I3);
//   * the trafo scripts of Balls.nmo (Ball_Explosion_<type>,
//     Ball_ResetPieces_<type>, Ball_PaperWind) until the trafo replication
//     lands (G-e; step T2 of the design removes them): the server runs no
//     trafo, so its record of them is a world without the client's pieces.
//
// is_trafo_script_name is the name half of script_replay::is_trafo_script
// (which also asks that the behaviour is a root script), kept here so the
// trafo names are written once.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bmmo::session {
    // The originals only: a trafo set's copies (game/trafo_sets.hpp, named
    // "<original>_Trafo<player>") are selected through the sets' registry,
    // never by name (trafo-replication design 4.3; the prefixes would match
    // the copies' explosions and resets).
    inline bool is_trafo_script_name(std::string_view script) {
        if (script.find("_Trafo") != std::string_view::npos) return false;
        return script.substr(0, 15) == "Ball_Explosion_" || script.substr(0, 17) == "Ball_ResetPieces_"
            || script == "Ball_PaperWind";
    }

    // `owner`: the name of the script's owner ("" when it has none);
    // `script`: the root script's name.
    inline bool keep_local_script(std::string_view owner, std::string_view script) {
        if (owner.substr(0, 10) == "PE_Balloon") return true;
        return is_trafo_script_name(script);   // until step T2
    }

    // ---- step A3 (resync design 2.6.3) ----

    // The bodies a resync never takes from the server (I3): the level-end
    // balloon and its airship, "PE_Balloon*", and the airship's slide box
    // "PE_Box_slide", which PE_Balloon Script's slider and spring join to the
    // balloon platform (a box taken from the server under a platform kept
    // from the anchor is pulled by that joint: measured 10 mm at the apply,
    // 29-44 m later) - left out of the server's savestate on both sides
    // (savestate_travel's excluded prefixes), their rows skipped, restored
    // from the client's own anchor record.  (The trafo
    // pieces are local too until T2, but by being the client's alone: the
    // server's world has none, so the blob does not know them; the host names
    // them, client_platform.hpp local_only_body.)
    inline const std::vector<std::string>& keep_local_body_prefixes() {
        static const std::vector<std::string> prefixes{"PE_Balloon", "PE_Box_slide"};
        return prefixes;
    }
    inline bool keep_local_body(std::string_view name) {
        for (const auto& prefix: keep_local_body_prefixes())
            if (name.substr(0, prefix.size()) == prefix) return true;
        return false;
    }

    // The travelling blob's renames (resync design 2.3, 9 Q1): the server
    // names every player's ball by its clone, "<ball row>_BMMO_<id>"; this
    // world names them its own way - the own ball by its own entity, a
    // mirror by the entity the host made for it (the headless client's
    // mirrors share the server's names, the retail spirit balls do not).
    // `local` maps a player to this world's entity ("" none); a name that
    // agrees, or a player with no entity here, maps nothing.
    inline std::vector<std::pair<std::string, std::string>> resync_renames(
            const std::vector<std::pair<uint32_t, std::string>>& server_clones, const std::map<uint32_t, std::string>& local) {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [player, clone]: server_clones) {
            const auto it = local.find(player);
            if (it == local.end() || it->second.empty() || clone.empty() || it->second == clone) continue;
            out.emplace_back(clone, it->second);
        }
        return out;
    }

    // A pass at M physicalizes its modules in frame M+5
    // (sector_union::kPassPhysicalizeDelay): the pass a body born at `birth`
    // belongs to - the one with M + 5 == birth, else the newest with
    // M + 5 <= birth - among `passes` (tick -> sector).  0 when none
    // explains it.
    inline int attribute_birth(const std::map<uint32_t, int>& passes, uint32_t birth, uint32_t* pass_tick = nullptr) {
        constexpr uint32_t kDelay = 5;
        if (birth < kDelay) return 0;
        if (const auto exact = passes.find(birth - kDelay); exact != passes.end()) {
            if (pass_tick) *pass_tick = exact->first;
            return exact->second;
        }
        auto it = passes.upper_bound(birth - kDelay);
        if (it == passes.begin()) return 0;
        --it;
        if (pass_tick) *pass_tick = it->first;
        return it->second;
    }

    // What the reconcile does with a movable mechanism of the anchor world
    // that the server's state at S has no row for (resync design 2.6.3).
    enum class resync_unrowed : uint8_t {
        keep_local,      // a keep-local body: the anchor's own state
        local_only,      // a body the client alone has (the trafo pieces until T2): the anchor's own state
        destroy,         // born after S by a pass the server's union lacks at S: destroyed, re-made by the replay
        unattributed,    // born after S, no pass explains it: kept (counted, gate 0)
        unexplained,     // none of the reasons: kept from the anchor (counted, logged by name, gate 0)
    };
    // `birth`: the body's latest birth tick in the host's birth log (a shared
    // tick; 0 unknown); `passes`: this world's union passes (pass_log_);
    // `server_active`: the sectors the server's union has active at S.
    inline resync_unrowed classify_unrowed(std::string_view name, bool local_only, uint32_t birth, uint32_t s,
                                           const std::map<uint32_t, int>& passes, const std::set<int>& server_active,
                                           int* sector = nullptr) {
        if (keep_local_body(name)) return resync_unrowed::keep_local;
        if (local_only) return resync_unrowed::local_only;
        if (birth == 0 || static_cast<int32_t>(birth - s) <= 0) return resync_unrowed::unexplained;
        const int k = attribute_birth(passes, birth);
        if (sector) *sector = k;
        if (k <= 0) return resync_unrowed::unattributed;
        return server_active.count(k) ? resync_unrowed::unexplained : resync_unrowed::destroy;
    }
}
