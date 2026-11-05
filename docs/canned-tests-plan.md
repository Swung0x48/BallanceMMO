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
  client's starvation the harness exempts). Its first body rule (positions within 0.05 m, the end's 60 ticks
  cut, a body only one side has reported, a run with nothing compared passing) was too weak; see 0c.
- 0b: every client journal can be judged strictly. Journal version 10: a client journal carries
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
  - judgment of those replays (the arms' server journals against the clients' FINAL rows, every 2 ticks),
    first with 0a's tolerance rule, then with 0c's exact one: every confirmed row bit-exact (own, mirror and
    mechanism rows; position, rotation, velocities, flags) and no body only one side has, in all 45 client
    journals of the 11 arms (e.g. h1_150 c0: own 0/944, mirror 0/2948, mechanism rows 0/95598 different). Left: the server-side starvation of the client each arm injects a fault into (the harness
    exempts it, a journal cannot tell) and resync_snapoff c2's unrequested resync, as the harness has them.
    Before PE_Box_slide was excluded it failed in cliff and swing_snapoff: each client starts the slide when
    its own ball reaches x ~ 861, the server when the first ball does - the keep-local exception, not a new
    fault.
  - 794/794 unit tests; `arena_map_test.py --selftest` PASS. `test_late_player_harness.py` has three errors
    (`KeyError: 'B'` in relay_start_hold) on the unchanged HEAD as well.

  For the wire check: a version 10 binary writes FINAL rows, so its replays' journals differ from every
  reference set captured before it; pass `NEW_EXTRA=--no-final-checkpoints` to compare them, or re-capture.
  The retail mod (x86) builds; no retail session has been run with it.

