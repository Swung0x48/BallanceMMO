# Input relay protocol: acknowledged delta (design, for review)

Status: **design decided by the user (2026-11-03, §7) - not implemented yet.** Plan: `.local-tools/work/RLD/plan.md`.

The user (2026-11-03): "如果要保证20人的物理会话在8Mbps的服务器带宽上正常运行，需要怎么设计？", "要不下一轮直接开始设计新协议吧，你先计划一下，不要直接开始实现", "你现在设计新协议吧". Reliable transport may be used where it does not hold back the unreliable stream (it does not, in GameNetworkingSockets: reliable messages queue among themselves only).

## 1. Why the relay changes

The relay is how a client learns the frame the server applied for every other member at every tick; with snapshots off it is the only thing that corrects a client's prediction of the other balls.

**Holes.** Today (protocol 5) every datagram repeats each member's frames of the last `physics_relay_window` = 8 ticks. A loss burst of 8 or more datagrams leaves a tick whose frame never arrives (a hole). The client drives that tick with a guess, and nothing ever corrects it: every persistent world divergence the LBM matrix found under packet loss is such a hole (`.local-tools/work/LBM/review.md` E, reproduced with replayable captures, `.local-tools/work/LBX`). Two shapes: the member changed input inside the hole, or the tick's live drive was stale (the first tick after a correction rewind) and its correcting frame was the one that never came.

**Bandwidth.** Measured on the 4-player arena (inbound captures, `LBX/capture_sizes.py`): 249 B per relay datagram, ~60 per second per client = ~15 KB/s. Per datagram: header 26 B; per member 5 B (player id 4, count 1) + 8 frames x 4 B + the first frame's camera 36 B. The camera is 43 % of the bytes, the repeated frames 38 %. Scaled to 20 members that is 1413 B per datagram - more than one packet (the unreliable payload limit per packet is 1248 B, `k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend`) - and ~15.5 Mbps of server upload. The target was first 8 Mbps for 20 members; the user then measured the server's real link (2026-11-03): **5.0 Mbps down, 5.2 Mbps up**. With ~55 B of wire overhead per packet, 5.2 Mbps up leaves **~437 B of relay payload per recipient per tick at 20 members** (all of it - nothing left for events or a resync burst), and the uplink of 20 members at today's ~326 B per datagram is **~4.0 Mbps of server download**, close to the 5.0 Mbps the server has.

## 2. What real input looks like (the user's own play, TRIAL1 round 1, 2 x 9 minutes)

- Frame changes (keys, flags, ball type): 1.4-1.5 per second per player; median gap 10 ticks; at most 10 changes in any 32 ticks, 26 in any 128 (`RLD/key_stats.py`).
- The camera basis the ball's force uses takes **3 exact values** in a whole session (0 deg, 90 deg, and the level start), bit-identical across both clients and across sessions, changing ~10 times in 9 minutes, each a one-tick jump; pitch is always 0 (`RLD/camera_stats.py`). The relay re-sends it 36 bytes at a time 60 times a second.

## 3. Options and their cost

