#!/usr/bin/env python3
"""Read BMMO session journals (.bmjr) and merge them with the client's log.

    journal_trace.py <a.bmjr> [b.bmjr ...] [--log ModLoader.log ...]
                     [--around TICK] [--window 40] [--nonempty]
    journal_trace.py <a.bmjr> [...] --list
    journal_trace.py <a.bmjr> <b.bmjr> [...] --diff
    journal_trace.py --selftest [--selftest-out DIR]

The default output is one merged timeline, one block per tick: the server
journal's inputs, events, notes and hash/pose/probe as "tick N  [server] ..."
lines, then every client journal's own tick hash, corrections, notes and
snapshots as "  [client p<id>] ..." lines, then the ModLoader.log lines that
carry that tick as "  [log <file>] ..." lines.  --around/--window cut that down
to one region.  An event the world consumed later than it was stamped for
shows as "event@<stamped tick>", which is a symptom in itself.  --list is the
fast triage pass (header, members, tick range, every player/event/note/
correction, the checkpoint ticks) without simulating or merging; it drops the
INPUT and TICK payloads as it reads, so it stays usable on a journal at the
256 MB cap.  --diff compares the first two journals tick by tick and reports
"matched=M/N first_divergence=T" for the world hash and the pose hash, plus
the per-body disagreements between a server journal's FULL checkpoints and a
client journal's RECEIVED ones.  Two server journals are the only pair whose
hashes are of the same world (that is what the SimTool's --write-journal is
for); a server and a client journal only share the server's snapshots, so
there the hashes are information and the exit code follows the checkpoints.
Two client journals of one room have no authoritative half at all and their
hashes are all there is, so they count again (a rollback on either side is a
divergence too).  Exit code 3 when --diff finds a divergence, 1 when it had
nothing to compare.  --nonempty drops the ticks
that carry nothing but a TICK record.

The format is the one BallanceMMOCommon/include/session/journal.hpp writes on
both sides (docs/session-journal-plan.md, design 9.15): magic "BMMOJRNL", u32 version,
then records of u8 tag + u32 payload size, everything little-endian and field
by field, so a reader can skip a tag it does not know and a truncated tail
(the crashed server case, which is the whole point of a black box) only costs
the records after the cut.  Python 3.8+, standard library only.
"""
import argparse
import datetime
import io
import mmap
import os
import re
import struct
import sys
import tempfile

MAGIC = b"BMMOJRNL"
VERSION = 1

TAG_HEADER = 0
TAG_PLAYER = 1
TAG_INPUT = 2
TAG_EVENT = 3
TAG_TICK = 4
TAG_CHECKPOINT = 5
TAG_NOTE = 6
TAG_CORRECTION = 7
TAG_NAMES = {TAG_HEADER: "HEADER", TAG_PLAYER: "PLAYER", TAG_INPUT: "INPUT", TAG_EVENT: "EVENT",
             TAG_TICK: "TICK", TAG_CHECKPOINT: "CHECKPOINT", TAG_NOTE: "NOTE", TAG_CORRECTION: "CORRECTION"}

KIND_SERVER = 0
KIND_CLIENT = 1

# Limits from the spec: a forged or corrupt count must never make the reader
# allocate.  MAX_PAYLOAD is our own guard - no legal record comes close (the
# biggest is a 4096-body checkpoint, under a megabyte).
MAX_STR = 4096
MAX_CHECKPOINT_BODIES = 4096
MAX_CONVEX = 8
MAX_BALLS = 4
MAX_CONCAVE = 8
MAX_PAYLOAD = 1 << 24

INPUT_FRESH = 1
INPUT_RELAYED = 2
CHECKPOINT_FULL = 1
CHECKPOINT_LOCAL = 2
CHECKPOINT_RECEIVED = 4

EVENT_TYPES = {0: "Physicalize", 1: "Unphysicalize", 2: "Sector", 3: "Finish", 4: "BodyRevived"}
CORRECTION_KINDS = {0: "mismatch", 1: "rollback", 2: "hard", 3: "blend", 4: "resync",
                    5: "too_far", 6: "frozen", 7: "unmatched"}
BODY_KINDS = {0: "ball", 1: "mechanism"}
# input_frame::keys bits (entity/session.hpp): the four navigation leaves,
# then shift and space, which the server records but does not consume.
KEY_CHARS = "0123S_"
# input_frame::flags bits.
INPUT_FRAME_FLAGS = ((1, "phys"), (2, "paused"), (4, "nav"))

_U8 = struct.Struct("<B")
_U16 = struct.Struct("<H")
_U32 = struct.Struct("<I")
_I32 = struct.Struct("<i")
_U64 = struct.Struct("<Q")
_F32 = struct.Struct("<f")
_F64 = struct.Struct("<d")


class JournalError(Exception):
    """The file is not a journal at all: magic, version or header unreadable."""


class _Invalid(Exception):
    """One record does not parse; the read stops here and the tail is dropped."""


def f32(value):
    """The double a float32 round trip leaves behind (for exact comparisons)."""
    return _F32.unpack(_F32.pack(value))[0]


# --------------------------------------------------------------------------
# byte cursor


class _Cursor(object):
    def __init__(self, data):
        self.data = data
        self.off = 0

    def remaining(self):
        return len(self.data) - self.off

    def _take(self, size):
        if self.off + size > len(self.data):
            raise _Invalid("payload short by %d bytes" % (self.off + size - len(self.data)))
        self.off += size
        return self.off - size

    def u8(self):
        return _U8.unpack_from(self.data, self._take(1))[0]

    def u16(self):
        return _U16.unpack_from(self.data, self._take(2))[0]

    def u32(self):
        return _U32.unpack_from(self.data, self._take(4))[0]

    def i32(self):
        return _I32.unpack_from(self.data, self._take(4))[0]

    def u64(self):
        return _U64.unpack_from(self.data, self._take(8))[0]

    def f32(self):
        return _F32.unpack_from(self.data, self._take(4))[0]

    def f64(self):
        return _F64.unpack_from(self.data, self._take(8))[0]

    def skip(self, size):
        """Step over a field whose value the caller does not want (but which
        still has to be there: a short payload is a broken record)."""
        self._take(size)

    def f32v(self, count):
        return tuple(self.f32() for _ in range(count))

    def f64v(self, count):
        return tuple(self.f64() for _ in range(count))

    def string(self):
        size = self.u16()
        if size > MAX_STR:
            raise _Invalid("string of %d bytes over the %d limit" % (size, MAX_STR))
        raw = self.data[self._take(size):self.off]
        # The writer turns fixed-size char arrays into strings; never trust a
        # terminator, but do not carry padding NULs into the output either.
        return raw.rstrip(b"\0").decode("utf-8", "replace")


def _put_str(buf, text):
    raw = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    if len(raw) > MAX_STR:
        raise ValueError("string of %d bytes over the %d limit" % (len(raw), MAX_STR))
    buf += _U16.pack(len(raw))
    buf += raw


def _put_vec(buf, values, count, packer):
    if len(values) != count:
        raise ValueError("expected %d components, got %d" % (count, len(values)))
    for value in values:
        buf += packer.pack(value)


# --------------------------------------------------------------------------
# writer


def empty_recipe():
    """A zeroed bmmo_physics_ball_recipe: every EVENT carries one shape."""
    return {"fixed": 0, "start_frozen": 0, "enable_collision": 0, "calc_mass_center": 0,
            "friction": 0.0, "elasticity": 0.0, "mass": 0.0, "linear_damp": 0.0, "rot_damp": 0.0,
            "mass_center": (0.0, 0.0, 0.0), "collision_surface": "",
            "convex": [], "balls": [], "concave": []}


