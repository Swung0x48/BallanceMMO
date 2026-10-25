#pragma once

// SimTool --synth-trafo-session DIR (trafo-replication design 7.2 S10, step
// T2): a server session with WORLD_TRAFO_INPUTS run by the real
// session_runner on a booted level, its members' Trafo events submitted as
// the network side would after validation, so the server's journal of it is
// a synthetic journal of the trafo inputs - every accept rule of the ledger
// (on time, moved by the span, a re-explosion of a live type set, a
// superseded reset, a late reset, a refused event, a leaver's final resets,
// the deadline) and the world's sets, frame, hold and retirement on the way.
// The run checks every echo the runner hands back against the ledger's rules
// (PASS/FAIL lines); the journal is then replayed with --replay-session (S10:
// matched N/N, 0 checkpoint mismatches, the pieces in every checkpoint).
//
// Real time: the runner steps at 66 Hz once every member is ready, so a run
// of N ticks takes N/66 s after the boot.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <entity/session.hpp>
#include <session/trafo_input.hpp>

#include "session_runner.hpp"

namespace bmmo::sim {

    struct synth_trafo_options {
        std::filesystem::path root;
        std::filesystem::path out_dir;
        int level = 2;
        uint32_t ticks = 1560;
        uint32_t sector_lead = 20;
        uint32_t checkpoint_ticks = 20;
    };

