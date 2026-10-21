# Running a room without snapshots

The goal: lockstep on the relayed input stream plus local prediction, with
snapshots demoted to a fallback — join, resync, desync recovery — rather than
the thing that keeps the clients in step every other tick. This records how far
that is from working, measured rather than reasoned about, and what the one
remaining blocker is.

## The determinism half already works

`BallanceMMOSimTool --replay-session` rebuilds the world a room ran from its
journal — members, every applied input frame, every lifecycle event, no
snapshots at all — and matches each tick's fingerprint. That is lockstep,
running offline, and it has been bit-exact across Windows x86/x64, Linux and
ARM64. So "can the physics agree?" is settled; the question is only whether the
network can hand every client the same input and event stream.

## The measurement

Mechanism arena, sector 2, four headless clients driving 40 s, server in Arch
WSL, 300 ms round trip, and the snapshot path turned down as far as it goes:

```
physics:
  snapshot_interval: 600
  full_snapshot_interval: 600
```

That is genuinely off: each client received **9–11 snapshots** over ~2 100
ticks, against ~1 100 at the default interval of 2. (`full_snapshot_interval`
had to be made configurable for this — the delta interval never governed it.
The cadence still forces a full on the first tick and on every body-set change,
which is the fallback behaviour working as intended.)

The verdict, from the clients' own per-tick world hashes
(`journal_trace.py --diff`, which for two client journals is the only evidence
there is — neither is authoritative):

```
hash: matched=204/2077  first_divergence=231
```

The journals run ticks 27–2103, and 230 − 27 + 1 = **exactly 204**. So:

> **Every one of the 204 ticks before the balls are born matched bit for bit.
> Zero of the 1 873 ticks after matched.**

The mechanisms-only world is already in perfect lockstep with no snapshots at
all. Ball birth is where it breaks, and it never recovers.

## Why: the body set does not agree tick for tick

The journal's `cores=` field counts bodies. Both clients, identically:

| tick | cores | what is in the world |
|---|---|---|
| ≤ 230 | 10 | the sector's mechanisms |
| **231** | **11** | the mechanisms **and this client's own ball** |
| **252** | **14** | the other three balls appear |

21 ticks = the session's input delay (17 at this latency) plus one trip.

A client physicalizes **its own** ball locally, at the tick its own scripts
create it. It learns of the other three from relayed `Physicalize` events, which
it applies at *its current tick* — a round trip later. So for ticks 231–251
every client is simulating a world with one ball in it, and **which** ball
differs per client. The hashes cannot match, and by tick 252, when all four
balls exist everywhere, each ball has had 21 ticks of physics on its owner and
none anywhere else.

This is not a bug in anything. It is the architecture: body creation happens at
the receiver's current tick, and the receivers are at different ticks. The
snapshot-plus-rollback path is what retroactively repairs it — `rollback.hpp`
has `birth_record` and `repose_tick` for exactly the case of a body born inside
a replayed window. Which is precisely why snapshots cannot be demoted yet: they
are not a safety net here, they are the mechanism.

## Two things ruled out along the way

Worth recording, because both looked like the answer and neither was.

**Not the wire precision.** `session_event_msg` carries the birth pose as
`float[3]` position and `float[9]` rotation, while `body_state` carries every
later pose as `double[3]` + `double[4]`. A newborn ball really is the one body
whose handover is lower precision than everything after it, and it is worth
tidying — but the divergence at the birth tick measures **4.5 cm**, three orders
of magnitude too large for float32 rounding at these coordinates.

**Not the event ordering.** The server applies every due event before its
physics step, sorted by join order, and `session_runner::step` calls that
ordering "part of the determinism contract now" because two balls created in
one tick are created in that order. The headless client was applying events on
packet arrival instead — at an arbitrary point relative to its own step and in
network arrival order. That is a real latent bug and it is now fixed
(`drain_due_events`), but fixing it moved the first divergence by exactly
nothing: still 231, still `matched=204`.

## Driving the rollback from the event instead: built, and not enough

The obvious reading of the above is that lifecycle events need an absolute
`apply_tick` everyone agrees on, so the body sets match. That costs a wire
change and makes a player's own ball appear a round trip after they earned it.
There is a cheaper option that reuses machinery already here, so it was built
first: **let a late event drive the rollback**. The restore-and-replay half of
`on_snapshot` already puts every recorded body back to an earlier tick and
replays forward, and already holds a body born mid-window at its birth pose and
releases it at its birth tick (`birth_record` / `repose_tick`). The only thing
missing was a way in that did not need a snapshot to disagree first.

`rollback_engine::rewind_to()` is that way in, and the client calls it when a
remote `Physicalize` arrives late: anchor at `stamp - 1`, replay forward with
the ball present, so the body set matches the owner's tick for tick with no
protocol change and no delay on anyone's own ball. It works mechanically — the
clients report `births=6/0`, six rewinds and none out of history.

**It did not fix the divergence.** Nor did two other things that looked like
the cause:

| tried | result |
|---|---|
| align the client's event application with the server (before the step, join order) | first divergence unchanged: still 231, still `matched=204` |
| rewind on a late birth (`rewind_to`) | fires 6/0, first divergence unchanged |
| report the owner's actual pre-step birth pose instead of the canonical resetpoint | mismatch count unchanged, `max_err` worse (2955 m vs 48 m) — reverted |

And the instrument has a floor worth knowing about: a client journal records
each tick's hash **as the live frame completes it**, so a birth tick is always
recorded before any later rewind re-simulates it. `first_divergence=231` is
therefore partly an artefact of when the hash is sampled, not only of the final
world. Client-vs-client hash comparison cannot validate a fix that works by
re-simulating the past.

So the audit was changed to let one full snapshot per second through
(`snapshot_interval 600`, `full_snapshot_interval 66`) — deltas still off, but
~37 chances for the server to check each client over 2 200 ticks. Of those,
**11 to 41 disagreed**, with errors of tens of metres, and no amount of the
above moved it.

The conclusion that matters: **the snapshot path is not covering rare events,
it is doing continuous corrective work every couple of ticks.** Demoting it to
a fallback is not one fix away.

## What is actually needed

Lifecycle events have to be applied at an **absolute tick every participant
agrees on**, instead of at each receiver's current tick. The shape:

* the server picks `apply_tick = max(stamp, now) + input_delay + margin` and
  relays it as a field of its own, distinct from the reported stamp (the spawn
  impulse direction derives from the stamp — `spawn_direction_index` — and both
  sides must keep deriving it from the same number);
* every participant applies the event at `apply_tick`, before that tick's step;
* **including the owner**, which means a player's own ball appears about a round
  trip after they earned it.

That last point is the real cost, and it is the same trade as the per-session
input delay: the consistency is bought with latency, and the latency is borne
where it belongs. A third of a second before your ball appears is noticeable;
whether that is acceptable is a product decision, not a technical one.

The server already has the horizon concept — `waiting_for_lifecycle` gives a
pending Physicalize until `tick + input_delay + 1` to show up — so the numbers
are not new, only their authority is.

## What was changed to get here

* The relayed input stream is now **reliable**. It was `UnreliableNoDelay`, one
  tick per message, no redundancy, so it survived zero packet loss; a snapshot
  every other tick was hiding that. A redundancy window was the wrong fix: the
  relay is already O(N²) in the room (N clients × N−1 frames × 66/s, 39 bytes a
  frame, so 83 KB/s per client at 30 players) and a window multiplies it.
* The headless client no longer drops a reordered input frame before recording
  it for the rollback, and no longer applies lifecycle events out of order.
* `physics.full_snapshot_interval` is configurable.
* The harness's latency proxy can drop datagrams (`--loss-pct`), with
  accounting — loopback does not drop, so an unreliable path passes a loss test
  by accident.

## Reproducing

```bash
# server config: snapshot_interval 600, full_snapshot_interval 600
python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \
    --root <game root> --level 13 --sectors 2 --players 4 --seconds 40 \
    --drive --beam-bays 9,9,9,9 --beam-spread 3 --latency-ms 150 \
    --checkpoint-ticks 2 --label snapoff

# the only evidence that counts: the clients' own per-tick hashes
python scripts/journal_trace.py maps/arena-test/snapoff/sector2/client0.bmjr \
                               maps/arena-test/snapoff/sector2/client1.bmjr --diff
```

## The missing rollback trigger, and where that got us

The three attempts above all addressed *birth*. The thing actually missing was
the rollback trigger lockstep runs on, and it had nothing to do with births.

A remote ball is predicted forward with its last known input frame. The frame
the server actually applied for it arrives `input_delay` + a trip later - ~21
ticks at a 300 ms round trip. The client stored that frame for a future replay
and updated its live predictor, **but nothing ever replayed the ticks it had
already simulated from the wrong input.** The only thing that re-simulated them
was an authoritative snapshot disagreeing. That is why turning the snapshot path
off left the clients tens of metres out: the correction was only ever arriving
through the mechanism we were trying to demote.

So each relayed frame is now compared against the one the engine recorded as
applied at that tick (`rollback_engine::recorded_input`). Equal means the
prediction was right, which is the common case because a held key predicts
perfectly. Different means that tick and every tick after it ran on the wrong
input, so `rewind_to` replays from there - coalesced to one rewind per frame at
the earliest corrected tick, since several remotes' frames arrive together.
`Unphysicalize` gained the same rewind as `Physicalize`, since a ball that died
or trafo'd at its owner's tick T was being removed ~21 ticks late everywhere
else.

Measured, deltas off and one full snapshot a second as the server's audit:

| | audits disagreeing, per client | resim ticks |
|---|---|---|
| before | 11 / 18 / 18 / 36 | 228 / 371 / 372 / 743 |
| input-driven rollback | **5 / 9 / 9 / 29** | 740 / 4317 / 4317 / 4263 |

The disagreement roughly halved, at a cost of ~1.9 extra physics steps per tick
- which is simply what lockstep costs at this latency. The rewinds fire 81-158
times per client with **none** falling outside the rollback history
(`inrew=.../0`), so the 96-tick window is comfortably deep enough at 300 ms.

Two notes on reading those numbers. `rewind_to` must not bump
`stats_.mismatched` - that counter means "snapshots this client disagreed with"
and is the measure of how far it is from the server, so a rewind the caller
asked for polluted it and made the first run of this look three times worse
than it was. And the server's audit is the right frame for it: under a design
where the server simulates to **catch cheating** rather than to keep clients in
step, an honest client's disagreement count is the false-positive rate, and it
has to reach zero for a different reason than synchronisation.

## Where it actually stands: the mechanisms are in lockstep

With both journals checkpointing the same ticks (below), restricted to the
map's own bodies - the ones whose names mean the same thing on every side -
**exactly one body ever diverges**:

| comparison | first divergence |
|---|---|
| client vs client | tick 292-668, `P_Ball_Paper_MF`, 0.07-0.10 m |
| client vs server | tick 274, `P_Ball_Paper_MF`, 0.06 m |
| all 13 mechanisms | agree within 5 cm across ~1 100 checkpoints |

So the script-driven mechanisms *are* in lockstep, client to client and client
to server, with the snapshot path essentially off. The one body that is not is
the loose prop ball: a free body with no input of its own, so the input-driven
rollback has nothing to correct it with directly, and it is hit by the player
balls - which are exactly what still disagrees during the ~21 ticks between a
ball's birth and the rest of the field hearing about it. It amplifies the
residual rather than causing it.

## Comparing the two sides at all took fixing

Worth recording because it wasted two cycles.

A client journal **does** carry its own world body by body
(`JOURNAL_CHECKPOINT_LOCAL`, every movable body with name, pose and
velocities). An earlier pass through this concluded it was hash-only, because
the first two checkpoints in the file are at a tick before any movable body
exists. Check more than the first record.

Its cadence was the hardcoded `kJournalCheckpointTicks = 660`, one checkpoint
every ten seconds: a black box, not a trace. It is now `--checkpoint-ticks`,
which the harness passes alongside the server's `journal_checkpoint_ticks`.

And the first dense run still compared nothing at all - 1 055 client and 1 320
server checkpoints with **zero ticks in common** - because the client's rule was
"every N ticks since the last one", which drifts onto the opposite parity from
the server's `tick % N == 0` and then never coincides. The client now uses the
server's rule.

Finally, names are not a cross-side identity for player balls. A player's own
ball is `Ball_<type>` on its own client whatever its id, so the same name means
a different physical ball on each client, and the server names them all by id.
`scripts/first_divergence.py --prefix P_` restricts the comparison to the map's
own bodies for that reason; comparing balls needs an id mapping that is not
written yet.

## The next step is an instrument, not another fix

Three hypotheses were tested one at a time against a measurement that takes a
build and a 40 s run, and all three came back "still diverging". That is the
signature of either several simultaneous causes or one that has not been
isolated, and guessing again is the wrong move.

That instrument is now `scripts/first_divergence.py`, and building it turned
this from guessing into an answer in one command. What it needs next is the
ball identity mapping, so the player balls can be compared too - right now the
only bodies that can be are the map's own.

## What converges, what does not, and the one structural blocker

With the input-driven rollback in and the comparison able to see every body,
the picture is finally clear, and it splits cleanly in two.

**The balls converge.** Traced against the server tick by tick, a mirror
appears ~0.5-2.5 m out (which is just the ~21 ticks of its input that have not
arrived yet), diverges for a few dozen ticks, and then comes back: by the end
of the session **every ball agrees with the server to 0.00 m**. That is lockstep
working. A ball's state is a function of the shared inputs, so replaying with
the inputs re-derives it exactly, and it does.

**The mechanisms do not stay converged.** A plate of `P_Modul_29` agrees
*exactly* - 0.000 m - for the first ~700 ticks, then creeps: 0.12 m at tick
736, 9.9 m by the end on the worst client. The errors ramp along the row
(Platte01 1.06 m, 02 2.65, 03 5.13, 04 7.29, 05 9.81, 06 11.74) rather than
jumping, and no time shift aligns the two traces, so it is not a phase offset
and not a joint rebuild - there is no death or reset in these runs at all.

**The blocker is in `rollback.hpp`'s own description of itself:** the replay
re-simulates *"physics + navigation only, no scripts"*. A ball is navigation; a
mechanism is a script. So a rollback restores a mechanism from our own record
and then steps it without the script that drives it, and each rollback perturbs
it a little. Nothing re-derives it, because unlike a ball it has no input to be
re-derived from - only an authoritative row can repair it, which is exactly the
snapshot path being demoted.

The measurement that pins it: the perturbation scales with the rollback count.

| | rollbacks per client | resim ticks | audits disagreeing |
|---|---|---|---|
| 300 ms round trip | 109-185 | 1 522-4 850 | 6 / 10 / 9 / **31** |
| no latency | 16-21 | 23-54 | 7 / 6 / 7 / **8** |

The worst client's disagreements fall from 31 to 8 and the spread across clients
vanishes, purely by removing the rollbacks. Its 3x penalty was never about
joining last - it was that it rolled back the most.

So: **lockstep recovers a body whose evolution is a function of the shared
inputs, and cannot recover one whose evolution depends on a history that has
already been perturbed.**

### It is not the rollback that perturbs them

