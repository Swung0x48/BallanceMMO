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
replayable" (which the whole-frame experiment showed is the wrong shape
anyway), it is **restore IVP's constraint state across a rewind**. The bridge
has a `m_ConstraintObserver` hook for design 9.26 diagnostics but no
capture/restore, so this needs new bridge functions reaching into the IVP
constraint objects - and the first question to answer is whether IVP's hinges
carry integration state at all (accumulated impulses for warm-starting) or are
solved afresh each step, because if they are stateless the residual is
somewhere else again, most likely the contact caches or the freeze timers the
Option A postmortem already names.

### BallanceTAS has nothing to lend here

Checked, since it is the other project that replays Ballance deterministically:
it is behind this one. It says outright that IVP physics determinism is **not**
enforced, has no savestates, and its fixed-timestep layer was removed. The one
transferable idea - validating frame by frame against a reference trace -
already exists here as the journal replay and is stronger, comparing a full
world hash rather than the ball. Useful mainly as confirmation that nobody has
a Virtools savestate to borrow.

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
