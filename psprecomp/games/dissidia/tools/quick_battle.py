#!/usr/bin/env python3
"""Take the native build from boot to the Quick Battle (1-on-1) character select: run.sh <workdir> --quick-battle.

It loads the save in the work folder as a player would and steers the menus through the runtime's debug socket
(B = buttons, S = screenshot, FF = fast-forward; on the port the game's log names, so a port another game holds is
never steered: then it stops); at Player Select the game is the player's again. Every press waits for the screen it is
meant for (seen twice in a row), so a press the game drops (a fade) is pressed again instead of landing on the wrong
item:
  title         CROSS until the save list opens (the cursor starts on LOAD GAME when there is a save)
  save list     its confirm button, once, then the load              (log: list dialog opened / mode=4 / complete)
  main menu     Story Mode highlighted: DOWN to Battle Mode           (screenshot: the highlight bar's row)
  Battle Mode   confirm: its list opens on Quick Battle (1-on-1)      (screenshot)
  Quick Battle  confirm: Player Select                                (screenshot: its tab and header)
The trip runs fast-forwarded at up to 4x, boot logos included (the runtime's FF command, leased for 15 s at a time and
paused for each press, which would repeat). Capped, not as fast as frames render: uncapped, the title went on to its
attract movie (over two minutes, which FF cannot shorten: a movie's picture follows its real-time sound) before a
press every 1.5 s got through. At Player Select the game is back at its own speed. A runtime without FF makes the same
trip at normal speed. Standard library only (the Steam Deck runs it with the system python3). run.sh starts it next to
the game; by hand, right after starting the game:
  quick_battle.py --log <workdir>/last_run.log
"""
import argparse
import os
import re
import shutil
import socket
import struct
import sys
import tempfile
import time

BUTTONS = {'cross': 0x4000, 'circle': 0x2000, 'up': 0x10, 'down': 0x40}
FAST_MS = 15000     # each FF lease; renewed every few seconds, so a driver that dies leaves the game at normal speed
FAST_X = 4          # FF speed cap (times real time): uncapped (~1500 frames/s), the title's idle timer ran out and
                    # started the attract movie before a press got through

# Places in the 480x272 screenshot (x0, x1, y). The menus' highlight bar is dark blue (about 120, 115, 205) on a near
# white list; Player Select's tab is blue as well and its header bar grey-blue (the title, save list and menus are light
# there).
STORY_ROW, BATTLE_ROW, QUICK_ROW = (330, 400, 65), (330, 400, 89), (360, 420, 107)
SELECT_TAB, SELECT_HEADER = ((35, 60, 39), (100, 125, 39)), (10, 30, 17)


class Game:
    def __init__(self, log, pid):
        self.port, self.log, self.pid = None, log, pid    # the port: from the log (wait_socket)
        self.pos, self.n = 0, 0
        self.ff, self.ff_at = None, 0.0                   # fast-forward: None = not tried, False = not in this runtime
        self.tmp = tempfile.mkdtemp(prefix='quick_battle_')

    def alive(self):
        if not self.pid:
            return True
        try:
            os.kill(self.pid, 0)
            return True
        except OSError:
            return False

    def cmd(self, line):
        with socket.create_connection(('127.0.0.1', self.port), timeout=12) as s:
            s.sendall(line.encode() + b'\n')
            reply = s.makefile('rb').readline().decode(errors='replace').strip()
        if not reply.startswith('OK'):
            raise IOError('debug socket: %r -> %r' % (line, reply))
        return reply

    def fast(self, on=True):
        """the runtime's fast-forward (FF <ms> <x>): fades, animations and loading run up to x times as fast"""
        if self.ff is False:
            return
        try:
            self.cmd('FF %d %d' % (FAST_MS, FAST_X) if on else 'FF 0')
            self.ff, self.ff_at = on or self.ff, time.time()
        except IOError:                                   # an older runtime: the trip runs at normal speed
            self.ff = False

    def press(self, button):
        if self.ff:                                       # a held button spans dozens of frames when fast (and the
            self.fast(False)                              # menus repeat it): the press runs at the game's own speed
        self.cmd('B %X 120' % BUTTONS[button])
        if self.ff:
            time.sleep(0.25)
            self.fast()

    def frame(self):
        """the frame on screen: (width, height, bytes per pixel, top row first, pixels); S writes a 32-bit TGA"""
        self.n += 1
        p = os.path.join(self.tmp, 'f%d.tga' % (self.n % 4))
        self.cmd('S ' + p)
        with open(p, 'rb') as f:
            d = f.read()
        w, h, bpp, desc = struct.unpack('<HHBB', d[12:18])
        return w, h, bpp // 8, bool(desc & 0x20), d[18 + d[0]:]

    def new_lines(self):
        """log lines written since the last call (whole lines only)"""
        try:
            with open(self.log, 'rb') as f:
                if os.fstat(f.fileno()).st_size < self.pos:   # started anew (read before the game started)
                    self.pos = 0
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return []
        cut = data.rfind(b'\n') + 1
        self.pos += cut
        return data[:cut].decode(errors='replace').splitlines()


def strip(img, x0, x1, y):
    """mean (r, g, b) of a row of pixels"""
    w, h, n, top, px = img
    row = y if top else h - 1 - y
    r = g = b = 0
    for x in range(x0, x1):
        i = (row * w + x) * n
        b, g, r = b + px[i], g + px[i + 1], r + px[i + 2]
    k = x1 - x0
    return r // k, g // k, b // k