Server upload for N members, every member receiving one datagram per tick (`RLD/cost_model.py`; payload per recipient per tick, then the session's total with ~55 B of wire overhead per packet). The delta columns assume a 300 ms round trip (20 ticks: a change is repeated until its acknowledgement returns); the outage columns assume every recipient is recovering at once (an upper bound - in practice only the one that lost packets is).

| N | window 8 (today) | window 32 | window 8 + camera id | delta, steady | delta, every member at peak change density | delta, after a 0.5 s outage | after 2 s |
|---|---|---|---|---|---|---|---|
| 4 | 245 B, 0.6 Mbps | 533 B, 1.2 | 140 B, 0.4 | 43 B, 0.2 | 130 B, 0.4 | 54 B, 0.2 | 88 B, 0.3 |
| 8 | 537 B, 2.5 | 1209 B, 5.3 | 292 B, 1.5 | 60 B, 0.5 | 263 B, 1.3 | 86 B, 0.6 | 165 B, 0.9 |
| 12 | 829 B, 5.6 | 1885 B, 12.3 | 444 B, 3.2 | 77 B, 0.8 | 396 B, 2.9 | 118 B, 1.1 | 242 B, 1.9 |
| 20 | 1413 B, 15.5 | 3237 B, 34.8 | 748 B, 8.5 | 111 B, 1.8 | 662 B, 7.6 | 182 B, 2.5 | 396 B, 4.8 |

- A bigger window both costs more and still leaves holes past its length.
- The camera id alone (window 8 kept) halves the bytes but leaves the holes and stays just over 8 Mbps at 20.
- The acknowledged delta fits 20 members with room, and has no holes at all inside the server's history (§4.3).
- A reliable gap-fill lane beside today's window would fix the holes but not the bandwidth, and adds a request/answer round trip.

**Recommended: the acknowledged delta with a camera dictionary (§4).**

## 4. The proposed protocol

### 4.1 The acknowledgement (uplink)

Every uplink datagram (`session_input_msg`) gains `relay_ack` (u32): the newest tick T such that the client holds every member's relayed frame for every tick <= T (contiguous coverage; ticks before the client's base count as held). The server keeps `acked[recipient]` = the highest `relay_ack` seen in the recipient's current numbering (never backwards; reset to the base on a re-anchor, like `held_notice.acked` today). The uplink already arrives every tick with redundancy, so a lost acknowledgement costs only a repeat.

### 4.2 The relay datagram (downlink)

Per recipient per stepped tick, as today; header as today (session, tick = newest, acked, held_total, epoch, wait) plus `covers_from` (u32): the datagram describes every member's frames for every tick in (covers_from, covers_to], covers_from = acked[recipient] normally.

Per member with anything to say:

| field | bytes | meaning |
|---|---|---|
| slot | 1 | the member's session slot (the join order), not the 4-byte id |
| flags | 1 | BASE: a full base frame follows (the member is new to this recipient since covers_from, or covers_from predates its presence); PRESENT_FROM: the member's frames start at a later tick (it joined/was born inside the span) |
| base frame | 0 or 4 | keys, flags, ball type, camera id - only with BASE |
| n changes | 1 | |
| changes | 5 each | tick offset back from covers_to (u8), keys, flags, ball type, camera id |

A member whose frame did not change in the span is omitted; a 3-byte presence bitmask (for up to 24 slots, wider if the session is larger) says which members the span covers at all, so an omitted member reads "unchanged since covers_from". The client expands the changes from its own copy of the member's frame at covers_from into one frame per tick - exactly the frames today's window would have delivered.

**Camera dictionary.** A camera id names an exact camera basis (the three float rows, bit for bit). The first datagrams that use a new id carry its definition (id + 36 B) inline until the recipient's acknowledgement passes the tick it was first used; the client keeps the dictionary per member for the session (cleared on a resync, which re-sends what is in use). With the measured 2-3 bases per session this is ~0 bytes in steady play, and exact: the client gets the same floats the server applied. (Deriving the camera from the keys and the initial state, the user's question, is possible in principle but needs a bit-exact copy of the game's camera logic on every side; the dictionary gives the same saving without that risk.)

### 4.3 Budget, ordering and the history

- The server keeps each member's applied frames for the last H ticks (H = 128 ~ 1.9 s; the deepest rollback reach is 120 by design 2.5, so a frame older than H is useless to a rewind anyway).
- The budget is a rate, not just a packet: per recipient per tick, min(1100 B, the configured server upload x 0.8 / members / 66 - the wire overhead) - `physics.relay_upload_kbps` (default 5200, the measured link) with 20 % left for events, notices and resync bursts: ~340 B at 20 members. Steady play needs ~110 B (section 3); every member at peak change density at once (~660 B) is the case the ordering below absorbs.
- If the datagram for (covers_from, newest] fits the budget, covers_to = newest. If not (a recipient recovering from a long outage), covers_to is the largest tick whose datagram fits - oldest first, the same span for every member, so coverage stays contiguous and every datagram that arrives moves the client's acknowledgement forward. The newest frames reach that client a little later while it catches up; no other recipient is affected.
- **No holes inside H:** any tick the client lacks is after its acknowledgement, and every datagram covers from the acknowledgement. A loss of any length shorter than H is filled by the first datagram that gets through.
- **Past H** (the recipient's acknowledgement is older than newest - H, i.e. an outage of ~2 s): the server cannot cover it. It sends `covers_from` = newest - H with a GAP flag; the client then needs the existing resync (its rewind could not reach that far either). The client then resyncs (the user's Q2).