class JournalWriter(object):
    """Writes the same file journal.hpp writes; the mirror of JournalReader.

    Every method takes the record dict shape the reader hands back, so a
    round trip is a plain comparison.  INPUT repeat compression is the
    writer's job (it keeps the last frame per player), exactly as in C++.
    """

    def __init__(self, path, header, max_bytes=256 << 20):
        self.path = path
        self.max_bytes = max_bytes
        self.bytes = 0
        self.records = 0
        self.capped = False
        self._last_frame = {}
        self._handle = open(path, "wb")
        self._handle.write(MAGIC + _U32.pack(VERSION))
        self.bytes += len(MAGIC) + 4
        self._record(TAG_HEADER, self._header_payload(header))

    # -- records ----------------------------------------------------------
    def _record(self, tag, payload):
        if self.capped:
            return
        size = 5 + len(payload)
        if self.bytes + size > self.max_bytes and tag != TAG_HEADER:
            self.capped = True
            note = bytearray()
            note += _U32.pack(0)
            _put_str(note, "cap: %d bytes reached, recording stopped" % self.max_bytes)
            self._emit(TAG_NOTE, bytes(note))
            self.flush()
            return
        self._emit(tag, payload)

    def _emit(self, tag, payload):
        self._handle.write(_U8.pack(tag) + _U32.pack(len(payload)))
        self._handle.write(payload)
        self.bytes += 5 + len(payload)
        self.records += 1

    def raw(self, tag, payload):
        """A record with a tag the reader may not know (forward compatibility)."""
        self._record(tag, bytes(payload))

    @staticmethod
    def _header_payload(header):
        buf = bytearray()
        buf += _U8.pack(header["kind"])
        buf += _U32.pack(header["session"])
        buf += _I32.pack(header["level"])
        buf += _I32.pack(header["seed"])
        buf += _F32.pack(header["spawn_impulse"])
        buf += _U32.pack(header["input_delay"])
        buf += _U32.pack(header["checkpoint_ticks"])
        buf += _U32.pack(header["first_tick"])
        buf += _U64.pack(header["anchor_hash"])
        buf += _U64.pack(header["anchor_surfaces"])
        _put_str(buf, header["build_id"])
        buf += _U64.pack(header["utc_ms"])
        buf += _U32.pack(header["own_player"])
        buf += _U8.pack(header["own_join_order"])
        return bytes(buf)

    def player(self, record):
        buf = bytearray()
        buf += _U32.pack(record["tick"])
        buf += _U32.pack(record["id"])
        buf += _U8.pack(record["join_order"])
        buf += _U8.pack(1 if record["added"] else 0)
        _put_str(buf, record.get("name", ""))
        self._record(TAG_PLAYER, bytes(buf))

    def input(self, record):
        frame = record["frame"]
        key = record["id"]
        packed = self._pack_frame(frame)
        repeat = self._last_frame.get(key) == packed
        buf = bytearray()
        buf += _U32.pack(record["tick"])
        buf += _U32.pack(key)
        buf += _U8.pack(1 if repeat else 0)
        buf += _U8.pack(record.get("flags", 0))
        if not repeat:
            buf += packed
            self._last_frame[key] = packed
        self._record(TAG_INPUT, bytes(buf))

    @staticmethod
    def _pack_frame(frame):
        buf = bytearray()
        buf += _U8.pack(frame["keys"])
        _put_vec(buf, frame["cam_right"], 3, _F32)
        _put_vec(buf, frame["cam_up"], 3, _F32)
        _put_vec(buf, frame["cam_dir"], 3, _F32)
        buf += _U8.pack(frame["ball_type"])
        buf += _U8.pack(frame["flags"])
        return bytes(buf)

    def event(self, record):
        self._record(TAG_EVENT, self._event_payload(record))

    @classmethod
    def _event_payload(cls, record):
        buf = bytearray()
        buf += _U32.pack(record["tick"])
        buf += _U32.pack(record["id"])
        buf += _U8.pack(record["type"])
        buf += _U8.pack(record["ball_type"])
        buf += _U8.pack(record["flags"])
        _put_vec(buf, record["position"], 3, _F32)
        _put_vec(buf, record["rotation"], 9, _F32)
        buf += _I32.pack(record["sector"])
        _put_str(buf, record.get("name", ""))
        cls._put_recipe(buf, record.get("recipe") or empty_recipe())
        # The stamped tick, trailing so a reader that predates it still parses
        # the record; journal.hpp substitutes `tick` for a zero the same way.
        stamp = record.get("event_tick") or 0
        buf += _U32.pack(stamp if stamp else record["tick"])
        return bytes(buf)

    @staticmethod
    def _put_recipe(buf, recipe):
        buf += _U8.pack(1 if recipe["fixed"] else 0)
        buf += _U8.pack(1 if recipe["start_frozen"] else 0)
        buf += _U8.pack(1 if recipe["enable_collision"] else 0)
        buf += _U8.pack(1 if recipe["calc_mass_center"] else 0)
        for name in ("friction", "elasticity", "mass", "linear_damp", "rot_damp"):
            buf += _F32.pack(recipe[name])
        _put_vec(buf, recipe["mass_center"], 3, _F32)
        _put_str(buf, recipe["collision_surface"])
        buf += _I32.pack(len(recipe["convex"]))
        for name in recipe["convex"]:
            _put_str(buf, name)
        buf += _I32.pack(len(recipe["balls"]))
        for ball in recipe["balls"]:
            _put_vec(buf, ball["center"], 3, _F32)
            buf += _F32.pack(ball["radius"])
        buf += _I32.pack(len(recipe["concave"]))
        for name in recipe["concave"]:
            _put_str(buf, name)

    def tick(self, record):
        buf = bytearray()
        buf += _U32.pack(record["tick"])
        buf += _U64.pack(record["hash"])
        buf += _U64.pack(record["pose"])
        buf += _I32.pack(record["cores"])
        buf += _U32.pack(record["ms"])
        _put_str(buf, record.get("probe_name", ""))
        _put_vec(buf, record["probe_position"], 3, _F64)
        _put_vec(buf, record["probe_speed"], 3, _F32)
        self._record(TAG_TICK, bytes(buf))

    def checkpoint(self, record):
        bodies = record["bodies"]
        if len(bodies) > MAX_CHECKPOINT_BODIES:
            raise ValueError("%d bodies over the %d limit" % (len(bodies), MAX_CHECKPOINT_BODIES))
        buf = bytearray()
        buf += _U32.pack(record["tick"])
        buf += _U8.pack(record["flags"])
        buf += _U32.pack(len(bodies))
        for body in bodies:
            buf += _U8.pack(body["kind"])
            buf += _U32.pack(body["owner"])
            _put_str(buf, body.get("name", ""))
            _put_vec(buf, body["position"], 3, _F64)
            _put_vec(buf, body["rotation"], 4, _F64)
            _put_vec(buf, body["linear"], 3, _F32)
            _put_vec(buf, body["angular"], 3, _F32)
            buf += _U8.pack(body["flags"])
        self._record(TAG_CHECKPOINT, bytes(buf))

    def note(self, record):
        buf = bytearray()
        buf += _U32.pack(record["tick"])
        _put_str(buf, record["text"])
        self._record(TAG_NOTE, bytes(buf))

    def correction(self, record):
        buf = bytearray()
        buf += _U32.pack(record["tick"])
        buf += _U32.pack(record["local_tick"])
        buf += _U8.pack(record["kind"])
        _put_str(buf, record.get("entity", ""))
        buf += _F32.pack(record["error_m"])
        buf += _F32.pack(record["velocity_error"])
        _put_vec(buf, record["local_position"], 3, _F64)
        _put_vec(buf, record["server_position"], 3, _F64)
        self._record(TAG_CORRECTION, bytes(buf))

    # -- lifecycle --------------------------------------------------------
    def flush(self):
        if self._handle:
            self._handle.flush()

    def close(self):
        if self._handle:
            self._handle.close()
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


# --------------------------------------------------------------------------
# reader


class JournalTick(object):
    """Every record of one tick; a group exists when the tick has any record."""

    __slots__ = ("tick", "players", "inputs", "events", "has_tick", "record",
                 "checkpoints", "notes", "corrections")

    def __init__(self, tick):
        self.tick = tick
        self.players = []
        self.inputs = []
        self.events = []
        self.has_tick = False
        self.record = None
        self.checkpoints = []
        self.notes = []
        self.corrections = []

    def bare(self):
        """Only a TICK record: nothing a triage pass wants to look at."""
        return not (self.players or self.inputs or self.events or self.checkpoints
                    or self.notes or self.corrections)


class Journal(object):
    def __init__(self, path, summary=False):
        self.path = path
        # summary: the --list pass keeps only what it prints.  INPUT and TICK
        # records are the bulk of a file (a 256 MB journal holds millions of
        # them), so retaining a dict per record costs gigabytes for a listing
        # that never looks at one.
        self.summary = summary
        self.header = None
        self.initial_players = []
        self.ticks = []
        self.by_tick = {}
        self.tick_ids = {}          # summary mode: tick -> has a TICK record
        self.notes = []
        self.records = 0
        self.tick_record_count = 0
        self.input_count = 0
        self.first_record = None    # first/last TICK record in file order (wall clock)
        self.last_record = None
        self.seen_tick = False
        self.duplicate_ticks = 0
        self.first_duplicate_tick = None
        self.orphan_inputs = 0
        self.first_orphan_input = None
        self.unknown_records = 0
        self.unknown_tags = {}
        self.bytes_read = 0
        self.bytes_dropped = 0
        self.trailing_bytes = 0
        self.size = 0
        self.warning = ""
        self.label = ""

    # -- record intake (the reader's side) ---------------------------------
    def count_tick(self, tick, tick_record=False):
        """Summary mode's stand-in for a group; returns True for a duplicate TICK."""
        had = self.tick_ids.get(tick, False)
        if tick_record:
            self.tick_ids[tick] = True
            return had
        if tick not in self.tick_ids:
            self.tick_ids[tick] = False
        return False

    def group(self, tick):
        """The tick's group, created on demand; also counts the tick."""
        if self.summary:
            self.count_tick(tick)
        return self.by_tick.setdefault(tick, JournalTick(tick))

    def add_input(self, record):
        self.input_count += 1
        if self.summary:
            self.count_tick(record["tick"])
        else:
            self.group(record["tick"]).inputs.append(record)

    def add_tick_record(self, record):
        tick = record["tick"]
        if self.summary:
            duplicate = self.count_tick(tick, tick_record=True)
        else:
            group = self.group(tick)
            duplicate = group.has_tick
            group.has_tick = True
            # The last record of a tick wins: after a resync that is the world
            # the client went on with.
            group.record = record
        if duplicate:
            self.duplicate_ticks += 1
            if self.first_duplicate_tick is None:
                self.first_duplicate_tick = tick
        else:
            self.tick_record_count += 1
        if self.first_record is None:
            self.first_record = record
        self.last_record = record
        self.seen_tick = True

    def group_count(self):
        return len(self.tick_ids) if self.summary else len(self.ticks)

    @property
    def kind(self):
        return self.header["kind"]

    @property
    def is_server(self):
        return self.header["kind"] == KIND_SERVER

    def default_label(self):
        if self.is_server:
            return "server"
        return "client p%d" % self.header["own_player"]

    def tick_range(self):
        if self.summary:
            if not self.tick_ids:
                return (None, None)
            return (min(self.tick_ids), max(self.tick_ids))
        if not self.ticks:
            return (None, None)
        return (self.ticks[0].tick, self.ticks[-1].tick)

    def tick_records(self):
        return [group.record for group in self.ticks if group.has_tick]

    def checkpoints(self):
        out = []
        for group in self.ticks:
            out.extend(group.checkpoints)
        return out

    def corrections(self):
        out = []
        for group in self.ticks:
            out.extend(group.corrections)
        return out

    def events(self):
        out = []
        for group in self.ticks:
            out.extend(group.events)
        return out

    def players(self):
        out = []
        for group in self.ticks:
            out.extend(group.players)
        return out


# A repeat INPUT whose player has no previous frame means the record that
# carried the frame was lost.  journal.hpp keeps the zeroed frame rather than
# guessing and reads on; so must we, or the two readers disagree about the
# contents of exactly the damaged file a black box exists for.
_ZERO_FRAME = {"keys": 0, "cam_right": (0.0, 0.0, 0.0), "cam_up": (0.0, 0.0, 0.0),
               "cam_dir": (0.0, 0.0, 0.0), "ball_type": 0, "flags": 0}


