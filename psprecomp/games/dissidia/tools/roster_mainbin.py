#!/usr/bin/env python3
"""Build <mods>/by_crc/93307c1c.bin: DATA/general_archive/main/main.bin with extra characters.

    roster_mainbin.py <disc0 or DATA dir> <mods dir> [--roster roster.txt]
                      [--char 34] [--like 9] [--code NOC100] [--max 43] [--first 34] [--objs new:src,...] [--check]
                      [--names extra_abilities.json | --names none]

Characters come from `<mods>/roster.txt` (the file the runtime reads: lines `<charId> <code> [like] ...`,
every charId >= 34 is built) or, without a roster file, from --char/--like/--code for one character.
--max is ROSTER_MAX (default 43, the same constant as roster.cpp's kRosterMax): charIds must be < max.
--check only rebuilds the archive without changes and verifies that the result is byte identical.

main.bin is an MPK archive (13 entries). Two of them matter for a new character:

  system.bin  -> ARC archive -> 'chrp': 0x90-byte character records, sorted by charId (byte 0), found by
                 a bsearch on that byte. Its +0x74 field is the 8-byte object/voice code ('P_FIV100'). The
                 voice loader resolves that code in the voice index, so an unknown code makes the battle
                 load wait forever after the model is read.
  objentry.bin-> 1170 records of 0x44 bytes sorted by their u16 id (charId at byte 6, costume at byte 7, file
                 type at byte 8, name at 0x24). ORG800 (charId 34) owns two (1067 and 1068).

For every new charId the tool
  * replaces (charId 34, where the unfinished ORG800 has a record) or inserts (charId > 34, keeping the table
    sorted) the chrp record with a clone of <like>'s, with the code at +0x74 set to P_<CODE> when --code
    (or a roster code) is given,
  * rewrites the existing objentry records of that charId (ORG800: two) in place with clones of <like>'s
    1P / 1P_A records, keeping their ids, and appends clones of the rest of <like>'s P_<LIKECODE>_* records
    (2P, 3P, ZAKO, W, A, PUB variants) with fresh ids above the current maximum, so the table stays sorted.
  * names in the cloned records become P_<CODE>_* (case preserved), so the game asks for
    obj/p_<code>_1p.gmo and friends, which the mods dir serves (alias copies of <like>'s files via
    tools/package_extract.py alias until the mod ships real ones).
  * gives the new charId its own abilities (system.bin 'ablt', 'chrl', 'amas'), see "Abilities" below.

Abilities. An ability id is (owner charId << 8) | n (0x40nn: shared support abilities, owner byte 0xFF). A save
record's ability sets store only the low byte n and compose the id with the set's owner byte (set +0xB6, FUN_088B7A88),
and the Abilities screen (menu overlay FUN_08AD6698) lists only the 'ablt' records whose owner byte (+0x1B) is the
character's charId (or 0xFF) and that its learned list holds. A charId without records of its own therefore shows no
attacks at all and can never leave the screen ("no HP attacks enabled"). For every new charId the tool
  * 'ablt' (16-byte header {'ABI ', version, u32 count, 0}, 40-byte records sorted by the u16 id at +0, found by a
    bsearch, FUN_088B1D9C): drops the records the charId already owns (charId 33's four 0x21xx, ORG800's 0x2200) and
    inserts a copy of every <like> record under id (charId << 8) | n with owner byte (+0x1B) charId and the ability
    references at +6 / +8 (prerequisite, base ability) moved along. Everything else (command id at +12, CP, AP, type
    +0x16, the name / help text ids) stays <like>'s.
    The loader (FUN_088B1CC0) allocates size + (808 - count) * 40 bytes, so a table of more than 808 records
    overflows that heap block unless the runtime's ablt hook (roster.cpp, dissidia_wpnp_install) is installed.
  * 'chrl' (abilities learned by level: header {'CLEV', version, u32 rows, 0}, rows {u32 charId, u32 count, u32 offset,
    0} sorted by charId, entries {u8 level, u8 flags, u16 id x3}) and 'amas' (abilities unlocked by mastering one:
    same index, entries {u16 id, u16, u16 id, u16 id}): adds a row for the charId with <like>'s entries, <like>'s ability
    ids moved to the new owner. Both loaders copy the table into a heap block of the table's own size, so they may grow.
  The save record of an extra character is a copy of <like>'s that the runtime (roster.cpp saverec_hook) moves to the
  new owner the same way (record byte 0, ability block +0x762 and set +0xB6, learned ids).

Ability names (games/dissidia/extra_abilities.json, --names PATH, --names none to skip; table in docs/roster.md). An
'ablt' record names its ability with two u16 text ids: +2 = a record of the name table (general_archive/main/<lang>/
main_lang.bin, an MPK the boot loader FUN_0893354C reads once into a 512 KiB buffer; member 'ability.bin', a mess_text
string table, 1307 retail records) and +4 = a record of entry 0 of text/<lang>/mess_pk_customize_ability_0.bin (the
help panel of the Abilities screen, 1221 retail records; entry 1 is an 'OFF4' table of move-detail texts that 'ablt'
does not reference). The copies above keep the like's ids, so a new character shows the like's names. For each roster
code the name map lists, the tool appends the new names and help texts to both tables (same ids in every language the
package has; this package has only 'en') and points that character's records at them; retail records keep their ids
and bytes, and a map entry whose 'cmd' differs from the record's command id is skipped. An entry may also change the
record's kind: "type" (brave / hp, +0x16), "phase" (ground / midair / follow-up, +0x17) and "after" (+6, the ability
it follows up, null = none), and its cost: "cp" (CP to equip, +0x0E; "cp_mastered" +0x10, default half), "ap" (AP to
master, +0x12) and "bonus" (the equip bonus, +0x14: a system.bin 'fncs' effect-set id, null = none); "learn": N on an
ability the like has moves it to level N of the 'chrl' row (set_learn_levels). An entry with "disabled": true takes the ability out of the kit but keeps its record
(owner byte 0xFE, command -2: not listed, does nothing in battle; see disable_abilities). An entry with "copy" adds an ability the like does not have: a copy of the character's
record with that low byte, under the entry's own low byte, with the entry's "cmd" as its command id; "learn": N lists it
in the character's 'chrl' row at level N (add_new_abilities; battle finds the move only through the record's +12, looked
up in the character's own objx comb). Both tables are copied into heap blocks of their own size (names: FUN_0887AC70 from
FUN_088B1CC0; help pack: FUN_0887B260, file size), so they may grow; main_lang.bin must stay under 0x80000 bytes. A table whose records no longer start below 64 Ki words (the u16
starts of the plain mess_text layout; the help texts get there first) is written as an OFF4 entry, which the game's
text code reads as well (mess_text.py). main() writes general_archive/main/<lang>/main_lang.bin and
text/<lang>/mess_pk_customize_ability_0.bin into the mods dir next to by_crc/93307c1c.bin.

Extra objects (`objs=new:src,...`): a roster line (or --objs on the command line) may declare additional
objects that are copies of retail ones under a new name, e.g. `35 ysh100 objs=w_ysh101:w_ten111,w_ysh102:w_ten112`.
For every `new:src` pair
  * every objentry record whose file stem (the lower case name at +0x34) is `src` (W_TEN111, W_TEN111_ZAKO, and the _A
    variants when they exist) is cloned with a fresh id above the maximum and the names `NEW`/`new`,
  * battle.bin -> 'wpnp' (the table of summoned / thrown weapon objects, 0x3c byte rows after an 8 byte header
    {u32 version?, u32 count}; row +0 = its own index, +4 u16 charId, +8 u16 objentry id, +0x2a u16 ZAKO id,
    +0x2c/+0x2e u16 A / ZAKO_A ids, +0x1c u32 script id) gets a copy appended of every row that spawns one of the
    declared objects (Yuna's aeons: 61 of the 84 retail rows), numbered from the old count, with the object ids
    replaced and the charId of the roster line (or --char). Rows are found by their global index: a body objx
    points at them with arg 1 of its ranged 0x075A / 0x0C65 events (Yuna: rows 5-9, 32-71, 83; Bartz: 77-82).
    `repoint_objx` (--repoint IN.objx:OUT.objx) rewrites those args to the copies, which is what makes a character
    use its own objects; args naming rows that were not copied keep the shared retail row.
    The game copies the whole table into a buffer that holds the retail 84 rows only (FUN_08850AB8 -> 0x09B16930,
    no bound check); a bigger table needs the runtime's relocation hook (roster.cpp, dissidia_wpnp_install).
    plya +0x60..+0x63 are NOT row lists (characters without summons use the same small values): leave them alone.

Both archive layers are rebuilt from their parsed entries (offsets, sizes and the record counts are
recomputed), so the tables may grow. The layout rules were verified by a byte identical round trip of the
unmodified archive (--check): MPK data entries start on 16 byte boundaries after the name table, ARC entries
are contiguous with 4 byte alignment, padding is zero. See games/dissidia/docs/roster.md for what else an extra id
needs. Standard library only.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import menu_pack as mp  # noqa: E402
import mess_text as mt  # noqa: E402
import package_extract as pe  # noqa: E402

MAIN_CRC = 0x93307C1C
CHRP_REC = 0x90
OBJ_REC = 0x44
ROSTER_MAX = 43          # charId slots 0..42 (35 retail slots + 8); keep in sync with roster.cpp kRosterMax
FIRST_NEW = 34


# ---------------------------------------------------------------------------------------------
# archive layers

def mpk_unpack(d):
    assert d[:4] == b"MPK ", "not an MPK archive"
    cnt = struct.unpack_from("<I", d, 8)[0]
    ents = []
    for i in range(cnt):
        name_off, data_off, size, extra = struct.unpack_from("<4I", d, 0x10 + 16 * i)
        name = d[name_off:d.index(b"\0", name_off)].decode()
        ents.append([name, extra, bytes(d[data_off:data_off + size])])
    return bytes(d[:8]), ents


def mpk_pack(head, ents):
    n = len(ents)
    names = b"".join(e[0].encode() + b"\0" for e in ents)
    name_base = 0x10 + 16 * n
    data_base = (name_base + len(names) + 15) & ~15
    table, offs, pos = bytearray(), [], name_base
    for e in ents:
        offs.append(pos)
        pos += len(e[0]) + 1
    out_pos = data_base
    body = bytearray()
    for e, no in zip(ents, offs):
        out_pos = (out_pos + 15) & ~15
        table += struct.pack("<4I", no, out_pos, len(e[2]), e[1])
        body += b"\0" * (out_pos - data_base - len(body))
        body += e[2]
        out_pos += len(e[2])
    body += b"\0" * ((-(data_base + len(body))) % 16)
    out = bytearray(head + struct.pack("<II", n, 0)) + table + names
    out += b"\0" * (data_base - len(out))
    return bytes(out + body)


def arc_unpack(d):
    assert d[:3] == b"ARC"
    cnt = struct.unpack_from("<I", d, 4)[0]
    ents = []
    for i in range(cnt):
        flag, tag, off, size = struct.unpack_from("<I4sII", d, 16 + 16 * i)
        ents.append([tag.decode(), flag, bytes(d[off:off + size])])
    return bytes(d[:4]), ents


def arc_pack(head, ents):
    n = len(ents)
    pos = 16 + 16 * n
    table, body = bytearray(), bytearray()
    for tag, flag, blob in ents:
        pad = (-pos) % 4
        body += b"\0" * pad
        pos += pad
        table += struct.pack("<I4sII", flag, tag.encode(), pos, len(blob))
        body += blob
        pos += len(blob)
    return bytes(head + struct.pack("<I", n) + b"\0" * 8 + table + body)


def parse_mpk(d):          # kept for other tools: {name: (offset, size)}
    assert d[:4] == b"MPK ", "not an MPK archive"
    cnt = struct.unpack_from("<I", d, 8)[0]
    ents = {}
    for i in range(cnt):
        name_off, data_off, size, _ = struct.unpack_from("<4I", d, 0x10 + 16 * i)
        ents[d[name_off:d.index(b"\0", name_off)].decode()] = (data_off, size)
    return ents


def parse_arc(d, base):    # {tag: (offset, size)}
    assert d[base:base + 3] == b"ARC"
    cnt = struct.unpack_from("<I", d, base + 4)[0]
    ents = {}
    for i in range(cnt):
        _, tag, off, size = struct.unpack_from("<I4sII", d, base + 16 + 16 * i)
        ents[tag.decode()] = (base + off, size)
    return ents


# ---------------------------------------------------------------------------------------------
# roster

def read_roster(path, min_id=FIRST_NEW):
    """[(charId, code, like)] for charId >= min_id (default FIRST_NEW) from a roster.txt ('<id> <code> [like] [k=v...]')."""
    out = []
    for line in open(path):
        line = line.split("#")[0].split()
        if len(line) < 2 or not line[0].isdigit():
            continue
        cid, code, like = int(line[0]), line[1], 9
        for tok in line[2:]:
            if "=" not in tok and tok.isdigit():
                like = int(tok)
        if cid >= min_id:
            out.append((cid, code, like))
    return out


def parse_objs(spec):
    """'w_ysh101:w_ten111,w_ysh102:w_ten112' -> [('w_ysh101', 'w_ten111'), ...]"""
    out = []
    for p in spec.split(","):
        if p.strip():
            new, src = p.strip().split(":")
            out.append((new.lower(), src.lower()))
    return out


def read_roster_objs(path):
    """(objs, charId of the line that declares the first objs= token) from a roster.txt; objs = every `objs=new:src,...` pair."""
    out, char = [], None
    for line in open(path):
        toks = line.split("#")[0].split()
        for tok in toks:
            if tok.startswith("objs="):
                out += parse_objs(tok[5:])
                if char is None and toks[0].isdigit():
                    char = int(toks[0])
    return out, char


WPNP_ROW = 0x3c
WPNP_ID_FIELDS = (8, 0x2a, 0x2c, 0x2e)      # u16 objentry ids inside a wpnp row


def add_objs(by, orecs, objs, log=print, row_char=None):
    """Clone objentry records and wpnp rows for the declared extra objects. Returns (row_offset, id_map)."""
    def oname(r):
        return bytes(r[0x24:0x34]).split(b"\0")[0].decode()

    def ostem(r):
        return bytes(r[0x34:]).split(b"\0")[0].decode()

    idmap = {}
    next_id = max(struct.unpack_from("<H", r, 0)[0] for r in orecs) + 1
    taken = {oname(r).lower() for r in orecs}
    for new, src in objs:
        srcs = [r for r in orecs if ostem(r) in (src, src + "_a")]     # the assist variant (w_ten112_a) travels along
        if not any(ostem(r) == src for r in srcs):
            raise SystemExit("objs: no objentry record with file stem %r" % src)
        if any(n.startswith(new) for n in taken):
            raise SystemExit("objs: name %r already exists" % new)
        for r in sorted(srcs, key=lambda r: struct.unpack_from("<H", r, 0)[0]):
            n = bytearray(r)
            nm = oname(r).lower().replace(src, new, 1).upper()
            stem = ostem(r).replace(src, new, 1)
            n[0x24:OBJ_REC] = nm.encode().ljust(0x10, b"\0") + stem.encode() + b"\0" * (OBJ_REC - 0x34 - len(stem))
            old_id = struct.unpack_from("<H", r, 0)[0]
            idmap[old_id] = next_id
            struct.pack_into("<H", n, 0, next_id)
            log("objentry: appended id %d %s (clone of %s id %d)" % (next_id, nm, oname(r), old_id))
            next_id += 1
            orecs.append(n)
    bt = by["battle.bin"]
    bh, bents = arc_unpack(bt[2])
    wp = next(e for e in bents if e[0] == "wpnp")
    cnt = struct.unpack_from("<I", wp[2], 4)[0]
    assert len(wp[2]) == 8 + WPNP_ROW * cnt, "unexpected wpnp size"
    rows = [bytearray(wp[2][8 + WPNP_ROW * i: 8 + WPNP_ROW * (i + 1)]) for i in range(cnt)]
    rowmap = {}
    for i in range(cnt):
        r = rows[i]
        if not any(struct.unpack_from("<H", r, f)[0] in idmap for f in WPNP_ID_FIELDS):
            continue                                        # spawns none of the cloned objects: stays shared
        n = bytearray(r)
        rowmap[i] = len(rows)
        struct.pack_into("<I", n, 0, len(rows))
        if row_char is not None:
            struct.pack_into("<H", n, 4, row_char)          # the copies belong to the new character
        for f in WPNP_ID_FIELDS:
            v = struct.unpack_from("<H", n, f)[0]
            if v in idmap:
                struct.pack_into("<H", n, f, idmap[v])
        rows.append(n)
    wp[2] = wp[2][:4] + struct.pack("<I", len(rows)) + b"".join(bytes(r) for r in rows)
    bt[2] = arc_pack(bh, bents)
    log("wpnp: %d rows -> %d (copies of the %d rows that spawn a cloned object; %d object ids replaced)"
        % (cnt, len(rows), len(rowmap), len(idmap)))
    log("wpnp: row map " + " ".join("%d>%d" % kv for kv in sorted(rowmap.items())))
    return rowmap, idmap


ABLT_REC = 40                     # system.bin 'ablt' record (after a 16-byte header)
ABLT_OWNER = 0x1B                 # owner charId byte (0xFF: shared ability)
ABLT_REFS = (6, 8)                # u16 ability ids inside a record (prerequisite, base ability), 0xFFFF = none
ABLT_GAME_ROWS = 0x328            # records FUN_088B1CC0 makes room for; more need roster.cpp's ablt hook
LEVEL_REC = 8                     # 'chrl' / 'amas' entry
LEVEL_IDS = {"chrl": (2, 4, 6), "amas": (0, 4, 6)}      # u16 ability ids inside an entry
ABLT_CMD = 12                     # u16 command id, looked up in the character's own objx comb
DISABLED_OWNER = 0xFE             # owner byte of a disabled ability: no charId (0..42) and not shared (0xFF)
DISABLED_CMD = 0xFFFE             # command id of a disabled ability (-2): matches no comb record (see disable_abilities)


def own_ability(v, like, char):
    """Ability id v of `like` -> the same ability of `char` ((owner << 8) | n); every other id is returned unchanged."""
    return (char << 8) | (v & 0xFF) if v != 0xFFFF and v >> 8 == like else v


def clone_ablt(blob, char, like, log=print):
    """'ablt' with the records of `char` replaced by copies of `like`'s (see "Abilities" in the module doc)."""
    n = struct.unpack_from("<I", blob, 8)[0]
    if len(blob) != 16 + ABLT_REC * n:
        raise SystemExit("ablt: unexpected size %d for %d records" % (len(blob), n))
    recs = [bytes(blob[16 + ABLT_REC * i: 16 + ABLT_REC * (i + 1)]) for i in range(n)]

    def rid(r):
        return struct.unpack_from("<H", r, 0)[0]

    old = [r for r in recs if r[ABLT_OWNER] == char or rid(r) >> 8 == char]
    keep = [r for r in recs if not (r[ABLT_OWNER] == char or rid(r) >> 8 == char)]
    clones = []
    for r in recs:
        if r[ABLT_OWNER] != like or rid(r) >> 8 != like:
            continue
        c = bytearray(r)
        for f in (0,) + ABLT_REFS:
            struct.pack_into("<H", c, f, own_ability(struct.unpack_from("<H", c, f)[0], like, char))
        c[ABLT_OWNER] = char
        clones.append(bytes(c))
    if not clones:
        log("ablt: charId %d has no records to copy for charId %d" % (like, char))
        return blob
    out = sorted(keep + clones, key=rid)
    ids = [rid(r) for r in out]
    if len(set(ids)) != len(ids):
        raise SystemExit("ablt: duplicate ability ids after adding charId %d" % char)
    if old:
        log("ablt: dropped charId %d's own records %s" % (char, " ".join("%04X" % rid(r) for r in old)))
    log("ablt: %d records -> %d, charId %d <- %d copies of charId %d's (%s)" % (
        n, len(out), char, len(clones), like, " ".join("%04X" % rid(r) for r in clones)))
    return bytes(blob[:8]) + struct.pack("<I", len(out)) + bytes(blob[12:16]) + b"".join(out)


