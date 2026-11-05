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

As built (K4b-i, 2026-11-05): `session_runner::input_stamp` (a thread-local stamp the network thread holds per
message - its arrival from the pass's one clock pair, `server::arrival_of` - and per status change and pass timers -
the pass's instant `poll_at_`), `post` stamps every command (nondecreasing), `mark` takes the handover mark after
every receive pass; `run` is the logical loop (`bmmo::session::next_logical_move`: command, step, wait, idle;
unit-tested), `step_instant` / `step_verdict` / `step_session` the step rule on `logical_` with `kStepGrace` = 2 ms
shifting the deadline and every patience threshold; every deciding runner time is `logical_` (the take's cutoff, the
event's arrival, `seen`, the start origin, the journal's start and TICK ms, the leaver's and the deadline trafo's
times, the baseline's settle); the server's waits are `wait_until` (a requested receive pass: messages, status, the
mark; no timers, no `main_pass_`), a receive repeats past its 1024 cap. Removed: B2SX's catch-up-only cutoff, the
drain-time `seen`, `late_join_tick` (no caller), `max_catch_up_ticks`, the wake / patience_wake diagnostics. New log
fields (S1 quorum line, a product log change): `seen-at` is now the real arrival-to-apply lag, `step-lag-ms` the
real time a step ran after its step instant, `after-deadline` / `after-deadline-ms` the frames the grace took.
The grace: three P-core arms on the old code (h1_150; + 20 ms jitter and 2 % loss; + 50 ms jitter, lead adjust off)
stepped no tick past its deadline and cut nothing by patience, so no p99 could be measured; 2 ms is the old late
wake's bound (GNS sets the 1 ms timer resolution; the old sleep was whole milliseconds), to be read in LBM's stall /
blackout arms from the new fields. Checked: 809/809 unit tests; the wire check of core_ref_s16: 44 send traces SAME,
5 journals DIFFERENT (resync_snapoff c2, resync_at150 c0, resync_at150_down c0, fan_dr c0, fan_dr c3.rejoin1) - the
same 5 with collision-overhaul's own build (6861e716, whose replays these match record for record): the merged
CBF1 F1 / CBF3 client changes, not K4b (core_ref_s16 predates them); live P-core smoke (h1_150): harness PASS,
server replay 2641/2641, the judge exact on all four clients; `step-lag-ms` p50 8 ms / p95 16 ms - the network
pass's 15.15 ms cadence, the same queueing the old `seen-at` showed (0.53 ticks). Not run: LBM (after K4b-iii).

As built (K4b-ii, 2026-11-05; the infrastructure - the test does not pass yet): the server's `--capture-inbound` /
`--replay-inbound` (test builds): `capture_seams.hpp` records / replays the reads of one thread (`capture_owner`:
the constructing thread, then the network loop's - the runner and its callbacks read the real clock and are not in
the capture); `capture_file::next_is_clock` lets the replay's `wait_until` replay exactly the receive passes the
runner asked for; a replay sends nothing, paces nothing and, when the capture ends, `session_runner::settle`s the
runner at the last mark; the journal header's utc comes from the network thread (`create_session`'s
`journal_utc_ms`); `server::clock_` is a pointer; the runner keeps the real clock; `BMMO_RUNNER_JITTER=SEED:MAX_US`
sleeps the runner randomly before each command and step. First run (h1_150 captured live, harness PASS; replayed
plain and with jitter 2 / 5 / 20 ms): every replay ran to the capture's end, but no session started - the replayed
network thread reaches the members' messages before the runner has booted the world (2 s for the whole capture),
and its decisions read what the runner hands back through callbacks: the world's resetpoints and trafo points
(events REJECTED as "a level without a trafo", "suspicious" poses), `world_ready`, the published tick ("server tick
0"). The same holds live, only hidden by real-time pacing: the network thread's session decisions depend on how far
the runner has got - the reverse direction of K4a's group B, and wider: on_world_ready, on_session_snapshot,
on_session_inputs, on_session_lead and on_event_accepted write network-side state (world_ready, resetpoints,
trafo_points, spawn pose, ticking, notices, relay, lead_repeats, last_physicalize, sector_history, trafo_relayed,
awaiting_events) that the network thread's handlers read. K4b-iii's direction is for the user to choose.

As built (K4b-iii, 2026-11-05, the user's option A): with the runner, every input the network thread takes - a
message, a connection state change, a pass's timers (start barriers, handshakes, assign repeats, rtt posts) - is
handled on the runner thread as a command stamped at its arrival (`server::post_input`), in stamp order with the
ticks; the network thread receives, stamps, posts, marks, keeps its connection list and reads their status once
per pass (into the capture; the handlers read that snapshot, `link_status`). A handler reads the logical now
(`server::instant`, 17 sites), the input's pass (`pass_now`: the assign copies' schedule) and transport clock
(`transport_now`), and a wall clock counted from one origin read at the loop's start (`utc_ms_now`); a post a
handler makes runs right after the command at its instant (`session_runner::deferred_`, so a session's world boots
after its handler has sent the room events, as before). Without the runner (physics off, a non-sim build) the
handlers run on the network thread as before. Cost: a world boot (~3 s) holds every input behind it, not only that
session's ticks. Checked: 809/809 unit tests; the wire check of core_ref_s16 ALL SAME (44 send traces; 40 journals,
the 5 known-stale ones skipped); live P-core h1_150 with the server captured: harness PASS, server replay 2641/2641,
the judge exact on all four clients; **the acceptance test passes**: the capture replayed plain and with the runner
perturbed (BMMO_RUNNER_JITTER 2 / 5 / 20 ms before every command and step) gives the live server journal byte for
byte all four times (12,337,074 bytes). Not run: LBM (next, before K4c).

LBM on K4b (2026-11-05, the user: "Run LBM now"; base bc2cc076, LBM5's 19 headless twin arms + the 2 retail L11
arms, on this branch's own Arch server, Windows headless clients and x86 retail client, compared arm by arm with the
main line's LBM5): all 38 headless twins FINAL exact and server replay exact; every one of LBM5's 11 open failures
passes on the flags-on twin (a6m_death c1 rollback ticks / late-ball rewinds, a4_swing_seed2 c3's resync, a8_r1..r3's
late-ball rewinds: the late frames are now judged by their arrival, so the server's own queueing no longer makes them
later); new raw failures: only three "own ball 0.0000" LOCAL items in the 10 %-loss A4 swing arms (LBM5's carried
class B, a different client per run); two timing items seen in overlap windows (A6M's server TICK gap, A8:r1's
joiner lead on an rtt tick boundary, 516.2 ms) did not recur in quiet reruns. Retail L11 ctl and raised: both
clients' FINAL rows exact (own, mirror, 46k body rows), no missing rows, server replay exact, the raised arm's one
lead decision identical to the main line's. The first retail run of this branch (K2's transport, K3, K4b). Overlap
with the main session's work logged (.local-tools/work/LBM5/contention.log); none of the reported results comes from
an arm whose criterion it could explain. Harness notes for a rerun: the Arch journal mirror must copy .bmjs; the
retail chain's data files and scripts/retail/server must exist in the tree that runs it.

As built (K4c, 2026-11-05): the waits are C1's (`session_clock`): `sleep_for(site, d)`, `wait_until(site, cv,
lock, t, ready)`, `wait_for(site, cv, lock, d, ready)` - the real clock sleeps and waits exactly as before (an
unbounded wait is a plain `cv.wait`), and a wait reads no clock, so a capture holds the same reads as before (the
version 1 reference captures replay unchanged). Through it: the server's network-loop wait (`server_loop_wait`),
the runner's three (idle, until a step instant, the 5 ms wait after asking for a pass: `runner_wait`, ended early
by a post, a mark or the stop - `changes_`), the headless client's seven sleeps (`wait_idle`, `wait_test_stall`,
`wait_room_list`, `wait_hold` x2, `wait_test_pause`, `wait_pace`). The capture twins: the recording clock waits on
the live one; the replaying one sleeps nothing on the capture's thread (a replay is unpaced, as before) and waits on
its own clock elsewhere. `session/virtual_clock.hpp`: now() is what `advance_to` set, a wait ends at its time or
when its condition holds, `next_wake` / `waiting` for the coordinator (T1); a condition wait rechecks every 1 ms of
real time (advance_to notifies without the waiters' locks). Left on the real clock: the test knobs' sleeps
(BMMO_SESSION_STALL, BMMO_RUNNER_JITTER), `settle`'s handshake, shutdown and console sleeps. K4 as planned is the
three parts K4a-K4c; the coordinator itself is T1. Checked: 811/811 unit tests (one new, the virtual clock's three
ends; 50 repeats clean); the wire check of core_ref_s16 ALL SAME (5 known-stale journals skipped); live P-core
h1_150 with the server captured: harness PASS, server replay 2641/2641; the K4b acceptance test again: the capture
replayed plain and with 2 / 5 / 20 ms runner jitter, the live server journal byte for byte all four times. The judge on the live run's four clients: every confirmed row bit-exact, no resync.

As built (K5, 2026-11-05), narrowed by K1-K4: of C5's three inputs, two were already on C1 - the server's steady
values on the wire (`server_us` in SessionAssign / SessionStartAt / the clock acks) are `instant()`, the logical
time, and the real-world broadcast reads `clock_->utc_us`; the clients only echo `server_us` and read 0 as absent
(the start instant they act on, `start_us`, the server maps onto the client's own clock), so under a virtual clock
they follow its epoch, which `virtual_clock::kDefaultStart` (1 h) keeps past 0. Left: the headless login uuid, from
the system clock: `--uuid-seed N` takes it from N instead (`mt19937_64`; without it, from the clock as before). Not
an input of a session (audited): the server's `rand()` for an unnamed login and the console's random-player injection,
the login time (display), the config's backup names; the mock client's and record parser's randomness. No
`session::entropy` class: one seed, one consumer. Checked: 811/811; the wire check ALL SAME; two live P-core runs
(h1_150, 20 s) with seeds 11-14 logged the same four uuids twice (f335fdf1, 64395222, 0f83cba2, 477e8b87), harness
PASS, server replay exact. "A seeded run byte-identical twice" needs T1's virtual clock: a live run's timing still
varies.

As built (K6, 2026-11-05): the rewind cost's reads were already C1's (`cost_clock_`, sites rewind_*, K1's era), so
under a virtual clock no time passes in a rewind and the cap would sit at 120. C6 is a fixed model beside the
measurement: `rewind_cost_model {restore_ms, replay_ms_per_tick}`, `rewind_cost_meter::fix` (the cap from the
model, from the session's start; samples ignored; the rewind reads no clock for them), `core_config::fixed_rewind_cost`,
headless `--rewind-cost R:P`. Production: unset, measured as before. Decision 1 stays the user's: which model a canned
test uses (a measured machine's numbers, e.g. the LBM x64 3.0 ms + 0.46 ms a tick, or the real measurement). Checked:
812/812 (one new: the fixed cap holds through ignored samples, clear() returns to measuring); x64 and x86 build; the
wire check ALL SAME; live P-core h1_150 with `--rewind-cost 3:0.46` on all four: harness PASS, server replay exact,
every status line `cap=92/3.00/0.460/1` (the measured smoke's moved 48 -> 82..87).

As built (K8, 2026-11-05): `session_client_core::set_own_input_source(source)` - `source(tick, frame)` fills the
keys, the camera rows and the nav-active flag of a tick it has (false: the poll's). An injected tick takes the forced
tick's path (`platform_.force_own`: the ball driven by the frame, polling off; `unforce_own` after the step) and is
the tick's own input: its keys and camera recorded, journaled FRESH and sent, the flags and ball type the world's
(a SessionLead's forced tick still comes first); `own_inputs_injected()` counts them. The source is kept across
sessions; empty (production, every host today): the poll. No host flag yet - T2 gives the headless client a recipe.
Checked: 813/813 (one new: an injected tick forces 7, plays keys 7 and its camera with the world's flags, not as a
forced tick; the next tick, which the source has not, is the poll's); x64 and x86 build; the wire check ALL SAME; live
P-core h1_150: harness PASS, server replay exact. K1-K8 are done but K7 (the retail host, stage 3, the user's call).

### 3.3 The tools (thin, after the seams)

| tool | built on | does |
|---|---|---|
| T0 `scripts/journal_judge.py` (done) | the journals | judges any run |
| T1 coordinator | C1 virtual + C2 simulated + C4 `next_due` + C5 seed + C6 model | starts the server and N headless clients, owns the one clock and the link model (the harness proxy's), advances to the earliest `next_due`; stage 1 |
| T2 recipe | C8 + T1 | extracts each player's frames, joins and leaves from a journal into a recipe, runs it in T1; stage 2 |
| T3 capture replay | C3 + C1 / C2 replayers | replays any process's v2 capture (the server's too); for retail inside Player.exe with renders skipped; method B and stage 3 |

#### T1 design (2026-11-05, for the user's review)

Goal (stage 1): the server and N headless clients run one session on one virtual time, as fast as the CPU allows,
and the same recipe and seed give the same bytes twice (journals, sends). K4 made this possible: the server's
results no longer depend on how its threads overlap (K4b), and every wait is on the clock (K4c).

Shape: one coordinator process and the usual processes (the server exe, N headless client exes), each started with
`--coordinator 127.0.0.1:PORT`. A process then runs on a `virtual_clock` and a `remote_transport` instead of the real
clock and GNS; nothing else in it changes.

- **The network lives in the coordinator.** It holds one `sim_network` (K2: connections, per-direction latency,
  reliable in order, seeded loss) with one endpoint per process. A process's `remote_transport` forwards its calls:
  a send goes to the coordinator (tagged with the sending thread's rank and its sequence), received messages and
  connection state changes come from it, `status()` answers the link model's round trip. No call waits on the
  coordinator except connect/listen at start.
- **Time moves only at the coordinator.** The loop: every process reports "idle, next wake t_i" (or "no wake");
  the coordinator applies the sends each one made since the last step (in a fixed order: process, thread rank,
  sequence), advances the network to t = min t_i, hands each process the messages and state changes due by t, and
  tells every process "advance to t". A process's threads wake, run, and wait again; when all of them wait, the
  process reports again. Computation costs no virtual time (a world boot, a step); the client's rewind cost is
  K6's fixed model.
- **Idle detection in the process.** `virtual_clock` learns how many threads take part (server: network loop +
  runner; headless: its one loop) and calls the process's coordinator link when all of them wait. To make that exact,
  the K4c seam gains `notify(site, cv)`: a notify through the clock counts the woken waiter out at once, so a thread
  that has been woken but has not run yet never counts as waiting (a plain `cv.notify_all` could let the
  coordinator advance past it). The four host notifies move to it (the runner's post and mark, the server's poll
  request; the runner-thread ones stay plain).
- **Same instant, fixed order.** Two server threads may send at the same virtual instant (K4a's C1). The coordinator
  orders a step's sends by (process, thread rank, sequence), so the wire order is the same every run; each thread's
  own order is kept.
- **Runs.** `BallanceMMOCoordinator --server ARGS --clients N --client-args ... --latency-ms L [--loss-pct P --seed S]
  --virtual-seconds T` starts the processes, runs to T, stops them, and leaves the journals where the live harness
  does, so `journal_judge.py` and the LBM scripts read them unchanged. The clients use `--drive` (scripted, by tick),
  `--uuid-seed` (K5), `--rewind-cost` (K6); T2 adds recipes (K8).
- **Not in T1:** the retail client (stage 3), jitter and bursty loss in the link model (sim_network has neither;
  they come when a test needs them). (Running processes ahead within the link latency came as T1e.)

Acceptance: (1) the same run twice gives byte-identical server and client journals and send traces; (2) the judge
on a T1 run is exact (FINAL rows), like a live run's; (3) speed: virtual seconds per real second for h1_150's shape,
reported.

Steps: T1a the clock's `notify` and idle callback (Common, unit-tested); T1b `remote_transport` + the coordinator
protocol (Common, a unit test over a socket pair); T1c the coordinator exe (BallanceMMOServer/sim) and the
`--coordinator` flag in the server and the headless client; T1d the two-runs acceptance and the judge.

Decided (the user, 2026-11-05: "还是按照你的推荐来吧", after weighing one process with the roles linked in as
libraries - set aside: the IVP random state and the Random hook are process-global, several CK contexts in one process
are unverified, and their steps would have to take turns): separate processes, the coordinator in C++, TCP through
asio, the fixed rewind cost adjustable (the coordinator's `--rewind-cost R:P`, passed to every client, a
per-client override; default 3.0 ms + 0.46 ms a tick - the user: "回滚耗时需要是可调的，那个只是默认值"). The options as
they were put:
1. Language of the coordinator: C++ (reuses `sim_network` as is; recommended) or Python (thin, but re-implements the
   connection model).
2. The IPC: TCP on localhost through asio (already a submodule; works the same on Windows and Linux; recommended).
3. The fixed rewind cost for T1 runs (K6, decision 1): LBM's x64 measurement, 3.0 ms + 0.46 ms a tick (cap 92;
   recommended), or another.

As built (T1a, 2026-11-05): `session_clock::notify(site, cv)` (real: `notify_all`; the capture twins forward); the
host notifies that end a participant's wait go through it (the runner's post, mark and stop; the server's poll
request). `virtual_clock` rewritten around exact idle: a wait the clock ends (notify, or an advance_to reaching its
time) counts as running at once and counts as waiting again only when its thread re-arms it (a generation per wait
closes the race between a waiter's check and its wait); `set_participants(n)`, `wait_idle()` (blocks until all n wait,
returns the earliest time one waits for), `idle()`, `next_wake()`, `waiting()`. Checked: 814/814 (one new: idle is
false between a notify and the woken thread's next wait, and between an advance and the sleeper's end; 30 repeats
clean); the wire check ALL SAME. No live run (the main session's trial games).

As built (T1b, 2026-11-05): `net/coordinator_protocol.hpp` (frames u32 size | u8 op | body over TCP; process ops
tagged rank u8 + sequence u64), `net/remote_transport.hpp` (the process side: every transport call an op to the
coordinator, deliveries and state changes queued until the host polls, `status()` from the link's round trip, the
clock's time as the transport clock; `set_thread_rank`; a link thread reports idle with the next wake and applies the
coordinator's answer; on stop the host's `on_stop`, then the clock runs free until the transport goes; own connects
1, 2, ..., brought-in ones from 0x10000), `net/coordinator.hpp` (the coordinator core: processes numbered by (role,
name) after every hello; a step applies the ops in the order process, rank, sequence, advances the sim_network to the
earliest wake, hands each process its deliveries, state changes - with the round trip once connected - and the time;
a client's latency and loss sit on its end of every link, both ways; a gone process's connections close).
`sim_network`'s `status()` now reports the link's round trip (both ends' latencies; it used to be the asking end's
only, so a server saw 0 ms for a client whose link had the delay on the client's side); `virtual_clock::close()`
releases a blocked `wait_idle`. Checked: 815/815 (one new: a server and a client process on their own virtual clocks
over TCP to a coordinator, 10 ms each way: the server sees a 20 ms round trip, the "hi" sent at +20 ms arrives and is
taken at +30 ms, and two runs log the same events at the same virtual times; 20 repeats clean).

As built (T1c, 2026-11-05): `--coordinator HOST:PORT` (the server's a test flag; the headless client's an
argument): the process opens a `remote_transport` on a `virtual_clock` before it boots (hello, welcome), runs on them
- the server's runner too - and starts the run when its loop starts (participants: the server's network loop and
runner, or the headless loop); the coordinator's stop ends the loop; the server has no console then. The thread rank
lives in transport.hpp (`set_thread_rank`: the server's loop 0, the runner 1). `~server()` releases the runner and the
transport before the clocks (the runner is declared before `clock_owned_`, so it outlived the clock it waits on) and
`~session_client()` the transport. `BallanceMMOCoordinator --processes N [--port P] [--latency-ms L] [--loss-pct X]
[--seed S] [--virtual-seconds T]`: prints its port, runs, reports the steps and the virtual and real seconds.
Checked: 815/815; x64 build with the new exe; the wire check ALL SAME. No live run (the main session's trial games).

As built (T1d, 2026-11-05): `arena_map_test.py --coordinated [--rewind-cost R:P]` (default 3.0:0.46): the harness
starts BallanceMMOCoordinator (one link for every client: --latency-ms, --loss-pct, the loss seed; no proxies, no
jitter, no link events), its own server with `--coordinator`, and every client with `--coordinator`, `--rewind-cost`
and `--uuid-seed 1001+i`; it waits for the coordinator, which stops the server once every client has gone (a rule
added here). Two fixes found by the first runs: a wake is reported rounded UP to the microsecond (a tick's 15151.515 us
rounded down was a time short of the wait: the run stepped in place, 722,000 steps); the coordinator says which
processes it is waiting for after 5 s of real time, and `--trace 1` prints every step.
Results (h1_150's shape: 4 clients, 150 ms each way, 40 s of play, P-cores):
- **byte-identical twice:** two runs, 28,927 steps each, the server journal (11.9 MB) and all four client journals
  (25 MB each) identical byte for byte; the harness PASS both times, the server replay 2630/2630.
- **the judge.** The first pair ran on the harness's own server defaults - snapshots every 2 ticks, a full one
  every 66, patience and lead adjust off - and the judge failed (from tick 1186, ball 65536's orientation a rounding
  bit off the server's on three clients). The control: the same arguments live (proxies, no coordinator) fail the
  judge too (and client0 starves and resyncs), so that failure belongs to that configuration, not to T1 - the first
  note here put it down to a birth rewind, wrongly. With the configuration the project ships and LBM judges
  (snapshots off, patience and lead adjust on - now `--coordinated`'s defaults, `--snapshots on|off` added), two
  coordinated runs: the judge exact on all four clients both times; the server journal, all four client journals AND
  all four send traces (2,694-2,702 sends each) identical byte for byte (`--trace-sends` was not opened on the
  coordinated path - fixed: `open_send_trace` on every flow).
- **speed:** 43.4 virtual s in 43 real s (1.0x) with four clients (0.87x with snapshots on); one client 1.92x. About 1.7 ms of real time a
  step (a round trip with every process); the clients' 2 ms pacing waits make most of the steps. Speed-ups for later:
  the processes running ahead within the link latency (conservative lookahead), a pacer that waits for its frame's
  time in one wait under a virtual clock. (T1e found most of that 1.0x was page heap on the server, below.)

As built (T1e, 2026-11-05, "先提速"): the processes run ahead within the link latency (conservative lookahead).
- **Rounds.** The coordinator grants every process a time, t0 (the earliest wake of all) + the network's lowest
  latency - 1 us: nothing sent from t0 on can arrive by then, so each process runs to it on its own (its link thread
  advances its clock to its next wake while that is within the grant) and reports idle only past it. The ops carry the
  process's virtual time and go in one batch with the idle; the coordinator applies a round's ops in the order time,
  process, rank, sequence, then hands each process what reaches it by the next grant, each message and state change with
  its time - the process holds them until its clock gets there. Anything that turns up inside a granted round fails the
  run. `--lockstep 1` (the exe) / `--lockstep` (arena_map_test.py) grants t0 alone: one wake time per round, the
  reference the lookahead must equal.
- **The network model** (sim_network): a connect reaches the listener, an accept the connector and a close the peer
  only after the link's latency (they were instant, a channel faster than the lookahead); an endpoint learns its own
  connect's "connecting" and its own accept's "connected" at once (remote_transport makes them locally); a close drops
  nothing in flight. `take(until)` hands a process its due messages and notices early; `lowest_latency()`.
- **The end.** Once every client has gone, everything runs to one time - the last client's going + the link's
  latency + 1 s (`kWindDown`: the server's session sees them go) - then stops. (Stopping at once left the server where
  its round happened to end: its journal's tail differed between lockstep and lookahead.)
- **virtual_clock:** a condition wait (`wait_until`) now waits for its end on the clock's own lock and condition (the
  caller's lock is released only once the clock's is held), so an end cannot fall between the two and be missed; a
  missed one cost a 1 ms recheck, which Windows rounds up to its timer tick. `wait_idle(slow, on_slow)` and
  `describe()`: a process not idle for 5 s of real time prints what its clock waits for; the coordinator reports who it
  waits for every 5 s (12 times a round).
- **`--server-exe NAME`** (arena_map_test.py): this machine runs `BallanceMMOServer.exe` under page heap (an Image File
  Execution Options GlobalFlag, matched by the file name); for timings use a copy under another name. Under page heap
  the coordinated run's server took 34.4 s of CPU, without it 1.8 s.
- **Android arm64** (`BMMO_WITHOUT_GNS`, CMake): the server and the headless client without GameNetworkingSockets,
  coordinated runs only (without `--coordinator` they say so and exit). Bionic defines the termios `B0` through asio's
  `<sys/ioctl.h>` (VxIntersect.h names a parameter so): undefined after asio in coordinator_protocol.hpp. Build: NDK
  28.2, android-29, `c++_static`, the headless engine's portable math.
- **GNS out of `role`** (the user, 2026-11-05: "先用stub吧，但是应该等会要重构一下把这个gns调用移走"; the first
  Android build answered `role`'s GNS calls with a stub): `role` no longer knows the library. Its constructor takes
  the transport its host chose (or none, set before the run); `init_socket()` became `init_process()` (the console,
  the start time); `destroy()` only closes the log; `set_logging_level` went. `net/gns.hpp` gained `start(sink)`
  (init or FatalError) and `stop()` (half a second for what is queued, then kill - the old destroy's order). The
  server and the headless client start GNS and make its transport only when not coordinated (a coordinated run
  starts no network library now); the mock client, the record parser and the retail client always. The server hands
  `config_.logging_level` to GNS in `load_config`; its uptime reads a steady start (a diagnostic). A
  `BMMO_WITHOUT_GNS` build leaves `gns_transport.cpp` out and the two hosts' GNS paths behind that macro: no stub.
  Checked: 815/815; x64, x86 retail and arm64 builds; a live run over GNS (2 clients, 50 ms: harness PASS, server
  replay exact, GNS started and stopped); a coordinated run the same as before byte for byte but the journals' build
  id (11 header bytes); the phone the same as the PC 9/9; the wire check of core_ref_s17 ALL SAME, nothing skipped.
  Not run: the retail client (its change: the constructor names the GNS transport it got by default, and its exit
  calls `gns::stop()` before `destroy()`, the same order as before).

Results (h1_150's shape, 4 clients, 150 ms each way, 40 s of play; the server copy without page heap):

| run | rounds | virtual s / real s | speed | CPU s: server, each client |
|---|---|---|---|---|
| PC P-cores, lookahead | 297 | 44.8 / 8.2 | 5.45x | 1.8, 3.8-4.0 |
| PC P-cores, lockstep | 29,077 | 44.8 / 11.9 | 3.75x | 2.6, 5.7-6.1 (coordinator 2.7) |
| PC E-cores 16-27, lookahead | 297 | 44.8 / 12.6 | 3.56x | 3.3, 8.3-8.7 |
| Phone (SM8850, 8 cores), lookahead | 297 | 44.8 / 10.9 | 4.11x | 2.7, 5.5-6.6 |

- lookahead and lockstep: the server journal, all four client journals and all four send traces identical byte for
  byte (9 of 9); three lookahead runs among themselves likewise; the judge exact on all four clients; the harness PASS,
  the server replay 2638/2638.
- the phone (arm64 Android, NDK clang) and the PC (x64 Windows, MSVC): the same nine files identical byte for byte.
- The earlier "stall near tick 2650" was not a bug: that run was killed (by me, for the LBM6 quiet machine).
- Unit tests: 815/815; the coordinator test runs lockstep twice and lookahead twice: the same log all four times, the
  lookahead in fewer rounds.

#### T2 design (2026-11-05, for the user's review)

Goal (stage 2): a black box (a session's server journal, its client journals if there are any) drives a coordinated
run (T1): the same players, each pressing what they pressed tick by tick, joining and leaving at the same ticks, on
the same level - offline, as fast as T1 runs, the same bytes every time. Not a reproduction (the humans reacted to
what they saw, and the original's stalls, resyncs and real network are not in it), but a reproducible re-run of the
session's inputs: a bug that the inputs alone cause comes back every time, and is then cheap to bisect and debug.

What a journal already holds: the HEADER (level, seed, input delay, world options, mechanism slots, first tick), a
PLAYER record per add and remove (tick, id, join order, name, life), an INPUT record per player per tick (keys, the
camera rows, ball type, flags; FRESH when the writer had the player's own frame, SUBSTITUTED when the server stood in),
the EVENTs (lifecycle, client-reported: the re-driven clients make their own), the server's rtt per player in its lead
notes.

The rule (stage 2's row in section 2, "survives code changes: yes, only inputs recorded"; the user, 2026-11-05: the
input file holds only the players' inputs and keeps working when the client and the server are upgraded): a recipe is
extracted from a journal ONCE and holds the inputs and nothing a build computes. A journal is not a recipe: its format
moves with the protocol (a reader refuses versions it no longer reads) and most of it - hashes, checkpoints, events,
corrections - is the build's own output. Only the extraction reads a journal; every later build reads the recipe.

The recipe (proposed format, its own version, fields only ever added):
- the level number and the map's identity (the custom map's file name and md5), so a later build finds the map;
- per player: the display name, the join order, the join tick, the leave ticks and the lives' rejoin ticks - every
  tick counted from the session's first tick;
- per player per tick: the keys (the game keys' bitmask of input_frame), the camera rows (right, up, dir) and the
  nav-active flag, the floats bit for bit; a run of equal frames as one frame and a count;
- NOT in it: ball types, events, hashes, checkpoints, the server's configuration, protocol or journal versions (the
  build makes or knows those); the test's conditions - latency, loss, the rewind cost model, the uuid seeds - are the
  run's parameters (command line, or a separate section of the recipe marked as such), not inputs.

Shape (proposed):
- **Extraction,** `scripts/journal_recipe.py SERVER_JOURNAL [--client CLIENT_JOURNAL ...] -o RECIPE` (scripts/
  journal.py reads the journal; the one version-bound step).
- **The headless client plays a recipe:** `--recipe FILE --recipe-player N` serves player N's frames through K8's
  `set_own_input_source`: every tick it has, that tick's frame; past its last one, nothing pressed.
- **Joins in virtual time:** `--join-at-tick T` - the client boots with the others, holds on the clock and asks to
  join once the session has reached T (like `--join-cue`'s pre-booted joiner); `--leave-room-at` / `--rejoin-at` exist
  for leaves and lives.
- **A driver,** `scripts/recipe_run.py RECIPE`: one client per player, `arena_map_test.py --coordinated`'s pieces,
  then the judge.
- **Acceptance:** (1) a coordinated drive run without beams (a beam is a client's own teleport, not an input),
  extracted and re-driven, gives the original's journals byte for byte (same build); (2) a real black box re-driven
  twice gives the same bytes twice, and the judge's verdict; (3) a recipe written by this build still plays on a later
  one (checked from the first change that touches the session).

Decided (the user, 2026-11-05: "3. 客户端的延迟要可配置。别的按推荐"): 1 the client journal's own FRESH frame when
given, else the server's applied frame; 2 a JSON manifest + a binary frame stream per player; 3 every client's latency
configurable (the coordinator takes a latency per process); 4 inputs only; 5 the trial 2 level 8 journals first.
The options as they were put:
1. A tick the server SUBSTITUTED (the player's own frame came late or never): the player's client journal's own
   FRESH frame when given, else the server's applied frame (recommended: what the world was driven with), or nothing
   pressed.
2. The recipe's encoding: a JSON manifest + a small binary frame stream per player (recommended: compact, frames bit
   exact, the manifest readable and easy to extend), or all JSON (readable, about 4x larger).
3. The network: one latency for every client (T1 as built; recommended for now) or each player's from the server's
   measured rtt (later, once a black box needs it: T1's coordinator has one link today).
4. The original's stalls and resyncs: not re-created (recommended: inputs only; `--stall-ms` exists for a test that
   wants one).
5. The first real black box (decision 3 of section 4): the user's trial 2 level 8 journals (server + two retail
   clients, a late joiner; recommended), or a core_ref arm's server journal.

As built (T2, 2026-11-05):
- `scripts/journal_recipe.py SERVER_JOURNAL [--client CLIENT_JOURNAL ...] [--map FILE] -o RECIPE.bmrp` (a zip; below): `recipe.json`
  (format `bmmo-input-recipe` 1: level, map {file, md5}, session_first_tick, session_ticks, players [{name,
  join_order, lives [{join_tick, leave_tick}], frames, ticks, source}], source) and `player<N>.frames` ("BMRF" 1:
  runs of u32 tick from the session's first tick, u32 count, u8 keys, u8 nav_active, f32 camera rows [9]). A
  substituted tick takes the client journal's own FRESH frame when given (decision 1); a client journal also adds the
  player's own frames the server never applied (its last ticks after the server removed it). Size: the trial 2 level
  8 session, 16 minutes of two players, is 90 KB (the camera turns in quarter turns).
- The headless client: `--recipe-frames FILE --recipe-first-tick S` (K8's source: a covered tick plays the run's keys
  and camera; past it, the poll's) and `--join-at-ms MS` (`--join-cue`'s pre-booted joiner, cued by the session clock).
  The coordinator: `--latency-ms-for NAME:MS` (decision 3); the harness passes it for a client whose `--link`
  latency differs. `scripts/recipe_run.py RECIPE.bmrp --label L [harness arguments]` (calibrates, below) over `arena_map_test.py --coordinated --recipe RECIPE.bmrp [--recipe-calibration JSON] [--recipe-until-tick T] [--recipe-start-ms MS] [--recipe-first-tick S]`:
  one client per recipe player, level and map from the manifest (the map found by md5), a late joiner at
  `--recipe-start-ms` + its join tick in ms, one session; every life's join, start and leave at the recipe's ticks
  (below, "Lives at the recipe's ticks").
- **Two fixes the first re-drives found:**
  - **K8's nav-active:** an injected tick forced the ball with the SOURCE frame's nav-active and sent the world's:
    in a re-drive the two differ where the new world's navigation is not the original's (trial 2 level 8, tick
    6894: the host's ball and the server's split, 1 cm growing to 1,249 m). Now the world's navigation state before
    the step for both. (A poll-driven run sends the state after the step: a recipe re-drive of a poll-driven
    original therefore differs from it at nav switches - expected; the recipe's own fixpoint is exact, below.)
  - **The join state's savestate carried the server's IVP_Environment address** (bytes 8..15, the in-process restore's
    guard): a late join's journals differed run to run there and only there. `clear_environment_identity` zeroes it
    in the blob that travels (`physics_world`'s resync state); a travelling restore never read it.

Results (P-cores, the server copy without page heap, 150 ms each way):

| run | what | result |
|---|---|---|
| the arena drive (4 clients, no beams) re-driven from its own recipe (before the nav fix) | the original's five journals | 5 of 5 byte for byte |
| a recipe-driven run, its recipe re-extracted and re-driven (after the fix) | the fixpoint | 5 of 5 byte for byte; 7.3x |
| trial 2 level 8 (2 players, a late joiner at tick 1381 of 63,810; retail journals of build 387aac27) | twice | 3 of 3 byte for byte; harness PASS, server replay 65265/65265, the judge exact: own 0/31503 and 0/31512, mirror 0/30781 and 0/30878; 993 virtual s in 59 s (16.8x) |

Checked also: 815/815; x64 build; the wire check of core_ref_s17 ALL SAME, nothing skipped.

The camera's keys (the user, 2026-11-05: "好的，填进去"; the camera turns with Shift + Left/Right): input_frame
has had KEY_SHIFT / KEY_SPACE bits since the design, but no client filled them - trial 2's retail journal never sets
either in 63,951 ticks while its camera takes four directions. Both clients fill them now
(`navigation_graph::camera_bits_from_state`: the game's "Keyboard" array's `rotation` and `camup` keys, row 0 - the
options' keys, 42 / 57 = Shift / Space by default; the array is the level's, read when the navigation is; Shift and Space
only when it is missing. The first version looked for an array named "Keys", which does not exist, and so always used
the defaults: corrected the same day, the user: "应该是读Keys数组，如果没读到才回退默认"). No world consumes them (the leaves drive the ball, the frame's camera rows the camera); a frame
that differs only in them costs a rollback where it was predicted otherwise, like any other input change. A later
journal so carries the player's own camera keys - what a keyboard-level re-drive (keys on the virtual keyboard, the
game's own camera script turning the camera) would need; a journal from before has only the camera rows (the K8 path
above). Checked: 815/815; x64 and x86 builds; a coordinated smoke (2 clients: PASS, the server replay exact, camera
keys 42 / 57 read from the array); the wire check of core_ref_s17 ALL SAME. Not run: retail (whether its Keyboard array holds the player's own keys at the navigation's discovery).
The original's 626 ticks the server stood in for the stalled joiner are played as the server applied them (decision 1).

Lives at the recipe's ticks (the user, 2026-11-05: "玩家离开和复活只报告，不重现？这个要做的。然后中途加入为啥不准看一下"):
- **Why the first re-drives missed:** the server decides a life's ticks from when its messages arrive, and the re-drive
  is not the original. The join came 12 ticks late (1421 against 1409: the Join's time was estimated without the
  150 ms trip); the start 320 ticks early (1471 against 1791: the original's retail joiner spent 5.8 s loading the level
  after it joined, the pre-booted headless one is ready at once); a leave 2 ticks late (a leave the server handles at T
  is its PLAYER record of T + 2). Moving the base itself 333 ticks later broke the joiner: the base's delay is added to
  the joiner's lead (as the J4 clamp's few ticks are), so it ran 333 ticks ahead - resyncs, relayed frames past reach.
- **First fix, withdrawn the same day:** a server test flag (`--test-life`) that held an early join, SessionReady or
  leave until its tick (53c1f87b). It landed every life, but it put test logic in the server; the user: the replay
  sees the whole trace and runs on one virtual clock, so the test side can time the clients exactly - the server is
  restored as it was (09509d3d's server.cpp), the clients are timed instead.
- **The recipe file, `.bmrp`** (the user: one zip file): `recipe.json`, `player<N>.frames` and, once calibrated,
  `calibration.json`. The lives carry `start_tick` (the base the server assigned, from its join-state note; else the
  life's first FRESH frame; a start member's join).
- **The calibration** (scripts/recipe_run.py; the server a black box): its ticks go on a fixed grid of the session
  clock, so a message sent d ms later lands d * 66 / 1000 ticks later. Per client: a late joiner's Join time
  (`--join-at-ms`), each life's SessionReady time (`--ready-at-ms MS`, new, repeatable: the life's level load waits
  until then - the original's player's machine still loading - and its SessionReady goes when it anchors), each
  leave's tick (`--leave-room-at`, or `--rejoin-at TICK:MS` with the next life's Join MS after the Leave; repeatable
  now). A run's misses move those by the ticks missed; a SessionReady that went later than its hold (its life could
  not anchor sooner: a rejoin waits for its old life's removal) moves from when it really went - the client logs
  "--ready-at-ms: life K anchored at X ms of the run" in a recipe run only - so the base follows a tick per tick.
  Without a calibration: `recipe_estimate`'s times, then short passes (each ends a second past the last life's
  event; the coordinator's `--virtual-seconds` through `--recipe-until-tick`) until every life lands, stored in the
  .bmrp, then the full run, checked. With one: one full run, checked; a miss moves the times and runs it again. A
  recipe with every player there from the first tick to the end runs once with nothing to time. The calibration
  belongs to the build and the run's parameters, not to the original: a miss makes it again. The host may not leave
  (its leave hands the room over): refused.
- **Checked:** the rejoin-and-leave recipe (client 1 leaves at 900 and is back 500 ms later, client 2 leaves at 1500):
  first run, no calibration: 3 short passes (about 5 s each), then the full run - every life the original's, (28, 28,
  902), (933, 983, -), (28, 28, 1502); again with the stored calibration: one run, every life landed, its 4 journals
  byte-identical to the first's. Trial 2 level 8: 2 short passes (7.5 s each), then the full run - the joiner (1409,
  1791) as the original's, 16.2x; again: one run, landed, the judge exact on both clients. (A start member's removal
  at the session's end - its client's play time ran out - is the run's end, not a recipe leave.)

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