That was the obvious next hypothesis and it is wrong. Keeping the mechanisms out
of the rollback entirely - never restoring or replaying them, letting their own
deterministic script timeline carry them - changes their agreement with the
server by **nothing**:

| mechanisms only (`P_Modul*`), mean worst-body error | client0 | client3 |
|---|---|---|
| 300 ms, mechanisms in the rollback | 0.16 m | **7.66 m** |
| 300 ms, mechanisms out of the rollback | 0.15 m | **7.29 m** |
| no latency, in the rollback | 0.62 m | 0.86 m |

Two things to read off that. The rollback is neutral for mechanisms, so the
"replay does not re-run the scripts" problem is real but not what is hurting
them here. And the damage is **not uniform across clients at the same
latency**: client0 at a 300 ms round trip is fine at 0.16 m while client3 is at
7.66 m. The difference is which balls actually reach the plate row - client3's
does, during the window when its view of the other balls is still wrong, and a
ball that displaces a plate while it is itself in the wrong place displaces it
permanently, because the plate has no input to be re-derived from.

So it comes back to the transient after all, and the measurement says what to
aim at: **a mirror must not be able to touch anything while its inputs have not
caught up.** It does not have to be in the right place - it is a remote player
at 300 ms - it only has to be unable to do irreversible damage. Creating the
mirror with collision disabled and enabling it once its input history has
caught up would cost nothing in protocol or in anyone's own latency, which is
the next thing to try.

(That measurement also cost a lesson: `mism` dropped from 31 to 6 when the
mechanisms left the rollback, which looked like a win and was not - the engine
simply stops *evaluating* a body it does not track, so the counter improved
while the world got slightly worse. The second time in this session that a
change quietly removed the metric being used to judge it.)

## The replay, measured in isolation: faithful for balls, not for mechanisms

Everything above was measured through the network, where latency, birth windows
and four clients all move at once. `--force-rollback <every>:<depth>` takes all
of that away: it rewinds on a fixed cadence with no network reason to, so in a
**solo** session - where the world is a pure function of one recorded input
track - a run with forced rollbacks must produce the same per-tick hashes as a
run without. Anything that differs is state the replay failed to restore, and
nothing else.

It differs immediately. Forty rewinds of depth 12, solo:

```
hash: matched=33/1632 first_divergence=41
```

Tick 40 is the first forced rewind. Everything before it matches, nothing after
- one depth-12 replay permanently changes the world.

Split by body kind, against the same solo run without the rewinds:

| | first difference | mean | max |
|---|---|---|---|
| the ball | tick 242 | **0.0017 m** | **0.0050 m** |
| the mechanisms (`P_Modul*`) | tick 42 | **23.3 m** | 310 m |

**The replay is faithful for a navigated body to within 5 mm, and
catastrophically unfaithful for a script-driven one on the very first rewind.**
That is the whole blocker, in one measurement, with no network involved.

The mechanism is mechanical and visible in the numbers: the replay's `step()`
steps the **whole** physics world, and a mechanism's script does not run during
a replay, so the plates get `depth` steps with nothing holding them and simply
fall - 2.1 m on the first rewind, peaking at 2.4 m, then decaying as the live
script pulls them back. It also explains why taking them out of the tracked set
changed nothing: not being *restored* does not stop them being *stepped*.

### Four ways to treat them in a replay, and the current one is the best

All measured the same way - solo, 40 forced rewinds of depth 12, mean error
against the same run without them:

| what the replay does with the mechanisms | ball | mechanisms |
|---|---|---|
| **steps them, as it does today** | 0.0017 m | **23.3 m** |
| freezes them for the replay, wakes them after | 0.0009 m | 48.0 m |
| captures them before and writes them back after | - | 47.1 m |
| runs the whole frame so the scripts drive them | 24.0 m | 103.3 m |

Every intervention is worse than doing nothing, and the reasons differ:

* **Freezing** them means the replayed ball meets plates at the pose they
  really held at the anchor, which is why it is the best row for the ball - but
  the freeze/wake cycle perturbs IVP's own freeze timers (a known gap, see the
  mechanism Option A postmortem) and the mechanisms end up twice as far out.
* **Writing them back** puts a plate at its live pose in a world whose balls
  are at replayed poses, so a plate can land inside a ball and the contact
  recheck launches it.
* **Running the whole frame** re-runs scripts for ticks they already ran, so
  one-shot actions fire twice; the behaviour clock advances by the replay's
  depth on top of the live frames; and the retail navigation script fights the
  bridge's own `nav_input` for the ball, which is why it ruins the one body
  kind that was already faithful.

So mechanism replay fidelity cannot be bought from outside the engine. The
first and third of those are kept as flags (`--force-rollback`,
`--replay-scripts`) with their numbers in the comments so the experiments are
repeatable; the other two are reverted rather than left as dead config.

### So the remaining work is exactly: run the scripts during the replay

What that needs, in the order the obstacles bite:

1. **Behaviour state at the anchor tick.** A progression block mid-animation is
   at its live state, not its tick-T state, so replaying from it produces the
   wrong motion. `CKSaveObjectState` on a `CKBehavior` returns only the base
   chunk unless `CK_STATESAVE_BEHAVIORONLY` - the full graph serialization is
   file-oriented - so this needs either that flag to be made to work for a
   round trip, or the specific driving blocks' state captured by hand. Finding
   *which* blocks drive `P_Modul_29` is the next concrete step
   (`--debug-frames` on the SimTool gives a block-level execution trace).
2. **The behaviour clock.** `headless_engine::tick()` is `context_->Process()`,
   which advances `CKTimeManager`. Replaying N ticks advances it N further, so
   anything reading absolute time sees the future. The fixed delta means the
   per-tick *deltas* stay right, which may be enough - worth checking before
   building anything.
3. **Suppressing the frame's other side effects** during a replay: the level
   request advance, `drain_player_commands`, the Random-block re-patch.

None of that is reachable from the rollback engine alone; it is an engine
change, which is what the measurements above were for. They say three things
worth keeping separate:

* **the balls do not need it** - they are already in lockstep, 0.00 m against
  the server over a network and under 2 mm under forced rewinds;
* **nothing short of it helps** - all four cheap treatments were measured and
  the status quo won;
* **and it must be selective.** Running the frame wholesale is not a smaller
  version of the right fix, it is a different and worse thing: the mechanism
  behaviours have to be executed on their own, with their state rewound, while
  the navigation scripts and the one-shot actions stay out of it.

## The bug that was hiding behind all of it: stepped but never restored

The rollback's tracked set was built from `mechanism_tracking_::tracked_entities`
- the bodies the server has named in a full snapshot **and** the registry has
resolved to a local body. Everything else in the client's own world was left
out of the records, so a replay never restored it. But the replay's `step()`
steps the **whole** physics world, so those bodies were stepped by every replay
and restored by none.

The signature is unmistakable once you look for it. `P_Modul_29`'s plates hang
from physics hinges - the script is `Physicalize` + `Set Physics Hinge` x10 +
`TT Scaleable Proximity`, so IVP carries them rather than a per-frame script -
and they swing continuously. Coming out of a solo depth-12 forced rewind, a
plate sat at **exactly** the pose the un-rewound run held 12 ticks *later*: not
displaced, phase-shifted by precisely the replay depth, because it took 12
extra steps that nothing undid.

And `mechanisms=22/12` had been in the status line of every run all session.
Ten of the client's own bodies, in that state.

The tracked set is now every movable body the bridge reports, minus the balls
which it already carries as own/remote entities. Solo, 40 forced rewinds of
depth 12:

| | before | after |
|---|---|---|
| mechanisms, mean | 23.34 m | **3.28 m** |
| mechanisms, p90 | 73.9 m | 15.5 m |
| mechanisms, max | 310 m | **20.4 m** |
| ball, mean | 0.0017 m | 0.0012 m |

Seven times better, and the ball improves too. In the networked single-sector
runs it changes little - 7.67 m stays 7.67 m on the worst client - because
there only *one* body was unresolved rather than ten. The win scales with how
much of the world the server had not named, which depends on the map and the
sector.

### It also invalidated one of the earlier conclusions

The "taking the mechanisms out of the rollback changes nothing, so the rollback
is not what perturbs them" result above is **wrong**, and wrong for this exact
reason: removing a body from the tracked set never stopped it being stepped, it
only stopped it being restored. Both arms of that experiment were measuring a
body nothing restored.

Re-measured properly, with the whole world tracked, the rollback count clearly
does drive it:

| mechanisms vs the server, mean worst-body error | client0 | client3 | rewinds |
|---|---|---|---|
| no latency | 0.069 m | 0.398 m | 17-22 |
| 300 ms round trip | 0.158 m | 7.67 m | 109-185 |

So each rollback still perturbs a mechanism even when it is properly restored
and replayed, and that residual is now the whole remaining problem.

### Which points at the hinges, not the scripts

The plates are not position-animated by a script; they hang from ten physics
hinges and IVP carries them. The restore puts each body back by pose and
velocity - and nothing puts back the **constraint** state. A chain of ten
hinged bodies restored one at a time can land in a configuration the hinges
disagree with, and the solver's correction is not gentle.

That reframes the engine change: it is not "make the Virtools behaviours
replayable", which the whole-frame experiment showed is the wrong shape anyway.

### Two candidates ruled out by reading, and the one that is left

**Not the hinge constraints.** `hk_Hinge_Constraint` holds the hinge axes
(`m_axis_os`, `m_axis_perp_os`) and static parameters (`m_tau`, `m_strength`,
`m_limit`), and its per-step working memory is handed in as scratch -
`get_vmq_storage_size()` / `setup_and_step_constraint(pi, void* mem, ...)`.
There are no accumulated impulses and no warm-start cache, so a hinge is solved
afresh each PSI from the bodies' poses and velocities. Restoring pose and
velocity is enough for it.

**Not the leapfrog delta either**, though it was the prime suspect from the
Option A postmortem's list of IVP restore gaps. `write_body_state` has a
`restore_psi_state` path that takes the per-PSI rotation increment across the
beam, rewrites `delta_world_f_core_psis` from the installed velocity, and moves
`pos_world_f_core_last_psi` back by the interpolation fraction - its comment
says, and means, "so a restore is idempotent whatever the phase". `set_body_state`,
which is what the rollback calls, passes it `true`.

**What is left is the queued impulses.** `bmmo_physics_body_state` carries
position, rotation, `linear` (`core->speed`) and `angular` (`core->rot_speed`)
- and not `core->speed_change` / `rot_speed_change`, the impulses IVP has
queued for the next PSI. So the capture never records them, and
`write_body_state` *zeroes* them on every write, deliberately: "A hard set
replaces the motion, so pushes queued for the next PSI must not survive it
(design 8.4)".

Every restore therefore drops whatever was queued at the anchor tick and
discards whatever is queued now. For a navigated ball that costs almost
nothing, because the next step re-applies its navigation force - which is
exactly why the ball replays to 1.2 mm. For a plate in a hinged chain under
contact, the queued impulse *is* its motion and nothing re-applies it.

**And that hypothesis is false.** Measured before touching the ABI, with a
`--trace` diagnostic that counts bodies carrying a queued impulse at the moment
the rollback captures state (`report_queued_impulses`): it is **zero for
essentially every body** - 0 of 10 to 14 at almost every sample over a
1700-tick solo session, with only `P_Modul_25_Bridge` occasionally holding a
constant |dv| of 1.2121. There is nothing meaningful being dropped, so growing
the captured state would buy nothing and the bridge ABI stays as it is.

Worth recording how that went wrong: the hypothesis was inferred from two true
facts - the capture omits `speed_change`, and `write_body_state` zeroes it -
without ever measuring the quantity itself. The premise was reachable with a
ten-line diagnostic and an existing dump function (`describe_cores_exact`,
whose `dv=` field is exactly this). Measure the premise before proposing a
change to a shared struct.

