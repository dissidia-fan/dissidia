"""Own effect-package (FEP) ids for extra characters, so they can share a battle with the character they were built on.

The game registers one FEP per (id & 0xFFFC0000) (FUN_0894349C / FUN_08943124): a second package with an id that is
already registered is taken for the loaded one, and its fighter plays none of its own effects (trails, projectile
visuals, `effect` events, contact flashes). Local resource keys are 0x98000000 | (id >> 18 & 0xFF) << 10 | n, so two
packages with the same low byte of (id >> 18) clash on their textures / models as well.

A character copied from a retail one (charmod.py new, the Move Studio's New character) starts with the donor's packages.
give_own_ids() rebases every package of the extra characters that clashes with the disc, with the hand-made extras
below or with another extra in the same build:
  * new id = the old id with a new low byte (id >> 18 & 0xFF) that no retail package and no other package uses,
  * one new low byte per old low byte within a character, so the retail rule assist = main + 2048 (same low byte) and a
    summon's _a twin stay paired,
  * Fep.rebase() rewrites the id, every effect / leaf / name id and every local resource key (blender/io_scene_dissidia/
    dfx/fep.py); the objx size does not change. FEPA / FXSE records and the timelines refer to effects by record
    number, so nothing outside the package changes.
Characters are handled in slot order and low bytes are handed out in a fixed order (from 0xA2 up), so a build gives
the same ids every time while the roster stays the same. Retail ids are read from PACKAGE.BIN once and cached in the
mods dir. Notes: docs/archive or the project doc dissidia-fep-ids-and-cpu-ai / dissidia-forms-jobp."""
import json
import mmap
import os
import struct
import sys

import charmod_objx as CO

HERE = os.path.dirname(os.path.abspath(__file__))
DFX = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..', 'blender', 'io_scene_dissidia', 'dfx'))
CACHE = 'readme_charmod_fep_ids.json'     # in the mods dir ('readme*' is never served to the game)

# ids (id >> 18) kept free for characters that are not a mod folder. Empty: the hand-made extras that had them
# (forge/ffxv_tools/fep_rebase/rebase_extras.py, 2026-10-22) are gone (Vivi, Y'shtola) or a mod folder that carries its
# ids in its own packages (Noctis 3221 / 5269), which must not count as clashing with themselves.
RESERVED = set()
ASSIST = 2048                              # retail: assist package id = main id + 2048


def _fep():
    if DFX not in sys.path:
        sys.path.insert(0, DFX)
    import fep
    return fep