def parse_level_table(blob, tag):
    """'chrl' / 'amas' -> (header, {charId: (word 3 of the index row, [8-byte entries])}); checks the layout is the one
    level_table_bytes writes back (index rows sorted by charId, entries contiguous in index order)."""
    n = struct.unpack_from("<I", blob, 8)[0]
    rows, pos = {}, 16 + 16 * n
    for k in range(n):
        cid, cnt, off, w3 = struct.unpack_from("<4I", blob, 16 + 16 * k)
        if off != pos or cid in rows or (rows and cid < max(rows)):
            raise SystemExit("%s: unexpected layout at row %d (charId %d)" % (tag, k, cid))
        rows[cid] = (w3, [bytes(blob[off + LEVEL_REC * j: off + LEVEL_REC * (j + 1)]) for j in range(cnt)])
        pos += LEVEL_REC * cnt
    if pos != len(blob):
        raise SystemExit("%s: %d bytes after the last entry" % (tag, len(blob) - pos))
    return bytes(blob[:16]), rows


def level_table_bytes(head, rows):
    base = 16 + 16 * len(rows)
    index, body = bytearray(), bytearray()
    for cid in sorted(rows):
        w3, ents = rows[cid]
        index += struct.pack("<4I", cid, len(ents), base + len(body), w3)
        body += b"".join(ents)
    return head[:8] + struct.pack("<I", len(rows)) + head[12:16] + bytes(index) + bytes(body)


