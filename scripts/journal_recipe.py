#!/usr/bin/env python3
"""A session's inputs, from its journals, as a recipe (docs/canned-tests-plan.md, step T2).

A recipe holds only what the players did - each player's frame of every tick (the game keys, the camera rows,
the nav-active flag), when each joined and left, the level and the map - and nothing a build computes, so a later
client and server still play it (the journal is read here, once; every later build reads the recipe).

    journal_recipe.py SERVER_JOURNAL [--client CLIENT_JOURNAL ...] [--map MAP_FILE] -o RECIPE_DIR

RECIPE_DIR gets recipe.json (the manifest) and player<N>.frames (N: the player's index in the manifest).

A tick the server substituted (the player's own frame came late or never): the player's client journal's own FRESH
frame when one is given (--client), else the frame the server applied (the user's decision 1, 2026-11-05).

The manifest (format "bmmo-input-recipe", version 1; fields are only ever added):
    level, map {file, md5} (null: the level's own map), session_first_tick (the source's; every tick in the recipe
    counts from it), session_ticks, players [{name, join_order, lives [{join_tick, leave_tick}], frames, ticks}],
    source {...} (where it came from: informational, nothing reads it to play)
The frame stream: "BMRF", u32 version 1, then records of
    u32 tick (from the session's first tick), u32 count (the frame holds for count ticks), u8 keys,
    u8 nav_active, f32 cam_right[3], f32 cam_up[3], f32 cam_dir[3]
little-endian, ticks increasing; a tick no record covers is a tick the player was not in the session.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import journal_trace as jt  # noqa: E402

FORMAT = "bmmo-input-recipe"
VERSION = 1
FRAMES_MAGIC = b"BMRF"
FRAMES_VERSION = 1
INPUT_FRESH = 1
INPUT_FLAG_NAV_ACTIVE = 1 << 2
_RECORD = struct.Struct("<IIBB9f")


def frame_key(frame):
    """What a recipe keeps of a frame: the keys, nav-active, the camera rows (bit for bit)."""
    return (frame["keys"], 1 if frame["flags"] & INPUT_FLAG_NAV_ACTIVE else 0,
            tuple(frame["cam_right"]) + tuple(frame["cam_up"]) + tuple(frame["cam_dir"]))


def expand(journal, player):
    """tick -> (frame, fresh) of `player`'s INPUT records, repeats expanded (the reader already did)."""
    out = {}
    for group in journal.ticks:
        for record in group.inputs:
            if record["id"] != player:
                continue
            fresh = bool(record["flags"] & INPUT_FRESH)
            prev = out.get(record["tick"])
            # A client file's later SUBSTITUTED record of the same tick wins the world's frame; its own FRESH
            # record is what the player pressed - keep the FRESH one (it is what a recipe replays).
            if prev is not None and prev[1] and not fresh:
                continue
            out[record["tick"]] = (record["frame"], fresh)
    return out


def lives_of(journal, player, last_tick):
    """[(join_tick, leave_tick or None)] from the PLAYER records (the initial adds included)."""
    events = [(r["tick"], r["added"]) for r in journal.initial_players if r["id"] == player]
    for group in journal.ticks:
        for r in group.players:
            if r["id"] == player and r not in journal.initial_players:
                events.append((r["tick"], r["added"]))
    lives, open_at = [], None
    for tick, added in sorted(events, key=lambda e: e[0]):
        if added and open_at is None:
            open_at = tick
        elif not added and open_at is not None:
            lives.append((open_at, tick))
            open_at = None
    if open_at is not None:
        lives.append((open_at, None))
    return lives


