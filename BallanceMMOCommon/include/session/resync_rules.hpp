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

#include <string_view>

namespace bmmo::session {
    inline bool is_trafo_script_name(std::string_view script) {
        return script.substr(0, 15) == "Ball_Explosion_" || script.substr(0, 17) == "Ball_ResetPieces_"
            || script == "Ball_PaperWind";
    }

    // `owner`: the name of the script's owner ("" when it has none);
    // `script`: the root script's name.
    inline bool keep_local_script(std::string_view owner, std::string_view script) {
        if (owner.substr(0, 10) == "PE_Balloon") return true;
        return is_trafo_script_name(script);   // until step T2
    }
}
