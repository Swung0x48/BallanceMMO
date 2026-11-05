# Virtual clock test mode (plan, for review, 2026-11-04)

The user's request: run the test sessions much faster than real time while every component believes time runs
normally, as BallanceTAS does for one offline game. Plan only; nothing is built.

## 1. What BallanceTAS does, and why it is not enough here

BallanceTAS (D:/repos/BallanceTAS) hooks `CKTimeManager::PreProcess` (vtable slot 5, `src/Game/TASHook.h`) and,
after the original computed the frame's real delta, overrides it with `SetLastDeltaTime(fixed)` and gives the physics
the same delta (`IpionManager::SetDeltaTime`, `src/Game/GameInterface.cpp:133`); `SkipRenderForNextTick()` drops
renders (`src/BallanceTAS.cpp:423`, Lua `skip_rendering(n)`). The frame loop then runs as fast as the CPU allows and
the game sees 1/66 s per frame.

Our retail driver already does both halves (`BallanceMMOClient/session/fixed_tick.cpp`): the time manager's minimum
and maximum delta pinned to 1000/66 ms, `CK_FRAMERATE_FREE`, and `SkipRenderForNextTick` when behind. It paces to the
wall clock on purpose: the server steps 66 ticks a real second and every member follows it. A faster session needs
every process - the server, the headless clients, the retail clients and the harness's proxy - on one clock that runs
S times faster than real time.

## 2. The rule

