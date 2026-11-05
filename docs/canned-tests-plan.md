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

## 2. Stages (goals; built from the seams of section 3)

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

## 3. Order of work (revised 2026-11-04): the seams in core first, the tools last

The user's rule: the changes that carry risk for the shipped product come first, built once in
BallanceMMOCommon (DRY across the server, the headless client and the retail client), observable, and split
behind narrow interfaces (GNS among them); the stage tools come after, each a thin layer over those seams.

Today every host does its own: the headless client records and replays its clock and messages itself
(`session_client.cpp:2336` wall_now, `inbound_capture.hpp`), the retail host ignores the clock sites
(`core_host.cpp:4182`), the server reads `steady_clock` directly (~30 places) and mixes it with GNS's own
clock (`server.cpp:2400-2405`), and all three hold a raw `ISteamNetworkingSockets*` (`role.hpp:60`) with a
static callback trampoline (`role.hpp:125-144`). The seams below replace those per-host copies.

### 3.1 The seams (product code, BallanceMMOCommon)

| | seam | what it owns | production implementation | other implementations | observable through |
|---|---|---|---|---|---|
| C1 | `session::clock` | every clock read that decides something, by site: `now(site)`, `utc_ms(site)`, `wait_until(t)`; one site enum for every process (the client's `clock_site`, append-only, gains the server's sites) | `steady_clock` / `system_clock`, as today | recorder, replayer, virtual (coordinator) | C3 |
| C2 | `net::transport` | the GNS calls the code uses (listen, connect, accept, close with linger, send by mode, receive, status changes, ping), a message as `{connection, bytes, received_at}` with `received_at` already on C1's clock; connection handles it hands out | `gns_transport`: GNS init/kill, the callback registry instead of `role::this_instance_` | recorder, replayer, simulated (coordinator) | C3 |
| C3 | `session::tap` | one typed record stream of every outside input a process takes (a C1 read, a C2 message or status change, a loop pass, a C8 input) and every send; the capture file is one consumer, `--trace-sends` another | off (no consumer: one branch per call) | capture writer (`.bmic` v2, any process), send trace, live counters | - |
| C4 | `pump(now) -> next_due` | each loop's work of one instant: the server's network pass and its runner pass (today two threads racing, `session_runner.cpp:1980-1997`), the headless pacer, the retail fixed-tick driver | the same threads as today, each calling its pump and waiting `wait_until(next_due)` | the coordinator calls the pumps one after another at its instants | `next_due` |
| C5 | `session::entropy` + wire epoch | the login uuid's seed (`session_client.cpp:1376`), the server's steady-epoch values on the wire (`server.cpp:2063-2065`) counted from C1's session epoch, the system-time broadcast (`server.cpp:4725`) through C1 | random seed, real epoch | a seed from the recipe, epoch 0 | C3 |
| C6 | `session::cost_model` | the rewind cost the reach cap is sized from (`rollback.hpp:723-766`, sites rewind_*) | the measurement, as today | a fixed model (decision 1) | C3 |
| C7 | the retail host's own outside inputs | the direct clock reads (`fixed_tick.cpp:87-108`, the deadlines, the 3-2-1, `core_host.cpp:2645, 5323`), Virtools `GetTime()` in mod state (`BallanceMMOClient.cpp:406, 436, 627, 716`), logic on the network thread (`BallanceMMOClient.cpp:1971-1973, 2252-2267`), state changes in `OnRender` (`core_host.cpp:2845, 2970-3126, 3583-3650`), the keyboard buffer (`session/input_injector.cpp`, already hooked) | through C1, C2 and C3; network-thread logic moved to the game thread's C2 receive | the C3 recorder / replayer | C3 |
| C8 | own-input source in the core | where the own ball's frame of a tick comes from: the platform's poll (today), or an injected frame keys + camera, journaled FRESH and sent (the forced-frame path, `client_core_held.cpp:322, 666, 674`, `client_core_frame.cpp:915-931`, made a public, test-only source) | the poll, as today | a recipe's frames | C3 |

Every seam lands with today's behaviour as its production implementation and is proven unchanged before
anything uses its other implementations: unit tests, the wire check of core_ref_s16
(`NEW_EXTRA=--no-final-checkpoints`: sends and journals identical), the server journal replay N/N, and for
C4 / C7 (timing) the LBM arms.

### 3.2 Steps