- 0c (the orchestrator's review, 2026-11-05): the judge's default is the project's standard. Every
  confirmed FINAL row bit-exact on the LBM audit's packed row (position, rotation, linear and angular
  velocity, flags), per kind (own, mirror, mechanism); a body only one side has fails; no end cut; coverage
  fails when nothing is compared, when unconfirmed rows appear before the session's last 128 ticks (rollback.hpp
  kReachHistoryTicks: the rows its end retires) outside a test-requested resync's or a join import's pending
  window, or when the compared rows stop before that tail; the replay needs matched == ticks > 0; no --simtool
  fails (--no-replay: N/A); no server journal fails (--client-only: a reported diagnostic against the received
  snapshots); an empty run directory exits 2; --tolerance and --end-cut remain as diagnostics. On the 45 client
  journals: all exact; the remaining FAILs are the fault-injected clients' starvation and resync_snapoff c2's
  unrequested resync (with its unconfirmed rows), as before. The live K1 / K2 smoke runs: exact too.

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
| K4 | C4 waits (split 2026-11-05, section 3.2a) | K4a: the audit of thread-order dependencies (done); K4b: the server's decisions from data only; K4c: the wait seam in session_runner's loop, the server's network loop, the headless pacer | K4b changes the server's scheduling (late-frame boundary, one poll before each step); K4c a deadlock if a wait point held a lock | K4b: unit tests, wire check identical, smoke, LBM PASS (timing criteria); K4c: production waits unchanged, wire check identical, server journal replay N/N |
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
(44 send traces, 45 journals identical); x64 and x86 build. Live smoke (2026-11-04 23:53-23:54, h1_150: 4
headless clients, 150 ms, 40 s, against this tree's Windows server as a renamed exe on port 26995, all on
P-cores 0-15): harness PASS, server journal replay 2641/2641, and the judge on the v10 journals (the first live
`.bmjs` / `.bmjc` with FINAL rows): every confirmed row of all four clients bit-exact (0c's rule), no resync.
LBM not run.

As built (K2, the user's call: every GNS type replaced, a platform layer): `net/transport.hpp` (the
connection handle, the transport clock, send flags, connection states and end-reason ranges with the
library's values; `message {conn, data, size, received}`, `status_change`, `connection_status`; the
`transport` interface: listen, accept, connect, send, flush, close with linger, set_name, status,
detailed_status, remote_address, receive, poll_status, local_timestamp, the test link faults, the connection
timeout; `process_timestamp()`), `net/gns.hpp` + `src/net/gns_transport.cpp` (init / kill / log level /
make_transport: the one file that includes GNS, static_asserts that every value is the library's), and
`net/sim_transport.hpp` (`sim_network`: in-process transports on one virtual clock, per-direction latency,
reliable in order, seeded unreliable loss, a lingering close that arrives after its data). `role` owns a
transport (GNS by default, any other through its constructor) and dispatches `message` / `status_change`; the
server, the headless client, the retail client, the mock client and the record parser use only these types;
`bmmo::log_type` replaces the library's debug output type in the logger. Steps: K2a 1169fec4 (names), K2b + K2c
(this commit). Checked: 797/797 unit tests (3 new: the simulated transport's life, order and linger, seeded
loss); the wire check of core_ref_s16 (`--no-final-checkpoints`) ALL SAME; x64 (server, session client,
SimTool, tests, mock client, record parser) and x86 build; live smoke on P-cores (2026-11-05 00:17:58-00:19:03,
h1_150): harness PASS, server replay 2641/2641, the judge exact on all four clients' FINAL rows, the server's
connection-request and closed-by-peer lines as before. Not run: LBM, a retail session, the mock client and the
record parser against a server. The received time stays on the transport's clock (as the library's was); its
mapping onto C1 waits for the stage 1 coordinator. Also live (00:25:09-00:25:58, 2 clients, 30 s, no proxy,
`BMMO_FAKE_LAG_MS=75`): the transport's fake lag took (both clients logged it), the server's status read a
150 ms round trip and sized the room's input delay from it (9 ticks), harness PASS, replay 1981/1981.
Unexercised GNS adapter paths: accept refused / no receive set, the retail `net lag` / `net blackout` verbs,
`connection_timeout_ms`.

As built (K3, narrowed): `session/capture.hpp` (`capture_file`: the headless inbound capture moved into
Common, format version 2 - N messages with the connection and the transport's receive time, T state changes
with every field, P status reads, L transport-clock reads - written in place of v1's M and S; the header and
the I / C / E records unchanged, a version 1 file reads as before) and `session/capture_seams.hpp`
(`recording_clock` / `replay_clock` / `recording_transport` / `replay_transport`: the record is written
before the host handles a message or a state change, a free site is read from the replay's own clock, utc_ms
and utc_us are recorded in the unit read, a read the live run did not make calls the host's divergence). The
headless client runs on them: `--capture-inbound` wraps its clock and transport with the recording twins,
`--replay-inbound` with the replaying ones; its own copies of the capture and replay code are gone (the pass
records, the report_init value, the divergence report and the send guards stay: loop structure). Not in
K3, changed from the table above: the server's recorder lands with K4 (its two racing threads make a capture
unreplayable until then), the retail one with K7; no generic tap bus until a second consumer exists. Checked:
798/798 unit tests (one new: record a simulated session, replay it, a read too many diverges); the wire check
of core_ref_s16 (version 1 captures through the replaying twins) ALL SAME; live (2026-11-05 01:02:16-01:03:20,
P-cores, h1_150, every client capturing): harness PASS, server replay 2641/2641, and each client's new v2
capture replayed by the same build gave its live send trace (SAME) and its live journal (IDENTICAL) back.
Readers outside the repo: `.local-tools/e0_replay.py` reads only the header (unchanged); the one-off parsers in
`.local-tools/work/` (FIN/level_finish_test.py, LBX/capture_relay.py, ...) stop at an unknown tag, so they
read version 1 captures only.

Two limits of `sim_network` for stage 1: it is single-threaded (today's server sends from its runner thread
too, so it can carry the server only once K4 runs the server's two loops from one thread), and its loss
draws come from `std::uniform_real_distribution`, which differs between standard libraries: "same seed, same
losses" holds within one build; a coordinator mixing a Linux server with Windows clients needs its own
generator.

### 3.2a K4a: where the server's results depend on thread order (audit, 2026-11-05)

The user's rule (2026-11-05): the server's results must not depend on how its two threads overlap. Today they
do, so K4 is split: K4a lists the dependencies (this section), K4b makes each decision depend only on data (a
message's receive time, a tick number), K4c adds the wait seam (C4) and the virtual clock. Once K4b is done, the
stage 1 coordinator need not keep the threads from overlapping; running one black box under several thread
orders and getting the same journals becomes a test of K4b.

The mechanism behind most items: the network thread polls the transport once per pass (15.15 ms,
`server.cpp:81-87`) and `post()`s each message's work to the runner (`session_runner.cpp:131-137`); the runner
applies the queue only when it drains it, once before each step (`:2013`, B2SX), and then reads "now" or the
tick it is on. A message's receive time is data (`server.cpp:2829-2833`, back-dated from the transport's stamp);
**the drain time and the tick at the drain are not**: they shift with the pass phase, with the step's cost and
with the OS scheduler.

**A. What a message counts for is decided at the drain (outcome: the world, the wire)**

| # | where | decided from | outcome that varies |
|---|---|---|---|
| A1 | the frame take, `step()` `session_runner.cpp:1343-1356`, `take` `:1380` | the buffer's content at the drain; the arrival cutoff applies only to a step more than a tick past its deadline | a frame that arrived before the deadline is applied or substituted |
| A2 | patience, `run()` `:2021-2063` (`patience_step_test` on the members' buffers at `at`, `due(at)`), the wait `:2145-2154` | the buffers and the clock when the runner wakes | a tick steps early without a member (a later frame is late), or waits; the cut lists |
| A3 | an event's applied tick, `submit_event` `:973-975` (`max(stamp, next)`), the trafo ledger `:1034, 1050` | `next` = the tick at the drain | a late event is applied at another tick; supersede / refuse verdicts |
| A4 | joins and leaves, `add_player` `:472`, `remove_player` `:565`, the leaver's events `:631-663` | the tick at the drain | the tick a ball is added or taken out |
| A5 | a resync request, `request_resync_state` `:1161-1185` | the tick at the drain vs the base | answered, or "base missed" and re-anchored |
| A6 | a ready / re-assign, `player_ready` `:701-769` (`buffer.reset`, `baseline.assigned(..., tick)`); `player_start_settled` `:777` (`settled(epoch, now)`) | the tick and the clock at the drain | the baseline the lateness meter measures from |
| A7 | the lead control's inputs: `submit_input` `:868-940` (the lookahead cursor at the drain `:880`, `c_est` from the tick at the drain `:925-932`); `observe_relative` `:1580-1606` (a missing frame counted at the step's `now`, `:2119`); `observe_quorum_arrival` `:1788` (waiver samples from `seen`, the drain time); the R12 wait meter `:2084-2097` (from `at`) | the drain time and tick, the step's clock | which tick a lead raise / lower is decided at (SessionLead on the wire); the wait value in every relay header |

**B. The network thread reads the runner's published view (stale by up to one runner pass)**

| # | where | outcome that varies |
|---|---|---|
| B1 | `running()` at `server.cpp:1718, 1775` | a joiner waits for the handshake / joins as a start member / joins late |
| B2 | `running()` at `server.cpp:2202, 2348` | the pass in which waiting joiners and the start resyncs go out |
| B3 | `running()` at `server.cpp:2512` | the start barrier fires (silent members dropped) or is disarmed |
| B4 | `running()` at `server.cpp:2746` | a SessionResync is answered or ignored |
| B5 | `reanchor_tick` `session_runner.cpp:184-221` (the floor from the view's `next_tick`), `leaving_horizon` `server.cpp:2682` | a late joiner's or a resync's base when the floor or the horizon binds |

**C. Two threads send**

| # | where | outcome that varies |
|---|---|---|
| C1 | the runner's callbacks send under `state_mutex_` (`on_inputs` relay, `on_event_accepted`, `on_player_added` SessionStart, `on_lead`, snapshots, resync states, base missed: `server.cpp:899-920`), the network thread sends too (`tick()` broadcast, assign repeats, handshake) | the order of the two streams on one connection; not yet checked whether any client reads one stream relative to the other |

**Not thread-order dependencies (no change for K4b)**: the step itself given its inputs; events of one tick,
applied in join order (`:1405-1416`, order-independent by design); the network thread's own deadlines (start
barrier, assign repeats, handshake, rtt posts), which depend on the pass times only and so follow the virtual
clock; the scheduled start's origin (`server.cpp:879`, `scheduled_start`: from the handshake, not the drain);
diagnostics only: the event's `intake` / margin / slack (`:945`), `arrival_queue`, `quorum_wake`, `current_tick`
in log lines (`server.cpp:3076, 3241`), the step-cost timers.

**K4b direction (to agree before code)**
- Every posted command carries its message's receive time; the runner applies it by that time against the
  tick's deadline, not by the drain (A3-A7: the tick a command counts for is the first tick whose deadline is at
  or after its receive time, or a rule of the same kind).
- A handover mark: the network thread publishes "every message received up to W is posted"; the runner takes
  tick T (A1) or makes a patience decision at instant X (A2) only with W at or past that instant, from the frames
  received by it. The network thread must then poll on demand (woken by the runner) instead of only every 15.15
  ms, or the mark lags by up to a pass. A fixed grace after the deadline (today's runner wakes 1-2 ms late and
  takes what came meanwhile) keeps today's leniency as a rule instead of luck.
- B1-B5: the decision taken from the session's own data (the schedule and the message's receive time), or moved
  to the runner and answered by a callback, instead of a read of the view.
