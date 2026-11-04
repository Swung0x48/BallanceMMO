"""Local physics-server test on a custom map (the BMMO Mechanism Arena).

Runs, per sector: a local BallanceMMOServer with physics enabled and N headless
BallanceMMOSessionClients that create a room on the map's level slot, ready up
and play a physics session, driven by generated input records.  Each run is
then checked four ways:

  * every client exited 0 and reported no hard corrections, no resyncs and no
    rollback mismatches it could not resolve,
  * the server's own journal replays bit-for-bit in BallanceMMOSimTool,
  * the mechanisms of that sector were all tracked (`mechanisms=N/N` with a
    non-zero second number - zero corrections on its own is NOT a pass, it is
    also what a client with no local mechanism bodies reports),
  * the run actually produced the interactions it is meant to test: balls
    against balls, balls against mechanisms, and mechanisms against the loose
    bodies parked on them.

The drive records are staggered on purpose.  Every player rolls down the same
lane, but each starts later than the last and the leader brakes periodically,
so the field concertinas and the balls repeatedly run into each other - that is
what produces peer-ball prediction work, which is the part of the session code
a single ball never exercises.

`--start-sector` is what makes the mechanism coverage complete: Ballance only
runs the mechanisms of the sector the ball is in, so a map whose mechanisms are
spread over four sectors needs one session per sector.

The late-player additions (docs/late-player-design.md 5.1, H1-H8):

  --link N:lat=,loss=,burst=,jitter=   client N's own link (a proxy per client)
  --event N:T:KIND=MS[:dur=MS][:dir=up|down|both][:until=T2]
                                       an episode on client N's link at its tick T:
                                       blackout (drop), spike (+MS for dur), step
                                       (+MS from T, until T2 or for good), stall
                                       (a CPU stall, the client's --stall-ms) or
                                       none (a control: only the phases)
  --event-client/--event-tick/--blackout-ms/--spike-ms/--step-ms/--event-dir/
  --event-ms/--event-until             the same, one event, in the design's spelling
  --client-arg-for N:"..."             arguments for client N alone
  --join-late N:T                      client N starts at tick T of the host
  --join-late N:cue                    client N is a pre-booted late joiner (step
                                       DX9, RL1-e): it starts with the others,
                                       loads the level and holds it, and sends its
                                       Join when the server's log says every start
                                       member is ready (the client's --join-cue)
  --leave N:T                          client N leaves the room at its tick T, ends
                                       its session and exits (the client's
                                       --leave-room-at: the server removes it at
                                       its next tick, not after a connection
                                       timeout as with --leave-at)
  --rejoin N:T[:MS]                    client N leaves the room at its tick T and
                                       joins it again on the same connection, in
                                       the same tick or MS milliseconds later
                                       (the client's --rejoin-at T[:MS], step
                                       TRL); its second life's journal is
                                       client{N}.rejoin1.bmjr
  --outage N:ANCHOR[+DELAY]:MS[:up|down|both]
                                       (FULL step HXP) a time-shaped outage on client
                                       N's link before or after tick 0: from the
                                       first downlink datagram ANCHOR names (the
                                       --drop-down anchors: an index or sizeA-B),
                                       DELAY ms later (default 0), every datagram
                                       of the direction (default down) is dropped
                                       for MS ms of wall clock; repeatable
  --no-hang-dump / --hang-dump-after S (FULL step HXP) every client process is
                                       watched: one still alive S s (default 20)
                                       after its log's "exit code N" line is
                                       minidumped (client<i>.hang.dmp, symbolized
                                       into client<i>.hang.dmp.txt by sym_dump.py),
                                       ended, and a FAIL line says so
  --server-log PATH                    an external server's log, copied and read
  --control DIR                        a control run (repeatable): the band the
                                       criteria compare with
  --arm NAME                           which arm of 5.2 this is (A0 .. A9c), for
                                       its own criteria; inferred when absent

Events and late joins follow explicit live-tick notifications, never an
assignment or a postponed start. The requested and observed ticks are checked;
the log watcher polls every 10 ms, with additional OS scheduling latency.
--seconds is the play budget after the first assigned frame; --startup-timeout
adds a separate wall-clock allowance. Loss draws stay seeded per proxy port and an
event takes the same draws as the control for every datagram it sees, so a
control and an event run lose the same datagrams up to T_on; after it the
datagrams themselves differ (resends, resyncs), and so do the draws.  A spike
or a step keeps each direction in order: a datagram never overtakes one the
event delayed (jitter alone still reorders).  Every client gets
--window-origin T_on, so its window: lines start a 60-tick window at T_on.

The event report (event_report.txt/json, and in summary.json) splits the run
into B (before T_on), D (T_on .. T_off + 47) and A (after) and gives per
client: the ticks the server substituted (its journal's INPUT records without
the FRESH flag), own/other/mechanism error against the server's checkpoints,
rollbacks by class with their depth, relay lag per source player and
relay_late (the client's window: lines; a window that overlaps D at all is
D's), snapshots, resyncs and lead changes; and per server: TICK gaps and late
events.  The criteria of 5.2 follow it, the others' for every arm and the
arm's own (--arm), each pass, fail, masked or n/a (a counter this revision
does not have yet).  A control's counters are re-cut on this run's phases
from its stored windows, so a longer D is not compared with a shorter one.
A join-late run alone is an episode too: T_on is the joiner's tick base, D
its first four windows.

usage:
  python scripts/arena_map_test.py --bin build-agent/BallanceMMOServer \\
      --root "C:/Users/geekerwan/Desktop/Ballance-Agent" --level 13 \\
      --players 4 --seconds 60 --drive
  python scripts/arena_map_test.py --selftest
"""
import argparse
import collections
import json
import math
import os
import pathlib
import shlex
import random
import re
import shutil
import struct
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
# journal_trace / first_divergence, for the event report: the batch scripts
# run this with python -I, which leaves the script's own directory off the path.
sys.path.insert(0, str(REPO / 'scripts'))
import late_player_harness as lph
FLAGS = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
EXE = '.exe' if sys.platform == 'win32' else ''

import heapq
import select
import socket
import threading


class DelayProxy:
    """A UDP relay that holds every datagram for `latency_ms` before passing it
    on, and drops `loss_pct` of them, so a client can be given a worse link
    than the rest of the field.

    The delay half is lifted from scripts/probe.py, which has used it for the
    same purpose.  The loss half matters for any test of what the session does
    when a message never arrives: loopback does not drop, so an unreliable
    message path passes such a test by accident and proves nothing about a real
    link.  Loss applies per datagram in both directions; `dropped` counts them.
    """

    def __init__(self, listen_port, server_addr, latency_ms, loss_pct=0.0, jitter_ms=0.0, loss_burst=1,
                 loss_seed=0, log_path=None):
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.bind(('127.0.0.1', listen_port))
        self.back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.back.bind(('127.0.0.1', 0))
        self.target = server_addr
        self.client = None
        self.delay = latency_ms / 1000.0
        # Seeded, so two runs of one case drop the same datagrams.
        self.loss = loss_pct / 100.0
        # Jitter: each datagram waits an extra uniform(0, jitter) on top of the
        # delay, so datagrams overtake each other (the relay's reassembly has
        # to land them by tick).  Burst: a drop takes the next loss_burst - 1
        # datagrams with it, so an outage longer than the relay's window
        # happens and the hole path runs.
        self.jitter = jitter_ms / 1000.0
        self.burst = max(1, int(loss_burst))
        # Per direction.  A burst of b started with probability p per datagram
        # drops p*b of every 1 + p*(b-1); p is solved so that is the loss rate.
        self.burst_left = {}
        self.burst_start = self.loss / (self.burst - self.loss * (self.burst - 1)) if self.loss > 0.0 else 0.0
        # --loss-seed (step F3): 0 keeps every earlier run's draws; another
        # value gives the same recipe a different loss pattern.
        self.rng = random.Random(0x10CC + listen_port + (loss_seed << 20))
        # --proxy-log (step F3, the lifecycle-event arrival margin): one line
        # per datagram - time.monotonic() at receipt, direction, size, the
        # verdict (pass / loss / burst / blackout) and, for a passed one, the
        # time it is due out.  Off by default.
        self.log = open(log_path, 'w', encoding='ascii', newline='\n') if log_path else None
        if self.log:
            self.log.write('# t_recv dir size verdict t_due\n')
        self.dropped = 0
        self.passed = 0
        # --drop-down (step SAXE): a deterministic drop on the downlink, not a
        # draw: (anchor, count) - from the downlink datagram the anchor names
        # (('index', n): the n-th downlink datagram, 0-based; ('size', a, b):
        # the first one of a..b bytes), that one and the next count - 1
        # downlink datagrams are dropped, once.  Counted apart from the loss.
        self.drop_down = None
        # --outage (FULL step HXP): Outage objects, each a wall-clock window
        # opened by a downlink anchor; drops counted by each Outage.
        self.outages = []
        self.down_seen = 0
        self.forced_left = 0
        self.forced_dropped = 0
        self.forced_fired = None   # (downlink index, size) of the anchor once it fired
        self.bytes_up = 0      # client -> server, after the loss
        self.bytes_down = 0    # server -> client, after the loss
        # Link events (LinkEvent): armed from the client's log by EventArmer,
        # read here per datagram.  Blackout drops are counted apart from the
        # loss, so `dropped` keeps meaning the random loss.
        self.events = []
        self.index = -1        # the client this proxy serves
        self.blacked = {'up': 0, 'down': 0}
        self.delayed = {'up': 0, 'down': 0}
        # The due time of the last datagram a spike or step delayed, per
        # direction: nothing after it may land before it.  A link whose delay
        # grows is a queue, not a reordering, and a datagram that overtook the
        # spiked ones would deliver the "late" ticks early (the input stream's
        # redundancy window carries them).
        self.event_due = {'up': 0.0, 'down': 0.0}
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def link_effect(self, direction, now):
        """(drop, extra delay in s) the armed events give a datagram that
        moves in `direction` ('up' = client to server) at `now`."""
        drop, extra = False, 0.0
        for ev in self.events:
            if ev.start is None or direction not in ev.dirs or now < ev.start:
                continue
            if ev.kind == 'blackout' and now < ev.start + ev.ms / 1000.0:
                drop = True
            elif ev.kind == 'spike' and now < ev.start + ev.dur_ms / 1000.0:
                extra += ev.ms / 1000.0
            elif ev.kind == 'step' and (ev.end is None or now < ev.end):
                extra += ev.ms / 1000.0
        return drop, extra

    def run(self):
        queue, seq = [], 0
        while self.running:
            due = max(0, queue[0][0] - time.monotonic()) if queue else 0.01
            for sock in select.select([self.front, self.back], [], [], min(0.01, due))[0]:
                try:
                    data, addr = sock.recvfrom(65535)
                except ConnectionResetError:
                    continue      # Windows reports ICMP port-unreachable after an exit
                if sock is self.front:
                    self.client = addr
                    out, dest = self.back, self.target
                elif self.client is not None:
                    out, dest = self.front, self.client
                else:
                    continue
                # A forced drop still walks the loss rule below (its burst
                # count and its rng draws) and only then is dropped, so the
                # seed's loss pattern around it is the one the same seed gives
                # without --drop-down (SAXE review: skipping the draws shifted
                # the pattern, and a burst of 4 became one of 8).
                forced = False
                idx = None
                if sock is self.back:
                    idx = self.down_seen
                    self.down_seen += 1
                if self.outages:
                    o_dir = 'up' if sock is self.front else 'down'
                    o_now = time.monotonic()
                    for o in self.outages:
                        dropped = o.drops(o_dir, idx, len(data), o_now)
                        if self.log and o.just_fired:
                            self.log.write('# outage: %s anchored at downlink datagram %d (%d B) at %.6f\n'
                                           % (o.spec, o.fired[0], o.fired[1], o.fired[2]))
                        if dropped and not forced:
                            forced = True
                            if self.log:
                                self.log.write('%.6f %s %d outage -\n' % (o_now, o_dir, len(data)))
                if sock is self.back and self.drop_down is not None:
                    if self.forced_fired is None and drop_anchor_matches(self.drop_down[0], idx, len(data)):
                        self.forced_fired = (idx, len(data))
                        self.forced_left = self.drop_down[1]
                        if self.log:
                            self.log.write('# drop-down: fired at downlink datagram %d (%d B), dropping %d\n'
                                           % (idx, len(data), self.forced_left))
                    if self.forced_left > 0:
                        self.forced_left -= 1
                        self.forced_dropped += 1
                        if self.log and not forced:
                            self.log.write('%.6f down %d drop -\n' % (time.monotonic(), len(data)))
                        forced = True
                if self.burst_left.get(sock, 0) > 0:
                    self.burst_left[sock] -= 1
                    if forced:
                        continue
                    self.dropped += 1
                    if self.log:
                        self.log.write('%.6f %s %d burst -\n' % (time.monotonic(), 'up' if sock is self.front else 'down', len(data)))
                    continue
                if self.loss > 0.0 and self.rng.random() < self.burst_start:
                    self.burst_left[sock] = self.burst - 1
                    if forced:
                        continue
                    self.dropped += 1
                    if self.log:
                        self.log.write('%.6f %s %d loss -\n' % (time.monotonic(), 'up' if sock is self.front else 'down', len(data)))
                    continue
                # The jitter draw comes before the event's verdict, so a
                # datagram an event drops still takes its draw and the rest of
                # the run draws exactly what the control drew.
                extra = self.rng.random() * self.jitter if self.jitter > 0.0 else 0.0
                if forced:
                    continue
                direction = 'up' if sock is self.front else 'down'
                now = time.monotonic()
                due = None
                if self.events:
                    blackout, added = self.link_effect(direction, now)
                    if blackout:
                        self.blacked[direction] += 1
                        if self.log:
                            self.log.write('%.6f %s %d blackout -\n' % (now, direction, len(data)))
                        continue
                    if added > 0.0:
                        self.delayed[direction] += 1
                        extra += added
                    due = max(now + self.delay + extra, self.event_due[direction])
                    if added > 0.0:
                        self.event_due[direction] = due
                self.passed += 1
                if sock is self.front:
                    self.bytes_up += len(data)
                else:
                    self.bytes_down += len(data)
                seq += 1
                due_at = due if due is not None else now + self.delay + extra
                if self.log:
                    self.log.write('%.6f %s %d pass %.6f\n' % (now, direction, len(data), due_at))
                heapq.heappush(queue, (due_at, seq, out, dest, data))
            while queue and queue[0][0] <= time.monotonic():
                _, _, out, dest, data = heapq.heappop(queue)
                out.sendto(data, dest)

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        self.front.close()
        self.back.close()
        if self.log:
            self.log.close()
            self.log = None


# The session's tick (headless_engine.hpp kTickRate = 66) and the rollback's
# reach (rollback.hpp max_resim_ticks): the event report's phases are cut on them.
TICK_S = 1.0 / 66.0
# lateness.hpp kQuorumLastMargin: patience waits for a late member's frame up
# to this many ticks after the last other member's.
QUORUM_LAST_MARGIN = 4
REACH = 48
# "0.0000 m" as the criteria print it.
ZERO_M = 0.00005

LINK_KEYS = {'lat': 'latency_ms', 'latency': 'latency_ms', 'loss': 'loss_pct',
             'burst': 'loss_burst', 'jitter': 'jitter_ms'}
EVENT_KINDS = ('blackout', 'spike', 'step', 'stall', 'none')
PROXY_KINDS = ('blackout', 'spike', 'step')


def parse_drop_down(spec):
    '''--drop-down 'N:ANCHOR:COUNT' (step SAXE) -> (N, (anchor, COUNT)).
    ANCHOR is a downlink datagram index (0-based, counted by client N's proxy)
    or sizeA / sizeA-B (the first downlink datagram of A..B bytes).'''
    parts = spec.split(':')
    if len(parts) != 3:
        raise ValueError('--drop-down wants N:ANCHOR:COUNT, got %r' % spec)
    index, anchor, count = int(parts[0]), parts[1].strip(), int(parts[2])
    if count < 1:
        raise ValueError('--drop-down count must be >= 1 in %r' % spec)
    if anchor.startswith('size'):
        lo, _, hi = anchor[4:].partition('-')
        return index, (('size', int(lo), int(hi or lo)), count)
    return index, (('index', int(anchor)), count)


def drop_anchor_matches(anchor, idx, size):
    if anchor[0] == 'index':
        return idx == anchor[1]
    return anchor[1] <= size <= anchor[2]


class Outage(object):
    """--outage N:ANCHOR[+DELAY]:MS[:up|down|both] (FULL step HXP): a
    time-shaped outage, which --drop-down (a count of datagrams) and --event
    (keyed to a client tick, so never before tick 0) cannot make.  The anchor
    is the --drop-down one, matched on the downlink: the first downlink
    datagram it names opens the window DELAY ms after its receipt; every
    datagram of the direction(s) received in [open, open + MS) is dropped
    (the anchor itself too when DELAY is 0 and the downlink is one of them).
    One-shot.  Times are the proxy's time.monotonic() at receipt."""

    def __init__(self, anchor, ms, dirs='down', delay_ms=0.0, spec=''):
        self.anchor = anchor
        self.ms = float(ms)
        self.delay_ms = float(delay_ms)
        self.dir_name = dirs
        self.dirs = ('up', 'down') if dirs == 'both' else (dirs,)
        self.spec = spec
        self.fired = None          # (downlink index, size, receipt time) of the anchor
        self.open_at = None
        self.just_fired = False
        self.dropped = {'up': 0, 'down': 0}

    def drops(self, direction, idx, size, now):
        """Whether the datagram moving in `direction` ('up' = client to
        server), the `idx`-th of the downlink (None on the uplink), of `size`
        bytes and received at `now`, is dropped."""
        self.just_fired = False
        if self.fired is None and direction == 'down' and drop_anchor_matches(self.anchor, idx, size):
            self.fired = (idx, size, now)
            self.open_at = now + self.delay_ms / 1000.0
            self.just_fired = True
        if self.open_at is None or direction not in self.dirs:
            return False
        if self.open_at <= now < self.open_at + self.ms / 1000.0:
            self.dropped[direction] += 1
            return True
        return False

    def as_dict(self):
        return {'spec': self.spec, 'fired_at': list(self.fired[:2]) if self.fired else None,
                'dropped_up': self.dropped['up'], 'dropped_down': self.dropped['down']}


def parse_outage(spec):
    """'N:ANCHOR[+DELAY]:MS[:up|down|both]' -> (N, Outage)."""
    parts = spec.split(':')
    if len(parts) not in (3, 4):
        raise ValueError('--outage wants N:ANCHOR[+DELAY]:MS[:up|down|both], got %r' % spec)
    index = int(parts[0])
    anchor_text, _, delay = parts[1].strip().partition('+')
    ms = float(parts[2])
    dirs = parts[3].strip() if len(parts) == 4 else 'down'
    if dirs not in ('up', 'down', 'both'):
        raise ValueError('--outage direction must be up, down or both in %r' % spec)
    if ms <= 0:
        raise ValueError('--outage MS must be > 0 in %r' % spec)
    anchor, _ = parse_drop_down('%d:%s:1' % (index, anchor_text))[1]
    return index, Outage(anchor, ms, dirs, float(delay) if delay else 0.0, spec)


RE_EXIT_LINE = re.compile(rb'(?:^|\n)exit code (-?\d+)')


