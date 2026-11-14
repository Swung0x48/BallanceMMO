#!/usr/bin/env python3
"""The facts of a session from its journals (step JNL): logs are for people,
the analyses read the journal.

    session_facts.py JOURNAL [JOURNAL ...]            what each journal records
    session_facts.py JOURNAL --lines client|server    the lines a log reader read, from the journal
    session_facts.py JOURNAL [...] --gate             the gate's counters (status, rewinds, resyncs)

A journal written from step JNL on carries a SCHEMA and typed records
(journal_stats.hpp): STAT for logical kinds, CLOCK for clock and link kinds.
Every fact is marked by where it came from:

  recorded      a typed record of the kind
  derived       from the records every journal has (PLAYER, INPUT, EVENT,
                NOTE, TICK, the HEADER) - what a pre-step journal can say
  not recorded  the kind is not in the file's SCHEMA (or there is none)
  truncated     the file says it stopped at its size cap

client_lines / server_lines render the records as the text lines the log
readers (scripts/arena_map_test.py, late_player_harness.parse_s1) parse, so
those parsers read a journal unchanged; the rendering is the log line's own
format for each kind, and work/JNL's proofs compare the two.
Python 3.8+, journal_trace.py beside it.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import journal_trace as jt  # noqa: E402

_U32 = struct.Struct("<I")


class Facts(object):
    """One journal walked once, in file order: the header, the SCHEMA's kinds,
    the typed records and the notes, interleaved as written."""

    def __init__(self, path):
        self.path = str(path)
        self.header = None
        self.schema = None          # {id: kind}; None: a pre-step journal
        self.items = []             # ('stat', record) / ('note', tick, text) / ('event', tick, payload) in file order
        self.truncated = False
        self.ticks = 0
        self.first_tick = self.last_tick = None
        self.last_ms = None
        for tag, payload in jt.records(self.path):
            if tag == jt.TAG_HEADER:
                self.header = jt.JournalReader._header(payload)
            elif tag == jt.TAG_SCHEMA:
                self.schema = jt.parse_schema(payload)
            elif tag in (jt.TAG_STAT, jt.TAG_CLOCK):
                self.items.append(('stat', jt.parse_stat(payload, self.schema or {}, tag == jt.TAG_CLOCK)))
            elif tag == jt.TAG_NOTE:
                cursor = jt._Cursor(payload)
                tick, text = cursor.u32(), cursor.string()
                self.items.append(('note', tick, text))
                if text.startswith('cap: '):
                    self.truncated = True
            elif tag == jt.TAG_EVENT:
                self.items.append(('event', jt.JournalReader._event(jt._Cursor(payload))))
            elif tag == jt.TAG_TICK:
                tick = _U32.unpack_from(payload, 0)[0]
                self.ticks += 1
                self.first_tick = tick if self.first_tick is None else self.first_tick
                self.last_tick = tick
                self.last_ms = _U32.unpack_from(payload, 24)[0]

    def recorded(self, kind):
        return self.schema is not None and any(k['name'] == kind for k in self.schema.values())

    def state(self, kind):
        if self.truncated:
            return 'truncated'
        return 'recorded' if self.recorded(kind) else 'not recorded'

    def stats(self, kind):
        return [item[1] for item in self.items if item[0] == 'stat' and item[1]['name'] == kind]

    def notes(self):
        return [(item[1], item[2]) for item in self.items if item[0] == 'note']

    def events(self):
        return [item[1] for item in self.items if item[0] == 'event']


# --------------------------------------------------------------------------
# The client's lines (client_core_report.cpp window_text / status_text, the notes' log lines)

WINDOW_FORMAT = ("window: tick={tick} inrew={inrew}/{inrew_depth} life={life}/{life_depth} forced={forced}/{forced_depth} "
                 "snaprb={snaprb}/{snaprb_depth} snaps={snaps} resyncs={resyncs_sent}/{resyncs_done} rebases={rebases} "
                 "relay={relay} late={late} covered={covered}"
                 " subst={subst_recv}/{subst_diff}/{subst_same}/{subst_bodiless}/{substrew}/{subst_late}"
                 " substrew={substrew}/{substrew_depth}"
                 " substco={subst_deferred}/{subst_unseen}/{subst_subsumed}/{subst_saved}"
                 " lead_adj={lead_adj} lead={lead} slew={slew}"
                 " moved={moved} echo={echo_acked}/{echo_moved}/{echo_superseded}/{echo_rejected}/{echo_missing}"
                 " ownlife={own_moved}/{own_removed}/{own_resynced} superseded={superseded}/{replaced}")


def window_line(values):
    text = WINDOW_FORMAT.format(**values)
    for tail in ('lag', 'inrew_src', 'sector_margin'):
        if values.get(tail):
            text += ' ' + values[tail]
    return text


_HOLD_HELD = re.compile(r'^hold-event: held (\S+) stamped (\d+)(?: for (\d+) ms| with its pair)$')
_HOLD_DROPPED = re.compile(r'^hold-event: dropped (\S+) stamped (\d+)$')
_HOLD_RELEASED = re.compile(r'^hold-event: released stamped (\d+)$')
_ASSIGNED = re.compile(r'^assigned: tick base (\d+) \((-?\d+) frames since the anchor\)$')
_RESYNC_BASE = re.compile(r'^resync: tick base (\d+) \(old tick (\d+), delta (-?\d+)\): anchored at (\d+)$')
_OWN_REWOUND = re.compile(r'^own (?:birth|beam) moved (\d+) -> (\d+|never): rewound to (\d+)$')
_OWN_RESYNC = re.compile(r'^(own (?:birth|beam) moved \S+ -> \S+(?: \(.*\))?): (.+), resync$')


def client_lines(facts):
    """The lines arena_map_test.read_client_log and status_metrics read, from a client journal."""
    h = facts.header
    lines = ['own id %d' % h['own_player'],
             'session %d starting: input delay %d' % (h['session'], h['input_delay'])]
    held = {}   # stamp -> [(type, held at)] in the order held (a pair holds two of one stamp)
    for item in facts.items:
        if item[0] == 'stat':
            r = item[1]
            v = r['values']
            if r['name'] == 'window':
                lines.append(window_line(v))
            elif r['name'] == 'status_clock':
                lines.append(v['text'])
            elif r['name'] == 'own_death':
                lines.append(v['line'])
            elif r['name'] == 'stall':
                lines.append('stall: %d ms at tick %d (--stall-ms)' % (v['ms'], v['tick']))
            continue
        if item[0] != 'note':
            continue
        tick, text = item[1], item[2]
        m = _ASSIGNED.match(text)
        if m:
            lines.append('tick base %s assigned (%s frames since anchor)' % (m[1], m[2]))
            continue
        m = _RESYNC_BASE.match(text)
        if m:
            lines.append("resynced: tick base %s (old tick %s, delta %+d): anchored at %s, the server's state of it follows"
                         % (m[1], m[2], int(m[3]), m[4]))
            continue
        if text.startswith("resync: re-stepped from the server's tick "):
            lines.append(text)
            continue
        m = _HOLD_HELD.match(text)
        if m:
            held.setdefault(int(m[2]), []).append((m[1], tick))
            lines.append('hold-event: own %s stamped tick %s held at tick %d' % (m[1], m[2], tick))
            continue
        m = _HOLD_DROPPED.match(text)
        if m:
            lines.append('hold-event: own %s stamped tick %s dropped at tick %d' % (m[1], m[2], tick))
            continue
        m = _HOLD_RELEASED.match(text)
        if m:
            kind, at = (held.get(int(m[1])) or [('?', 0)]).pop(0)
            lines.append('hold-event: own %s stamped tick %s (held at tick %d) released at tick %d' % (kind, m[1], at, tick))
            continue
        m = _OWN_REWOUND.match(text)
        if m:
            moved = 'never made' if m[2] == 'never' else 'moved to ' + m[2]
            lines.append('own physicalize stamped %s %s by the server at local tick %d: rewound to %s' % (m[1], moved, tick, m[3]))
            continue
        m = _OWN_RESYNC.match(text)
        if m:
            lines.append('%s at local tick %d: %s, resync' % (m[1], tick, m[2]))
            continue
    return lines


def rewinds(facts):
    """Each rewind: what was asked (the rewind record: anchor, at, cause, body) with the base its restore was and
    the ticks it re-simulated (the rewind_base record after it: base, depth; raw, step LVR's wall-clock gate)."""
    asked, done = facts.stats('rewind'), facts.stats('rewind_base')
    if len(asked) != len(done):
        raise ValueError('%s: %d rewind records but %d rewind_base records' % (facts.path, len(asked), len(done)))
    return [dict(a['values'], base=d['values']['base'], depth=d['values']['depth']) for a, d in zip(asked, done)]


# --------------------------------------------------------------------------
# The server's lines (the [Sim] S1 lines, the resync and S5 lines, the notes)

EVENT_TYPE_NAMES = jt.EVENT_TYPES


def server_lines(facts):
    """The lines arena_map_test.event_report / airship_spans / lead_mode and parse_s1 read, from a server journal."""
    h = facts.header
    sid = h['session']
    lines = []
    for r in facts.stats('server_config'):
        v = r['values']
        lines.append('Physics sessions enabled (patience %s, lead adjust %s).'
                     % ('on' if v['patience'] else 'off', 'on' if v['lead_adjust'] else 'off'))
    for item in facts.items:
        if item[0] == 'stat':
            r = item[1]
            v = r['values']
            if r['name'] in ('s1', 's1_quorum'):
                lines.append('[session %d] S1 %s' % (sid, v['text']))
            elif r['name'] == 'ghost':
                lines.append("world: spawn ghost: player %d's ball ? is %s at tick %d"
                             % (v['player'], 'solid' if v['solid'] else 'a spawn ghost', v['tick']))
            elif r['name'] == 'resync_request' and v['why'] != 'join':
                lines.append('Physics session %d: p%d resynced at tick %d (last full snapshot it had: %d; held counted '
                             'through tick %d).' % (sid, v['player'], v['base'], v['last_full'], v['counted_through']))
            elif r['name'] == 's5':
                late = v['applied'] - v['origin']
                lines.append('Physics session %d: S5 event player=%d type=%s origin=%d applied=%d late=%d delay_us=%d '
                             'delay_ticks=%.2f to=%d%s' % (sid, v['player'], v['type'], v['origin'], v['applied'], late,
                                                           v['delay_us'], v['delay_us'] / 15000.0, v['to'],
                                                           ' ' + v['flag'] if v['flag'] else ''))
        elif item[0] == 'note':
            tick, text = item[1], item[2]
            if text.startswith('S4 bodiless '):
                lines.append('[session %d] %s' % (sid, text))
            m = re.match(r'^airship: p(\d+) in the airship zone at tick (\d+)', text)
            if m:
                lines.append("world: airship: player %s's ball ? is in the airship zone at tick %s" % (m[1], m[2]))
        elif item[0] == 'event':
            e = item[1]
            origin = e.get('origin_tick', e['event_tick'])
            if origin < e['tick']:
                lines.append('[session %d] event type %d from %d stamped %d applied at tick %d (late by %d), re-stamped'
                             % (sid, e['type'], e['id'], origin, e['tick'], e['tick'] - origin))
    return lines


# --------------------------------------------------------------------------
# The gate's counters

def gate(facts):
    """What a gate reads at a session's end: the status counters, the rewinds, the resyncs."""
    out = {'journal': os.path.basename(facts.path), 'status': facts.state('status')}
    status = facts.stats('status')
    if status:
        out.update({k: status[-1]['values'][k] for k in ('mism', 'rb', 'resyncs_sent', 'resyncs_done',
                                                         'scr_mismatches', 'scr_float', 'sdiff_queued',
                                                         'sdiff_classified', 'sdiff_forced', 'sdiff_unclassified')})
        out['resim'] = facts.stats('status_clock')[-1]['values']['resim']
        rw = rewinds(facts)
        out['rewinds'] = len(rw)
        out['rewound_ticks'] = sum(r['depth'] for r in rw)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('journals', nargs='+')
    parser.add_argument('--lines', choices=('client', 'server'))
    parser.add_argument('--gate', action='store_true')
    args = parser.parse_args(argv)
    for path in args.journals:
        facts = Facts(path)
        if args.lines:
            for line in (client_lines if args.lines == 'client' else server_lines)(facts):
                print(line)
        elif args.gate:
            print(' '.join('%s=%s' % kv for kv in gate(facts).items()))
        else:
            kinds = sorted(k['name'] for k in (facts.schema or {}).values())
            counts = {}
            for item in facts.items:
                if item[0] == 'stat':
                    counts[item[1]['name']] = counts.get(item[1]['name'], 0) + 1
            print('%s: %s, ticks %d, typed kinds %s' % (path, 'truncated' if facts.truncated else 'whole', facts.ticks,
                                                         ', '.join('%s=%d' % (k, counts.get(k, 0)) for k in kinds)
                                                         or 'none (written before step JNL)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