class JournalReader(object):
    """Parses one .bmjr file into a Journal, records grouped by tick.

    Unknown tags are skipped and counted.  A record cut off by the end of the
    file, or a payload that does not parse, ends the read: everything before
    it is kept and the remaining bytes are reported as dropped.  In summary
    mode (--list) the INPUT and TICK records are parsed and counted but not
    kept: only what the listing prints survives.
    """

    def __init__(self, data, path=None, summary=False):
        self.data = data
        self.path = path
        self.summary = summary

    def read(self):
        data = self.data
        journal = Journal(self.path, self.summary)
        journal.size = len(data)
        if len(data) < len(MAGIC) + 4 or data[:len(MAGIC)] != MAGIC:
            raise JournalError("not a BMMO journal (bad magic)")
        version = _U32.unpack_from(data, len(MAGIC))[0]
        if version != VERSION:
            raise JournalError("journal version %d, this reader knows %d" % (version, VERSION))
        off = len(MAGIC) + 4
        record = self._next(off)
        if record is None or record[0] != TAG_HEADER:
            raise JournalError("no HEADER record")
        try:
            journal.header = self._header(record[1])
        except _Invalid as err:
            raise JournalError("HEADER does not parse: %s" % err)
        journal.records = 1
        off = record[2]
        journal.label = journal.default_label()
        last_frame = {}
        warnings = []
        while off < len(data):
            record = self._next(off)
            if record is None:
                warnings.append("truncated tail")
                break
            tag, payload, end = record
            if tag not in TAG_NAMES:
                journal.unknown_records += 1
                journal.unknown_tags[tag] = journal.unknown_tags.get(tag, 0) + 1
                journal.records += 1
                off = end
                continue
            try:
                self._dispatch(journal, tag, payload, last_frame)
            except _Invalid as err:
                warnings.append("%s record at offset %d: %s" % (TAG_NAMES[tag], off, err))
                break
            journal.records += 1
            off = end
        journal.bytes_read = off
        journal.bytes_dropped = len(data) - off
        journal.ticks = [journal.by_tick[tick] for tick in sorted(journal.by_tick)]
        if journal.duplicate_ticks:
            warnings.append("%d duplicate TICK records (first at tick %d, kept the last of each)"
                            % (journal.duplicate_ticks, journal.first_duplicate_tick))
        if journal.orphan_inputs:
            warnings.append("%d repeat INPUT records with no previous frame (first at tick %d, frame zeroed)"
                            % (journal.orphan_inputs, journal.first_orphan_input))
        if journal.trailing_bytes:
            warnings.append("%d unread payload bytes (a newer writer?)" % journal.trailing_bytes)
        journal.warning = "; ".join(warnings)
        return journal

    def _next(self, off):
        data = self.data
        if off + 5 > len(data):
            return None
        tag = data[off]
        size = _U32.unpack_from(data, off + 1)[0]
        if size > MAX_PAYLOAD or off + 5 + size > len(data):
            return None
        return tag, data[off + 5:off + 5 + size], off + 5 + size

    def _dispatch(self, journal, tag, payload, last_frame):
        cursor = _Cursor(payload)
        if tag == TAG_HEADER:
            raise _Invalid("a second HEADER record")
        elif tag == TAG_PLAYER:
            record = self._player(cursor)
            journal.group(record["tick"]).players.append(record)
            # The founding members are the adds written before the first TICK
            # record; a removal at the same tick is a record, not a member.
            if not journal.seen_tick and record["added"] and record["tick"] == journal.header["first_tick"]:
                journal.initial_players.append(record)
        elif tag == TAG_INPUT:
            if journal.summary:
                record = self._input_summary(cursor, last_frame)
            else:
                record = self._input(cursor, last_frame)
            if record.get("orphan"):
                journal.orphan_inputs += 1
                if journal.first_orphan_input is None:
                    journal.first_orphan_input = record["tick"]
            journal.add_input(record)
        elif tag == TAG_EVENT:
            record = self._event(cursor)
            journal.group(record["tick"]).events.append(record)
        elif tag == TAG_TICK:
            journal.add_tick_record(self._tick_summary(cursor) if journal.summary else self._tick(cursor))
        elif tag == TAG_CHECKPOINT:
            record = self._checkpoint(cursor)
            if journal.summary:
                record["bodies"] = []       # --list prints the tick and the flags only
            journal.group(record["tick"]).checkpoints.append(record)
        elif tag == TAG_NOTE:
            record = self._note(cursor)
            journal.group(record["tick"]).notes.append(record)
            journal.notes.append(record)
        elif tag == TAG_CORRECTION:
            record = self._correction(cursor)
            journal.group(record["tick"]).corrections.append(record)
        journal.trailing_bytes += cursor.remaining()

    @staticmethod
    def _header(payload):
        cursor = _Cursor(payload)
        header = {"kind": cursor.u8(), "session": cursor.u32(), "level": cursor.i32(), "seed": cursor.i32(),
                  "spawn_impulse": cursor.f32(), "input_delay": cursor.u32(), "checkpoint_ticks": cursor.u32(),
                  "first_tick": cursor.u32(), "anchor_hash": cursor.u64(), "anchor_surfaces": cursor.u64(),
                  "build_id": cursor.string(), "utc_ms": cursor.u64(), "own_player": cursor.u32(),
                  "own_join_order": cursor.u8()}
        return header

    @staticmethod
    def _player(cursor):
        return {"tick": cursor.u32(), "id": cursor.u32(), "join_order": cursor.u8(),
                "added": cursor.u8() != 0, "name": cursor.string()}

    @staticmethod
    def _input(cursor, last_frame):
        tick = cursor.u32()
        player = cursor.u32()
        repeat = cursor.u8() != 0
        flags = cursor.u8()
        orphan = False
        if repeat:
            frame = last_frame.get(player)
            if frame is None:
                # The record that carried the frame was lost; keep the zeroed
                # one and read on, exactly as journal.hpp does.
                frame, orphan = _ZERO_FRAME, True
            frame = dict(frame)
        else:
            frame = {"keys": cursor.u8(), "cam_right": cursor.f32v(3), "cam_up": cursor.f32v(3),
                     "cam_dir": cursor.f32v(3), "ball_type": cursor.u8(), "flags": cursor.u8()}
            last_frame[player] = frame
        record = {"tick": tick, "id": player, "flags": flags, "repeat": repeat, "frame": frame}
        if orphan:
            record["orphan"] = True
        return record

    # The frame body: keys, three float3 camera axes, ball type, flags.
    _FRAME_BYTES = 1 + 3 * 12 + 1 + 1

    @classmethod
    def _input_summary(cls, cursor, last_frame):
        """--list never prints a frame, and there are millions of them.

        The payload is still walked to the end (a short one is as broken here
        as anywhere), but nothing is built from it except the repeat/orphan
        bookkeeping the warning needs.
        """
        tick = cursor.u32()
        player = cursor.u32()
        repeat = cursor.u8() != 0
        cursor.skip(1)                          # flags
        orphan = False
        if repeat:
            orphan = player not in last_frame
        else:
            cursor.skip(cls._FRAME_BYTES)
            last_frame[player] = True
        record = {"tick": tick, "id": player, "repeat": repeat}
        if orphan:
            record["orphan"] = True
        return record

    @staticmethod
    def _tick_summary(cursor):
        """Only the tick and the ms; the hashes and the probe are not listed."""
        tick = cursor.u32()
        cursor.skip(8 + 8 + 4)                  # hash, pose, cores
        ms = cursor.u32()
        cursor.string()                         # probe name (still length checked)
        cursor.skip(3 * 8 + 3 * 4)              # probe position, probe speed
        return {"tick": tick, "ms": ms}

    @classmethod
    def _event(cls, cursor):
        record = {"tick": cursor.u32(), "id": cursor.u32(), "type": cursor.u8(), "ball_type": cursor.u8(),
                  "flags": cursor.u8(), "position": cursor.f32v(3), "rotation": cursor.f32v(9),
                  "sector": cursor.i32(), "name": cursor.string(), "recipe": cls._recipe(cursor)}
        # `tick` is the tick the world APPLIED the event at, `event_tick` the
        # tick stamped on the event itself (they differ when the event arrived
        # late).  A file written before the field existed has neither the four
        # bytes nor the distinction.
        record["event_tick"] = cursor.u32() if cursor.remaining() >= 4 else record["tick"]
        return record

    @staticmethod
    def _recipe(cursor):
        recipe = {"fixed": cursor.u8(), "start_frozen": cursor.u8(), "enable_collision": cursor.u8(),
                  "calc_mass_center": cursor.u8(), "friction": cursor.f32(), "elasticity": cursor.f32(),
                  "mass": cursor.f32(), "linear_damp": cursor.f32(), "rot_damp": cursor.f32(),
                  "mass_center": cursor.f32v(3), "collision_surface": cursor.string(),
                  "convex": [], "balls": [], "concave": []}
        count = cursor.i32()
        if count < 0 or count > MAX_CONVEX:
            raise _Invalid("convex count %d out of range" % count)
        recipe["convex"] = [cursor.string() for _ in range(count)]
        count = cursor.i32()
        if count < 0 or count > MAX_BALLS:
            raise _Invalid("ball count %d out of range" % count)
        recipe["balls"] = [{"center": cursor.f32v(3), "radius": cursor.f32()} for _ in range(count)]
        count = cursor.i32()
        if count < 0 or count > MAX_CONCAVE:
            raise _Invalid("concave count %d out of range" % count)
        recipe["concave"] = [cursor.string() for _ in range(count)]
        return recipe

    @staticmethod
    def _tick(cursor):
        return {"tick": cursor.u32(), "hash": cursor.u64(), "pose": cursor.u64(), "cores": cursor.i32(),
                "ms": cursor.u32(), "probe_name": cursor.string(), "probe_position": cursor.f64v(3),
                "probe_speed": cursor.f32v(3)}

    @staticmethod
    def _checkpoint(cursor):
        tick = cursor.u32()
        flags = cursor.u8()
        count = cursor.u32()
        if count > MAX_CHECKPOINT_BODIES:
            raise _Invalid("checkpoint of %d bodies over the %d limit" % (count, MAX_CHECKPOINT_BODIES))
        bodies = []
        for _ in range(count):
            bodies.append({"kind": cursor.u8(), "owner": cursor.u32(), "name": cursor.string(),
                           "position": cursor.f64v(3), "rotation": cursor.f64v(4),
                           "linear": cursor.f32v(3), "angular": cursor.f32v(3), "flags": cursor.u8()})
        return {"tick": tick, "flags": flags, "bodies": bodies}

    @staticmethod
    def _note(cursor):
        return {"tick": cursor.u32(), "text": cursor.string()}

    @staticmethod
    def _correction(cursor):
        return {"tick": cursor.u32(), "local_tick": cursor.u32(), "kind": cursor.u8(),
                "entity": cursor.string(), "error_m": cursor.f32(), "velocity_error": cursor.f32(),
                "local_position": cursor.f64v(3), "server_position": cursor.f64v(3)}


# Over this size the file is mapped instead of slurped: --list on a journal at
# the 256 MB cap must not need 256 MB of Python heap on top of the records.
MMAP_THRESHOLD = 8 << 20


def read_journal(path, summary=False):
    """Parse one journal; `summary` keeps only what --list prints.

    The heap, not the disk, is what hurts on a capped 256 MB journal: a dict
    per INPUT record costs gigabytes for a listing that never looks at one.
    Summary mode drops them as they are parsed and the file is mapped rather
    than read, so --list stays a triage pass.
    """
    with open(path, "rb") as handle:
        try:
            size = os.fstat(handle.fileno()).st_size
        except OSError:
            size = 0
        mapped = None
        if size > MMAP_THRESHOLD:
            try:
                mapped = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
            except (OSError, ValueError):
                mapped = None
        if mapped is None:
            return JournalReader(handle.read(), path, summary).read()
        try:
            return JournalReader(mapped, path, summary).read()
        finally:
            mapped.close()


# --------------------------------------------------------------------------
# ModLoader.log parsing

