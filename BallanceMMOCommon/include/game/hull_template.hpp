#pragma once

// The paper hull built from a template body, the same in every world (FULL
// step H1X; the user's option (A2) of 2026-10-30, "建个模板纸球好像是最简单
// 而且最彻底的办法"; roadmap section 4 item 4, "Cause established at FULL step
// H1C2").
//
// physics_RT caches a named collision hull per manager, compiled with the
// scale of the FIRST body that physicalizes the name (engine change #10:
// PhysicsScaleFromMatrix of its local matrix, whose row lengths round by the
// rotation).  Every paper ball - the own one and every mirror - shares
// Ball_Paper_Mesh, and a new paper ball inherits the rotation of the ball it
// replaces: so the world whose first paper ball came first in real time (a
// client running ahead with its own trafo) built a hull one ulp off the
// others', and a rollback does not undo the cache.  Here every world compiles
// that hull from one template body (bridge v30, physics_state.hpp
// hull_template: identity rotation, unit scale, removed again inside the call)
// at its session anchor, before anything can physicalize a paper ball.
//
// When the cache is cleared (H1X census, levels 1-13 and the arena): only by
// CKIpionManager::OnCKPostReset / PostClearAll / Reset.  No script of any
// level holds a Physics Reset block or links Set Physics Globals' reset input,
// so in a session that is the engine's own (re)boot - the composition load -
// which is always followed by an anchor; a level reset ("Reset Level") keeps
// the cache.  The retail process keeps its cache across levels and sessions
// (single-player play included): the anchor's call forgets a cached hull no
// body uses and builds the template's.
//
// The census of named hulls whose first build can come from a runtime pose:
//   * Ball_Paper_Mesh ("physicalize new Ball", the trafo's and the respawn's
//     ball; mirrors) - templated here;
//   * Ball_Wood / Ball_Stone: spheres (the ball branch): no named hull;
//   * the explosion pieces' Ball_*_piece*_Mesh: forgotten at every anchor while
//     they have no body and compiled again from the squared, restored
//     hierarchy (restore_explosion_pieces, engine change #9);
//   * a trafo set's copies (..._Trafo<label>): compiled from the copy's saved
//     initial conditions (game/trafo_sets.hpp build step 6), copied from the
//     originals restored at the anchor;
//   * every other named hull (floors, modules, placeholders, props) is
//     compiled by the level's scripts at the level load or a sector's
//     activation, from the level file's pose (Restore IC), at one tick in
//     every world (the sector union).
//
// Only the bridge's C structs: compiles in the x86 client target and against
// Ballanced.  The backend is duck-typed (physics_direct / physics_view), as in
// game/host_backend.hpp.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include <physics/physics_rt_api.h>

namespace bmmo::game {
    struct templated_hull {
        const char* surface;   // the cache name the scripts physicalize under
        const char* mesh;      // the level's mesh the template's one convex is
    };
    inline constexpr templated_hull kTemplatedHulls[] = {
        {"Ball_Paper_Mesh", "Ball_Paper_Mesh"},
    };

    inline constexpr int kTemplatedHullCount = static_cast<int>(sizeof(kTemplatedHulls) / sizeof(kTemplatedHulls[0]));

    // Every templated hull built (or checked) once; one log line each, with
    // `failed` set when that hull does not hold the template's build (NOT
    // built, or a cached hull in use that DIFFERS): this world then compiles
    // it from a runtime pose and differs from every other one (H1C2), so a
    // host logs the line as an error and ends its session (H1X review).
    // Returns how many hulls hold the template's build afterwards (all of
    // them = kTemplatedHullCount).  `where` names the host's point ("the anchor").
    template <class Backend>
    int build_hull_templates(const Backend& backend, const char* where,
                             const std::function<void(const std::string& line, bool failed)>& log) {
        int holding = 0;
        for (const templated_hull& hull: kTemplatedHulls) {
            bmmo_physics_hull_template out{};
            std::string error;
            char line[384];
            bool failed = false;
            if (!backend.hull_template(hull.surface, hull.mesh, out, error)) {
                failed = true;
                std::snprintf(line, sizeof(line), "hull template: %s NOT built at %s: %s", hull.surface, where, error.c_str());
            } else if (out.outcome == BMMO_HULL_TEMPLATE_KEPT) {
                const bool equal = out.previous == out.signature;
                if (equal) ++holding;
                else failed = true;
                std::snprintf(line, sizeof(line),
                              "hull template: %s in use at %s - the cached hull %016llx kept (%s the template's %016llx, %u bytes)",
                              hull.surface, where, static_cast<unsigned long long>(out.previous),
                              equal ? "equal to" : "DIFFERS from", static_cast<unsigned long long>(out.signature),
                              static_cast<unsigned>(out.size));
            } else {
                ++holding;
                if (out.outcome == BMMO_HULL_TEMPLATE_REBUILT)
                    std::snprintf(line, sizeof(line),
                                  "hull template: %s built from the template at %s (hull %016llx, %u bytes; an unused cached "
                                  "hull %016llx forgotten first)",
                                  hull.surface, where, static_cast<unsigned long long>(out.signature),
                                  static_cast<unsigned>(out.size), static_cast<unsigned long long>(out.previous));
                else
                    std::snprintf(line, sizeof(line), "hull template: %s built from the template at %s (hull %016llx, %u bytes)",
                                  hull.surface, where, static_cast<unsigned long long>(out.signature),
                                  static_cast<unsigned>(out.size));
            }
            if (log) log(line, failed);
        }
        return holding;
    }
}