def write_frames(path, frames, first_tick):
    """frames: sorted [(tick, key)] -> the run-length stream; returns the record count."""
    buf = bytearray(FRAMES_MAGIC + struct.pack("<I", FRAMES_VERSION))
    records = 0
    i = 0
    while i < len(frames):
        tick, key = frames[i]
        count = 1
        while i + count < len(frames) and frames[i + count][0] == tick + count and frames[i + count][1] == key:
            count += 1
        keys, nav, cam = key
        buf += _RECORD.pack(tick - first_tick, count, keys, nav, *cam)
        records += 1
        i += count
    path.write_bytes(bytes(buf))
    return records


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("server_journal")
    ap.add_argument("--client", action="append", default=[], help="a client journal of the same session")
    ap.add_argument("--map", default="", help="the custom map the session ran (its file name and md5 go in)")
    ap.add_argument("-o", "--out", required=True, help="the recipe directory (must not exist)")
    args = ap.parse_args()

    server = jt.read_journal(args.server_journal)
    if not server.is_server:
        ap.error("%s is not a server journal" % args.server_journal)
    header = server.header
    first, last = header["first_tick"], server.ticks[-1].tick if server.ticks else header["first_tick"]
    clients = {}
    for path in args.client:
        c = jt.read_journal(path)
        if c.is_server or c.header["session"] != header["session"]:
            ap.error("%s is not a client journal of session %d" % (path, header["session"]))
        clients[c.header["own_player"]] = c

    ids = []
    for r in list(server.initial_players) + [r for g in server.ticks for r in g.players]:
        if r["added"] and r["id"] not in [i for i, _, _ in ids]:
            ids.append((r["id"], r["join_order"], r["name"]))
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    players = []
    for index, (pid, join_order, name) in enumerate(sorted(ids, key=lambda e: e[1])):
        applied = expand(server, pid)
        own = expand(clients[pid], pid) if pid in clients else {}
        frames, substituted, from_client = [], 0, 0
        for tick in sorted(applied):
            frame, fresh = applied[tick]
            if not fresh:
                substituted += 1
                mine = own.get(tick)
                if mine is not None and mine[1]:
                    frame = mine[0]
                    from_client += 1
            frames.append((tick, frame_key(frame)))
        # The player's own frames the server never applied (its client stepped on past the server's removal,
        # or ahead of the server's last tick): inputs too, from its client journal.
        beyond = [t for t in sorted(own) if own[t][1] and t not in applied and t >= first]
        frames = sorted(frames + [(t, frame_key(own[t][0])) for t in beyond])
        records = write_frames(out / ("player%d.frames" % index), frames, first)
        players.append({
            "name": name, "join_order": join_order,
            "lives": [{"join_tick": j - first, "leave_tick": (l - first) if l is not None else None}
                      for j, l in lives_of(server, pid, last)],
            "frames": "player%d.frames" % index, "ticks": len(frames),
            "source": {"id": pid, "substituted_ticks": substituted, "from_client_journal": from_client,
                       "beyond_the_server": len(beyond),
                       "applied_by_server": substituted - from_client, "records": records},
        })
    map_entry = None
    if args.map:
        data = pathlib.Path(args.map).read_bytes()
        map_entry = {"file": pathlib.Path(args.map).name, "md5": hashlib.md5(data).hexdigest()}
    manifest = {
        "format": FORMAT, "version": VERSION,
        "level": header["level"], "map": map_entry,
        "session_first_tick": first, "session_ticks": last - first + 1,
        "players": players,
        "source": {"server_journal": pathlib.Path(args.server_journal).name,
                   "client_journals": [pathlib.Path(p).name for p in args.client],
                   "build_id": header["build_id"], "utc_ms": header["utc_ms"], "session": header["session"],
                   "input_delay": header["input_delay"]},
    }
    (out / "recipe.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for p in players:
        s = p["source"]
        print("%-12s join order %d, lives %s, %d ticks (%d records); substituted %d: %d from its client journal, %d "
              "as the server applied; %d own frames past the server's" % (
                  p["name"], p["join_order"], [(l["join_tick"], l["leave_tick"]) for l in p["lives"]], p["ticks"],
                  s["records"], s["substituted_ticks"], s["from_client_journal"], s["applied_by_server"],
                  s["beyond_the_server"]))
    print("recipe: %s (level %d, %d ticks from session tick %d)" % (out, header["level"], last - first + 1, first))
    return 0


if __name__ == "__main__":
    sys.exit(main())
