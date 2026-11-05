# Canned tests: fast, repeatable session tests from black boxes (plan, 2026-11-04)

The user's goal: a canned test that checks the network protocol, the physics engine, the scripts and the
rollback engine as fast as the CPU allows, can be fed with real black-box data, and reports what went wrong.
This plan replaces the scaled clock of `docs/virtual-clock-test-mode.md` (VC1-VC5): a clock S times faster
still waits, and S is capped by every process's per-tick CPU and the 1 ms sleep. Its VC6 (event-driven mode)
is stage 1 here.

## 1. What exists

| | A: server journal replay | B: headless capture replay + wire check |
|---|---|---|
| command | `BallanceMMOSimTool --replay-session` | `--replay-inbound` (`.local-tools/wire_check.sh`) |
| re-runs | the server world (physics + scripts) | the whole client core: messages, prediction, rollback, sends |
| from | the server journal's applied inputs and events | the capture: received messages, connection events, clock reads by site |
| speed | CPU | CPU (no network, no sleep) |
| answers | "the server world did not change" | "the client did not change" |

Both are open loop: the other side's answers are recorded. A deliberate protocol or rollback change makes them
diverge at once, which is why the reference sets were re-captured from s6 to s16. They cannot tell whether new
behaviour is right; only the real-time harness (closed loop, real GNS, real clock) can, and it is slow and not
reproducible run to run.

Recordings today:

| | server journal | client journal | `.bmic` capture | `.bmrc` |
|---|---|---|---|---|
| written by | the server | each client (headless and retail) | headless only | retail `record` / SimTool |
| inputs | every player's applied frame, fresh or substituted | own frames, relayed frames | received messages (raw) | 256 keys per tick, solo |
| results | authoritative TICK hash, FULL checkpoints | TICK hash at step time (prediction included), LOCAL / RECEIVED / FINAL rows, corrections, resyncs | none | solo world hash + probe |
| outside inputs | TICK ms only | TICK ms only | clock reads by site, passes, connection states | IVP time |

A client's TICK hash holds its predictions and is not rewritten after a rollback, and the client and server
worlds hold different body sets (names, mirrors, per-world airship), so the judges compare bodies
(`scripts/first_divergence.py` keys a ball by its owner, a body by its name), never hashes.

## 2. Stages

| | 0 offline judge | 1 event-driven | 2 black-box re-drive | 3 retail capture |
|---|---|---|---|---|
| upgrade of | A (A plus every journal against the others) | the real-time harness | stage 1 | B (moved to retail) |
| loop | reads only | closed | closed | open |
| player input | as recorded | bots / `.bmrc` | the humans' frames from journals | recorded |
| server logic | read (+ A) | runs | runs | recorded |
| client core, worlds | read | run (headless) | run (headless) | run (retail) |
| retail-only code | read | headless stands in | headless stands in | runs |
| network | read | simulated (fake GNS) | simulated | recorded |
| clock | read | one virtual clock (coordinator) | virtual | recorded |
| CPU cost | read | a model (decision 1) | a model | recorded |
| finds | divergence, starvation, resyncs, lost events in any run, retail included | new behaviour, reproducibly | the same, on real play | one real game reproduced bit for bit |
| survives code changes | yes (reads results) | yes (no recording) | yes (only inputs recorded) | no (re-record, needs a human) |