def write_minidump(pid, path):
    """A minidump of process `pid` (thread stacks, thread info, handles,
    unloaded modules - the flags of SAXH's hang_dump.py) into `path`.
    Returns (ok, last error, bytes written); (False, -1, -1) off Windows."""
    if sys.platform != 'win32':
        return False, -1, -1
    import ctypes
    import ctypes.wintypes as wt
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    dbghelp = ctypes.WinDLL('dbghelp', use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    # Our own child: all access (WithHandleData wants PROCESS_DUP_HANDLE too).
    h = k32.OpenProcess(0x1F0FFF, False, pid)
    if not h:
        return False, ctypes.get_last_error(), -1
    k32.CreateFileW.restype = wt.HANDLE
    k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
    fh = k32.CreateFileW(str(path), 0x40000000, 0, None, 2, 0x80, None)   # GENERIC_WRITE, CREATE_ALWAYS
    if not fh or fh == wt.HANDLE(-1).value:
        err = ctypes.get_last_error()
        k32.CloseHandle(h)
        return False, err, -1
    dbghelp.MiniDumpWriteDump.argtypes = [wt.HANDLE, wt.DWORD, wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.LPVOID,
                                          wt.LPVOID]
    ok = bool(dbghelp.MiniDumpWriteDump(h, pid, fh, 0x1000 | 0x20 | 0x4, None, None, None))
    err = ctypes.get_last_error()
    k32.CloseHandle(fh)
    k32.CloseHandle(h)
    return ok, err, (os.path.getsize(path) if os.path.exists(path) else -1)


class HangWatch(object):
    """FULL step HXP: the process-exit hang (a session client that logged
    'exit code N' and whose process did not end - SAXH hs3's joiner for
    240 s, RC1d's resync_at150_down d15_brake client 1).  Follows client i's
    log; once the exit line is there and the process still lives `after_s`
    seconds later, writes a minidump of it (`dump`), symbolizes it with
    scripts/sym_dump.py into `dump`.txt next to it, then ends the process so
    the run's own checks go on.  The verdict fails the run on it."""

    def __init__(self, index, proc, log_path, dump_path, after_s, symbolize=True):
        self.index, self.proc, self.log_path, self.dump_path = index, proc, log_path, dump_path
        self.after_s = after_s
        self.symbolize = symbolize   # False: the selftest (no symbol lookup for a stand-in)
        self.logged_code = None
        self.hang = None
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        offset, tail = 0, b''
        while self.running and self.logged_code is None:
            try:
                with open(self.log_path, 'rb') as log:
                    log.seek(offset)
                    data = log.read()
            except OSError:
                data = b''
            if data:
                offset += len(data)
                text = tail + data.replace(b'\0', b'')
                m = RE_EXIT_LINE.search(text)
                if m:
                    self.logged_code = int(m.group(1))
                    break
                # From the last line start only: a tail cut mid-line would
                # let RE_EXIT_LINE's '^' take 'exit code N' inside a longer
                # line for the exit line (and dump a live client).
                tail = text[text.rfind(b'\n') + 1:]
            elif self.proc.poll() is not None:
                return            # ended without the line (a crash, a kill): not a hang
            time.sleep(0.2)
        if self.logged_code is None:
            return
        seen = time.monotonic()
        while self.running and self.proc.poll() is None and time.monotonic() - seen < self.after_s:
            time.sleep(0.2)
        if self.proc.poll() is not None or not self.running:
            return
        after = time.monotonic() - seen
        ok, err, size = write_minidump(self.proc.pid, self.dump_path)
        if not ok and self.proc.poll() is not None:
            return                # it ended while the dump was being taken: not a hang
        print('  HANG: client%d (pid %d) still alive %.0f s after its "exit code %d" line: minidump %s (%s, %d bytes)'
              % (self.index, self.proc.pid, after, self.logged_code, self.dump_path,
                 'ok' if ok else 'FAILED %d' % err, size), flush=True)
        self.hang = {'after_s': round(after, 1), 'pid': self.proc.pid, 'logged_exit_code': self.logged_code,
                     'dump': str(self.dump_path) if ok else None, 'dump_error': None if ok else err,
                     'symbolized': None}
        self.proc.terminate()
        if ok and self.symbolize:
            sym = str(self.dump_path) + '.txt'
            try:
                with open(sym, 'wb') as out:
                    # Not -I: the 'minidump' package lives in the user's site-packages.
                    rc = subprocess.run([sys.executable, '-X', 'utf8', str(REPO / 'scripts' / 'sym_dump.py'),
                                         str(self.dump_path)], stdout=out, stderr=subprocess.STDOUT, timeout=300,
                                        creationflags=FLAGS).returncode
                if rc == 0:
                    self.hang['symbolized'] = sym
                else:
                    print('  HANG: client%d symbolizing failed: sym_dump.py exit %d (see %s)' % (self.index, rc, sym),
                          flush=True)
            except (OSError, subprocess.TimeoutExpired) as e:
                print('  HANG: client%d symbolizing failed: %s' % (self.index, e), flush=True)

    def close(self):
        self.running = False
        # A hang being symbolized gets its time (sym_dump's own cap is 300 s).
        self.thread.join(timeout=320 if self.hang else 2)


def parse_link(spec):
    """'N:lat=150,loss=10,burst=8,jitter=3' -> (N, {link fields})."""
    index, _, rest = spec.partition(':')
    out = {}
    for item in rest.split(','):
        if not item.strip():
            continue
        key, _, value = item.partition('=')
        name = LINK_KEYS.get(key.strip())
        if name is None:
            raise ValueError('unknown link field %r in %r' % (key, spec))
        out[name] = int(value) if name == 'loss_burst' else float(value)
    return int(index), out


class LinkEvent(object):
    """One episode on one client's link (or a stall in the client itself).

    `tick` is in the session's numbering, which the client's own ticks share
    once it is assigned; `start`/`end` are the harness's monotonic clock, set
    when the client's log says which tick it is at (EventArmer)."""

    def __init__(self, client, tick, kind, ms=0.0, dur_ms=0.0, direction='both', until=0):
        if kind not in EVENT_KINDS:
            raise ValueError('unknown event kind %r' % kind)
        if direction not in ('up', 'down', 'both'):
            raise ValueError('unknown event direction %r' % direction)
        self.client, self.tick, self.kind = int(client), int(tick), kind
        self.ms, self.dur_ms, self.direction, self.until = float(ms), float(dur_ms), direction, int(until)
        if self.kind == 'spike' and self.dur_ms <= 0.0:
            self.dur_ms = 1000.0
        self.dirs = ('up', 'down') if direction == 'both' else (direction,)
        self.start = None
        self.end = None
        self.armed = []        # (base, monotonic) of every arming, for the report
        self.observed_tick = None
        self.observed_until = None

    @property
    def t_off(self):
        """The last tick the episode itself touches; recovery is the report's concern."""
        if self.kind in ('blackout', 'stall'):
            return self.tick + int(math.ceil(self.ms / 1000.0 / TICK_S))
        if self.kind == 'spike':
            # the last datagram the spike delays leaves at dur and lands ms later
            return self.tick + int(math.ceil((self.dur_ms + self.ms) / 1000.0 / TICK_S))
        if self.kind == 'step':
            return self.until if self.until else self.tick
        return self.tick + int(math.ceil(self.dur_ms / 1000.0 / TICK_S))

    def describe(self):
        text = 'client%d %s' % (self.client, self.kind)
        if self.kind != 'none':
            text += ' %g ms' % self.ms
        if self.kind == 'spike' or (self.kind == 'none' and self.dur_ms):
            text += ' for %g ms' % self.dur_ms
        if self.kind in PROXY_KINDS:
            text += ' ' + self.direction
        text += ' at tick %d' % self.tick
        if self.until:
            text += ' until tick %d' % self.until
        return text

    def as_dict(self):
        return {'client': self.client, 'tick': self.tick, 'kind': self.kind, 'ms': self.ms,
                'dur_ms': self.dur_ms, 'dir': self.direction, 'until': self.until, 't_off': self.t_off,
                'observed_tick': self.observed_tick, 'observed_until': self.observed_until,
                'armed': [{'base': b, 'starts_in_s': round(s - t, 3)} for b, t, s in self.armed]}


def parse_event(spec):
    """'N:T:KIND=MS[:dur=MS][:dir=up|down|both][:until=T2]' -> LinkEvent."""
    parts = spec.split(':')
    if len(parts) < 3:
        raise ValueError('event %r: want N:T:KIND=MS' % spec)
    kind, _, ms = parts[2].partition('=')
    fields = {}
    for part in parts[3:]:
        key, _, value = part.partition('=')
        fields[key.strip()] = value.strip()
    return LinkEvent(int(parts[0]), int(parts[1]), kind.strip(), float(ms or 0),
                     float(fields.get('dur', 0)), fields.get('dir', 'both'), int(fields.get('until', 0)))


RE_BASE = re.compile(r'(?:^tick base (\d+) assigned|^resynced: tick base (\d+))')
RE_TICK_REACHED = re.compile(r'^harness: target (\d+) reached at tick (\d+)$')


class ServerReadyCue(object):
    """Step DX9 (RL1-e): follows the server's log from `offset` (its size when
    the case began) and, at the first "[session N] all K players ready" line,
    creates every cue file - the moment the pre-booted joiners send their Join
    (--join-cue).  The line is the runner's, printed right after the schedule
    started, so a Join it cues comes into the running session (a late join)."""

    MARK = b'players ready, ticking from'

    def __init__(self, path, offset, cue_files, mark=None):
        self.path = path
        self.offset = offset
        self.cue_files = cue_files
        # --join-cue-mark (FULL step SAXH): another server line cues the
        # Join, e.g. the start handshake's first line - a joiner in the
        # handshake window, before the session ticks.
        if mark:
            self.MARK = mark.encode('utf8')
        self.fired_at = None
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        offset, pending = self.offset, b''
        while self.running and self.fired_at is None:
            try:
                with open(self.path, 'rb') as log:
                    log.seek(offset)
                    data = log.read()
            except OSError:
                data = b''
            if data:
                offset += len(data)
                pending += data
                *lines, pending = pending.split(b'\n')
                if any(self.MARK in raw for raw in lines):
                    for f in self.cue_files:
                        pathlib.Path(f).write_text('cue\n', encoding='utf8')
                    self.fired_at = time.monotonic()
                    print('  join cue: the server\'s %r line seen; cue written for %d joiner(s)'
                          % (self.MARK.decode('utf8', 'replace'), len(self.cue_files)), flush=True)
                    return
            time.sleep(0.002)

    def close(self):
        self.running = False


class LogWatch(object):
    """Report requested milestones after the client actually steps a live tick.
    Target 0 marks its first assigned frame. Assignment and start proposals
    deliberately do not move this clock."""

    def __init__(self, path, on_tick, done=None):
        self.path = path
        self.on_tick = on_tick
        # Stops polling once `done()` says nothing is left to arm, so the
        # harness process (whose threads are also the proxies) stays idle.
        self.done = done or (lambda: False)
        self.ticks = {}
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        offset, pending = 0, b''
        while self.running and not self.done():
            try:
                with open(self.path, 'rb') as log:
                    log.seek(offset)
                    data = log.read()
            except OSError:
                data = b''
            if data:
                offset += len(data)
                pending += data
                *lines, pending = pending.split(b'\n')
                now = time.monotonic()
                for raw in lines:
                    m = RE_TICK_REACHED.match(raw.decode('utf8', 'replace').replace('\x00', '').strip())
                    if m:
                        target, actual = map(int, m.groups())
                        if target not in self.ticks:
                            self.ticks[target] = (actual, now)
                            self.on_tick(target, actual, now)
            time.sleep(0.01)

    def close(self):
        self.running = False
        self.thread.join(timeout=1)


def arm_events(events):
    """Arm once at the observed tick; an until tick is observed independently.
    A resync skipping the requested tick is recorded and fails coverage."""
    def on_tick(target, actual, now):
        for ev in events:
            if ev.tick == target and ev.start is None:
                ev.observed_tick = actual
                ev.start = now
                ev.armed.append((actual, now, now))
            if ev.until and ev.until == target and ev.end is None:
                ev.observed_until = actual
                ev.end = now
    return on_tick


def timing_problems(targets, observed):
    """Missing/skipped milestones cannot silently pass a shorter run."""
    problems = []
    for target in sorted(targets):
        actual = observed.get(target)
        if actual is None:
            problems.append('target tick %d never reached' % target)
        elif target != 0 and actual != target:
            problems.append('target tick %d skipped (observed %d)' % (target, actual))
    return problems


DIK_UP, DIK_DOWN, DIK_LEFT, DIK_RIGHT = 200, 208, 203, 205

# the arena's own geometry, from maps/build_arena_map.py.  The driving lane and
# the mechanism row are different lines: the mechanisms stand on the road, so a
# lane through them would just be a wall.
BAY_SPAN = 40.0
PLAY_Y = 15.0
MECH_Y = 43.0
BAYS_PER_SECTOR = 6
N_BAYS = 23


def write_drive_record(path, level, ticks, player, steer=False, stop_tick=0):
    """An input record that rolls the ball down the arena, into the mechanisms.

    Holding "up" walks the ball along the bay centre line, one mechanism after
    another.  Steering is off by default on purpose: a pre-recorded steer has
    no feedback, so any lateral input accumulates and the ball ends up running
    15-20 units off centre, missing the very mechanisms the run is meant to
    hit.

    The per-player stagger is what makes the balls meet.  Player i waits
    120 + 40*i ticks before setting off, and the leader brakes briefly every
    500 ticks, so the field concertinas and the balls run into each other
    repeatedly - without stopping anyone getting down the arena.  Braking hard
    or often does produce collisions, but it also leaves every ball parked in
    the first sector, which is worse: then nothing reaches a mechanism.
    """
    with open(path, 'wb') as rec:
        rec.write(struct.pack('<4sIdi64s32s4x', b'BMRC', 3, 66., level, b'', b''))
        start = 120 + 40 * player
        for tick in range(ticks):
            keys = bytearray(256)
            # stop_tick releases the controls: a ball then coasts to a stop
            # where it is, which is how a mechanism that acts on a *resting*
            # ball gets tested - a ventilator cannot lift a ball that crosses
            # its column at 15 units a second.
            if stop_tick and tick >= stop_tick:
                pass
            elif tick >= start:
                braking = player == 0 and (tick - start) % 500 < 25
                keys[DIK_DOWN if braking else DIK_UP] = 1
                if steer:
                    swing = (tick + player * 97) % 400
                    if swing < 30:
                        keys[DIK_LEFT] = 1
                    elif 200 <= swing < 230:
                        keys[DIK_RIGHT] = 1
            rec.write(keys + bytes(120))


def status_metrics(line):
    """Pull the interesting counters out of a client `status:` line."""
    out = {}
    for key in ('mism', 'hard', 'mech_hard', 'mech_max_err', 'max_err',
                'unmatched', 'starved', 'rebases', 'tick', 'frames', 'resim',
                't_rest', 't_rep', 't_max', 't_cap'):
        m = re.search(r'\b' + key + r'=([0-9.]+)', line)
        if m:
            out[key] = float(m.group(1)) if '.' in m.group(1) else int(m.group(1))
    m = re.search(r'resyncs=(\d+)/(\d+)', line)
    if m:
        out['resyncs'] = int(m.group(1))
    m = re.search(r'snapshots=(\d+)/(\d+)/(\d+)', line)
    if m:
        out['snapshots'] = int(m.group(1))
    # relay=new/dup/parked/late[/covered] lag=max/p99: the relayed frames
    # reassembled, and how late their first arrivals were (the rollback
    # reaches 48 ticks); covered (step RLT) = first arrivals for a tick the
    # exact state this client stands on already contains - not late.
    m = re.search(r'\brelay=(\d+)/(\d+)/(\d+)/(\d+)(?:/(\d+))?', line)
    if m:
        out['relay_new'], out['relay_dup'], out['relay_parked'], out['relay_late'] = (int(g) for g in m.groups()[:4])
        out['relay_covered'] = int(m.group(5) or 0)
    m = re.search(r'\blag=(\d+)/(\d+)', line)
    if m:
        out['relay_lag_max'], out['relay_lag_p99'] = int(m.group(1)), int(m.group(2))
    # input: reports=N starved=S (..%) worst=W lead=L - the server's reports
    # that found no fresh frame from this client, the longest such run in
    # ticks, and how far this client's own tick runs ahead of the snapshots.
    m = re.search(r'\bworst=(\d+) lead=([0-9.]+)', line)
    if m:
        out['worst_starved'], out['lead'] = int(m.group(1)), float(m.group(2))
    # mechanisms=<known>/<resolved>: the second number is the one that means
    # the client really has local mechanism bodies (level-reset postmortem).
    m = re.search(r'mechanisms=(\d+)/(\d+)', line)
    if m:
        out['mechanisms'] = f'{m.group(1)}/{m.group(2)}'
    return out


RE_CORRECTED = re.compile(r'(?:rollback to tick \d+ \(|mismatch at tick \d+ \(local \d+\): )'
                          r'([A-Za-z0-9_]+)')
RE_BODY = re.compile(r'\b(P_[A-Za-z0-9_]+|Ball_[A-Za-z]+(?:_BMMO_\d+)?)\b(?= dp=| off by)')


def interaction_report(case_dir, players):
    """Evidence that the run produced the interactions it is meant to test.

    Everything here is read back out of what the session itself recorded, so a
    quiet run cannot be mistaken for a thorough one."""
    mechanisms, peer_balls, own_balls = set(), set(), set()
    for i in range(players):
        log = case_dir / f'client{i}.log'
        if not log.exists():
            continue
        text = log.read_text(errors='replace')
        for name in RE_BODY.findall(text):
            if name.startswith('P_'):
                mechanisms.add(name)
            elif '_BMMO_' in name:
                peer_balls.add(name)
            else:
                own_balls.add(name)
    return {
        # mechanism bodies the clients predicted and the server corrected:
        # proof the mechanisms were simulated locally, not just received
        'mechanism_bodies_corrected': sorted(mechanisms),
        # peer balls corrected: only happens when a remote ball's predicted
        # path is disturbed, which is what a ball-to-ball collision does
        'peer_balls_corrected': sorted(peer_balls),
        'own_ball_types': sorted(own_balls),
    }


def journal_events(args, journal):
    """Ball-type changes and mechanism wakes, from the server's own journal."""
    exe = pathlib.Path(args.bin).resolve() / f'BallanceMMOSimTool{EXE}'
    proc = subprocess.run([str(exe), '--replay-session', journal, '--list'],
                          capture_output=True, text=True, timeout=600)
    text = proc.stdout + proc.stderr
    revived = collections.Counter(re.findall(r'BodyRevived .*name=(\S+)', text))
    ball_types = collections.Counter(re.findall(r'Physicalize ball=(\d+)', text))
    sectors = re.findall(r'Sector .*sector=(\d+)', text)
    return {'mechanisms_revived': dict(revived),
            'physicalize_by_ball_type': dict(ball_types),
            'distinct_sectors_entered': sorted(set(sectors))}


def parse_client_ticks(specs, players, what, delays=None):
    """--leave / --rejoin N:T specs -> {N: T}.  N must be a client of the
    run, T a tick > 0, and each client named once per option; both are
    refused for client 0, the host (its leave would hand the room over).
    With `delays` (a dict, --rejoin; step TRL) a spec may be N:T:MS, the
    rejoin's Join sent MS milliseconds after the Leave: delays[N] = MS."""
    out = {}
    for spec in specs:
        a, sep, b = spec.partition(':')
        ms = None
        if delays is not None and ':' in b:
            b, _, c = b.partition(':')
            try:
                ms = int(c)
            except ValueError:
                raise ValueError('%s %r: N:T:MS expected' % (what, spec))
            if ms < 0:
                raise ValueError('%s %r: a delay >= 0 expected' % (what, spec))
        try:
            index, tick = int(a), int(b)
        except ValueError:
            raise ValueError('%s %r: N:T expected' % (what, spec))
        if not sep or index < 0 or index >= players or tick <= 0:
            raise ValueError('%s %r: client 0..%d and a tick > 0 expected' % (what, spec, players - 1))
        if index in out:
            raise ValueError('%s names client %d twice' % (what, index))
        if index == 0:
            raise ValueError('%s of client 0: the host cannot leave its room and keep it' % what)
        out[index] = tick
        if ms:
            delays[index] = ms
    return out


def member_args(args, i):
    """Client i's leave / rejoin switches (world-agreement design 7.3 J2-J5,
    step M0), from --leave and --rejoin."""
    cmd = []
    if i in getattr(args, 'rejoin', {}):
        delay = getattr(args, 'rejoin_delay', {}).get(i, 0)
        cmd += ['--rejoin-at', str(args.rejoin[i]) + (':%d' % delay if delay else '')]
    if i in getattr(args, 'leave', {}):
        cmd += ['--leave-room-at', str(args.leave[i])]
    return cmd


def client_links(args):
    """Every client's link: the shared one (for the clients --latency-clients
    names, all when it is empty), then that client's --link fields on top."""
    laggy = {int(x) for x in args.latency_clients.split(',') if x.strip()}
    out = {}
    for i in range(args.players):
        link = {'latency_ms': 0.0, 'loss_pct': 0.0, 'jitter_ms': 0.0, 'loss_burst': args.loss_burst}
        if not laggy or i in laggy:
            link.update(latency_ms=args.latency_ms, loss_pct=args.loss_pct, jitter_ms=args.jitter_ms)
        link.update(args.links.get(i, {}))
        out[i] = link
    return out


def run_case(args, sector, case_dir):
    case_dir.mkdir(parents=True, exist_ok=False)
    # an external server writes its journal wherever its own config says;
    # --journal-dir points the replay check at it
    journals = (pathlib.Path(args.journal_dir).resolve() if args.journal_dir
               else case_dir / 'journals')
    config = (
        'rooms_enabled: true\n'
        'maximum_members: 8\n'
        'physics:\n'
        '  enabled: true\n'
        f'  game_root: {args.root}\n'
        f'  snapshot_interval: {100000 if args.scenario else 2}\n'
        f'  full_snapshot_interval: {100000 if args.scenario else 66}\n'
        f'  patience: {str(args.patience == "on").lower()}\n'
        f'  lead_adjust: {str(args.lead_adjust == "on").lower()}\n'
        f'  test_clock_skew: "{args.test_clock_skew}"\n'
        f'  input_delay: {args.input_delay}\n'
        '  maximum_physics_rooms: 1\n'
        '  event_rate_limit: 0\n'
        f'  spawn_impulse: {args.spawn_impulse}\n'
        f'  journal_dir: {str(journals).replace(chr(92), "/")}\n'
        '  journal_max_mb: 64\n'
        f'  journal_checkpoint_ticks: {args.checkpoint_ticks}\n'
        'auto_flush_log: true\n'
    )
    (case_dir / 'config.yml').write_text(config, encoding='utf8')

    bindir = pathlib.Path(args.bin).resolve()
    handles, children, proxies = [], [], []
    clients = {}
    proxy_of, watches = {}, []
    hang_watches = {}
    proxy_stats = {}
    server = None
    controller_started = False
    case_started = time.time()
    # Step B2SX: the journals already there, by name and mtime, so a case
    # never takes an earlier one (a mirrored journal lands seconds late).
    journals_before = ({q.name: q.stat().st_mtime for q in journals.glob('*.bmjr')}
                       if journals.exists() else {})
    links = client_links(args)
    # --external-server points the clients at a server someone else is running
    # (the Arch WSL one, say).  Its journal still has to land somewhere this
    # side can read, which is what journal_dir in its own config decides.
    host, _, port = (args.external_server or f'127.0.0.1:{args.port}').partition(':')
    server_addr = (host, int(port))
    try:
        if args.server_control:
            lph.server_control(args, 'start', case_dir / 'server-control-start.log')
            controller_started = True
        if not args.external_server:
            sf = open(case_dir / 'server.log', 'wb')
            handles.append(sf)
            server = subprocess.Popen(
                [str(bindir / f'BallanceMMOServer{EXE}'), '--port', str(args.port)],
                cwd=case_dir, stdin=subprocess.PIPE, stdout=sf,
                stderr=subprocess.STDOUT, creationflags=FLAGS)
            children.append(server)
            time.sleep(1.5)

        # Step DX9: a cue joiner (--join-late N:cue) starts with the others.
        late = {i: tick for i, tick in args.join_late.items() if i not in args.join_cue}
        host_ticks = {}
        tick_watches, tick_targets = {}, {}
        cue_files = {i: str(case_dir / ('join_cue%d' % i)) for i in sorted(args.join_cue)}
        if cue_files:
            cue_log = pathlib.Path(args.server_log) if args.external_server and args.server_log else case_dir / 'server.log'
            try:
                cue_offset = cue_log.stat().st_size
            except OSError:
                cue_offset = 0
            watches.append(ServerReadyCue(cue_log, cue_offset, list(cue_files.values()),
                                          getattr(args, 'join_cue_mark', None)))

        def launch(i, seconds):
            client_dir = case_dir / f'client{i}'
            client_dir.mkdir()
            log_path = case_dir / f'client{i}.log'
            cf = open(log_path, 'wb')
            handles.append(cf)
            connect_host, connect_port = server_addr
            link = links[i]
            link_events = [ev for ev in args.events if ev.client == i and ev.kind in PROXY_KINDS]
            # A proxy per client, kept by index: its own link, and the place
            # an event on that client's link happens.
            drop_down = args.drop_downs.get(i)
            outages = [parse_outage(spec)[1] for spec in args.outage if parse_outage(spec)[0] == i]
            if (link['latency_ms'] > 0 or link['loss_pct'] > 0 or link['jitter_ms'] > 0 or link_events or drop_down
                    or outages):
                pport = args.port + 400 + i
                proxy = DelayProxy(pport, server_addr, link['latency_ms'], link['loss_pct'], link['jitter_ms'],
                                   link['loss_burst'], args.loss_seed,
                                   str(case_dir / f'proxy{i}.log') if args.proxy_log else None)
                proxy.events = link_events
                proxy.index = i
                proxy.drop_down = drop_down
                proxy.outages = outages
                proxies.append(proxy)
                proxy_of[i] = proxy
                connect_host, connect_port = '127.0.0.1', pport
            # Explicit milestones survive arbitrary startup postponements,
            # pacer stalls and lead slews without extrapolating from an assign.
            stall_events = [ev for ev in args.events if ev.client == i and ev.kind == 'stall']
            timed_events = [ev for ev in args.events if ev.client == i and ev.kind != 'stall']
            targets = {0} | {ev.tick for ev in timed_events} | {ev.until for ev in timed_events if ev.until}
            if i == 0:
                targets.update(late.values())
            tick_targets[i] = targets
            def on_tick(target, actual, now, armer=arm_events(timed_events), host=(i == 0)):
                armer(target, actual, now)
                if host:
                    host_ticks[target] = (actual, now)
            watch = LogWatch(log_path, on_tick)
            tick_watches[i] = watch
            watches.append(watch)
            cmd = [str(bindir / f'BallanceMMOSessionClient{EXE}'),
                   '--root', args.root,
                   '--server', f'{connect_host}:{connect_port}',
                   '--name', f'Arena{i}',
                   '--level', str(args.level),
                   *(['--map', args.map] if args.map else []),
                   '--seconds', str(seconds + args.startup_timeout),
                   '--play-seconds', str(seconds),
                   '--trace',
                   '--journal', str(case_dir / f'client{i}.bmjr'),
                   # the client's own world at the same cadence as the server's,
                   # so the two can be compared body by body at the same ticks
                   '--checkpoint-ticks', str(args.checkpoint_ticks)]
            for tick in sorted(targets):
                cmd += ['--notify-tick', str(tick)]
            if args.client_arg:
                cmd.extend(args.client_arg)
            if sector > 1:
                cmd += ['--start-sector', str(sector)]
            if args.drive:
                record = case_dir / f'input{i}.bmrc'
                write_drive_record(record, args.level, args.seconds * 66 + 200, i,
                                   args.steer, args.drive_stop)
                cmd += ['--record', str(record)]
                if args.keep_lane:
                    cmd += ['--keep-lane', '15']
            if args.beam_bays:
                # Put each ball just short of a mechanism of its own, so the run
                # does not depend on anyone driving the full length of the arena
                # to reach one.  Virtools axes: the arena runs along x, y is up,
                # and the mechanism row is z (BAY_SPAN and MECH_Y below mirror
                # maps/build_arena_map.py).
                if args.beam_bays == 'auto':
                    # the bays of the sector this run has active: a mechanism in
                    # any other sector is not physicalized, so beaming a ball at
                    # it would meet nothing.  --beam-offset rotates the choice so
                    # a second pass reaches the bays the first pass did not.
                    in_sector = list(range((sector - 1) * BAYS_PER_SECTOR,
                                           min(sector * BAYS_PER_SECTOR, N_BAYS)))
                    bays = [in_sector[(args.beam_offset + k) % len(in_sector)]
                            for k in range(args.players)]
                else:
                    bays = [int(b) for b in args.beam_bays.split(',')]
                bay = bays[i % len(bays)]
                # spread the landing points along the lane: without this two
                # clients sent to the same bay materialise inside each other
                # --beam-y picks the line: MECH_Y for a mechanism, PLAY_Y for
                # the driving lane (the checkpoints and resetpoints live there,
                # 28 units off the mechanism row - beaming at a checkpoint and
                # aiming at the mechanism row simply misses it)
                cmd += ['--beam', repr(bay * BAY_SPAN + args.beam_dx - i * args.beam_spread),
                        repr(4.0), repr(args.beam_y if args.beam_y >= 0 else MECH_Y),
                        str(args.beam_tick)]
            if args.stall_client == i and args.stall_ms > 0:
                cmd += ['--pause-at', str(args.stall_at), '--pause-ms', str(args.stall_ms)]
            for ev in stall_events:
                cmd += ['--stall-ms', '%d:%d' % (ev.tick, ev.ms)]
            if args.window_origin:
                cmd += ['--window-origin', str(args.window_origin)]
            cmd += member_args(args, i)
            cmd.extend(args.client_arg_for.get(i, []))
            # Explicit coverage endpoints supplied by a level driver must be
            # judged too, not merely logged by the client.
            targets.update(int(cmd[k + 1]) for k, option in enumerate(cmd[:-1]) if option == '--notify-tick')
            if i in cue_files:
                cmd += ['--join-cue', cue_files[i]]
            # The host waits for the members that start with it; a late joiner
            # (a cue joiner too) comes into the running session.
            cmd += ['--host', '--expect', str(args.players - len(args.join_late))] if i == 0 else ['--join-first']
            p = subprocess.Popen(cmd, cwd=client_dir, stdin=subprocess.DEVNULL,
                                 stdout=cf, stderr=subprocess.STDOUT, creationflags=FLAGS)
            clients[i] = p
            children.append(p)
            if args.hang_dump:
                hang_watches[i] = HangWatch(i, p, log_path, case_dir / f'client{i}.hang.dmp', args.hang_dump_after)

        for i in range(args.players):
            if i in late:
                continue
            launch(i, args.seconds)
            time.sleep(0.4)
        # The host must actually reach T. Loading and backoff do not consume
        # its play budget, nor the late joiner's remaining budget.
        while late:
            if 0 in host_ticks:
                _, at = host_ticks[0]
                for i, tick in sorted(late.items()):
                    if tick in host_ticks:
                        remaining = args.seconds - (time.monotonic() - at)
                        print('  client%d joins late at host tick ~%d (%.0f s left)' % (i, tick, remaining), flush=True)
                        launch(i, max(1, remaining))
                        del late[i]
            if clients[0].poll() is not None:
                break
            time.sleep(0.01)

        for i, p in sorted(clients.items()):
            p.wait(timeout=args.seconds + args.startup_timeout + 30)
        if server is not None:
            server.stdin.write(b'sessions\nstop\n')
            server.stdin.flush()
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.terminate()
                server.wait(timeout=10)
    finally:
        for p in children:
            if p.poll() is None:
                p.terminate()
        for w in watches:
            w.close()
        for w in hang_watches.values():
            w.close()
        for h in handles:
            h.close()
        # What each link event did, from the proxy that ran it.
        for q in proxies:
            if q.events:
                proxy_stats.setdefault('events', []).append(
                    {'client': q.index, 'blacked_up': q.blacked['up'], 'blacked_down': q.blacked['down'],
                     'delayed_up': q.delayed['up'], 'delayed_down': q.delayed['down'],
                     'events': [ev.as_dict() for ev in q.events]})
        # Prove the loss actually happened: a run that claims to survive
        # dropped datagrams has to show that some were dropped.  Counted here,
        # while the proxies are alive; attached to the result once it exists.
        if any(q.loss for q in proxies):
            proxy_stats['dropped'] = sum(q.dropped for q in proxies)
            proxy_stats['passed'] = sum(q.passed for q in proxies)
        # The link's load per client, from what the proxies passed: the relayed
        # input stream is the bulk of the downlink in a room without snapshots.
        if proxies:
            ordered = sorted(proxies, key=lambda q: q.index)
            proxy_stats['bytes_down'] = [q.bytes_down for q in ordered]
            proxy_stats['bytes_up'] = [q.bytes_up for q in ordered]
            proxy_stats['links'] = {q.index: links[q.index] for q in ordered}
        for q in proxies:
            if q.drop_down is not None:
                proxy_stats.setdefault('drop_down', []).append(
                    {'client': q.index, 'fired_at': q.forced_fired, 'dropped': q.forced_dropped})
            for o in q.outages:
                proxy_stats.setdefault('outage', []).append(dict(o.as_dict(), client=q.index))
        for q in proxies:
            q.close()
        if controller_started:
            lph.server_control(args, 'stop', case_dir / 'server-control-stop.log')

    result = {'sector': sector, 'dir': str(case_dir), 'clients': []}
    if 'dropped' in proxy_stats:
        result['proxy_dropped'] = proxy_stats['dropped']
        result['proxy_passed'] = proxy_stats['passed']
        print('  proxy dropped %d of %d datagrams'
              % (proxy_stats['dropped'], proxy_stats['dropped'] + proxy_stats['passed']))
    if 'drop_down' in proxy_stats:
        result['proxy_drop_down'] = proxy_stats['drop_down']
        for d in proxy_stats['drop_down']:
            print('  proxy drop-down: client%d %s' % (
                d['client'], 'fired at downlink datagram %d (%d B), dropped %d' % (d['fired_at'][0], d['fired_at'][1], d['dropped'])
                if d['fired_at'] else 'NEVER FIRED (no downlink datagram matched the anchor)'))
    if 'outage' in proxy_stats:
        result['proxy_outage'] = proxy_stats['outage']
        for d in proxy_stats['outage']:
            print('  proxy outage: client%d %s: %s' % (
                d['client'], d['spec'], 'anchored at downlink datagram %d (%d B), dropped %d up / %d down'
                % (d['fired_at'][0], d['fired_at'][1], d['dropped_up'], d['dropped_down'])
                if d['fired_at'] else 'NEVER FIRED (no downlink datagram matched the anchor)'))
    if 'bytes_down' in proxy_stats:
        result['proxy_bytes_down'] = proxy_stats['bytes_down']
        result['proxy_bytes_up'] = proxy_stats['bytes_up']
        print('  proxy per client: down %s KB/s, up %s KB/s'
              % (' / '.join('%.1f' % (b / 1024.0 / max(args.seconds, 1)) for b in proxy_stats['bytes_down']),
                 ' / '.join('%.1f' % (b / 1024.0 / max(args.seconds, 1)) for b in proxy_stats['bytes_up'])))
    if 'links' in proxy_stats:
        result['links'] = proxy_stats['links']
        if args.links:
            print('  proxy links: %s' % '; '.join(
                'client%d %g ms loss %g%% burst %d jitter %g ms' % (i, l['latency_ms'], l['loss_pct'], l['loss_burst'], l['jitter_ms'])
                for i, l in sorted(proxy_stats['links'].items())))
    for e in proxy_stats.get('events', []):
        result.setdefault('link_events', []).append(e)
        for ev in e['events']:
            print('  proxy event: %s; observed tick %s; blacked %d up / %d down, delayed %d up / %d down datagrams'
                  % (LinkEvent(ev['client'], ev['tick'], ev['kind'], ev['ms'], ev['dur_ms'], ev['dir'], ev['until']).describe(),
                     ', '.join(str(a['base']) for a in ev['armed']) or 'NEVER',
                     e['blacked_up'], e['blacked_down'], e['delayed_up'], e['delayed_down']))
    # One row per client index, launched or not: a late joiner the host
    # outlived is a row that says so, so client k is always row k.
    for i in range(args.players):
        p = clients.get(i)
        if p is None:
            print(f'  note: client{i} was never started (its --join-late tick came after the host ended)', flush=True)
            result['clients'].append({'exit_code': None, 'metrics': None, 'never_started': True})
            continue
        text = (case_dir / f'client{i}.log').read_text(errors='replace')
        statuses = [s for s in text.splitlines() if s.startswith('status:')]
        row = {
            'exit_code': p.returncode,
            'metrics': status_metrics(statuses[-1]) if statuses else None,
        }
        observed = {tick: actual for tick, (actual, _) in tick_watches[i].ticks.items()}
        play_start = re.search(r'play time started after ([0-9.]+) s of startup; budget ([0-9.]+) s', text)
        play_end = re.search(r'play time limit reached \(([0-9.]+) s after the first assigned frame\)', text)
        row['timing'] = {
            'targets': sorted(tick_targets[i]), 'observed': observed,
            'startup_s': float(play_start[1]) if play_start else None,
            'play_budget_s': float(play_start[2]) if play_start else None,
            'play_elapsed_s': float(play_end[1]) if play_end else None,
            'problems': timing_problems(tick_targets[i], observed),
        }
        hang = hang_watches[i].hang if i in hang_watches else None
        if hang:
            # The process was ended by the watcher: its own exit code is the
            # one it logged; the hang is a row of its own.
            row['exit_code'] = hang['logged_exit_code']
            row['hang'] = hang
        result['clients'].append(row)
    result['interaction'] = interaction_report(case_dir, args.players)
    # H5: an external server's log next to the clients', so the report can
    # read its late events and barrier lines.
    if args.server_log and pathlib.Path(args.server_log).exists():
        shutil.copy(args.server_log, case_dir / 'server.log')
    if args.external_server:
        # Step B2SX: the local test server writes its journal into WSL's ext4
        # and a mirror copies it here once the server has closed it, at the
        # session's end (.local-tools/arch_journal_mirror.sh): give it a few
        # seconds to land before deciding the case has none.  Only a journal
        # that was not there at the case's start (or has changed since) is
        # this case's: an earlier case's, a few seconds old, would otherwise
        # end the wait before this one's has landed.
        def own_journals():
            if not journals.exists():
                return []
            return [q for q in journals.glob('*.bmjr') if journals_before.get(q.name) != q.stat().st_mtime]
        wait_until = time.time() + 20
        while time.time() < wait_until and not own_journals():
            time.sleep(0.25)
    journal_paths = sorted(journals.glob('*.bmjr')) if journals.exists() else []
    if args.external_server:
        # The external server's directory may hold earlier sessions' journals:
        # only the ones this case wrote are its own.
        # None of them is: no replay and no event report, said aloud, rather
        # than an older session's journal replayed and reported as this one.
        fresh = sorted(own_journals())
        if not fresh:
            print(f'  note: the external server wrote no journal for this case in {journals} '
                  f'({len(journal_paths)} older ones ignored): no replay, no event report', flush=True)
        journal_paths = fresh
    result['journal'] = [str(q) for q in journal_paths]
    return result


# --------------------------------------------------------------------------
# H6: the event report.  Everything in it is read back out of what the run
# recorded: the server journal (substitutions, checkpoints, TICK times, late
# events), the clients' journals (their own world, received snapshots) and
# the clients' window: lines (rollbacks, relay lag, resyncs).

RE_WINDOW = re.compile(r'^window: tick=(\d+)\s*(.*)$')
RE_OWN_ID = re.compile(r'own id (\d+)')
RE_SUBST = re.compile(r'\bsubst=([0-9/]+)')
RE_LEAD_ADJ = re.compile(r'\blead_adj=(\d+)')
RE_HOLD = re.compile(r'^hold-event: own (\w+) (?:#\d+ )?stamped tick (\d+) (held at tick (\d+)|dropped at tick (\d+)|'
                     r'\(held at tick (\d+)\) released at tick (\d+)|\(held at tick \d+\) discarded(:))')
RE_STALL = re.compile(r'^stall: (\d+) ms at tick (\d+)')
# Plan step 19 (late-player design 3.3): the owner's own birth the server
# applied later (moved inside the history, or past the reach: a resync) or
# never, and the echo counters of the status line.
RE_OWN_MOVE = re.compile(r'^own physicalize stamped (\d+) (?:moved to (\d+)|never made) by the server at local tick (\d+): '
                         r'rewound to (\d+)')
# Every resync the owner asks for about its own birth: past the reach, no
# record (past the history), a refused rewind, a life ended since, a pair
# moved as a whole, a missing echo; the reason is kept.
RE_OWN_MOVE_FAR = re.compile(r'^own (?:birth|beam) moved (\d+) -> (\d+|never) at local tick (\d+): (.+), resync\s*$')
# acked/moved/superseded/rejected[/missing]
RE_ECHO = re.compile(r'\becho=(\d+)/(\d+)/(\d+)/(\d+)(?:/(\d+))?')
RE_OWNLIFE = re.compile(r'\bownlife=([0-9/]+)')
RE_LATE_BY = re.compile(r'event type (\d+) from (\d+) stamped (\d+) applied at tick (\d+) \(late by (\d+)\)')
RE_BARRIER = re.compile(r'no Physicalize for player (\d+) at tick (\d+)')
# Plan step 20 (late-player design 3.2, S4): the barrier is gone; a run of
# ticks a member's frame claimed a ball the server's world did not have is
# stepped and told when it ends (or once while still open after 66 ticks).
RE_S4 = re.compile(r'S4 bodiless player=(\d+) ticks=(\d+)\.\.(\d+) n=(\d+)( open)?')
# Protocol 9 (late-player plan step 8, the gate of step 17): the server's
# intake-to-send delay of every accepted lifecycle event.
RE_S5 = re.compile(r'\bS5 event player=(\d+) type=(\w+) origin=(\d+) applied=(\d+) late=(-?\d+) delay_us=(\d+)'
                   r' delay_ticks=([0-9.]+) to=(\d+)(?: (superseded|leaver))?')
# A resync's base and the newest tick the old epoch's held_total counted
# (server.cpp handle_session_resync): the ticks between are applied held under
# no numbering the client has, so no notice of them ever reaches it.
RE_RESYNCED = re.compile(r'resynced at tick (\d+) \(last full snapshot it had: \d+; held counted through tick (\d+)\)')
# Plan step 10's S1 lead line (session_runner.cpp apply_lead_decision): a
# SessionLead sent, its k - with the meter's decided k and the binding cap
# when a cap (the total, the Sector margin) cut it - the reset range
# [held-from, E) (held-from 0 for a lower) and the frames the buffer reset
# dropped; and the server's line for the client's acknowledgement
# (session_runner.cpp acknowledge_lead).
RE_LEAD_SENT = re.compile(r'\bS1 .*\blead player=(\d+) id=(\d+) k=(-?\d+)(?: decided=(-?\d+))?'
                          r'(?: cap=(\S+) binding=(\S+))? N=(\d+) held-from=(\d+) E=(\d+).*?\bdropped=(\d+)')
RE_LEAD_ACK = re.compile(r'\bS1 lead-ack player=(\d+) id=(\d+) applied=(-?\d+) tick=(\d+)')
PHASES = ('B', 'D', 'A')


def parse_window(line):
    """A client's window: line -> dict, or None.  Unknown fields are kept as
    strings, so a later revision's additions (subst=, lead_adj=, ...) read."""
    m = RE_WINDOW.match(line)
    if not m:
        return None
    out = {'tick': int(m.group(1)), 'lag': {}, 'inrew_src': {}}
    for token in m.group(2).split():
        key, _, value = token.partition('=')
        lm = re.match(r'^(lag|inrew)\[(\d+)\]$', key)
        if lm and lm.group(1) == 'lag':
            hist = out['lag'].setdefault(int(lm.group(2)), {})
            for pair in value.split(','):
                if '*' in pair:
                    lag, _, n = pair.partition('*')
                    hist[int(lag)] = hist.get(int(lag), 0) + int(n)
        elif lm:
            out['inrew_src'][int(lm.group(2))] = int(value)
        elif re.fullmatch(r'-?\d+(/\d+)*', value or 'x'):
            parts = [int(v) for v in value.split('/')]
            out[key] = parts if len(parts) > 1 else parts[0]
        else:
            out[key] = value
    return out


def hist_stats(hist):
    """max and 99th percentile (the client's own rule) of a lag histogram."""
    total = sum(hist.values())
    if not total:
        return {'n': 0, 'max': 0, 'p99': 0}
    seen = 0
    p99 = max(hist)
    for lag in sorted(hist):
        seen += hist[lag]
        if seen * 100 >= total * 99:
            p99 = lag
            break
    return {'n': total, 'max': max(hist), 'p99': p99}


def merge_hist(into, hist):
    for lag, n in hist.items():
        into[lag] = into.get(lag, 0) + n


def phase_of(tick, phases):
    for name in PHASES:
        lo, hi = phases[name]
        if lo <= tick < hi:
            return name
    return None


def distance(a, b):
    return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))


