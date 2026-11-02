# Startup confirmation retries (STEX, 2026-11-02)

The startup panel continues showing the current handshake and its countdown.
A missing confirmation moves the start for every member before the old start
instant. This is the user's replacement for the proposed three-stage startup UI
and uncapped catch-up implementation; neither of those changes is included.

Let `R` be the largest member RTT used to schedule this handshake and `n` the
number of postponements so far. From the instant a new start is sent:

- Confirmation deadline: `(R + 160 ms) * 2^n` later.
- Start instant: another `(2R + 60 ms) * 2^n` after that deadline.

Thus even a confirmation arriving exactly at the deadline leaves at least two
maximum RTTs before the scheduled start. The server uses the confirmation's
network arrival timestamp; an answer arriving after the deadline does not
release the old start. The same clock-offset and half-RTT conversion used by the
existing protocol maps the server instant onto each client's clock.

At a missed deadline the server increments the sequence and broadcasts the
later instant to every remaining member. All members must confirm the new
sequence; an old sequence's confirmation cannot count toward it. The previous
in-flight-answer allowance ends at the deadline instead of borrowing time from
the postponement guard. Both the confirmation window and guard double, including
their small fixed margins so LAN retries also back off. A lower RTT sample or a
departing slow member cannot move a postponed start earlier: `R` only increases
during this handshake and resets when the next handshake begins.

The existing three-postponement limit still applies. After that, missing members
are refused and the remaining members receive a fresh scheduled start. The
exponent is bounded by that last round. This finite guard accommodates packet
loss and retransmission; it cannot guarantee delivery during an arbitrary
outage. The existing committed-start/resync recovery and the 33-tick catch-up cap
are unchanged.

## Verification

- Windows x64 and x86 builds passed, including dependency checks.
- All 777 unit tests passed. New cases cover the two-RTT guard, exponential
  windows, repeated lost confirmations, renewed confirmation by every member,
  stale sequence rejection, late-arriving confirmations and decreasing RTTs.
- GCC syntax check of the scheduling tests and the Arch server build passed.
- The four-client `core_ref_s14/swing_snapoff` wire arm matched rRTDF: all send
  traces equal and all four journals identical (protocol-24 capture accepted).
- A 60-second four-client arena session at 150 ms each way, snapshots off,
  blocked client 1's uplink for 1300 ms from its first start notice. The proxy
  dropped 38 uplink packets. Starts 1, 2 and 3 had lead times of approximately
  1138, 2277 and 4555 ms. All four clients started on sequence 3, with measured
  start-instant skew 1.81 ms; each released within 0.55-1.52 ms of its scheduled
  instant. All had zero rebases, resyncs and input starvation. The server replay
  matched 3633/3633 ticks and 1816/1816 checkpoints.
- The subsequent two-client retail demo on Level 11 blocked one uplink for
  1300 ms, dropping 35 packets. Start lead times were 1179, 2358 and 4716 ms;
  both clients started on sequence 3 and released about 2 ms apart. Both
  frozen worlds were unchanged at release; rebases and resyncs remained zero.
  The startup panel was inspected in both game copies.

Local logs are under `.local-tools/work/STEX/`; the live capture is
`maps/arena-test/saxr/stex_loss`; the retail demo is under
`.local-tools/work/STEX/manual-20261102-1`. The full wire matrix and further
loss seeds remain close-out checks under the quick-tier budget. These runs are
startup coverage, not a new claim of mechanism or ball-to-ball collision coverage.

## Test timing and follow-up plan (STTM, 2026-11-02)

The harness previously treated `tick base N assigned` as the start of play. A scheduled assignment actually
keeps the world held, so STEX's longer waits made proxy faults and late joins fire before their named ticks.
The headless client now exposes opt-in `--notify-tick T` milestones after real assigned live frames (0 means
the first frame). The harness follows these, records requested/observed ticks, and fails missing or skipped
targets. The watcher polls every 10 ms; OS scheduling can add latency, so this is not packet-exact fault placement.

Harness `--seconds` now requests play time from the first assigned frame. `--startup-timeout` adds a bounded
startup allowance (180 s by default). The standalone client retains its wall-time `--seconds` and adds
`--play-seconds`; old replay paths need no extra clock reads. Mid-session stalls and resync waits count in play
time. Deliberate Leave/Finish scenarios may end sooner and must prove their named endpoints. Timed late joins
take their remaining play budget from the host's first live milestone, not its process launch.

The initial panel notice is now `start scheduled`; receipt of a proposal cannot establish that everyone has
confirmed. Later proposals still show `new start time`. This changes neither protocol nor handshake behavior.

STTM precedes REFB, which records protocol-26 references with current timing. LB0's remaining harness work,
LB1-LB5 and LBM remain pending; LB1 tests final-start baseline gating and LBM checks that startup waits cause
no lead adjustment. CBF retargets startup faults to the exponential schedule (at R=300 ms the four confirmation
windows total 6.9 s before refusal, excluding polling/transport), keeps long-outage recovery and the 33-tick
residual, and runs the deferred matrix. Stage D inherits milestone/endpoint coverage and records intentional
startup waiting separately from loading and release lateness. T3C's running-time performance target is unchanged.
