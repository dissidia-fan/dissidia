#!/usr/bin/env python3
"""Self-test for roster_mainbin.py on a synthetic main.bin (no game files): chrp / objentry clones and the abilities
of a new charId (system.bin 'ablt', 'chrl', 'amas'), and their own names / help texts (main_lang.bin 'ability.bin',
the help pack, mess_text's OFF4 layout).
Run: python3 -I test_roster_mainbin.py"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import menu_pack as MP  # noqa: E402
import mess_text as MT  # noqa: E402
import roster_mainbin as R  # noqa: E402


def arc(ents):
    return R.arc_pack(b"ARC\x01", [[tag, 1, blob] for tag, blob in ents])


def chrp():
    recs = b""
    for cid, code in ((1, "ONE100"), (9, "FIV100"), (21, "GST100"), (34, "ORG800")):
        r = bytearray(R.CHRP_REC)
        r[0] = cid
        r[0x74:0x7C] = ("P_" + code).encode().ljust(8, b"\0")
        recs += bytes(r)
    return struct.pack("<II", 0x21, 4) + recs


def objentry():
    recs = b""
    for oid, cid, typ, name in ((1, 9, 0, "P_FIV100_1P"), (2, 9, 1, "P_FIV100_1P_A"), (3, 34, 0, "P_ORG800_1P")):
        r = bytearray(R.OBJ_REC)
        struct.pack_into("<H", r, 0, oid)
        r[6], r[8] = cid, typ
        r[0x24:0x34] = name.encode().ljust(16, b"\0")
        stem = name.lower().rsplit("_1p", 1)[0] + name.lower().rsplit("_1p", 1)[1]
        r[0x34:0x44] = stem.encode().ljust(16, b"\0")
        recs += bytes(r)
    return b"OBJE" + struct.pack("<I", 3) + recs


def ability(aid, owner, refs=(0xFFFF, 0xFFFF), cmd=0, cp=15):
    r = bytearray(R.ABLT_REC)
    struct.pack_into("<HHH", r, 0, aid, 0x100 + (aid & 0xFF), 0x200 + (aid & 0xFF))
    struct.pack_into("<HH", r, 6, *refs)
    struct.pack_into("<H", r, 12, cmd)
    struct.pack_into("<H", r, 16, cp)
    r[0x16] = 1 if aid & 0x08 else 0           # HP / Brave
    r[R.ABLT_OWNER] = owner
    return bytes(r)


ABLT = [ability(0x0101, 1, cmd=0x10), ability(0x0902, 9, cmd=0xA4), ability(0x0906, 9, cmd=0xAB),
        ability(0x0907, 9, refs=(0x0906, 0xFFFF), cmd=0xAC), ability(0x090C, 9, refs=(0xFFFF, 0x0902), cmd=0xA2),
        ability(0x1500, 21, cmd=0x00), ability(0x2100, 33, cmd=0x3A5), ability(0x2200, 34, cmd=0x3F9),
        ability(0x4025, 0xFF, cmd=0xFFFF), ability(0x40AA, 0xFF, cmd=0xFFFF)]


def ablt(recs):
    return struct.pack("<4I", 0x20494241, 0x20100611, len(recs), 0) + b"".join(recs)


def level(magic, rows):
    """rows: {charId: [8-byte entries]} -> 'chrl' / 'amas'"""
    return R.level_table_bytes(struct.pack("<4I", magic, 0x20090409, 0, 0), {c: (0, e) for c, e in rows.items()})


def chrl_entry(lv, flags, *ids):
    ids = list(ids) + [0xFFFF] * (3 - len(ids))
    return struct.pack("<BB3H", lv, flags, *ids)


def amas_entry(aid, prereq):
    return struct.pack("<4H", aid, 1, prereq, 0xFFFF)


CHRL = {1: [chrl_entry(1, 3, 0x4025, 0x40AA), chrl_entry(4, 0, 0x0101)],
        9: [chrl_entry(1, 3, 0x4025, 0x40AA), chrl_entry(4, 0, 0x0902), chrl_entry(9, 0, 0x0906, 0x4025)],
        21: [chrl_entry(4, 0, 0x1500)]}
AMAS = {1: [amas_entry(0x4004, 0x4003)], 9: [amas_entry(0x0907, 0x0906), amas_entry(0x4004, 0x4003)]}


def main_bin():
    sysb = arc([("chrp", chrp()), ("ablt", ablt(ABLT)), ("chrl", level(0x56454C43, CHRL)),
                ("amas", level(0x53414D41, AMAS))])
    return R.mpk_pack(b"MPK \x06\x06\x07\x20", [["battle.bin", 0, b"xx"], ["objentry.bin", 0, objentry()],
                                                ["system.bin", 0, sysb]])


def tables(data):
    _h, ents = R.mpk_unpack(data)
    _a, aents = R.arc_unpack(next(e for e in ents if e[0] == "system.bin")[2])
    return {e[0]: e[2] for e in aents}


def ablt_records(blob):
    n = struct.unpack_from("<I", blob, 8)[0]
    assert len(blob) == 16 + R.ABLT_REC * n
    return {struct.unpack_from("<H", blob, 16 + R.ABLT_REC * i)[0]: blob[16 + R.ABLT_REC * i: 16 + R.ABLT_REC * (i + 1)]
            for i in range(n)}


def test_level_table_round_trip():
    for magic, rows in ((0x56454C43, CHRL), (0x53414D41, AMAS)):
        blob = level(magic, rows)
        head, got = R.parse_level_table(blob, "t")
        assert R.level_table_bytes(head, got) == blob
        assert struct.unpack_from("<I", blob, 8)[0] == len(rows)
        assert struct.unpack_from("<I", blob, 16 + 8)[0] == 16 + 16 * len(rows)   # first entries follow the index


def test_abilities():
    base = main_bin()
    log = []
    out = R.build(base, [(34, "noc100", 9)], log=log.append, min_id=33)
    old, new = tables(base), tables(out)
    a_old, a_new = ablt_records(old["ablt"]), ablt_records(new["ablt"])
    ids = [struct.unpack_from("<H", new["ablt"], 16 + R.ABLT_REC * i)[0] for i in range(len(a_new))]
    assert ids == sorted(ids), ids
    assert 0x2200 not in a_new                                  # ORG800's own record is gone
    assert len(a_new) == len(a_old) - 1 + 4                     # + Bartz's 4 records
    for aid, rec in a_old.items():                              # every other record is unchanged
        if aid >> 8 != 34:
            assert a_new[aid] == rec, hex(aid)
    for n in (0x02, 0x06, 0x07, 0x0C):
        src, got = a_old[0x0900 | n], a_new[0x2200 | n]
        assert got[R.ABLT_OWNER] == 34
        assert got[12:14] == src[12:14] and got[16:18] == src[16:18] and got[0x16] == src[0x16]   # command, CP, type
        assert got[2:6] == src[2:6]                             # names stay Bartz's
        diff = [i for i in range(R.ABLT_REC) if got[i] != src[i]]
        assert set(diff) <= {1, 7, 9, R.ABLT_OWNER}, (hex(n), diff)
    assert struct.unpack_from("<H", a_new[0x2207], 6)[0] == 0x2206   # prerequisite moved along
    assert struct.unpack_from("<H", a_new[0x220C], 8)[0] == 0x2202   # base ability moved along

    for tag, magic in (("chrl", 0x56454C43), ("amas", 0x53414D41)):
        _h, rows_old = R.parse_level_table(old[tag], tag)
        _h, rows_new = R.parse_level_table(new[tag], tag)
        assert sorted(rows_new) == sorted(list(rows_old) + [34])
        for cid in rows_old:
            assert rows_new[cid] == rows_old[cid]
        assert len(rows_new[34][1]) == len(rows_old[9][1])
        assert new[tag][:4] == struct.pack("<I", magic)
    lv = R.parse_level_table(new["chrl"], "chrl")[1][34][1]
    assert lv[0] == chrl_entry(1, 3, 0x4025, 0x40AA)            # shared abilities stay shared
    assert lv[1] == chrl_entry(4, 0, 0x2202) and lv[2] == chrl_entry(9, 0, 0x2206, 0x4025)
    am = R.parse_level_table(new["amas"], "amas")[1][34][1]
    assert am == [amas_entry(0x2207, 0x2206), amas_entry(0x4004, 0x4003)]
    assert any(line.startswith("ablt: dropped charId 34") for line in log), log

    # charId 33 (the retail SEV120 slot) loses its four leftover records to the copies of its like (Shantotto)
    out33 = R.build(base, [(33, "viv100", 21)], log=lambda *_: None, min_id=33)
    a33 = ablt_records(tables(out33)["ablt"])
    assert a33[0x2100][R.ABLT_OWNER] == 33 and a33[0x2100][12:14] == a_old[0x1500][12:14]
    assert 0x2200 in a33                                        # not charId 33's
    # building again from a built main.bin gives the same tables (rows / records are replaced, not added twice)
    _h, ents = R.mpk_unpack(out)
    _a, aents = R.arc_unpack(next(e for e in ents if e[0] == "system.bin")[2])
    R.add_abilities(aents, 34, 9, log=lambda *_: None)
    assert {e[0]: e[2] for e in aents}["ablt"] == new["ablt"]
    assert {e[0]: e[2] for e in aents}["chrl"] == new["chrl"] and {e[0]: e[2] for e in aents}["amas"] == new["amas"]


def test_game_row_limit():
    many = [ability(0x0100 | i, 1) for i in range(256)] + [ability(0x0200 | i, 2) for i in range(256)] + \
           [ability(0x0300 | i, 3) for i in range(256)] + [ability(0x0900 | i, 9) for i in range(40)]
    _h, ents = R.mpk_unpack(main_bin())
    sysb = next(e for e in ents if e[0] == "system.bin")
    _a, aents = R.arc_unpack(sysb[2])
    next(e for e in aents if e[0] == "ablt")[2] = ablt(many)
    sysb[2] = R.arc_pack(b"ARC\x01", aents)
    log = []
    out = R.build(R.mpk_pack(b"MPK \x06\x06\x07\x20", ents), [(34, "noc100", 9)], log=log.append, min_id=33)
    assert len(ablt_records(tables(out)["ablt"])) == 808 + 40 > R.ABLT_GAME_ROWS
    assert any("needs the runtime's ablt hook" in line for line in log), log


BONUS = "Minor \x10\u0101\uff81Speed Boost\x1b effect."
CHASE = R.EFFECT_PREFIX + "Additional Effect: chase\x1b"


def text_package(n_names=0x110, n_helps=0x210, full=False):
    """read(path) of a package holding en/main_lang.bin (MPK, 'ability.bin' = names) and the en help pack (entry 0 plain
    help texts, entry 1 OFF4). The synthetic 'ablt' records use name 0x100 + n, help 0x200 + n. full: padding help records
    until the plain layout holds the table but not two more help texts (like the retail help table)."""
    names = ["name %d" % i for i in range(n_names)]
    helps = ["help %d" % i for i in range(n_helps)]
    helps[0x20C] = "[Close] Slash with two swords.\n" + BONUS + "\n" + CHASE
    while full and MT.plain_fits(helps + ["x" * 200, "", ""]):
        helps.append("x" * 200)
    while full and MT.plain_fits(helps + ["", ""]):             # now the table fits, two more records do not
        helps.append("")
    lang = R.mpk_pack(b"MPK \x06\x06\x07\x20", [["common.bin", 0, MT.build(["a", "b"])], ["ability.bin", 0, MT.build(names)]])
    pack = MP.build({"magic": b"mess pak", "ver": 1, "base": 0}, [MT.build(helps), MT.build_off4(["detail 0", "detail 1"])])
    files = {R.LANG_ARCHIVE % "en": lang, R.HELP_PACK % "en": pack}
    return lambda path: files[path]


NAME_MAP = {"noc100": {0x0C: {"cmd": "0xA2", "name": "Blitz Attack {ground}",
                              "help": "[Close] Leaping combo.\n{bonus}\n{effect:wall rush}"},
                       0x02: {"cmd": "0xA4"},                                     # kept: like's texts
                       0x06: {"cmd": "0x123", "name": "Wrong moveset"},           # command differs: skipped
                       0x07: {"cmd": "0xAC", "name": "Blitz Attack {ground}", "help": "{effects}"}}}


def test_mess_text_off4():
    texts = ["", "a", "ab", "abc\nd", "\x10\u0141\uff41x\x1b"]
    for t in (texts, texts[:4]):
        e = MT.build_off4(t)
        assert e[:4] == MT.OFF4 and MT.records(e) == t and MT.records(MT.build(t)) == t
        assert len(e) % 4 == 0
    assert MT.plain_fits(["x"] * 10) and MT.build_fit(["x"] * 10) == MT.build(["x"] * 10)
    big = ["y" * 1000] * 70                                      # starts beyond 64 Ki words: plain cannot hold it
    assert not MT.plain_fits(big) and MT.build_fit(big)[:4] == MT.OFF4 and MT.records(MT.build_fit(big)) == big


def built_texts(files):
    lang = R.mpk_unpack(files[R.LANG_ARCHIVE % "en"])[1]
    pack = MP.parse(files[R.HELP_PACK % "en"])
    return MT.records(next(e[2] for e in lang if e[0] == "ability.bin")), MT.records(MP.entry(pack, 0)), pack, lang


def test_ability_names():
    base = main_bin()
    plain = R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33)
    for full in (False, True):                                   # the help table fits the plain layout / it does not
        read = text_package(full=full)
        texts = R.AbilityTexts(read)
        log = []
        out = R.build(base, [(34, "noc100", 9)], log=log.append, min_id=33, names=NAME_MAP, texts=texts)
        a_old, a_new = ablt_records(tables(plain)["ablt"]), ablt_records(tables(out)["ablt"])
        changed = [aid for aid in a_new if a_new[aid] != a_old[aid]]
        assert changed == [0x2207, 0x220C], [hex(x) for x in changed]
        for aid in changed:                                      # only the two text ids differ
            assert a_new[aid][:2] == a_old[aid][:2] and a_new[aid][6:] == a_old[aid][6:]
        assert any("0x123" in line and "left as is" in line for line in log), log
        files = texts.files()
        names, helps, pack, lang = built_texts(files)
        ret_names = ["name %d" % i for i in range(0x110)]
        n0, n1 = texts.retail["name"], texts.retail["help"]
        nid7, hid7 = struct.unpack_from("<HH", a_new[0x2207], 2)   # records are visited in id order: 0x2207 first
        assert (nid7, hid7) == (n0, n1) and helps[hid7] == ""   # its like's help has no effect line
        nid, hid = struct.unpack_from("<HH", a_new[0x220C], 2)
        assert (nid, hid) == (n0, n1 + 1)                       # the same name is stored once
        assert names[nid] == "Blitz Attack \x11P(ground)\x11d" and names[:n0] == ret_names and len(names) == n0 + 1
        assert helps[hid] == "[Close] Leaping combo.\n" + BONUS + "\n" + R.EFFECT_PREFIX + "Additional Effect: wall rush\x1b"
        assert helps[:n1] == R.AbilityTexts(read).helps["en"]    # retail help records unchanged
        assert MP.entry(pack, 1) == MT.build_off4(["detail 0", "detail 1"])
        assert next(e[2] for e in lang if e[0] == "common.bin") == MT.build(["a", "b"])
        layout = MP.entry(pack, 0)[:4] == MT.OFF4
        assert layout == full, (full, layout)
    # helpers
    assert R.expand_help("{like}", "a\nb") == "a\nb"
    try:
        R.expand_help("x" * 36, "")
        raise AssertionError("a 36-character help line must be refused")
    except SystemExit:
        pass
    names, texts = R.load_names_and_texts(lambda p: (_ for _ in ()).throw(KeyError(p)))   # no text tables: no names
    assert (names, texts) == (None, None)


def test_ability_kind():
    """'type' / 'phase' / 'after' in the name map set the record's +0x16 / +0x17 / +6 (Bartz's HP follow-up Flare 0x0907,
    which follows Holy 0x0906, made a midair brave move of its own); nothing else in the record changes."""
    base = main_bin()
    plain = R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33)
    kind = {"noc100": {0x07: {"cmd": "0xAC", "type": "brave", "phase": "midair", "after": None},
                       0x06: {"cmd": "0xAB", "type": "HP", "after": "0x2202"},
                       0x02: {"cmd": "0x123", "type": "hp"}}}                      # command differs: skipped
    log = []
    out = R.build(base, [(34, "noc100", 9)], log=log.append, min_id=33, names=kind, texts=R.AbilityTexts(text_package()))
    a_old, a_new = ablt_records(tables(plain)["ablt"]), ablt_records(tables(out)["ablt"])
    assert [aid for aid in a_new if a_new[aid] != a_old[aid]] == [0x2206, 0x2207]
    r7 = a_new[0x2207]
    assert r7[R.ABLT_TYPE] == 0 and r7[R.ABLT_PHASE] == 1 and struct.unpack_from("<H", r7, R.ABLT_AFTER)[0] == 0xFFFF
    assert struct.unpack_from("<H", a_old[0x2207], R.ABLT_AFTER)[0] == 0x2206          # it did follow 0x2206
    r6 = a_new[0x2206]
    assert r6[R.ABLT_TYPE] == 1 and r6[R.ABLT_PHASE] == a_old[0x2206][R.ABLT_PHASE] and struct.unpack_from("<H", r6, 6)[0] == 0x2202
    for aid in (0x2206, 0x2207):                                  # only those bytes
        diff = [i for i in range(R.ABLT_REC) if a_new[aid][i] != a_old[aid][i]]
        assert set(diff) <= {R.ABLT_AFTER, R.ABLT_AFTER + 1, R.ABLT_TYPE, R.ABLT_PHASE}, (hex(aid), diff)
    assert a_new[0x2202] == a_old[0x2202]
    assert any("2207 type 0 phase 01 after FFFF" in line for line in log), log
    assert all(a_new[aid] == a_old[aid] for aid in a_new if aid >> 8 != 34)             # retail records untouched


def test_new_ability():
    """"copy" adds an ability of the character's own: Noctis's ground Warp-Strike style 0x2204 = a copy of 0x2207 with
    command 0x0B0, ground phase, learned at level 9 (a free id slot of the level-9 entry) and at level 2 (a new entry);
    every other record and row is unchanged; a missing source, an existing id or a missing cmd is refused."""
    base = main_bin()
    plain = R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33)
    m = {"noc100": {0x04: {"copy": "07", "cmd": "0x0B0", "phase": "ground", "learn": 9, "name": "New {ground}", "help": "[Mid] x"},
                    0x0D: {"copy": "02", "cmd": "0x0B1", "learn": 2}}}
    log = []
    texts = R.AbilityTexts(text_package())
    out = R.build(base, [(34, "noc100", 9)], log=log.append, min_id=33, names=m, texts=texts)
    a_old, a_new = ablt_records(tables(plain)["ablt"]), ablt_records(tables(out)["ablt"])
    ids = [struct.unpack_from("<H", tables(out)["ablt"], 16 + R.ABLT_REC * i)[0] for i in range(len(a_new))]
    assert ids == sorted(ids) and set(a_new) == set(a_old) | {0x2204, 0x220D}
    assert all(a_new[aid] == a_old[aid] for aid in a_old), [hex(a) for a in a_old if a_new[a] != a_old[a]]
    r4, r7 = a_new[0x2204], a_old[0x2207]
    assert struct.unpack_from("<H", r4, 12)[0] == 0x0B0 and r4[R.ABLT_PHASE] == 0 and r4[R.ABLT_OWNER] == 34
    diff = [i for i in range(R.ABLT_REC) if r4[i] != r7[i]]
    assert set(diff) <= {0, 1, 2, 3, 4, 5, 12, 13, R.ABLT_PHASE}, diff                # id, texts, command, phase only
    assert struct.unpack_from("<H", a_new[0x220D], 12)[0] == 0x0B1 and a_new[0x220D][2:6] == a_old[0x2202][2:6]   # no names: the copy's
    _h, rows = R.parse_level_table(tables(out)["chrl"], "chrl")
    _h, rows0 = R.parse_level_table(tables(plain)["chrl"], "chrl")
    lv = {e[0]: struct.unpack_from("<3H", e, 2) for e in rows[34][1]}
    assert lv[9] == (0x2206, 0x4025, 0x2204) and lv[2] == (0x220D, 0xFFFF, 0xFFFF), lv
    assert {c: r for c, r in rows.items() if c != 34} == {c: r for c, r in rows0.items() if c != 34}
    assert [e for e in rows[34][1] if e[0] not in (2, 9)] == [e for e in rows0[34][1] if e[0] != 9]
    assert any("new ability 2204 = copy of 2207 with command 0B0" in line for line in log), log
    names, _helps, _pack, _lang = built_texts(texts.files())
    assert names[struct.unpack_from("<H", r4, 2)[0]].startswith("New ")
    for bad in ({"copy": "0A", "cmd": "0xB0"}, {"copy": "07"}, {"copy": "07", "cmd": "0xB0", "_": 0}):
        k = 0x02 if bad.get("_") == 0 else 0x04                                     # 0x2202 exists already
        try:
            R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33, names={"noc100": {k: bad}},
                    texts=R.AbilityTexts(text_package()))
        except SystemExit:
            continue
        raise AssertionError("must be refused: %r" % bad)


def test_disabled_ability():
    """"disabled": true keeps the record (saves that learned / equipped it still find it) but makes it unlisted and
    inert: owner 0xFE, command -2; only those bytes change. Its 'chrl' entry goes (a level-4 entry that held only it is
    dropped), an 'amas' entry that unlocks it goes, one that needs it stays; nothing else changes. A disabled id
    without a record is refused; without the flag the build is the plain one."""
    base = main_bin()
    texts = R.AbilityTexts(text_package())
    plain = R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33, names={"noc100": {0x02: {"cmd": "0xA4"}}},
                    texts=texts)
    log = []
    m = {"noc100": {0x02: {"cmd": "0xA4", "disabled": True}, 0x07: {"cmd": "0xAC", "disabled": True},
                    0x06: {"cmd": "0xAB", "disabled": False}}}
    out = R.build(base, [(34, "noc100", 9)], log=log.append, min_id=33, names=m, texts=R.AbilityTexts(text_package()))
    a_old, a_new = ablt_records(tables(plain)["ablt"]), ablt_records(tables(out)["ablt"])
    assert set(a_new) == set(a_old)                                          # no record removed
    for aid in (0x2202, 0x2207):
        r = a_new[aid]
        assert r[R.ABLT_OWNER] == R.DISABLED_OWNER == 0xFE and struct.unpack_from("<h", r, R.ABLT_CMD)[0] == -2
        diff = [i for i in range(R.ABLT_REC) if r[i] != a_old[aid][i]]
        assert set(diff) <= {R.ABLT_CMD, R.ABLT_CMD + 1, R.ABLT_OWNER}, (hex(aid), diff)
    assert all(a_new[aid] == a_old[aid] for aid in a_new if aid not in (0x2202, 0x2207))
    t_old, t_new = tables(plain), tables(out)
    _h, rows0 = R.parse_level_table(t_old["chrl"], "chrl")
    _h, rows = R.parse_level_table(t_new["chrl"], "chrl")
    assert [e[0] for e in rows0[34][1]] == [1, 4, 9] and [e[0] for e in rows[34][1]] == [1, 9]   # lv 4 held only 0x2202
    assert {c: r for c, r in rows.items() if c != 34} == {c: r for c, r in rows0.items() if c != 34}
    _h, am0 = R.parse_level_table(t_old["amas"], "amas")
    _h, am = R.parse_level_table(t_new["amas"], "amas")
    assert [struct.unpack_from("<H", e, 0)[0] for e in am0[34][1]] == [0x2207, 0x4004]
    assert [struct.unpack_from("<H", e, 0)[0] for e in am[34][1]] == [0x4004]                  # unlocked 0x2207: dropped
    assert any("2202 disabled" in line for line in log) and any("2207 disabled" in line for line in log), log
    # 0x2206 is needed by the dropped amas entry only; with 0x2207 kept and 0x2206 disabled, the entry stays
    m2 = {"noc100": {0x06: {"cmd": "0xAB", "disabled": True}}}
    log2 = []
    out2 = R.build(base, [(34, "noc100", 9)], log=log2.append, min_id=33, names=m2, texts=R.AbilityTexts(text_package()))
    _h, am2 = R.parse_level_table(tables(out2)["amas"], "amas")
    assert am2[34][1] == am0[34][1] and any("needs a disabled ability" in line for line in log2), log2
    _h, ch2 = R.parse_level_table(tables(out2)["chrl"], "chrl")
    assert [struct.unpack_from("<3H", e, 2) for e in ch2[34][1] if e[0] == 9] == [(0xFFFF, 0x4025, 0xFFFF)]
    try:
        R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33, names={"noc100": {0x0D: {"disabled": True}}},
                texts=R.AbilityTexts(text_package()))
    except SystemExit:
        pass
    else:
        raise AssertionError("a disabled id without a record must be refused")


def test_ability_cost_and_learn():
    """"cp" / "cp_mastered" / "ap" / "bonus" set an ability's +0x0E / +0x10 / +0x12 / +0x14 (mastered CP defaults to half);
    "learn": N on an ability the like has moves it to level N in the character's 'chrl' row (out of its old entry)."""
    base = main_bin()
    plain = R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33)
    m = {"noc100": {0x06: {"cp": 35, "ap": 260, "bonus": None}, 0x07: {"cp": 20, "cp_mastered": 20, "bonus": "7"},
                    0x02: {"learn": 3}}}
    log = []
    out = R.build(base, [(34, "noc100", 9)], log=log.append, min_id=33, names=m, texts=R.AbilityTexts(text_package()))
    a_old, a_new = ablt_records(tables(plain)["ablt"]), ablt_records(tables(out)["ablt"])
    assert struct.unpack_from("<4h", a_new[0x2206], R.ABLT_CP) == (35, 17, 260, -1)
    r7 = struct.unpack_from("<4h", a_new[0x2207], R.ABLT_CP), struct.unpack_from("<4h", a_old[0x2207], R.ABLT_CP)
    assert r7[0][:2] == (20, 20) and r7[0][2] == r7[1][2] and r7[0][3] == 7, r7
    for aid in (0x2206, 0x2207):
        diff = [i for i in range(R.ABLT_REC) if a_new[aid][i] != a_old[aid][i]]
        assert set(diff) <= set(range(R.ABLT_CP, R.ABLT_BONUS + 2)), (hex(aid), diff)
    assert all(a_new[aid] == a_old[aid] for aid in a_new if aid not in (0x2206, 0x2207))
    _h, rows = R.parse_level_table(tables(out)["chrl"], "chrl")
    _h, rows0 = R.parse_level_table(tables(plain)["chrl"], "chrl")
    where = lambda ents, aid: [e[0] for e in ents if aid in struct.unpack_from("<3H", e, 2)]
    assert where(rows[34][1], 0x2202) == [3], (where(rows0[34][1], 0x2202), where(rows[34][1], 0x2202))
    assert {c: r for c, r in rows.items() if c != 34} == {c: r for c, r in rows0.items() if c != 34}
    others = lambda ents: sorted((e[0], i) for e in ents for i in struct.unpack_from("<3H", e, 2) if i not in (0xFFFF, 0x2202))
    assert others(rows[34][1]) == others(rows0[34][1])
    assert any("learns 2202 at level 3" in line for line in log), log
    try:
        R.build(base, [(34, "noc100", 9)], log=lambda *_: None, min_id=33, names={"noc100": {0x02: {"learn": 0}}}, texts=R.AbilityTexts(text_package()))
    except SystemExit:
        pass
    else:
        raise AssertionError("learn level 0 must be refused")


if __name__ == "__main__":
    for t in (test_level_table_round_trip, test_abilities, test_game_row_limit, test_mess_text_off4, test_ability_names,
              test_ability_kind, test_new_ability, test_disabled_ability, test_ability_cost_and_learn):
        t()
        print("ok", t.__name__)