### 4.4 The uplink frames

The uplink today sends the last 8 frames, each with its 36-byte camera (~320 B per datagram; 20 members = ~3.4 Mbps of server download). Frames past the deadline are substituted and noticed (protocol 7), so the uplink needs no acknowledged scheme; with the camera id and a run-length of repeats it drops to ~30-40 B. (The user's Q3: compressed.) With the measured 5.0 Mbps of server download this is needed, not optional: 20 members today send ~4.0 Mbps, compressed ~1 Mbps.

### 4.5 What stays as it is

Held notices (protocol 7) and lifecycle events riding the datagram (protocol 25), the late-player meter and the Sector bound (they read uplink arrival times and the newest uplink frame, both unchanged), the rollback and its rewind reach, resync, snapshots (off), GameNetworkingSockets settings (default send rate 256 KB/s per connection is enough: 20 members steady ~11 KB/s per recipient). No reliable lane is needed: everything rides the unreliable datagram and is repeated until acknowledged.

### 4.6 As built (first cut, branch relay-delta)

- The member field stays the 4-byte player id (the 1-byte slot is not wired yet): ~10 B per unchanged member instead of ~7,
  ~2.9 Mbps of server upload at 20 members steady instead of ~2.1.
- The rate budget is not a deferral yet: every tick's relay goes out whole, split into one-packet parts (part/parts in the
  header; the recipient holds a span once it has every part). Only every member at peak change density at once exceeds it.
- Measured (2026-11-04, 4 clients, 150 ms, 10 % loss in bursts of 8 on every link, seeds 0/1/2, snapshots off): every confirmed
  FINAL row of every client equal to the server's (protocol 28: 1.1-81 m divergences in the same cells), no resync, server
  journals replay exactly; relay datagram mean 71 B (protocol 28: 248 B).

### 4.7 Camera headings (protocol 30, the user's decision of 2026-11-04)

The retail camera only turns by 90 degrees about the vertical axis, but its script computes each turn with sines and
cosines: one heading arrives in up to 5 bit patterns (1.3e-5 apart at most; census of 767 journals, 1.5 M own frames:
none further than that from an exact quarter turn; zero rows only before the first camera, never with a key down). So:

- **The owner snaps** (`session/camera_heading.hpp` `heading_snap`, client_core_frame.cpp where the observed rows are
  stored): rows within 1e-3 of a quarter turn of its first camera (the base) become that turn exactly; a quarter turn
  swaps two components and negates one, exact on every host. Anything else (another orientation, say a map whose
  checkpoint faces 45 degrees off) becomes the base. The game's own camera and Cam_OrientRef are untouched (what the
  player sees); the snapped rows are what every world drives the ball with - the frame, and the own ball's navigation
  (`own_camera_rows`, fed from the same stored rows; the retail force blocks stay zeroed). The force direction differs
  from the game's by 1.3e-5 rad at most.
- **On the wire** a frame's camera is a 3-bit heading code in the top bits of the ball_type byte: 0-3 quarter turns of
  the stream's base in force at the frame's tick, 4 the zero rows, 7 inline (the rows follow and are the base from that
  tick on). Lossless whatever the rows: a frame no turn reproduces bit for bit goes inline (`camera_bases::encode`).
  A relay run is 4 B (was 5), an uplink frame 3 B (was 4, and the first frame of every datagram no longer carries 36 B).