RE_INPUT_DELAY = re.compile(r'^session \d+ starting: .*\binput delay (\d+)')
# Step F0: a member's lives in one client log (a --rejoin-at begins the next
# with a new 'session N starting:' line), a join's import of the server's
# state (the late joiner's and the rejoiner's first state: a resync from tick
# 0 - not a resync of a running client) and where its re-step ended.
RE_SESSION_LIFE = re.compile(r'^session \d+ starting: ')
RE_ASSIGNED = re.compile(r'^tick base (\d+) assigned')
RE_JOIN_IMPORT = re.compile(r'^resynced: tick base (\d+) \(old tick 0\b')
RE_RESTEP = re.compile(r"^resync: re-stepped from the server's tick (\d+) to (\d+)")
# Step F0: the airship (BALA; the balloon is the design's one exception, never
# compared): a ball is out of the comparison from the tick the server's world
# says it entered the zone to its next birth (a spawn ghost line).
RE_AIRSHIP_ZONE = re.compile(r"player (\d+)'s ball \S+ is in the airship zone at tick (\d+)")
RE_SPAWN_BIRTH = re.compile(r"player (\d+)'s ball \S+ is a spawn ghost at tick (\d+)")
# Step F0: the server's lead adjust mode (A8's joiner lead rule depends on it).
RE_LEAD_MODE = re.compile(r'Physics sessions enabled \(.*\blead adjust (on|off)\b')
RE_STATUS_LEAD = re.compile(r'\bworst=\d+ lead=([0-9.]+)')
# A join-late episode's D: the joiner's first four windows (5.2 A8).
JOIN_WINDOWS = 4
WINDOW_TICKS = 60


def read_client_log(path):
    """What the report needs from one client log."""
    out = {'own_id': 0, 'bases': [], 'windows': [], 'subst': None, 'lead_adj': None, 'holds': [], 'stalls': [],
           'last_tick': 0, 'input_delays': [], 'status_lead': None, 'exists': path.exists(),
           'echo': None, 'ownlife': None, 'own_moves': [], 'own_deaths': [],
           # step F0: one entry per life (a --rejoin-at's next life is the
           # second): its assigned base, its join import (the server's first
           # state, not a resync), where that import's re-step ended, and the
           # life's real resync bases
           'lives': []}
    if not path.exists():
        return out
    life = None
    for line in path.read_text(errors='replace').replace('\x00', '').splitlines():
        if not out['own_id']:
            m = RE_OWN_ID.search(line)
            if m:
                out['own_id'] = int(m.group(1))
        if RE_SESSION_LIFE.match(line) or (life is None and RE_BASE.search(line)):
            life = {'base': None, 'import': None, 'import_end': None, 'import_applied': False, 'resyncs': []}
            out['lives'].append(life)
        m = RE_BASE.search(line)
        if m:
            out['bases'].append(int(m.group(1) or m.group(2)))
            ma, mj = RE_ASSIGNED.match(line), RE_JOIN_IMPORT.match(line)
            if ma and life['base'] is None:
                life['base'] = int(ma.group(1))
            elif mj and life['import'] is None and not life['resyncs']:
                life['import'] = int(mj.group(1))
            elif not ma:
                life['resyncs'].append(int(m.group(1) or m.group(2)))
        # The first re-step after the import ends the span the world is not
        # yet the session's (a state that never applied - a later resync
        # took its place - ends it at that resync's re-step); the import is
        # a counted resync only when its own state applied.
        m = RE_RESTEP.match(line)
        if m and life is not None and life['import'] is not None and life['import_end'] is None:
            life['import_end'] = int(m.group(2))
            life['import_applied'] = not life['resyncs']
        w = parse_window(line)
        if w:
            out['windows'].append(w)
            out['last_tick'] = max(out['last_tick'], w['tick'])
            continue
        m = RE_INPUT_DELAY.match(line)
        if m:
            out['input_delays'].append(int(m.group(1)))
        if line.startswith('status:'):
            m = RE_SUBST.search(line)
            if m:
                out['subst'] = m.group(1)
            m = RE_LEAD_ADJ.search(line)
            if m:
                out['lead_adj'] = int(m.group(1))
            m = RE_STATUS_LEAD.search(line)
            if m:
                out['status_lead'] = float(m.group(1))
            m = re.search(r'\btick=(\d+)', line)
            if m:
                out['last_tick'] = max(out['last_tick'], int(m.group(1)))
            # acked/moved/superseded/rejected and moved/removed/resynced/
            # remade/other, cumulative: the last status line wins
            m = RE_ECHO.search(line)
            if m:
                out['echo'] = [int(m.group(k)) for k in range(1, 5)] + [int(m.group(5) or 0)]
            m = RE_OWNLIFE.search(line)
            if m:
                out['ownlife'] = m.group(1)
        m = RE_OWN_MOVE.match(line)
        death = re.match(r'^own death moved (\d+) -> (\d+) (.+) at local tick (\d+): (.+)', line)
        if death:
            out['own_deaths'].append(dict(origin=int(death[1]), applied=int(death[2]),
                                           kind=death[3], at=int(death[4]), outcome=death[5]))
        if m:
            out['own_moves'].append({'origin': int(m.group(1)), 'applied': int(m.group(2)) if m.group(2) else None,
                                     'at': int(m.group(3)), 'how': 'rewound'})
        m = RE_OWN_MOVE_FAR.match(line)
        if m:
            out['own_moves'].append({'origin': int(m.group(1)),
                                     'applied': None if m.group(2) == 'never' else int(m.group(2)),
                                     'at': int(m.group(3)), 'how': 'resync', 'why': m.group(4)})
        m = RE_HOLD.match(line)
        if m:
            if m.group(4):
                out['holds'].append({'type': m.group(1), 'stamp': int(m.group(2)), 'held_at': int(m.group(4))})
            elif m.group(5):
                out['holds'].append({'type': m.group(1), 'stamp': int(m.group(2)), 'dropped_at': int(m.group(5))})
            elif m.group(7) and out['holds']:
                out['holds'][-1]['released_at'] = int(m.group(7))
            elif m.group(8) and out['holds']:
                out['holds'][-1]['discarded'] = True
        m = RE_STALL.match(line)
        if m:
            out['stalls'].append({'ms': int(m.group(1)), 'tick': int(m.group(2))})
    # Each window covers the ticks after the previous line up to its own (a
    # new session's numbering starts the chain again).
    prev = None
    for w in out['windows']:
        w['lo'] = prev + 1 if prev is not None and prev < w['tick'] else w['tick'] - WINDOW_TICKS + 1
        prev = w['tick']
    # Step F0: a join import is not a resync - its count comes off the first
    # window at or after it that has one done.
    out['join_resyncs_removed'] = 0
    for lf in out['lives']:
        if lf['import'] is None or not lf['import_applied']:
            continue
        for w in out['windows']:
            if w['tick'] >= lf['import'] and isinstance(w.get('resyncs'), list) and w['resyncs'][1] > 0:
                w['resyncs'] = [w['resyncs'][0], w['resyncs'][1] - 1]
                out['join_resyncs_removed'] += 1
                break
    return out


def episode(args, logs):
    """The episode the report is cut on: T_on, T_off, the clients it is on,
    what it was and where it came from (an --event, else the hold or stall a
    client log reports, else a late join), or None."""
    if args.events:
        return {'t_on': min(ev.tick for ev in args.events), 't_off': max(ev.t_off for ev in args.events),
                'clients': sorted({ev.client for ev in args.events}), 'describe': [ev.describe() for ev in args.events],
                'source': 'event'}
    for i, log in sorted(logs.items()):
        for h in log['holds']:
            on = h.get('held_at', h.get('dropped_at'))
            off = h.get('released_at', on)
            what = 'dropped' if 'dropped_at' in h else ('discarded' if h.get('discarded') else 'held')
            return {'t_on': on, 't_off': off, 'clients': [i], 'source': 'hold', 'drop': 'dropped_at' in h,
                    'describe': ['client%d hold-event %s stamped %d, %s' % (i, h['type'], h['stamp'], what)]}
        for s in log['stalls']:
            return {'t_on': s['tick'], 't_off': s['tick'] + int(math.ceil(s['ms'] / 1000.0 / TICK_S)), 'clients': [i],
                    'source': 'stall', 'describe': ['client%d stall %d ms at tick %d' % (i, s['ms'], s['tick'])]}
    for i, asked in sorted(getattr(args, 'join_late', {}).items()):
        bases = logs.get(i, {}).get('bases') or []
        if bases:
            # The joiner's tick base, not the tick asked: the engine boots
            # first, so the session meets it a few hundred ticks later.
            on = bases[0]
            return {'t_on': on, 't_off': on + JOIN_WINDOWS * WINDOW_TICKS - REACH, 'clients': [i], 'source': 'join',
                    'describe': ['client%d joins late: asked at host tick %d, tick base %d (%+d)' % (i, asked, on, on - asked)]}
    return None


def window_phase(w, phases):
    """The phase a window: line belongs to: D when it overlaps D at all (the
    episode's tail must not leak into A, nor its first tick into B), else the
    phase it overlaps."""
    lo, hi = w.get('lo', w['tick'] - WINDOW_TICKS + 1), w['tick']

    def overlaps(p):
        a, b = phases[p]
        return a < b and lo < b and hi >= a
    if overlaps('D'):
        return 'D'
    for p in ('B', 'A'):
        if overlaps(p):
            return p
    return None


def aggregate(wins, late_ids, index_of):
    """The window counters of one phase."""
    ph = {'windows': len(wins)}

    def pair(key):
        n = sum(w[key][0] for w in wins if isinstance(w.get(key), list))
        d = sum(w[key][1] for w in wins if isinstance(w.get(key), list))
        return [n, d]
    ph['rb'] = {'input': pair('inrew'), 'life': pair('life'), 'snap': pair('snaprb'), 'forced': pair('forced'),
                # the own-correction rewinds of plan step 4; no field before it
                'subst': pair('substrew') if any('substrew' in w for w in wins) else None}
    # step 4b's coalescing of the own replays: deferred frames / unseen diffs /
    # waiting replays another rewind carried / amendments with no own replay
    ph['substco'] = [sum(w['substco'][k] for w in wins if isinstance(w.get('substco'), list) and len(w['substco']) > k)
                     for k in range(4)] if any('substco' in w for w in wins) else None
    ph['inrew_late'] = sum(n for w in wins for src, n in w['inrew_src'].items() if src in late_ids)
    overall, by_src = {}, {}
    for w in wins:
        for src, hist in w['lag'].items():
            merge_hist(overall, hist)
            merge_hist(by_src.setdefault(src, {}), hist)
    ph['lag'] = hist_stats(overall)
    ph['lag_src'] = {('c%d' % index_of[src]) if src in index_of else str(src): hist_stats(h)
                     for src, h in sorted(by_src.items())}
    ph['relay_late'] = sum(w.get('late', 0) for w in wins if isinstance(w.get('late'), int))
    ph['relay_covered'] = sum(w.get('covered', 0) for w in wins if isinstance(w.get('covered'), int))   # step RLT
    ph['snaps'] = sum(w.get('snaps', 0) for w in wins if isinstance(w.get('snaps'), int))
    ph['resyncs'] = sum(w['resyncs'][1] for w in wins if isinstance(w.get('resyncs'), list))
    ph['resync_requests'] = sum(w['resyncs'][0] for w in wins if isinstance(w.get('resyncs'), list))
    ph['rebases'] = sum(w.get('rebases', 0) for w in wins if isinstance(w.get('rebases'), int))
    ph['lead_adj'] = sum(w['lead_adj'] for w in wins if isinstance(w.get('lead_adj'), int)) \
        if any('lead_adj' in w for w in wins) else None
    ph['moved'] = sum(w['moved'] for w in wins if isinstance(w.get('moved'), int)) \
        if any('moved' in w for w in wins) else None
    return ph


def client_phases(phases, base, end):
    """A client's own phases: from its base (a late joiner's B is empty) to
    its end."""
    return {'B': (max(base, phases['B'][0]), phases['B'][1]), 'D': phases['D'],
            'A': (phases['A'][0], max(phases['A'][0], end))}


ARM_FAMILY = {'A0': 'control', 'A0J': 'control', 'A1': 'burst', 'A2': 'burst', 'A3X': 'burst', 'A4': 'burst',
              'A7': 'burst', 'A9': 'burst', 'A3': 'a3', 'A3L': 'reported', 'A5': 'reported', 'A5U': 'lead',
              'A5S': 'lead', 'A5D': 'a5d', 'A6': 'hold', 'A6M': 'hold', 'A6B': 'hold', 'A8': 'join', 'A9C': 'stall'}
ARM_FAMILY.update(lph.FAMILIES)