# BMLPlus: "[09/04/2026 22:47:06.611] [BallanceMMOClient/INFO]: <message>".
_LOG_TIME_US = re.compile(r"^\[(\d{1,2})/(\d{1,2})/(\d{4})\s+(\d{1,2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?\]\s*")
_LOG_TIME_ISO = re.compile(r"^\[(\d{4})-(\d{2})-(\d{2})[ T](\d{1,2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?\]\s*")
# BMMO's own console: "[09-05 04:09:19] Loading config from config.yml..." -
# local time, no year, no fraction; the server log and the headless session
# client logs are written in it.  What may sit in front of the stamp is the
# console erasing its input line before it prints ("\x1b[0K", and the carriage
# return with it when the reader keeps one).
_LOG_TIME_BMMO = re.compile(r"^(?:\x1b\[[0-9;]*[A-Za-z]|\r)*"
                            r"\[(\d{1,2})-(\d{1,2})\s+(\d{1,2}):(\d{2}):(\d{2})\]\s*")
_LOG_SOURCE = re.compile(r"^\[[^\]]+\]:?\s*")

# The specific shapes first (they say what happened), then the lenient
# "tick N" fallback so a line nobody thought of still lands on its tick.
_LOG_PATTERNS = [
    ("mismatch", re.compile(r"mismatch at tick (\d+)(?:\s*\(local (\d+)\))?")),
    ("rollback", re.compile(r"rollback to tick (\d+)\s*\(")),
    ("resim", re.compile(r"resim tick (\d+):")),
    ("physicalize", re.compile(r"physicalized at tick (\d+)")),
    ("correction", re.compile(r"correction of \S+ for tick (\d+)")),
    ("input", re.compile(r"input edge at tick (\d+)")),
    ("resync", re.compile(r"resync requested \([^)]*\) at tick (\d+)")),
    ("tickbase", re.compile(r"tick base (\d+)")),
    ("tick", re.compile(r"\btick (\d+)")),
]


class LogFile(object):
    def __init__(self, path, year=None):
        self.path = path
        self.name = os.path.basename(path)
        self.lines = 0
        self.matched = 0
        self.first_time = None
        self.last_time = None
        self.by_tick = {}
        # BMMO's console stamps carry no year: the journal says which one it
        # was (see _log_year), and today's is the fallback.
        self.year = year or datetime.datetime.now().year
        self.month = None

    def yearless(self, month):
        """The year a "[MM-DD ...]" line belongs to.

        A log may run across New Year, and the only sign of it in the file is
        the month going backwards, so a December-to-January step moves the
        whole rest of the log into the next year.
        """
        if self.month is not None and month < self.month:
            self.year += 1
        self.month = month
        return self.year


def _log_time(text, dater=None):
    """(timestamp, rest) of a log line; (None, text) when it carries no time.

    `dater` dates BMMO's yearless console stamp from its month (LogFile.yearless).
    """
    match = _LOG_TIME_US.match(text)
    if match:
        month, day, year, hour, minute, second, frac = match.groups()
    else:
        match = _LOG_TIME_ISO.match(text)
        if match:
            year, month, day, hour, minute, second, frac = match.groups()
        else:
            match = _LOG_TIME_BMMO.match(text)
            if not match:
                return None, text
            month, day, hour, minute, second = match.groups()
            frac = None
            year = dater(int(month)) if dater else datetime.datetime.now().year
    micro = int((frac or "0").ljust(6, "0")[:6])
    try:
        stamp = datetime.datetime(int(year), int(month), int(day), int(hour), int(minute), int(second), micro)
    except ValueError:
        return None, text
    return stamp, text[match.end():]


def parse_log(path, year=None):
    log = LogFile(path, year)
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for number, raw in enumerate(handle, 1):
            raw = raw.rstrip("\r\n")
            if not raw.strip():
                continue
            log.lines += 1
            stamp, rest = _log_time(raw, log.yearless)
            if stamp is not None:
                if log.first_time is None:
                    log.first_time = stamp
                log.last_time = stamp
                source = _LOG_SOURCE.match(rest)
                if source:
                    rest = rest[source.end():]
            kind, tick, local = None, None, None
            for name, pattern in _LOG_PATTERNS:
                match = pattern.search(rest)
                if not match:
                    continue
                kind = name
                tick = int(match.group(1))
                if name == "mismatch" and match.lastindex and match.lastindex >= 2 and match.group(2):
                    local = int(match.group(2))
                break
            if tick is None:
                continue
            if kind == "physicalize" and "unphysicalized" in rest:
                kind = "unphysicalize"
            log.matched += 1
            log.by_tick.setdefault(tick, []).append(
                {"file": log.name, "line": number, "time": stamp, "kind": kind, "tick": tick,
                 "local": local, "text": rest})
    return log


def _log_year(journals):
    """The year BMMO's yearless console stamps belong to.

    That console prints LOCAL time, so the header's utc_ms is read as local
    time here too; a run without a journal (or one whose header time is unset)
    leaves today's year as the only guess.
    """
    for journal in journals:
        ms = journal.header.get("utc_ms")
        if not ms:
            continue
        try:
            return datetime.datetime.fromtimestamp(ms / 1000.0).year
        except (OSError, OverflowError, ValueError):
            continue
    return datetime.datetime.now().year


# --------------------------------------------------------------------------
# formatting


