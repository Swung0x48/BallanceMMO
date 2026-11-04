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