def infer_arm(args, source):
    """--arm, or the arm the episode's kind says (5.2's table)."""
    if getattr(args, 'arm', ''):
        return args.arm.upper()
    if source in ('hold', 'join', 'stall'):
        return {'hold': 'A6', 'join': 'A8', 'stall': 'A9C'}[source]
    kinds = {ev.kind for ev in args.events}
    if kinds <= {'none'}:
        return 'A0'
    if 'stall' in kinds:
        return 'A9C'
    if 'step' in kinds:
        ev = next(e for e in args.events if e.kind == 'step')
        # A5 is +300 ms both ways, A5s +100 ms both ways (5.2's table)
        return 'A5D' if ev.until else ('A5U' if ev.direction == 'up' else ('A5S' if ev.ms <= 150 else 'A5'))
    if 'spike' in kinds:
        return 'A9'
    if any(ev.kind == 'blackout' and ev.direction == 'both' for ev in args.events):
        return 'A3'
    return 'A1'


def s5_summary(lines):
    """p50/p99/max of the S5 intake-to-send delay in ticks, per type and split
    on-time / re-stamped, plus 'all' (the step-17 gate: p99 <= 2 ticks in A0
    and A4)."""
    groups = collections.defaultdict(list)
    for e in lines:
        kind = 'restamped' if e['late'] > 0 else 'on-time'
        for key in ('all', 'all/' + kind, e['type'], e['type'] + '/' + kind):
            groups[key].append(e['delay_ticks'])
    out = {}
    for key, values in groups.items():
        ordered = sorted(values)
        n = len(ordered)
        out[key] = {'n': n, 'p50': ordered[min(n - 1, int(0.50 * n))], 'p99': ordered[min(n - 1, int(0.99 * n))],
                    'max': ordered[-1]}
    return out


def server_log_text(case_dir):
    """The run's server log (the case directory's, else the run's)."""
    for cand in (case_dir / 'server.log', case_dir.parent / 'server.log'):
        if cand.exists():
            return cand.read_text(errors='replace').replace('\x00', '')
    return ''


def relay_samples(path):
    import journal_trace as jt
    import struct
    reader = jt.JournalReader(path.read_bytes())
    offset = len(jt.MAGIC) + 4
    _, payload, offset = reader._next(offset)
    current = reader._header(payload)['first_tick']
    samples = []
    while offset < len(reader.data):
        record = reader._next(offset)
        if record is None:
            break
        tag, payload, offset = record
        if tag == jt.TAG_TICK:
            current = struct.unpack_from('<I', payload)[0]
        elif tag == jt.TAG_INPUT:
            tick, sender, _, flags = struct.unpack_from('<IIBB', payload)
            if flags & jt.INPUT_RELAYED and current > tick:
                samples.append([tick, current, current - tick, sender])
    return samples


def relay_start_hold(report, row, controls, control=False):
    """Relay lag in D with patience on, against the same network healthy (the
    user's decisions of 2026-11-04, after LBM2/LBM3).

    The baseline is this run's own B for the client and source: A0h and A0j
    run other links than A0, and a client's lag differs by up to 2 ticks from
    run to run in every phase alike (A0j's clock anchoring), so neither A0's
    band nor the off twin's run is the same network.  A control arm (A0, A0h,
    A0j: no late member) exempts nothing; a fault arm exempts one hold per
    episode: the first staircase over that baseline at or after
    the episode - consecutive ticks whose lag never rises and starts at most
    baseline + margin, the server waiting for the late member and catching up
    a tick at a time (25, 24, 23 ...; it starts a tick or so after the
    episode, and an A6 event's 20 ticks later).  Either way the rest's p99
    may be one tick over the baseline: a single 23 among a short D's 48
    samples per sender is what every phase of every run shows, the off
    twin's included."""
    episodes = report.get('relay_episodes') or [dict(tick=report['t_on'], clients=report['event_clients'])]
    margins = [(episode['tick'], 10 if len(report['clients']) >= 4
                and 2 <= len(episode['clients']) <= len(report['clients']) / 2 else 4) for episode in episodes]
    bands = [rep for rep in controls if rep.get('arm') == 'A0' and rep.get('lead_adjust') == 'off']
    twins = [rep for rep in controls if rep.get('arm') == report.get('arm') and rep.get('lead_adjust') == 'off']
    lo, hi = report['phases']['D']
    majority_raise = report.get('arm', '').upper() == 'A3M' and row['index'] == 0
    effective = [lead['e'] for member in report['clients'] if member['index'] in report['event_clients']
                 for lead in member.get('leads', []) if lead['k'] > 0] if majority_raise else []
    last_effective = max(effective) if effective else None
    if majority_raise:
        hi = row['span'][1] + 1
    samples = [sample for sample in row.get('relay_samples', []) if lo <= sample[0] < hi]
    output = []
    for sender in [None] + sorted({sample[3] for sample in samples}):
        def edge(reports, window):
            values = []
            for rep in reports:
                member = next((member for member in rep['clients'] if member['index'] == row['index']), None)
                if member is None:
                    continue
                source = next((member['index'] for member in report['clients'] if member['id'] == sender), None)
                source_id = next((member['id'] for member in rep['clients'] if member['index'] == source), None)
                span = window(rep)
                values.extend(sample[2] for sample in member.get('relay_samples', [])
                              if (span is None or span[0] <= sample[0] < span[1])
                              and (sender is None or sample[3] == source_id))
            return sorted(values)[min(len(values) - 1, int(.99 * len(values)))] if values else None
        twin = edge(twins, lambda rep: rep['phases']['B'])
        healthy = sorted(sample[2] for sample in row.get('relay_samples', [])
                         if report['phases']['B'][0] <= sample[0] < report['phases']['B'][1]
                         and (sender is None or sample[3] == sender))
        band = healthy[min(len(healthy) - 1, int(.99 * len(healthy)))] if healthy else twin
        if band is None:
            band = edge(bands, lambda rep: None)
        selected = [sample for sample in samples if sender is None or sample[3] == sender]
        excess = [sample for sample in selected if band is not None and sample[2] > band]
        if majority_raise:
            bad = [sample for sample in excess if last_effective is not None
                   and sample[0] < last_effective and sample[2] > band + 22]
            rest = sorted(sample[2] for sample in selected
                          if last_effective is not None and sample[0] >= last_effective + 48)
            limit = band
        else:
            worst = {}
            for sample in selected:
                worst[sample[0]] = max(worst.get(sample[0], sample[2]), sample[2])
            holds = []
            for start, margin in ([] if control else margins):
                first = next((t for t in sorted(worst) if t >= start and band is not None and worst[t] > band
                              and not any(a <= t < b for a, b in holds)), None)
                if first is None or worst[first] > band + margin:
                    continue
                end = first + 1
                while end in worst and worst[end] > band and worst[end] <= worst[end - 1]:
                    end += 1
                holds.append((first, end))
            bad = []
            rest = sorted(sample[2] for sample in selected if not any(a <= sample[0] < b for a, b in holds))
            limit = None if band is None else band + 1
        p99 = rest[min(len(rest) - 1, int(.99 * len(rest)))] if rest else None
        ok = None if band is None or p99 is None or (not majority_raise and twin is None) else not bad and p99 <= limit
        output.append(dict(source=sender, ok=ok, margins=margins, band=band, twin=twin, rest_p99=p99,
                           excess=len(excess), bad=bad[:20], samples=len(selected), last_effective=last_effective))
    return output


def airship_spans(text):
    """Step F0: player -> [(zone tick, next birth tick)) from the server log:
    a ball in its airship span is per-world (BALA) and never compared."""
    zones, births = collections.defaultdict(list), collections.defaultdict(list)
    for m in RE_AIRSHIP_ZONE.finditer(text):
        zones[int(m.group(1))].append(int(m.group(2)))
    for m in RE_SPAWN_BIRTH.finditer(text):
        births[int(m.group(1))].append(int(m.group(2)))
    out = {}
    for player, ticks in zones.items():
        out[player] = [(z, min([b for b in births[player] if b > z] or [1 << 31])) for z in sorted(set(ticks))]
    return out


def in_spans(spans, tick):
    return any(lo <= tick < hi for lo, hi in spans)


def lead_mode(text):
    """Step F0: 'on' / 'off' from the server's start line, None unknown."""
    m = RE_LEAD_MODE.search(text)
    return m.group(1) if m else None


def life_journals(case_dir, i, lives):
    """Step F0: (journal, from tick, to tick) per life of client i: the first
    life's client{i}.bmjr up to the second life's base, a rejoin's next life
    client{i}.rejoin{k}.bmjr from its own base on."""
    bases = [lf['base'] for lf in lives[1:]]
    out = []
    first = case_dir / f'client{i}.bmjr'
    if first.exists():
        out.append((first, -(1 << 31), bases[0] if bases and bases[0] is not None else 1 << 31))
    for k, base in enumerate(bases, start=1):
        nxt = bases[k] if k < len(bases) and bases[k] is not None else 1 << 31
        path = case_dir / f'client{i}.rejoin{k}.bmjr'
        if path.exists() and base is not None:
            out.append((path, base, nxt))
    return out


def event_report(args, case, case_dir):
    """H6.  None when the run had no episode to report on."""
    import first_divergence as fd
    import journal_trace as jt
    logs = {i: read_client_log(case_dir / f'client{i}.log') for i in range(args.players)}
    ep = episode(args, logs)
    if ep is None or not case.get('journal'):
        return None
    t_on, t_off, event_clients = ep['t_on'], ep['t_off'], ep['clients']
    server = jt.read_journal(case['journal'][-1])
    ticks = [g.tick for g in server.ticks if g.has_tick]
    first, last = (ticks[0], ticks[-1]) if ticks else (0, 0)
    phases = {'B': (first, t_on), 'D': (t_on, t_off + REACH), 'A': (t_off + REACH, last + 1)}
    id_of = {i: logs[i]['own_id'] for i in logs}
    index_of = {pid: i for i, pid in id_of.items() if pid}
    arm = infer_arm(args, ep['source'])
    family = ARM_FAMILY.get(arm, 'burst')
    s1 = lph.parse_s1(server_log_text(case_dir), server.header['session'])
    client_cadence = {}
    for i in range(args.players):
        path = case_dir / ('client%d.bmjr' % i)
        client_cadence[i] = lph.cadence(jt.read_journal(path).tick_records()) if path.exists() else None
    cadence_by_tick = {r['tick']: lph.cadence([g.record for g in server.ticks if g.has_tick
                                             and r['tick'] - 659 <= g.tick <= r['tick']])
                       for r in s1.get('lateness') or [] if 'tick' in r}
    clock_fault = lph.host_clock(s1, client_cadence, {k: v for k, v in cadence_by_tick.items() if v is not None})
    if family == 'clock' and clock_fault['spans']:
        t_on, t_off = clock_fault['spans'][0]['first'], clock_fault['spans'][-1]['last'] or last
        phases = {'B': (first, t_on), 'D': (t_on, t_off + REACH), 'A': (t_off + REACH, last + 1)}
    outside_faults = clock_fault['spans'] if family != 'clock' else []
    def fault_overlap(lo, hi):
        return any(lo <= (span['last'] or last) and hi >= span['first'] for span in outside_faults)
    # A control has no late player: every client is one of "the others".
    late_ids = set() if family == 'control' else {id_of[i] for i in event_clients if id_of.get(i)}
    report = {'episode': ep['describe'], 'source': ep['source'], 'arm': arm, 'family': family,
              'drop': ep.get('drop', False), 't_on': t_on, 't_off': t_off, 'phases': phases,
              'event_clients': event_clients, 'server_journal': case['journal'][-1], 'clients': [],
              's1': s1, 'host_clock': clock_fault,
              'requested_flags': {'patience': args.patience, 'lead_adjust': args.lead_adjust},
              'window_origin': getattr(args, 'window_origin', 0), 'input_delay': args.input_delay,
              # A5u/A5s: the uplink step K is judged against
              'step_ms': max([ev.ms for ev in (getattr(args, 'events', None) or []) if ev.kind == 'step'] or [0.0])}
    if getattr(args, 'join_late', None):
        links = client_links(args)
        report['join'] = [{'client': i, 'asked': asked, 'base': (logs[i]['bases'] or [None])[0],
                           'latency_ms': links.get(i, {}).get('latency_ms', 0.0)}
                          for i, asked in sorted(args.join_late.items())]

    # The server: substituted ticks per player, TICK gaps, late events.
    substituted = collections.defaultdict(list)
    for g in server.ticks:
        for rec in g.inputs:
            if not rec['flags'] & 1:
                substituted[rec['id']].append(g.tick)
    gaps = {p: [] for p in PHASES}
    gaps30 = {}
    previous = None
    for g in server.ticks:
        if not g.has_tick:
            continue
        if previous is not None and g.tick == previous[0] + 1:
            gap = g.record['ms'] - previous[1]
            phase = phase_of(g.tick, phases)
            if phase:
                gaps[phase].append((gap, g.tick))
            start = g.tick - g.tick % 30
            gaps30[start] = max(gaps30.get(start, 0), gap)
        previous = (g.tick, g.record['ms'])
    tick_gaps = {}
    for phase, values in gaps.items():
        if values:
            ordered = sorted(v for v, _ in values)
            worst = max(values)
            tick_gaps[phase] = {'max_ms': worst[0], 'at': worst[1], 'p99_ms': ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))]}
    # The stamp is the origin since journal v3 (protocol 9: a late event is
    # re-stamped to the tick the server applies it at, event_tick included);
    # a version 2 journal has it in event_tick.
    late_events = [{'tick': e['tick'], 'stamp': e.get('origin_tick', e['event_tick']), 'type': e['type'],
                    'player': e['id'], 'client': index_of.get(e['id']), 'state': e.get('state', 0)}
                   for e in server.events() if e['tick'] != e.get('origin_tick', e['event_tick'])]
    server_log = case_dir / 'server.log'
    late_by_lines, barrier_lines, s5_lines, bodiless = [], [], [], []
    counted_through = {}   # resync base -> the old epoch's last counted tick
    # SessionLeads sent to this run's players and their acks (an external
    # server's log holds other runs too: kept to this run's ids and ticks).
    leads = []
    if server_log.exists():
        for line in server_log.read_text(errors='replace').splitlines():
            m = RE_RESYNCED.search(line)
            if m:
                counted_through[int(m.group(1))] = int(m.group(2))
            m = RE_LEAD_SENT.search(line)
            if m and int(m.group(1)) in index_of and first <= int(m.group(7)) <= last:
                sector_bound = re.search(r'\bsector-bound=(-?\d+)', line)
                offset = re.search(r'\boffset=(\d+)', line)
                leads.append({'player': int(m.group(1)), 'client': index_of[int(m.group(1))], 'id': int(m.group(2)),
                              'k': int(m.group(3)),
                              'sector_bound': int(sector_bound[1]) if sector_bound else None,
                              'offset': int(offset[1]) if offset else None,
                              # the meter's k before a cap cut it (= k when uncapped)
                              'decided': int(m.group(4)) if m.group(4) is not None else int(m.group(3)),
                              'binding': m.group(6), 'n': int(m.group(7)), 'held_from': int(m.group(8)),
                              'e': int(m.group(9)), 'dropped': int(m.group(10)), 'applied': None})
            m = RE_LEAD_ACK.search(line)
            if m:
                for lead in leads:
                    if lead['player'] == int(m.group(1)) and lead['id'] == int(m.group(2)):
                        lead['applied'] = int(m.group(3))
            m = RE_LATE_BY.search(line)
            if m:
                late_by_lines.append({'type': int(m.group(1)), 'player': int(m.group(2)), 'stamp': int(m.group(3)),
                                      'tick': int(m.group(4)), 'late_by': int(m.group(5))})
            m = RE_BARRIER.search(line)
            if m:
                barrier_lines.append({'player': int(m.group(1)), 'tick': int(m.group(2))})
            m = RE_S4.search(line)
            if m and int(m.group(1)) in index_of and first <= int(m.group(2)) <= last:
                bodiless.append({'player': int(m.group(1)), 'client': index_of[int(m.group(1))],
                                 'first': int(m.group(2)), 'last': int(m.group(3)), 'n': int(m.group(4)),
                                 'open': bool(m.group(5))})
            # This run's players and ticks only (an external server's log
            # holds other runs too).
            m = RE_S5.search(line)
            if m and int(m.group(1)) in index_of and first <= int(m.group(4)) <= last:
                s5_lines.append({'player': int(m.group(1)), 'type': m.group(2), 'origin': int(m.group(3)),
                                 'applied': int(m.group(4)), 'late': int(m.group(5)), 'delay_us': int(m.group(6)),
                                 'delay_ticks': float(m.group(7)), 'to': int(m.group(8)), 'flag': m.group(9) or ''})
    report['server'] = {'ticks': [first, last], 'tick_gaps': tick_gaps, 'late_events': late_events,
                        'late_by_lines': late_by_lines, 'barrier_lines': barrier_lines, 'leads': leads,
                        'bodiless': bodiless,
                        's5': s5_summary(s5_lines), 's5_superseded': sum(1 for e in s5_lines if e['flag'] == 'superseded'),
                        # the whole run in 30-tick buckets, so a later arm can
                        # re-cut this run's gaps on its own phases
                        'gaps30': sorted([s, v] for s, v in gaps30.items())}

    server_world = fd.worlds(server, want_local=False)
    # Step F0: the airship spans per player (server log): never compared.
    airship = airship_spans(server_log_text(case_dir))
    report['lead_adjust'] = lead_mode(server_log_text(case_dir))
    report['relay_episodes'] = [dict(tick=tick, clients=sorted({event.client for event in args.events if event.tick == tick}))
                                for tick in sorted({event.tick for event in args.events})]
    for i in range(args.players):
        log = logs[i]
        pid = id_of.get(i, 0)
        path = case_dir / f'client{i}.bmjr'
        lives = log.get('lives') or []
        row = {'index': i, 'id': pid, 'base': log['bases'][0] if log['bases'] else None,
               # step F0: a join import (a late joiner's or a rejoin's first
               # state) is not a resync; a later life's base is a rejoin base
               'resync_bases': [b for lf in lives for b in lf['resyncs']] if lives else log['bases'][1:],
               'rejoin_bases': [lf['base'] for lf in lives[1:] if lf['base'] is not None],
               'join_resyncs_removed': log.get('join_resyncs_removed', 0),
               'subst_client': log['subst'], 'lead_adj': log['lead_adj'],
               'holds': log['holds'], 'stalls': log['stalls'], 'input_delays': log['input_delays'],
               'echo': log['echo'], 'ownlife': log['ownlife'], 'own_moves': log['own_moves'],
               'own_deaths': log.get('own_deaths', []),
               'status_lead': log['status_lead'], 'never_started': not log['exists'], 'phases': {},
               'relay_samples': relay_samples(path) if path.exists() else []}
        report['clients'].append(row)
        row['host_clock_resync_bases'] = [tick for tick in row['resync_bases'] if fault_overlap(tick, tick)]
        row['resync_bases'] = [tick for tick in row['resync_bases'] if not fault_overlap(tick, tick)]
        base = row['base'] if row['base'] is not None else first
        # The client's own phases: from its base (a late joiner's B is empty),
        # to a second before its last line, past its exit's own effects.
        end = min(last, log['last_tick']) - 60 if log['last_tick'] else last
        cphases = client_phases(phases, base, end)
        row['span'] = [base, end]
        # Substitutions inside the client's span (the ticks before its base
        # are applied held by construction and precede its numbering).
        mine_all = [t for t in substituted.get(pid, []) if base <= t <= end]
        row['clock_outside_substitutions'] = [t for t in mine_all if t < t_on or t > t_off + 2] if family == 'clock' else None
        row['host_clock_substitutions'] = [t for t in mine_all if fault_overlap(t, t)]
        mine = [t for t in mine_all if not fault_overlap(t, t)]
        # ...and so are the ones a resync re-anchors: after the old epoch's
        # last counted tick and before the new base (they precede the new
        # numbering; the client is told nothing of them).  Without this the
        # A3 equality "recv + late == server count" fails in any arm with a
        # resync.  A server older than the log field leaves them in.
        unnumbered = [(counted_through[b], b) for b in row['resync_bases'] if b in counted_through]
        row['subst_unnumbered'] = sum(1 for t in mine if any(lo < t < b for lo, b in unnumbered))
        mine = [t for t in mine if not any(lo < t < b for lo, b in unnumbered)]
        row['subst_server'] = {p: sum(1 for t in mine if cphases[p][0] <= t < cphases[p][1]) for p in PHASES}
        row['subst_total'] = len(mine)
        # The client's subst= counts to its last tick, past the span's end:
        # its count lies between the span's and this one (LBM4 swing seeds).
        row['subst_total_full'] = row['subst_total'] + sum(
            1 for t in substituted.get(pid, []) if end < t <= end + 60 and not fault_overlap(t, t))
        row['substituted_ticks'] = mine
        row['subst_outside_event'] = sum(1 for t in mine if not t_on <= t <= t_off + 2)
        runs = []
        for t in mine:
            if runs and t == runs[-1][1] + 1:
                runs[-1][1] = t
            else:
                runs.append([t, t])
        row['subst_runs'] = [r for r in runs if r[1] >= t_on - 60 and r[0] <= t_off + 120][:20]

        # Errors against the server's checkpoints, per tick.
        per_tick = {}
        presence = {}   # tick -> (server has the own ball, client has it)
        received = []
        # Step F0: every life's journal (a rejoin's next life writes
        # client{i}.rejoin{k}.bmjr), each from its own base to the next
        # life's; the ticks before a join's import are not compared (the
        # joiner's world is not yet the session's), nor a ball in its
        # airship span.
        local = {}
        excluded = {'pre_import': [], 'airship': []}
        for k, (jpath, lo, hi) in enumerate(life_journals(case_dir, i, lives)):
            journal = jt.read_journal(str(jpath))
            world = fd.worlds(journal, want_local=True)
            received += [cp['tick'] for cp in journal.checkpoints() if cp['flags'] & 4 and lo <= cp['tick'] < hi]
            for tick, bodies in world.items():
                if lo <= tick < hi:
                    local[tick] = bodies
        for lf in lives:
            if lf['import'] is not None and lf['import_end'] is not None:
                lo_imp = lf['base'] if lf['base'] is not None else lf['import']
                dropped = [tk for tk in local if lo_imp <= tk <= lf['import_end']]
                for tk in dropped:
                    del local[tk]
                excluded['pre_import'].append([lo_imp, lf['import_end'], len(dropped)])
        air_counted = collections.Counter()
        if path.exists() or local:
            for tick in sorted(set(server_world) & set(local)):
                a, b = server_world[tick], local[tick]
                presence[tick] = (('ball', pid) in a, ('ball', pid) in b)
                own = oth = late = mech = 0.0
                have_own = False
                for key in set(a) & set(b):
                    if key[0] == 'ball' and in_spans(airship.get(key[1], ()), tick):
                        air_counted[key[1]] += 1
                        continue
                    err = distance(a[key], b[key])
                    if key[0] == 'ball':
                        if key[1] == pid:
                            own, have_own = err, True
                        else:
                            oth = max(oth, err)
                            if key[1] in late_ids:
                                late = max(late, err)
                    else:
                        mech = max(mech, err)
                per_tick[tick] = (own if have_own else None, oth, late, mech)
        for owner, spans in sorted(airship.items()):
            if air_counted.get(owner):
                excluded['airship'].append(['own' if owner == pid else 'c%s' % index_of.get(owner, owner),
                                            [[lo, hi if hi < 1 << 31 else None] for lo, hi in spans],
                                            air_counted[owner]])
        row['excluded'] = excluded
        row['host_clock_rows'] = [[t, *v] for t, v in per_tick.items() if fault_overlap(t, t)]
        per_tick = {t: v for t, v in per_tick.items() if not fault_overlap(t, t)}
        window_lo, window_hi = t_on - 60, t_off + REACH + 180
        windows30 = []
        for start in range(window_lo - window_lo % 30, window_hi, 30):
            vals = [per_tick[t] for t in range(start, start + 30) if t in per_tick]
            if vals:
                windows30.append([start, max((v[0] or 0.0) for v in vals), max(v[1] for v in vals),
                                  max(v[2] for v in vals), max(v[3] for v in vals)])
        row['windows30'] = windows30
        # The whole run in 30-tick buckets (own, other balls, mechanisms), so
        # a later arm can re-cut this run's errors on its own phases.
        err30 = {}
        for t, v in per_tick.items():
            s = t - t % 30
            e = err30.setdefault(s, [s, None, 0.0, 0.0])
            if v[0] is not None:
                e[1] = max(e[1] or 0.0, v[0])
            e[2], e[3] = max(e[2], v[1]), max(e[3], v[3])
        row['err30'] = [err30[s] for s in sorted(err30)]
        own_bad = [t for t, v in per_tick.items() if t >= t_on and t <= end and v[0] is not None and v[0] >= ZERO_M]
        row['own_zero_from'] = (max(own_bad) + 2) if own_bad else t_on
        row['own_after'] = max([v[0] for t, v in per_tick.items() if t_off + REACH <= t <= end and v[0] is not None] or [0.0])
        row['late_mirror_after'] = max([v[2] for t, v in per_tick.items() if t_off + 60 <= t <= end] or [0.0])
        # The same over the 300 ticks after those points only: the whole rest
        # of a run also holds respawn piles that have nothing to do with the
        # episode (the 30-tick windows say which is which).
        row['own_after_300'] = max([v[0] for t, v in per_tick.items()
                                    if t_off + REACH <= t < t_off + REACH + 300 and t <= end and v[0] is not None] or [0.0])
        row['late_mirror_after_300'] = max([v[2] for t, v in per_tick.items()
                                            if t_off + 60 <= t < t_off + 360 and t <= end] or [0.0])
        # A3: the own ball 48 ticks after the client's last resync (or T_off).
        # A5u/A5s: this client's SessionLeads (the S1 lead lines), and from the
        # first raise's E: the substitutions outside [T_on, E], the first tick
        # at or after E the server applied fresh, and the own ball from E+48.
        row['leads'] = [lead for lead in leads if lead['client'] == i]
        raises = [lead for lead in row['leads'] if lead['k'] > 0 and lead['held_from']]
        if raises:
            e = raises[0]['e']
            held = set(mine)
            fresh_from_e = next((t for t in range(e, end + 1) if t not in held), None)
            row['lead_e'] = {'e': e, 'n': raises[0]['n'], 'subst_outside': sum(1 for t in mine if not t_on <= t <= e),
                             'first_fresh': fresh_from_e,
                             'own_after': max([v[0] for t, v in per_tick.items()
                                               if e + REACH <= t <= end and v[0] is not None] or [0.0])}
        # Plan step 19: the own ball from T'+48 on, T' the last tick the
        # server applied one of this client's own births at, later than
        # stamped (the client's own move line, else the server journal's
        # re-stamped Physicalize of this player).
        moved_to = [mv['applied'] for mv in log['own_moves'] if mv['applied'] is not None]
        if not moved_to:
            moved_to = [e['tick'] for e in late_events
                        if e['player'] == pid and e['type'] in (0, 'Physicalize', 'physicalize') and not e['state'] & 2]
        if moved_to:
            t_prime = max(moved_to)
            row['own_after_move'] = [t_prime, t_prime + REACH,
                                     max([v[0] for t, v in per_tick.items()
                                          if t_prime + REACH <= t <= end and v[0] is not None] or [0.0])]
        else:
            row['own_after_move'] = None
        settle = max([t_off] + [b for b in row['resync_bases'] if b >= t_on]) + REACH
        row['own_after_resync'] = [settle, max([v[0] for t, v in per_tick.items()
                                                if settle <= t <= end and v[0] is not None] or [0.0])]
        # A6 drop: the checkpoints from T_on+48 on where the client has its own
        # ball and the server has not, or the other way round.
        row['own_presence_mismatch'] = sum(1 for t, (srv, loc) in presence.items()
                                           if t_on + REACH <= t <= end and srv != loc)
        # ...and from 48 ticks after the owner gave its birth up as never
        # made (plan step 20: the missing echo's deadline, on the server's
        # tick from the relay header, or a REJECTED/SUPERSEDED echo), which is
        # where design 5.2 judges the drop: the own ball's presence on each
        # side and its error where both have it.
        never = [mv['at'] for mv in log['own_moves'] if mv['applied'] is None and mv['at'] >= t_on]
        if never:
            settle_never = min(never) + REACH
            after_never = [t for t in presence if settle_never <= t <= end]
            row['own_never'] = {
                'at': min(never), 'from': settle_never, 'compared': len(after_never),
                'mismatch': sum(1 for t in after_never if presence[t][0] != presence[t][1]),
                'server_has': sum(1 for t in after_never if presence[t][0]),
                'client_has': sum(1 for t in after_never if presence[t][1]),
                'own_err': max([v[0] for t, v in per_tick.items() if settle_never <= t <= end and v[0] is not None]
                               or [0.0])}
        else:
            row['own_never'] = None
        row['received_in_event'] = sum(1 for t in received if t_on <= t <= t_off + REACH)
        row['received_total'] = len(received)

        # The window: lines, kept raw too: a control's are re-cut on the
        # phases of the arm it is compared with (recut_phases).
        row['host_clock_windows'] = [w for w in log['windows'] if fault_overlap(w.get('lo', w['tick'] - 59), w['tick'])]
        row['raw_windows'] = [w for w in log['windows'] if not fault_overlap(w.get('lo', w['tick'] - 59), w['tick'])]
        for p in PHASES:
            lo, hi = cphases[p]
            vals = [v for t, v in per_tick.items() if lo <= t < hi]
            ph = {'ticks': [lo, hi], 'compared': len(vals),
                  'own_max': max([v[0] for v in vals if v[0] is not None] or [0.0]),
                  'oth_max': max([v[1] for v in vals] or [0.0]),
                  'late_mirror_max': max([v[2] for v in vals] or [0.0]),
                  'mech_max': max([v[3] for v in vals] or [0.0])}
            ph.update(aggregate([w for w in log['windows'] if window_phase(w, cphases) == p
                                 and not fault_overlap(w.get('lo', w['tick'] - 59), w['tick'])], late_ids, index_of))
            row['phases'][p] = ph
        # A snapshot rollback inside the episode's reach repairs the late
        # client by a path the criteria do not test: the run is masked.
        row['snap_rb_in_event'] = sum(
            w['snaprb'][0] for w in log['windows']
            if isinstance(w.get('snaprb'), list) and w['tick'] >= t_on and w.get('lo', w['tick'] - 59) <= t_off + REACH)
    return report


