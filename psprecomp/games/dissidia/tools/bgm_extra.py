#!/usr/bin/env python3
"""Add extra BGM tracks to Dissidia 012 (data only, no hooks; see ../docs/audio.md).

    bgm_extra.py <DATA dir> <mods dir> [--check]

Reads <mods>/bgm.txt, one track per line (# comments):
    XV1.AT3  vol=63  FFXV - Veiled in Black
    FILE     8.3 upper-case name of a file in DATA/BGM (the native host path is case sensitive)
    vol=N    playback volume in percent (retail battle tracks: effective loudness ~ -14.3 LUFS)
    cat=N    battle category, 0 = not offered in battle (default 1); flags=N picker flags (default 1)
Appends a BENT row per track to bgm_entry.bin in general_archive/main/main.bin (<mods>/by_crc/93307c1c.bin) and
its name to bgm_name.bin in every general_archive/main/<lang>/main_lang.bin. Starts from the files already in
<mods> (the roster build's output) or the retail ones, and is idempotent: tracks already present are skipped.
roster_mainbin.py calls install() after it rewrites those archives, so a roster rebuild keeps the tracks.
--check: rebuild retail with no tracks and verify byte identity.

Table (FUN_0889EE60 copies it into room for 256 entries; FUN_0889F234 opens DISC0:/PSP_GAME/USRDIR/DATA/BGM/<file>):
  +0 s16 bgm id   +2 s16 name index   +4 4 x s16 unlock flags (-1 = none)   +0xC u8 flags (bit 0/1 picker, bit 2 DLC)
  +0xD s8 volume %   +0xE s8 battle category   +0x10 u64 random-pool mask (0 = never random)   +0x1C char[16] file
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mess_text as mt  # noqa: E402
import package_extract as pe  # noqa: E402

MAIN = "general_archive/main/main.bin"
LANG = "general_archive/main/%s/main_lang.bin"
LANGS = ("en", "fr", "de", "it", "es", "jp")
ENTRY_SIZE, BGM_CAP, LANG_MAX = 44, 256, 0x80000


def mpk():
    import roster_mainbin as rm      # late: roster_mainbin imports this module
    return rm.mpk_unpack, rm.mpk_pack


def read_list(path):
    tracks = []
    for ln in open(path, encoding="utf-8"):
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        parts = ln.split()
        t = dict(file=parts[0].upper(), vol=70, cat=1, flags=1)
        rest = []
        for p in parts[1:]:
            k, eq, v = p.partition("=")
            if eq and k in ("vol", "cat", "flags") and not rest:
                t[k] = int(v)
            else:
                rest.append(p)
        t["name"] = " ".join(rest) or t["file"]
        assert len(t["file"]) < 16 and 0 <= t["vol"] <= 127, ln
        tracks.append(t)
    return tracks


def append_names(e, texts):
    """Append records to a plain mess_text entry, keeping every retail byte. bgm_name.bin has "no text" records
    (start word 0) that mess_text.build() would turn into strings, so the entry is not rebuilt: the start array grows
    by len(texts) words (non-zero starts shift by that much) and the new records go at the end. A record's XOR key
    depends only on its index, so the retail records stay valid."""
    e = bytes(e)
    assert e[:4] != mt.OFF4
    cnt = struct.unpack_from("<H", e, 0)[0]
    k = len(texts)
    starts = [s + k if s else 0 for s in struct.unpack_from("<%dH" % cnt, e, 2)]
    body = bytearray(e[2 + 2 * cnt:])
    assert len(body) % 2 == 0
    base = 1 + cnt + k                       # words
    for i, t in enumerate(texts):
        idx = cnt + i
        u = [ord(c) for c in t] + [0]
        assert all(c < 0x10000 for c in u)
        starts.append(base + len(body) // 2)
        body += struct.pack("<H", 2 * len(u)) + struct.pack("<%dH" % len(u), *[c ^ mt.KEY[(idx + j) % 16] for j, c in enumerate(u)])
    assert starts[-1] < 0x10000
    return struct.pack("<H", cnt + k) + struct.pack("<%dH" % len(starts), *starts) + bytes(body)


def bent_parse(b):
    assert b[:4] == b"BENT"
    n = struct.unpack_from("<I", b, 8)[0]
    assert len(b) == 16 + n * ENTRY_SIZE
    return b[:16], [b[16 + i * ENTRY_SIZE:16 + (i + 1) * ENTRY_SIZE] for i in range(n)]


def entry(bid, name_idx, t):
    return (struct.pack("<hh4hBbb", bid, name_idx, -1, -1, -1, -1, t["flags"], t["vol"], t["cat"])
            + bytes(28 - 15) + t["file"].encode("ascii").ljust(16, b"\0"))


def apply(main_bin, langs, tracks, log=print):
    """main_bin: bytes; langs: {lang: main_lang.bin bytes}. Returns (main_bin, {lang: bytes}) with tracks appended."""
    unpack, pack = mpk()
    mh, ments = unpack(main_bin)
    me = next(e for e in ments if e[0] == "bgm_entry.bin")
    head, ents = bent_parse(me[2])
    lang_parts = {}
    for g, b in langs.items():
        lh, lents = unpack(b)
        le = next(e for e in lents if e[0] == "bgm_name.bin")
        lang_parts[g] = (lh, lents, le, struct.unpack_from("<H", le[2], 0)[0], [], mt.records(le[2]))
    have = {e[28:].split(b"\0")[0].decode().upper() for e in ents}
    next_id = max(struct.unpack_from("<h", e, 0)[0] for e in ents) + 1
    n_names = max([p[3] for p in lang_parts.values()] + [max(struct.unpack_from("<h", e, 2)[0] for e in ents) + 1])
    added = 0
    for t in tracks:
        if t["file"] in have:
            log("bgm: %s already present" % t["file"])
            continue
        bid = next_id
        next_id += 1
        # a name left by an earlier run (main.bin rebuilt from retail, main_lang.bin kept) is reused, not added twice
        old = {g: [i for i, r in enumerate(p[5]) if r == t["name"]] for g, p in lang_parts.items()}
        common = set.intersection(*[set(v) for v in old.values()]) if old else set()
        if common:
            ni = min(common)
        else:
            ni = n_names
            n_names += 1
            for p in lang_parts.values():
                p[4].extend([""] * (ni - p[3] - len(p[4])) + [t["name"]])
        ents.append(entry(bid, ni, t))
        added += 1
        log('bgm: id %d name %d %s vol %d "%s"' % (bid, ni, t["file"], t["vol"], t["name"]))
    assert len(ents) <= BGM_CAP
    if not added:
        return main_bin, dict(langs)
    me[2] = head[:8] + struct.pack("<I", len(ents)) + head[12:] + b"".join(ents)
    out = {}
    for g, (lh, lents, le, _, names, _r) in lang_parts.items():
        le[2] = append_names(le[2], names) if names else le[2]
        out[g] = pack(lh, lents)
        if len(out[g]) > LANG_MAX:
            raise SystemExit("%s: %d B, the game reads at most %d B" % (LANG % g, len(out[g]), LANG_MAX))
    return pack(mh, ments), out


def install(pk, mods, log=print):
    """Apply <mods>/bgm.txt on top of the archives in <mods> (or retail). No-op without bgm.txt."""
    lst = os.path.join(mods, "bgm.txt")
    if not os.path.exists(lst):
        return 0
    tracks = read_list(lst)
    main_p = os.path.join(mods, "by_crc", "%08x.bin" % pe.package_crc(MAIN))
    main_b = open(main_p, "rb").read() if os.path.exists(main_p) else pk.read(MAIN)
    langs, paths = {}, {}
    for g in LANGS:
        p = os.path.join(mods, LANG % g)
        try:
            langs[g] = open(p, "rb").read() if os.path.exists(p) else pk.read(LANG % g)
        except KeyError:
            continue
        paths[g] = p
    bgm_dir = os.path.join(pk.dir, "BGM")
    for t in tracks:
        if not os.path.exists(os.path.join(bgm_dir, t["file"])):
            log("bgm: warning: %s is not in %s yet" % (t["file"], bgm_dir))
    m2, l2 = apply(main_b, langs, tracks, log)
    if m2 != main_b:
        os.makedirs(os.path.dirname(main_p), exist_ok=True)
        open(main_p, "wb").write(m2)
        log("wrote %s %d bytes" % (main_p, len(m2)))
    for g, b in l2.items():
        if b != langs[g]:
            os.makedirs(os.path.dirname(paths[g]), exist_ok=True)
            open(paths[g], "wb").write(b)
            log("wrote %s %d bytes" % (paths[g], len(b)))
    return 0


def main(argv):
    if len(argv) < 1 or (len(argv) < 2 and "--check" not in argv):
        print(__doc__)
        return 2
    pk = pe.Package(argv[0])
    if "--check" in argv:
        langs = {}
        for g in LANGS:
            try:
                langs[g] = pk.read(LANG % g)
            except KeyError:
                pass
        main_b = pk.read(MAIN)
        _, pack = mpk()
        unpack = mpk()[0]
        ok = pack(*unpack(main_b)) == main_b and all(pack(*unpack(b)) == b for b in langs.values())
        nm = [bytes(next(e for e in unpack(b)[1] if e[0] == "bgm_name.bin")[2]) for b in langs.values()]
        ok = ok and all(append_names(e, []) == e for e in nm)
        for e in nm:                         # appended records decode back, retail ones unchanged
            cnt = struct.unpack_from("<H", e, 0)[0]
            real = [i for i, s in enumerate(struct.unpack_from("<%dH" % cnt, e, 2)) if s]   # skip "no text" records
            r0, r1 = mt.records(e), mt.records(append_names(e, ["FFXV - Test", "Second"]))
            ok = ok and [r1[i] for i in real] == [r0[i] for i in real] and r1[cnt:] == ["FFXV - Test", "Second"]
        print("round trip (main.bin + %s):" % " ".join(langs), "identical" if ok else "DIFFERS")
        return 0 if ok else 1
    return install(pk, argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