def clone_level_table(blob, tag, char, like, log=print):
    """'chrl' / 'amas' with a row for `char` that copies `like`'s, ability ids moved to `char`."""
    head, rows = parse_level_table(blob, tag)
    if level_table_bytes(head, rows) != bytes(blob):
        raise SystemExit("%s: does not round trip" % tag)
    if like not in rows:
        log("%s: charId %d has no row to copy for charId %d" % (tag, like, char))
        return blob
    w3, ents = rows[like]
    new = []
    for e in ents:
        e = bytearray(e)
        for f in LEVEL_IDS[tag]:
            struct.pack_into("<H", e, f, own_ability(struct.unpack_from("<H", e, f)[0], like, char))
        new.append(bytes(e))
    log("%s: charId %d row %s, %d entries copied from charId %d" % (tag, char, "replaced" if char in rows else "added",
                                                                    len(new), like))
    rows[char] = (w3, new)
    return level_table_bytes(head, rows)


def add_abilities(aents, char, like, log=print):
    """system.bin ARC entries: give `char` its own copies of `like`'s 'ablt' records and 'chrl' / 'amas' rows."""
    by = {e[0]: e for e in aents}
    by["ablt"][2] = clone_ablt(by["ablt"][2], char, like, log)
    for tag in ("chrl", "amas"):
        by[tag][2] = clone_level_table(by[tag][2], tag, char, like, log)