def load_controls(paths, sector):
    """The event reports of the control runs given with --control."""
    out = []
    for p in paths:
        path = pathlib.Path(p)
        if path.is_dir():
            path = path / 'summary.json'
        try:
            cases = json.loads(path.read_text(encoding='utf8'))
        except (OSError, ValueError):
            print('  note: control %s unreadable' % p, flush=True)
            continue
        for case in cases:
            if case.get('sector') == sector and case.get('event_report'):
                out.append(case['event_report'])
    return out


def normalize_window(w):
    """A window read back from JSON: its source-player keys are strings there."""
    w = dict(w)
    w['lag'] = {int(src): {int(lag): n for lag, n in hist.items()} for src, hist in (w.get('lag') or {}).items()}
    w['inrew_src'] = {int(src): n for src, n in (w.get('inrew_src') or {}).items()}
    return w


def recut_phases(rep, row, phases, late_indices):
    """A control row's phase figures cut on another run's phases: the same
    D length, the same windows-overlap rule, and "the late ball" being the
    ball of the same client index.  An older control without raw windows
    keeps its own phases (marked, so the criterion says so)."""
    if 'raw_windows' not in row:
        out = {p: dict(row['phases'][p]) for p in PHASES}
        out['recut'] = False
        return out
    ids = {r['index']: r['id'] for r in rep['clients']}
    index_of = {pid: i for i, pid in ids.items() if pid}
    late_ids = {ids[i] for i in late_indices if ids.get(i)}
    windows = [normalize_window(w) for w in row['raw_windows']]
    base, end = row['span']
    cph = client_phases(phases, base, end)
    out = {'recut': True}
    for p in PHASES:
        lo, hi = cph[p]
        ph = aggregate([w for w in windows if window_phase(w, cph) == p], late_ids, index_of)
        errs = [e for e in row.get('err30', []) if e[0] < hi and e[0] + 30 > lo]
        ph['mech_max'] = max([e[3] for e in errs] or [0.0])
        ph['oth_max'] = max([e[2] for e in errs] or [0.0])
        ph['own_max'] = max([e[1] for e in errs if e[1] is not None] or [0.0])
        out[p] = ph
    return out


def band_upper(controls, index, getter, phases, late_indices):
    """The control band's upper edge for client `index`, every control re-cut
    on `phases`: the most any control run had (the criteria are one-sided).
    (edge or None, whether every control could be re-cut)."""
    values, recut = [], True
    for rep in controls:
        for row in rep['clients']:
            if row['index'] == index:
                cut = recut_phases(rep, row, phases, late_indices)
                recut = recut and cut['recut']
                try:
                    v = getter(cut)
                except (KeyError, TypeError):
                    v = None
                if v is not None:
                    values.append(v)
    return (max(values) if values else None), recut


def control_gap_upper(controls, lo, hi):
    """The most any control's server waited between two ticks in [lo, hi)."""
    values = []
    for rep in controls:
        g = [v for s, v in rep['server'].get('gaps30', []) if s < hi and s + 30 > lo]
        if g:
            values.append(max(g))
    return max(values) if values else None


def criteria(report, case, controls):
    """H8: each criterion of design 5.2 that this run can decide, as
    (who, criterion, status, detail); status is pass, fail, masked or n/a.
    The others' criteria apply to every arm; the late client's are the arm's
    own (report['arm'], report['family'])."""
    out = []
    t_on, t_off = report['t_on'], report['t_off']
    family, arm = report.get('family', 'burst'), report.get('arm', 'A1')
    late = set() if family == 'control' else set(report['event_clients'])
    late_indices = sorted(report['event_clients'])
    phases = report['phases']

    def add(who, name, ok, detail, masked=False):
        status = 'n/a' if ok is None else ('masked' if masked and not ok else ('pass' if ok else 'fail'))
        out.append({'who': who, 'criterion': name, 'status': status, 'detail': detail})

    def band(index, getter):
        edge, recut = band_upper(controls, index, getter, phases, late_indices)
        return edge, '' if recut else ' (a control without raw windows: its own phases)'

    def subst_parts(row):
        return [int(v) for v in row['subst_client'].split('/')] if row['subst_client'] else None

    no_subst = 'no subst= counter at this revision'
    no_lead = 'no lead_adj counter at this revision'
    replays = case.get('replays', [])
    add('server', 'journal replays exactly',
        all(r['exit_code'] == 0 and 'first_divergence=-1' in r['summary'] for r in replays) if replays else None,
        '; '.join(r['summary'] for r in replays) or 'no replay')
    add('all', 'arm', None, '%s (%s criteria)' % (arm, family))
    clock = report.get('host_clock', {})
    if family == 'clock':
        add('server', 'clock fault actually observed', bool(clock.get('label')), str(clock.get('spans', [])))
    if clock.get('label'):
        add('all', 'host clock span recorded', all(span['last'] is not None for span in clock['spans']), str(clock['spans']))
        add('all', 'client cadence stayed on its real clock', clock['client_cadence_normal'], str(clock['client_tick_ms_p50']))
        add('all', 'ordinary arm criteria use rows outside host clock spans', None,
            'inside rows/substitutions retained separately; clock arms use their entire injected span')
    for row in report['clients']:
        who = 'client%d' % row['index']
        D = row['phases']['D']
        if row.get('never_started'):
            add(who, 'client ran', False, 'never started')
            continue
        lead = [row['phases'][p]['lead_adj'] for p in PHASES]
        lead_none = all(v is None for v in lead)
        if row['index'] not in late:
            for p in PHASES:
                add(who, 'own ball 0.0000 in %s' % p, row['phases'][p]['own_max'] < ZERO_M,
                    '%.4f m' % row['phases'][p]['own_max'])
            add(who, 'relay_late 0', sum(row['phases'][p]['relay_late'] for p in PHASES) == 0,
                'B/D/A %s' % '/'.join(str(row['phases'][p]['relay_late']) for p in PHASES))
            add(who, 'resyncs 0', sum(row['phases'][p]['resyncs'] for p in PHASES) == 0 and not row['resync_bases'],
                '%d (bases %s)' % (sum(row['phases'][p]['resyncs'] for p in PHASES), row['resync_bases']))
            add(who, 'lead_adj 0', None if lead_none else sum(v or 0 for v in lead) == 0, no_lead if lead_none else str(lead))
            if late:
                add(who, 'late ball mirror <= 0.001 m from T_off+60', row['late_mirror_after'] <= 0.001,
                    '%.4f m (%.4f m in the 300 ticks after)' % (row['late_mirror_after'], row['late_mirror_after_300']))
            if controls:
                for p in PHASES:
                    edge, note = band(row['index'], lambda c: c[p]['mech_max'])
                    add(who, 'mechanism error in %s within control' % p,
                        None if edge is None else row['phases'][p]['mech_max'] <= edge + 0.0005,
                        '%.4f vs <= %s%s' % (row['phases'][p]['mech_max'], 'n/a' if edge is None else '%.4f' % edge, note))
                # The re-simulated ticks, not the rollbacks: the CPU spent
                # (the user's decisions after LBM4 - A8's joiner frames
                # arriving one by one cost one rollback more, fewer ticks;
                # and a tick per rollback, its message's arrival jitter, as
                # the relay lag's rest within a tick).
                for cls in ('input', 'life', 'snap', 'forced'):
                    edge, note = band(row['index'], lambda c: c['D']['rb'][cls][1])
                    add(who, '%s rollback ticks in D within control' % cls,
                        None if edge is None else D['rb'][cls][1] <= edge + D['rb'][cls][0],
                        '%d ticks in %d rollbacks vs <= %s + 1 a rollback%s' % (D['rb'][cls][1], D['rb'][cls][0], edge, note))
                if late:
                    # A count too: one more is no failure while the input
                    # rollbacks' ticks stay within control (LBM5 A8: the
                    # joiner's frames one by one, 3 vs 2, fewer ticks).
                    edge, note = band(row['index'], lambda c: c['D']['inrew_late'])
                    ticks, _ = band(row['index'], lambda c: c['D']['rb']['input'][1])
                    cheaper = ticks is not None and D['rb']['input'][1] <= ticks + D['rb']['input'][0]
                    add(who, 'input rewinds naming the late ball in D within control',
                        None if edge is None else D['inrew_late'] <= edge or cheaper,
                        '%d vs <= %s%s%s' % (D['inrew_late'], edge, note,
                                            '; input rollback ticks within control' if cheaper else ''))
                lag_phases = ('D', 'A') if family == 'lead' else ('D',)
                for p in lag_phases:
                    if p == 'D':
                        continue
                    st_all = row['phases'][p]['lag']
                    edge, note = band(row['index'], lambda c: c[p]['lag']['p99'])
                    add(who, 'relay lag p99 in %s within control' % p, None if edge is None else st_all['p99'] <= edge,
                        '%d vs <= %s%s' % (st_all['p99'], edge, note))
                    for src, st in sorted(row['phases'][p]['lag_src'].items()):
                        edge, note = band(row['index'], lambda c: c[p]['lag_src'][src]['p99'])
                        add(who, 'relay lag p99 in %s from %s within control' % (p, src),
                            None if edge is None else st['p99'] <= edge, '%d vs <= %s%s' % (st['p99'], edge, note))
            else:
                add(who, 'mechanism error / rollbacks / relay lag within control', None, 'no --control given')
            for result in relay_start_hold(report, row, controls, control=family == 'control'):
                relay_rule = 'A3m band + 22 until last E, band from E + 48' if arm.upper() == 'A3M' else 'one initial patience hold'
                add(who, 'relay lag in D: %s (source %s)' % (relay_rule, result['source']),
                    result['ok'], str(result))
            if family == 'control':
                # A0/A0j: substituted ticks per player <= HEAD + 0.5 % of the
                # ticks (the controls given are the HEAD runs), moved births 0.
                span = max(1, row['span'][1] - row['span'][0])
                head =[r['subst_total'] for rep in controls for r in rep['clients'] if r['index'] == row['index']]
                add(who, 'substituted ticks <= HEAD + 0.5 %',
                    None if not head else row['subst_total'] <= max(head) + 0.005 * span,
                    '%d of %d ticks vs HEAD %s' % (row['subst_total'], span, max(head) if head else 'n/a (no --control)'))
                moved = [row['phases'][p]['moved'] for p in PHASES]
                add(who, 'moved births 0', None if all(v is None for v in moved) else sum(v or 0 for v in moved) == 0,
                    'no moved counter at this revision' if all(v is None for v in moved) else str(moved))
            continue

        # The late client: the arm's own criteria.
        masked = row['snap_rb_in_event'] > 0 or row['received_in_event'] > 0
        mask_note = ' (masked: %d snapshot rollbacks, %d RECEIVED checkpoints in [T_on, T_off+48])' % (
            row['snap_rb_in_event'], row['received_in_event']) if masked else ''
        parts = subst_parts(row)
        resyncs = len(row['resync_bases'])
        if family == 'burst':
            add(who, 'substituted ticks only in [T_on, T_off+2]',
                None if arm == 'A4' else row['subst_outside_event'] == 0,
                ('a whole-run loss has no burst window; ' if arm == 'A4' else '')
                + '%d of %d outside [%d, %d]; runs %s' % (row['subst_outside_event'], row['subst_total'], t_on, t_off + 2,
                                                          row['subst_runs']))
            add(who, 'subst recv == server count, subst late 0',
                None if not parts or len(parts) < 6 else
                (row['subst_total'] <= parts[0] <= row.get('subst_total_full', row['subst_total']) and parts[5] == 0),
                no_subst if row['subst_client'] is None else 'subst=%s vs server %d..%d' % (
                    row['subst_client'], row['subst_total'], row.get('subst_total_full', row['subst_total'])))
            add(who, 'own ball 0.0000 from T_off+48', row['own_after'] < ZERO_M,
                '%.4f m (%.4f m in the 300 ticks after); own error last >= 0.00005 m before tick %d (T_off+%d)%s' % (
                    row['own_after'], row['own_after_300'], row['own_zero_from'], row['own_zero_from'] - t_off,
                    mask_note), masked)
            add(who, 'lead_adj 0', None if lead_none else sum(v or 0 for v in lead) == 0, no_lead if lead_none else str(lead))
        elif family == 'a3':
            add(who, 'subst recv + subst late == server count',
                None if not parts or len(parts) < 6 else row['subst_total'] <= parts[0] + parts[5] <= row.get('subst_total_full', row['subst_total']),
                no_subst if row['subst_client'] is None else 'subst=%s vs server %d..%d' % (
                    row['subst_client'], row['subst_total'], row.get('subst_total_full', row['subst_total'])))
            add(who, 'exactly one resync if subst late > 0',
                None if not parts or len(parts) < 6 else (resyncs == 1 if parts[5] > 0 else True),
                (no_subst + '; ' if not parts else '') + '%d resyncs (bases %s)' % (resyncs, row['resync_bases']))
            settle, err = row['own_after_resync']
            add(who, 'own ball 0.0000 48 ticks after the resync (or T_off)', err < ZERO_M,
                '%.4f m from tick %d%s' % (err, settle, mask_note), masked)
        elif family == 'reported':
            add(who, 'reported only (%s)' % arm, None,
                'own after T_off+48 %.4f m; relay_late B/D/A %s; resyncs %d; subst %d (outside the window %d)' % (
                    row['own_after'], '/'.join(str(row['phases'][p]['relay_late']) for p in PHASES), resyncs,
                    row['subst_total'], row['subst_outside_event']))
        elif family == 'lead' and arm == 'A5UB' and row['index'] == 0:
            spike = next(event for link in case['link_events'] for event in link['events']
                         if event['client'] == row['index'] and event['kind'] == 'spike')
            add(who, 'spike client cut only inside the spike, not raised',
                None if 'substituted_ticks' not in row else all(spike['tick'] <= tick <= spike['t_off'] + 2
                                                              for tick in row['substituted_ticks'])
                and not row.get('leads') and (lead_none or sum(v or 0 for v in lead) == 0),
                'subst runs %s; leads %s' % (row['subst_runs'], row.get('leads')))
        elif family == 'lead':
            # K from the server's S1 lead line (plan step 10), judged against
            # the uplink step; the client's lead_adj counts what it applied.
            sent = row.get('leads', [])
            k_lo = int(math.ceil(report.get('step_ms', 0.0) / 1000.0 / TICK_S))
            # K is the meter's decision: a cap (the total, the Sector margin)
            # cuts what is sent below it, and is shown, not misread as a
            # wrong K
            ks = [lead.get('decided', lead['k']) for lead in sent]
            capped = ['%d->%d (%s)' % (lead['decided'], lead['k'], lead['binding'] or '?')
                      for lead in sent if lead.get('decided', lead['k']) != lead['k']]
            one = (not lead_none and sum(v or 0 for v in lead) == 1)
            add(who, 'exactly one lead_adj, K in [ceil(up_ms/15), +4]',
                None if lead_none and not sent else (one and len(ks) == 1 and k_lo <= ks[0] <= k_lo + 4),
                '%s; lead_adj B/D/A %s; K %s vs [%d, %d]%s; acked %s' % (
                    'S1 lead lines: none (no --server-log, or a server before step 10)' if not sent else '%d sent' % len(sent),
                    no_lead if lead_none else '/'.join(str(v) for v in lead), ks, k_lo, k_lo + 4,
                    '; capped %s' % ', '.join(capped) if capped else '',
                    [lead['applied'] for lead in sent]))
            le = row.get('lead_e')
            resets = [lead for lead in sent if lead['held_from']]
            add(who, 'exactly one buffer reset, E >= the first on-time frame after the slew',
                None if not sent else (len(resets) == 1 and le is not None and le['first_fresh'] == le['e']),
                'no S1 lead line' if not sent else '%d resets; %s' % (
                    len(resets), 'no raise' if le is None else 'N %d E %d, first tick applied fresh from E: %s (dropped %d)' % (
                        le['n'], le['e'], le['first_fresh'], resets[0]['dropped'] if resets else 0)))
            add(who, 'substitutions only in [T_on, E]', None if le is None else le['subst_outside'] == 0,
                'no E (no raise in the S1 lines); substituted %d (B/D/A %s)' % (
                    row['subst_total'], '/'.join(str(row['subst_server'][p]) for p in PHASES)) if le is None else
                '%d of %d outside [%d, %d]; runs %s' % (le['subst_outside'], row['subst_total'], t_on, le['e'],
                                                        row['subst_runs']))
            add(who, 'own ball 0.0000 from E+48', None if le is None else le['own_after'] < ZERO_M,
                ('no E; own after T_off+48 %.4f m' % row['own_after']) if le is None else
                '%.4f m from tick %d%s' % (le['own_after'], le['e'] + REACH, mask_note), masked)
            add(who, 'resyncs 0', resyncs == 0 and sum(row['phases'][p]['resyncs'] for p in PHASES) == 0,
                '%d (bases %s)' % (resyncs, row['resync_bases']))
            add(who, "the session's input delay printed once", len(row['input_delays']) == 1,
                'printed %s' % row['input_delays'])
        elif family == 'a5d':
            add(who, 'one raise and one lower', None if lead_none else sum(v or 0 for v in lead) == 2,
                no_lead if lead_none else str(lead))
        elif family == 'hold':
            gap = report['server']['tick_gaps'].get('D', {})
            lo, hi = phases['D']
            edge = control_gap_upper(controls, lo, hi) if controls else None
            # With patience on the server may wait its M for a frame (the
            # user kept that wait after LBM4: lbm4_a6b's 60 ms at tick 1541
            # was client2's frame, late behind its own 40-tick rewind).
            slack = QUORUM_LAST_MARGIN * TICK_S * 1000 if report.get('requested_flags', {}).get('patience') == 'on' else 15
            add(who, 'largest server TICK gap near the hold <= control + %s' % ('M ticks' if slack != 15 else '15 ms'),
                None if edge is None or not gap else gap['max_ms'] <= edge + slack,
                '%s ms at tick %s vs control %s' % (gap.get('max_ms'), gap.get('at'), 'n/a' if edge is None else edge))
            near = [b for b in report['server']['barrier_lines'] if lo <= b['tick'] < hi]
            add(who, 'no barrier line', not near, '%d in D (%d in the run)' % (len(near), len(report['server']['barrier_lines'])))
            if resyncs:
                # Plan step 20 (step-20 review): a resync must not leave the
                # owner's relay lag past where the start put it.  In
                # lp19b_a6_1500 it went 33 -> 49 (p99, B -> A): the session
                # stalled 22 ticks on the deadlines of the ticks the resync
                # skipped, and the late-tick lead added 4 more.  4 ticks of
                # slack: the resync's anchoring error (A0j: up to 2.1 ticks)
                # and the SessionAssign's jitter.
                before = (row['phases'].get('B') or {}).get('lag') or {}
                after_lag = (row['phases'].get('A') or {}).get('lag') or {}
                ok = None if before.get('p99') is None or after_lag.get('p99') is None \
                    else after_lag['p99'] <= before['p99'] + 4
                add(who, "owner's relay lag p99 after the resync <= before the event + 4", ok,
                    'B %s, A %s (max %s -> %s)' % (before.get('p99'), after_lag.get('p99'), before.get('max'),
                                                   after_lag.get('max')))
            if report.get('drop'):
                nv = row.get('own_never')
                add(who, 'drop: the owner gave the birth up (missing or rejected echo)', nv is not None,
                    'own moves %s; echo acked/moved/superseded/rejected/missing %s' % (
                        row.get('own_moves') or '-', row.get('echo')))
                add(who, 'drop: the owner matches the server within 48 ticks of that',
                    (row['own_presence_mismatch'] == 0 and row['own_after'] < ZERO_M) if nv is None
                    else nv['mismatch'] == 0 and nv['own_err'] < ZERO_M,
                    ('no decision; %d checkpoints from T_on+48 where the own ball exists on one side only' %
                     row['own_presence_mismatch']) if nv is None else
                    ('given up at tick %d; from %d: %d of %d checkpoints with the own ball on one side only '
                     '(server has it %d, client %d), own error %.4f m; %d mismatched from T_on+48 in all' % (
                         nv['at'], nv['from'], nv['mismatch'], nv['compared'], nv['server_has'], nv['client_has'],
                         nv['own_err'], row['own_presence_mismatch'])))
            elif arm == 'A6M' and any('unphysicalize' in value.lower() or 'pair:' in value.lower()
                                      for value in report.get('episode', [])):
                deaths = row.get('own_deaths', [])
                paired = any(hold['type'].lower() == 'physicalize' for hold in row['holds'])
                add(who, 'ODM superseded death kept' if paired else 'ODM lone death moved inside history',
                    any(('nothing to move' if paired else 'rewound to') in death['outcome'] for death in deaths),
                    str(deaths))
                add(who, 'ODM owner no resync', resyncs == 0, str(row['resync_bases']))
                add(who, 'ODM branch converges after move or resync', row['own_after'] < ZERO_M
                    and row['own_presence_mismatch'] == 0,
                    'own after %.4f m; presence mismatches %d' % (row['own_after'], row['own_presence_mismatch']))
            elif arm == 'A6M':
                # The move inside the reach (plan step 19): the owner rewinds
                # to T-1 and makes its ball again at T', no resync.
                moves = row.get('own_moves') or []
                rewound = [mv for mv in moves if mv['how'] == 'rewound' and mv['applied'] is not None]
                add(who, 'owner moved its birth inside the history', bool(rewound),
                    'moves %s; echo acked/moved/superseded/rejected/missing %s; ownlife %s' % (
                        moves, row.get('echo'), row.get('ownlife')))
                add(who, 'owner no resync', resyncs == 0, '%d (bases %s)' % (resyncs, row['resync_bases']))
                after = row.get('own_after_move')
                add(who, "own ball 0.0000 from T'+48", None if after is None else after[2] < ZERO_M,
                    "no moved birth found (client log or server journal)" if after is None
                    else "%.4f m from tick %d (T' %d)" % (after[2], after[1], after[0]))
            else:
                # At 150 ms each way the move is past the owner's reach
                # (design 3.3) unless the hold is short: lp19b_a6_300 fitted
                # inside it, and a move inside the reach with no resync is
                # the better outcome, so it passes too (plan step 20).
                moves = row.get('own_moves') or []
                rewound = [mv for mv in moves if mv['how'] == 'rewound' and mv['applied'] is not None]
                inside = resyncs == 0 and bool(rewound)
                add(who, 'owner exactly one resync (past its reach at 150 ms), or the move inside the reach',
                    resyncs == 1 or inside,
                    '%d (bases %s)%s; own moves %s' % (resyncs, row['resync_bases'],
                                                      ', moved inside the reach' if inside else '', moves))
                after = row.get('own_after_move')
                if inside and after is not None:
                    add(who, "own ball 0.0000 from T'+48 (the move inside the reach)", after[2] < ZERO_M,
                        "%.4f m from tick %d (T' %d)" % (after[2], after[1], after[0]))
                else:
                    # Expected to fail at step 20 with snapshots off: the
                    # resync applies the server's state of its own tick S at
                    # the client's base B > S, and nothing tells the client
                    # what the server did from S to B (plan step 25).
                    add(who, 'own ball 0.0000 48 ticks after the resync (or the release) [exact resync: plan step 25]',
                        row['own_after_resync'][1] < ZERO_M,
                        '%.4f m from tick %d' % (row['own_after_resync'][1], row['own_after_resync'][0]))
        elif family == 'join':
            info = next((j for j in report.get('join', []) if j['client'] == row['index']), None)
            # Step F0 (WGQ7's re-read): D + ceil(rtt) + 2 is the joiner's
            # lead with physics.lead_adjust ON; with it off (the default) a
            # joiner runs at the session's input delay, as every member.
            mode = report.get('lead_adjust')
            founders = [r['input_delays'][0] for r in report['clients']
                        if r['index'] not in late and r.get('input_delays')]
            session_d = founders[0] if founders else report.get('input_delay', 0)
            if info and row['input_delays'] and mode == 'off':
                add(who, "joiner lead == the session's input delay (lead_adjust off)",
                    row['input_delays'][0] == session_d,
                    'printed input delay %d vs the founders\' %d (status lead %s; asked tick %d, base %s)' % (
                        row['input_delays'][0], session_d, row['status_lead'], info['asked'], info['base']))
            elif info and row['input_delays']:
                measured = next((sample for sample in report.get('s1', {}).get('lateness') or []
                                 if sample.get('player') == row['id'] and isinstance(sample.get('rtt'), (int, float))
                                 and 'assigned' in sample), {})
                rtt = int(math.ceil(measured.get('rtt', 0) / 1000.0 / TICK_S))
                want = session_d + rtt + 2
                add(who, 'joiner lead == D + ceil(rtt) + 2 (lead_adjust %s)' % (mode or 'unknown: the on rule'),
                    None if not measured else measured['assigned'] == want,
                    'assigned lead %s vs %d + %d + 2 = %d (status lead %s; asked tick %d, base %s)' % (
                        measured.get('assigned'), session_d, rtt, want, row['status_lead'],
                        info['asked'], info['base']))
            else:
                add(who, 'joiner lead', None, 'no session line or no --join-late for it')
            add(who, 'subst late 0 after the first four windows', None if not parts or len(parts) < 6 else parts[5] == 0,
                no_subst if row['subst_client'] is None else 'subst=%s' % row['subst_client'])
        elif family == 'stall':
            add(who, 'lead_adj 0', None if lead_none else sum(v or 0 for v in lead) == 0, no_lead if lead_none else str(lead))
            ok = (row['subst_server']['D'] > 0 and row['own_after'] < ZERO_M) or resyncs > 0
            add(who, "the stall's ticks substituted and corrected, or a rebase and resync", ok,
                'substituted in D %d, own after T_off+48 %.4f m, resyncs %d, rebases in D %d' % (
                    row['subst_server']['D'], row['own_after'], resyncs, row['phases']['D']['rebases']))
        elif family == 'clock':
            add(who, 'substitutions confined to clock fault span + 2', not row['clock_outside_substitutions'],
                str(row['clock_outside_substitutions'][:20]))
            if arm == 'A9H8':
                add(who, 'lead_adj 0', None if lead_none else sum(v or 0 for v in lead) == 0,
                    no_lead if lead_none else str(lead))
            elif arm == 'A9H20':
                decisions = [decision for decision in report.get('s1', {}).get('decisions') or []
                             if decision.get('player') == row['id']]
                proposals = [decision for decision in decisions if decision['action'] == 'raise']
                acks = [decision for decision in decisions if decision['action'] == 'lead-ack']
                resets = [decision for decision in decisions if decision['action'] == 'reset']
                sent = [lead for lead in row.get('leads', []) if lead['k'] > 0]
                raises = [decision for decision in proposals if any(decision['tick'] + 1 == lead['n'] for lead in sent)]
                assigned = next((sample['assigned'] for sample in report.get('s1', {}).get('lateness') or []
                                 if sample.get('player') == row['id'] and 'assigned' in sample), None)
                add(who, 'A9h20 every raise is a rule decision within caps',
                    all(any(decision['tick'] + 1 == lead['n'] and decision['k'] >= lead['k']
                            and decision.get('rule') in ('relative', 'minority', 'absolute', 'shared-absolute')
                            for decision in proposals) and assigned is not None and lead.get('offset') is not None
                        and assigned + lead['offset'] + lead['k'] <= 80
                        and lead.get('sector_bound') is not None and lead['k'] <= lead['sector_bound'] for lead in sent),
                    'decisions %s; sent %s' % (raises, sent))
                add(who, 'A9h20 raises only inside the fault span',
                    all(t_on <= decision['tick'] <= t_off for decision in raises), str(raises))
                cooldown = 132 + 2 * max([decision['k'] for decision in raises] or [0])
                deadline = t_off + cooldown + 660 + 66
                lowered = [ack for ack in acks if t_off <= ack['tick'] <= deadline and ack.get('offset', 81) <= 2]
                settled = [sample for sample in report.get('s1', {}).get('lateness') or []
                           if sample.get('player') == row['id'] and t_off <= sample.get('tick', 0) <= deadline
                           and sample.get('offset', 81) <= 2]
                add(who, 'A9h20 offset <= 2 within cooldown + 660 + 66 after fault',
                    bool(lowered or settled),
                    'deadline %d; acks %s' % (deadline, acks))
                add(who, 'A9h20 no raise or resync after fault end + 48',
                    not any(decision['tick'] > t_off + 48 for decision in raises + resets),
                    'end+48 %d; resets %s' % (t_off + 48, resets))
                add(who, 'A9h20 resyncs and relay_late inside fault (reported)', None,
                    'resets %s; relay_late D %d' % (resets, row['phases']['D']['relay_late']))
        elif family in ('pair', 'minority', 'shared'):
            decisions = report.get('s1', {}).get('decisions')
            mine = [d for d in decisions or [] if d.get('player') == row['id'] and d.get('action') in ('raise', 'would-raise2')]
            add(who, 'one revision-3 raise decision', None if decisions is None else len(mine) == 1,
                'n/a: revision-3 decision counters not present' if decisions is None else str(mine))
            rule = {'pair': 'relative', 'minority': 'minority', 'shared': 'shared-absolute'}[family]
            low, high = (30, 32) if arm == 'A10X' else ((20, 22) if arm == 'A10' else (20, 24))
            sent = [lead for lead in row.get('leads', []) if lead['k'] > 0]
            add(who, 'one sent raise by %s, k in [%d, %d]' % (rule, low, high),
                None if report.get('lead_adjust') != 'on' else len(sent) == 1 and low <= sent[0]['k'] <= high
                and any(decision.get('rule') == rule and decision['tick'] + 1 == sent[0]['n'] for decision in mine),
                'sent %s; decisions %s' % (sent, mine))
            if arm == 'A10X':
                add(who, 'A10x at most one resync before shared raise',
                    None if report.get('lead_adjust') != 'on' else resyncs <= 1,
                    '%d resyncs; bases %s' % (resyncs, row['resync_bases']))
    return out