# A corrupt or pre-1970 utc_ms (the C++ writers cast a signed count) must not
# cost us the whole report: the records are still readable.  Windows raises
# OSError outside the CRT's year range, other platforms ValueError/OverflowError.
def _fmt_utc(ms):
    if not ms:
        return "unset"
    try:
        stamp = datetime.datetime.fromtimestamp(ms / 1000.0, datetime.timezone.utc)
    except (OSError, OverflowError, ValueError):
        return "utc_ms=%d (out of range)" % ms
    return stamp.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (stamp.microsecond // 1000)


def _fmt_local(ms):
    if not ms:
        return "unset"
    try:
        stamp = datetime.datetime.fromtimestamp(ms / 1000.0)
    except (OSError, OverflowError, ValueError):
        return "utc_ms=%d (out of range)" % ms
    return stamp.strftime("%Y-%m-%d %H:%M:%S.") + "%03d" % (stamp.microsecond // 1000)


def _fmt_clock(stamp):
    if stamp is None:
        return "--:--:--.---"
    return stamp.strftime("%H:%M:%S.") + "%03d" % (stamp.microsecond // 1000)


def _fmt_vec(values, fmt="%.4f"):
    return "(" + ",".join(fmt % value for value in values) + ")"


def _flag_names(flags, table):
    names = [name for bit, name in table if flags & bit]
    left = flags & ~sum(bit for bit, _ in table)
    if left:
        names.append("0x%02x" % left)
    return "|".join(names) if names else "-"


def _keys_text(keys):
    text = "".join(KEY_CHARS[bit] for bit in range(len(KEY_CHARS)) if keys & (1 << bit))
    left = keys >> len(KEY_CHARS)
    if left:
        text += "+0x%02x" % (left << len(KEY_CHARS))
    return "%02x[%s]" % (keys, text)


def _fmt_input(record):
    frame = record["frame"]
    parts = ["p%d" % record["id"], "keys=" + _keys_text(frame["keys"])]
    if frame["flags"]:
        parts.append("in=" + _flag_names(frame["flags"], INPUT_FRAME_FLAGS))
    if frame["ball_type"]:
        parts.append("ball=%d" % frame["ball_type"])
    tags = _flag_names(record["flags"], ((INPUT_FRESH, "fresh"), (INPUT_RELAYED, "relayed")))
    if tags != "-":
        parts.append(tags)
    if record["repeat"]:
        # (rep?): a repeat whose frame was never recorded, so this is a zeroed one.
        parts.append("(rep?)" if record.get("orphan") else "(rep)")
    return " ".join(parts)


def _fmt_recipe(recipe):
    return ("recipe fixed=%d frozen=%d coll=%d mass=%.3f fric=%.3f elast=%.3f surf=%s convex=%d balls=%d concave=%d"
            % (recipe["fixed"], recipe["start_frozen"], recipe["enable_collision"], recipe["mass"],
               recipe["friction"], recipe["elasticity"], recipe["collision_surface"] or "-",
               len(recipe["convex"]), len(recipe["balls"]), len(recipe["concave"])))


def _fmt_event(record):
    # "@N": the event was stamped for tick N and applied here instead, which is
    # a symptom in itself (the event arrived after the world had passed N).
    stamp = ""
    if record.get("event_tick", record["tick"]) != record["tick"]:
        stamp = "@%d" % record["event_tick"]
    text = "event%s p%d %s ball=%d flags=%02x sector=%d pos=%s" % (
        stamp, record["id"], EVENT_TYPES.get(record["type"], "type%d" % record["type"]),
        record["ball_type"], record["flags"], record["sector"], _fmt_vec(record["position"], "%.3f"))
    if record["name"]:
        text += " name=%s" % record["name"]
    recipe = record["recipe"]
    if record["type"] == 0 or recipe["convex"] or recipe["balls"] or recipe["concave"]:
        text += " " + _fmt_recipe(recipe)
    return text


def _fmt_tick(record):
    return "hash=%016x pose=%016x cores=%d ms=%d probe=%s pos=%s v=%s" % (
        record["hash"], record["pose"], record["cores"], record["ms"], record["probe_name"] or "-",
        _fmt_vec(record["probe_position"], "%.6f"), _fmt_vec(record["probe_speed"], "%.4f"))


def _checkpoint_flags(flags):
    return _flag_names(flags, ((CHECKPOINT_FULL, "FULL"), (CHECKPOINT_LOCAL, "LOCAL"),
                               (CHECKPOINT_RECEIVED, "RECEIVED")))


def _fmt_checkpoint(record):
    balls = sum(1 for body in record["bodies"] if body["kind"] == 0)
    return "ckpt %s bodies=%d balls=%d mech=%d" % (_checkpoint_flags(record["flags"]),
                                                   len(record["bodies"]), balls,
                                                   len(record["bodies"]) - balls)


def _fmt_correction(record):
    text = "corr %s local=%d err=%.4fm dv=%.4f" % (
        CORRECTION_KINDS.get(record["kind"], "kind%d" % record["kind"]), record["local_tick"],
        record["error_m"], record["velocity_error"])
    if record["entity"]:
        text += " entity=%s local=%s server=%s" % (record["entity"],
                                                   _fmt_vec(record["local_position"], "%.4f"),
                                                   _fmt_vec(record["server_position"], "%.4f"))
    return text


def _fmt_player(record):
    return "player %sp%d join=%d%s" % ("+" if record["added"] else "-", record["id"], record["join_order"],
                                       ' "%s"' % record["name"] if record["name"] else "")


def _group_lines(group):
    """The record lines of one tick group, in reading order."""
    lines = []
    if group.has_tick:
        lines.append(_fmt_tick(group.record))
    if group.inputs:
        lines.append("in " + " | ".join(_fmt_input(record) for record in group.inputs))
    lines.extend(_fmt_event(record) for record in group.events)
    lines.extend("note " + record["text"] for record in group.notes)
    lines.extend(_fmt_player(record) for record in group.players)
    lines.extend(_fmt_checkpoint(record) for record in group.checkpoints)
    lines.extend(_fmt_correction(record) for record in group.corrections)
    return lines


# --------------------------------------------------------------------------
# --list


def print_header(journal):
    header = journal.header
    print("=== %s" % (journal.path or "<memory>"))
    print("header: %s session=%d level=%d seed=%d impulse=%.3f input_delay=%d checkpoint_ticks=%d first_tick=%d"
          % ("server" if journal.is_server else "client", header["session"], header["level"], header["seed"],
             header["spawn_impulse"], header["input_delay"], header["checkpoint_ticks"], header["first_tick"]))
    print("        anchor hash=%016x surfaces=%016x build=%s"
          % (header["anchor_hash"], header["anchor_surfaces"], header["build_id"] or "-"))
    print("        utc=%s (local %s) own_player=%d own_join_order=%d"
          % (_fmt_utc(header["utc_ms"]), _fmt_local(header["utc_ms"]), header["own_player"],
             header["own_join_order"]))
    members = " | ".join('p%d join=%d "%s"' % (record["id"], record["join_order"], record["name"])
                         for record in journal.initial_players if record["added"])
    print("members: %s" % (members or "(none)"))
    first, last = journal.tick_range()
    unknown = ""
    if journal.unknown_records:
        unknown = " unknown=%d (%s)" % (journal.unknown_records,
                                        ", ".join("tag %d x%d" % (tag, count)
                                                  for tag, count in sorted(journal.unknown_tags.items())))
    print("ticks: %s..%s groups=%d tick_records=%d inputs=%d records=%d%s size=%d read=%d dropped=%d"
          % (first if first is not None else "-", last if last is not None else "-", journal.group_count(),
             journal.tick_record_count, journal.input_count, journal.records, unknown, journal.size,
             journal.bytes_read, journal.bytes_dropped))
    if journal.warning:
        print("warning: %s" % journal.warning)


def print_list(journal):
    print_header(journal)
    for name, records, formatter in (("players", journal.players(), _fmt_player),
                                     ("events", journal.events(), _fmt_event),
                                     ("notes", [note for note in journal.notes], lambda r: "note " + r["text"]),
                                     ("corrections", journal.corrections(), _fmt_correction)):
        if not records:
            continue
        print("%s: %d" % (name, len(records)))
        for record in records:
            print("  tick %-8d %s" % (record["tick"], formatter(record)))
    checkpoints = journal.checkpoints()
    if checkpoints:
        by_flags = {}
        for record in checkpoints:
            by_flags.setdefault(_checkpoint_flags(record["flags"]), []).append(record["tick"])
        print("checkpoints: %d" % len(checkpoints))
        for flags in sorted(by_flags):
            ticks = by_flags[flags]
            shown = ", ".join(str(tick) for tick in ticks[:20])
            if len(ticks) > 20:
                shown += ", ... (+%d)" % (len(ticks) - 20)
            print("  %s: %d [%s]" % (flags, len(ticks), shown))


def print_log_summary(log):
    print("=== %s" % log.path)
    print("log: lines=%d with_tick=%d ticks=%d time %s .. %s (local)"
          % (log.lines, log.matched, len(log.by_tick), _fmt_clock(log.first_time), _fmt_clock(log.last_time)))


def print_wall_clock(journals, logs):
    """Header UTC + the ms of the first/last TICK, so "around 21:37" lands on a tick."""
    print("wall clock (journal times converted to this machine's local time; log times are local):")
    for journal in journals:
        header = journal.header
        print("  [%s] header %s = local %s" % (journal.label, _fmt_utc(header["utc_ms"]),
                                               _fmt_local(header["utc_ms"])))
        first, last = journal.first_record, journal.last_record
        if first is not None:
            print("           tick %d ms=%d -> %s ; tick %d ms=%d -> %s"
                  % (first["tick"], first["ms"], _fmt_local(header["utc_ms"] + first["ms"]),
                     last["tick"], last["ms"], _fmt_local(header["utc_ms"] + last["ms"])))
    for log in logs:
        print("  [log %s] %s .. %s  lines=%d with_tick=%d"
              % (log.name, _fmt_clock(log.first_time), _fmt_clock(log.last_time), log.lines, log.matched))


# --------------------------------------------------------------------------
# merged timeline


def print_timeline(journals, logs, around, window, nonempty):
    ticks = set()
    for journal in journals:
        ticks.update(journal.by_tick)
    for log in logs:
        ticks.update(log.by_tick)
    if around is not None:
        low, high = around - window, around + window
        ticks = set(tick for tick in ticks if low <= tick <= high)
    if not ticks:
        print("(no ticks in range)")
        return
    print("timeline: %d ticks%s" % (len(ticks), "" if around is None else
                                    " around %d (+-%d)" % (around, window)))
    for tick in sorted(ticks):
        block = []
        interesting = False
        for journal in journals:
            group = journal.by_tick.get(tick)
            if group is None:
                continue
            if not group.bare():
                interesting = True
            for line in _group_lines(group):
                if journal.is_server:
                    block.append("tick %d  [%s] %s" % (tick, journal.label, line))
                else:
                    block.append("  [%s] %s" % (journal.label, line))
        for log in logs:
            for entry in log.by_tick.get(tick, []):
                interesting = True
                block.append("  [log %s] %s %s %s" % (log.name, _fmt_clock(entry["time"]),
                                                      entry["kind"], entry["text"]))
        if not block or (nonempty and not interesting):
            continue
        if not block[0].startswith("tick "):
            block.insert(0, "tick %d" % tick)
        for line in block:
            print(line)


# --------------------------------------------------------------------------
# --diff


def _hash_diff(name, a, b):
    """matched=M/N first_divergence=T over the ticks both journals recorded."""
    left = dict((record["tick"], record) for record in a.tick_records())
    right = dict((record["tick"], record) for record in b.tick_records())
    common = sorted(set(left) & set(right))
    matched, first = 0, -1
    for tick in common:
        if left[tick][name] == right[tick][name]:
            matched += 1
        elif first < 0:
            first = tick
    print("  %-5s matched=%d/%d first_divergence=%d" % (name + ":", matched, len(common), first))
    return first, len(common)


def _checkpoint_diff(server, client):
    """Server FULL checkpoints against the client's RECEIVED ones, by (kind, owner)."""
    def collect(journal, mask):
        out = {}
        for record in journal.checkpoints():
            if record["flags"] & mask:
                out[record["tick"]] = record
        return out

    left = collect(server, CHECKPOINT_FULL)
    right = collect(client, CHECKPOINT_RECEIVED)
    common = sorted(set(left) & set(right))
    full_received = sum(1 for record in right.values() if record["flags"] & CHECKPOINT_FULL)
    print("  checkpoints: server FULL=%d client RECEIVED=%d (full=%d, delta=%d) common=%d"
          % (len(left), len(right), full_received, len(right) - full_received, len(common)))
    if not common:
        return -1, 0
    first, shown, bad_ticks, bad_bodies = -1, 0, 0, 0
    delta_ticks, delta_bodies = 0, 0
    for tick in common:
        a = dict(((body["kind"], body["owner"]), body) for body in left[tick]["bodies"])
        b = dict(((body["kind"], body["owner"]), body) for body in right[tick]["bodies"])
        shared = set(a) & set(b)
        differing, worst, worst_key = 0, 0.0, None
        for key in shared:
            distance = sum((x - y) ** 2 for x, y in zip(a[key]["position"], b[key]["position"])) ** 0.5
            if distance > 1e-9:
                differing += 1
                if distance > worst:
                    worst, worst_key = distance, key
        only_server, only_client = len(set(a) - set(b)), len(set(b) - set(a))
        # The server sends DELTA snapshots by default: they carry only the
        # bodies that changed, so a body the client was never sent that tick is
        # not a disagreement.  Only a full snapshot has to list them all.
        full = bool(right[tick]["flags"] & CHECKPOINT_FULL)
        missing = only_server if full else 0
        if not full and only_server:
            delta_ticks += 1
            delta_bodies += only_server
        if not (differing or missing or only_client):
            continue
        bad_ticks += 1
        bad_bodies += differing + missing + only_client
        if first < 0:
            first = tick
        if shown < 20:
            shown += 1
            reasons = []
            if differing:
                reasons.append("%d/%d common bodies differ, worst %s owner %d d=%.6g m"
                               % (differing, len(shared), BODY_KINDS.get(worst_key[0], str(worst_key[0])),
                                  worst_key[1], worst))
            if missing:
                reasons.append("%d body/bodies only in the server checkpoint" % missing)
            if only_client:
                reasons.append("%d body/bodies only in the client snapshot" % only_client)
            print("    tick %d (%s, %d common): %s"
                  % (tick, "full" if full else "delta", len(shared), "; ".join(reasons)))
    if shown == 20 and bad_ticks > 20:
        print("    ... (+%d more ticks)" % (bad_ticks - 20))
    if delta_ticks:
        print("  checkpoints: %d delta snapshots did not carry %d bodies the server checkpoint had"
              " (expected for a delta)" % (delta_ticks, delta_bodies))
    print("  checkpoints: disagreeing_ticks=%d bodies=%d first=%d" % (bad_ticks, bad_bodies, first))
    return first, len(common)


def run_diff(journals):
    if len(journals) < 2:
        print("--diff needs two journals", file=sys.stderr)
        return 1
    a, b = journals[0], journals[1]
    print("diff: a=[%s] %s" % (a.label, a.path))
    print("      b=[%s] %s" % (b.label, b.path))
    if a.header["level"] != b.header["level"] or a.header["seed"] != b.header["seed"]:
        print("  note: different level/seed (a level=%d seed=%d, b level=%d seed=%d)"
              % (a.header["level"], a.header["seed"], b.header["level"], b.header["seed"]))
    if a.header["build_id"] != b.header["build_id"]:
        print("  note: different build id (a %s, b %s)" % (a.header["build_id"], b.header["build_id"]))
    # Only two server journals record the same world: that is the case
    # --write-journal exists for (two platforms' replays of one session).
    # Against a server journal a client's TICK hash is of its own predicted
    # world, captured after that frame's corrections over a different body set,
    # so there it is information and the exit code follows the checkpoints.
    # Two clients have no authoritative half at all, and comparing their hashes
    # is how a desync between two players is triaged: there the hashes count.
    mixed = a.is_server != b.is_server
    if mixed:
        print("  note: a client journal's own TICK hashes are of its own predicted world, so the"
              " hash/pose lines below are information only; the exit code follows the checkpoints")
    elif not a.is_server:
        print("  note: neither journal is authoritative: both hashes are of a client's own predicted"
              " world, so a divergence can also be one client's rollback")
    diverged = False
    hash_common = 0
    for name in ("hash", "pose"):
        first, common = _hash_diff(name, a, b)
        hash_common = max(hash_common, common)
        if not common:
            print("  %-5s no common tick records" % (name + ":"))
        elif first >= 0 and not mixed:
            diverged = True
    servers = [journal for journal in journals if journal.is_server]
    clients = [journal for journal in journals if not journal.is_server]
    checkpoint_common = 0
    if servers and clients:
        first, checkpoint_common = _checkpoint_diff(servers[0], clients[0])
        if first >= 0:
            diverged = True
    else:
        print("  checkpoints: need one server and one client journal to compare snapshots")
    # A crashed or boot-failed journal has nothing in common with anything;
    # exiting 0 there would make a CI check pass on a run that never ticked.
    # Which half has to have compared something follows the pair: the hashes
    # when both journals are of the same kind, the checkpoints for a server and
    # a client (the server's snapshots are the only authoritative half there).
    if not mixed and not hash_common:
        print("journal_trace.py: --diff compared nothing: no tick both journals recorded",
              file=sys.stderr)
        return 1
    if mixed and not checkpoint_common:
        print("journal_trace.py: --diff compared nothing authoritative: the server's snapshots are the"
              " only comparable half of a client journal, and no tick carries both", file=sys.stderr)
        return 1
    return 3 if diverged else 0


# --------------------------------------------------------------------------
# selftest


def _same(a, b):
    if isinstance(a, dict) and isinstance(b, dict):
        return set(a) == set(b) and all(_same(a[key], b[key]) for key in a)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(_same(x, y) for x, y in zip(a, b))
    if isinstance(a, bool) or isinstance(b, bool):
        return bool(a) == bool(b)
    return a == b


def _check(what, expected, actual):
    if not _same(expected, actual):
        raise AssertionError("%s:\n  expected %r\n  actual   %r" % (what, expected, actual))


def _frame(keys, seed, ball_type=0, flags=0):
    return {"keys": keys, "cam_right": (f32(seed), f32(-seed), f32(seed * 2)),
            "cam_up": (f32(0.0), f32(1.0), f32(seed / 8.0)),
            "cam_dir": (f32(seed * 3), f32(0.25), f32(-1.5)),
            "ball_type": ball_type, "flags": flags}


def _body(kind, owner, name, base):
    return {"kind": kind, "owner": owner, "name": name,
            "position": (base, base + 1.0, base + 2.0),
            "rotation": (0.0, 0.5, 0.0, 0.8660254037844387),
            "linear": (f32(0.5), f32(-0.25), f32(base)), "angular": (f32(1.5), f32(0.0), f32(-2.5)),
            "flags": 3}


def _synthetic_server():
    """Every tag, every field populated, repeated inputs and a full recipe."""
    header = {"kind": KIND_SERVER, "session": 42, "level": 13, "seed": -1337, "spawn_impulse": f32(1.5),
              "input_delay": 4, "checkpoint_ticks": 660, "first_tick": 100,
              "anchor_hash": 0x0123456789ABCDEF, "anchor_surfaces": 0xFEDCBA9876543210,
              "build_id": "ballanced-0123abc+bridge-9", "utc_ms": 1757000000123,
              "own_player": 0, "own_join_order": 255}
    recipe = {"fixed": 1, "start_frozen": 1, "enable_collision": 1, "calc_mass_center": 1,
              "friction": f32(0.7), "elasticity": f32(0.5), "mass": f32(2.0), "linear_damp": f32(0.1),
              "rot_damp": f32(0.05), "mass_center": (f32(0.0), f32(0.25), f32(-0.5)),
              "collision_surface": "Ball_Wood_Surface",
              "convex": ["Ball_Wood_Mesh", "Ball_Wood_Mesh_LOD"],
              "balls": [{"center": (f32(0.0), f32(0.0), f32(0.0)), "radius": f32(2.0)},
                        {"center": (f32(1.0), f32(-1.0), f32(0.5)), "radius": f32(0.75)}],
              "concave": ["Ball_Wood_Concave"]}
    # The room name and one player name are non-ASCII on purpose: they are
    # player-chosen text, and printing them must survive a console code page
    # that cannot represent them.  Escaped so this script stays pure ASCII.
    room = "\u6d4b\u8bd5\u623f\u95f4"          # a Chinese room name
    chinese = "\u5c0f\u7403\u73a9\u5bb6"       # a Chinese player name
    plan = []
    plan.append(("note", {"tick": 100,
                          "text": 'start: room 7 "%s": Alice(1, join 0), %s(2, join 1)' % (room, chinese)}))
    plan.append(("player", {"tick": 100, "id": 1, "join_order": 0, "added": True, "name": "Alice"}))
    plan.append(("player", {"tick": 100, "id": 2, "join_order": 1, "added": True, "name": chinese}))
    plan.append(("input", {"tick": 101, "id": 1, "flags": INPUT_FRESH, "repeat": False,
                           "frame": _frame(0x0B, 0.5, 3, 5)}))
    plan.append(("input", {"tick": 101, "id": 2, "flags": 0, "repeat": False,
                           "frame": _frame(0x00, 0.25)}))
    plan.append(("event", {"tick": 101, "event_tick": 101, "id": 1, "type": 0, "ball_type": 3, "flags": 1,
                           "position": tuple(f32(v) for v in (1.5, -2.25, 3.0)),
                           "rotation": tuple(f32(v) for v in (1, 0, 0, 0, 1, 0, 0, 0, 1)),
                           "sector": 2, "name": "Ball_Wood", "recipe": recipe}))
    plan.append(("tick", {"tick": 101, "hash": 0x1122334455667788, "pose": 0x99AABBCCDDEEFF00,
                          "cores": 12, "ms": 15, "probe_name": "Ball_Wood_BMMO_2",
                          "probe_position": (1.25, -2.5, 3.125),
                          "probe_speed": (f32(0.5), f32(-0.5), f32(0.0))}))
    # the same frame again for player 1: the writer must compress it
    plan.append(("input", {"tick": 102, "id": 1, "flags": INPUT_FRESH, "repeat": True,
                           "frame": _frame(0x0B, 0.5, 3, 5)}))
    plan.append(("input", {"tick": 102, "id": 2, "flags": INPUT_RELAYED, "repeat": False,
                           "frame": _frame(0x04, -0.75, 1, 1)}))
    plan.append(("tick", {"tick": 102, "hash": 2, "pose": 3, "cores": 12, "ms": 30,
                          "probe_name": "", "probe_position": (0.0, 0.0, 0.0),
                          "probe_speed": (f32(0.0), f32(0.0), f32(0.0))}))
    plan.append(("checkpoint", {"tick": 102, "flags": CHECKPOINT_FULL,
                                "bodies": [_body(0, 1, "Ball_Wood_BMMO_2", 10.0),
                                           _body(1, 7, "Trafo_Piece_01", -3.5)]}))
    # stamped for 101, applied at 103: the late event the two tick fields exist for
    plan.append(("event", {"tick": 103, "event_tick": 101, "id": 2, "type": 2, "ball_type": 0, "flags": 0,
                           "position": (f32(0.0), f32(0.0), f32(0.0)),
                           "rotation": tuple(f32(0.0) for _ in range(9)),
                           "sector": 3, "name": "", "recipe": empty_recipe()}))
    plan.append(("correction", {"tick": 103, "local_tick": 107, "kind": 1, "entity": "Ball_Wood_BMMO_2",
                                "error_m": f32(0.0123), "velocity_error": f32(0.5),
                                "local_position": (1.0, 2.0, 3.0), "server_position": (1.01, 2.0, 3.0)}))
    plan.append(("tick", {"tick": 103, "hash": 4, "pose": 5, "cores": 13, "ms": 45,
                          "probe_name": "Ball_Paper_BMMO_3", "probe_position": (-1.0, 0.5, 2.0),
                          "probe_speed": (f32(1.0), f32(2.0), f32(3.0))}))
    plan.append(("player", {"tick": 104, "id": 3, "join_order": 2, "added": True, "name": "Cara"}))
    plan.append(("player", {"tick": 105, "id": 2, "join_order": 1, "added": False, "name": ""}))
    plan.append(("note", {"tick": 105, "text": "end: room closed"}))
    # last, and big enough that chopping a few bytes truncates exactly one record
    plan.append(("checkpoint", {"tick": 105, "flags": CHECKPOINT_FULL,
                                "bodies": [_body(0, 1, "Ball_Wood_BMMO_2", 20.0),
                                           _body(0, 3, "Ball_Paper_BMMO_4", 21.0),
                                           _body(1, 7, "Trafo_Piece_01", -9.0)]}))
    return header, plan


def _synthetic_client():
    header = {"kind": KIND_CLIENT, "session": 42, "level": 13, "seed": -1337, "spawn_impulse": f32(1.5),
              "input_delay": 4, "checkpoint_ticks": 660, "first_tick": 0,
              "anchor_hash": 0x0123456789ABCDEF, "anchor_surfaces": 0xFEDCBA9876543210,
              "build_id": "ballanced-0123abc+bridge-9", "utc_ms": 1757000000456,
              "own_player": 3, "own_join_order": 2}
    plan = []
    plan.append(("note", {"tick": 0, "text": "anchor: hash 0123456789abcdef surfaces fedcba9876543210"}))
    plan.append(("player", {"tick": 0, "id": 3, "join_order": 2, "added": True, "name": "Cara"}))
    plan.append(("note", {"tick": 100, "text": "assigned: tick base 100"}))
    plan.append(("input", {"tick": 101, "id": 3, "flags": INPUT_FRESH, "repeat": False,
                           "frame": _frame(0x01, 0.125, 3, 5)}))
    plan.append(("input", {"tick": 101, "id": 1, "flags": INPUT_RELAYED, "repeat": False,
                           "frame": _frame(0x0B, 0.5, 3, 5)}))
    plan.append(("tick", {"tick": 101, "hash": 0xDEADBEEFCAFEBABE, "pose": 0x99AABBCCDDEEFF00,
                          "cores": 12, "ms": 16, "probe_name": "Ball_Paper_BMMO_4",
                          "probe_position": (1.25, -2.5, 3.125),
                          "probe_speed": (f32(0.5), f32(-0.5), f32(0.0))}))
    plan.append(("checkpoint", {"tick": 102, "flags": CHECKPOINT_RECEIVED,
                                "bodies": [_body(0, 1, "", 10.0), _body(1, 7, "", -3.5)]}))
    plan.append(("checkpoint", {"tick": 102, "flags": CHECKPOINT_LOCAL,
                                "bodies": [_body(0, 1, "Ball_Wood_BMMO_2", 10.0),
                                           _body(1, 7, "Trafo_Piece_01", -3.5)]}))
    plan.append(("input", {"tick": 102, "id": 3, "flags": INPUT_FRESH, "repeat": True,
                           "frame": _frame(0x01, 0.125, 3, 5)}))
    plan.append(("tick", {"tick": 102, "hash": 7, "pose": 3, "cores": 12, "ms": 31,
                          "probe_name": "Ball_Paper_BMMO_4", "probe_position": (2.0, 0.0, -1.0),
                          "probe_speed": (f32(0.0), f32(0.0), f32(1.0))}))
    for kind in sorted(CORRECTION_KINDS):
        plan.append(("correction", {"tick": 103 + kind, "local_tick": 110 + kind, "kind": kind,
                                    "entity": "Ball_Paper_BMMO_4" if kind != 7 else "",
                                    "error_m": f32(0.25 * (kind + 1)), "velocity_error": f32(0.5 * kind),
                                    "local_position": (1.0 + kind, 2.0, 3.0),
                                    "server_position": (1.0, 2.0, 3.0 + kind)}))
    # an emoji in a user-typed mark: the other half of the print-path check
    plan.append(("note", {"tick": 120, "text": "mark: it looked wrong here \U0001f600"}))
    plan.append(("checkpoint", {"tick": 120, "flags": CHECKPOINT_LOCAL,
                                "bodies": [_body(0, 3, "Ball_Paper_BMMO_4", 30.0)]}))
    plan.append(("note", {"tick": 121, "text": "end: level finished"}))
    return header, plan


def _write_plan(path, header, plan, unknown_after=None):
    writer = JournalWriter(path, header)
    for index, (kind, record) in enumerate(plan):
        getattr(writer, kind)(record)
        if unknown_after is not None and index == unknown_after:
            writer.raw(200, b"a tag from the future")
    writer.close()
    return writer


def _verify(path, header, plan, expect_unknown):
    journal = read_journal(path)
    _check("header", header, journal.header)
    _check("warning", "", journal.warning)
    _check("dropped bytes", 0, journal.bytes_dropped)
    _check("unknown records", expect_unknown, journal.unknown_records)
    _check("bytes read", journal.size, journal.bytes_read)
    read_back = {"player": journal.players(), "input": [], "event": journal.events(),
                 "tick": journal.tick_records(), "checkpoint": journal.checkpoints(),
                 "note": list(journal.notes), "correction": journal.corrections()}
    for group in journal.ticks:
        read_back["input"].extend(group.inputs)
    seen = dict((kind, 0) for kind in read_back)
    for kind, record in plan:
        index = seen[kind]
        seen[kind] += 1
        if index >= len(read_back[kind]):
            raise AssertionError("%s record %d missing from the file" % (kind, index))
        _check("%s record %d" % (kind, index), record, read_back[kind][index])
    for kind, records in read_back.items():
        if seen[kind] != len(records):
            raise AssertionError("%s: wrote %d records, read %d" % (kind, seen[kind], len(records)))
    return journal


def _expect_stop(directory, name, tag, payload, needle):
    """A record that breaks a limit must end the read, not the process."""
    path = os.path.join(directory, "selftest_%s.bmjr" % name)
    header = _synthetic_server()[0]
    writer = JournalWriter(path, header)
    writer.note({"tick": 1, "text": "the record before the bad one"})
    writer.raw(tag, payload)
    writer.note({"tick": 3, "text": "past the bad record, must not be read"})
    writer.close()
    journal = read_journal(path)
    _check("%s: notes before the bad record" % name, 1, len(journal.notes))
    if journal.bytes_dropped <= 0 or needle not in journal.warning:
        raise AssertionError("%s: expected a stop mentioning %r, got %r (dropped %d)"
                             % (name, needle, journal.warning, journal.bytes_dropped))
    os.remove(path)


def _expect_survives(directory, name, tag, payload, needle):
    """A record journal.hpp tolerates must not cost us the rest of the file."""
    path = os.path.join(directory, "selftest_%s.bmjr" % name)
    header = _synthetic_server()[0]
    writer = JournalWriter(path, header)
    writer.note({"tick": 1, "text": "the record before the odd one"})
    writer.raw(tag, payload)
    writer.note({"tick": 3, "text": "past the odd record, must still be read"})
    writer.close()
    journal = read_journal(path)
    _check("%s: notes on both sides of the odd record" % name, 2, len(journal.notes))
    _check("%s: dropped bytes" % name, 0, journal.bytes_dropped)
    if needle not in journal.warning:
        raise AssertionError("%s: expected a warning mentioning %r, got %r"
                             % (name, needle, journal.warning))
    os.remove(path)
    return journal


def _limit_checks(directory):
    payload = _U32.pack(2) + _U8.pack(CHECKPOINT_FULL) + _U32.pack(100000)
    _expect_stop(directory, "limit_bodies", TAG_CHECKPOINT, payload, "over the")
    payload = _U32.pack(2) + _U16.pack(5000) + b"x" * 5000
    _expect_stop(directory, "limit_string", TAG_NOTE, payload, "over the")
    payload = _U32.pack(2) + _U32.pack(1) + _U8.pack(0)
    _expect_stop(directory, "limit_short", TAG_PLAYER, payload, "short by")
    # An orphan repeat is a LOST record, not a broken file: journal.hpp keeps
    # the zeroed frame and reads on, and so must we, or the two readers
    # disagree about the contents of exactly the damaged box we exist for.
    payload = _U32.pack(2) + _U32.pack(99) + _U8.pack(1) + _U8.pack(INPUT_FRESH)
    journal = _expect_survives(directory, "orphan_repeat", TAG_INPUT, payload, "no previous frame")
    orphans = [record for group in journal.ticks for record in group.inputs if record.get("orphan")]
    _check("orphan repeat count", 1, len(orphans))
    _check("orphan repeat frame", _ZERO_FRAME, orphans[0]["frame"])
    _check("orphan repeat formatting", True, _fmt_input(orphans[0]).endswith("(rep?)"))


def _event_tick_checks(directory):
    """Both halves of the trailing event_tick: a new file and an old one."""
    path = os.path.join(directory, "selftest_event_tick.bmjr")
    header, _ = _synthetic_server()
    late = {"tick": 16, "event_tick": 6, "id": 7, "type": 4, "ball_type": 2, "flags": 0,
            "position": (f32(1.0), f32(2.0), f32(3.0)), "rotation": tuple(f32(0.0) for _ in range(9)),
            "sector": 1, "name": "P_Box_MF002", "recipe": empty_recipe()}
    writer = JournalWriter(path, header)
    writer.event(late)
    # the same event as a pre-event_tick writer wrote it: no trailing u32
    old = dict(late, tick=20, event_tick=20)
    writer.raw(TAG_EVENT, JournalWriter._event_payload(old)[:-4])
    writer.close()
    journal = read_journal(path)
    _check("event_tick: warning", "", journal.warning)
    _check("event_tick: dropped bytes", 0, journal.bytes_dropped)
    events = journal.events()
    _check("event_tick: records", 2, len(events))
    _check("event_tick: the late event", late, events[0])
    # an old payload has no stamp of its own, so it reads back as its own tick
    _check("event_tick: the old-style event", old, events[1])
    if "@6" not in _fmt_event(events[0]):
        raise AssertionError("a late event must show its stamped tick: %s" % _fmt_event(events[0]))
    if "@" in _fmt_event(events[1]):
        raise AssertionError("an event applied at its own tick must not show a stamp: %s"
                             % _fmt_event(events[1]))
    os.remove(path)


def _diff_checks(directory):
    """--diff's exit code follows whichever half of the pair is comparable."""
    paths = []

    def write(name, header, plan):
        path = os.path.join(directory, "selftest_diff_%s.bmjr" % name)
        _write_plan(path, header, plan)
        paths.append(path)
        return path

    def bend(plan, tick, value):
        """The same plan with one TICK record's world hash changed."""
        return [(kind, dict(record, hash=value) if kind == "tick" and record["tick"] == tick else record)
                for kind, record in plan]

    def diff(*files):
        journals = [read_journal(path) for path in files]
        label_journals(journals)
        out, err = io.StringIO(), io.StringIO()
        saved = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = out, err
        try:
            code = run_diff(journals)
        finally:
            sys.stdout, sys.stderr = saved
        return code, out.getvalue() + err.getvalue()

    server_header, server_plan = _synthetic_server()
    client_header, client_plan = _synthetic_client()
    server = write("server", server_header, server_plan)
    server_twin = write("server_twin", server_header, server_plan)
    server_bent = write("server_bent", server_header, bend(server_plan, 102, 0xBADBAD))
    client = write("client", client_header, client_plan)
    client_twin = write("client_twin", client_header, client_plan)
    client_bent = write("client_bent", client_header, bend(client_plan, 102, 0xBADBAD))
    crashed = write("crashed", server_header, [("note", {"tick": 100, "text": "boot failed: no level"})])

    _check("diff: two equal server journals", 0, diff(server, server_twin)[0])
    _check("diff: two server journals that disagree", 3, diff(server, server_bent)[0])
    # Two clients of one room have no authoritative half, but their hashes are
    # the whole comparison: that pair is how a desync between two players is
    # triaged, so it must answer 0/3 and never "nothing to compare".
    _check("diff: two equal client journals", 0, diff(client, client_twin)[0])
    _check("diff: two client journals that disagree", 3, diff(client, client_bent)[0])
    # A server and a client only share the server's snapshots; the two hashes
    # are of different worlds, so they are printed but stay out of the code.
    code, text = diff(server, client)
    _check("diff: a server and a client journal", 0, code)
    if "first_divergence=101" not in text:
        raise AssertionError("the hash lines must still report where the two differ:\n%s" % text)
    # nothing in common is a tooling error, not a green CI run
    _check("diff: against a journal that never ticked", 1, diff(server, crashed)[0])
    for path in paths:
        os.remove(path)


def _log_checks(directory, journal):
    """--log: the three stamp forms, and where the yearless one gets its year."""
    path = os.path.join(directory, "selftest_log.log")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            # BMLPlus (retail mod), ISO, then BMMO's own console format
            "[09/04/2026 22:47:06.611] [BallanceMMOClient/INFO]: mismatch at tick 101 (local 103)\n"
            "[2026-09-04 22:47:07] rollback to tick 101 (2 ticks)\n"
            "[12-31 23:59:58] [Sim] [session 42] world: tick 102 hash=0\n"
            # the server erasing its console input line before it prints
            "\x1b[0K[12-31 23:59:59] input edge at tick 102\n"
            "[01-01 00:00:01] resync requested (too far) at tick 103\n"
            "no stamp on this one: tick base 104 assigned\n")
    log = parse_log(path, 2026)
    _check("log: lines", 6, log.lines)
    _check("log: lines with a tick", 6, log.matched)
    _check("log: ticks", [101, 102, 103, 104], sorted(log.by_tick))
    _check("log: kinds", ["mismatch", "rollback", "tick", "input", "resync", "tickbase"],
           [entry["kind"] for tick in sorted(log.by_tick) for entry in log.by_tick[tick]])
    _check("log: the local tick of a mismatch", 103, log.by_tick[101][0]["local"])
    _check("log: the source prefix is stripped", "mismatch at tick 101 (local 103)",
           log.by_tick[101][0]["text"])
    _check("log: BMLPlus stamp", datetime.datetime(2026, 9, 4, 22, 47, 6, 611000),
           log.by_tick[101][0]["time"])
    _check("log: ISO stamp", datetime.datetime(2026, 9, 4, 22, 47, 7), log.by_tick[101][1]["time"])
    _check("log: BMMO console stamp", datetime.datetime(2026, 12, 31, 23, 59, 58),
           log.by_tick[102][0]["time"])
    _check("log: BMMO console stamp behind an erase sequence",
           datetime.datetime(2026, 12, 31, 23, 59, 59), log.by_tick[102][1]["time"])
    # the month went backwards, so the log crossed New Year
    _check("log: BMMO console stamp after New Year", datetime.datetime(2027, 1, 1, 0, 0, 1),
           log.by_tick[103][0]["time"])
    _check("log: a line without a stamp", None, log.by_tick[104][0]["time"])
    _check("log: first stamp", datetime.datetime(2026, 9, 4, 22, 47, 6, 611000), log.first_time)
    _check("log: last stamp", datetime.datetime(2027, 1, 1, 0, 0, 1), log.last_time)
    _check("log: clock column", "23:59:58.000", _fmt_clock(log.by_tick[102][0]["time"]))
    # the year comes from the journal (its utc_ms read as local time), and
    # from today's date when no journal was given
    _check("log: the year of a journal", datetime.datetime.fromtimestamp(1757000000.123).year,
           _log_year([journal]))
    _check("log: the year without a journal", datetime.datetime.now().year, _log_year([]))
    _check("log: an undated log falls back to this year", datetime.datetime.now().year,
           parse_log(path).by_tick[102][0]["time"].year)
    os.remove(path)


def run_selftest(out_dir):
    keep = out_dir is not None
    directory = out_dir or tempfile.mkdtemp(prefix="bmjr_selftest_")
    if keep and not os.path.isdir(directory):
        os.makedirs(directory)
    server_path = os.path.join(directory, "selftest_server.bmjr")
    client_path = os.path.join(directory, "selftest_client.bmjr")
    truncated_path = os.path.join(directory, "selftest_truncated.bmjr")
    try:
        server_header, server_plan = _synthetic_server()
        client_header, client_plan = _synthetic_client()
        writer = _write_plan(server_path, server_header, server_plan, unknown_after=3)
        _write_plan(client_path, client_header, client_plan)
        server = _verify(server_path, server_header, server_plan, 1)
        client = _verify(client_path, client_header, client_plan, 0)
        print("selftest: server journal %d bytes, %d records, %d tick groups"
              % (server.size, server.records, len(server.ticks)))
        print("selftest: client journal %d bytes, %d records, %d tick groups"
              % (client.size, client.records, len(client.ticks)))

        # the repeat compression really happened, and the reader unpacked it
        repeats = [record for group in server.ticks for record in group.inputs if record["repeat"]]
        if len(repeats) != 1:
            raise AssertionError("expected exactly one repeated input, got %d" % len(repeats))
        first = server.by_tick[101].inputs[0]
        if repeats[0]["frame"] != first["frame"]:
            raise AssertionError("a repeated input did not reproduce the previous frame")
        members = [record["name"] for kind, record in server_plan
                   if kind == "player" and record["added"] and record["tick"] == server_header["first_tick"]]
        _check("initial players", members, [record["name"] for record in server.initial_players])
        if all(name.isascii() for name in members):
            raise AssertionError("the selftest lost its non-ASCII member name")
        _check("writer bytes", server.size, writer.bytes)

        # summary mode is what --list reads; it drops the INPUT and TICK
        # payloads, so it must still agree on everything the listing prints
        for path, full in ((server_path, server), (client_path, client)):
            brief = read_journal(path, summary=True)
            what = os.path.basename(path)
            _check("summary %s: tick range" % what, full.tick_range(), brief.tick_range())
            _check("summary %s: groups" % what, len(full.ticks), brief.group_count())
            _check("summary %s: records" % what, full.records, brief.records)
            _check("summary %s: tick records" % what, full.tick_record_count, brief.tick_record_count)
            _check("summary %s: inputs" % what, full.input_count, brief.input_count)
            _check("summary %s: events" % what, [record["tick"] for record in full.events()],
                   [record["tick"] for record in brief.events()])
            _check("summary %s: notes" % what, [record["text"] for record in full.notes],
                   [record["text"] for record in brief.notes])
            _check("summary %s: checkpoints" % what,
                   [(record["tick"], record["flags"]) for record in full.checkpoints()],
                   [(record["tick"], record["flags"]) for record in brief.checkpoints()])
        print("selftest: summary mode (--list) agrees with the full read on every listed counter")

        # a truncated tail keeps everything before the cut and reports the loss
        with open(server_path, "rb") as handle:
            data = handle.read()
        with open(truncated_path, "wb") as handle:
            handle.write(data[:-7])
        cut = read_journal(truncated_path)
        _check("truncated header", server_header, cut.header)
        _check("truncated record count", server.records - 1, cut.records)
        if cut.bytes_dropped <= 0 or not cut.warning:
            raise AssertionError("a truncated file must report dropped bytes and a warning")
        print("selftest: truncated copy kept %d/%d records, dropped %d bytes (%s)"
              % (cut.records, server.records, cut.bytes_dropped, cut.warning))

        # a bad magic and a bad version are the only fatal cases
        for broken, why in ((b"BMMOXXXX" + data[8:], "magic"), (MAGIC + _U32.pack(99) + data[12:], "version")):
            path = os.path.join(directory, "selftest_broken.bmjr")
            with open(path, "wb") as handle:
                handle.write(broken)
            try:
                read_journal(path)
            except JournalError:
                pass
            else:
                raise AssertionError("a bad %s should have been fatal" % why)
            os.remove(path)

        _limit_checks(directory)
        print("selftest: limits (bodies, string length, short payload) stop the read;"
              " an orphan repeat is tolerated like journal.hpp does")
        _event_tick_checks(directory)
        print("selftest: event_tick round trips, and an old EVENT payload without it reads as its own tick")
        _diff_checks(directory)
        print("selftest: --diff exits 3 on a divergence and 1 only when the comparable half is empty")
        _log_checks(directory, server)
        print("selftest: --log reads BMLPlus, ISO and BMMO console stamps;"
              " the yearless ones are dated from the journal and roll over at New Year")
        print("selftest: OK")
        if keep:
            print("selftest: kept %s, %s, %s" % (server_path, client_path, truncated_path))
        return 0
    finally:
        if not keep:
            # A failed check leaves its file behind; rmdir must not be the thing
            # that hides the real assertion.
            for name in os.listdir(directory):
                try:
                    os.remove(os.path.join(directory, name))
                except OSError:
                    pass
            try:
                os.rmdir(directory)
            except OSError:
                pass


# --------------------------------------------------------------------------


def use_utf8_output():
    """Never let a name or a note kill the report.

    Journal text is free-form UTF-8 (room names, player names, /mmo journal
    marks), but a redirected stdout on Windows encodes with the ANSI code page
    (cp936 here, cp1252 on a Western box), so one Chinese room name or one
    emoji raises UnicodeEncodeError halfway through and the operator is left
    with a partial, misleading report.
    """
    for name in ("stdout", "stderr"):
        stream = getattr(sys, name, None)
        if stream is None:
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
            continue
        except (AttributeError, ValueError, OSError):
            pass
        buffer = getattr(stream, "buffer", None)
        if buffer is None:
            continue
        try:
            setattr(sys, name, io.TextIOWrapper(buffer, encoding="utf-8", errors="replace",
                                                line_buffering=True))
        except (AttributeError, ValueError, OSError):
            pass


def label_journals(journals):
    counts = {}
    for journal in journals:
        base = journal.default_label()
        counts[base] = counts.get(base, 0) + 1
    seen = {}
    for journal in journals:
        base = journal.default_label()
        if counts[base] == 1:
            journal.label = base
            continue
        seen[base] = seen.get(base, 0) + 1
        journal.label = "%s #%d" % (base, seen[base])


def main():
    use_utf8_output()
    parser = argparse.ArgumentParser(prog="journal_trace.py", add_help=True,
                                     description="Read BMMO session journals and merge them with client logs.")
    parser.add_argument("files", nargs="*", metavar="FILE.bmjr")
    parser.add_argument("--log", action="append", default=[], metavar="ModLoader.log",
                        help="a client or server log to merge into the timeline (repeatable)")
    parser.add_argument("--around", type=int, metavar="TICK", help="only the ticks around TICK")
    parser.add_argument("--window", type=int, default=40, metavar="N", help="half width of --around (default 40)")
    parser.add_argument("--list", action="store_true", dest="list_only",
                        help="header, members, events, notes, corrections, checkpoints; no timeline")
    parser.add_argument("--diff", action="store_true", help="compare the first two journals tick by tick")
    parser.add_argument("--nonempty", action="store_true",
                        help="skip ticks that carry nothing but a TICK record")
    parser.add_argument("--selftest", action="store_true",
                        help="write a synthetic journal with every tag through this script's writer and read it back")
    parser.add_argument("--selftest-out", metavar="DIR", help="keep the --selftest journals in DIR")
    args = parser.parse_args()

    if args.selftest or args.selftest_out:
        return run_selftest(args.selftest_out)
    if not args.files:
        parser.error("need at least one .bmjr file (or --selftest)")

    # --list alone never looks at an INPUT or a checkpoint body, so it reads in
    # summary mode; --diff and the timeline need the whole thing.
    summary = args.list_only and not args.diff
    journals = []
    for path in args.files:
        try:
            journals.append(read_journal(path, summary))
        except (JournalError, OSError) as err:
            print("journal_trace.py: %s: %s" % (path, err), file=sys.stderr)
            return 1
    label_journals(journals)
    logs = []
    # BMMO's console format has no year in its stamps; the journals do.
    year = _log_year(journals)
    for path in args.log:
        try:
            logs.append(parse_log(path, year))
        except OSError as err:
            print("journal_trace.py: %s: %s" % (path, err), file=sys.stderr)
            return 1
    for journal in journals:
        if journal.warning:
            print("journal_trace.py: %s: %s" % (journal.path, journal.warning), file=sys.stderr)

    print_wall_clock(journals, logs)
    if args.list_only:
        for journal in journals:
            print_list(journal)
        for log in logs:
            print_log_summary(log)
        if not args.diff:
            return 0
    if args.diff:
        return run_diff(journals)
    print_timeline(journals, logs, args.around, args.window, args.nonempty)
    return 0


if __name__ == "__main__":
    sys.exit(main())