    inline int run_synth_trafo_session(const synth_trafo_options& o) {
        using bmmo::session::event_type;
        using bmmo::session::TRAFO_FINAL;
        using bmmo::session::TRAFO_RESET;
        using bmmo::session::TRAFO_SERVER;
        std::mutex mutex;
        std::condition_variable wake;
        bool ready = false, ok = false, failed = false;
        world_ready_info info;
        std::vector<accepted_event> accepted;
        std::vector<bmmo::session::trafo_ledger_entry> ledger;
        uint32_t ledger_tick = 0;
        bool have_ledger = false;
        runner_config rc;
        rc.game_root = o.root;
        rc.input_delay = 6;
        rc.snapshot_interval = 100000;   // the server never depends on snapshots
        rc.full_snapshot_interval = 100000;
        rc.journal_dir = o.out_dir;
        rc.journal_checkpoint_ticks = o.checkpoint_ticks;
        rc.seed = 1;
        session_callbacks cb;
        cb.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        cb.on_world_ready = [&](const world_ready_info& i) {
            std::lock_guard lk(mutex);
            info = i;
            ok = i.ok;
            ready = true;
            wake.notify_all();
        };
        cb.on_event_accepted = [&](const accepted_event& a) {
            std::lock_guard lk(mutex);
            accepted.push_back(a);
            std::printf("echo p%u %s %s flags=%u origin=%u applied=%u%s%s%s\n", a.player,
                        bmmo::session::event_type_name(static_cast<int>(a.type)),
                        a.type == event_type::Trafo ? bmmo::session::trafo_type_name(a.ball_type) : "", a.flags, a.origin,
                        a.applied, a.applied != a.origin ? " MOVED" : "", a.superseded ? " SUPERSEDED" : "",
                        a.rejected ? " REJECTED" : "");
            std::fflush(stdout);
        };
        // The ledger of a join / resync state (design 4.7) taken while the
        // leaver's set is between its final resets and its retirement.
        cb.on_resync_state = [&](const resync_state_out& out) {
            std::lock_guard lk(mutex);
            ledger = out.state.trafo_ledger;
            ledger_tick = out.tick;
            have_ledger = true;
            for (const auto& e: ledger)
                std::printf("ledger S=%u p%u %s explosion=%d@%u bodies=%d reset=%d@%u final=%d@%u\n", out.tick, e.player,
                            bmmo::session::trafo_type_name(e.type), e.has_explosion ? 1 : 0, e.explode_applied,
                            e.has_bodies ? 1 : 0, e.has_reset ? 1 : 0, e.reset_applied, e.final ? 1 : 0, e.final_applied);
            std::fflush(stdout);
        };
        cb.on_failed = [&](uint32_t, const std::string& reason) {
            std::lock_guard lk(mutex);
            failed = true;
            std::printf("session failed: %s\n", reason.c_str());
        };
        session_runner runner(rc, cb);
        const uint32_t S = 1;
        const std::vector<std::pair<uint32_t, uint8_t>> players = {{101, 0}, {102, 1}, {103, 2}};
        runner.create_session(S, o.level, players, 6, 0, 0.0f, "synthetic trafo session (SimTool --synth-trafo-session)",
                              {"p101", "p102", "p103"}, o.sector_lead, bmmo::session::WORLD_TRAFO_INPUTS);
        {
            std::unique_lock lk(mutex);
            wake.wait_for(lk, std::chrono::seconds(180), [&] { return ready; });
            if (!ready || !ok) {
                std::printf("world boot failed: %s\n", info.error.c_str());
                return 1;
            }
        }
        // Explosions 3.000 m above a P_Trafo_*_MF, as every measured trafo is
        // (session/trafo_input.hpp kTrafoReach).
        std::vector<std::array<float, 3>> at;
        for (const auto& p: info.trafo_points)
            if (p.name.find("_MF") != std::string::npos) at.push_back({p.position[0], p.position[1] + 3.0f, p.position[2]});
        std::printf("synth: %zu P_Trafo_* entities, %zu MF\n", info.trafo_points.size(), at.size());
        for (const auto& p: info.trafo_points)
            std::printf("synth: trafo %s %.4f %.4f %.4f\n", p.name.c_str(), static_cast<double>(p.position[0]),
                        static_cast<double>(p.position[1]), static_cast<double>(p.position[2]));
        // The network side's check (session/trafo_input.hpp trafo_validate)
        // against this world's trafo points: the explosion positions measured
        // in the R2 / r2t / p3_two / p3_h4 journals and by T1's S7 hold, and
        // the run's own.
        {
            struct measured { int level; float p[3]; };
            const measured known[] = {{2, {948.74127f, 3.7663198f, -56.514111f}}, {11, {60.951f, 8.095f, -12.578f}},
                                      {13, {0.0f, 3.095f, 43.0f}}, {13, {74.0f, 3.095f, 43.0f}}, {13, {240.0f, 3.095f, 43.0f}}};
            for (const auto& k: known) {
                if (k.level != o.level) continue;
                const auto v = bmmo::session::trafo_validate(0, 0, k.p, info.trafo_points);
                std::printf("synth: measured explosion (%.3f, %.3f, %.3f): %s, nearest %s at %.4f m\n", static_cast<double>(k.p[0]),
                            static_cast<double>(k.p[1]), static_cast<double>(k.p[2]), bmmo::session::trafo_check_name(v.verdict),
                            v.nearest >= 0 ? info.trafo_points[static_cast<size_t>(v.nearest)].name.c_str() : "-", v.distance);
            }
        }
        if (at.size() < 3) {
            std::printf("synth: the level needs 3 trafos\n");
            return 1;
        }
        for (const auto& [id, order]: players) runner.player_ready(S, id, 0);
        const auto now_tick = [&] { return runner.current_tick(S); };
        const auto wait_for = [&](uint32_t tick) {
            while (static_cast<int32_t>(now_tick() - tick) < 0) {
                {
                    std::lock_guard lk(mutex);
                    if (failed) return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return true;
        };
        uint64_t token = 1;
        const auto trafo = [&](uint32_t player, uint8_t type, uint8_t flags, uint32_t stamp, const std::array<float, 3>* where) {
            lifecycle_event e;
            e.type = event_type::Trafo;
            e.ball_type = type;
            e.flags = flags;
            if (where) for (int k = 0; k < 3; ++k) e.position[k] = (*where)[k];
            e.tick = stamp;
            std::printf("synth: at tick %u submit p%u %s %s stamped %u\n", now_tick(), player, bmmo::session::trafo_type_name(type),
                        (flags & TRAFO_RESET) ? "reset" : "explode", stamp);
            runner.submit_event(S, player, stamp, e, token++);
        };
        // The scenario (ticks from the session's first, 0).
        bool alive = true;
        alive = alive && wait_for(20);  trafo(101, 0, 0, 40, &at[0]);                  // a: on time
        alive = alive && wait_for(40);  trafo(102, 2, 0, 60, &at[1]);                  // b: paper, never reset: the deadline
        alive = alive && wait_for(80);  trafo(103, 1, 0, 100, &at[2]);                 // c: stone
        alive = alive && wait_for(180); trafo(103, 1, 0, 200, &at[2]);                 // d: a live type set again: the server's reset first
        alive = alive && wait_for(280); trafo(101, 0, TRAFO_RESET, 300, nullptr);      // e: the owner's reset
        alive = alive && wait_for(300); trafo(101, 0, 0, 320, &at[0]);                 // f: moved to the reset + 140
        trafo(101, 0, TRAFO_RESET, 310, nullptr);                                      // g: older than f: superseded
        const uint32_t late_at = 500;
        alive = alive && wait_for(late_at); trafo(103, 1, TRAFO_RESET, late_at - 5, nullptr);   // h: late: moved to N
        alive = alive && wait_for(520); trafo(101, 7, 0, 540, &at[0]);                 // i: a bad piece type: refused
        alive = alive && wait_for(600); trafo(103, 0, 0, 620, &at[2]);                 // j: wood, then 103 leaves
        alive = alive && wait_for(610);
        std::printf("synth: at tick %u p103 leaves\n", now_tick());
        runner.remove_player(S, 103);
        // A resync state of S = 659 for p101: the leaver's set is held (its
        // final resets at about 621, retired about 133 ticks later), p101's
        // stone and paper never exploded.
        alive = alive && wait_for(640); runner.request_resync_state(S, 101, 660);
        alive = alive && wait_for(880); trafo(101, 0, TRAFO_RESET, 900, nullptr);      // k: f's life ends
        alive = alive && wait_for(o.ticks);
        runner.describe(S);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::string journal = runner.journal_path(S);
        runner.destroy_session(S, "synthetic run done");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!alive) {
            std::printf("synth: the session failed\n");
            return 1;
        }
        // The checks: every rule of the ledger as the runner applied it.
        std::lock_guard lk(mutex);
        int failures = 0;
        const auto find = [&](uint32_t player, uint8_t type, uint8_t flags, bool any_origin, uint32_t origin) -> const accepted_event* {
            for (const auto& a: accepted)
                if (a.type == event_type::Trafo && a.player == player && a.ball_type == type && a.flags == flags
                        && (any_origin || a.origin == origin))
                    return &a;
            return nullptr;
        };
        const auto check = [&](const char* what, bool pass, const std::string& detail) {
            std::printf("%s %s: %s\n", pass ? "PASS" : "FAIL", what, detail.c_str());
            if (!pass) ++failures;
        };
        const auto show = [](const accepted_event* a) {
            return a ? "origin " + std::to_string(a->origin) + " applied " + std::to_string(a->applied)
                           + (a->superseded ? " superseded" : "") + (a->rejected ? " rejected" : "")
                     : std::string("missing");
        };
        const auto* a = find(101, 0, 0, false, 40);
        check("a on time", a && a->applied == 40 && !a->superseded, show(a));
        const auto* b = find(102, 2, 0, false, 60);
        check("b on time", b && b->applied == 60, show(b));
        const auto* c = find(103, 1, 0, false, 100);
        check("c on time", c && c->applied == 100, show(c));
        const auto* d_reset = find(103, 1, TRAFO_RESET | TRAFO_SERVER, true, 0);
        const auto* d = find(103, 1, 0, false, 200);
        check("d the server's reset of the live type set", d_reset && d_reset->origin == d_reset->applied && c
                  && static_cast<int32_t>(d_reset->applied - (c->applied + 1)) >= 0, show(d_reset));
        check("d the re-explosion after the span", d && d_reset && d->applied == d_reset->applied + bmmo::session::kTrafoResetSpan,
              show(d));
        const auto* e = find(101, 0, TRAFO_RESET, false, 300);
        check("e the owner's reset on time", e && e->applied == 300, show(e));
        const auto* f = find(101, 0, 0, false, 320);
        check("f moved to the reset + 140", f && e && f->applied == e->applied + bmmo::session::kTrafoResetSpan, show(f));
        const auto* g = find(101, 0, TRAFO_RESET, false, 310);
        check("g superseded", g && g->superseded, show(g));
        const auto* h = find(103, 1, TRAFO_RESET, false, late_at - 5);
        check("h late: moved, never before its stamp's session tick", h && !h->superseded
                  && static_cast<int32_t>(h->applied - h->origin) > 0, show(h));
        const auto* i = find(101, 7, 0, false, 540);
        check("i a bad piece type refused", i && i->rejected, show(i));
        const auto* j = find(103, 0, 0, false, 620);
        check("j on time", j && j->applied == 620, show(j));
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            const auto* fin = find(103, t, TRAFO_RESET | TRAFO_SERVER | TRAFO_FINAL, true, 0);
            const bool after = !fin ? false : (t == 0 ? (j && static_cast<int32_t>(fin->applied - (j->applied + 1)) >= 0) : true);
            check((std::string("leaver final reset ") + bmmo::session::trafo_type_name(t)).c_str(), fin && after, show(fin));
        }
        const auto* k = find(101, 0, TRAFO_RESET, false, 900);
        check("k the owner's reset on time", k && k->applied == 900, show(k));
        const auto* dl = find(102, 2, TRAFO_RESET | TRAFO_SERVER, true, 0);
        check("the deadline reset at the explosion + 1320", dl && b && dl->applied == b->applied + bmmo::session::kTrafoLife,
              show(dl));
        const auto* none = find(101, 0, TRAFO_RESET | TRAFO_SERVER, true, 0);
        check("no deadline for a type set its owner reset", none == nullptr, show(none));
        // The ledger names every set the world holds (T2's review): three
        // entries per set, the leaver's final with its final tick, a type set
        // that never exploded included.
        const auto entry = [&](uint32_t player, uint8_t type) -> const bmmo::session::trafo_ledger_entry* {
            for (const auto& x: ledger)
                if (x.player == player && x.type == type) return &x;
            return nullptr;
        };
        check("ledger taken", have_ledger && ledger_tick == 659, "S " + std::to_string(ledger_tick));
        check("ledger every set's three type sets", ledger.size() == 9, std::to_string(ledger.size()) + " entries");
        bool leaver_final = true;
        for (uint8_t t = 0; t < bmmo::session::kTrafoTypes; ++t) {
            const auto* fin = find(103, t, TRAFO_RESET | TRAFO_SERVER | TRAFO_FINAL, true, 0);
            const auto* x = entry(103, t);
            leaver_final = leaver_final && x && fin && x->final && x->has_reset && x->reset_applied == fin->applied
                        && x->final_applied == fin->applied;
        }
        check("ledger the leaver's set final", leaver_final, "p103");
        const auto* never = entry(101, 1);
        check("ledger a type set that never exploded", never && !never->has_explosion && !never->has_bodies && !never->final,
              "p101 stone");
        const auto* live = entry(102, 2);
        check("ledger the live paper", live && live->has_explosion && live->has_bodies && b && live->explode_applied == b->applied
                  && !live->has_reset, "p102 paper");
        std::printf("synth: journal %s\n", journal.c_str());
        std::printf("synth summary: failures=%d echoes=%zu\n", failures, accepted.size());
        std::fflush(stdout);
        return failures ? 3 : 0;
    }
}