def format_report(report, crit):
    """The report as text: one line per client and phase, then the server and
    the criteria."""
    lines = ['event report: %s; arm %s; T_on %d, T_off %d; B < %d, D %d..%d, A >= %d%s' % (
        '; '.join(report['episode']), report.get('arm', '?'), report['t_on'], report['t_off'], report['phases']['B'][1],
        report['phases']['D'][0], report['phases']['D'][1] - 1, report['phases']['A'][0],
        ('; window grid from tick %d' % report['window_origin']) if report.get('window_origin') else '')]
    if report.get('host_clock', {}).get('label'):
        lines.append('host clock: %s; client tick ms p50 %s' %
                     (report['host_clock']['spans'], report['host_clock']['client_tick_ms_p50']))
    lines.append('S1 diagnostics: ' + ', '.join('%s=%s' % (key, 'n/a' if rows is None else len(rows))
                                                for key, rows in report.get('s1', {}).items()))
    for j in report.get('join', []):
        lines.append('join: client%d asked at host tick %d, tick base %s, link %g ms' % (
            j['client'], j['asked'], j['base'], j['latency_ms']))
    for row in report['clients']:
        if row.get('never_started'):
            lines.append('client%d never started' % row['index'])
            continue
        lines.append('client%d (id %d, base %s%s): server substituted B/D/A %s (runs near the event %s); client subst=%s; '
                     'own 0.0000 from tick %d; own after T_off+48 %.4f (next 300 ticks %.4f), '
                     'late mirror after T_off+60 %.4f (next 300 ticks %.4f); '
                     'RECEIVED in the episode %d; input delay %s' % (
                         row['index'], row['id'], row['base'],
                         ((', resynced to ' + ','.join(map(str, row['resync_bases']))) if row['resync_bases'] else '')
                         + ((', rejoined at ' + ','.join(map(str, row['rejoin_bases']))) if row.get('rejoin_bases') else ''),
                         '/'.join(str(row['subst_server'][p]) for p in PHASES), row['subst_runs'] or '-',
                         row['subst_client'] or 'n/a', row['own_zero_from'], row['own_after'], row['own_after_300'],
                         row['late_mirror_after'], row['late_mirror_after_300'], row['received_in_event'],
                         '/'.join(map(str, row.get('input_delays', []))) or '?'))
        for p in PHASES:
            ph = row['phases'][p]
            rb = ph['rb']
            fmt_rb = lambda v: 'n/a' if v is None else ('%d/%.1f' % (v[0], v[1] / v[0]) if v[0] else '0')
            lines.append('  %s %5d-%-5d own %.4f oth %.4f late %.4f mech %.4f | windows %d rb input %s life %s snap %s '
                         'forced %s subst %s co %s, naming late %d | lag %d/%d (n %d) %s | relay_late %d relay_covered %d snaps %d resyncs %d '
                         'rebases %d lead_adj %s moved %s' % (
                             p, ph['ticks'][0], ph['ticks'][1] - 1, ph['own_max'], ph['oth_max'], ph['late_mirror_max'],
                             ph['mech_max'], ph['windows'], fmt_rb(rb['input']), fmt_rb(rb['life']), fmt_rb(rb['snap']),
                             fmt_rb(rb['forced']), fmt_rb(rb['subst']),
                             'n/a' if ph.get('substco') is None else '/'.join(map(str, ph['substco'])),
                             ph['inrew_late'], ph['lag']['max'],
                             ph['lag']['p99'], ph['lag']['n'],
                             ' '.join('%s:%d/%d' % (src, st['max'], st['p99']) for src, st in ph['lag_src'].items()),
                             ph['relay_late'], ph.get('relay_covered', 0), ph['snaps'], ph['resyncs'], ph['rebases'],
                             'n/a' if ph['lead_adj'] is None else ph['lead_adj'],
                             'n/a' if ph['moved'] is None else ph['moved']))
        ex = row.get('excluded') or {}
        if ex.get('pre_import') or ex.get('airship') or row.get('join_resyncs_removed'):
            lines.append('  not compared (step F0): %s%s%s' % (
                '; '.join('before the join import %d..%d (%d checkpoints)' % tuple(e) for e in ex.get('pre_import', [])),
                ('; ' if ex.get('pre_import') else '') + '; '.join(
                    '%s ball in its airship span %s (%d checkpoints)' % (
                        who_, ','.join('%d..%s' % (lo, 'end' if hi is None else hi - 1) for lo, hi in spans), n)
                    for who_, spans, n in ex.get('airship', [])),
                '; join imports not counted as resyncs %d' % row['join_resyncs_removed']
                if row.get('join_resyncs_removed') else ''))
        if row['holds'] or row['stalls']:
            lines.append('  holds %s stalls %s' % (row['holds'], row['stalls']))
        if row.get('own_never'):
            nv = row['own_never']
            lines.append('  own birth given up at tick %d: from %d, %d of %d checkpoints with the own ball on one side '
                         'only (server has it %d, client %d), own error %.4f m' % (
                             nv['at'], nv['from'], nv['mismatch'], nv['compared'], nv['server_has'],
                             nv['client_has'], nv['own_err']))
        if row.get('echo') or row.get('own_moves'):
            after = row.get('own_after_move')
            lines.append("  own echoes acked/moved/superseded/rejected/missing %s; ownlife moved/removed/resynced/remade/other %s; "
                         "own moves %s; own after T'+48 %s" % (
                             '/'.join(map(str, row['echo'])) if row.get('echo') else 'n/a', row.get('ownlife') or 'n/a',
                             row.get('own_moves') or '-',
                             'n/a' if not after else "%.4f m from tick %d (T' %d)" % (after[2], after[1], after[0])))
    s = report['server']
    lines.append('server: ticks %d..%d; TICK gap max/p99 ms %s; late events %d %s; "late by" lines %d; barrier give-ups %d; '
                 'S4 bodiless runs %s' % (
        s['ticks'][0], s['ticks'][1],
        ' '.join('%s %d@%d/%d' % (p, g['max_ms'], g['at'], g['p99_ms']) for p, g in sorted(s['tick_gaps'].items(),
                                                                                         key=lambda kv: PHASES.index(kv[0]))),
        len(s['late_events']), [(e['client'], e['type'], e['stamp'], e['tick']) for e in s['late_events'][:8]],
        len(s['late_by_lines']), len(s['barrier_lines']),
        ' '.join('c%s:%d..%d%s' % (b['client'], b['first'], b['last'], '+' if b['open'] else '')
                 for b in (s.get('bodiless') or [])[:12]) or '-'))
    s5 = s.get('s5') or {}
    if s5:
        lines.append('server S5 intake-to-send ticks (n p50/p99/max; step 17: all p99 <= 2): %s; superseded %d' % (
            '; '.join('%s %d %.2f/%.2f/%.2f' % (k, v['n'], v['p50'], v['p99'], v['max'])
                      for k, v in sorted(s5.items(), key=lambda kv: (kv[0].split('/')[0] != 'all', kv[0]))),
            s.get('s5_superseded', 0)))
    counts = collections.Counter(c['status'] for c in crit)
    for c in crit:
        lines.append('criterion %-8s %s: %s - %s' % (c['who'], c['criterion'], c['status'].upper(), c['detail']))
    lines.append('criteria: pass=%d fail=%d masked=%d n/a=%d' % (counts['pass'], counts['fail'], counts['masked'],
                                                                 counts['n/a']))
    return lines


def replay(args, journal, out_dir):
    """The gold-standard check: the server's journal must replay bit for bit."""
    exe = pathlib.Path(args.bin).resolve() / f'BallanceMMOSimTool{EXE}'
    proc = subprocess.run([str(exe), '--root', args.root, '--replay-session', journal,
                           *(['--map', args.map] if getattr(args, 'map', '') else []),
                           '--report-every', '5000'],
                          capture_output=True, text=True, cwd=out_dir, timeout=1800)
    summary = [l for l in (proc.stdout + proc.stderr).splitlines() if l.startswith('summary:')]
    return {'exit_code': proc.returncode, 'summary': summary[-1] if summary else ''}


DEFAULT_REFUSAL = 'placement signature mismatch'


def parse_expect_refused(specs):
    """--expect-refused N[:TEXT] -> {N: TEXT} (TEXT defaults to DEFAULT_REFUSAL)."""
    out = {}
    for spec in specs:
        index, _, text = spec.partition(':')
        out[int(index)] = text or DEFAULT_REFUSAL
    return out


def verdict(case, event_clients=(), expect_refused=None):
    """Pass only when the run was both clean and actually interactive.
    `event_clients`: the clients an --event is on.  Their substituted ticks are
    the event's (since step 4 `starved` and `worst` count every substitution,
    from the relay header, with snapshots off too), so on them a starvation
    count is a note, not a problem.
    `expect_refused` {client: text}: clients the server must refuse (step DX7: a
    placement signature mismatch refuses that member alone, with a reason).  Such
    a client passes when it exited 0 and its log holds the session-end reason
    text; none of the playing client's checks apply to it (it never plays), and
    the rest of the verdict is the survivors'."""
    problems, warnings = [], []
    expect_refused = expect_refused or {}
    for i, c in enumerate(case['clients']):
        if c.get('hang'):
            h = c['hang']
            problems.append(f'client{i} process-exit hang: alive {h["after_s"]:.0f} s after its "exit code '
                            f'{h["logged_exit_code"]}" line (dump {h["dump"] or "FAILED"})')
        if i in expect_refused:
            text = expect_refused[i]
            try:
                log = (pathlib.Path(case['dir']) / f'client{i}.log').read_text(encoding='utf8', errors='replace')
            except OSError:
                log = ''
            ended = [l for l in log.splitlines() if 'ended: ' in l and text in l]
            if c.get('never_started'):
                problems.append(f'client{i} never started (expected refused)')
            elif c['exit_code'] != 0:
                problems.append(f'client{i} exit {c["exit_code"]} (expected refused)')
            elif not ended:
                problems.append(f'client{i} not refused with "{text}" (expected refused)')
            else:
                warnings.append(f'client{i} refused as expected: {ended[0].strip()[:160]}')
            continue
        if c.get('never_started'):
            problems.append(f'client{i} never started')
            continue
        problems.extend(f'client{i} {problem}' for problem in c.get('timing', {}).get('problems', []))
        m = c['metrics'] or {}
        if c['exit_code'] != 0:
            problems.append(f'client{i} exit {c["exit_code"]}')
        if m.get('hard') or m.get('mech_hard'):
            problems.append(f'client{i} hard corrections')
        if m.get('resyncs'):
            problems.append(f'client{i} resyncs')
        # A handful of starved ticks is the local box being busy, not a
        # protocol problem: the server repeats the last input, the session
        # recovers, and the journal still replays bit-exact.  Only a sustained
        # rate means the run is not worth trusting.
        starved, ticks = m.get('starved', 0), max(m.get('tick', 1), 1)
        if starved and i in event_clients:
            warnings.append(f'client{i} starved {starved}/{ticks} ticks (worst {m.get("worst_starved", 0)}; '
                            'the substitutions of its event)')
        elif starved > max(20, 0.01 * ticks):
            problems.append(f'client{i} input starvation {starved}/{ticks}')
        elif starved:
            warnings.append(f'client{i} starved {starved}/{ticks} ticks')
        # mechanisms=<known>/<resolved>.  The invariant that matters is
        # resolved > 0: a client with no local mechanism bodies reports zero
        # mechanism corrections, which otherwise reads as perfect sync (see the
        # 9.28 level-reset postmortem).  known > resolved is NOT a failure - it
        # is the normal state whenever the players straddle a checkpoint, since
        # the server runs the union of their sectors and names mechanisms this
        # client has no body for.
        known, _, resolved = (m.get('mechanisms') or '0/0').partition('/')
        if resolved == '0':
            problems.append(f'client{i} resolved no mechanism bodies ({m.get("mechanisms")})')
        # A relayed frame that arrived too late for the rollback to replay it
        # (deeper than its reach, or past the history) left that tick on the
        # prediction for good: the relay's own pass criterion.
        # Frames the exact state this client stands on already contains
        # (step RLT: relay=.../covered) are not late and fail nothing.
        if m.get('relay_late'):
            problems.append(f'client{i} relayed frames past the rollback reach: {m["relay_late"]} '
                            f'(lag max/p99 {m.get("relay_lag_max")}/{m.get("relay_lag_p99")}; '
                            f'covered by a resync state {m.get("relay_covered", 0)})')
        # The server went without this client's frames for a long run of
        # ticks: the client ran behind the server (a late SessionAssign, a
        # stall), every frame stale, every relayed frame in its future.
        if m.get('worst_starved', 0) > 100 and i not in event_clients:
            problems.append(f'client{i} ran behind the server: {m["worst_starved"]} ticks without a fresh frame '
                            f'(lead {m.get("lead")})')
    for r in case.get('replays', []):
        if r['exit_code'] != 0 or 'first_divergence=-1' not in r['summary']:
            problems.append('journal replay diverged')
    inter = case['interaction']
    # The interaction is read off the corrections, which need snapshots to
    # disagree with: a run against a server whose snapshots are off (the
    # snapshot-free measurements, a few dozen lifecycle-forced ones in
    # 60 s) has none to show even when every mechanism and every ball is
    # tracked and in step, so there both checks are a note, not a failure.
    snapshot_free = all((c['metrics'] or {}).get('snapshots', 0) < 100
                        for i, c in enumerate(case['clients'])
                        if not c.get('never_started') and i not in expect_refused)
    free_note = ' (snapshot-free run: the corrections cannot show it)' if snapshot_free else ''
    if not inter['mechanism_bodies_corrected']:
        (warnings if snapshot_free else problems).append('no mechanism interaction observed' + free_note)
    # Ball-to-ball interaction too is only observable through the body-set
    # corrections (peer_balls_corrected), so every snapshot-free multi-client
    # arm failed on it (step B9DX, item HWARN; found at REF8's review).
    if len(case['clients']) - len(expect_refused) > 1 and not inter['peer_balls_corrected']:
        (warnings if snapshot_free else problems).append('no ball-to-ball interaction observed' + free_note)
    return problems, warnings