def retail_ids(disc_path, mods_dir=None):
    """{id >> 18} of every effect package on the disc (cached next to the build by PACKAGE.BIN size + mtime)."""
    pkg = os.path.join(disc_path, 'PSP_GAME', 'USRDIR', 'DATA', 'PACKAGE.BIN')
    if not os.path.exists(pkg):
        pkg = os.path.join(disc_path, 'PACKAGE.BIN')
    st = os.stat(pkg)
    key = '%d:%d' % (st.st_size, int(st.st_mtime))
    cp = os.path.join(mods_dir, CACHE) if mods_dir else None
    if cp and os.path.exists(cp):
        try:
            c = json.load(open(cp))
            if c.get('key') == key:
                return set(c['ids'])
        except (ValueError, KeyError):
            pass
    ids = set()
    with open(pkg, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        i = m.find(b'FEP\xcd')
        while i != -1:
            if struct.unpack_from('<HH', m, i + 4) == (1, 3):
                ids.add(struct.unpack_from('<I', m, i + 0x10)[0] >> 18)
            i = m.find(b'FEP\xcd', i + 4)
        m.close()
    if cp and os.path.isdir(mods_dir):
        with open(cp, 'w') as f:
            json.dump({'_info': 'effect package ids (id >> 18) on the disc, for charmod_fep.py', 'key': key,
                       'ids': sorted(ids)}, f)
    return ids


def package_id(objx):
    """id >> 18 of the objx's effect package, or None (no 'effe' entry / not an objx)."""
    if objx[:4] != b'ARC\x01':
        return None
    fep = _fep()
    for _, tag, b in CO.arc_read(objx)[1]:
        if tag == b'effe':
            m = fep.Mpk.from_bytes(b)
            try:
                return fep.Fep.from_bytes(m.get(0x40)).id_base >> 18
            except Exception:
                return None
    return None


def rebase(objx, new):
    """The objx with its effect package moved to id (new << 18). Same size."""
    fep = _fep()
    extra, ents = CO.arc_read(objx)
    out = []
    for t, tag, b in ents:
        if tag == b'effe':
            m = fep.Mpk.from_bytes(b)
            f = fep.Fep.from_bytes(m.get(0x40))
            old_key = f.local_id(0)
            f.rebase(new << 18)
            fb = f.to_bytes()
            left = sum(1 for o in range(0, len(fb) - 3, 4) if struct.unpack_from('<I', fb, o)[0] & ~0x3FF & 0xFFFFFFFF == old_key)
            if left:
                raise ValueError('rebase left %d old resource keys' % left)
            m.set(0x40, fb)
            b = m.to_bytes()
        out.append((t, tag, b))
    data = CO.arc_write(extra, out)
    if len(data) != len(objx):
        raise ValueError('rebase changed the objx size (%d -> %d)' % (len(objx), len(data)))
    return data


def installed_ids(mods_dir, skip=()):
    """{id >> 18} of the effect packages of the installed objx files (<mods>/obj/), except the game paths in skip:
    characters a build of some folders only leaves alone keep their ids."""
    ids, od = set(), os.path.join(mods_dir, 'obj')
    if os.path.isdir(od):
        for fn in sorted(os.listdir(od)):
            if fn.endswith('.objx') and 'obj/' + fn not in skip:
                with open(os.path.join(od, fn), 'rb') as f:
                    pid = package_id(f.read())
                if pid is not None:
                    ids.add(pid)
    return ids


def give_own_ids(chars, retail, log=print, also_taken=()):
    """chars: [(name, {game path: bytes})] of the extra characters in slot order; their objx files are replaced in place.
    also_taken: ids other packages in the game already have (installed_ids). -> {name: {game path: (old id, new id)}}"""
    retail_lows = {x & 0xFF for x in retail}
    other = set(also_taken) - set(retail)
    taken = set(retail) | RESERVED | other
    taken_lows = retail_lows | {x & 0xFF for x in RESERVED | other}
    found = []                                        # (name, files, path, id)
    for name, files in chars:
        for gp in sorted(files):
            if gp.endswith('.objx'):
                pid = package_id(files[gp])
                if pid is not None:
                    found.append((name, files, gp, pid))
    # a package that already has an id of its own (no retail / reserved low byte, nobody else with that low byte) keeps it
    by_low = {}
    for name, _, _, pid in found:
        by_low.setdefault(pid & 0xFF, set()).add((name, pid))
    kept = {pid for low, owners in by_low.items() if low not in taken_lows and len({n for n, _ in owners}) == 1
            for _, pid in owners}
    pids = [pid for _, _, _, pid in found]
    kept = {pid for pid in kept if pids.count(pid) == 1}
    taken |= kept
    taken_lows |= {pid & 0xFF for pid in kept}
    # from just above the range the hand-made extras proved in game (0x94-0xA1), then the rest
    free = [x for x in list(range(0xA2, 0x100)) + list(range(1, 0xA2)) if x not in taken_lows]
    done, lowmap = {}, {}
    for name, files, gp, pid in found:
        if pid in kept:
            continue
        k = (name, pid & 0xFF)
        if k not in lowmap:
            if not free:
                raise SystemExit('no free effect package ids left (%s %s)' % (name, gp))
            lowmap[k] = free.pop(0)
        new = (pid & ~0xFF) | lowmap[k]
        if new in taken:
            raise SystemExit('effect package id %d for %s %s is already used' % (new, name, gp))
        taken.add(new)
        files[gp] = rebase(files[gp], new)
        done.setdefault(name, {})[gp] = (pid, new)
    for name, d in done.items():
        log('  %s: own effect ids %s' % (name, ', '.join('%s %d -> %d' % (os.path.basename(g), o, n)
                                                          for g, (o, n) in sorted(d.items()))))
    return done