- C1: first check whether any client depends on the order between the two streams; if one does, one sender per
  connection.
- Cost: a frame that today lands in the runner's late wake-up gap becomes late unless the grace covers it;
  the handover adds up to one network poll before each step. Measured by LBM (timing criteria) before K4c.

#### K4b design (2026-11-05, the user's choices: grace from today's late-wake p99, a wakeable network poll, no old path, the lateness basis may change, the server capture as the acceptance test)

The principle: the runner is a pure function of the stream the network thread hands it - stamped commands in
stamp order - and of nothing else; when it runs only decides when the results come out, never what they are.

- **Stamps.** Every `post()` carries a stamp on the session clock (C1): a message's receive time (one clock pair
  per poll, `arrival = P_c - (P_t - received)`, P_c / P_t read once before the receive: no per-message clock
  reads); a connection status change or a network-thread timer (start barrier, handshake, assign repeats, rtt
  posts) the poll's P_c. Stamps are posted in nondecreasing order (the poll group's queue is in enqueue order,
  which is stamp order; a timer's P_c is at or after every arrival of its pass); a stamp below the last one is
  raised to it.
- **The handover mark W.** After a poll (and its timers), the network thread posts the mark W = P_c: every
  command stamped at or before W has been posted. Exact for GNS: the library stamps a packet
  (`usecRecvFromEnd`, `steamnetworkingsockets_socketthread.cpp:2573`) and enqueues its message under the one
  global lock that `ReceiveMessages` takes, so a message stamped before P_t is in the queue by the receive. A
  receive that returns the batch cap (1024) is repeated in the same pass until it returns less (reliable
  gap-fills give several messages one stamp, so "the last message's stamp" is no mark). Command stamps are raised
  to stay monotonic; marks never are.