def selftest():
    """The harness's own logic without a game: the specs, the window line, the
    percentile, the phases, and a proxy that blacks out and delays real
    datagrams on loopback.  Prints one line per check; 0 when all pass."""
    failures = []

    def check(what, ok, detail=''):
        print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, (' - ' + detail) if detail else ''))
        if not ok:
            failures.append(what)

    m = RE_S5.search('[Sim] Physics session 3: S5 event player=17 type=Physicalize origin=100 applied=130 late=30 '
                     'delay_us=21000 delay_ticks=1.40 to=1 superseded')
    check('RE_S5', m is not None and m.group(2) == 'Physicalize' and int(m.group(5)) == 30 and m.group(9) == 'superseded',
          str(m.groups() if m else None))
    m = RE_S4.search('[Sim] [session 1] S4 bodiless player=17 ticks=2502..2567 n=66 open')
    m2 = RE_S4.search('[Sim] [session 1] S4 bodiless player=17 ticks=2500..2503 n=4')
    check('RE_S4', m is not None and int(m.group(2)) == 2502 and int(m.group(4)) == 66 and bool(m.group(5))
          and m2 is not None and not m2.group(5), str((m.groups() if m else None, m2.groups() if m2 else None)))
    s5 = s5_summary([{'type': 'Sector', 'late': 0, 'delay_ticks': d} for d in (0.1, 0.2, 3.0)]
                    + [{'type': 'Physicalize', 'late': 4, 'delay_ticks': 1.0}])
    check('s5_summary', s5['all']['n'] == 4 and s5['Sector/on-time']['max'] == 3.0 and s5['all/restamped']['n'] == 1
          and s5['Sector']['p50'] == 0.2, str(s5))
    i, link = parse_link('2:lat=250,loss=10,burst=8,jitter=45')
    check('parse_link', i == 2 and link == {'latency_ms': 250.0, 'loss_pct': 10.0, 'loss_burst': 8, 'jitter_ms': 45.0}, str(link))
    ev = parse_event('2:1500:blackout=300:dir=up')
    check('parse_event blackout', (ev.client, ev.tick, ev.kind, ev.ms, ev.dirs, ev.t_off) == (2, 1500, 'blackout', 300.0, ('up',), 1520),
          '%s t_off %d' % (ev.describe(), ev.t_off))
    ev = parse_event('2:1500:spike=300')
    check('parse_event spike defaults', ev.dur_ms == 1000.0 and ev.dirs == ('up', 'down') and ev.t_off == 1500 + 86,
          'dur %g t_off %d' % (ev.dur_ms, ev.t_off))
    ev = parse_event('2:1500:step=300:dir=up:until=2700')
    check('parse_event step until', ev.t_off == 2700 and ev.until == 2700)
    arms = [infer_arm(argparse.Namespace(arm='', events=[parse_event(spec)]), 'link')
            for spec in ('1:1500:step=300:dir=up', '1:1500:step=100', '1:1500:step=300', '1:1500:step=300:dir=up:until=2700')]
    check('infer_arm A5u/A5s/A5/A5d', arms == ['A5U', 'A5S', 'A5', 'A5D'], str(arms))
    w = parse_window('window: tick=1559 inrew=3/41 life=0/0 forced=0/0 snaprb=1/12 snaps=1 resyncs=0/0 rebases=0 '
                     'relay=180 late=2 covered=4 lag[11]=23*4,24*50,49*2 lag[12]=30*60 inrew[11]=3 subst=5/4/1/0/1/0 '
                     'substco=7/1/1/9 lead_adj=1 lead=21 slew=-3')
    check('parse_window', w and w['tick'] == 1559 and w['inrew'] == [3, 41] and w['late'] == 2 and w['covered'] == 4
          and w['lag'] == {11: {23: 4, 24: 50, 49: 2}, 12: {30: 60}} and w['inrew_src'] == {11: 3}
          and w['subst'] == [5, 4, 1, 0, 1, 0] and w['substco'] == [7, 1, 1, 9] and w['lead_adj'] == 1
          and w['lead'] == 21 and w['slew'] == -3, str(w))
    # the server's S1 lead and lead-ack lines (session_runner.cpp)
    m = RE_LEAD_SENT.search('[session 3] S1 would-raise player=11 tick=1700 k=21 q10=-18.40 r10=2.10 r01=0.40 '
                            'misses=60/66 streak=132 offset=0; lead player=11 id=2 k=19 decided=21 cap=total '
                            'binding=total N=1700 held-from=1700 E=1745 c-est=1722 rtt=301 held-source=1690 '
                            'dropped=9 epoch=1200')
    check('RE_LEAD_SENT', m is not None and m.groups() == ('11', '2', '19', '21', 'total', 'total', '1700', '1700',
                                                          '1745', '9'), str(m.groups() if m else None))
    m = RE_LEAD_SENT.search('[session 3] S1 would-raise player=11 tick=1700 k=21 offset=0; lead player=11 id=3 k=7 '
                            'N=1700 held-from=1700 E=1718 c-est=? rtt=? held-source=default dropped=0 epoch=1200')
    check('RE_LEAD_SENT uncapped', m is not None and m.groups() == ('11', '3', '7', None, None, None, '1700', '1700',
                                                                   '1718', '0'), str(m.groups() if m else None))
    m = RE_LEAD_ACK.search('[session 3] S1 lead-ack player=11 id=2 applied=19 tick=1721 effective=1745 offset=19')
    check('RE_LEAD_ACK', m is not None and m.groups() == ('11', '2', '19', '1721'), str(m.groups() if m else None))
    st = hist_stats({23: 4, 24: 50, 49: 2})
    check('hist_stats p99', st == {'n': 56, 'max': 49, 'p99': 49}, str(st))
    st = hist_stats({i: 1 for i in range(1, 201)})
    check('hist_stats p99 of 1..200', st['p99'] == 198 and st['max'] == 200, str(st))
    check('parse_window ignores others', parse_window('status: tick=5') is None)
    phases = {'B': (10, 100), 'D': (100, 168), 'A': (168, 400)}
    check('phase_of', (phase_of(99, phases), phase_of(100, phases), phase_of(167, phases), phase_of(168, phases),
                       phase_of(400, phases)) == ('B', 'D', 'D', 'A', None))
    log = [b'logged in (own id 77, 2 online)\n', b'tick base 27 assigned (0 frames since anchor)\n',
           b'hold-event: own physicalize #2 stamped tick 900 held at tick 893 for 300 ms\n',
           b'hold-event: own physicalize stamped tick 900 (held at tick 893) released at tick 913\n',
           b'stall: 500 ms at tick 1500 (--stall-ms)\n', b'resynced: tick base 1600\n',
           b'window: tick=1619 inrew=0/0 life=0/0 forced=0/0 snaprb=0/0 snaps=0 resyncs=1/1 rebases=1 relay=0 late=0\n',
           b'own physicalize stamped 900 moved to 921 by the server at local tick 958: rewound to 899 (12 records changed,'
           b' made again in the replay)\n',
           b'own birth moved 1700 -> 1760 at local tick 1790: past the reach (depth 91), resync\n',
           b'own birth moved 2100 -> never at local tick 2330: no echo by the server\'s tick 2293, resync\n',
           b'status: phase=2 session=1 tick=1800 events=3/9/0 echo=4/2/0/0/1 ownlife=1/0/1/6/0 overtaken=0\n']

    # The window grid and the phases: a window that overlaps D at all is D's.
    ph = {'B': (27, 1500), 'D': (1500, 1568), 'A': (1568, 3600)}
    got = [window_phase({'tick': t, 'lo': t - 59}, ph) for t in (1439, 1499, 1500, 1559, 1619, 1627, 1680)]
    check('window_phase overlap', got == ['B', 'B', 'D', 'D', 'D', 'A', 'A'], str(got))
    # A control re-cut on a longer D: its windows, not its stored phases.
    def win(t, inrew, lag):
        return {'tick': t, 'lo': t - 59, 'inrew': [inrew, inrew * 10], 'life': [0, 0], 'forced': [0, 0],
                'snaprb': [0, 0], 'snaps': 0, 'resyncs': [0, 0], 'rebases': 0, 'relay': 0, 'late': 0,
                'lag': {'11': {str(lag): 60}}, 'inrew_src': {'11': inrew}}
    ctrl_row = {'index': 1, 'id': 12, 'span': [27, 3000], 'phases': {},
                'raw_windows': [win(t, 1 if t < 1560 else 4, 42 if t < 1560 else 45) for t in range(1499, 3000, 60)],
                'err30': [[s, 0.0, 0.0, 0.001 if s < 1560 else 0.004] for s in range(30, 3000, 30)]}
    ctrl = {'clients': [ctrl_row, {'index': 2, 'id': 11}]}
    short = recut_phases(ctrl, ctrl_row, {'B': (27, 1500), 'D': (1500, 1548), 'A': (1548, 3600)}, [2])
    long_ = recut_phases(ctrl, ctrl_row, {'B': (27, 1500), 'D': (1500, 1615), 'A': (1615, 3600)}, [2])
    check('recut_phases follows the arm\'s D',
          short['D']['windows'] == 1 and long_['D']['windows'] == 2 and short['D']['rb']['input'][0] == 1
          and long_['D']['rb']['input'][0] == 5 and long_['D']['inrew_late'] == 5 and long_['D']['lag_src']['c2']['p99'] == 45
          and short['D']['mech_max'] == 0.001 and long_['D']['mech_max'] == 0.004,
          'short %s / long %s' % ((short['D']['windows'], short['D']['rb']['input'], short['D']['mech_max']),
                                  (long_['D']['windows'], long_['D']['rb']['input'], long_['D']['mech_max'])))

    class FakeArgs(object):
        events, join_late, arm = [], {3: 1500}, ''
    jl = episode(FakeArgs(), {0: {'holds': [], 'stalls': [], 'bases': [27]},
                              3: {'holds': [], 'stalls': [], 'bases': [1713]}})
    check('a join-late run is an episode at the joiner\'s base',
          jl and jl['t_on'] == 1713 and jl['source'] == 'join' and jl['t_off'] + REACH - jl['t_on'] == 240
          and infer_arm(FakeArgs(), 'join') == 'A8', str(jl))
    FakeArgs.events = [parse_event('2:1500:step=300:dir=up')]
    check('infer_arm', (infer_arm(FakeArgs(), 'event'),
                        infer_arm(type('X', (), {'events': [parse_event('2:1500:blackout=200')], 'arm': ''})(), 'event'),
                        infer_arm(type('X', (), {'events': [parse_event('2:1500:none=0')], 'arm': ''})(), 'event'))
          == ('A5U', 'A3', 'A0'))

    # Step M0: --leave / --rejoin specs and the switches they give a client.
    lv = parse_client_ticks(['1:900', '3:1200'], 5, '--leave')
    rj = parse_client_ticks(['2:1000'], 5, '--rejoin')
    MA = type('MA', (), {'leave': lv, 'rejoin': rj})
    check('--leave / --rejoin: the client switches',
          lv == {1: 900, 3: 1200} and rj == {2: 1000}
          and member_args(MA, 1) == ['--leave-room-at', '900'] and member_args(MA, 2) == ['--rejoin-at', '1000']
          and member_args(MA, 0) == [] and member_args(type('X', (), {})(), 1) == [],
          '%s %s' % (lv, rj))
    refused = []
    for specs, what in ((['5:900'], '--leave'), (['1:0'], '--leave'), (['1'], '--leave'), (['x:9'], '--leave'),
                        (['1:900', '1:950'], '--leave'), (['0:900'], '--rejoin'),
                        (['0:900'], '--leave')):
        try:
            parse_client_ticks(specs, 5, what)
        except ValueError:
            refused.append(True)
        else:
            refused.append(False)
    check('--leave / --rejoin: bad specs refused (out of range, tick 0, no tick, not a number, twice, rejoin or leave of the host)',
          all(refused), str(refused))
    # Step TRL: a delayed rejoin N:T:MS.
    dl = {}
    rj2 = parse_client_ticks(['1:1200:1000', '2:1300'], 5, '--rejoin', dl)
    MB = type('MB', (), {'rejoin': rj2, 'rejoin_delay': dl})
    bad = []
    for spec in ('1:1200:x', '1:1200:-5'):
        try:
            parse_client_ticks([spec], 5, '--rejoin', {})
        except ValueError:
            bad.append(True)
        else:
            bad.append(False)
    check('--rejoin N:T:MS: a delayed rejoin (step TRL)',
          rj2 == {1: 1200, 2: 1300} and dl == {1: 1000}
          and member_args(MB, 1) == ['--rejoin-at', '1200:1000'] and member_args(MB, 2) == ['--rejoin-at', '1300']
          and all(bad), '%s %s %s' % (rj2, dl, bad))

    # Step F0: a rejoiner's log - two lives, the second's join import is
    # not a resync (nor its window's count), a real resync in the second life
    # is; the import's re-step end; the airship spans; the lead mode.
    import tempfile
    life_log = [b'session 1 starting: level 13, 3 players, join order 2, life 1, seed 1, input delay 6\n',
                b'tick base 8 assigned (0 frames since anchor)\n',
                b'window: tick=67 inrew=0/0 resyncs=0/0 late=0\n',
                b'session 1 starting: level 13, 3 players, join order 3, life 2, seed 1, input delay 6\n',
                b'tick base 1297 assigned (0 frames since anchor)\n',
                b'resynced: tick base 1297 (old tick 0, delta -1296): anchored at 1296, the server\'s state follows\n',
                b"resync: re-stepped from the server's tick 1296 to 1310 (14 ticks)\n",
                b'window: tick=1319 inrew=0/0 resyncs=0/1 late=0\n',
                b'resynced: tick base 1530 (old tick 1392, delta -137): anchored at 1529\n',
                b"resync: re-stepped from the server's tick 1529 to 1540 (11 ticks)\n",
                b'window: tick=1559 inrew=0/0 resyncs=1/1 late=0\n']
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / 'client.log'
        path.write_bytes(b''.join(life_log))
        lv = read_client_log(path)
        lives_got = [(lf['base'], lf['import'], lf['import_end'], lf['resyncs']) for lf in lv['lives']]
        check('read_client_log: lives, join import, re-step end (step F0)',
              lives_got == [(8, None, None, []), (1297, 1297, 1310, [1530])] and lv['lives'][1]['import_applied']
              and [w['resyncs'] for w in lv['windows']] == [[0, 0], [0, 0], [1, 1]] and lv['join_resyncs_removed'] == 1,
              str((lives_got, [w['resyncs'] for w in lv['windows']])))
        path.write_bytes(b''.join(life_log[:6] + life_log[8:]))   # the import's state never applied (DRJ)
        lv2 = read_client_log(path)
        got2 = [(lf['base'], lf['import'], lf['import_end'], lf['import_applied'], lf['resyncs']) for lf in lv2['lives']]
        check('read_client_log: an import that never applied is not a counted resync (step F0)',
              got2 == [(8, None, None, False, []), (1297, 1297, 1540, False, [1530])]
              and [w['resyncs'] for w in lv2['windows']] == [[0, 0], [1, 1]] and lv2['join_resyncs_removed'] == 0,
              str((got2, [w['resyncs'] for w in lv2['windows']])))
        (pathlib.Path(tmp) / 'client2.bmjr').write_bytes(b'x')
        (pathlib.Path(tmp) / 'client2.rejoin1.bmjr').write_bytes(b'x')
        lj = [(p_.name, lo, hi) for p_, lo, hi in life_journals(pathlib.Path(tmp), 2, lv['lives'])]
        check('life_journals (step F0)', lj == [('client2.bmjr', -(1 << 31), 1297), ('client2.rejoin1.bmjr', 1297, 1 << 31)],
              str(lj))
    spans = airship_spans("world: airship: player 7's ball Ball_Stone_BMMO_8 is in the airship zone at tick 2233 (a ghost)\n"
                          "world: spawn ghost: player 7's ball Ball_Stone_BMMO_8 is a spawn ghost at tick 212\n"
                          "world: spawn ghost: player 7's ball Ball_Stone_BMMO_8 is a spawn ghost at tick 2673\n"
                          "world: airship: player 7's ball Ball_Stone_BMMO_8 is in the airship zone at tick 3186 (a ghost)\n")
    check('airship_spans (step F0)', spans == {7: [(2233, 2673), (3186, 1 << 31)]}
          and in_spans(spans[7], 2233) and not in_spans(spans[7], 2673) and in_spans(spans[7], 9000), str(spans))
    check('lead_mode (step F0)', lead_mode('[x] Physics sessions enabled (game root /a, patience off, lead adjust off).') == 'off'
          and lead_mode('nothing') is None)

    # A proxy in front of a loopback "server": a client sends one datagram
    # every 5 ms for 1.2 s, an up blackout covers 0.3-0.6 s, a spike of 200 ms
    # the down datagrams sent in 0.8-1.0 s.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / 'client.log'
        path.write_bytes(b''.join(log))
        parsed = read_client_log(path)
        check('read_client_log', parsed['own_id'] == 77 and parsed['bases'] == [27, 1600]
              and parsed['holds'] == [{'type': 'physicalize', 'stamp': 900, 'held_at': 893, 'released_at': 913}]
              and parsed['stalls'] == [{'ms': 500, 'tick': 1500}] and len(parsed['windows']) == 1, str(parsed))
        check('read_client_log: own moves and echoes (plan step 19)',
              parsed['own_moves'] == [{'origin': 900, 'applied': 921, 'at': 958, 'how': 'rewound'},
                                      {'origin': 1700, 'applied': 1760, 'at': 1790, 'how': 'resync',
                                       'why': 'past the reach (depth 91)'},
                                      {'origin': 2100, 'applied': None, 'at': 2330, 'how': 'resync',
                                       'why': "no echo by the server's tick 2293"}]
              and parsed['echo'] == [4, 2, 0, 0, 1] and parsed['ownlife'] == '1/0/1/6/0',
              str((parsed['own_moves'], parsed['echo'], parsed['ownlife'])))
        # Assignments and two postponed starts must not arm a playing fault.
        armed = LinkEvent(0, 166, 'step', 100, until=200)
        watch_log = pathlib.Path(tmp) / 'watch.log'
        watch_log.write_bytes(b'')
        watch = LogWatch(watch_log, arm_events([armed]))
        with open(watch_log, 'ab') as f:
            f.write(b'tick base 100 assigned (0 frames since anchor)\n'
                    b'start handshake: start 1 accepted\n'
                    b'start handshake: start 2 accepted\n'
                    b'harness: target 0 reached at tick 100\n')
        t0 = time.monotonic()
        while 0 not in watch.ticks and time.monotonic() - t0 < 2:
            time.sleep(0.01)
        check('LogWatch ignores assignments and postponements', 0 in watch.ticks and armed.start is None)
        with open(watch_log, 'ab') as f:
            f.write(b'harness: target 166 reached at tick 166\n')
        t0 = time.monotonic()
        while armed.start is None and time.monotonic() - t0 < 2:
            time.sleep(0.01)
        check('LogWatch arms on the actual tick', armed.start is not None and abs(armed.start - t0) < 0.1,
              'starts %.3f s after the line' % ((armed.start or t0) - t0))
        original_start = armed.start
        with open(watch_log, 'ab') as f:
            f.write(b'resynced: tick base 190\n'
                    b'harness: target 166 reached at tick 190\n'
                    b'harness: target 200 reached at tick 200\n')
        t0 = time.monotonic()
        while armed.end is None and time.monotonic() - t0 < 2:
            time.sleep(0.01)
        watch.close()
        check('LogWatch ends at the actual until tick and never rearms',
              armed.end is not None and armed.start == original_start
              and armed.observed_tick == 166 and armed.observed_until == 200)
        check('milestone coverage catches missing and skipped ticks',
              not timing_problems({0, 166}, {0: 100, 166: 166})
              and len(timing_problems({0, 166, 200}, {0: 100, 166: 190})) == 2)

    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(('127.0.0.1', 0))
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
    probe.close()
    proxy = DelayProxy(port, server.getsockname(), 20.0)
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.bind(('127.0.0.1', 0))
    start = time.monotonic()
    black = LinkEvent(0, 0, 'blackout', 300, direction='up')
    black.start = start + 0.3
    spike = LinkEvent(0, 0, 'spike', 200, 200, 'down')
    spike.start = start + 0.8
    proxy.events = [black, spike]
    got_up, got_down = [], []
    server.settimeout(0.002)
    client.settimeout(0.002)
    n = sent_in_blackout = 0
    while time.monotonic() - start < 1.6:
        now = time.monotonic() - start
        if now < 1.2 and n * 0.005 <= now:
            client.sendto(struct.pack('<id', n, now), ('127.0.0.1', port))
            sent_in_blackout += 0.3 <= now < 0.6
            n += 1
        try:
            data, addr = server.recvfrom(64)
            seq, sent = struct.unpack('<id', data)
            got_up.append((seq, sent))
            server.sendto(struct.pack('<id', seq, time.monotonic() - start), addr)
        except (socket.timeout, ConnectionResetError):
            pass
        try:
            data, _ = client.recvfrom(64)
            seq, echoed = struct.unpack('<id', data)
            got_down.append((seq, echoed, time.monotonic() - start))
        except (socket.timeout, ConnectionResetError):
            pass
    proxy.close()
    server.close()
    client.close()
    lost = [sent for seq, sent in got_up]
    in_blackout = [s for s in lost if 0.3 <= s < 0.6]
    check('blackout drops the up datagrams of its window, and only those',
          not in_blackout and proxy.blacked['up'] > 10 and abs(proxy.blacked['up'] - sent_in_blackout) <= 2
          and len(got_up) + proxy.blacked['up'] == n,
          'sent %d, passed %d, blacked %d of the %d sent in the window, %d passed inside it'
          % (n, len(got_up), proxy.blacked['up'], sent_in_blackout, len(in_blackout)))
    late_down = [arrived - echoed for seq, echoed, arrived in got_down if 0.82 <= echoed < 0.98]
    early_down = [arrived - echoed for seq, echoed, arrived in got_down if echoed < 0.25]
    check('spike delays the down datagrams of its window',
          late_down and min(late_down) > 0.2 and early_down and max(early_down) < 0.1 and proxy.delayed['down'] > 0,
          'spiked trip %.3f..%.3f s, normal %.3f s' % (min(late_down or [0]), max(late_down or [0]), max(early_down or [0])))
    order = [seq for seq, echoed, arrived in got_down]
    after = [seq for seq, echoed, arrived in got_down if echoed >= 1.0]
    check('nothing overtakes the spiked datagrams',
          order == sorted(order) and after,
          '%d down datagrams, %d sent after the spike, %d out of order'
          % (len(order), len(after), sum(1 for a, b in zip(order, order[1:]) if b < a)))
    # SAXE: --drop-down parses, and drops exactly the anchored downlink run.
    check('parse_drop_down',
          parse_drop_down('1:size51:4') == (1, (('size', 51, 51), 4))
          and parse_drop_down('0:12:3') == (0, (('index', 12), 3))
          and parse_drop_down('2:size40-60:2') == (2, (('size', 40, 60), 2)),
          str((parse_drop_down('1:size51:4'), parse_drop_down('0:12:3'))))
    server2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server2.bind(('127.0.0.1', 0))
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(('127.0.0.1', 0))
    port2 = probe.getsockname()[1]
    probe.close()
    proxy2 = DelayProxy(port2, server2.getsockname(), 0.0)
    proxy2.drop_down = (('size', 51, 51), 4)
    client2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client2.bind(('127.0.0.1', 0))
    server2.settimeout(0.5)
    client2.settimeout(0.05)
    client2.sendto(b'hello', ('127.0.0.1', port2))
    _, caddr = server2.recvfrom(64)
    # Downlink sizes 30, 30, 51, 48 x 6: the 51 and the next three go.
    sizes = [30, 30, 51] + [48] * 6
    for k, n_bytes in enumerate(sizes):
        server2.sendto(bytes([k]) * n_bytes, caddr)
        time.sleep(0.003)
    got = []
    t_end = time.monotonic() + 0.4
    while time.monotonic() < t_end:
        try:
            data, _ = client2.recvfrom(128)
            got.append(data[0])
        except (socket.timeout, ConnectionResetError):
            pass
    proxy2.close()
    server2.close()
    client2.close()
    check('drop-down drops the anchored run once, and only it',
          got == [0, 1, 6, 7, 8] and proxy2.forced_fired == (2, 51) and proxy2.forced_dropped == 4,
          'got %s, fired %s, dropped %d' % (got, proxy2.forced_fired, proxy2.forced_dropped))

    # SAXE review: under random loss a forced drop still takes the loss rule's
    # draws, so the same seed draws the same stream with and without it.
    def lossy_run(drop):
        srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        srv.bind(('127.0.0.1', 0))
        pr = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        pr.bind(('127.0.0.1', 0))
        lport = pr.getsockname()[1]
        pr.close()
        px = DelayProxy(lport, srv.getsockname(), 0.0, loss_pct=30.0, jitter_ms=1.0, loss_burst=2)
        px.rng = random.Random(4242)
        px.drop_down = drop
        cl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cl.bind(('127.0.0.1', 0))
        srv.settimeout(0.5)
        caddr = None
        for _ in range(20):          # the hello may be lost: send until one passes
            cl.sendto(b'hello', ('127.0.0.1', lport))
            try:
                _, caddr = srv.recvfrom(64)
                break
            except socket.timeout:
                pass
        draws_up = px.rng.getstate()
        for k, n_bytes in enumerate([30, 30, 51] + [48] * 20):
            srv.sendto(bytes([k]) * n_bytes, caddr)
            time.sleep(0.003)
        time.sleep(0.1)
        px.close()
        srv.close()
        cl.close()
        return draws_up, px.rng.getstate(), px.dropped, px.forced_dropped
    plain, forced_run = lossy_run(None), lossy_run((('size', 51, 51), 4))
    check('drop-down keeps the seed\'s loss draws aligned',
          plain[0] == forced_run[0] and plain[1] == forced_run[1] and forced_run[3] == 4,
          'same before %s, same after %s, forced %d' % (plain[0] == forced_run[0], plain[1] == forced_run[1], forced_run[3]))
    # HXP: --outage parses, and on a synthetic stream drops exactly the
    # datagrams of its direction inside its wall-clock window.
    _, o = parse_outage('1:size81:60')
    _, o2 = parse_outage('2:size60-70+400:200:both')
    _, o3 = parse_outage('0:5:30:up')
    check('parse_outage',
          (o.anchor, o.ms, o.dirs, o.delay_ms) == (('size', 81, 81), 60.0, ('down',), 0.0)
          and (o2.anchor, o2.ms, o2.dirs, o2.delay_ms) == (('size', 60, 70), 200.0, ('up', 'down'), 400.0)
          and (o3.anchor, o3.dirs) == (('index', 5), ('up',)),
          str([(x.anchor, x.ms, x.dirs, x.delay_ms) for x in (o, o2, o3)]))
    bad = 0
    for spec in ('1:size81', '1:size81:0', '1:size81:60:sideways', 'x:size81:60'):
        try:
            parse_outage(spec)
        except ValueError:
            bad += 1
    check('parse_outage refuses bad specs', bad == 4, '%d of 4 refused' % bad)

    def outage_stream(outage, stream):
        """stream: (t_ms, dir, size) -> the indices of the dropped ones."""
        down = 0
        dropped = []
        for k, (t_ms, d, size) in enumerate(stream):
            idx = None
            if d == 'down':
                idx, down = down, down + 1
            if outage.drops(d, idx, size, 1000.0 + t_ms / 1000.0):
                dropped.append(k)
        return dropped
    # Every 10 ms a downlink datagram (30 B) and an uplink one (20 B); the
    # anchor (81 B) at 100 ms; a second 81 B at 300 ms must not re-anchor.
    stream = []
    for t in range(0, 600, 10):
        stream.append((t, 'down', 81 if t in (100, 300) else 30))
        stream.append((t + 5, 'up', 20))
    win = lambda lo, hi, d: [k for k, (t, dd, _) in enumerate(stream) if dd in d and lo <= t < hi]
    got = outage_stream(parse_outage('0:size81:60')[1], stream)
    check('outage: down for 60 ms from the anchor', got == win(100, 160, ('down',)), str(got))
    got = outage_stream(parse_outage('0:size81:60:up')[1], stream)
    check('outage: up only, anchored on the downlink', got == win(100, 160, ('up',)), str(got))
    got = outage_stream(parse_outage('0:size81+202:100:both')[1], stream)
    check('outage: +DELAY, both directions, one-shot', got == win(302, 402, ('up', 'down')), str(got))
    got = outage_stream(parse_outage('0:size90:60')[1], stream)
    check('outage: an anchor that never matches drops nothing', got == [], str(got))
    # Through the proxy: the anchored downlink datagram and the ones right
    # after it go, the ones well after the window pass; the rng draws stay
    # those of the same seed without the outage (the --drop-down rule).
    server3 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server3.bind(('127.0.0.1', 0))
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(('127.0.0.1', 0))
    port3 = probe.getsockname()[1]
    probe.close()
    proxy3 = DelayProxy(port3, server3.getsockname(), 0.0)
    proxy3.outages = [parse_outage('0:size81:150')[1]]
    client3 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client3.bind(('127.0.0.1', 0))
    server3.settimeout(0.5)
    client3.settimeout(0.05)
    client3.sendto(b'hello', ('127.0.0.1', port3))
    _, caddr = server3.recvfrom(64)
    for k, n_bytes in enumerate([30, 81, 48, 48]):        # inside the window: 1, 2, 3 go
        server3.sendto(bytes([k]) * n_bytes, caddr)
    time.sleep(0.4)                                        # well past it: 4, 5 pass
    for k, n_bytes in ((4, 48), (5, 81)):
        server3.sendto(bytes([k]) * n_bytes, caddr)
    got = []
    t_end = time.monotonic() + 0.4
    while time.monotonic() < t_end:
        try:
            data, _ = client3.recvfrom(128)
            got.append(data[0])
        except (socket.timeout, ConnectionResetError):
            pass
    proxy3.close()
    server3.close()
    client3.close()
    oo = proxy3.outages[0]
    check('outage through the proxy drops the window, and only it',
          got == [0, 4, 5] and oo.fired[:2] == (1, 81) and oo.dropped == {'up': 0, 'down': 3},
          'got %s, fired %s, dropped %s' % (got, oo.fired, oo.dropped))
    plain_o, outage_run = lossy_run(None), None

    def lossy_outage_run():
        srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        srv.bind(('127.0.0.1', 0))
        pr = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        pr.bind(('127.0.0.1', 0))
        lport = pr.getsockname()[1]
        pr.close()
        px = DelayProxy(lport, srv.getsockname(), 0.0, loss_pct=30.0, jitter_ms=1.0, loss_burst=2)
        px.rng = random.Random(4242)
        px.outages = [parse_outage('0:size51:40')[1]]
        cl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cl.bind(('127.0.0.1', 0))
        srv.settimeout(0.5)
        caddr = None
        for _ in range(20):
            cl.sendto(b'hello', ('127.0.0.1', lport))
            try:
                _, caddr = srv.recvfrom(64)
                break
            except socket.timeout:
                pass
        draws_up = px.rng.getstate()
        for k, n_bytes in enumerate([30, 30, 51] + [48] * 20):
            srv.sendto(bytes([k]) * n_bytes, caddr)
            time.sleep(0.003)
        time.sleep(0.1)
        px.close()
        srv.close()
        cl.close()
        return draws_up, px.rng.getstate(), px.outages[0].dropped['down']
    outage_run = lossy_outage_run()
    check('outage keeps the seed\'s loss draws aligned',
          plain_o[0] == outage_run[0] and plain_o[1] == outage_run[1] and outage_run[2] >= 1,
          'same before %s, same after %s, outage dropped %d' % (plain_o[0] == outage_run[0],
                                                                plain_o[1] == outage_run[1], outage_run[2]))
    # HXP: the hang watcher end to end (HangWatch.run on a log written in
    # chunks): a mid-line 'exit code 4' cut where a chunk ends is not the
    # exit line; the exit line split over two reads and a NUL is; a process
    # still alive after_s later is dumped and ended; one that exits is not
    # flagged.  The verdict fails a hung client (its logged exit code stands).
    import tempfile
    check('exit line pattern', not RE_EXIT_LINE.search(b'warning: an exit code 4 path\n')
          and [int(m.group(1)) for m in RE_EXIT_LINE.finditer(b'status: tick=5\nexit code 0\n')] == [0], '')
    with tempfile.TemporaryDirectory() as tmp:
        def watch(seconds, chunks, after_s):
            log = pathlib.Path(tmp) / ('client%d.log' % seconds)
            log.write_bytes(b'')
            child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(%d)' % seconds],
                                     stdin=subprocess.DEVNULL, creationflags=FLAGS)
            try:
                w = HangWatch(seconds, child, log, pathlib.Path(tmp) / ("client%d.hang.dmp" % seconds), after_s,
                              symbolize=False)
                for chunk in chunks:
                    with open(log, 'ab') as f:
                        f.write(chunk)
                    time.sleep(0.5)   # each chunk its own read (the watcher polls every 0.2 s)
                w.thread.join(timeout=20)
                try:   # the watcher's terminate() does not wait for the end
                    return w, child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    return w, None
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait()
        # 'exit code 4 ' + 52 B = the last 64 bytes of the first chunk: the
        # old 64-byte tail began at 'exit' and took it for a line start.
        hung, hung_rc = watch(30, [b'status: ' + b'A' * 100 + b'exit code 4 ' + b'B' * 52,
                                   b'\nstatus: tick=5\nex\0it co', b'de 0\n'], 0.5)
        # Exits about 0.5 s after its line, inside its 4 s: not a hang.
        clean, _ = watch(1, [b'status: tick=5\nexit code 0\n'], 4.0)
        dumped = hung.hang is not None and (sys.platform != 'win32' or (
            hung.hang['dump'] is not None and os.path.getsize(hung.hang['dump']) > 0))
        check('hang watcher end to end',
              hung.logged_code == 0 and dumped and hung_rc is not None and not hung.thread.is_alive()
              and clean.logged_code == 0 and clean.hang is None,
              'hung: code %r hang %r rc %r; clean: code %r hang %r' % (hung.logged_code, hung.hang, hung_rc,
                                                                      clean.logged_code, clean.hang))
    # DX7: --expect-refused judges the named client by its session-end reason only.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        metrics = {'mechanisms': '9/9', 'snapshots': 0, 'tick': 3000}
        (pathlib.Path(tmp) / 'client1.log').write_text(
            'session 1 ended: placement signature mismatch: yours 0000000000000001, the server\'s 0123\n', encoding='utf8')
        case = {'dir': tmp, 'replays': [],
                'interaction': {'mechanism_bodies_corrected': [], 'peer_balls_corrected': []},
                'clients': [{'exit_code': 0, 'metrics': dict(metrics)},
                            {'exit_code': 0, 'metrics': {'mechanisms': '0/0', 'snapshots': 0, 'tick': 0}},
                            {'exit_code': 0, 'metrics': dict(metrics)}]}
        p_plain, _ = verdict(case)
        p_ok, w_ok = verdict(case, (), parse_expect_refused(['1']))
        p_text, _ = verdict(case, (), parse_expect_refused(['1:some other reason']))
        p_wrong, _ = verdict(case, (), parse_expect_refused(['2']))
        check('verdict --expect-refused',
              any('client1 resolved no mechanism' in p for p in p_plain) and not p_ok
              and any('refused as expected' in w for w in w_ok)
              and any('client1 not refused' in p for p in p_text)
              and any('client2 not refused' in p for p in p_wrong) and any('client1 resolved no' in p for p in p_wrong),
              str((p_plain, p_ok, w_ok, p_text, p_wrong)))
        case['clients'][2]['hang'] = {'after_s': 20.4, 'logged_exit_code': 0, 'dump': 'client2.hang.dmp'}
        p_hang, _ = verdict(case, (), parse_expect_refused(['1']))
        check('verdict fails a process-exit hang',
              len(p_hang) == 1 and 'client2 process-exit hang' in p_hang[0], str(p_hang))
    print('selftest: %s' % ('PASS' if not failures else 'FAIL - ' + ', '.join(failures)))
    return 1 if failures else 0