- **The base reaches the receiver** attached to a message whose first coded frame turns it (since u32 + rows, 40 B,
  flagged in the top bit of the uplink's count / the entry's nruns), while the receiver may lack it:
  - uplink: until the relay header's `acked` (the newest of our frames the server applied) reaches the base's tick -
    the first datagram the server got with a frame that late was sent before the client knew, so it carried the base;
  - relay: per recipient and player, until the recipient acknowledges the first datagram that carried the base (inline
    or attached); an entry with no coded frame carries the base in force at its end (the next turn's). A re-anchor
    forgets what was carried; a leaver's bases are dropped (a returning member starts inline).
  Both ends keep the bases by tick (`camera_bases`), so a reordered older datagram decodes against the base of its
  own ticks. A message turning a base its receiver does not hold is not used (the relay: its span is not held, so it
  comes again with the base).
- The relay's camera dictionary is gone. Journals record rows, as before: no journal version.
- Measured (2026-11-04): headless A0 and the loss seeds 0/1/2 (10 %, bursts of 8, every link) - every confirmed FINAL
  row of every client exact, replays N/N, no resync; relay datagram mean 64-68 B (protocol 29: 71), uplink 42 B for 8
  frames (86). Wire check against core_ref_s15 (relay datagrams bridged to the protocol-30 layout): with the snap
  switched off for the experiment, all 45 journals identical to protocol 29's replays and the sends differ only in
  SessionInput's encoding and SessionReady's protocol - the codec is lossless. With the snap, the level-8 and fan arms
  differ only in a few own INPUT records (every TICK hash equal); the arena arms' own ball diverges from its first
  force, since the headless arena camera wanders up to 2e-5 off its first sample and the capture's server applied the
  unsnapped rows - a capture replay cannot agree with a live world that never existed, by construction.

### 4.8 Leavers, stalled recipients and body-set snapshots (protocol 31, 2026-11-04)

Found by the 20-player bandwidth runs (BW20, nftables meter on the server's port):

- **A leaver's last frames.** The ring relayed only players present at the newest tick, so when a member left, its
  frames still in flight (sent, not yet acknowledged) never reached anyone: every other client drove the leaver's ball
  with a guess for those ticks and marked every later FINAL row UNCONFIRMED (LBF3's sticky rule; 19 of 20 clients).
  Now every run of consecutive ticks after the recipient's acknowledgement is relayed, an entry may end before the
  datagram's tick (`back`, in an entry extension: nruns 0, then flags, nruns, back; the base flag moved there too, so
  an entry of 128 runs no longer reads as base-flagged), and a leaver's frames go until each recipient acknowledged
  them. The ticks a leaver stays in the world after it left are still driven with its last relayed frame on every
  side and not relayed (session_runner `leaving`); its ball is gone by then. Measured: the 20-player run with a member
  leaving early has no UNCONFIRMED row and no relay hole but those; a 4-player run with a leave at 1800 is exact.
- **Past the history.** A recipient whose acknowledgement is older than the history must resync whatever it holds
  (its own frames were substituted for longer than any rewind reaches, the others' it lacks are gone), so it gets
  header-only notices (from = oldest - 1) 1, 2, 4 .. 64 ticks apart instead of the whole history every tick; the first
  one a live client receives starts its resync, a dead one costs a small datagram a second. Measured: 3 s downlink
  and uplink blackouts each end in one resync, FINAL exact.
- **Body-set snapshots.** With snapshots off the full snapshot of a body-set change is taken for its mechanism names
  only, and only the level-reset guard still needs a name; it now goes only to a member without names yet. 20 players
  with deaths and respawns: server upload 6-11.8 Mbps -> 3.0-4.4 Mbps.
- The headless test client closes its connection when its run ends, before its wind-down (it used to linger ~2 s,
  which looked like 20 stalled clients: a 7.9 Mbps burst at every run's end).

### 4.9 Lifecycle events ride the acknowledged relay (protocol 33, 2026-11-04)

**The gap (LBM5, swing seed 2, both twins).** A birth stamped tick 231 reached client 3 at its tick 307 (on twin; 310
off twin): 76 ticks after its stamp, past the reach of 74, so the client resynced. The client's input frames of the
same ticks all arrived in time. The two downlink paths differ:

- A relayed frame rides every relay datagram until the recipient's acknowledgement passes it (§4.2), so the first
  datagram after a loss burst brings it.
- A lifecycle event (and a SessionMember, and the rejected-event echo) goes once reliably and then as six unreliable
  copies two ticks apart (lifecycle.hpp kLifecycleEventRepeats/kLifecycleEventRepeatSpacing, server.cpp
  `event_repeats`): twelve ticks of cover. At 10 % loss in bursts of 8 datagrams the twelve ticks can all be lost,
  and the reliable copy then comes by GNS's retransmit, 0.4 s or more with its backoff. At session start the relay
  lag is already 42 ticks, so the event lands past the reach.

**The proposal.** Every message `event_repeats` carries today goes, to a relay recipient, inside its relay datagrams:
in every one from the first that carries it until the recipient's acknowledgement reaches that first datagram's tick
(the camera base's rule, §4.7: any datagram the recipient applied from then on carried it, so by then it holds it).
The reliable copy stays (the backstop, and the order for a recipient that applies it first); the unreliable repeats
stay only for a member that is not a relay recipient yet (before its first assignment). The client already takes the
first copy of an event and drops the rest (`event_seen`, SessionMember copies), so a third path needs no new dedupe.

- **Wire (downlink): a compact events section** (the user's decision of 2026-11-04: the raw messages would ride
  every datagram of a round trip, at the start 19 births of 140 B each). Of a Physicalize's 140 B, 66 are the ball's
  recipe (its physics parameters and collision shape, one per ball type in a level) and 48 its pose (position and the
  rotation rows - at the start every member's is the resetpoint's); what differs per event is ~13 B. So the section
  after the held one is three tables:
  - recipes: n u8, per recipe id u8 and the recipe as write_recipe writes it. A session-wide dictionary: the server
    numbers each distinct recipe on its first use, and attaches it to a recipient until the recipient's
    acknowledgement reaches the first datagram that carried it (the camera base's rule, §4.7); a reanchor
    re-attaches. The client keeps id -> recipe for the session.
  - poses: n u8, per pose position f32x3 and rotation f32x9 - each distinct pose of this datagram's events once.
  - events: n u8, per event a head u8 (bits 0-2 the type, 7 = a raw message; bits 3-5 the state; bit 6 an origin
    byte follows), player u32, life u16, tick as i16 from the datagram's tick, the origin byte (tick - origin_tick,
    when it differs and fits), then by type: Physicalize ball_type u8, flags u8, pose index u8, recipe id u8;
    Unphysicalize and Finish nothing; Sector i32; Trafo ball_type u8, flags u8, position f32x3. A raw message (code
    7: len u16 and the bytes as sent reliably) carries every other kind - SessionMember, BodyRevived,
    AirshipRelease - and any event the compact form cannot (an origin over 255 back, a 256th recipe).
  Every float is copied bit for bit (a quaternion would change the rotation's bits, and with them the body every
  world makes). The decoder rebuilds the session_event_msg the reliable copy carries, so the handlers and the dedupe
  see one message whichever way it came. A datagram of a split tick carries the section in its first part with room;
  a section past one packet goes into datagrams of its own, as the held section does.
- **Server.** relay_ring keeps per recipient the pending messages with the tick of the first datagram that carried
  each; build_datagrams adds them; acknowledge drops those whose first carry the acknowledgement reached; reanchor (a
  resync, a late join) drops those at or before the anchor (the state holds them) and marks the rest uncarried;
  forget drops the recipient's. The three push sites of `event_repeats` push here for relay recipients.
- **Client.** The events section is unpacked and each message handed to the handler a reliable copy goes to
  (on_session_event, on_session_member, the rejected echo), before the datagram's entries - an event the server took
  before a tick's frames is applied before them, as the reliable stream gives today.
- **Uplink, the same rule.** A client's own Physicalize/Unphysicalize/Sector/Trafo already rides every input datagram
  (protocol 25, event_piggyback.hpp), but only until its stamp + 7. It rides until the server's header `acked` reaches
  the stamp instead (the server takes an event before the frames of the datagram carrying it, so a frame at the stamp
  acknowledged means the event was taken). Finish and BodyRevived keep their repeats.

**Cost.** A message is carried for about one round trip: RTT 40 ms -> 3-4 copies, 150 ms each way -> ~20 copies
(today the reliable one and 6 at any RTT). Compact sizes: a Physicalize 13 B plus its pose (48 B, shared in a
datagram) and, until acknowledged, its recipe (67 B, once a session); an Unphysicalize 9 B, a Sector 13 B, a Trafo
23 B (raw today: 140, 24, 28, 38). The start, every member born at one tick: 19 births = 3 + 67 + 48 + 19 x 13 = ~365 B
per recipient per datagram (~300 B once the recipe is acknowledged), one packet with the relay's entries, for a
round trip - at RTT 40 ms ~1.5 KB per recipient (today's raw repeats 7 x 2.7 KB = ~19 KB). A respawn elsewhere: 61 B.
If bursts still measure too large, an explicit event acknowledgement (a per-recipient sequence number and a u16 in
every uplink) would let copies be spaced and capped per datagram, at a field each way.

**Verification.** Unit tests (the ring's carry, acknowledge, reanchor and forget of messages; the section's round
trip and split). Headless: the swing seeds 0-2 twice (no late-event resync), the A6 lifecycle arms (A6b, A6m, death,
superseded, drop) and A8, all FINAL exact with journal replay N/N; a downlink outage of 20 ticks over a birth (today
a resync, then on time after the outage); BW20 driving, server upload against 3.0-4.4 Mbps; retail L11.

**Implementation steps.** (1) session/event_codec.hpp: the three tables, encode and decode, bit-exact round trips of
every type and the raw escape, every truncation refused. (2) relay_ring: the pending messages and the recipe carry
per recipient (build_datagrams, acknowledge, reanchor, forget). (3) session_remote_input_msg: the section (protocol
33, the held section no longer last). (4) Server: the three push sites go to the ring for relay recipients; the
repeats stay for a member not yet a recipient. (5) Client: the section decoded before the entries, the recipe
dictionary per session, cleared at a reanchor. (6) Uplink: the own events ride until the header's `acked` reaches the
stamp (raw, as today: one own event at a time). (7) Docs, then the verification above; the wire check's sends differ
in SessionRemoteInput and SessionInput by construction.

**As built (protocol 33, branch event-relay, 2026-11-04).** session/relay_events.hpp (relay_event, the recipe
numbering, the compact form's sizes and rebuild), input_relay.hpp (per recipient: the pending messages and the
recipes it may lack, each with the tick of the first datagram of its current carry - left out of a datagram by the
budget or a header-only notice, the carry starts again; acknowledge drops what the acknowledgement reached; a
reanchor starts every carry again and keeps the recipes the recipient holds), session_remote_input_msg (the
section), server.cpp `send_copies` (members through the ring, anyone else through the old repeats), the client's
`deliver_relayed_events` (the recipes into the session's dictionary, then every message to its reliable copy's
handler; while not running nothing is taken and the datagram's span is not acknowledged), event_piggyback (own events
until the header's `acked` reaches max(stamp, the carry's first newest frame), 128 ticks at most). 794 tests.

Measured (headless twins on the worktree build, RLD Arch server):
- The gap: the old build at 20 % loss in bursts of 8 resynced once on a late lifecycle event (seed 0, as LBM5 seed 2
  did at 10 %); the new build never did in 8 loss runs (10 % x 5 with seed 2 twice, 20 % x 3); its events never
  arrived later than the frames of their datagrams. All FINAL exact, replay N/N; 11 twin arms (A0, swing 0-2, A6b,
  A6m death/superseded/drop, A6m, A8) exact.
- A pair of events split at the server's intake (1 tick) is pre-existing: 2 of 70 pairs on the old build, 3 of 71 on
  the new; A6m superseded's +1 relay tick is timing noise (idle interleave: new 3/3 pass, old 2/3).
- Bandwidth, 20 players driving (server upload): start burst 4.75 -> 3.52 Mbps (2-s sample), window mean 3.41 ->
  3.04 Mbps, per player download 171 -> 155 kbit/s.
- Retail L11 (both flags on, the worktree x86 build): own/mirror/body 784/1000/46385 compared, 0 different, 0
  missing, replay 2568/2568.

### 4.10 Hints: the frames queued at the server (protocol 37, 2026-11-09)

**Why.** The server steps tick T when the last member's frame for T is in, and relays the applied frames after the
step. Every client runs the same schedule shifted by its own latency, so the last frame to arrive is the highest-ping
member's: a relayed frame reaches a client about the room's worst round trip (plus ~4 ticks) after that client
stepped T, whoever sent it. Measured on the public test's Level 6 recording (13 players, K = 20, each client at its
own public-test ping, 0-114 ms round trip, the coordinator's virtual clock): every client's rewinds are 11.5-11.7
ticks deep (p50 11-12, p90 15), the one at 0 ms like the one at 114 ms; 23 rewinds a second, 4 replayed ticks per
live tick. The same with every latency at 0: 1 tick deep. A member made late mid-session (+150 ms each way) adds only
~2 ticks for the others (patience stops the wait), and an input delay of 40 ticks instead of 7 changes nothing: the
depth is the worst round trip, not the delay and not a slow member. Meanwhile the frames of the other twelve sit in
the server's buffers for ticks.

**What.** Every relay datagram also says what the other members' frames already queued at the server hold for the
ticks after its newest: per member, the keys and flags of the consecutive ticks from the cursor on that had arrived
by the step instant (the take's own cutoff, so a replayed inbound capture builds the same hints), at most 64 ticks,
run-length coded (4 + 1 + 1 + 3 B per run), and only for a member with some queued tick that differs from the frame
just applied for it - the rest hold-last already predicts. They ride the datagram's spare room, never open one, and
are not acknowledged: the next tick's datagram carries what is queued then. A member whose cursor a lead reset or a
re-anchor holds ahead gets none. The server's world and journal do not change; `physics.input_hints` (default on since
the retail smoke below; off until then) switches the section, and without it a datagram is byte for byte protocol 36's.

**The client.** A hint is a prediction, the applied frame stays the only truth: nothing is journalled or counted as
relayed. Per mirror, a map of hinted ticks; past the newest relayed frame the mirror is driven with that frame and
the hinted keys and flags (the hint of the tick, else the newest hint before it), in the live step (`live_drive`)
and in every replay (`recorded_input_at`) - one rule, `hinted_frame`. A hint for a tick already stepped on another
frame asks for the input replay from that tick, by `note_input_correction`'s rule (reach, body, birth tick); out of
reach it is only counted. When the applied frame arrives the hints up to it go; equal to what the tick ran on, no
replay follows. So a key change costs one rewind as before, as deep as the sender's own trip instead of the room's
worst. The camera is not hinted (a turn is still corrected by the applied frame).

**As built.** `session/input_relay.hpp` (input_hint, hint_entry, relay_ring::set_hints and the placement in
build_datagrams, hint_store / hints_confirmed / hinted_frame), `timeline.hpp` input_buffer::queued,
session_remote_input_msg (the trailing section), session_runner (on_inputs gets the queued frames), server.cpp,
the client core (take_hints, note_hint; status field ` hints=news/rewinds/wrong/late/bytes` once one arrived),
arena_map_test.py `--input-hints`, lbm_fast.sh `HARNESS=`. 835 tests.

**Measured** (the recording above, one build, the option off / on, 6270 ticks): depth mean 11.5 -> 5.6 (p50 11 -> 5,
p90 15 -> 11: the high-ping members' own key changes stay late), replayed ticks per live tick 3.97 -> 1.92 (-52 %),
rewinds 22.8 -> 22.6 a second; the two server journals byte-identical, replayed 6248/6248, every client's FINAL rows
exact; per client ~15 k hinted ticks taken, ~2.3 k replays asked by hints, 0 contradicted by the applied frame, 0
out of reach. Fast LBM with the option on: 38/38 twins FINAL exact, criteria equal to pt16 arm by arm (uniform
latency: few hints); again with the four clients at 15 / 30 / 45 / 70 ms one way: 38/38 FINAL exact, 17.6 k hinted
ticks, 1.6 k replays, 0 contradicted, 0 out of reach. The whole recording (33224 ticks, every link 15 ms longer
each way so the coordinator runs near real time: round trips 30-144 ms): depth 13.8 -> 8.7 (p50 14 -> 8), replayed
ticks per live tick 4.60 -> 2.86 (-38 %), server journals byte-identical, 0 contradicted, 0 out of reach, no resync;
the hint sections cost each recipient 1.3-1.4 KB/s (650-690 KB in 505 s; the status field's fifth number).
Time, the 6270-tick pair run alone, one after the other (headless x64, medians over the 13 clients): a rewind
10.2 -> 6.7 ms (restore 1.95 the same, replay 8.2 -> 4.8), rewind work 234 -> 152 ms per second, frame p99
16.2 -> 14.2, p999 24.5 -> 18.8 ms. (A recipe with a 0 ms link runs at 0.2x on the coordinator;
with the smallest latency at 15 ms the same arms run at 6x.)

**Retail, and the default (2026-11-10).** Two retail x86 copies on the Arch server (protocol 38), T3C S0's driver,
RT1's L11 recipe at human key rates (`ctl_h`), E-cores. Both copies at 150 ms one way: FINAL rows exact (own,
mirror, 186 mechanism rows), no resync, 1-2 hints a copy (uniform latency). Copy 1 at 150 ms and copy 2 at 20 ms one
way, the option off / on (one run each, the copies' SessionTelemetry, `.local-tools/work/ARCH/a6`): copy 1 replayed
1418 -> 394 ticks (22.2 -> 6.2 a rewind, 64 rewinds either way), frame interval p99 21.7 -> 16.8 and p999 45.1 ->
32.3 ms, frames over 33 ms 4 -> 0 a minute, the core's work p99 17.5 -> 9.2 ms; 1163 hinted ticks, 0 contradicted, 0
out of reach; copy 2 unchanged (24.7 / 24.9 a rewind: copy 1's frames reach the server late, nothing is queued to
hint). Both FINAL exact in both runs. So `physics.input_hints` is on by default from here (config_manager,
arena_map_test.py `--input-hints`); a config.yml a protocol 37-38 server already loaded has `input_hints: false`
written into it (a missing key is written with the default) and keeps it until edited.

## 5. Interactions to handle

- **Numbering and re-anchors.** The acknowledgement is in the recipient's numbering; a resync, rejoin or late join resets `acked` to the base (as `held_notice` does); the camera dictionary and the per-member base frames are re-sent after it.
- **Members appearing and leaving inside a span** (a late join, a birth, a death, a leave): PRESENT_FROM and the presence bitmask; a leaver is not relayed (as today).
- **Q-SLOT (a)** and the drive of the next tick are unchanged; the user's narrow exception (re-choose the first live tick's drive after a correction rewind) lands in the LBM fixes before this.
- **Journals and captures.** The client's journal keeps one relayed-input record per (player, tick) as today; the capture format is unchanged (it stores raw messages); old captures do not replay on the new protocol (protocol bump).
- **Retail and headless** share the session core (decoder, ring, dictionary); the server (`server.cpp` relay send, `input_relay.hpp` `relay_ring`) gets the encoder and the per-recipient state.
- **The wire check** changes by construction (a new message layout): its expectation is rebuilt, every difference attributed to the relay.

## 6. Verification

- Unit: encoder/decoder round trip; any loss pattern shorter than H reconstructs every frame exactly; over-budget ordering keeps coverage contiguous; camera dictionary definitions until acknowledged; members appearing/leaving inside a span; re-anchor resets.
- Live (headless, snapshots off): loss sweeps of burst length 1..120 datagrams on every link - zero holes, every FINAL row exact, no resync below H; the LBM loss cells (A4, A4 swing seeds 0/1/2) exact; relay bytes per tick measured against the model at N = 4.
- At N = 20: 20 full headless clients are too heavy for this machine; a light relay-only client (joins a physics session, acknowledges relays, simulates nothing) measures the server's real upload at N = 20 (to build, part of the implementation).
- Retail session (two copies) and the wire check.

## 7. The user's decisions (2026-11-03)

- **Q1 - the scheme:** the acknowledged delta with the camera dictionary ("按确认差量 + 相机字典").
- **Q2 - beyond the history:** resync automatically ("自动重同步").
- **Q3 - the uplink:** compressed too - camera id + repeats ("一起压缩").
- **Q4 - H:** 128 ticks, ~1.9 s ("128 tick（约 1.9 秒）").

## 8. Implementation outline

1. Shared encoder/decoder + camera dictionary + unit tests (`input_relay.hpp`, `session_remote_input_msg.hpp`).
2. Server: per-recipient `acked`, member history H, the budgeted span (`server.cpp`); uplink `relay_ack` (`session_input_msg.hpp`); protocol bump.
3. Clients (headless and retail via the shared core): acknowledgement, decoding into the ring, the GAP path.
4. Uplink compression (Q3).
5. Light relay-only client for the N = 20 measurement; loss sweeps; LBM loss cells; retail session; wire check.