REPOINT_OPS = (0x075A, 0x0C65)     # ranged events whose arg 1 is a global wpnp row (summoned / thrown object)


def _row_arg_sites(blob):
    """Offsets inside one motion's event script of arg 1 of its ranged REPOINT_OPS events (None: undecodable).
    Same layout as dfx/objx.py decode_events: 16-byte header (u16 [1] = keyed << 8 | ranged + 1, [2] = keyed
    offset), a 4..n byte preamble, then ranged events {u16 start, u16 end, u16 op, u16, u32, u32 x (op >> 8)}."""
    h = struct.unpack_from("<8H", blob, 0)
    n_r, off_k = (h[1] & 0xFF) - 1, h[2]
    for pre in range(4, off_k - 16 + 1, 4):
        p, sites, good = 16 + pre, [], True
        for _ in range(max(n_r, 0)):
            if p + 6 > off_k:
                good = False
                break
            op = struct.unpack_from("<H", blob, p + 4)[0]
            if op in REPOINT_OPS and (op >> 8) >= 2:
                sites.append(p + 16)
            p += 12 + 4 * (op >> 8)
        if good and p == off_k:
            return sites
    return None


def repoint_objx(data, rowmap, log=print):
    """Point a body objx's summon / thrown-object events (REPOINT_OPS arg 1) at the copied wpnp rows."""
    d = bytearray(data)
    if d[:4] != b"ARC\x01":
        raise SystemExit("repoint: not an ARC objx")
    changed = undecoded = 0
    for i in range(struct.unpack_from("<I", d, 4)[0]):
        t, _tag, o, size = struct.unpack_from("<I4sII", d, 16 + i * 16)
        if t != 8:
            continue
        nb = struct.unpack_from("<I", d, o + 4)[0]
        offs = list(struct.unpack_from("<%dI" % nb, d, o + 8)) + [size]
        for b in range(nb):
            bo = o + offs[b]
            sites = _row_arg_sites(bytes(d[bo:o + offs[b + 1]]))
            if sites is None:
                undecoded += 1
                continue
            for p in sites:
                v = struct.unpack_from("<I", d, bo + p)[0]
                if v in rowmap:
                    struct.pack_into("<I", d, bo + p, rowmap[v])
                    changed += 1
    log("repoint: %d event args moved to copied wpnp rows (%d motion scripts not decodable, left as is)" % (changed, undecoded))
    return bytes(d)


# ---------------------------------------------------------------------------------------------
# ability names and help texts (see "Ability names" in the module doc and games/dissidia/docs/roster.md)
# Added 2026-10-19; nothing above this block changed. build() calls apply_ability_names() only when it is given a name map.

TEXT_LANGS = ("en", "fr", "de", "it", "es", "jp")      # language folders; this package has only 'en'
LANG_ARCHIVE = "general_archive/main/%s/main_lang.bin"  # MPK, read once at boot; member NAME_MEMBER = ability names
NAME_MEMBER = "ability.bin"
HELP_PACK = "text/%s/mess_pk_customize_ability_0.bin"  # mess pak of the Customize screens; entry 0 = help texts
LANG_ARCHIVE_MAX = 0x80000      # the boot loader reads main_lang.bin into a 512 KiB slice of a 2 MiB block (FUN_0893354C)
TEXT_RECORDS_MAX = 0x7FFF       # the text decoder (FUN_0887A6A0) sign-extends the record count to 16 bits
ABLT_NAME, ABLT_HELP = 2, 4     # u16 text ids in an 'ablt' record: ability.bin record, help pack entry 0 record
ABLT_AFTER = 6                  # u16 prerequisite ability: the ability this one follows up (0xFFFF: none)
ABLT_TYPE, ABLT_PHASE = 0x16, 0x17   # u8 type (0 brave, 1 HP, 2-4 other kinds); u8 0 ground, 1 midair, 0xFF follow-up
ABLT_TYPES = {"brave": 0, "hp": 1}
ABLT_CP, ABLT_CP_MASTERED, ABLT_AP, ABLT_BONUS = 0x0E, 0x10, 0x12, 0x14   # s16 CP to equip (before / after mastering),
# s16 AP to master, s16 equip bonus = a system.bin 'fncs' effect-set id (-1 none); docs/archive/ablt-fields
ABLT_PHASES = {"ground": 0, "midair": 1, "follow-up": 0xFF}
ABILITY_NAMES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "extra_abilities.json")
HELP_LINES, HELP_WIDTH, NAME_WIDTH = 3, 35, 31          # the largest retail help text / name (characters shown)
EFFECT_PREFIX = "\x10Łａ"                      # orange 'Additional Effect: ...' / 'Effects: ...' line
NAME_TOKENS = {"{ground}": "\x11P(ground)\x11d", "{midair}": "\x11P(midair)\x11d"}


def _shown(s):
    """Characters a text shows (formatting codes \\x10 + 2 units, \\x11 + 1 unit and \\x1b left out)."""
    return re.sub("\x11.", "", re.sub("\x10..", "", s)).replace("\x1b", "")


def expand_name(text):
    for k, v in NAME_TOKENS.items():
        text = text.replace(k, v)
    if len(_shown(text)) > NAME_WIDTH:
        raise SystemExit("ability name %r: %d characters, the longest retail name has %d" % (text, len(_shown(text)), NAME_WIDTH))
    return text


def expand_help(text, like):
    """A help text of the name map with its tokens filled in from `like` (the help text the record had, i.e. its like's):
    a line '{bonus}' = the like's equip-bonus lines (Bartz: 'Minor Speed Boost effect.'; they describe the record, which
    keeps the like's values), '{effects}' = the like's orange effect lines ('Additional Effect: chase'), '{like}' = all
    of it; '{effect:X}' / '{effects:X}' = an orange 'Additional Effect: X' / 'Effects: X' line."""
    lines = like.split("\n") if like else []
    eff = [ln for ln in lines if ln.startswith(EFFECT_PREFIX)]
    bonus = [ln for ln in lines if "\x10" in ln and not ln.startswith(EFFECT_PREFIX)]
    out = []
    for ln in text.split("\n"):
        if ln.strip() in ("{bonus}", "{effects}", "{like}"):
            out += {"{bonus}": bonus, "{effects}": eff, "{like}": lines}[ln.strip()]
            continue
        ln = re.sub(r"\{effect:\s*([^}]*)\}", lambda m: EFFECT_PREFIX + "Additional Effect: " + m.group(1) + "\x1b", ln)
        ln = re.sub(r"\{effects:\s*([^}]*)\}", lambda m: EFFECT_PREFIX + "Effects: " + m.group(1) + "\x1b", ln)
        out.append(ln)
    if len(out) > HELP_LINES or any(len(_shown(ln)) > HELP_WIDTH for ln in out):
        raise SystemExit("help text %r: the help panel shows %d lines of at most %d characters" % (out, HELP_LINES, HELP_WIDTH))
    return "\n".join(out)


def load_ability_names(path):
    """{code: {n: entry}} from the JSON name map: {"<code>": {"<n, hex>": {"cmd": "0xA2", "name": ..., "help": ...}}}.
    Keys starting with '_' are comments. name / help may be a string (every language) or {"en": ..., "fr": ...}."""
    with open(path, encoding="utf-8") as f:
        j = json.load(f)
    return {code.lower(): {int(k, 16): v for k, v in tab.items() if not k.startswith("_")}
            for code, tab in j.items() if not code.startswith("_")}