def emit_report(args, case, case_dir):
    """H6/H8 for one case: into the case, its directory and stdout."""
    try:
        report = event_report(args, case, case_dir)
    except Exception as exc:   # a report bug must not cost the run's summary
        import traceback
        traceback.print_exc()
        print(f'  event report failed: {exc}', flush=True)
        return
    if report is None:
        return
    crit = criteria(report, case, load_controls(args.control, case['sector']))
    case['event_report'], case['criteria'] = report, crit
    lines = format_report(report, crit)
    (case_dir / 'event_report.txt').write_text('\n'.join(lines) + '\n', encoding='utf8')
    (case_dir / 'event_report.json').write_text(json.dumps({'report': report, 'criteria': crit}, indent=1),
                                                encoding='utf8')
    for line in lines:
        print('  ' + line, flush=True)


def report_only(args):
    """--report-only: the report of a finished run, again (a newer harness,
    or a control given afterwards)."""
    base = pathlib.Path(args.report_only)
    cases = json.loads((base / 'summary.json').read_text(encoding='utf8'))
    for case in cases:
        if (base / 'server.bmjr').exists():
            case['journal'] = [str(base / 'server.bmjr')]
        # The verdict again too (DX7: a run judged afterwards with --expect-refused).
        case['dir'] = str(base / f'sector{case["sector"]}')
        problems, warnings = verdict(case, {ev.client for ev in args.events if ev.kind != 'none'},
                                     args.expect_refused)
        case['problems'], case['warnings'] = problems, warnings
        print(f'=== sector {case["sector"]} ===', flush=True)
        if warnings:
            print(f'  note: {"; ".join(warnings)}', flush=True)
        print(f'  VERDICT: {"PASS" if not problems else "FAIL - " + "; ".join(problems)}', flush=True)
        emit_report(args, case, base / f'sector{case["sector"]}')
    (base / 'summary.json').write_text(json.dumps(cases, indent=2), encoding='utf8')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=str(REPO / 'build-agent/BallanceMMOServer'))
    ap.add_argument('--root', default='', help='Ballance data directory (required for a run)')
    ap.add_argument('--level', type=int, default=13)
    ap.add_argument('--map', default='auto',
                    help='step CMAP: the custom map loaded into --level\'s slot, as BML+ loads one (the clients announce it '
                         'by md5, the server runs it from its custom_maps directory, the journal replays take it). '
                         '"auto" (default): the arena, <root>/ModLoader/Maps/BMMO_Mechanism_Arena.nmo, for level 13 '
                         '(this harness\'s map; the slot itself holds the original Level 13 since CMAP), none otherwise; '
                         '"none": the slot\'s own file')
    ap.add_argument('--sectors', default=None)
    ap.add_argument('--players', '--clients', dest='players', type=int, default=None)
    ap.add_argument('--seconds', type=int, default=60,
                    help='play budget after each client steps its first assigned frame')
    ap.add_argument('--startup-timeout', type=int, default=180,
                    help='extra wall-clock allowance for loading and startup; total limit is seconds plus this')
    ap.add_argument('--input-delay', type=int, default=6)
    ap.add_argument('--spawn-impulse', type=float, default=3.0)
    ap.add_argument('--port', type=int, default=32990)
    ap.add_argument('--drive', action='store_true',
                    help='feed the clients staggered input records that roll the balls through the bays')
    ap.add_argument('--steer', action='store_true',
                    help='add alternating left/right (drifts the balls off the bays)')
    ap.add_argument('--keep-lane', action='store_true',
                    help='close the loop on the drive: the clients add LEFT/RIGHT to hold their balls on the '
                         'lane centre (z = 15), so every ball takes every checkpoint and respawns at its sector')
    ap.add_argument('--beam-bays', default='',
                    help='"auto" (the bays of the active sector) or comma-separated bay indices; '
                         'client i is beamed just short of bay[i] '
                         'so it meets that mechanism without driving the whole arena')
    ap.add_argument('--drive-stop', type=int, default=0,
                    help='tick at which the clients stop driving and coast to a halt')
    ap.add_argument('--journal-dir', default='',
                    help="where the server writes journals, when it is not ours")
    ap.add_argument('--external-server', default='',
                    help='HOST:PORT of a server to use instead of starting one')
    ap.add_argument('--latency-ms', type=float, default=None,
                    help='each way delay added to the affected clients links')
    ap.add_argument('--client-arg', action='append', default=[],
                    help='extra argument passed straight to every session client; repeatable')
    ap.add_argument('--loss-pct', type=float, default=0.0,
                    help='percentage of datagrams the proxy drops, each way; on its own it '
                         'puts a lossless-latency proxy in the path with loss only')
    ap.add_argument('--jitter-ms', type=float, default=0.0,
                    help='extra delay, uniform in [0, this), per datagram: datagrams overtake each other')
    ap.add_argument('--loss-burst', type=int, default=1,
                    help='a dropped datagram takes this many in a row with it (the loss rate is kept)')
    ap.add_argument('--loss-seed', type=int, default=0,
                    help='another loss pattern for the same recipe (0 = the draws every earlier run made)')
    ap.add_argument('--proxy-log', action='store_true',
                    help='each proxy writes proxy<i>.log: one line per datagram (receipt time, direction, '
                         'size, pass/loss/burst/blackout, due time); for measurement runs only - the write '
                         'happens on the forwarding thread and delays forwarding slightly')
    ap.add_argument('--latency-clients', default='',
                    help='client indices to put behind the delay proxy; empty = all')
    ap.add_argument('--stall-client', type=int, default=-1,
                    help='client index to freeze mid-session')
    ap.add_argument('--stall-at', type=int, default=1200,
                    help='session tick at which that client freezes')
    ap.add_argument('--stall-ms', type=int, default=0,
                    help='how long it freezes for, in milliseconds')
    ap.add_argument('--beam-spread', type=float, default=3.0,
                    help='metres between the beam landing points of successive clients')
    ap.add_argument('--beam-offset', type=int, default=0,
                    help='rotate which bays of the sector --beam-bays auto picks')
    ap.add_argument('--checkpoint-ticks', type=int, default=None,
                    help='ticks between full body checkpoints in the journals; small '
                         'values make contact measurable with collision_report.py')
    ap.add_argument('--beam-dx', type=float, default=-9.0,
                    help='offset from the bay centre for the first beam landing; the default puts the ball just short of the bay mechanism')
    ap.add_argument('--beam-y', type=float, default=-1.0,
                    help='lane to beam onto: %g for the mechanism row, %g for the '
                         'driving lane; default is the mechanism row' % (MECH_Y, PLAY_Y))
    ap.add_argument('--beam-tick', type=int, default=700,
                    help='session tick at which --beam-bays fires')
    ap.add_argument('--out', default=str(REPO / 'maps/arena-test'))
    ap.add_argument('--gap', type=float, default=6.0,
                    help='seconds to wait between cases so the last session fully exits')
    ap.add_argument('--label', default=time.strftime('%Y%m%d%H%M%S'))
    # The late-player harness (docs/late-player-design.md 5.1).
    ap.add_argument('--drop-down', action='append', default=[], metavar='N:ANCHOR:COUNT',
                    help='step SAXE: client N\'s proxy drops COUNT consecutive downlink datagrams once, from the '
                         'one ANCHOR names - a 0-based downlink datagram index, or sizeA / sizeA-B (the first '
                         'downlink datagram of A..B bytes); a deterministic burst, apart from --loss-pct; repeatable')
    ap.add_argument('--outage', action='append', default=[], metavar='N:ANCHOR[+DELAY]:MS[:up|down|both]',
                    help='FULL step HXP: a time-shaped outage on client N\'s link: from the first downlink '
                         'datagram ANCHOR names (the --drop-down anchors), DELAY ms later (default 0), every '
                         'datagram of the direction (default down; up = client to server) is dropped for MS ms of '
                         'wall clock; works before tick 0, unlike --event; repeatable')
    ap.add_argument('--no-hang-dump', dest='hang_dump', action='store_false',
                    help='FULL step HXP: do not watch the client processes for the process-exit hang (on by '
                         'default: a client alive --hang-dump-after s after its "exit code" line is minidumped '
                         'into client<i>.hang.dmp, symbolized into .txt, ended, and the verdict fails)')
    ap.add_argument('--hang-dump-after', type=float, default=20.0, metavar='S',
                    help='FULL step HXP: seconds after the "exit code" line before a live client counts as hung')
    ap.add_argument('--link', action='append', default=[], metavar='N:lat=,loss=,burst=,jitter=',
                    help='H1: client N\'s own link, over the shared one; repeatable')
    ap.add_argument('--event', action='append', default=[], metavar='N:T:KIND=MS[:dur=MS][:dir=D][:until=T2]',
                    help='H2: an episode on client N at its tick T; KIND blackout, spike, step, stall or none; repeatable')
    ap.add_argument('--event-client', type=int, default=-1,
                    help='H2, one event: the client (default 2, the design\'s: not the host)')
    ap.add_argument('--event-tick', type=int, default=0, help='H2, one event: the tick (alone: a control\'s phases)')
    ap.add_argument('--blackout-ms', type=float, default=0.0, help='H2: drop the link for this long')
    ap.add_argument('--spike-ms', type=float, default=0.0, help='H2: add this much delay for --event-ms')
    ap.add_argument('--step-ms', type=float, default=0.0, help='H2: add this much delay from the tick on')
    ap.add_argument('--event-ms', type=float, default=0.0,
                    help='H2: how long a spike lasts (default 1000); for a control, the episode length its phases assume')
    ap.add_argument('--event-until', type=int, default=0, help='H2: the tick a step ends at (default: never)')
    ap.add_argument('--event-dir', default='both', choices=('up', 'down', 'both'),
                    help='H2: up = client to server')
    ap.add_argument('--client-arg-for', action='append', default=[], metavar='N:"ARGS"', dest='client_arg_for_specs',
                    help='H3: extra arguments for client N alone; repeatable')
    ap.add_argument('--join-late', action='append', default=[], metavar='N:T',
                    help='H4: client N starts when the host reaches tick T; N:cue (step DX9): a pre-booted joiner '
                         'whose Join the server\'s all-ready line cues')
    ap.add_argument('--join-cue-mark', default=None, metavar='TEXT',
                    help='FULL step SAXH: the server log line (a substring) that cues the N:cue joiners instead of '
                         'the all-ready line, e.g. "start handshake: the start base" (a join inside the start '
                         'handshake\'s window)')
    ap.add_argument('--expect-refused', action='append', default=[], metavar='N[:TEXT]',
                    help='DX7: the server must refuse client N (its log holds "session S ended: ...TEXT..."; '
                         'default TEXT "%s"); the verdict is the other clients\' ' % DEFAULT_REFUSAL),

    ap.add_argument('--leave', action='append', default=[], metavar='N:T',
                    help='world agreement J2/J4: client N leaves the room at its tick T and exits (--leave-room-at)')
    ap.add_argument('--rejoin', action='append', default=[], metavar='N:T[:MS]',
                    help='world agreement J5: client N leaves the room at its tick T and joins it again '
                         'on the same connection, MS ms later when given (--rejoin-at; not client 0)')
    ap.add_argument('--server-log', default='', help='H5: an external server\'s log, copied into the case')
    ap.add_argument('--control', action='append', default=[],
                    help='H8: a control run (its directory or summary.json) whose event report is the band; repeatable')
    ap.add_argument('--arm', default='', help='H8: the arm of design 5.2 (A0, A0j, A1 .. A9c) whose own criteria '
                                              'apply; inferred from the episode when absent')
    ap.add_argument('--window-origin', type=int, default=-1,
                    help='the tick the clients\' window: grid starts at (default: T_on of the --event ticks, else the '
                         '--join-late tick; 0 = the plain grid)')
    ap.add_argument('--selftest', action='store_true', help='check the harness\'s own logic offline and exit')
    ap.add_argument('--scenario', type=str.upper, choices=lph.ARMS, default='')
    ap.add_argument('--variant', choices=('lane', 'swing'), default='lane')
    ap.add_argument('--hold', help='lifecycle hold in milliseconds, or drop (A6: 496; A6m/A6b: 300)')
    ap.add_argument('--patience', choices=('off', 'on'), default='off')
    ap.add_argument('--lead-adjust', choices=('off', 'on'), default='off')
    ap.add_argument('--test-clock-skew', default='', metavar='T:MS:PCT')
    ap.add_argument('--server-control', default='', metavar='PYTHON_SCRIPT',
                    help='controller called with start/stop and JSON physics flags; required for external-server twins')
    ap.add_argument('--twins', action='store_true', help='run flags-off and flags-on sequentially and compare')
    ap.add_argument('--twin-flags', choices=('both', 'patience', 'lead'), default='both')
    ap.add_argument('--dry-run', action='store_true', help='print the resolved scenario without starting processes')
    ap.add_argument('--report-only', default='',
                    help='redo the event report and criteria of an earlier run directory (give the run\'s own '
                         '--players and event flags, and --control); its server.bmjr, when the batch copied one '
                         'there, is the journal')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    try:
        lph.apply_scenario(args)
        if args.server_control and not args.external_server:
            raise ValueError('--server-control requires --external-server')
        if args.twins and not args.dry_run:
            return lph.twins(args, sys.argv[1:])
    except ValueError as e:
        ap.error(str(e))
    if args.dry_run:
        print(json.dumps(vars(args), indent=2))
        return 0
    if args.seconds <= 0 or args.startup_timeout <= 0:
        ap.error('--seconds and --startup-timeout must be positive')
    if not args.root and not args.report_only:
        ap.error('--root is required')
    # Step CMAP: the arena is a BML+ custom map now, no longer installed as Level_13.NMO.
    if args.map == 'auto':
        arena = pathlib.Path(args.root) / 'ModLoader' / 'Maps' / 'BMMO_Mechanism_Arena.nmo' if args.root else None
        args.map = str(arena) if args.level == 13 and arena is not None and arena.is_file() else ''
    elif args.map == 'none':
        args.map = ''
    if args.map:
        print('custom map: %s in slot %d' % (args.map, args.level))
    args.links = dict(parse_link(s) for s in args.link)
    args.drop_downs = dict(parse_drop_down(s) for s in args.drop_down)
    for spec in args.outage:
        parse_outage(spec)              # a bad spec fails here, not in a proxy thread
    args.expect_refused = parse_expect_refused(args.expect_refused)
    args.events = [parse_event(s) for s in args.event]
    if args.event_client >= 0 or args.event_tick:
        kinds = [(k, ms) for k, ms in (('blackout', args.blackout_ms), ('spike', args.spike_ms), ('step', args.step_ms)) if ms > 0]
        if len(kinds) > 1:
            ap.error('one of --blackout-ms, --spike-ms, --step-ms per --event-client')
        kind, ms = kinds[0] if kinds else ('none', 0.0)
        # Client 2 unless named: the design's event client, and the same
        # client in a control and its arms, so the band compares like with like.
        client = args.event_client if args.event_client >= 0 else min(2, args.players - 1)
        args.events.append(LinkEvent(client, args.event_tick, kind, ms, args.event_ms,
                                     args.event_dir, args.event_until))
    for ev in args.events:
        if ev.kind == 'stall' and args.stall_client == ev.client and args.stall_ms > 0:
            ap.error('client %d has both --stall-client and a stall --event: one stall knob per client' % ev.client)
    if args.window_origin < 0:
        ticks = [ev.tick for ev in args.events] or [int(b) for _, _, b in (s.partition(':') for s in args.join_late)
                                                     if b != 'cue']
        args.window_origin = min(ticks) if ticks else 0
    args.client_arg_for = {}
    for spec in args.client_arg_for_specs:
        index, _, rest = spec.partition(':')
        args.client_arg_for.setdefault(int(index), []).extend(shlex.split(rest))
    # Step DX9: N:cue - asked at tick 0 in the report, cued by the server's log.
    specs = [(int(a), b) for a, _, b in (s.partition(':') for s in args.join_late)]
    args.join_cue = set(i for i, b in specs if b == 'cue')
    args.join_late = dict((i, 0 if b == 'cue' else int(b)) for i, b in specs)
    if 0 in args.join_late:
        ap.error('--join-late: not the host (client 0)')
    try:
        args.leave = parse_client_ticks(args.leave, args.players, '--leave')
        args.rejoin_delay = {}
        args.rejoin = parse_client_ticks(args.rejoin, args.players, '--rejoin', args.rejoin_delay)
    except ValueError as e:
        ap.error(str(e))
    for i in sorted(set(args.leave) & set(args.rejoin)):
        if args.leave[i] <= args.rejoin[i]:
            ap.error('client %d leaves at %d, before its rejoin at %d' % (i, args.leave[i], args.rejoin[i]))
    if args.report_only:
        return report_only(args)

    base = pathlib.Path(args.out) / args.label
    if base.exists():
        ap.error('output already exists; choose another --label or clean it explicitly: ' + str(base))
    base.mkdir(parents=True)

    results, failed = [], 0
    base_port = args.port
    for n, sector in enumerate(int(s) for s in args.sectors.split(',')):
        args.port = base_port + n
        print(f'=== sector {sector}: {args.players} players, {args.seconds}s '
              f'(port {args.port}) ===', flush=True)
        case = run_case(args, sector, base / f'sector{sector}')
        for j in case['journal']:
            case.setdefault('replays', []).append(replay(args, j, base))
            case.setdefault('events', []).append(journal_events(args, j))
        problems, warnings = verdict(case, {ev.client for ev in args.events if ev.kind != 'none'},
                                     args.expect_refused)
        case['problems'], case['warnings'] = problems, warnings
        failed += bool(problems)
        results.append(case)

        for i, c in enumerate(case['clients']):
            print(f'  client{i} rc={c["exit_code"]} {c["metrics"]}', flush=True)
        for r in case.get('replays', []):
            print(f'  replay rc={r["exit_code"]} {r["summary"]}', flush=True)
        inter = case['interaction']
        print(f'  mechanisms touched ({len(inter["mechanism_bodies_corrected"])}): '
              f'{", ".join(inter["mechanism_bodies_corrected"]) or "none"}', flush=True)
        print(f'  peer balls corrected ({len(inter["peer_balls_corrected"])}): '
              f'{", ".join(inter["peer_balls_corrected"]) or "none"}', flush=True)
        for e in case.get('events', []):
            print(f'  ball types physicalized: {e["physicalize_by_ball_type"]}; '
                  f'sectors entered: {e["distinct_sectors_entered"]}; '
                  f'mechanisms revived: {len(e["mechanisms_revived"])}', flush=True)
        if warnings:
            print(f'  note: {"; ".join(warnings)}', flush=True)
        print(f'  VERDICT: {"PASS" if not problems else "FAIL - " + "; ".join(problems)}',
              flush=True)
        # H6 and H8: the event report and the criteria, beside the verdict
        # (which keeps its meaning: the run's own health).
        emit_report(args, case, base / f'sector{sector}')
        (base / 'summary.json').write_text(json.dumps(results, indent=2), encoding='utf8')
        # Let the previous session's processes actually go before the next
        # server binds.  One server plus N headless clients is N+1 full
        # Ballance engines stepping at 66 Hz; starting the next set while the
        # last is still tearing down starves the new clients of CPU, and a
        # client that cannot step its own physics (frames far below tick)
        # reports input starvation and re-anchors.  That is a host capacity
        # limit, not a protocol fault - the server's journal still replayed
        # bit-exact through it - but it makes the suite flap, so leave a gap.
        time.sleep(args.gap)

    print(f'\n{len(results) - failed}/{len(results)} sectors passed; wrote '
          + str(base / 'summary.json'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
