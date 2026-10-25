#!/usr/bin/env python3
"""Make extra draw bones of a modded character start hidden (Dissidia 012 .objx event scripts).

Symptom this fixes: a character GMO with extra drawn parts (the Forge "warp dash": four ghost copies of
the body, a hovering sword, twelve trail parts; or any effect/weapon part added past the retail bone list)
shows those parts permanently. A body with ghost copies (bright white-blue palette, drawn over the real body
from the same pose) looks flat pale blue, as if untextured.

Why (measured in game, 17 Oct): the battle code keeps a per-character visibility table of 3-byte entries
{draw bone, state, arg}, one slot per Part of the model (0xFF = free), at ctrl+0x10c with the slot count at
ctrl+0x8e. A draw bone that is not in the table is drawn unconditionally. At the start of a battle
FUN_08833534 calls hide(bone) (FUN_088372D0 -> FUN_0886CB64) for the 18 bytes of the current attack record
(atkp, bytes +12..+29 of each 68-byte record). Retail fills 16 of them (64..71, 73..80 = wep0..wep15, wep17,
wep18; wep16 is not listed, so the cape variant it holds is always drawn) and leaves two 0xFF, so only two
more bones could be added there. Event 0x011B (show, arg = draw bone) / 0x011C (hide) go through the same
function and register a bone in the first free slot, which is why the weapons appear and vanish with
their motion's events but a bone nobody ever hid stays visible.

Fix: give every motion timeline a ranged 0x011C (hide) event for each extra draw bone covering the
whole motion. The first timeline played registers the bone as hidden; a bone's own 0x011B events still
show it for their range. Slots are not a limit: a model has one slot per Part, retail uses 16 and the
extra bones come with their own Parts.

usage: objx_hide_bones.py IN.objx OUT.objx --gmo MODEL.gmo   [--skip-motions 16-19]
       objx_hide_bones.py IN.objx OUT.objx --bones 118-134   [--skip-motions 16-19]

--gmo picks every bone of the mesh model that has a DrawPart and is not the body part (part 0) and
not named wep<N> (those are already hidden by the game). Timelines that already
carry 0x011B/0x011C events for these bones (the dash motions) are left alone automatically, since the table
persists across motions; --skip-motions adds more.
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gmo_arrays_check as g

HIDE_OP = 0x011C
ALIGN = {14: 64, 2: 16, 3: 16}


# ---------------------------------------------------------------- ARC container (see dfx.objx)
def arc_read(d):
    if d[:4] != b'ARC\x01':
        raise ValueError('not an objx (ARC) file')
    n = struct.unpack_from('<I', d, 4)[0]
    ents = []
    for i in range(n):
        t, tag, o, s = struct.unpack_from('<I4sII', d, 16 + i * 16)
        ents.append([t, tag, d[o:o + s]])
    return d[8:16], ents


def arc_write(extra, ents):
    table, body, o = bytearray(), bytearray(), 16 + len(ents) * 16
    for t, tag, data in ents:
        pad = (-o) % ALIGN.get(t, 4)
        body += b'\0' * pad
        o += pad
        table += struct.pack('<I4sII', t, tag, o, len(data))
        body += data
        o += len(data)
    return b'ARC\x01' + struct.pack('<I', len(ents)) + extra + bytes(table) + bytes(body)


# ---------------------------------------------------------------- type 8: per-motion event scripts
# header u16[8]: motion, (keyed << 8 | ranged + 1), offset of the keyed events, size. Then a 4..20 byte
# preamble, the ranged events {u16 start, u16 end, u16 op, u16 pad, u32 x, u32 args[op >> 8]}, then the keyed ones.
def events_split(d):
    ver, n = struct.unpack_from('<II', d, 0)
    offs = list(struct.unpack_from('<%dI' % n, d, 8)) + [len(d)]
    return ver, [d[offs[i]:offs[i + 1]] for i in range(n)]


def events_join(ver, blobs):
    o, offs = 8 + 4 * len(blobs), []
    for b in blobs:
        offs.append(o)
        o += len(b)
    return struct.pack('<II', ver, len(blobs)) + struct.pack('<%dI' % len(blobs), *offs) + b''.join(blobs)


def ranged_events(blob):
    """(preamble end offset, [(op, first arg or None)]) of the ranged events of one timeline; None when it
    does not decode."""
    h = struct.unpack_from('<8H', blob, 0)
    n_r, off_k = (h[1] & 0xFF) - 1, h[2]
    for pre in range(4, off_k - 16 + 1, 4):
        p, good, evs = 16 + pre, True, []
        for _ in range(max(n_r, 0)):
            if p + 6 > off_k:
                good = False
                break
            op = struct.unpack_from('<H', blob, p + 4)[0]
            n = op >> 8
            evs.append((op, struct.unpack_from('<I', blob, p + 12)[0] if n else None))
            p += 12 + 4 * n
        if good and p == off_k:
            return p, evs
    return None


def add_ranged(blob, events):
    """Append ranged events (raw 16-byte records) to one timeline; None when the timeline does not decode."""
    r = ranged_events(blob)
    if r is None:
        return None
    p, extra = r[0], b''.join(events)
    h = list(struct.unpack_from('<8H', blob, 0))
    h[1] += len(events)
    h[2] += len(extra)
    h[3] += len(extra)
    return struct.pack('<8H', *h) + blob[16:p] + extra + blob[p:]


def hide_event(bone):
    return struct.pack('<HHHHII', 0, 0xFFFF, HIDE_OP, 0, 0, bone)


def patch_objx(data, bones, skip=(), keep_own=True):
    extra, ents = arc_read(data)
    ev = next((e for e in ents if e[0] == 8), None)
    if ev is None:
        raise ValueError('objx has no type 8 (event scripts) entry')
    ver, blobs = events_split(ev[2])
    out, done, bad, owners = [], 0, [], []
    for b in blobs:
        motion = struct.unpack_from('<H', b, 0)[0] if len(b) >= 16 else None
        new = None
        r = ranged_events(b) if len(b) >= 16 else None
        own = keep_own and r is not None and any(op in (0x011B, HIDE_OP) and a in bones for op, a in r[1])
        if own:
            owners.append(motion)
        elif motion not in skip and len(b) >= 16:
            new = add_ranged(b, [hide_event(x) for x in bones])
            if new is None:
                bad.append(motion)
        out.append(new if new is not None else b)
        done += new is not None
    ev[2] = events_join(ver, out)
    return arc_write(extra, ents), done, bad, owners


# ---------------------------------------------------------------- GMO: which bones are extra draw bones
def extra_draw_bones(gmo):
    """Bone indices of the mesh model (the Model block that has Parts) with a DrawPart that are neither
    part 0 (the body) nor named wep<N>."""
    models, cur = [], None
    for o, t, hs, sz, dep in g.walk(gmo, 16, len(gmo)):
        if t == 3 and dep == 1:
            cur = {'bones': [], 'parts': 0}
            models.append(cur)
        elif cur is not None and t == 5 and dep == 2:
            cur['parts'] += 1
        elif cur is not None and t == 4 and dep == 2:
            name = gmo[o + 16:o + hs].split(b'\0')[0].decode('latin1')
            cur['bones'].append([name, None])
        elif cur is not None and t == 0x804E and dep == 3 and cur['bones']:
            cur['bones'][-1][1] = struct.unpack_from('<I', gmo, o + 8)[0] & 0xFFF
    mesh = next((m for m in models if m['parts']), None)
    if mesh is None:
        raise ValueError('no mesh model in the GMO')
    out = []
    for i, (name, part) in enumerate(mesh['bones']):
        if part is None or part == 0xFFF or part == 0:
            continue
        low = name.lower()
        if low.startswith('wep') and low[3:].isdigit():
            continue
        out.append(i)
    return out


def parse_ranges(s):
    out = []
    for piece in s.split(','):
        a, _, b = piece.partition('-')
        out += range(int(a), int(b or a) + 1)
    return out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    opt = {argv[i]: argv[i + 1] for i in range(1, len(argv) - 1) if argv[i].startswith('--')}
    if len(args) < 2 or not ('--gmo' in opt or '--bones' in opt):
        print(__doc__)
        return 2
    src, dst = args[0], args[1]
    if '--bones' in opt:
        bones = parse_ranges(opt['--bones'])
    else:
        bones = extra_draw_bones(open(opt['--gmo'], 'rb').read())
    skip = set(parse_ranges(opt['--skip-motions'])) if '--skip-motions' in opt else set()
    if not bones:
        print('no extra draw bones: nothing to do')
        return 0
    data, done, bad, owners = patch_objx(open(src, 'rb').read(), bones, skip)
    open(dst, 'wb').write(data)
    print('%d bone(s) %s hidden in %d timeline(s); %s%s' % (
        len(bones), '%d..%d' % (bones[0], bones[-1]) if len(bones) > 1 else bones[0], done,
        'undecodable timelines left alone: %s' % bad if bad else 'all other timelines patched',
        '; timelines that show/hide these bones themselves left alone: %s' % sorted(owners) if owners else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
