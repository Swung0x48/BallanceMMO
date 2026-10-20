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

* **The sector union across players.** A client still runs only the sectors
  *it* enters. The server runs every player's, so a trailing player's world
  lacks the leader's sector until it crosses too (see "Not covered").
* **Rollbacks while a new sector's bodies are untracked.** See "Still open":
  measured at 0.2-0.6 m, and the next thing to fix.

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

* **A new sector's bodies are untracked until the server's dictionary
  arrives**, and a rollback in that window leaves them live and steps them
  again. The client records a mechanism only once `mechanism_registry` has
  resolved it, and the registry learns names only from full snapshots, which
  reach the client an input delay plus a trip after the birth. Until then a
  restore has no record of the bodies ("no opinion"), so the replay steps them
  another `depth` times. One forced rewind across the sector-2 activation
  (`--force-rollback 880:40`, solo, otherwise exact) put every sector-2 body
  0.2-0.6 m off on the next tick, caught only by the next full snapshot. The
  item below about the dictionary is the cause; a client that runs a sector
  activation itself knows those bodies before the server can name them, so it
  can record them from their birth.
* After the same forced rewinds, `P_Modul_26_Sack` and `P_Modul_08_Schaukel`
  diverged by 0.95 m and 1.78 m on rewinds that crossed no activation. Those
  are the script-driven mechanisms the replay does not run (see "So the
  remaining work is exactly: run the scripts during the replay").
* The server's journal replay failed at `matched=689 first_divergence=701` in
  6 of this round's 11 4-player runs, at the same tick every time: 5 of the 9
  led runs and neither of the 2 lead-0 controls. A pre-v11 control failed the
  same way in the earlier round, so it predates this work, but two controls
  are not a base rate. The solo crossing runs all replayed exactly. It is the
  other thing to fix next.

* The prop ball `P_Ball_Paper_MF` disappears from one side's body set after
  tick ~1 270 and stays missing for the rest of the session. A real
  asymmetry, not yet explained.
* `mechanisms: 14/13` in every run: one dictionary entry never resolves to a
  local body. Harmless here (it is another instance's) but unverified.
* The name-to-index dictionary is delivered **only in full snapshots**
  (`mechanism_registry::note_name`, "a delta row carries no name"). If
  snapshots are to be demoted, that dictionary has to arrive some other way -
  once at session start, or derived from the map on both sides.
* `mism` has a floor of 6-8 per client even at zero latency with snapshots
  off, which nothing above explains.

## Not covered

Every run here was **single-sector**, with all four players in one bay, so the
server's sector union equalled each client's own set. A multi-sector room has a
second, independent problem: the server runs the union of the players' sectors
while each client runs only its own, so the clients are not simulating the same
world at all. That has to be settled before any of this generalises — but it is
not on the critical path, because a single-sector room already diverges at the
birth tick.