class AbilityTexts:
    """The ability names (LANG_ARCHIVE -> NAME_MEMBER) and help texts (HELP_PACK entry 0) of every language the package
    has, so that new records can be appended with the same id in every language. read(path) -> bytes (KeyError if
    missing). Retail records keep their ids and bytes; files() gives the rebuilt files (empty when nothing was added)."""

    def __init__(self, read, required=True):
        self.langs, self.names, self.helps, self._src, self.added = [], {}, {}, {}, {}
        for lang in TEXT_LANGS:
            try:
                arc_data, pak_data = read(LANG_ARCHIVE % lang), read(HELP_PACK % lang)
            except KeyError:
                continue
            head, ents = mpk_unpack(arc_data)
            member = next((e for e in ents if e[0] == NAME_MEMBER), None)
            pak = mp.parse(pak_data)
            if member is None or mpk_pack(head, ents) != arc_data:
                raise SystemExit("%s: no %s or does not round trip" % (LANG_ARCHIVE % lang, NAME_MEMBER))
            names, helps = mt.records(member[2]), mt.records(mp.entry(pak, 0))
            def same_layout(texts, e):
                return (mt.build_off4 if bytes(e[:4]) == mt.OFF4 else mt.build)(texts) == bytes(e)
            if not same_layout(names, member[2]) or not same_layout(helps, mp.entry(pak, 0)) or \
                    mp.build(pak, [mp.entry(pak, k) for k in range(len(pak["ents"]))]) != pak_data:
                raise SystemExit("%s / %s: text tables do not round trip" % (LANG_ARCHIVE % lang, HELP_PACK % lang))
            self.langs.append(lang)
            self.names[lang], self.helps[lang], self._src[lang] = names, helps, (head, ents, pak)
        if not self.langs and required:
            raise SystemExit("no %s / %s in the package" % (LANG_ARCHIVE % "<lang>", HELP_PACK % "<lang>"))
        self.retail = {"name": max((len(self.names[g]) for g in self.langs), default=0),
                       "help": max((len(self.helps[g]) for g in self.langs), default=0)}

    def text(self, kind, i, lang=None):
        tab = (self.names if kind == "name" else self.helps)[lang or self.langs[0]]
        return tab[i] if i < len(tab) else ""

    def add(self, kind, per_lang):
        """Append one record (or reuse an identical one added before); per_lang: {lang: text}. Returns its id."""
        tab = self.names if kind == "name" else self.helps
        key = (kind, tuple(per_lang[g] for g in self.langs))
        if key not in self.added:
            i = max(len(tab[g]) for g in self.langs)
            for g in self.langs:
                tab[g] += [""] * (i - len(tab[g])) + [per_lang[g]]
            self.added[key] = i
        return self.added[key]

    def files(self, log=None):
        """{package path: bytes} of the rebuilt name archive and help pack of every language ({} if nothing was added).
        Each table is written in the retail (plain) layout while its records start below 64 Ki words, else as an OFF4
        entry (mess_text.build_fit; the game reads both, retail entry 1 of the help pack is OFF4). The help texts outgrow
        the plain layout first: retail entry 0 is 128,302 of the at most ~131,070 bytes."""
        if not self.added:
            return {}
        out = {}
        for g in self.langs:
            for kind, tab in (("names", self.names[g]), ("help texts", self.helps[g])):
                if len(tab) > TEXT_RECORDS_MAX:
                    raise SystemExit("%s (%s): %d records, the game reads at most %d" % (kind, g, len(tab), TEXT_RECORDS_MAX))
                if log and not mt.plain_fits(tab):
                    log("ablt names: %s (%s) written as an OFF4 table (%d records, too many words for the plain layout)"
                        % (kind, g, len(tab)))
            head, ents, pak = self._src[g]
            ents = [[e[0], e[1], mt.build_fit(self.names[g]) if e[0] == NAME_MEMBER else e[2]] for e in ents]
            out[LANG_ARCHIVE % g] = mpk_pack(head, ents)
            if len(out[LANG_ARCHIVE % g]) > LANG_ARCHIVE_MAX:
                raise SystemExit("%s: %d B, the game reads at most %d B" % (LANG_ARCHIVE % g, len(out[LANG_ARCHIVE % g]), LANG_ARCHIVE_MAX))
            blobs = [mp.entry(pak, k) for k in range(len(pak["ents"]))]
            blobs[0] = mt.build_fit(self.helps[g])
            out[HELP_PACK % g] = mp.build(pak, blobs)
        return out


def load_names_and_texts(read, path=None, required=False):
    """(name map, AbilityTexts) for build(), or (None, None) when there is no name map (path 'none', or the default file
    missing). read: package read(path) -> bytes. required: a missing file is an error (an explicit --names PATH)."""
    path = ABILITY_NAMES if path is None else path
    if path == "none":
        return None, None
    if not os.path.exists(path):
        if required:
            raise SystemExit("--names: %s not found" % path)
        return None, None
    texts = AbilityTexts(read, required=False)
    if not texts.langs:                     # a package without the text tables (a synthetic test package): names stay
        return None, None
    return load_ability_names(path), texts