### Stage 0: offline judge
- 0a (done, `scripts/journal_judge.py`, 7e13ee5e): one table per run from the journals alone - headers, server
  replay N/N, each client's bodies against the server's rows, resyncs, corrections, starvation, own inputs,
  lifecycle events - with the harness's own thresholds. LOCAL rows are reported (they hold predictions), FINAL
  rows judged. Calibrated on core_ref_s16's eleven arms: 11/11 replays N/N, 7 verdicts as the harness's, 4
  explained differences (the harness's own notify-tick deadline; test-requested resyncs; a fault-injected
  client's starvation the harness exempts).
- 0b (this step): every client journal can be judged strictly. Journal version 10: a client journal carries
  FINAL rows at its checkpoint cadence by default (the D0 diagnostic made the default, nothing new in the
  layout); the server journal is named `.bmjs` and a client journal `.bmjc` (readers take `.bmjr` too); the
  judge fails a version 10 client journal without FINAL rows instead of falling back to LOCAL. A server and its
  clients compare at every tick both checkpointed: by default every 660 ticks; denser with the existing
  cadence options.

  As built (0b): `journal.hpp` version 10 and the three extension constants; the server writes
  `session_<id>_level<N>_<UTC>.bmjs`, the retail client `..._p<id>.bmjc` (its pruning counts the old `.bmjr`
  too), the harness names its clients `client<i>.bmjc`; every reader takes all three
  (`journal_trace.journal_files` / `client_journal`). `core_config::journal_final_checkpoints` defaults to on
  (headless `--no-final-checkpoints`, retail `BMMO_JOURNAL_FINAL_CHECKPOINTS=0` turn it off; both FINAL
  switches are left out of a capture replay's argument check: they change what the journal holds, not what
  the session does); the retire observer builds the rows only on a checkpoint tick. The judge excludes
  `resync_rules.hpp`'s keep-local bodies (PE_Balloon, PE_Box_slide: the user's "fix everything except the
  balloon") from the body checks by default and reports them on a row of their own.

  Measured on core_ref_s16 (the x64 build of this step, replays of the eleven arms' 44 captures):
  - with `--no-final-checkpoints`: wire check ALL SAME (44 send traces SAME, 45 journals IDENTICAL);
  - by default: 44 send traces SAME; each of the 45 journals equals its no-FINAL replay record for record once
    its FINAL rows are left out (so the rows are the only difference);
  - strict judgment of those replays (the arms' server journals against the clients' FINAL rows, every 2
    ticks): every shared body within 0.05 m and every own ball exact (< 0.00005 m) in all 45 client journals
    of the 11 arms. Left: the server-side starvation of the client each arm injects a fault into (the harness
    exempts it, a journal cannot tell) and resync_snapoff c2's unrequested resync, as the harness has them.
    Before PE_Box_slide was excluded it failed in cliff and swing_snapoff: each client starts the slide when
    its own ball reaches x ~ 861, the server when the first ball does - the keep-local exception, not a new
    fault.
  - 794/794 unit tests; `arena_map_test.py --selftest` PASS. `test_late_player_harness.py` has three errors
    (`KeyError: 'B'` in relay_start_hold) on the unchanged HEAD as well.

  For the wire check: a version 10 binary writes FINAL rows, so its replays' journals differ from every
  reference set captured before it; pass `NEW_EXTRA=--no-final-checkpoints` to compare them, or re-capture.
  The retail mod (x86) builds; no retail session has been run with it.

### Stage 1: event-driven server and headless clients
A coordinator owns one virtual clock and a simulated network; each process does the work of instant t and
reports its next wake-up; the coordinator advances to the earliest one. No waiting, byte-reproducible.
Blockers found in the shipping code (audit 2026-11-04):
- server: the network thread and the runner thread race (the runner sends from callbacks under state_mutex_,
  server.cpp:1973-2000; whether a frame is buffered when its tick is taken depends on scheduling,
  session_runner.cpp:1980-1997, 1345-1356) - the runner must become a synchronous pump(now);
- server: ~25-30 direct steady_clock reads plus GNS's GetLocalTimestamp back-dating arrivals
  (server.cpp:2400-2405) - one clock entry with one time base;
- server: raw clock values on the wire (steady_us, server.cpp:2063-2065; the system-time broadcast,
  server.cpp:4725);
- GNS hard-wired (role.hpp:60, callbacks through a global, role.hpp:125-144); the fake must supply m_nPing
  (input delay and lead sizing), m_usecTimeReceived, reliable/unreliable semantics, linger close, deterministic
  connection handles;
- the server class and main() share server.cpp (tests cannot link it);
- headless: ready at the clock (wall_now, wall_sleep) but has no next-wake interface; the rewind cost meter
  drives the reach cap (rollback.hpp:723-766 -> client_core_resync.cpp:838-846) and reads 0 ms on a virtual
  clock (cap 120); the login uuid is seeded from the system clock; one client per process (globals).

### Stage 2: re-drive from black boxes
Extract each player's frames (server journal: every player, FRESH vs substituted; client journal: own FRESH),
joins/leaves and the level into a recipe; run it in stage 1 with a network model; judge with stage 0.
Blockers: the forced-own-frame path drives the ball with keys and camera (session_client.cpp:2090,
client_core_held.cpp:674) but is filled only from SessionLead (client_core_held.cpp:322), journaled
SUBSTITUTED and dropped as stale by the server (client_core_frame.cpp:915-931), refused out of sync
(client_core_held.cpp:666); observe_input re-derives the camera and nav flags from the scripts
(session_client.cpp:3952, 904-925). A re-drive is not a reproduction: the humans reacted to what they saw.

### Stage 3: retail capture (optional)
Record every outside input of the retail client and replay it offline in Player.exe at CPU speed.
Blockers: p.now ignores sites (core_host.cpp:4182) and many reads bypass it (fixed_tick.cpp:87-108, deadlines,
the 3-2-1, budgets); Virtools GetTime() is a real clock and feeds mod state (BallanceMMOClient.cpp:406, 436,
627, 716); the network thread runs logic (try_to_lock drops, BallanceMMOClient.cpp:1971-1973; the countdown
sleeps and restarts, 2252-2267); state changes in OnRender (core_host.cpp:2845, 2970-3126, 3583-3650); no
locked frame time before the anchor (fixed_tick.cpp:61-64); mouse, ImGui, console and command channels are
further inputs. Recordings die with every behaviour change and need a human to make: a debugging tool, not a
regression library.

## 3. Decisions

Taken: stage 0 first (user, 2026-11-04); journal version 10 with `.bmjs` / `.bmjc` (user); 0b as FINAL rows
by default at the checkpoint cadence (recommendation; the alternative, a per-tick hash over the bodies both
sides share, needs one shared-set definition on every side and is left open).

Open:
1. Stage 1's rewind cost: the real measurement (the real cap, not reproducible) or a fixed model
   (reproducible, the cap may differ from a real machine).
2. The retail scaled clock (old VC4): kept or dropped.
3. The black-box library's first members: core_ref arms' server journals, the user's play journals (retail
   keeps the last 10 in `ModLoader/BMMOJournals`).

## 4. Not covered by any stage
GNS internals (retransmit, Nagle, timeouts), CPU stalls (zero virtual time unless charged), and retail-only
code outside stage 3: the real-time harness and the LBM arms stay for those.