def blue(c):
    return c[0] < 170 and c[2] - c[0] > 45


def screen(g):
    """'story' / 'battle' (main menu, the highlighted row), 'quick' (Battle Mode's list on Quick Battle 1-on-1),
    'select' (Player Select) or None (anything else: a fade, a loading screen)"""
    img = g.frame()
    if all(blue(strip(img, *p)) for p in SELECT_TAB) and strip(img, *SELECT_HEADER)[0] < 190:
        return 'select'
    s, b, q = (blue(strip(img, *p)) for p in (STORY_ROW, BATTLE_ROW, QUICK_ROW))
    if s and not b:
        return 'story'
    if b and not s and not q:
        return 'battle'
    if q and not s and not b:
        return 'quick'
    return None


def stop(g, why):
    sys.exit('[quick battle] %s; take it from here by hand' % why)


def check(g):
    if not g.alive():
        sys.exit('[quick battle] the game closed')
    if g.ff and time.time() - g.ff_at > 5:                # renew the fast-forward lease
        g.fast()


def wait_socket(g):
    """this game's debug socket, on the port its log names. A port another game holds (one already running) fails
    to bind here: pressing there would steer that game"""
    end = time.time() + 60
    while time.time() < end:
        check(g)
        for l in g.new_lines():
            m = re.search(r'(Debug socket listening|Failed to bind debug socket) on port (\d+)', l)
            if m and m.group(1).startswith('Failed'):
                stop(g, 'port %s is taken (another game running?): close that one, or start this one with '
                        'PSPRECOMP_DEBUG_PORT=<another port>' % m.group(2))
            if m:
                g.port = int(m.group(2))
                return
        time.sleep(0.25)
    stop(g, 'no debug socket in the log')


def to_save_list(g):
    """CROSS on the title until the save list opens -> its confirm button; or None when the save loaded at once
    (PSPRECOMP_SAVEDATA_AUTO)"""
    end, last = time.time() + 150, 0.0
    while time.time() < end:
        check(g)
        for l in g.new_lines():
            m = re.search(r'list dialog opened.*\b(CROSS|CIRCLE) confirms', l)
            if m:
                return m.group(1).lower()
            if 'sceUtilitySavedata complete: mode=4' in l:
                return None
        if time.time() - last >= 1.5:                     # the title (or a logo / movie it skips) takes it
            g.press('cross')
            last = time.time()
        time.sleep(0.25)
    stop(g, 'the save list did not open')


def load_save(g, confirm):
    """confirm on the save list, once; again only if the load has not started after 3 s"""
    end, last, started = time.time() + 40, 0.0, False
    while time.time() < end:
        check(g)
        for l in g.new_lines():
            started = started or 'sceUtilitySavedata mode=4 ' in l
            m = re.search(r'sceUtilitySavedata complete: mode=4 result=(0x[0-9A-Fa-f]+)', l)
            if m:
                if int(m.group(1), 16):
                    stop(g, 'the save did not load (%s)' % m.group(1))
                return
        if confirm and not started and time.time() - last >= 3.0:
            g.press(confirm)
            last = time.time()
        time.sleep(0.25)
    stop(g, 'the save did not load')


def menu_step(g, before, button, after, timeout=20, again=1.0):
    """on screen `before`, press `button` until the screen is `after`. A screen counts when it is seen twice in a row
    (one frame of a fade can look like anything), and `after` only once this step has pressed (the save list's last
    frames read as Battle Mode: its highlighted entry covers that row). A press the game dropped is pressed again;
    nothing is pressed on any other screen (a fade, or not where it should be)."""
    end, last, prev, seen = time.time() + timeout, 0.0, None, None
    while time.time() < end:
        check(g)
        prev, seen = seen, screen(g)
        if last and seen == after and prev == after:
            return
        if seen == before and prev == before and time.time() - last >= again:
            g.press(button)
            last = time.time()
        time.sleep(0.15)
    stop(g, 'expected %s, the screen shows %s' % (after, seen or 'something else'))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--log', required=True, help='the game\'s log (run.sh: <workdir>/last_run.log or --log)')
    ap.add_argument('--pid', type=int, default=0, help='the game\'s process: stop when it closes')
    a = ap.parse_args(argv)
    g = Game(a.log, a.pid)
    t0 = time.time()
    try:
        wait_socket(g)
        g.fast()
        confirm = to_save_list(g)
        t1 = time.time()
        load_save(g, confirm)
        t2 = time.time()
        confirm = confirm or 'cross'
        menu_step(g, 'story', 'down', 'battle')
        menu_step(g, 'battle', confirm, 'quick')
        menu_step(g, 'quick', confirm, 'select')
        g.fast(False)
        t3 = time.time()
        print('[quick battle] Player Select, %.0f s after start (title %.1f s, save %.1f s, menus %.1f s%s)' % (
            t3 - t0, t1 - t0, t2 - t1, t3 - t2, ', fast-forwarded up to %dx' % FAST_X if g.ff else ''), file=sys.stderr)
        return 0
    finally:
        if g.ff and g.alive():                            # never leave the game fast-forwarding
            g.fast(False)
        shutil.rmtree(g.tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