def apply_ability_names(aents, chars, names, texts, log=print):
    """Point the 'ablt' records of every roster character that the name map lists at new name / help records (appended
    to `texts`). A record whose command id differs from the map's 'cmd' is left alone (the map was written for another
    moveset). An entry may also set the record's kind: "type" brave / hp (+0x16), "phase" ground / midair / follow-up
    (+0x17) and "after" (+6, the ability it follows up; null = none), e.g. to turn the like's HP follow-up into a brave
    move of its own. Returns {ability id: (name id, help id)} of the records whose texts changed."""
    by = {e[0]: e for e in aents}
    blob = by["ablt"][2]
    n = struct.unpack_from("<I", blob, 8)[0]
    recs = [bytearray(blob[16 + ABLT_REC * i: 16 + ABLT_REC * (i + 1)]) for i in range(n)]
    changed, fields = {}, {}
    for char, code, _like in chars:
        table = names.get((code or "").lower())
        if not table:
            continue
        seen = set()
        for r in recs:
            rid = struct.unpack_from("<H", r, 0)[0]
            ent = table.get(rid & 0xFF)
            if r[ABLT_OWNER] != char or rid >> 8 != char or ent is None:
                continue
            seen.add(rid & 0xFF)
            cmd = struct.unpack_from("<H", r, 12)[0]
            if "cmd" in ent and int(str(ent["cmd"]), 0) != cmd:
                log("ablt names: %04X has command %03X, the name map says %s: left as is" % (rid, cmd, ent["cmd"]))
                continue
            before = bytes(r)
            if "type" in ent:                       # brave / HP (+0x16): which set slots the Abilities screen offers
                r[ABLT_TYPE] = ABLT_TYPES[str(ent["type"]).lower()]
            if "phase" in ent:                      # ground / midair / follow-up of the +6 ability (+0x17)
                r[ABLT_PHASE] = ABLT_PHASES[str(ent["phase"]).lower()]
            if "after" in ent:                      # prerequisite ability (+6): null = none, else an ability id
                v = ent["after"]
                struct.pack_into("<H", r, ABLT_AFTER, 0xFFFF if v in (None, "", "none") else int(str(v), 16) if isinstance(v, str) else int(v))
            if ent.get("cp") is not None:           # CP to equip (+0x0E); once mastered (+0x10) defaults to half, rounded down
                cp = int(ent["cp"])
                struct.pack_into("<hh", r, ABLT_CP, cp, int(ent["cp_mastered"]) if ent.get("cp_mastered") is not None else cp // 2)
            elif ent.get("cp_mastered") is not None:
                struct.pack_into("<h", r, ABLT_CP_MASTERED, int(ent["cp_mastered"]))
            if ent.get("ap") is not None:           # AP to master (+0x12)
                struct.pack_into("<h", r, ABLT_AP, int(ent["ap"]))
            if "bonus" in ent:                      # equip bonus (+0x14): a 'fncs' effect-set id, null = none
                v = ent["bonus"]
                struct.pack_into("<h", r, ABLT_BONUS, -1 if v in (None, "", "none") else int(str(v), 0))
            if bytes(r) != before:
                log("ablt names: %04X type %d phase %02X after %04X cp %d/%d ap %d bonus %d (was %d / %02X / %04X / %d/%d / %d / %d)" % (
                    (rid, r[ABLT_TYPE], r[ABLT_PHASE], struct.unpack_from("<H", r, ABLT_AFTER)[0]) + struct.unpack_from("<4h", r, ABLT_CP) +
                    (before[ABLT_TYPE], before[ABLT_PHASE], struct.unpack_from("<H", before, ABLT_AFTER)[0]) + struct.unpack_from("<4h", before, ABLT_CP)))
                fields[rid] = (r[ABLT_TYPE], r[ABLT_PHASE], struct.unpack_from("<H", r, ABLT_AFTER)[0])
            nid, hid = struct.unpack_from("<HH", r, ABLT_NAME)

            def per_lang(v, fn):
                return {g: fn(v.get(g, v.get("en")) if isinstance(v, dict) else v, g) for g in texts.langs}
            if ent.get("name"):
                nid = texts.add("name", per_lang(ent["name"], lambda t, g: expand_name(t)))
            if ent.get("help"):
                hid = texts.add("help", per_lang(ent["help"], lambda t, g: expand_help(t, texts.text("help", hid, g))))
            if (nid, hid) != struct.unpack_from("<HH", r, ABLT_NAME):
                struct.pack_into("<HH", r, ABLT_NAME, nid, hid)
                changed[rid] = (nid, hid)
        for k in sorted(set(table) - seen):
            log("ablt names: %s has no ability %04X" % (code, (char << 8) | k))
    if changed:
        log("ablt names: %d records point at new texts (names from %d, help texts from %d; %d / %d new records)" % (
            len(changed), texts.retail["name"], texts.retail["help"],
            max(len(t) for t in texts.names.values()) - texts.retail["name"],
            max(len(t) for t in texts.helps.values()) - texts.retail["help"]))
    by["ablt"][2] = bytes(blob[:16]) + b"".join(bytes(r) for r in recs)
    return changed


def add_new_abilities(aents, chars, names, log=print):
    """Name-map entries with "copy" add an ability of the character's own: record (char << 8) | n = a copy of the
    character's (char << 8) | copy with command id (+12) = the entry's "cmd" (required); "learn": N lists it in the 'chrl'
    row at level N (a free id slot of that level's entry, else a new entry). Its name / help / type / phase / after come
    from the same entry afterwards (apply_ability_names). Returns the new ids."""
    by = {e[0]: e for e in aents}
    blob = by["ablt"][2]
    n = struct.unpack_from("<I", blob, 8)[0]
    recs = [bytes(blob[16 + ABLT_REC * i: 16 + ABLT_REC * (i + 1)]) for i in range(n)]
    have = {struct.unpack_from("<H", r, 0)[0]: r for r in recs}
    added, learn = [], {}
    for char, code, _like in chars:
        for k, ent in sorted((names.get((code or "").lower()) or {}).items()):
            if "copy" not in ent:
                continue
            nid, sid = (char << 8) | k, (char << 8) | int(str(ent["copy"]), 16)
            if "cmd" not in ent:
                raise SystemExit("ablt: new ability %04X needs a 'cmd'" % nid)
            if nid in have:
                raise SystemExit("ablt: new ability %04X already exists" % nid)
            src = have.get(sid)
            if src is None or src[ABLT_OWNER] != char:
                raise SystemExit("ablt: new ability %04X: no record %04X of charId %d to copy" % (nid, sid, char))
            r = bytearray(src)
            struct.pack_into("<H", r, 0, nid)
            struct.pack_into("<H", r, 12, int(str(ent["cmd"]), 0))
            have[nid] = bytes(r)
            added.append(nid)
            log("ablt: new ability %04X = copy of %04X with command %03X" % (nid, sid, int(str(ent["cmd"]), 0)))
            if ent.get("learn") is not None:
                learn.setdefault(char, []).append((int(ent["learn"]), nid))
    if not added:
        return added
    out = sorted(have.values(), key=lambda r: struct.unpack_from("<H", r, 0)[0])
    by["ablt"][2] = bytes(blob[:8]) + struct.pack("<I", len(out)) + bytes(blob[12:16]) + b"".join(out)
    if learn:
        head, rows = parse_level_table(by["chrl"][2], "chrl")
        for char, items in learn.items():
            w3, ents = rows.get(char, (0, []))
            ents = [bytearray(e) for e in ents]
            for lv, aid in items:
                e = next((e for e in ents if e[0] == lv), None)
                slot = next((f for f in LEVEL_IDS["chrl"] if e is not None and struct.unpack_from("<H", e, f)[0] == 0xFFFF), None)
                if slot is not None:
                    struct.pack_into("<H", e, slot, aid)
                else:
                    ents.append(bytearray(struct.pack("<BB3H", lv, 0, aid, 0xFFFF, 0xFFFF)))
                    ents.sort(key=lambda x: x[0])
                log("chrl: charId %d learns %04X at level %d" % (char, aid, lv))
            rows[char] = (w3, [bytes(e) for e in ents])
        by["chrl"][2] = level_table_bytes(head, rows)
    return added


def set_learn_levels(aents, chars, names, log=print):
    """Name-map entries without "copy" but with "learn": N move that ability of the character to level N in its 'chrl'
    row: its id leaves the entries it was in (an entry left with no id goes) and takes a free id slot of the level-N
    entry, else a new entry. (New abilities get their level in add_new_abilities.) Returns {ability id: level}."""
    by = {e[0]: e for e in aents}
    head, rows = parse_level_table(by["chrl"][2], "chrl")
    moved = {}
    for char, code, _like in chars:
        items = [(k, ent) for k, ent in sorted((names.get((code or "").lower()) or {}).items())
                 if "copy" not in ent and ent.get("learn") is not None and not ent.get("disabled")]
        if not items or char not in rows:
            continue
        w3, ents = rows[char]
        ents = [bytearray(e) for e in ents]
        for k, ent in items:
            aid, lv = (char << 8) | k, int(ent["learn"])
            if not 1 <= lv <= 100:
                raise SystemExit("chrl: %04X: learn level %d is not 1..100" % (aid, lv))
            have = [e for e in ents if aid in [struct.unpack_from("<H", e, f)[0] for f in LEVEL_IDS["chrl"]]]
            if [e[0] for e in have] == [lv]:
                continue
            for e in have:
                for f in LEVEL_IDS["chrl"]:
                    if struct.unpack_from("<H", e, f)[0] == aid:
                        struct.pack_into("<H", e, f, 0xFFFF)
            ents = [e for e in ents if any(struct.unpack_from("<H", e, f)[0] != 0xFFFF for f in LEVEL_IDS["chrl"])]
            e = next((e for e in ents if e[0] == lv), None)
            slot = next((f for f in LEVEL_IDS["chrl"] if e is not None and struct.unpack_from("<H", e, f)[0] == 0xFFFF), None)
            if slot is not None:
                struct.pack_into("<H", e, slot, aid)
            else:
                ents.append(bytearray(struct.pack("<BB3H", lv, 0, aid, 0xFFFF, 0xFFFF)))
                ents.sort(key=lambda x: x[0])
            moved[aid] = lv
            log("chrl: charId %d learns %04X at level %d (was %s)" % (char, aid, lv, ", ".join(str(x[0]) for x in have) or "never"))
        rows[char] = (w3, [bytes(e) for e in ents])
    if moved:
        by["chrl"][2] = level_table_bytes(head, rows)
    return moved


def disable_abilities(aents, chars, names, log=print):
    """Name-map entries with "disabled": true take an ability out of the character's kit (Star of the Rogue, 22 Oct
    2026, until its animation exists) while keeping its 'ablt' record, so that saves which learned or equipped it stay
    valid. Remove the flag and rebuild to bring the ability back exactly as before.

    The record must stay: the game dereferences the record of every learned and every equipped id without a null check
    (EBOOT FUN_088B5254 at 0x088B5340: the record of each learned id; FUN_0884921C at 0x08849644: the record of the
    equipped slot that was pressed), so a deleted record would be read through a null pointer. Instead:
      * owner byte (+0x1B) = DISABLED_OWNER. The Abilities screen lists only records whose owner is the edited charId or
        0xFF (menu FUN_08AD6698 at 0x08AD6860 and, for the abilities it adds, 0x08AD6924), so the ability is not offered;
        a set slot that still holds it finds no list entry and shows as empty (its 0x3EBA index stays 0xFF). FUN_08ACF0D8
        (menu, learned-ability count) checks the owner the same way; FUN_088B5254 only compares two records' owners;
        roster.cpp learn_all_own teaches only records whose owner is the charId, so it stops teaching this one.
      * command id (+12) = DISABLED_CMD (-2). Battle resolves an equipped ability through its command: FUN_0884921C
        looks it up in the actor's comb (FUN_08AB8134 compares each comb record's +0 id and +4 halfword; Noctis's are
        0xA2..0xB0 and -1) and does nothing when there is no match (0x0884966C beqz). FUN_08AB7F40 (enabled-command bits)
        then sets no bit, so the CPU never picks it, and FUN_08A651A8 / FUN_08A65248 return a negative command, which
        their caller FUN_08A652F4 treats as "no command". -1 is avoided on purpose: it would match the comb records'
        +4 = -1 in FUN_08AB8134 and play the first command.
      * the character's 'chrl' entries drop the id and an 'amas' entry that unlocks it is dropped (learning it again
        would only re-learn the hidden record); an 'amas' entry that merely needs it is left alone.
    Saves keep what they hold: a learned row of the id stays (hidden, inert) and a set slot holding it stays (shown
    empty, does nothing in battle) until the player equips something else there.
    The name / help texts stay. Returns the disabled ids."""
    by = {e[0]: e for e in aents}
    blob = by["ablt"][2]
    n = struct.unpack_from("<I", blob, 8)[0]
    recs = [bytearray(blob[16 + ABLT_REC * i: 16 + ABLT_REC * (i + 1)]) for i in range(n)]
    have = {struct.unpack_from("<H", r, 0)[0]: r for r in recs}
    off = {}
    for char, code, _like in chars:
        for k, ent in sorted((names.get((code or "").lower()) or {}).items()):
            if not ent.get("disabled"):
                continue
            aid = (char << 8) | k
            r = have.get(aid)
            if r is None or r[ABLT_OWNER] != char:
                raise SystemExit("ablt: disabled ability %04X: no record of charId %d" % (aid, char))
            cmd = struct.unpack_from("<H", r, ABLT_CMD)[0]
            r[ABLT_OWNER] = DISABLED_OWNER
            struct.pack_into("<H", r, ABLT_CMD, DISABLED_CMD)
            off.setdefault(char, set()).add(aid)
            log("ablt: %04X disabled (owner %02X -> %02X, command %03X -> %04X; record and texts kept)"
                % (aid, char, DISABLED_OWNER, cmd, DISABLED_CMD))
    if not off:
        return []
    by["ablt"][2] = bytes(blob[:16]) + b"".join(bytes(r) for r in recs)
    for tag in ("chrl", "amas"):
        head, rows = parse_level_table(by[tag][2], tag)
        changed = False
        for char, ids in off.items():
            if char not in rows:
                continue
            w3, ents = rows[char]
            new = []
            for e in ents:
                e = bytearray(e)
                hit = [f for f in LEVEL_IDS[tag] if struct.unpack_from("<H", e, f)[0] in ids]
                if tag == "amas" and 0 in hit:              # the ability this entry unlocks is disabled: drop it
                    changed = True
                    log("amas: charId %d entry %s dropped (unlocks a disabled ability)" % (char, bytes(e).hex()))
                    continue
                if tag == "amas" and hit:                   # a disabled prerequisite: left as it is (logged)
                    log("amas: charId %d entry %s needs a disabled ability (left as is)" % (char, bytes(e).hex()))
                    hit = []
                for f in hit:
                    struct.pack_into("<H", e, f, 0xFFFF)
                if hit:
                    changed = True
                    if tag == "chrl" and all(struct.unpack_from("<H", e, f)[0] == 0xFFFF for f in LEVEL_IDS[tag]):
                        log("chrl: charId %d level %d entry dropped (only a disabled ability)" % (char, e[0]))
                        continue
                    log("%s: charId %d entry %s no longer lists a disabled ability" % (tag, char, bytes(e).hex()))
                new.append(bytes(e))
            rows[char] = (w3, new)
        if changed:
            by[tag][2] = level_table_bytes(head, rows)
    return sorted(i for ids in off.values() for i in ids)


def build(data, chars, rmax=ROSTER_MAX, log=print, min_id=FIRST_NEW, objs=None, objs_char=None, rowmap_out=None,
          names=None, texts=None):
    """data: original main.bin; chars: [(charId, code or None, like)]. Returns the new main.bin.
    min_id: lowest charId accepted (33 takes over the retail SEV120 slot: its remaining objentry records are dropped).
    names / texts: a name map (load_ability_names) and the AbilityTexts the new records are appended to (the caller
    writes texts.files()); without them the copied abilities keep the like's names."""
    head, ents = mpk_unpack(data)
    by = {e[0]: e for e in ents}
    ahead, aents = arc_unpack(by["system.bin"][2])
    chrp = next(e for e in aents if e[0] == "chrp")
    obj = by["objentry.bin"]

    chd = chrp[2]
    cn = struct.unpack_from("<I", chd, 4)[0]
    recs = [bytearray(chd[8 + CHRP_REC * i: 8 + CHRP_REC * (i + 1)]) for i in range(cn)]
    ohead = obj[2][:4]
    ocnt = struct.unpack_from("<I", obj[2], 4)[0]
    orecs = [bytearray(obj[2][8 + OBJ_REC * i: 8 + OBJ_REC * (i + 1)]) for i in range(ocnt)]

    def oname(r):
        return bytes(r[0x24:]).split(b"\0")[0].decode()

    for char, code, like in chars:
        if not min_id <= char < rmax:
            raise SystemExit("charId %d outside %d..%d (ROSTER_MAX %d)" % (char, min_id, rmax - 1, rmax))
        cidx = {r[0]: i for i, r in enumerate(recs)}
        if like not in cidx:
            raise SystemExit("chrp has no record for like charId %d (has %s)" % (like, sorted(cidx)))
        new = bytearray(recs[cidx[like]])
        like_code = bytes(new[0x74:0x7C]).rstrip(b"\0").decode()[2:]      # 'P_FIV100' -> 'FIV100'
        new[0] = char
        if code:
            if len(code) > 6:
                raise SystemExit("code %r too long: chrp holds 'P_' + 6 characters" % code)
            new[0x74:0x7C] = ("P_" + code.upper()).encode().ljust(8, b"\0")
        if char in cidx:
            recs[cidx[char]] = new
            log("chrp: charId %d record %d <- clone of charId %d (code %s)" %
                (char, cidx[char], like, bytes(new[0x74:0x7C]).rstrip(b"\0").decode()))
        else:
            pos = sum(1 for r in recs if r[0] < char)
            recs.insert(pos, new)
            log("chrp: charId %d inserted at record %d <- clone of charId %d (code %s)" %
                (char, pos, like, bytes(new[0x74:0x7C]).rstrip(b"\0").decode()))

        def renamed(r):
            nm = oname(r)
            if code:
                parts = nm.split("_")                    # P_FIV100_1P -> [P, FIV100, 1P]
                if len(parts) < 3 or parts[1] != like_code:
                    raise SystemExit("unexpected objentry name %r" % nm)
                parts[1] = code.upper()
                nm = "_".join(parts)
            b = nm.encode()
            # +0x24 holds the object name (16 bytes), +0x34 the file stem (16 bytes): 'p_fiv100', 'p_fiv100_w', 'p_fiv100_a'. The engine
            # builds obj/<stem>.objx (the move / motion script of the body) from it, so a clone must carry its own stem; with the field
            # zeroed (what this function used to do) the body loads but its objx is never requested and the actor never runs.
            stem = bytes(r[0x34:OBJ_REC]).split(b"\0")[0].decode()
            if code and like_code.lower() in stem:
                stem = stem.replace(like_code.lower(), code.lower(), 1)
            r[0x24:0x34] = b.ljust(0x10, b"\0")[:0x10]
            r[0x34:OBJ_REC] = stem.encode().ljust(OBJ_REC - 0x34, b"\0")[:OBJ_REC - 0x34]

        likes = {oname(r): r for r in orecs if r[6] == like and oname(r).startswith("P_%s_" % like_code)}
        used = set()
        for i, r in enumerate(orecs):                    # rewrite the records the id already owns (ORG800)
            if r[6] != char:
                continue
            for nm, j in sorted(likes.items()):
                if nm in used or not nm.endswith(("_1P", "_1P_A")) or j[8] != r[8]:
                    continue
                n = bytearray(j)
                n[6] = char
                struct.pack_into("<H", n, 0, struct.unpack_from("<H", r, 0)[0])   # keep the unique id
                renamed(n)
                log("objentry: record %d (%s) <- %s as charId %d" % (i, oname(r), nm, char))
                orecs[i] = n
                used.add(nm)
                break
        next_id = max(struct.unpack_from("<H", r, 0)[0] for r in orecs) + 1
        for nm, j in sorted(likes.items(), key=lambda kv: struct.unpack_from("<H", kv[1], 0)[0]):
            if nm in used:
                continue
            n = bytearray(j)
            n[6] = char
            struct.pack_into("<H", n, 0, next_id)
            next_id += 1
            renamed(n)
            orecs.append(n)
            log("objentry: appended id %d %s (clone of %s)" % (next_id - 1, oname(n), nm))
        keep = {struct.unpack_from("<H", r, 0)[0] for r in orecs if r[6] == char and oname(r).startswith("P_%s_" % (code or like_code).upper())}
        for r in [r for r in orecs if r[6] == char and struct.unpack_from("<H", r, 0)[0] not in keep]:
            log("objentry: dropped leftover record %d (%s) of charId %d" % (struct.unpack_from("<H", r, 0)[0], oname(r), char))
            orecs.remove(r)
        # the table is sorted by id; clones keep ascending ids so it stays sorted
        assert [struct.unpack_from("<H", r, 0)[0] for r in orecs] == sorted(struct.unpack_from("<H", r, 0)[0] for r in orecs)
        add_abilities(aents, char, like, log)

    if names:
        add_new_abilities(aents, chars, names, log)
    ablt = next(e for e in aents if e[0] == "ablt")[2]
    if struct.unpack_from("<I", ablt, 8)[0] > ABLT_GAME_ROWS:
        log("ablt: %d records > %d: needs the runtime's ablt hook (roster.cpp, ROSTER_ABLT), or the game's copy overflows"
            % (struct.unpack_from("<I", ablt, 8)[0], ABLT_GAME_ROWS))
    if objs:
        rowmap, _ = add_objs(by, orecs, objs, log, objs_char)
        if rowmap_out is not None:
            rowmap_out.update(rowmap)
    if names and texts is not None:
        apply_ability_names(aents, chars, names, texts, log)
    if names:
        set_learn_levels(aents, chars, names, log)
    if names:                                    # last: the naming above looks records up by owner and command
        disable_abilities(aents, chars, names, log)
    chrp[2] =bytes(chd[:4]) + struct.pack("<I", len(recs)) + b"".join(bytes(r) for r in recs)
    obj[2] = bytes(ohead) + struct.pack("<I", len(orecs)) + b"".join(bytes(r) for r in orecs)
    by["system.bin"][2] = arc_pack(ahead, aents)
    return mpk_pack(head, ents)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = {}
    flags = {"check"}
    i = 0
    while i < len(argv):
        if argv[i].startswith("--"):
            k = argv[i][2:]
            if k in flags:
                opt[k] = True
                i += 1
            else:
                opt[k] = argv[i + 1]
                i += 2
        else:
            i += 1
    args = [a for a in args if a not in opt.values()]
    if len(args) < 2 and not (opt.get("check") and args):
        print(__doc__)
        return 2
    pk = pe.Package(args[0])
    data = bytes(pk.read_crc(MAIN_CRC))
    if opt.get("check"):
        head, ents = mpk_unpack(data)
        by = {e[0]: e for e in ents}
        ahead, aents = arc_unpack(by["system.bin"][2])
        ok_arc = arc_pack(ahead, aents) == by["system.bin"][2]
        ok_mpk = mpk_pack(head, ents) == data
        print("ARC round trip:", "identical" if ok_arc else "DIFFERS")
        print("MPK round trip:", "identical" if ok_mpk else "DIFFERS")
        t = AbilityTexts(pk.read)                    # exits if a text table does not round trip
        print("ability texts round trip: identical (%s; %d names, %d help texts)" % (
            " ".join(t.langs), t.retail["name"], t.retail["help"]))
        return 0 if ok_arc and ok_mpk else 1
    rmax = int(opt.get("max", ROSTER_MAX))
    roster = os.path.join(args[1], "roster.txt")
    objs, objs_char = (parse_objs(opt["objs"]), int(opt["char"]) if "char" in opt else None) if "objs" in opt \
        else (read_roster_objs(roster) if os.path.exists(roster) else ([], None))
    if "objs" in opt and "char" not in opt and not os.path.exists(roster):
        chars = []                                   # objects only: no new character
    elif "char" in opt or not os.path.exists(roster):
        chars = [(int(opt.get("char", FIRST_NEW)), opt.get("code"), int(opt.get("like", 9)))]
    else:
        chars = read_roster(roster, int(opt.get("first", FIRST_NEW)))
        if "code" in opt and len(chars) == 1:
            chars = [(chars[0][0], opt["code"], chars[0][2])]
    rowmap = {}
    names, texts = load_names_and_texts(pk.read, opt.get("names"), required="names" in opt)
    out = build(data, chars, rmax, min_id=int(opt.get("first", FIRST_NEW)), objs=objs, objs_char=objs_char, rowmap_out=rowmap,
                names=names, texts=texts)
    text_files = texts.files(print) if texts else {}  # before anything is written: exits if a table no longer fits
    path = os.path.join(args[1], "by_crc", "%08x.bin" % MAIN_CRC)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(out)
    print("wrote", path, len(out), "bytes (was %d)" % len(data))
    for rel, blob in sorted(text_files.items()):     # main.bin's new text ids need these files: install them together
        p = os.path.join(args[1], rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(blob)
        print("wrote", p, len(blob), "bytes (ability names / help texts)")
    if "repoint" in opt:                         # --repoint IN.objx:OUT.objx (the body objx of the objs= character)
        src, dst = opt["repoint"].split(":", 1)
        open(dst, "wb").write(repoint_objx(open(src, "rb").read(), rowmap))
        print("wrote", dst)
    import bgm_extra                              # <mods>/bgm.txt: extra BGM rows go on top of the new main.bin
    bgm_extra.install(pk, args[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