- **The logical timeline.** The runner keeps a logical now L. Each running session's next tick T has a step
  instant X(T): the earliest x >= L at which today's step test passes with the state as of L - all frames in
  (x = L), a member's patience threshold, or the deadline plus the grace (`due(x)` = x >= deadline(T) + grace).
  The runner repeatedly takes the earliest of: the next command (stamp a <= W; a command at x = X goes first,
  the take's "arrived by" being `at <= cutoff`), applied with L = a; or the earliest X <= W, stepped with L = X.
  With neither, it blocks until a post arrives; the real time reaching the earliest X is only its cue to ask
  the network thread for a poll (the mark has to pass X) - never a busy loop while W < X, never a sleep on X
  read against another process's clock (a replay). Every deciding clock read in the runner becomes L (in a
  command: its stamp; in a step: X); the runner's remaining clock reads only size its waits (free sites, not
  captured). The tick a command counts for follows: the commands applied before step T are in effect for T.
- **What this removes.** The drain-time rule of A1-A7 (`seen` becomes the stamp; `tick_index()` inside a
  command is now the logical one; the B2SX catch-up cutoff becomes the take's cutoff X on every step); the
  frames a late wake-up used to take are the grace's.
- **The network thread's waits.** Its two sleeps become a wait the runner can cut short with a poll request
  ("need W >= X"); a requested poll is a receive pass only (messages, status changes, the mark): the pass timers
  keep their 15.15 ms cadence and `main_pass_` counts regular passes only (the STR assign-repeat cadence counts
  them).
- **B1-B5: the network thread asks, the runner decides at the request's stamp, a callback answers.** The joiner's
  start / late / waiting choice, the start barrier, the start resyncs and the waiting joiners, a SessionResync,
  the re-anchor's base (its floor is the logical next tick) - each becomes a stamped command whose answer comes
  back by callback (under `state_mutex_`, as every runner callback does today) and is sent from there.
- **Not changed.** The wire and the protocol; the step; C1 (the runner's callbacks still send from the runner
  thread: their relative order with the network thread's sends is the real-time order, as today; stage 1 fixes
  it with the coordinator's rule at equal virtual instants, K4c).
- **To check while building.** Network-thread state written by runner callbacks (`s.late`, `awaiting_events`,
  relay acknowledgements, ...): a post decided from it would make the stream depend on the runner's timing.
- **The acceptance test.** The server's capture records the network thread's inputs only (transport messages
  and status, its clock reads); a replay feeds them back with the runner perturbed (random sleeps before steps
  and drains) and must give the live server journal byte for byte, several perturbation seeds. Its
  prerequisites: in a replay a poll request is a no-op and the network thread replays its recorded passes (a
  regular pass and a requested one look alike in the capture) as fast as it can, then posts a terminal mark;
  `server::clock_` rebindable (as the headless client's in K3); the runner on the real clock (`rc.clock`), not the
  recording one, so a second thread never interleaves into the one capture stream; the journal header's start and
  UTC values read on the network thread and passed in `create_session` (today read in the command body,
  `session_runner.cpp:295`).
- **Order.** K4b-i: stamps, the mark, the logical loop, the wakeable poll (A1-A7); gate: unit tests, wire check,
  smoke. K4b-ii: the server capture and the perturbed replay test (it finds any callback-written state that a
  post depends on). K4b-iii: B1-B5 and whatever K4b-ii found. Each committed on its gate; LBM after K4b-iii.

### 3.3 The tools (thin, after the seams)

| tool | built on | does |
|---|---|---|
| T0 `scripts/journal_judge.py` (done) | the journals | judges any run |
| T1 coordinator | C1 virtual + C2 simulated + C4 `next_due` + C5 seed + C6 model | starts the server and N headless clients, owns the one clock and the link model (the harness proxy's), advances to the earliest `next_due`; stage 1 |
| T2 recipe | C8 + T1 | extracts each player's frames, joins and leaves from a journal into a recipe, runs it in T1; stage 2 |
| T3 capture replay | C3 + C1 / C2 replayers | replays any process's v2 capture (the server's too); for retail inside Player.exe with renders skipped; method B and stage 3 |

## 4. Decisions

Taken: the gate (user, 2026-11-05): each K step lands on unit tests, the wire check and a P-core live smoke;
LBM runs in full once, before the branch is merged into collision-overhaul. Stage 0 first (user, 2026-11-04); journal version 10 with `.bmjs` / `.bmjc` (user); 0b as FINAL rows
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
