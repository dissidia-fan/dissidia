#!/usr/bin/env python3
"""Log every fighter's script id as the game runs (native build, debug socket on 127.0.0.1:9999).

Which scripts a move really plays, in order and for how long: e.g. a press whose action runs one of the game's shared
scripts (Sephiroth's Hell's Gate, action 62 -> script 48) that are not in the character's motion_map.json.

  script_trace.py [--out trace.txt] [--port 9999] [--hz 120]
  1. start the game (games/dissidia/tools/run.sh), start a battle with the character;
  2. run this; it waits for the battle and then prints a line each time a fighter's script changes;
  3. do the move (a few times is fine); Ctrl-C stops and writes the log.

The fighters: the battle's actor list (head at [[0x089D4958]+0x14], next at +0x1114, FUN_08841F84); per actor the
current script id is the s16 at +0x180 (docs/archive/event-ops/D.md), the kind (0/1 = fighters) the byte at +0x542.
Scripts are shown with their motion when the character's motion_map.json is given (--moveset mods/<x>/moveset/p_<c>).
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

LIST_BASE = 0x089D4958
HEAD_OFF, NEXT_OFF, SCRIPT_OFF, KIND_OFF = 0x14, 0x1114, 0x180, 0x542


class Sock:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.f = self.s.makefile('rb')

    def read(self, addr, n):
        self.s.sendall(b'R %X %d\n' % (addr & 0x0FFFFFFF, n))
        head = self.f.readline().decode(errors='replace').strip()
        if not head.startswith('OK'):
            raise IOError('debug socket replied %r to R %X %d' % (head, addr, n))
        size = int(head.split()[1])
        data = self.f.read(size)
        if len(data) < n:
            raise IOError('debug socket: short read (%d of %d bytes); did the game close?' % (len(data), n))
        return data

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]


def fighters(sk):
    base = sk.u32(LIST_BASE)
    if not base:
        return []
    a, out, seen = sk.u32(base + HEAD_OFF), [], set()
    while a and a not in seen and len(out) < 32:
        seen.add(a)
        out.append(a)
        a = sk.u32(a + NEXT_OFF)
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default='script_trace.txt')
    ap.add_argument('--port', type=int, default=9999)
    ap.add_argument('--hz', type=float, default=120)
    ap.add_argument('--moveset', action='append', default=[],
                    help='a moveset folder (motion_map.json, actions.json): scripts are shown with their motion')
    a = ap.parse_args(argv)
    names = {}
    for d in a.moveset:
        try:
            for r in json.load(open(os.path.join(d, 'motion_map.json')))['rows']:
                names.setdefault(r['script'], 'motion %03d' % r['motion'])
        except OSError as e:
            print('no motion_map in %s: %s' % (d, e))
    try:
        sk = Sock(a.port)
    except OSError as e:
        sys.exit('cannot reach the game\'s debug socket on port %d (%s): is the native build running?' % (a.port, e))
    out = open(a.out, 'w', buffering=1)                 # line by line: the log is kept however this stops
    print('logging to %s' % os.path.abspath(a.out))
    n, last, start, waited = 0, {}, time.time(), False
    print('waiting for a battle...' if not fighters(sk) else 'battle found: do the move, Ctrl-C to stop')
    try:
        while True:
            t = time.time() - start
            fs = fighters(sk)
            if not fs:
                if not waited:
                    waited = True
                time.sleep(0.25)
                continue
            if waited:
                print('battle found: do the move, Ctrl-C to stop')
                waited = False
            for i, act in enumerate(fs):
                kind = sk.read(act + KIND_OFF, 1)[0]
                if kind > 1:
                    continue
                scr = struct.unpack('<h', sk.read(act + SCRIPT_OFF, 2))[0]
                if last.get(act, (None,))[0] != scr:
                    prev = last.get(act)
                    held = '' if prev is None else ' (after %d ticks of %d)' % (round((t - prev[1]) * 60), prev[0])
                    line = '%8.3fs  fighter %d @%08X  script %5d  %s%s' % (t, i, act, scr, names.get(scr, ''), held)
                    print(line)
                    out.write(line + '\n')
                    n += 1
                    last[act] = (scr, t)
            time.sleep(1.0 / a.hz)
    except KeyboardInterrupt:
        pass
    except Exception as e:                              # the game closed, or something unexpected: keep what we have
        import traceback
        traceback.print_exc()
        out.write('# stopped: %r\n' % e)
    out.close()
    print('%d changes -> %s' % (n, os.path.abspath(a.out)))


if __name__ == '__main__':
    main(sys.argv[1:])