So the residual is still unexplained. What is left to look at, in rough order of
how likely they are to carry per-step state that a pose+velocity restore misses
while barely affecting a force-driven free body: the contact point caches and
mindist structures, the sleeping/freeze counters, `hk_Local_Constraint_System`
as a system rather than the individual constraints, and IVP'''s controllers.

### BallanceTAS has nothing to lend here

Checked, since it is the other project that replays Ballance deterministically:
it is behind this one. It says outright that IVP physics determinism is **not**
enforced, has no savestates, and its fixed-timestep layer was removed. The one
transferable idea - validating frame by frame against a reference trace -
already exists here as the journal replay and is stronger, comparing a full
world hash rather than the ball. Useful mainly as confirmation that nobody has
a Virtools savestate to borrow.

## The cause, found and verified: the restore deletes IVP's queued impulses

`write_body_state` ends with this, and has since design 8.4:

    // A hard set replaces the motion, so pushes queued for the next PSI
    // must not survive it (design 8.4).
    core->speed_change.set_to_zero();
    core->rot_speed_change.set_to_zero();

A rollback restore goes through the same function, so **every rewind deletes
whatever IVP had queued for the next PSI** on every body it touches. That is the
divergence.

### The probe: a rewind of depth 0

`--force-rollback <every>:0` restores the record for the **current** tick over a
world that already holds exactly that state, and replays nothing: `lag` is 0, so
the re-simulation loop runs zero times. A pure capture-and-restore round trip,
which a faithful restore would make a no-op. It is not one - and because the
live core *is* the anchor core at depth 0, "do not zero the queue" is there
exactly equivalent to "restore the queue". So the zeroing can be tested on its
own, with nothing added to any record and no ABI touched
(`--keep-queued-impulses`).

Solo, one forced rewind at tick 40, worst error per body over ticks <= 48, with
a control run twice to establish the floor:

| body | floor | zeroing (today) | keeping |
|---|---|---|---|
| `P_Modul_29_Platte08` | 0 | 3.62e-02 m | **1.81e-08 m** |
| `P_Modul_29_Platte09` | 0 | 3.54e-02 m | 1.72e-08 m |
| `P_Modul_29_Platte05` | 0 | 2.97e-02 m | 5.06e-08 m |
| `P_Modul_29_Platte04` | 0 | 2.95e-02 m | 2.88e-08 m |
| `P_Modul_29_Platte07` | 0 | 2.62e-02 m | 4.34e-08 m |
| `P_Modul_29_Platte06` | 0 | 2.20e-02 m | 3.34e-08 m |
| `P_Modul_29_Platte03` | 0 | 1.86e-02 m | 7.59e-08 m |
| `P_Modul_29_Platte02` | 0 | 1.63e-02 m | 1.59e-07 m |
| `P_Ball_Paper_MF` | 0 | 9.95e-03 m | 4.59e-08 m |
| `P_Modul_29_Platte01` | 0 | 7.88e-03 m | 1.30e-07 m |
| `P_Modul_30_Wippe`, `P_Modul_25_Bridge`, `P_Box_MF`, `P_Ball_Stone_MF` | 0 | 0 | 0 |

**Six orders of magnitude**, on every affected body, and the unaffected bodies
stay at exactly zero. The player ball is bit-identical (0.0000 m) in every arm.

### Why the plates and not the ball

IVP's impact solver runs in mindist events *between* PSIs, and `delay_decision`
parks the impulse on the core with the **larger `virt_mass`** - which, when a
ball hits a plate, is the plate. The ball's push is applied at once; the plate's
is queued, and the queue is what the restore deletes. A navigated ball also has
its force re-applied by its navigation on the very next step, so even a lost
push costs it almost nothing; a hinged plate under contact has nothing to
re-apply it.

The field trace shows only **one** body carrying a queue at any given rewind
(`P_Modul_29_Platte09`, `|dv|` = 1.212 m/s). All ten plates move anyway because
they are one hinge-coupled chain: perturb one and the rest answer within a few
ticks. And 1.212 m/s over a 15.15 ms tick is 1.8 cm, which is the order the
table shows.

### What the record still has to carry

At depth 0 "keep the live queue" and "restore the anchor's queue" are the same
thing. At any real depth they are not, and the measurement says so - the same
rewind at depth 1:

| body | zeroing | keeping the *live* queue |
|---|---|---|
| `P_Modul_29_Platte08` | 3.81e-02 m | 1.05e-03 m |
| `P_Modul_29_Platte09` | 3.47e-02 m | 9.41e-04 m |
| `P_Modul_29_Platte03` | 1.97e-02 m | 3.82e-03 m |

30x rather than 10^6. The obvious reading - one tick of staleness costs five
orders - turned out to be **wrong**, and the fix below says so: restoring the
*anchor's* queue gives bit-identical numbers to keeping the live one, because
the queue is near-constant from tick to tick. What is left at depth 1 is a
different, depth-scaling term. See "What is left" below.

The fix is still to **record `speed_change`/`rot_speed_change` at the anchor and
restore them**, with the zeroing kept for a genuine hard set, which is what
design 8.4 was actually about - the two operations are wearing one function.

That record is a **local savestate**, which is what GGPO's
`save_game_state`/`load_game_state` always were. It never goes on the wire, so
it is not a snapshot; and it need not go in `bmmo_physics_body_state`, the
struct the retail mod shares - a second, local struct keeps that ABI untouched.

## The fix, as built (bridge API v11)

`bmmo_physics_core_extra` is a **new** struct carrying `speed_change`,
`rot_speed_change` and both PSI quaternions, with a `fields` mask saying which
of them a caller means; `bmmo_physics_body_state` is byte-identical to before,
because it is what the wire and the renderer need and the retail mod shares it.
Two v11 entries, appended to the v-table, read and write the queue
(`get_core_extra` / `set_core_extra`), and `rollback.hpp` carries the values in
a map parallel to `tick_state::bodies`.

Three things make it correct rather than just effective:

* **`set_body_state` still zeroes the queue.** A replay restore writes the
  recorded queue back immediately *after* the pose write. That is sound because
  nothing in `write_body_state` after the zeroing touches the queue again, and
  neither `ensure_in_simulation` nor `disable_simulation` does either - checked
  against IVP, whose only clearers are `set_pinned`,
  `abort_all_async_pushes`, `commit_all_async_pushes` (the PSI, which is what we
  want to happen next) and `stop_movement_without_collision_recheck`. The
  composition measures bit-identically to never zeroing at all.
* **Only a body restored from our own record gets its queue back.** A body
  restored from an authoritative snapshot row does not: a row is a pose the
  server reported, it carries no queue, and the queue we happen to hold locally
  belongs to the state being replaced. *That* write is the hard set design 8.4
  is about. The `from_row` flag already encoded the distinction. A body born
  inside the replayed window is parked rather than restored, so it gets none
  either.
* **The A/B lives in one binary.** `--no-restore-impulses` reproduces the
  pre-v11 restore, because two builds' plain runs disagree by more than this
  effect is worth (see the methodology section).

### Measured

Solo, one forced rewind every 40 ticks, worst error per body over the window,
control run twice in the same batch for the floor:

| depth | window | floor | `--no-restore-impulses` | restored | |
|---|---|---|---|---|---|
| 0 | <= 48 | 0 | 3.62e-02 m | **1.81e-08 m** | 10^6 |
| 1 | <= 48 | 0 | 3.81e-02 m | 1.05e-03 m | 36x |
| 12 | <= 60 | 0 | 1.37e-01 m | 7.39e-02 m | ~2x |

(worst plate in each arm; the ball is 0.0000 m throughout, and
`P_Modul_30_Wippe`, `P_Modul_25_Bridge` are exactly 0 in every arm)

At depth 0 the restore is now essentially exact - 1.8e-08 m is the quaternion
floor below, not the queue. The win shrinks with depth because a second term
takes over.

### Both quaternions as well, which makes the restore exact

The same local struct also carries `q_world_f_core_last_psi` and
`q_world_f_core_next_psi`, under its own bit of the field mask
(`BMMO_PHYSICS_EXTRA_QUATS`, switched by `--no-restore-quats`). The reason is
the floor above: `read_body_state` captures **one** quaternion, so
`write_body_state` re-derives the other from the live rotation step through
`fast_normize_quat`. Recording both removes the derivation.

It was added on a hypothesis that the measurement then **refuted**: that the
derivation uses the *live* rotation step rather than the anchor's, and so would
explain the depth-scaling residual. It does not. All four mask combinations at
depth 1, same batch:

| worst plate at depth 1 | nothing | impulses | quaternions | both |
|---|---|---|---|---|
| `P_Modul_29_Platte08` | 3.811e-02 | 1.046e-03 | 3.811e-02 | 1.046e-03 |
| `P_Modul_29_Platte05` | 2.955e-02 | 1.464e-03 | 2.955e-02 | 1.464e-03 |

The quaternion column is **identical to doing nothing**, and `both` is identical
to `impulses`. The quaternion half buys exactly zero at depth 1, and the same at
depth 12.

What it does buy is at depth 0, and it is worth having:

| worst body, depth 0 | impulses only | both |
|---|---|---|
| `P_Modul_29_Platte02` | 1.589e-07 m | **0** |
| `P_Modul_29_Platte01` | 1.300e-07 m | **0** |
| every other body | 1.7e-08 .. 7.6e-08 m | **0** |

Exactly zero, on every body. And by the pose hash, which is stricter than the
per-body comparison:

| | matched | first divergence |
|---|---|---|
| two plain runs (the floor) | 80 | tick 88 |
| depth 0, pre-v11 restore | 33 | tick 41 |
| depth 0, impulses only | 33 | tick 41 |
| **depth 0, both** | **80** | **tick 88** |

A depth-0 forced rollback is now indistinguishable from the run-to-run floor:
the divergence moves off the first rewind (tick 40) and onto tick 88, which is
where two *plain* runs part company anyway. **The capture-and-restore round trip
is bit-exact to the limit of what the harness can measure.**

### So the restore is solved, and what is left is the replay

| worst plate | depth 0 | depth 1 | depth 12 |
|---|---|---|---|
| pre-v11 | 3.62e-02 m | 3.81e-02 m | 1.37e-01 m |
| + queued impulses | 1.81e-08 m | 1.05e-03 m | 7.39e-02 m |
| + both quaternions | **0** | 1.05e-03 m | 7.40e-02 m |

This inverts where the problem now lives, and it is worth being precise about
the order of the two claims, because they look contradictory and are not:

* **Before the fix**, a depth-0 rewind diverged, which proved the restore was
  lossy and that the replay-treatment experiments were aimed at the wrong half.
* **After it**, a depth-0 rewind is at the floor, so the restore is no longer a
  source of error at all - and every remaining metre is injected by the replayed
  steps.

The residual is not the queue (restored) and not the rotation (restored, and
measured to change nothing at depth >= 1). Two bodies point at where it is:
`P_Box_MF` (2.978e-04 m) and `P_Ball_Stone_MF` (7.498e-05 m) are **exactly 0 at
depth 0** and unchanged by either half of the fix at depth 1 - bodies whose only
error is the replay's. The next question is what a physics-only replayed step
does that a live frame's step does not, now that it is the only variable left.

### The floor underneath: the quaternion round trip

What survives at depth 0 with the queue kept - 1.8e-08 to 1.6e-07 m - is a
second, much smaller loss, and the field trace names it. `read_body_state`
captures **one** quaternion, `calc_at_quaternion` at the current time, the pose
the renderer sees; IVP's state is **two**, `q_..._last_psi` and
`q_..._next_psi`. So `write_body_state` re-derives the second, taking the PSI
rotation step off the **live** core and carrying it through
`fast_normize_quat()`. Over eight depth-0 rewinds that moved 10-11 cores each:

| field | times changed | size |
|---|---|---|
| `q_world_f_core_last_psi` | 84 / 84 | ~2e-10 rad |
| `q_world_f_core_next_psi` | 84 / 84 | ~2e-10 rad |
| `pos_world_f_core_last_psi` | 3 | 1 ULP |
| `speed_change` | 1 | 1.212 m/s |

Never `speed`, `rot_speed`, `delta_world_f_core_psis`, `i_delta_time` or
`time_of_last_psi`. Two e-10 radians is the bit-exactness floor, not the
divergence; it matters only once the queued impulses are fixed, and the same
local savestate fixes it, by holding both quaternions instead of re-deriving
one.

## Four things ruled out by measurement, not argument

* **The replay is not the problem.** Depth 0 replays nothing and diverges
  anyway. Every one of the four "how should a replay treat a mechanism"
  experiments above was aimed at the wrong half.
* **Not the contact recheck.** `restore_and_replay` passes
  `recheck = write_moves_body(...)`, true for every body a real rollback moves
  and no body at depth 0, so it looked like the discriminator. It is not:
  with `--no-restore-recheck`, a depth-1 rewind's per-body error is identical to
  four significant figures on every body. (An earlier draft of this document
  claimed a 325x effect here. That was an artefact of comparing an arm against a
  control from a *different run batch*; see the floor note below.)
* **Not the delta-time filter.** `CKIpionManager::SetDeltaTime` is a stateful
  exponential moving average, `m_DeltaTime = (m_DeltaTime * 3 + delta) / 4`, and
  `Simulate()` calls it on every call including every replayed step - so a
  replay really does mutate integration state nothing restores. Measured in
  exact hex across rewinds, `m_PhysicsDeltaTime` is **UNCHANGED** at both depth 0
  and depth 1: with a fixed `kFixedDeltaMs` the filter sits on its float fixed
  point, where `(d * 3 + d) / 4 == d`. Worth re-checking the day the delta stops
  being fixed.
* **Not the IVP clock, as an instrument bug.** The world hash does feed
  `current_time` and the rollback never rewinds the IVP clock, so the `hash`
  field genuinely cannot match again after a rewind. But the `pose` hash, which
  feeds neither the environment nor `time_of_last_psi`, diverges at exactly the
  same tick, so the measurements stand as made.

## The environment clock: restored, measured null, left opt-in

With the restore bit-exact, `--trace-restore` at depth 1 brackets one replayed
step: "before" is live tick N from a full frame, "after" is the same tick from
an exact restore to N-1 plus one physics-only step. The one field that differed
on **every** core (85 of 85) was `time_of_last_psi`, by exactly one tick - the
rollback restores bodies but never the environment's clock, so every replayed
step runs one tick late in IVP time, and every rollback leaves the clock
permanently ahead by its depth.

`bmmo_physics_clock_state` records the environment's three times, the time
manager's `base_time`/`last_time` and `next_movement_check`; the restore puts
them back **before** the body writes (the beam stamps each core with the
environment's `time_of_last_psi`), shifts every queued event so it keeps its
absolute time, and repositions the PSI event at the anchor's `time_of_next_psi`.
All of it from outside IVP, through public members and the existing access shim.
It works as a mechanism - `tlast` differences fell from 85 to 4 - and the body
error at depth 1 did not move: 3.823e-03 -> 3.815e-03 m on the worst plate,
pose hash still diverging at tick 41. The `st`/`idt` signals it exposed turned
out to be artefacts of the rechecking beam with no effect on the outcome
(recheck on/off identical to four significant figures, again).

It stays **opt-in** (`--restore-clock`): no measured benefit, and IVP's hull
manager keeps time-anchored state (`last_vpsi_time`) that a rewound clock turns
into negative deltas. One null dereference in the mindist minimizer appeared in
four clock-on networked runs against none in eight without; not proven, not
worth the risk for nothing.

So after impulses, quaternions and clock, what a physics-only replayed step
still gets wrong is confined to bodies in contact (`Wippe` and `Bridge`, hinged
but touching nothing moving, stay bit-exact) and is not the recheck. The
candidates left are the per-pair structures - friction contact points and
mindists - whose state is `protected` but reachable with the existing
access-shim pattern, without touching IVP. Before building that, the networked
measurement below says how much it would be worth.

## What actually limits the mechanisms in a networked session

Measured against the **server**, 4 players, 300 ms, every mechanism in sector 2,
client0 and client3 identically:

| body | first tick off the server by > 1e-4 m | by how much |
|---|---|---|
| `P_Ball_Paper_MF` | **26** (the first comparable tick) | 0.43 m |
| `P_Modul_29_Platte01..09` | **26** | 0.07 - 0.26 m |
| `P_Box_MF`, `P_Ball_Stone_MF` | **26** | 0.03 - 0.05 m |
| first rollback on the client | 90 | |

Every plate is already a quarter of a metre off the server sixty ticks before
the client's first rollback. The Paper ball's error then *oscillates* - 0.4, 1.6,
0.2, 1.6 m with the plates' period - which is what a time shift of a periodic
motion looks like, and the logs say where the shift comes from: the clients
activate sector 2 locally at tick **12** and the server at tick **21**, on
*receipt* of the client's Sector event, one-way latency later (tick 14 in an
earlier run; it moves with the network). The journals agree: the server's first
checkpoint carrying the mechanisms is tick 26 against the client's 18-20, and
the Paper ball first moves at server tick 28 against client tick 20. Everything
in the sector is physicalized and released at a different session tick on the
two sides.

It is not a pure shift either: the best alignment (k = -6 ticks) halves the mean
error (0.42 -> 0.23 m on the Paper ball) and does not remove it, because the
Paper ball falls under gravity from its spawn whether or not the plates have
started, so a later start is a different motion, not a delayed copy.

This is the architectural item the original plan called the sector phase
offset, and it is two to three orders of magnitude above the rollback residual
(1e-3 m per rewind). Rollback fidelity is not what limits the mechanisms in a
real session; the activation tick is.

One tool note: against a server journal the server start has since wiped,
`first_divergence.py` had reported the first mechanism divergence at tick 258
for the run before this one, while a direct scan of the same pair showed 0.38 m
at tick 236. On the surviving pair the two agree (tick 26). Unresolved, so the
harness scripts now copy each run's server journal beside its client journals
(`server.bmjr`) and a claim like that can be re-checked.

## Sector activation as an input: measured, and it is the whole mechanism error

A harness-only experiment, no server, IVP or retail change: `--sector-lead N`
makes the headless client treat its start sector's activation the way inputs
are treated. Instead of activating at the anchor and reporting it after the
fact, it stamps a Sector event for a tick `lead` ahead (`-1` = `input_delay + 1`,
the lead inputs already get), sends it first, and activates locally when that
tick comes - before its engine tick, which is where the server's
`update_sectors()` runs relative to its own. The server already holds a stamped
event until its tick (`session_runner::step`, `e.tick <= tick`, join order), so
nothing on the server had to change for the measurement.

4 players, proxied to a 617-620 ms round trip (input delay 32 ticks), same
batch, client0 and client3 identically:

| run | client activates | server activates | mechanisms first appear (server / client) | plates first off the server | before the first rollback |
|---|---|---|---|---|---|
| today | 12 | 21 (on arrival) | 26 / 18 | 0.07 - 0.26 m at tick 26 | up to 1.6 m |
| lead = input_delay + 1 | 45 | 47 | 52 / 50 | 0.015 - 0.044 m at tick 54 | |
| lead = 50 | 62 | 64 | 68 / 66 | 0.003 - 0.011 m at tick 70 | |
| **lead = 50, client +2** | **64** | **64** | **68 / 68** | **never** | **0.00000 m, every body, until tick 215** |

With the activation tick aligned, every mechanism in the sector - the ten hinged
plates, the Paper ball riding them, the crate, the stone - agrees with the
server **exactly** for the hundred and fifty ticks until the client's first
rollback (the remote births at 216 and the 300 ms input corrections). What
appears after that is 1-6 mm at tick 258 and 1-3 cm by tick 300: the per-rewind
contact-state residual, accumulating under frequent rollbacks at a 600 ms round
trip, and now cleanly separated from everything else.

### The 2-tick guard, fixed - and the pacer it hid

In both led runs the server activated exactly two ticks after the stamp
(45 -> 47, 62 -> 64). `physics_world::update_sectors()` returned early while
`desired == active_sectors_` and only *then* ran its idle counter, so the
counter never accumulated while the sets agreed and always needed three passes
once they differed. It now counts every tick (`session/sector_pacer.hpp`, five
tests): a pass may start when the manager has been idle for three ticks, the
rule the comment always described.

That made the stamped tick land at lead 33 (45 -> 45), and exposed the rule
itself. At lead 1 and lead 8 the server still came out one tick late (13 -> 14,
20 -> 21): the level's own sector-1 walk at session start had not been idle for
three ticks yet. **The pacer is part of the simulation.** Its input, whether
`Gameplay_SectorManager` is active, is the same on every side of a lockstep
session, so each client now counts the same pacer at the same point - once per
step, before it - and runs a held sector at the first tick at or after its stamp
the pacer allows. Same batch, 4 players, 300 ms:

| lead | stamped | server | client | bodies appearing at different ticks | worst before the first rollback |
|---|---|---|---|---|---|
| 1, pacer mirrored | 13 | 14 | 14 | 0 | **0.00000 m** |
| 8, pacer mirrored | 20 | 21 | 21 | 0 | **0.00000 m** |
| 8, `--no-sector-pacer` | 20 | 21 | 20 | 3 | 0.281 m |
| input_delay + 1 (33), twice | 45 | 45 | 45 | 0 | **0.00000 m** |

The one-tick miss also moved the first rollback from tick 215 to tick 90: a
mechanism one tick out of phase is caught by the first full snapshot that
carries it.

### The server ran the level's own checkpoint chain

The first retail run that crossed a checkpoint for real (beamed onto
`PC_TwoFlames`) found a second activation path. The stamped Sector event and
the server's pass agreed (633 = 633), and yet the server's sector-2 bodies
appeared at tick 624 and the client's at 638 - the same trajectory, shifted by
14 ticks - and the server's sector-1 bodies vanished at 620 while the client
kept them.

The server's world runs the level's scripts, and `rewire_proximity_probes()`
points every `TT Scaleable Proximity` that watches `Ball_Pos_Frame` at the
nearest player's clone, the checkpoint flames' included. So a clone that
reaches a checkpoint fires `Gameplay_Events / activate Sektor` in the server's
world, which runs `Gameplay_SectorManager` on its own cells: the next sector
**and the deactivation of the previous one**, at the tick the server's clone got
there, behind `update_sectors()`'s back. In a multi-player room that tears down
a sector other players are still in.

The level's own manager run is now held in **every** world
(`game/sector_activation.hpp`): the chain's `Execute Script` is fed a parameter
holding no script. Execute Script activates its Out at once on a null script,
so the chain, its two `Set Cell`s and whatever hangs behind it keep their
shape; only the manager run is gone. The server's `update_sectors()` is then the
only path that runs the manager there, and each client runs it for the sectors
it enters, at the stamped tick.

### The protocol, as built

* **Every world holds the checkpoint's manager run**: the server's world and
  the headless client's with the shared helper, the retail client's with its
  own copy (neutralization N3), which hands the original source back at
  session end like N1 and N2.
* **A sector a client enters is an input.** Its Sector event is stamped
  `input_delay + 1` ticks ahead of the step after the crossing and sent at once;
  the client runs the manager (deactivating nothing, like N2) at the end of the
  frame before that step - or the first one after it the mirrored pacer allows.
  The server applies the event before that step and its pass runs right after,
  under the same pacer.
* **Two ways to see a crossing, deduplicated.** The retail client takes it from
  `OnPostCheckpointReached` and from a poll of `IngameParameter[0][1]` in
  `physics_session_frame`. On the arena the poll saw it first: the hook sits
  at the end of the checkpoint chain, which finishes a frame after its
  `Set Cell`. A sector is an input once per session.
* **The automation's `sector` verb** goes through the same hold during a
  session.
* **The headless client** does all of it by default (`--sector-lead -1`); `0` is
  the old behaviour, kept as the control.

### Measured: real crossings, headless and retail

Solo, driving the whole arena through all three checkpoints, 300 ms, 50 s:

| | sector 2 / 3 / 4 (client) | server | bodies appearing at different ticks | rollbacks | worst body |
|---|---|---|---|---|---|
| **as built** | 855 / 1349 / 1842 | 855 / 1349 / 1842 | **0 of 52** | **0** | **0.00000 m** over 1375 checkpoints |
| `--sector-lead 0` | 821 / 1315 / 1808 | 821 / 1315 / 1809 | 17 | 1 | 0.297 m |

The control is worth a second look. Its event arrived in time and the server's
pass ran on the client's tick (821 = 821), and its bodies still came out two
ticks late. The client's chain starts the manager in the middle of a frame,
from inside the script graph; the server's pass starts it before the frame. An
event that is merely on time is not enough: both sides have to start the
manager from the same point of the frame.

The retail client, solo on the arena against the Arch server: a real
checkpoint crossing held for 583 and run for 583, the `sector 3` verb held for
1287 and run for 1287, the server's passes on both ticks. All 23 bodies appear
on the same tick on both sides. Every sector-2 and sector-3 body is
**0.00000 m** over the whole session, 350 and 331 checkpoints, with one
exception that is a recording artefact, not a divergence (see the measurement
traps).

### What it does not cover

* **A rollback across a sector's setup.** See "Still open": measured at
  0.2-0.7 m when the anchor falls in the fifteen ticks after an activation,
  and the next thing to fix.

The sector union across players - a client ran only the sectors *it* entered
while the server ran every player's - was the other gap here; the next section
is its fix.

## The sector union, as built

Every side of a session now runs the same set of sectors, each started at the
same tick everywhere (`session/sector_union.hpp`, physics protocol 3). The
rule is one pure struct the server's world, the headless client and the retail
client all run.

* **A Sector event is an input to everyone.** The member that crosses a
  checkpoint stamps it `sector_lead` ticks ahead and sends it at once; the
  server applies it at that tick and relays it without Nagle; every other
  member applies the relay at the same tick, holding it until then (the
  retail client used to apply relayed events on arrival; a Sector event now
  waits for its stamp). The pass that starts the sector - the three writes of
  `update_sectors()`, under the shared pacer, the lowest owed sector first -
  then runs at one tick on every side, and the sector's mechanisms are born on
  the same tick everywhere. The headless `--start-sector` and the automation's
  `sector` verb go through the same stamp.
* **The lead is the server's, sized per session** from the worst member round
  trip like the input delay (`session::sector_lead_for_ping`, in
  `SessionStart`): the input delay (the one-way budget of the leg to the
  server), one tick (a client can only run the manager before its next step),
  and a second one-way budget for the relay leg. 9 ticks on a LAN, about 15 at
  a 100 ms round trip, 33 at 300 ms. Too short and the relay reaches a member
  after its tick: that member starts the sector late and stays out of phase on
  every mechanism of it, which no rollback repairs (it cannot replay a
  sector's setup). Too long and the mechanisms start a little later after the
  crossing. The union counts late stamps on every side, so a measurement can
  tell late from wrong.
* **Nothing is deactivated, on the server either.** The retail game tears a
  sector down when its one ball leaves it. The server used to as well, once no
  player was left in a sector, while the clients never did (their chains are
  pinned, N2) - so the two worlds were not the same world even in a solo
  session, and only the absence of the torn-down bodies from the snapshots hid
  it. A client tearing a sector down would destroy mechanism bodies under its
  rollback history, and a sector one member left is one another may still be
  in or come back to. So the union only grows: sector 1 from the anchor, then
  every sector any member reports, for the rest of the session; the server's
  `+2 -1` passes became `+2`. What it costs is the scripts of the sectors
  behind the players, which the retail game would have stopped.
* **Late joiners and resyncs** get every Sector event of the session re-sent
  after the members' last Physicalize, so their union grows by the same
  sectors (one pass per idle manager, so a joiner catches up over a few dozen
  ticks while the full snapshot corrects it); a stamp of a sector that is
  already up is a no-op, which is what a resynced member sees.
* **The measurement knobs are gone.** `--sector-lead`, `--no-sector-pacer`
  and `--sector-skew` were A/B switches for the single-client measurement
  above. With the lead the server's and the passes the union's, a client cannot
  be given a lead or a pacing of its own; the rows above that used them are
  history.

### Measured

Four headless clients driving the arena on staggered records (the harness's
`--drive`: player i sets off 120 + 40 i ticks in, the first brakes every 500),
so one of them leads through all three checkpoints and the others cross into
sectors that are already up, against the server in Arch WSL. Before this, the
trailing client took one rollback of 0.6-2.7 m on a mechanism of the sector it
had just entered at every boundary (docs/mechanism-phase-at-sector-activation.md).

| | lead | passes, server | passes, each of the 4 clients | late relays | mechanism corrections, any client | server journal |
|---|---|---|---|---|---|---|
| LAN, 50 s | 9 ticks | +2 @779, +3 @1223, +4 @1667 | the same three ticks | 0 | **0**, max error **0.00000 m**, 52/52 bodies | 2995/2995 replayed |
| 150 ms each way, 90 s | 35 ticks (530 ms) | +2 @841, +3 @1286, +4 @1730 | the same three ticks | 0 | **0**, max error **0.00000 m**, 52/52 bodies | 5542/5542 replayed, and 6032/6032 on Linux |
| the retail client hosting 3 driving bots, LAN, 48 s | 9 ticks | +4 @352 (the client's `sector 4` verb), +2 @827, +3 @1321 (the leading bot's crossings) | the same three ticks on the three bots and on the retail client, "as stamped" | 0 | see below | 4045/4045 replayed |

In the 150 ms run two trailers crossed into sector 2 at ticks 867 and 870 and
one of them went on through 3 and 4: three stamps of sectors already up,
relayed to everyone, and not one pass or correction came of them. The balls'
own prediction errors in those runs (`max_err` 20-38 m, four balls shoving each
other down one lane behind a 300 ms round trip) are the ball story, unchanged
by this; the mechanisms are the point here, and they are exact.

The retail run has both directions of the relay: the real client's stamp
started sector 4 on the server and the bots at 352, the leading bot's stamps
started 2 and 3 on the server, the other bots and the real client at 827 and
1321, and the two stamps of sectors already up - the bot's sector 4 at 1814,
the real client's own crossing into sector 2 held for 2380 - started nothing
anywhere. Sector 4's bodies, born at 352 with no ball within 500 m of them,
had no mismatch for 1600 ticks, until the balls reached the balloon platform
at 1982; the phase offset of docs/mechanism-phase-at-sector-activation.md is
gone. The rollback mismatches that remain (26 on the real client, at the same
ticks and on the same bodies as on the bots: the pusher at 600, the see-saw at
1092, the sliding stone at 1388, the balloon platform at 1982) were first read
as the ball story - predicted remote balls arriving at mechanisms. **They are
not.** Every one of those lines carries the `z` suffix on the local pose, the
trace's mark for a body that is not simulated, against a server row that is
moving (the pusher: `local=(162.508,-1.485,43.650)z`, `dv=0.067` on the
server), and the real client and a bot log the pusher's mismatch with the same
`dp=0.0898 dv=0.067` to four decimals, which two different ball predictions
could not produce. The clients had put bodies to sleep that the server kept
awake, deterministically, after the same early rollbacks - the next section.

## The replay's sleep decisions, restored (bridge API v13)

The restore was bit-exact for pose, velocity, queue and quaternions, and still
every rewind changed the world. What it left at its live values was IVP's
freeze bookkeeping: each core's two calm references (`time_of_calm_reference`,
the calm pose and quaternion `calc_movement_state` compares the body against to
decide whether it may sleep), each simulation unit's movement-check countdown
(engine change #5), and the sleep state itself, which the `keep` mode of a
restore from our own record deliberately left as it was NOW rather than as it
was at T - and a body born in the replayed window was parked and re-posed awake
whatever its birth record said.

Measured before touching anything (the queued-impulse lesson): the journals
carry `BODY_FLAG_SIMULATED` every two ticks, and `scripts/sleep_divergence.py`
compares two client journals' local checkpoints body by body for the first tick
their sleep flags differ. Solo, one forced depth-12 rewind every 40 ticks (85
rewinds), against a control from the same batch (two controls agree on every
body to 0.0000 m and on every sleep flag):

| | bodies whose sleep flag ever differs | the plates born in the window (`P_Modul_29_Platte*`) | `P_Ball_Paper_MF` | `P_Modul_30_Wippe` |
|---|---|---|---|---|
| before (the v11 restore) | **44** | **0.48-0.62 m** from the first checkpoint after the rewind across their births | 0.63 m | 0.35 m |

The see-saw and the bridge were asleep in the control from their birth
(physicalized frozen) and awake in the arm: parked awake by the birth branch,
they tilted under gravity for the replayed window and were released awake. The
plates' 0.6 m is the same thing on a hinged chain. That is the "rewind cannot
cross a sector's setup" item of the open list, and it is not the scripts: the
block trace of the frames after an activation (`--debug-frames` on the SimTool)
shows the manager walking for three frames, every module script physicalizing
and hinging its bodies in its first frame, and nothing but proximity sensing
from then on.

**The fix** (`bmmo_physics_core_extra` v13, `rollback.hpp`): the calm references
travel with every record and are written back after the pose - shifted by the
live clock's offset from the anchor's when the clock is not rewound, so they are
as old relative to every replayed step as they were live; the unit countdown is
written in a pass of its own after every body is back; a body restored from our
own record is woken or frozen as the RECORD has it; a birth is parked and
re-posed in its birth record's sleep state with its birth record's bookkeeping.
Each half is a switch in one binary (`--no-restore-calm`, `--no-restore-unit`,
`--no-sleep-as-recorded`), same batch, same control:

| arm | sleep flags differ | plates | Paper ball | see-saw | `P_Modul_03_*` | `P_Modul_08_Schaukel` |
|---|---|---|---|---|---|---|
| all three off (the old restore, rebuilt) | 44 | 0.48-0.62 m | 0.63 m | 0.35 m | 0.54 m | 0.93 m |
| calm + unit, live sleep state | 44 | 0.48-0.62 m | 0.63 m | 0.35 m | - | 0.93 m |
| sleep as recorded + unit, **no calm** | - | 0.17 m | 0.26 m | 0.09 m | - | sack **8.6 m**, rope 3.7 m, the ball 139 m |
| sleep as recorded + calm, no unit | 42 | 0.013-0.02 m | 0.19 m | - | 0.32 m | 0.93 m |
| **all three on** | 42 | **0.02-0.03 m** | 0.19 m | - | 0.32 m | 0.93 m |
| all three on + `--restore-clock` | 42 | 0.02-0.06 m | **1.02 m** | - | 0.32 m | 0.93 m |

Three things the matrix says:

* **The sleep state as recorded is the half that matters, and it needs the
  calm references.** Alone it is a disaster: a body woken from the record with
  its live references in place reads as "moved since the reference" for ever
  and never sleeps again, and the chain it is part of comes apart (the sack
  and rope at 8.6 and 3.7 m, the ball at 139 m). With the references it is a
  twenty-fold improvement on every body born in the window.
* **The unit countdown is within noise** here (the no-unit arm is, if anything,
  a little better on the plates). Kept, because it is the state engine change
  #5 added and nothing else puts it back, but it bought nothing measurable.
* **The clock rewind hurts** (the Paper ball 0.19 -> 1.02 m) and stays opt-in,
  as the earlier null measurement and the hull manager's time-anchored state
  already suggested; the shifted references make it unnecessary.

**What is left is two different kinds of thing.** `P_Modul_08_Schaukel`
(0.93 m, from a rewind at 1920): the sector-4 block trace names the cause.
`P_Modul_08_MF Script` is `Physicalize` + `Set Physics Hinge` and then
**`SetPhysicsForce` + `Physics WakeUp` + `Delayer`, with the force toggled by
the Delayer every ~32 frames** - a script that drives its mechanism on a
timer, which a physics-only replay keeps at whatever phase the toggle had
when the rewind started. The sandbag has the same shape (`P_Modul_26_MF
Script`, group `Swing`: Delayer -> Sequencer -> SetPhysicsForce) and the
balloon script drives its sliders and springs from a Sequencer; those three
are the mechanisms left in the networked mechanism-rollback column below.
That is the class the script replay was always for, and it is the next
item. The `P_Modul_03` group (0.32 m in the frames after their births at
~2000) is something else: it has **no script at all** (`--list-scripts
P_Modul_03` finds none; the manager's own Type 2/3 rows physicalize its
parts), so its bodies are born awake and settle against each other - which
points at the contact and friction-system state the earlier candidate list
already named, not at scripts. The ~40 bodies whose sleep flags still differ
somewhere are the small residual (the pusher 0.0004 m, the bridge 0.002 m):
a body woken by a restore lands in a fresh simulation unit whose merges with
its neighbours IVP decides anew, so its check phase cannot be put back
exactly through the body API.

**On the networked path** (four headless clients driving, 150 ms each way,
90 s, the same run as the union's measurement, before and after), every
rollback classified by the body that breached:

| | rollbacks on a ball (prediction at a 300 ms round trip) | rollbacks on a mechanism |
|---|---|---|
| before, clients 0-3 | 46 / 433 / 433 / 408 | 62 / 65 / 66 / 60 |
| after | 37 / 425 / 427 / 406 | 61 / 66 / **46** / **32** |

The ball column is the ball story and does not move. The mechanism column
moves where sleep was the cause, and what is left of it is named: the
balloon chain (`PE_Balloon_Ballon0x`, 40-52 of each client's mechanism
rollbacks before, up to 40 after) and the swing (`P_Modul_08_Schaukel`, 14-24
after) - the two mechanisms whose scripts drive them on a timer (the balloon
script's Sequencer, the swing's Delayer-toggled force), with the sandbag a
distant third. Those are the script replay's, next. The server journals of
both runs replay bit for bit.

## The script replay, as built (bridge API v14)

The mechanisms a physics-only replay could not follow were the ones a script
drives on a timer: the swing's Delayer toggling its force every ~32 frames, the
sandbag's Delayer -> Sequencer -> force, the level-end balloon's Sequencer behind
its sliders and springs. A rewind kept their forces at whatever phase the toggle
had when the rewind started, and the swing alone cost 0.93 m per depth-12
rewind on the solo probe. So the scripts travel with the rollback records now
(`game/script_replay.hpp`):

* **What is state.** Every tick the client captures, for every mechanism script
  (a root script whose owner is `P_*`/`PE_*` and not `P_Extra*` - 15 on the
  arena, 292 blocks, 1575 parameters), what the engine itself changes between
  frames: the runtime activation flags of every block (`ACTIVE`,
  `ACTIVATENEXTFRAME`, `DEACTIVATENEXTFRAME`, `RESETNEXTFRAME`, `LAUNCHEDONCE`),
  which inputs and outputs are active, and the bytes of every local and output
  parameter (a Delayer keeps its elapsed time in output 0, a Sequencer its
  position). The graph - blocks, links, input parameters - is not state.
  Pointer-typed parameters are left alone, bar one: `SetPhysicsForce` keeps a
  raw `PhysicsControllerForce*` in local parameter 0, and that controller is
  the one piece of script state CK cannot carry, so the bridge captures whether
  the block has one and the point and force it pushes with
  (`BmmoPhysicsForceBlockState`) and a restore deletes or re-creates it
  (`BmmoPhysicsForceBlockRestore`), so the replayed Shutdown / Create edges meet
  the object they expect. 23.6 KB per tick on the arena.
* **What a replay does.** A restore writes the anchor's script state back
  before the bodies (a force block's controller first, then the bodies' poses
  over it), and every replayed tick runs one behaviour frame of those scripts
  - the next-frame activation flags, then `CKBehavior::Execute` of each active
  script, the order the behaviour manager has - before its physics step. The
  bridge's object event log is muted while the scripts run, so a replayed
  `Physics WakeUp` is not reported to the server as a live `BodyRevived`.
* **What is not replayed, and why.** The sensors' reference frame
  (`Ball_Pos_Frame`, which every `TT Scaleable Proximity` of the mechanisms
  measures) is a child of the ball entity, so it follows the restored and
  replayed ball without any script; running the `TT Set Dynamic Position`
  blocks that touch it as "satellites" was tried and tore the level-end bridge
  apart (7.9 m), because the one in `Gameplay_Ingame` moves something else.
  Nothing outside the mechanism scripts is executed.
* **Two gates, both measured in.** A script that was not active at the anchor
  keeps its live state and is not executed: the anchor's state would put a
  script the sector manager activated inside the window back to sleep, and
  nobody in the replay would wake it (a module activated at 2480 never started
  after a rewind across it). And a script the game told to reset after the
  anchor - the adapter watches `RESETNEXTFRAME` every tick and remembers the
  tick per script - keeps its live state too: a sector activation resets the
  new sector's scripts, a death resets every module of the sector, and the
  anchor's mid-flight state coming back over a teardown in progress put the
  balloons' lift back on bodies the reset was rebuilding (the bridge flew
  1800 m). A global barrier (every script physics-only across any reset) was
  the first form and was replaced: it also silenced the other sectors' scripts
  across every sector activation.

**Measured, solo probe** (one headless client, the WSL server, a forced depth-12
rewind every 40 ticks, the same binaries and the same map for control and arms,
compared over the ticks before the bot drives off the road end at 2473 - the
death that follows is the next section's subject):

| worst body against the control | physics-only replay | with the scripts |
|---|---|---|
| `P_Modul_08_Schaukel` (the swing) | **0.93 m** | not in the top eight |
| `P_Modul_03_*` (gate, floor, walls) | 0.32 m | not in the top eight |
| `P_Ball_Paper_MF` (the prop ball on the plates) | 0.37 m | 0.19 m |
| `P_Box_MF` | 0.09 m | 0.09 m |
| `P_Modul_29_Platte*` | - | 0.03 m |

Server mismatches over the run (the guard below in place, so a death costs
none): **16 with physics only** - the swing eight times at 0.39-0.93 m,
`P_Modul_03` twice, the prop ball four times, the box once, the own ball once -
and **6 with the scripts**: the box once, the prop ball four times, the own ball
once. The own ball's is the same 0.76 m in both arms and in every forced arm
before: a rewind that crosses the ball's departure from the road edge puts it
0.76 m lower, a contact the beam could not bring back - the next section. The re-executed scripts arrive where the live frames left them on
every rewind but three (of 83): the ventilator's `TurnON_Permission` after the
first two rewinds of the run (1 live vs 0 replayed - a one-shot the record has
no bearing on) and the balloon's `UFO` referential (an object id vs 0) on the
first rewind after sector 4's activation; 23 rewinds differ in float parameters
only (sensor distances, timers following the physics by an ulp), and three
rewinds kept a script live for having been reset after their anchor. The
sleep-flag disagreements against the control do not move (41 vs 40 bodies).

**The death found a second thing.** The arena has a death zone since 2859d88,
and the bot's drive leaves the road end at 2473: with no rewind at all the
headless client then disagreed with the server by 12-20 m on the level-end
balloon chain for the rest of the session, 11 corrections in the control run
itself. Retail Ballance's `Deactivate Ball` resets the whole sector on a death
(`Execute Script` on `Gameplay_SectorManager` with Reset); the retail mod
neutralizes that call site and the level reset's for the session (design 9.25
N1, 9.28) because the server's world never resets, and the headless client
never had that edit. It has it now (`game/sector_reset_guard.hpp`, the same
walk of `Gameplay_Ingame` for the two `Execute Script` blocks, the death one
emptied at session start and the level-reset one once the sector is up).
With the guard, the control run's 11 server mismatches become **0**: a death costs
nothing, on the headless client as on the retail one.

**Measured, networked** (four headless clients, 150 ms each way, 40 s, all
four beamed to the swing's bay in sector 4 - where the script-driven mechanisms
are - with every rollback classified by the body that breached, the same binary
with the scripts on and `--no-script-replay`):

| | rollbacks on a ball (prediction at a 300 ms round trip) | rollbacks on a mechanism | of them, the swing | server mismatches |
|---|---|---|---|---|
| physics-only replay, clients 0-3 | 6 / 7 / 6 / 6 | 17 / 21 / 23 / 24 | **16 / 19 / 21 / 22** | 23 / 28 / 29 / 30 |
| with the scripts | 7 / 7 / 8 / 8 | **1 / 3 / 4 / 6** | **0 / 0 / 0 / 0** | 8 / 10 / 12 / 14 |

The ball column is the ball story and does not move. The mechanism column
loses the swing entirely - 78 rollbacks on `P_Modul_08_Schaukel` across the
four clients became none - and what is left of it is the prop ball and the box
the players knock about and the see-saw, which are contact, not script. The
same pair on the plate row of sector 2 (bay 9, no scripted mechanism in reach)
moves nothing either way: two pairs there gave 8/9/4/4 vs 3/3/2/11 and then
5/5/4/4 vs 7/1/0/4 mechanism rollbacks, which is the floor of a bay whose
mechanism column is a bistable prop ball. The server journals of all six runs
replay bit for bit.


### The restore wrote through CK's parameter links (found 2026-10-22)

The ventilator's `TurnON_Permission` above was not a one-shot the record has no
bearing on. It is a Boolean local of the `Physics Force` graph in
`P_Modul_18_MF Script` (`BallanceMMOSimTool --describe-script "P_Modul_18_MF
Script"` prints the graph): two `Identity` blocks swap the graph's
`TurnON_Permission` / `TurnOFF_Permission` locals when the ball enters or
leaves the fan's box, by writing their output parameters, whose
**destinations** the locals are. The restore wrote every output parameter with
`CKParameter::SetValue`, and on a `CKParameterOut` that call ends in
`DataChanged()`, which copies the value into every destination parameter -
so the Identity block's parked output (0) was propagated into the local the
restore had written a moment before (1), on every rewind, and the fan never
lifted the paper ball again: the arm's live scripts agreed with the control's
on everything else from tick 8 to the end (`BallanceMMOSessionClient
--compare-scripts`, below), and the paper ball and the plates it bounces on
were the bodies that drifted (0.32 m and up to 3 cm). The same path wrote the
balloon's `UFO` referential (an object id) back to 0. The live frame
propagates only when the block executes, and the replayed frames do that
themselves, so the restore now writes through `GetWriteDataPtr()` - a plain
buffer write with no notification - and the mismatch is gone from the
engine's own check and from the cross-run compare.

How it was found: `--dump-savestate A B DIR` now writes each tick's recorded
script state beside the blob (`<t>.scr`), and `--compare-scripts DIR` compares
this run's record, tick by tick and keyed on every listed difference, with
another run's files, naming script, block and field - the first difference
that is NOT a replayed record's by-design live state (a script activated
inside the window keeps its live flags) was this local, at the first rewind.

### The sector activation, replayed (script replay v2, 2026-10-22)

The v14 rule left one class of rewind inexact by construction: a window that
spanned a sector activation.  The scripts the activation starts were kept live
and not re-run, the bodies they made inside the window were parked at their
birth pose and rechecked back in after their birth tick, and what a recheck
cannot reproduce - IVP's overlap-tree layout, the moment a body's mindists and
hull entries are created, a fresh core's age - left the plate row and the swing
off the control after every deep rewind (20:12 and 50:48: 11 of 53 bodies,
the swing 5.7 m).  Re-creating the bodies from their recorded Physicalize made
45 of 46 births bit-identical but severed the joints the kept-live scripts had
attached.

So the activation is treated as the input it is.  Every activation of
`Gameplay_SectorManager` the live frames made is recorded by the tick whose
frame consumed it - the union's own passes, and the ones scripts outside the
replay asked for (Init Ingame at the level start), seen as next-frame flags
after their frame - and the replay re-issues it at that tick, before the tick's
scripts.  The manager travels with the records like the modules do; a restore
writes EVERY selected script, the inactive ones included, since the replayed
manager activates them again with the reset; a mechanism body born inside the
window is destroyed before the blob is applied, its entity put back at the
matrix its Physicalize found (the local matrix under a module's parent, which
the replayed manager then places), and the replayed module script makes it
again - body, joints, force controllers - the live way.  Three things the
capture had to learn for that, found by an adversarial review of the design
before it was built: a graph's in-flight delayed links (the manager's walk is
a four-frame link the SDK does not expose; the countdown on the link is
captured and re-armed), the joint blocks' handle presence (a block that had no
joint at the anchor deletes its live one through its own reset callback, so
the destroyed body carries none and the replayed Create starts from nothing),
and the three data-array cells the chain reads.  The headless frame runs its
rewinds before the union's pass, as the retail client always did, so a pass is
never written over by a restore in the same frame; the live cells are kept
across a rewind; a pending activation is re-issued for the frame about to run
after any rewind.  A presence gate in the harness fails an arm that loses a
body the control has.

Measured (`scripts/savestate_check.py --journals`, fresh controls):
50:48 (48 rewinds), 20:12 (123) and 7:5 (351) are **EXACT on all 53 bodies**,
unrounded worst 0.000e+00 m; 49, 46 and 50 bodies made again inside replays,
every one bit-identical to its birth record after its birth tick's step.  Two
wrong turns on the way, both measured: putting the entity back to the scene's
initial conditions (the level file's poses, before the manager placed the
module: a box 900 m off, the ball fell to its death), and writing the world
matrix of a parented piece (a micrometre off once the parent is placed).

## The one the ball still had: a contact that ended inside the window (bridge API v15)

Every forced-rewind arm above, scripts or not, took the same single correction
on the own ball: at tick 2400 the server had it 0.76 m higher, and the control
run took none. The rewind that does it is the one at 2360, anchored at 2348:

| rewind at 2360, anchor | the ball at the anchor | against the control |
|---|---|---|
| 2348 (depth 12) | rolling on the road, its floor contact alive | **0.76 m** |
| 2348, `--no-restore-recheck` | same | 0.76 m |
| 2348, `--no-restore-impulses` | same | 0.76 m |
| 2352 (depth 8) | just off the edge | exact |
| 2356 (depth 4) | falling | exact |
| rewind at 2340, anchor 2328 | on the road, the contact still alive at 2340 | exact |

So the restore is exact whenever the anchor's floor contact still exists at the
rewind tick, and loses 0.76 m exactly when the contact ended inside the window.
The per-tick data says the same: the first checkpoint after the rewind is
already 0.24 m lower and 0.54 m/s faster downward than the control, and the
rest is free fall. The contact diagnostic (`describe_contacts`, the friction
contact points and exact mindists of one body) shows why. Live, the ball rolled
on `A_Floor_035` with one friction contact until tick 2347 and lost it at the
edge; beamed back to 2348 it has no contact and no mindist at all; the mindists
are back one step later (0.019 m, exact); a friction contact never comes. IVP
creates a contact point only on an impact (`IVP_Mindist::do_impact`), and a
ball put back at rest on the floor has none coming - it rolls its last ticks on
the road resting on nothing, and leaves the lip early.

A beam cannot carry contacts, so the records do (`bmmo_physics_contact`, bridge
API v15): per tracked body, the partner of each friction contact and the state
IVP keeps on the point (the gap, the friction spans, the pressure, the pull and
keeper counters). A restore, after every body is back, re-creates a recorded
contact the body lacks through IVP's own path: the pair's collision checkers
driven to exact mindists now (the beam's recheck had made them hulls - a beamed
body has `current_speed` 0, so the hull check length is just the collision
distance, and the PSI would only turn them exact a step later; for a mesh the
checker is an `IVP_OO_Watcher` whose children must be driven one by one), the
shortest exact mindist to the recorded partner, `try_to_generate_managed_friction`
on it, which files the point into a friction system and fires the game's
friction listeners as an impact would. `docs/engine-changes.md` has the bridge
note; `--no-restore-contacts` turns it off and `--contact-flags` picks the
variant.

**Which variant, measured** (the forced physics-only run against the same
control, so the contacts are the only thing that changed; the baseline took 16
corrections, one of them the edge):

| re-created | fields copied onto the point | corrections | on the own ball | the edge |
|---|---|---|---|---|
| for any body with a recorded contact missing | all | 25 | 9 | fixed |
| any body | none (IVP's fresh point) | 17 | 1 | fixed |
| only a body with no contact left | none | 15 | 0 | fixed |
| only a body with no contact left | all | 21 | 6 | fixed |
| no contact left, static partner only | none | **16** | **0** | **fixed** |
| no contact left, static partner only | all | 21 | 6 | fixed |

Copying the recorded fields onto the new point costs 5-11 cm on a ball crossing
from one floor block to the next inside a window (a case the plain restore
got exact: the point IVP makes at the next impact is consistent with the
`tmp_contact_info` the solver rebuilds, and one carrying another moment's gap
and spans is not), and re-creating a point between two movable bodies - two
player balls, a ball on a plate - crashed the next impact solve
(`IVP_Impact_System::test_loop_all_pairs`, a null pointer; read from a dump
below) on one of four networked clients. So the default is the last row but one: a fresh point,
only for a body that has no contact left, only against static geometry - the
measured case and nothing more; the recorded fields stay in the record for the
diagnostic.

**With the default in place:** the single rewind across the edge is exact
(one contact re-created, no correction); the scripts arm over the whole forced
run takes **5** server corrections (6 without the restore, 16 physics-only),
none of them on the own ball, and the swing and `P_Modul_03` stay off the
worst-body list; the four-client swing-bay run at 150 ms neither crashes nor
moves - mechanism rollbacks 2 / 5 / 4 / 4 against 1 / 3 / 4 / 6 without it, the
ball column 7 / 5 / 5 / 6 against 7 / 7 / 8 / 8, the server journal bit-exact.
The 12 contact-restore failures each client logs there are the remote balls'
records restored while their body is unphysicalized, which is nothing to
restore. The 189 unit tests pass.

### Movable partners: what the crash was, and the precondition IVP never states

The movable-partner crash reproduced on demand (`--contact-flags 0`, the
four-client swing bay at 150 ms: one client of four, at tick 424-735, in the
start pile, in two of three runs) and was read from a full-memory minidump
(`BMMO_CRASH_DUMP=1`, a Python walk of the registers with the class layouts
MSVC reports under `/d1reportAllClassLayout` - the Windows Kits debugger on
this machine does not start). The faulting instruction is
`core_was_already_pushed_in_system(core)`: the impact system reads
`core->tmp_null.old_sync_info->was_pushed_during_i_s`, and the pushed core -
`Ball_Stone_BMMO_3813333153`, another player's ball, in a friction system
with the own ball and the floor, in the own ball's simulation unit - has
`old_sync_info == NULL`. It is NULL because the core's `movement_state` is
`IVP_MT_NOT_SIM`: the ball was asleep, and the impact system synchronizes
(allocates that record for) a core it pulls in only `if
(IVP_MTIS_SIMULATED(movement_state))`.

IVP never meets that case, and the reason is a precondition the engine keeps
without stating it: **a contact point is created only by an impact, and an
impact revives both objects first** (`IVP_Mindist::do_impact` calls
`revive_object_for_simulation` on both, then `synchronize_with_rot_z` on each
simulated core, before `do_impact_of_two_objects` creates the point). So the
movable cores of one friction system are all simulated, or all asleep - they
share one simulation unit, and the unit sleeps as a whole. The v15 restore
broke that from the outside: it created a point between the own ball, awake,
and a ball the restore had asleep, and the first impact that reached the pair
through the own ball dereferenced the record no one had made for the sleeper.
A static partner never shows it (an unmovable core is never pushed, never
synchronized), which is why the default was safe.

Two ways to respect the precondition, both from outside IVP. The default now
is the conservative one: a pair whose two movable cores differ in simulation
state is left to IVP (its own impact will create the point when they meet,
reviving both). `BMMO_PHYSICS_CONTACT_REVIVE` (1024) is the other: wake the
sleeper the way the impact would (`ensure_core_to_be_in_simulation`, which
revives its unit) and then create the point. Either way a pair of two awake
cores or two asleep ones is created as before.

**Measured where the piles are** (the lane-kept four-client drive at 150 ms,
60 s, the server's snapshots off so nothing but the 17 lifecycle snapshots
ever pulls a client back; two runs per policy; `scripts/server_client_drift.py`
per 600-tick window; the host is the ball the other three spawn into):

| movable-partner policy (`--contact-flags`) | crashes | contacts re-created per client | host's own ball vs server, ticks 0-599 / 600-1199 | every own ball, 1200-1799 (max) | own balls, respawn pile 2400-2999 |
|---|---|---|---|---|---|
| left to IVP (768, the default) | 0 / 2 runs | 30-94 | 11.0 / 6.7 m, 11.0 / 8.8 m | 0.35 m | 0.01-6.1 m |
| mixed pair left to IVP, awake pairs made (0) | 0 / 2 | 531-959 | 12.0 / 9.9 m, 11.8 / 9.3 m | 0.70 m | 0.2-5.1 m |
| sleeper woken, pair made (1024) | 0 / 2 | 463-915 | 11.8 / 9.6 m, 17.2 / 13.6 m | 0.76 m | 0.04-4.3 m |

(And the four-client swing bay that produced the crash in two runs of three
with the old code ran clean twice with policy 0.) So the precondition holds -
five hundred to a thousand points a minute per client between player balls,
no crash in six runs - and **the piles do not care**: the host's ball is 11-17
m from the server's through the start pile under every policy, every ball is
within 0.8 m once the field strings out, and the respawn pile scatters them
by metres again under every policy. The contact points were not what a pile
lost in the restore, so the default stays at 768 - the narrow rule that was
measured on the road edge, nothing else gained - and 0 / 1024 stay as the
switches. What the table does say is where to look next: the ball that
suffers is the one the others spawn into, in the ticks when a kicked remote
ball meets it under a prediction that still has 20 ticks of its inputs wrong,
and the last joiner, which no ball spawns into, is within 2-3 m. That points
at the prediction window around a remote spawn rather than at the restore.
The restore side of the same question - does a rewind across a ball shoving
movable bodies come back exact when nothing is predicted? - is the probe
below.

**The solo probe across the prop cluster** (`--force-rollback 870:12`: one
rewind of depth 12, anchor 858, while the ball is shoving the prop balls and
the box - they first move at tick 834-836 in the control - against the
same-batch control over ticks 830-1300, no prediction anywhere):

| rewind (anchor) | policy | contacts re-created at the restore | `P_Ball_Paper_MF` | `P_Box_MF` | plates `P_Modul_29` | first difference |
|---|---|---|---|---|---|---|
| 850 (838) | 768 | 0 | 0.048 m | 0.058 m | 0.005-0.007 m | tick 852, 0.057 m on the box |
| 850 (838) | 0 | 2 | **0.170 m** | 0.058 m | 0.011-0.017 m | tick 852, 0.057 m |
| 850 (838) | 1024 | 2 | **0.170 m** | 0.058 m | 0.011-0.017 m | tick 852, 0.057 m |
| 870 (858) | 768 | 0 | 0.290 m | - | 0.012-0.019 m | tick 872, 0.060 m on the Paper ball |
| 870 (858) | 0 | 4 | 0.290 m | - | 0.014-0.021 m | tick 872, 0.060 m |
| 870 (858) | 1024 | 4 | 0.299 m | - | 0.023-0.035 m | tick 872, 0.060 m |

The points the two movable policies re-create (on the prop bodies; the own
ball had only its floor contact at both rewinds, kept alive through them)
never help: at the earlier anchor they cost the Paper ball 12 cm and the
plates a centimetre, at the later one they change nothing on the Paper ball
and cost the plates a centimetre - the same lesson as copying the recorded
fields, that a point made at the restore is not the point IVP would have
had, and the default stays at 768 for the second reason too. And the
residual's shape says what it is not: the box is 5.7 cm off at the first
checkpoint after the replay under every policy, the Paper ball matches the
control at its rewind tick and is 6 cm off two ticks later - a 4 m/s
difference appearing in one live tick, i.e. an impact that lands in one
world and not the other, not a contact point missing from the restore.
Which is the next thing to read from a trace, and the one the piles share.

### What the trace shows: a restore keeps IVP's live collision state

`--trace-body P_Ball_Paper_MF --trace-ticks 856 880` on the headless client
logs the prop ball's core (movement state, velocities, queued changes), its
contacts, the friction pairs it is in (the easing countdown, the integrated
anti-energy, the time since the pair's last impact) and its exact mindists
(status, length, IVP's collision-distance selector and decrease counter,
the angular hull time) after every live tick, before every replayed tick and
after the rewind; `scratchpad`'s `trace_diff.py` lines the rewound run up
against the traced control tick by tick. Read across the rewind at 870
(anchor 858):

* **Before the rewind the two runs are identical** to the last digit, in
  every field.
* **At the anchor, the live world had two contact points on `Platte04`** -
  one just made by an impact in that very tick (gap 0.0100 m, pressure 0,
  pull 0), one established (gap 0.01077 m, pressure 8.03) - and one exact
  mindist to it (0.0100 m); its contact with `Platte05` had ended.
* **The restored anchor still has the live tick's contacts**: one point on
  `Platte05` (gap 0.0225 m, pull -4, pressure 7.29 - the values of tick
  870) and none on `Platte04`; its exact mindists are the live ones too,
  `Platte04` at 0.0456 m, the distance of tick 870, not 0.0100. A beam -
  with or without IVP's collision recheck - moves the core and leaves the
  friction contact points and the mindists' lengths where they were; the
  recheck rebuilds which pairs exist, not what they hold, and the lengths
  are recomputed only at the next PSI.
* **So the first replayed step already differs** (0.03 m/s), the impacts
  of ticks 865-868 land differently, and after the rewind the ball is
  0.36 m/s off - the 6 cm two ticks later.

Three ways to put the collision state right, all from outside IVP, all
measured on the same probe (`rc_*`, `pn_*` runs against the same control;
the baseline is the narrow default, 768):

| what the restore does about it | anchor 858: `P_Ball_Paper_MF` | anchor 838: Paper / box |
|---|---|---|
| nothing beyond the narrow default (768) | 0.290 m | 0.048 / 0.058 m |
| every pose written first, then every moved body rechecked (`--two-pass-recheck`, bridge API v16 `body_recheck`) | 0.290 m | 0.155 / 0.058 m |
| the live contacts the record does not name deleted first, the recorded ones re-created fresh (`--contact-flags 2048`, PRUNE) | 0.278 m | 0.143 / 0.058 m |
| the same, with the recorded fields copied onto the new points (2079) | 0.288 m | 0.178 / 0.058 m |

With PRUNE the restored anchor has the right partner - one point on
`Platte04`, the `Platte05` point gone - and still not the live anchor's
state: one point where the live world had two (the second came from an
impact *in* the anchor tick, on a ledge that has no exact mindist any
more, so IVP's creation path cannot make it), the pair's "time since last
impact" at zero, the mindist lengths still the live tick's until the next
PSI. The first replayed step differs by the same 0.03 m/s, and the
sequence ends 0.28 m off instead of 0.29. The box is 5.7 cm off at the
first checkpoint under every row: whatever it is, it is not in anything a
contact record can carry.

So the pose + velocity + queue + freeze + contacts record is not a
savestate of IVP's collision state for a body in multi-body contact, and
the pieces it lacks - which ledge pairs have points, points made by the
anchor tick's own impacts, per-pair easing and anti-energy, the mindists'
lengths and distance selectors - are IVP's internal structures, writable
only by re-running the impacts that made them. Every variant stays as an
A/B switch; the default is unchanged, and the prop residual (5-30 cm on a
free prop, 1-2 cm on the plates, bounded by the next snapshot) is accepted
as the floor of a rewind through contacts between movable bodies.

PRUNE on the four-client lane piles (snapshots off, two runs, the same
measure as the table above) closes the case the hard way: one run had the
host's ball 3.4 / 2.9 m from the server's through the start pile - the best
host figure of any policy - and the other 12.6 / 9.9 m, the usual; the
other three balls sat where they sit under every policy (2.2-6.8 m). And
the second run tore `P_Modul_03`'s walls off their floor on two clients
(walls 245-1196 m out, the mechanism mean 1.9-21.6 m): the walls rest on
the floor asleep, their contacts with it are not in the ball's record, and
deleting a contact wakes its cores (`delete_friction_distance` resets the
freeze check, `unlink_contact_points` revives), so a stack that slept
through the whole control came apart at the first rewind that reached it.
A contact set cannot be pruned per body: the record names the partners a
body touched, not the partners every other body may keep.

### The start pile: a coincident spawn that no rewind can reproduce

The ball the others spawn into was 11-17 m from the server's through the
start pile under every contact policy. Taken apart with the same four-client
lane drive, snapshots off:

* **At zero latency it is still there.** Three runs: the host's own ball
  2.5-3.3 m from the server's over ticks 0-1199 (2.5522 m twice - it is
  deterministic), client1's own ball **0.001 m for 800 ticks** (it touched
  nobody), the other two 2-3 m; 1100-2100 ticks re-simulated in the whole
  minute against 20000 at 150 ms. So it is not the prediction window, and the
  two restore changes it suggested (below) could not have been the fix.
* **Two players at zero latency:** the host's own ball 0.38 / 0.16 m, the
  other client's own ball 0.0000 m - and that client sees the host's ball
  exactly as the server does (0.0000 m). The divergence is in the host's
  own world only.
* **Tick by tick** (`ct768_l0_*`): the four balls are born at one tick, at
  one point, the reset point, and kicked apart by the deterministic spawn
  impulse. From its first step the server's host ball drifts -z 0.004 m a
  tick, the host's own copy +0.002: a 0.4 m/s difference out of the
  pile's resolution in the birth tick itself. At tick 229 the server's ball,
  on its way -z, touches ball 1467 (1.14 m apart: one diameter) and takes a
  0.4 m sideways push; the host's, on its way +z, never does. The 3.5 m
  lateral offset that leaves persists until the lane steer and the
  checkpoint's lifecycle snapshot (tick ~1300) bring it back. At 150 ms the
  same, plus the births arriving 20 ticks late: the first birth replay knocks
  the host's ball 0.8 m back at tick 252 and it runs 11-17 m behind until
  the checkpoint snapshot at tick ~900.

Two things the replay does differently from the server at that tick were
fixed and measured:

| change | 150 ms, host's own ball 0-599 / 600-1199 | 0 ms |
|---|---|---|
| none | 11.0 / 6.7 m, 11.0 / 8.8 m | 3.25 / 2.02 m |
| a body born in the window re-posed to its end-of-tick record *after* its birth tick's step (the old rule) → to its pre-step birth state, kick queued, *before* the step, so the birth tick is re-simulated with every body born into it | 13.5 / 10.9 m | 2.55 / 2.81 m |
| a late remote birth created live and beamed back → created inside the replay at its stamped tick, in the world as it stood then (`pending_births_`, `drain_pending_births` in `pre_step`) | 11.6 / 10.2 m | 2.55 / 3.37 m |
| both, with every birth note in use (a diagnostic line per birth confirmed it) | - | **a ball 109-414 m out, client1's own ball 186 m off** |

The last row is the answer. IVP leaves a pair created coincident alone (a
zero contact normal, no event: `player_collision_filter`'s comment, design
9.10) and the kicks take the balls apart; a pair *beamed* back on top of
each other and stepped is separated by the impact solver's rescue push,
with whatever normal the pair's bookkeeping had. Beaming four balls into
coincidence is not creating four balls coincident, so a rewind across the
spawn tick cannot re-simulate it, and the own ball of the client whose
world the server differs from is the one that pays, because its own ball is
the one body the level script created (the clones are made by events, live
or in a replay) and the four worlds - the server's and the three clients' -
resolve the same coincident pile in the order each one's object set and
creation history gives IVP. The birth re-pose was removed again; the
in-replay creation of a late birth stays (neutral, and the right place: a
body created live and beamed back carries collision bookkeeping built
against the live world).

What bounds it: with the server's snapshots on, as deployed, the
corrections hold every own ball within 0.01-0.05 m of the server's through
the pile (the interval sweep above); snapshot-free, the host is metres off
until the first lifecycle snapshot, the checkpoint. What would remove it
is a spawn where no two balls are born inside each other - a deterministic
lateral offset by join order of at least one diameter (1.2 units), applied
by the owner before its spawn kick and carried in its Physicalize pose, or
a kick strong enough to clear a diameter in the first tick. The spawn ring
was replaced by the coincident spawn plus impulse on purpose (design 9.10),
so that is a design call - made next, and measured.

### Spawn spacing: balls born a diameter apart (protocol 4)

`spawn_spacing` (server config, map units, default 0 = the design 9.10
layout) travels in SessionStart next to the impulse; with it, every member's
ball is born in a cell of its own around the resetpoint - join order 0 at
the resetpoint itself, the rest spiralling out on the resetpoint's lateral
and forward axes in whole multiples of the spacing
(`session/spawn_impulse.hpp`, `kSpawnOffsetTable`, 16 cells). The owner
moves its own ball before anything else sees it - the headless client beams
the body it finds at the resetpoint, the retail client moves the entity in
OnPhysicalize before the block creates the body - and reports the offset
pose in its Physicalize event, so the server and every mirror create the
clone where the ball is; nothing else needs the spacing, and the kick is
unchanged. A ball is 4 units across, so 4.2 keeps same-tick spawns, and
respawns at a shared checkpoint, from being born inside each other.

Measured on the four-client lane drive, snapshots off, spacing 4.2 against
0 (the runs above):

| latency | spacing | host's own ball 0-599 / 600-1199 | the other three, 0-599 | server replay |
|---|---|---|---|---|
| 0 ms | 0 | 2.5-3.3 m / 2.0-3.9 m (three runs) | 0.00-3.0 m | bit-exact |
| 0 ms | 4.2 | **0.64 / 1.03 m** | 0.002-0.68 m | bit-exact |
| 150 ms | 0 | 10.6-13.5 m / 8.9-10.9 m (four runs) | 1.9-7.3 m | bit-exact |
| 150 ms | 4.2 | **0.95 / 0.58 m** | 2.0-4.3 m | bit-exact |

Two more pairs (`ct768_l0_s42_*`, `ct768_l150_s42_*`) hold it: at 0 ms the
host's ball 0.33 / 1.39 m and 0.40 / 2.77 m, at 150 ms 1.69 / 1.22 m and
1.52 / 1.24 m - against 10.6-13.5 m in every run without the spacing - and
the balls nobody touches stay at 0.000-0.002 m. The other three balls
improve less (2-5 m through the start pile at 150 ms, 0.5-2 m at 0 ms,
against 2-7 m and 0.5-3 m): what remains of the pile is ordinary ball-ball
contact under a 20-tick input prediction, the floor the prop probe
measured, and no longer the birth. The server's journal replayed bit-exact
in all six runs; no crash. One of the six (the first at 150 ms) had a
different incident on one client: the hinged plate row of `P_Modul_29`,
far behind the balls, came apart after the sector-3 checkpoint's lifecycle
rewind at tick 1271 (plates 0.7 m off in the replayed window, 50-130 m
within 100 ticks, 10 km by the end) right after that rewind re-created two
floor contacts for `Platte09`; it did not recur in five runs with the same
settings and never appeared in the fifteen earlier lane runs, so it is
noted as an open item (a sleeping hinge chain restored with a re-created
contact), not attributed to the spacing.

So `spawn_spacing` defaults to 4.2 on the server (`config_manager.hpp`;
0 restores the design 9.10 layout exactly, and a solo session always gets
0). The server's own check that a spawn Physicalize lands near a resetpoint
(2.5 units) flagged every offset spawn as suspicious in those six runs -
informational, nothing rejected - and now reaches 2.3 spacings further when
a spacing is set, the farthest of the 16 cells being sqrt(5) spacings out.
The retail client applies the same offset in its OnPhysicalize and
compiles, but has not been run on the arena with it.

## Without snapshots: what the input stream alone sustains

The design never let the server depend on a snapshot - its world is the
inputs, simulated from the anchor - but the clients did: every other tick the
server sends the authoritative poses, and a client that disagrees past the
tolerance rolls back to it. That is the fallback the "lockstep + prediction +
rollback" story wanted to make unnecessary, and three things decide whether it
is:

* **The inputs reach the server.** A client sends its frame unreliably with no
  delay, carrying the last `MAX_INPUT_FRAMES` (8) frames of its history, so a
  lost packet is covered by the next one. A frame that still has not arrived
  when its tick is simulated is not waited for: the server applies the last
  frame it had (`timeline.hpp`, `input_buffer::take`), journals what it
  applied, and the late frame is counted stale and never applied. So a loss
  burst longer than eight frames costs the player's intent for those ticks,
  never the consistency of the world: what the server applied is what the
  session is.
* **The applied inputs reach every other client, losslessly.** The server
  relays, once per tick, every other member's applied frame in one message,
  **reliable** (`server.cpp`, `on_session_inputs`): a redundancy window would
  have multiplied an O(N²) stream, and reliable delivery costs nothing in the
  steady state and only latency under loss - which the prediction and the
  rollback window absorb. Being in the same ordered stream as the lifecycle
  events, a client never sees an event before the inputs of the ticks that
  preceded it. A frame that differs from the one the client predicted with
  triggers the rewind ("input corrected at tick T") that re-simulates from
  there with the truth.
* **The replay is faithful**, which the preceding sections measured piece by
  piece: the restore bit-exact for a quiet window (v11-v13), the scripts
  re-executed (v14), a contact a beam cannot carry re-created (v15), the
  activation tick of every sector an input (the sector union).

**Measured** (four headless clients at 150 ms each way, 60 s, the server's
snapshot interval 2 / 66 as deployed against 100000 / 100000 - none in the run
but the ~25 full snapshots that lifecycle events force; `--loss-pct 10` on the
third run):

| run, 4 clients, 60 s | snapshots received | rollbacks per client | own ball vs server, last 900 ticks (mean) | mechanisms, last 900 ticks (mean) |
|---|---|---|---|---|
| swing bay, snapshots on | 1767 | 353 / 144 / 365 / 370 | 0.010 / 0.007 m | 0.23 / 0.17 m |
| swing bay, snapshots off | 25-26 (forced by lifecycle events) | 12 / 14 / 12 / 15 | 0.03 / 0.14 m, 0.70 m in the window after | 0.16 / 0.15 m |
| lane-kept drive, on | ~1700 | 326 / 449 / 477 / 232 | 0.03 / 0.01 m | 0.07 / 0.16 m |
| lane-kept drive, off | 17 | 11 / 10 / 7 / 8 | 0.03 / **2.66 m** | **1.42 / 0.006 m** |
| swing bay, off, 10 % loss | 18 | 5 / 4 / 8 / 10, `far` 5 / 9 | **120-150 m** | 0.2-0.6 m |

(`rollbacks` here are snapshot corrections; the input-corrected rewinds that
do the work without snapshots are `rb` on the status line: 708 / 939 in the
swing bay, 501 / 639 in the lane run, every one of them replayed - `far=0`,
`unmatched=0` - with 3000-3500 relayed input frames received per client.)

So the two things that had to be true are true - no input frame is ever lost
to the session, and every relayed frame arrives and is replayed - and the
third is not yet: **the replay does not converge on its own under multi-ball
play.** With no snapshot to pull it back, seven to nine hundred input-corrected
rewinds a minute, each re-simulating a pile of four balls, leave the own ball
0.03-0.14 m off in the swing bay and up to 2.7 m off in the lane run, and the
level-end balloon chain 1.4 m off there - exactly the two residuals this
document has not closed: contacts between movable bodies are not restored (the
narrow v15 default), and the sector's setup window is not replayable. Under
10 % loss the reliable relay arrives late enough that corrections fall past the
rollback window (`far` 5-9): a correction that cannot be replayed is applied
as a pose write without re-simulation, and the world comes apart by hundreds of
metres. A snapshot is the only repair for that, so the fallback stays. What
snapshots cost is also visible: with the deployed interval 2 the swing-bay
clients take 350 corrections a minute and re-simulate 20-25 thousand ticks in
60 s - most of them on the balloon chain (`PE_Balloon_Ballon02` 106-212 per
client), which a row restores without its queue and extras and so disagrees
again two ticks later.

**The snapshot interval, swept.** The same four-client swing bay (150 ms,
60 s), a full snapshot every 66 ticks throughout, the interval between the
incremental ones 2 (the row above), 8, 22 and 66. The server's journal
replayed bit-exact in all four runs (3258-3352 ticks, no checkpoint mismatch),
every client ended with `hard=0`, `resyncs=0`, and the input-corrected rewinds
(`rb` less the corrections) stayed at 400-800 per client whatever the
interval - they are the relayed inputs' doing, not the snapshots':

| interval | snapshots per client | corrections per client (on balls + on mechanisms) | ticks re-simulated | own ball vs server, last 900 ticks | mechanisms, last 900 ticks |
|---|---|---|---|---|---|
| 2 | 1767-1793 | 353 / 144 / 365 / 370 (102+251 / 126+18 / 103+262 / 88+282) | 19-25 k | 0.010 / 0.013 m | 0.23 / 0.17 m |
| 8 | 450-460 | 84 / 33 / 104 / 101 (28+56 / 26+7 / 36+68 / 28+73) | 10-15 k | 0.0003 / 0.0008 m | 0.34 / 0.24 m |
| 22 | 197-201 | 39 / 18 / 46 / 53 (12+27 / 15+3 / 15+31 / 16+37) | 10-15 k | 0.014 / 0.009 m | 0.49 / 0.45 m |
| 66 | 109-112 | 24 / 15 / 31 / 33 (8+16 / 13+2 / 11+20 / 11+22) | 11-15 k | 0.053 / 0.015 m | 1.15 / 0.46 m |

(own ball and mechanisms: `scripts/server_client_drift.py` on clients 0 and 3,
the window 2700-3599 where the balloon chain is lifting; the mechanism
corrections are `scripts/classify_rollbacks.py`'s count, 70-80 % of them on
`PE_Balloon_Ballon02..04` at every interval.)

Three things the sweep settles. Corrections fall with the snapshots, a little
faster than in proportion - a correction needs a snapshot to disagree with,
and a chain just corrected disagrees again at the next one. The re-simulation
halves from interval 2 to 8 and then stops falling: the floor, 10-15 thousand
ticks a minute per client, is the input-corrected rewinds, which no interval
touches. And the own ball is as close to the server's at 66 as at 2 - it is
the replay that keeps it there, not the snapshots. What the snapshots buy is
the mechanisms: the level-end balloon chain, 0.2 m off at interval 2, is 1.1 m
off at 66, because a chain that has come apart from the server's is pulled
back only when a snapshot arrives and stays wrong until then. So the interval
trades a client's CPU for the mechanisms' fidelity in the seconds a player is
watching them, and 2 stays deployed: its cost is ten thousand re-simulated
ticks a minute more than 8 would take, its gain is the balloons where the
server has them. (8 is the compromise if that CPU ever matters: the same own
ball, a quarter of the corrections, half the re-simulation, 0.1 m more on the
chain.) What would move the trade is a correction that holds: the restore
puts every body the snapshot carries back together, yet the chain disagrees
again within two ticks (149 of client3's 212 balloon corrections at interval 2
came exactly two ticks apart), so whatever drives it apart is not in the rows
- which is the same open item as the piles below, state between movable
bodies that the record does not carry.

Where the own ball loses the server without snapshots is instructive. In the
lane run, client2's own ball sits **within a millimetre** of the server's from
tick 1500 to 2400 - nine hundred ticks of driving with three other balls on the
lane and no authoritative pose in sight - and is metres off in two places: the
start (ticks 300-900, 3.4-5.4 m), where the four kicked spawns pile up, and
after tick 2400, where all four die at the far end within 200 ticks and respawn
at x = 700 almost together, kicked into each other again. The replay sustains a
field of balls strung out along a lane; it does not sustain a pile - the
contacts between movable bodies that the v15 restore deliberately leaves to
IVP, and the spawn kicks that start every pile.

**Solo, the clean statement** (one client, 150 ms, the lane-kept drive, 60 s;
with no other ball there is no prediction, so the client's world and the
server's are the same input stream on the same ticks and every difference is
the replay's own):

| solo, 60 s at 150 ms | snapshots received | corrections | rewinds | own ball vs server | mechanisms vs server |
|---|---|---|---|---|---|
| snapshots on (2 / 66) | 1558 | 0 | 0 | **0.0000 m at all 1531 checkpoints** | 0.0000 m |
| snapshots off | 11 (forced by the spawn, the death, the respawn) | 0 | 0 | **0.0000 m at all 1560 checkpoints** | 0.0000 m |

Bit for bit, through the whole drive, the far-end death and the respawn at
x = 700, with nothing arriving from the server but the relayed clock. That is
the deterministic-lockstep core working as designed: one input stream, two
machines, one world. Everything the four-client table loses is lost in the
prediction of the other players and in what the replay cannot yet put back
when their balls pile into ours.

## Checked on the real networked path, and two things found there

The solo probe is where the fix was measured, so the 4-player 300 ms run was the
check that it had not broken anything. It found two faults, neither of them the
fix - and the way the first one hid is worth recording.

### A printf, and three runs spent blaming the wrong thing

The commit that added the impulse restore also added an `imp=` argument to the
status line's `printf` **without its format specifier**, so every argument after
it shifted: `max_err=%.4f` consumed the integer and `last=%s` consumed a double
as a `char*`. Four clients, every networked run, dead at around tick 400 with
`0xC0000005`.

The solo runs could not have caught it. There `rs.max_error` is `0.0`, which
`%s` reads as a null pointer and MSVC prints as `(null)`; only a run with a real
error value turns it into a wild pointer. So a change measured entirely on the
solo probe shipped a crash that the solo probe was structurally incapable of
seeing.

And the harness could not say where it was, because `BallanceMMOSessionClient`
never installed the crash reporter the SimTool has had since the reset
heisenbug - all it could report was a bare exit code. It is installed now, and
the first trace named `logf` -> `session_client::report` immediately.

One further trap on the way: the first two "pre-v11 controls" crashed
identically, which looked like proof the crash was not the fix's. It was not
proof of anything - the switches gated the *write*, so the new capture still ran
on every body every tick. The callbacks are now not installed at all when
neither half is wanted, which makes `--no-restore-impulses --no-restore-quats` a
true control.

### An intermittent journal-replay divergence at tick 701, which is NOT the fix

With the crash gone, the 4-player run sometimes fails its journal replay:
`matched=689 first_divergence=701`, with `checkpoint_mismatches=0`. The tick is
the same every time it happens, so it is one specific event rather than noise.

It is not the fix, and that took the right control to establish:

| arm | runs | result |
|---|---|---|
| both halves on | 3 | FAIL, PASS, FAIL |
| impulses only | 1 | PASS |
| quaternions only | 1 | PASS |
| **both halves off (pre-v11)** | 2+ | PASS, **FAIL with the identical 689/701** |

A true pre-v11 control reproduces it, at the same tick, with the same counts. So
it is a pre-existing intermittent nondeterminism in the server's own session,
exposed by this configuration and not by the fix. Worth noting that a single
passing control run was nearly enough to pin it on the fix - an intermittent
failure needs a base rate, not one comparison.

Still open, and recorded here rather than guessed at.

## How to measure this without fooling yourself

Two methodological traps cost real time here, and both are cheap to avoid.

**Run the control in the same batch as the arm.** Two plain runs from
*different* builds disagree by 8.74 m mean / 99.3 m max - as large as any effect
measured - while two plain runs from the same build, back to back, agree to
**0.0006 m max**. The harness is deterministic; the comparison is not, unless
control and arm come from one batch. Always run the control twice and print the
floor.

**Do not read a saturated metric.** The session mean over all ticks is pinned by
one bistable prop: `P_Ball_Paper_MF` either stays on the map or does not, and
once that resolves, `p90` and `max` come out at the same two numbers
(107.3526 / 177.6555 m) in arms that have nothing else in common. Measure in an
early tick window, one rewind wide, and report per body - `scripts/worst_bodies.py`
and the `early` window of `scripts/client_divergence.py`. The first difference
at the first rewind is the clean signal; everything after it is Lyapunov growth.


* **A `journal mark` checkpoint is labelled one tick early.** The mark records
  the bodies from inside the automation handler, after the frame's step but
  under the previous tick's number, so a comparison against the server shows a
  fake error at exactly that tick (the sandbag, 0.050 m at 1948, exact at 1946
  and 1950). Read the mark tick out of a comparison.

## Four bugs found on the way, all real

1. **The missing trigger.** The client only rolled back when a snapshot
   disagreed, never when a relayed remote input for an already-simulated tick
   differed from the prediction. That is the GGPO trigger and the whole reason
   snapshots can be demoted.
2. **"No recorded input" was treated as "no opinion".** For a mirror just
   rewound into existence the replay could only feed it the frames that had
   arrived, which for most of the birth window is none - so there was no
   recorded input to compare against, the correction was skipped, and those
   balls could never converge. A missing record is news, not silence.
3. **The lifecycle rewind ran too early.** It was called before
   `attach_remote_navigation` and before the spawn impulse, and a remote only
   enters the tracked set when physicalized **and** navigated - so the replay
   ran with the mirror untracked, undriven and unkicked. The balls ran away
   (2.2 m at birth to 11 m twenty ticks later) instead of converging.
4. **`rewind_to` bumped `stats_.mismatched`.** That counter means "snapshots
   this client disagreed with" and is read as the distance from the server, so
   a caller-requested rewind polluted the one metric being used to judge the
   work, and made a measurement look three times worse than it was.

## Still open

* **Closed by the script replay: a rewind across a new sector's setup.** One
  forced rewind across the sector-2 activation (solo, otherwise exact;
  activation at 855, bodies born at 859-860) used to put the new sector's
  bodies 0.2-0.7 m off, and moving the anchor located it in the window right
  after the activation, where the module scripts are still setting the sector
  up (its hinges among it) and a physics-only replay cannot redo that. Measured
  again on sector 4 (activation at 1814) with one forced rewind of depth 12,
  against the same control:

  | anchor | physics-only replay | with the scripts |
  |---|---|---|
  | 1800 (the window ends before the activation) | exact | exact |
  | 1818 (4 ticks in) | **swing 3.90 m, sandbag 0.48 m, rope 0.21 m**, 3 corrections | **0.0019 m**, none |
  | 1826 (12 ticks in) | exact | exact |
  | 1836 (22 ticks in) | exact | exact |

  The window is the first ten ticks, and what a physics-only replay could not
  redo there is the scripts' own work - the swing's and the sandbag's setup
  chains toggling their forces - which the replay now runs. The freeze
  bookkeeping (v13) had already taken the births' part of it from 0.6 to
  0.02 m.
* **Fixed in be9c559: the retail client recorded only what the registry had
  resolved.** The fix that made the headless client record its whole movable
  world (fd74a59) had never reached `physics_session_client.cpp`, so on the
  retail client a body the server had not named yet - a new sector's, until
  the next full snapshot - was stepped by every replay and restored by none.
  It now records every movable body but the balls, like the headless client.
* After the same forced rewinds, `P_Modul_26_Sack` and `P_Modul_08_Schaukel`
  diverged by 0.95 m and 1.78 m on rewinds that crossed no activation. Those
  were the script-driven mechanisms the replay did not run; it runs them now
  (see "The script replay, as built"), and the swing left the worst-body list.
* **The journal-replay failure at tick 701 is a hash artefact, and it follows
  the join order.** Over 17 four-player runs it failed in all 8 whose players
  joined out of client order (join order 0132 or 0321) and in none of the 9
  that joined 0123, controls and led runs alike. Tick 701 is where all four
  players' beams land: an Unphysicalize and a Physicalize each, every ball's
  body recreated in one tick. The pose hash differs from 701 on, while all 1283
  checkpoints of the failing session agree with the replay to 1e-6 m and
  1e-4 m/s, through 1400 ticks of four balls on hinged plates, where any real
  divergence would have grown far past that. So the physics is identical and
  only the hash is not: `capture_world_hash` folds the cores into one FNV
  stream in `m_MovableObjects` order, which the recreations evidently leave
  different between the live world and the replay. The surface signature next
  to it is already order-independent, a sum of per-body hashes. Doing the same
  for the cores would fix the replay check, but it changes every hash value,
  in journals and at the anchor exchange, so it is a decision, not a fix in
  passing.

  A temporary probe on a fresh 0132 run showed the live server and the replay
  with identical state and an identical order-independent hash through tick
  702 while the `m_MovableObjects` list order already differed at 701 - which
  looked like a pure hash artefact. It was not.

  **With the fold order-independent, the failure moved to tick 716 and
  stayed.** Not the hash: the balls' tilt quaternions really differ from 716
  on, by about 1e-9, with positions bit-identical and every checkpoint inside
  its tolerance. Probing both sides inside tick 701 (the bridge's event log
  plus temporary dumps of each ball's hull list and overlap-tree partners):
  identical creation events, identical insertion-time state, and then the
  fourth ball's two contacts - with the balls at 348 and 342, both 3 units
  away - created in opposite orders on the two sides, because its overlap
  partners had been gathered in opposite orders. And the decisive test: the
  Linux replay tool replays the Linux journal exactly, and the same tool built
  with one line of the Windows variant reproduces the Windows divergence on
  Linux to the hash. The line is `ivp_int_floor`/`ivp_int_ceil` in
  `ivp_clustering_longrange.cxx`: `floorf` of the box edge on Windows,
  `floor` of the same double under `LINUX`, which file an object under a
  different grid box when an edge lands on a grid line. Engine change #16
  (docs/engine-changes.md) makes every platform use the float variant the
  retail game uses; the order-independent fold stays, because a hash of a
  list in list order was never a hash of the world.

  **Fixed: the fold is order-independent now** (`world_hash.hpp`: one FNV hash
  per core, multiplied by an odd constant and summed, the way the surface
  signature always was). Every hash value changed with it, and nothing reads
  the old ones: the journal format is version 2, the tick-record format
  version 3, the bridge API version 12, and a physics session carries a
  protocol version (`session::PHYSICS_PROTOCOL_VERSION`, 2) in `SessionStart`
  and `SessionReady` that both sides require to match. The server's minimum
  client version moved with it, so a client from before this cannot log in at
  all. Deliberately no compatibility path: an old and a new build must never
  share a session or compare journals.

  Verified on the real path: old journals are refused by the replay tool and
  the Python reader; a pre-change client gets "Outdated client (client:
  4.0.0-alpha1; minimum: 4.0.0-alpha2)" at login; a new client on a
  pre-change server logs "SessionStart could not be decoded: the server runs
  a different physics protocol" and leaves.

* The prop ball `P_Ball_Paper_MF` once disappeared from one side's body set
  after tick ~1 270 and stayed missing. Not reproduced in the 2026-10-20 runs:
  in the four-client swing-bay and lane runs it is present on the server and
  on the client for exactly the same checkpoints (913 of 1728, 1213 of 1619),
  and the body sets agree at the last tick (55 and 56 bodies).
* `mechanisms: 14/13` in every run: one dictionary entry never resolves to a
  local body. Harmless here (it is another instance's) but unverified.
* The name-to-index dictionary is delivered **only in full snapshots**
  (`mechanism_registry::note_name`, "a delta row carries no name"). If
  snapshots are to be demoted, that dictionary has to arrive some other way -
  once at session start, or derived from the map on both sides. (With the
  server's snapshot interval set past the run, the 11-26 full snapshots the
  lifecycle events force still carry it, which is why the snapshot-free runs
  above resolved every mechanism.)
* Without snapshots the replay does not converge under multi-ball play: the
  own ball ends 0.03-2.7 m off the server after the balls pile up, and under
  10 % packet loss corrections fall past the rollback window and the world
  comes apart ("Without snapshots" above). Of the two pieces named there,
  the sector setup window is closed (the script replay) and the contacts
  between movable bodies are re-creatable without the crash (the sleeping-
  partner precondition, "Movable partners" above) - and re-creating them
  changes nothing in the piles. What the piles lose is elsewhere: the ball
  the others spawn into is metres off through the start pile under every
  contact policy, while the last joiner is within 2-3 m, which points at the
  prediction around a remote spawn; and the solo prop probe, traced tick by
  tick ("What the trace shows" above), shows what a rewind through contacts
  between movable bodies cannot put back: the collision state IVP builds
  only by running the impacts itself - which ledge pairs hold contact
  points, points made by the anchor tick's own impacts, per-pair easing and
  anti-energy, the mindists' lengths before the next PSI. A recheck pass of
  its own, pruning the live contacts, re-creating the recorded ones with or
  without their fields: all measured, none better than the narrow default,
  and pruning tears sleeping stacks apart. The start pile itself is a
  coincident spawn ("The start pile" above): four balls born at one point
  resolve by IVP's rescue pushes in the order each world's creation history
  gives it, the server's and each client's differently, and a rewind cannot
  re-create "created coincident" (beaming the balls back on top of each
  other explodes them). A ball nobody touches is exact; a touched one is
  metres off until the next lifecycle snapshot, and within 0.05 m with the
  snapshots on as deployed. Removing it is a spawn-layout decision (balls
  born a diameter apart), not a restore fix.

## Not covered

Every run here was **single-sector**, with all four players in one bay, so the
server's sector union equalled each client's own set. A multi-sector room had a
second, independent problem - the server ran the union of the players' sectors
while each client ran only its own, so the clients were not simulating the same
world at all - which "The sector union, as built" settles: every side runs the
union now, and nothing is deactivated anywhere.