| step | seam | touches | risk for the product | done when |
|---|---|---|---|---|
| K1 | C1 clock | server.cpp + session_runner (~30 reads), the headless wall_now (moves into C1), the retail p.now (sites honoured) | a missed or mis-routed read moves a deadline | every read in the three hosts goes through C1 (a grep has no `steady_clock::now` left outside C1 and the diagnostics); wire check identical; LBM PASS |
| K2 | C2 transport | role.hpp, the server's sends / receives / callbacks, client.h, session_client.cpp | message lifetime, callback order (messages before status changes), linger | the three hosts hold a `net::transport`; GNS calls only in `gns_transport`; wire check identical; LBM PASS; a unit test pair over the simulated transport |
| K3 | C3 tap + capture v2 | the headless inbound capture becomes C3's writer and a C1 / C2 replayer; the server and the retail host get the same recorder | capture cost on the hot path when off (must be a branch) | core_ref_s16's `.bmic` files still replay (v1 read) and a new v2 capture of each host replays its own run bit for bit |
| K4 | C4 pumps | session_runner's loop, the server's network loop, the headless pacer, the retail driver | the server's threading: input arrival vs the tick's take, the cutoff, a deadlock | the same two server threads call the pumps; wire check identical; server journal replay N/N; LBM PASS (timing criteria at S = 1) |
| K5 | C5 entropy + epoch | the login uuid, the wire's steady values, the broadcast | the clients use those values as differences only (`client_core.cpp:951-1046`; 0 means absent) - to verify; a value change on the wire may need a protocol bump | production bytes unchanged in meaning; a test run with a seed is byte-identical twice |
| K6 | C6 cost model | rollback's rewind timer | none in production (the measurement stays) | the fixed model in unit tests |
| K7 | C7 retail host | fixed_tick, core_host, BallanceMMOClient network handlers and render callbacks | the retail feel and stability (highest retail risk) | a retail run (with the user's go-ahead) PASS on LBM's retail arms; a retail capture replays its own run bit for bit |
| K8 | C8 own-input source | the client core's forced-frame path | the server's substitutions use the same path | unit tests; wire check identical |

K1-K6 and K8 are shared by every stage; K7 serves stage 3 only and can wait for the user's call on stage 3.

As built (K1): `session/session_clock.hpp` (`session_clock`: `now(site)`, `utc_us(site)`, `utc_ms(site)`;
`real_clock`, `real_session_clock()`); the server's 39 sites appended to `clock_site` (server_*, runner_*);
`server::clock_` reaches the runner through `runner_config::clock`; every deciding read of server.cpp and
session_runner.cpp goes through it, the journal's file name and utc_ms included (one read); the headless
wall_now / wall_utc_ms read their live value through it; the retail `p.now` / `p.utc_ms` pass their site to
it. The capture's free-site mask is a `std::bitset<256>` (the site is a u8 in the file; it was one uint64_t).
Left on the real clock on purpose: the runner's step-cost timers, the headless frame-time histogram, the log
and display stamps (login time, uptime); left for later steps: GNS's own clock (K2: the transport turns
`m_usecTimeReceived` into a C1 time), the login uuid's seed (K5), the retail host's own reads (K7).
Checked offline: 794/794 unit tests; the wire check of core_ref_s16 with `--no-final-checkpoints` ALL SAME
(44 send traces, 45 journals identical); x64 and x86 build. The live smoke run (h1_150 against this tree's
Arch server) is held while another session's timing runs use the machine.

### 3.3 The tools (thin, after the seams)

| tool | built on | does |
|---|---|---|
| T0 `scripts/journal_judge.py` (done) | the journals | judges any run |
| T1 coordinator | C1 virtual + C2 simulated + C4 `next_due` + C5 seed + C6 model | starts the server and N headless clients, owns the one clock and the link model (the harness proxy's), advances to the earliest `next_due`; stage 1 |
| T2 recipe | C8 + T1 | extracts each player's frames, joins and leaves from a journal into a recipe, runs it in T1; stage 2 |
| T3 capture replay | C3 + C1 / C2 replayers | replays any process's v2 capture (the server's too); for retail inside Player.exe with renders skipped; method B and stage 3 |

## 4. Decisions

Taken: stage 0 first (user, 2026-11-04); journal version 10 with `.bmjs` / `.bmjc` (user); 0b as FINAL rows
by default at the checkpoint cadence (recommendation; the alternative, a per-tick hash over the bodies both
sides share, needs one shared-set definition on every side and is left open); SessionJournal off by default
in retail (user); the seams before the tools (user).

Open:
1. Stage 1's rewind cost: the real measurement (the real cap, not reproducible) or a fixed model
   (reproducible, the cap may differ from a real machine).
2. The retail scaled clock (old VC4): kept or dropped.
3. The black-box library's first members: core_ref arms' server journals, the user's play journals (retail
   keeps the last 10 in `ModLoader/BMMOJournals`).
4. One clock-site enum for every process (the client's, append-only, gains the server's sites) or one per
   process (recommended: one, so the capture format is one).
5. K7 (the retail host) now, or with stage 3.

## 5. Not covered by any stage
GNS internals (retransmit, Nagle, timeouts), CPU stalls (zero virtual time unless charged), and retail-only
code outside stage 3: the real-time harness and the LBM arms stay for those.