One virtual clock per process: `v = v0 + (real - real0) * S`, the same S everywhere (a session-start parameter; S = 1
is today's behaviour exactly). Every input to session logic is virtual time:

- every clock read that decides a step, a deadline, a timeout, a lateness or slack sample, a schedule, a journal ms;
- network milliseconds turned into ticks: GameNetworkingSockets' `m_nPing` and a message's receive age are real, so
  they are multiplied by S where they are read (server.cpp:1031-1036, 1766-1770, 1929-1930, 4702-4704; age at
  2385-2390, 2829-2834); the handshake's own RTT comes from clock stamps and scales by itself;
- the proxy's link model: latency, jitter, spike, step, blackout and outage durations divided by S in real time
  (loss and bursts are counted in datagrams and stay as they are).

Real time stays where the work is real:

- CPU cost decisions: the rewind cost meter and its 45.45 ms hitch budget (rollback.hpp:714-766), the server's step
  budget log - measured and judged in real ms, so the reach cap is the same as in a real-time run of this machine;
- I/O-bound budgets: level load, world boot, the retail restart and reload deadlines (5/10/20 s), the 180 s boot
  panel, the harness's startup timeout and journal waits, the GNS connection timeout;
- GNS internals (retransmit, keepalive, Nagle) - out of reach; the retransmit after a loss is S times longer in
  virtual time, so a lost reliable message is later in ticks than in a real run (the relay itself is unaffected);
- identity: utc_ms, file names, the real-time broadcast; diagnostics (frame and capture timings).

The physics deltas (physics_state.cpp:1094, kFixedDeltaMs, the time manager pinned at 1000/66 ms) never scale.

## 3. Changes per component

### 3.1 Server (VC1)
- A `session_clock` (now(site), sleep_until(virtual), to_real(duration)) owned by the server and the runner, in the
  form the client core already has (`platform.now(clock_site)`, clock_site.hpp): the ~30 direct
  `steady_clock::now()` reads in server.cpp and session_runner.cpp go through it, each with a site of its own (a
  server site enum; inventory in the VC1 step's notes). Why the server has none today: the client's single clock
  entry came from the headless client's capture replay (E0, 2026-10-22: every clock read is an input, recorded and
  handed back by site), and the server never needed one - its journal records each tick's outcome (the frames
  applied, the substitutions, the events' ticks), so a replay re-simulates without re-running any timing decision.
  The same entry makes the server's timing decisions recordable later (optional, not in this plan). `physics.test_time_scale: S` in config.yml (test builds, BMMO_SERVER_TEST_FLAGS), validated 1..8.
- The main loop's pass rate becomes 66*S passes a real second (SERVER_RECEIVE_INTERVAL / S), so the assignment and
  start copies (counted in passes, min gap 5 ms virtual) and the 768-pass ping keep their virtual spans.
- `test_clock_skew` composes on top of the virtual clock (its deadlines are in virtual time).
- Journal TICK ms, the handshake stamps (steady_us) and the start origin are virtual.

### 3.2 Shared core and headless client (VC2)
- The core already reads through `platform.now(clock_site)`; the headless `wall_now` maps every site to virtual
  time except the cost sites (rewind_*), which stay real (a site table, like the capture's free-site mask).
- The two direct reads in rollback.hpp's rewind timer are diagnostics (real).
- The pacer runs at 66*S frames a real second; its 1-2 ms sleeps become waits on the virtual deadline.
- `--time-scale S`; `--seconds` splits into a real startup budget and a virtual play budget (`--play-seconds`
  virtual); test flags with ms (`--stall-ms`, `--pause-ms`, `--hold-event`, `--rejoin-at`) are virtual.

### 3.3 Harness and proxy (VC3)
- `--time-scale S` passes S to the server's config, every client and the proxy.
- The proxy divides every link-model duration by S; its select timeout drops from 10 ms to 1 ms (at S = 4 a tick is
  3.8 ms real); its clock stays `time.monotonic()` (QueryPerformanceCounter on Windows).
- The judge: journal ms are virtual, so tick gaps, cadence and the 15 ms bounds keep their meaning. Every criterion
  is tagged real-only or scale-safe. Real-only (reported "n/a (time scale S)"): anything a CPU stall moves - relay lag
  in D, the TICK gap near a hold, rollback ticks against control, frame and step timings, E-core figures - because a
  40 ms real stall is 2.6 ticks at S = 1 and 10.6 at S = 4. Scale-safe: FINAL exactness, journal replay N/N, resync
  and refusal counts where no CPU stall is involved, event delivery, UNCONFIRMED rows, lifecycle outcomes.

### 3.4 Retail client (VC4)
- The fixed tick driver's origin and schedule on the virtual clock (kTickSeconds / S real); the min/max delta stay
  pinned at 1000/66 ms, so the game's scripts still see 1/66 s a frame - BallanceTAS's half we already have.
- Renders: one in K frames (`SkipRenderForNextTick` on the others, K from S and the measured render cost); the anchor
  hold's ImGui frame keeps drawing.
- core_host's direct clock reads (2645, 5219, 5231, 5323, 4113) go through `p.now`; `p.now` maps sites like the
  headless client; the countdown (3 s) and hold timeouts are virtual, restart/reload deadlines real.
- The network thread's poll interval (client.h CLIENT_RECEIVE_INTERVAL) becomes 1/(66*S) s; the `net lag` and
  `net blackout` verbs scale their ms.
- Windows timer resolution: in test mode only, `timeBeginPeriod(1)` (or a high-resolution waitable timer) for the
  1 ms sleeps - a system call the CK/BML+ APIs do not offer; justified and test-only.

## 4. How fast

Measured today: a twin pair ~210 s = 2 x (boot ~10 s + start ~5 s + 60 s play + teardown) + ~40 s of audit, replay
and judge. At S = 4: ~2 x (10 + 5 + 15) + 40 = ~100 s, about 2x; the audit/replay (CPU-bound, offline) can run in
parallel with the next pair for more. Limits on S: headless clients ~1.5-2 ms of CPU a tick on the E-cores (S <= ~5
at three cores a client), the server's step cost, the retail frame without render (to measure; S = 2-3 expected),
and OS sleep resolution (1 ms).

## 5. Steps and checks

| Step | Scope | Done when |
|---|---|---|
| VC1 | server clock, config knob, ping/age scaling, pass rate | unit tests of the clock and conversions; S = 1 wire check ALL SAME; a server-only S = 2 run steps 132 ticks a real second, journal ms 15.15 a tick |
| VC2 | core sites table, headless pacer and flags | A0 headless at S = 1 unchanged (wire check), at S = 2 and 4 FINAL exact, replay N/N, journal cadence 14-16.5 virtual ms |
| VC3 | harness/proxy scaling, judge tagging | the same arm at S = 1 and S = 4: every scale-safe criterion agrees; LBM's lifecycle and swing arms at S = 4 FINAL exact |
| VC4 | retail driver, render skipping, core_host reads, timer resolution | retail L11 ctl at S = 2: FINAL exact both clients, replay N/N; real frames per second and render share reported |
| VC5 | measurement | wall time per pair at S = 1/2/4, the chosen default S for the fast tier |

S = 1 must be today's code path to the byte (wire check and the existing tests); every step lands with that check.

## 5a. VC6: an event-driven mode for the server and the headless clients (S -> infinity)

A scaled clock has a ceiling: a tick's real time (15.15 ms / S) must stay above every process's per-tick CPU, sleeps
are 1 ms coarse, and GameNetworkingSockets' retransmits and timeouts run on its own real clock. The other way is
discrete-event time: a coordinator owns one global virtual clock; each process does the work of the current instant,
reports "done, my next event is at t", and the coordinator advances the clock to the earliest next event once every
process is done. The network is simulated by the coordinator too (a datagram with 150 ms of latency is handed over at
virtual +150 ms), so no step ever waits on real time and the run goes as fast as the slowest process computes a tick
(the headless clients' 2-3 ms: about 5-7x real time, adapting by itself, without a timer-resolution limit).

- What it buys beyond a scaled clock: full reproducibility - one recipe and one loss seed give the same bytes every
  run (today's runs move with the machine's load: A6m superseded's +1 relay tick); no idle waiting; no S to choose.
- Its costs:
  - the transport: GNS cannot be driven from outside, so in this mode the server and the headless clients use a
    coordinator-controlled transport (loss, latency, reordering modelled in virtual time) behind the interface the
    session code already sends through;
  - CPU in virtual time: a 40 ms rollback stall costs 0 virtual ms unless the measured real cost is charged to the
    virtual clock (realism back, reproducibility partly lost) - a per-run switch, off by default;
  - every wait becomes "report and wait for the coordinator's release" (the VC1-VC2 clock entries are where).
- Scope: the server and the headless clients only; the retail client keeps the scaled clock (the Virtools frame loop,
  its render and its GNS are not ours to step).
- Use: statistical reruns, protocol and soak runs, FINAL/replay arms. Never the timing criteria or perf figures.
- Order: after VC5 - VC1-VC2's single clock entries are its prerequisite. Effort: two to three times VC1-VC3.
- Done when: the same headless arm run twice in this mode gives byte-identical journals on every side; its scale-safe
  criteria agree with the same arm at S = 1; the wall time per pair is reported.

Note: for FINAL and determinism alone an infinitely fast path exists today - the server's journal replay and the
headless capture replay run at full CPU speed, with no real time involved.

## 6. Decisions for the user

1. Which tests use the fast tier: proposed - FINAL/replay/protocol arms, soak runs and reruns for statistics at
   S = 4; the LBM timing criteria and every perf figure stay at S = 1.
2. CPU-cost decisions on real time (the reach cap from the real rewind cost): proposed, so a fast run makes the same
   cap a real run makes on this machine.
3. `timeBeginPeriod(1)` in test mode on the retail client and the headless client (Windows; a system call).
4. The default S once VC5 measured it.
