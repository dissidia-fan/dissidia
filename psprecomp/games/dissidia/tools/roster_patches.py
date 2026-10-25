#!/usr/bin/env python3
"""Generate the roster [[patches]] of overlay_menu.toml / overlay_battle.toml from ROSTER_MAX.

    roster_patches.py [--write] [--check] [--verify <DATA dir with OVL_*_APP.ELF>]

Reads runtime/roster_max.h (DISSIDIA_ROSTER_MAX, DISSIDIA_RETAIL_SLOTS and the relocated table bases) and
emits the instruction patches that make the overlays use it:

  * grid bounds: three `slti` immediates in the Quick Battle grid code (FUN_08A7F2B4, FUN_08A80130,
    FUN_08A84FD4) bound the record index; they become 33 + ROSTER_MAX - 35 (41 for 43).
  * the charId guard (`slti 0x23`) of the relocated name table lookup becomes ROSTER_MAX.
  * relocated tables: each 35-slot string/record table has exactly one `lui hi` + `addiu lo` reference in
    its overlay; both words are rewritten to point at a ROSTER_MAX-slot copy in guest scratch RAM that
    roster.cpp fills when the overlay is streamed in.

The block between `# BEGIN ROSTER_MAX patches` and `# END ROSTER_MAX patches` in each toml is replaced
(--write) or compared (--check, exit 1 if it differs). Without flags the block is printed.
After changing a toml, regenerate that overlay (tools/build_overlay.py / pipeline.sh). Standard library only.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.join(HERE, "..")
BEGIN, END = "# BEGIN ROSTER_MAX patches (tools/roster_patches.py)", "# END ROSTER_MAX patches"


def defines():
    d = {}
    for line in open(os.path.join(GAME, "runtime", "roster_max.h")):
        m = re.match(r"#define\s+(DISSIDIA_\w+)\s+(0x[0-9A-Fa-f]+|\d+)", line)
        if m:
            d[m.group(1)] = int(m.group(2), 0)
    return d


def lui(word, hi):
    return (word & 0xFFFF0000) | (hi & 0xFFFF)


OVL_BASE, OVL_HDR = 0x089DD4C0, 0x54
ELF_NAMES = {"menu": "OVL_MENU_APP.ELF", "battle": "OVL_BATTLE_APP.ELF", "extra": "OVL_EXTRA_APP.ELF"}
_verify_dir = None   # set by --verify <dir holding the OVL_*_APP.ELF files>
PROBLEMS = []


def elf_word(overlay, addr):
    """The original instruction word at `addr` (ELF virtual address = analysis address = recompiled address).
    NOTE: the overlay is loaded at OVL_BASE from file offset OVL_HDR (0x54), so addr -> file offset addr-OVL_BASE+0x54.
    An address taken from a raw file offset (without the 0x54) points 21 instructions too early."""
    with open(os.path.join(_verify_dir, ELF_NAMES[overlay]), "rb") as f:
        f.seek(addr - OVL_BASE + OVL_HDR)
        return int.from_bytes(f.read(4), "little")


# Class (b) of docs/roster.md "slti 0x23 audit": `slti rd, rs, 0x23` charId validity checks (charId < 35) in front of code that only
# calls the hooked save-record accessors or stores/compares the id. (address, original instruction word); only the immediate changes.
CHARID_BOUND_SITES = [
    (0x08A1BE74, 0x28840023), (0x08A4CB10, 0x28840023), (0x08A6A0C8, 0x28440023), (0x08A6A930, 0x2A250023),
    (0x08A6AA68, 0x2A250023), (0x08A6ADDC, 0x28A50023), (0x08A8032C, 0x2A040023), (0x08A80438, 0x28850023),
    (0x08A804C8, 0x28850023), (0x08A816F0, 0x28850023), (0x08A81760, 0x28850023), (0x08AB1CE8, 0x28850023),
    (0x08B36268, 0x2A040023), (0x08B38720, 0x28840023), (0x08B388BC, 0x2A840023), (0x08B3A758, 0x2AC40023),
    (0x08B3AB08, 0x28850023), (0x08B409A8, 0x28E70023), (0x08B41024, 0x28E70023),
]


def block(overlay, d):
    rmax, retail = d["DISSIDIA_ROSTER_MAX"], d["DISSIDIA_RETAIL_SLOTS"]
    rows = 33 + rmax - d["DISSIDIA_FIRST_NEW"]
    out = []

    def patch(addr, word, reason):
        if _verify_dir:   # a patch only rewrites the 16 bit immediate: opcode and registers must match the real instruction
            ow = elf_word(overlay, addr)
            if (ow & 0xFFFF0000) != (word & 0xFFFF0000):
                PROBLEMS.append("%s 0x%08X: patch word 0x%08X does not match the ELF word 0x%08X" % (overlay, addr, word, ow))
        out.append('[[patches]]\naddress = "0x%08X"\ninstruction = "0x%08X"\nreason = "%s"\n' % (addr, word, reason))

    def reloc(lui_addr, lui_word, add_addr, add_word, base, what):
        assert (base >> 16) == 0x0870 and (base & 0xFFFF) < 0x8000, "relocated bases need hi 0x0870"
        patch(lui_addr, lui(lui_word, base >> 16), "%s: table base -> 0x%08X (hi)" % (what, base))
        patch(add_addr, lui(add_word, base & 0xFFFF), "%s: table base -> 0x%08X (lo)" % (what, base))

    if overlay == "menu":
        # original words: slti v1,t3,0x21 / slti a0,s0,0x21 / slti a0,s4,0x20
        patch(0x08A7F30C, 0x29630000 | rows, "FUN_08A7F2B4: grid record idx < %d (ROSTER_MAX %d)" % (rows, rmax))
        patch(0x08A8019C, 0x2A040000 | rows, "FUN_08A80130: grid record idx < %d" % rows)
        patch(0x08A85100, 0x2A840000 | rows, "FUN_08A84FD4: record scan loop < %d" % rows)
        # the name table lookup is guarded by `slti a0,s2,0x23` (charId < 35)
        patch(0x08A541E8, 0x2A440000 | rmax, "charId -> name text record guard: charId < %d" % rmax)
        for addr, word in CHARID_BOUND_SITES:
            patch(addr, (word & 0xFFFF0000) | rmax, "charId validity check: charId < %d (class b, docs/roster.md)" % rmax)
        # original lui/addiu words of the single reference of each table
        reloc(0x089E9F18, 0x3C05089F, 0x089E9F20, 0x24A5A3C0, d["DISSIDIA_TBL_MENU_A"], "arcade last-chara gim (FUN_089E9EB4)")
        reloc(0x089EA528, 0x3C05089F, 0x089EA530, 0x24A5C568, d["DISSIDIA_TBL_MENU_B"], "arcade select gim (FUN_089EA4D0)")
        # VS screen / battle prep face-in names (FUN_08B558F0): `lui a3,0x8b5` / `addiu s6,a3,0x7fb8`
        reloc(0x08B55AF4, 0x3C0708B5, 0x08B55B00, 0x24F67FB8, d["DISSIDIA_TBL_VS"], "VS screen face-in names (FUN_08B558F0)")
        reloc(0x08A541F4, 0x3C0508A5, 0x08A541FC, 0x24A544E0, d["DISSIDIA_TBL_NAME"], "charId -> name text record")
    else:
        reloc(0x08AF6D00, 0x3C1708B0, 0x08AF6D18, 0x26F7BC68, d["DISSIDIA_TBL_BATTLE"], "story/encounter face-in names (FUN_08AF6B70)")
        # Opponent Info name text ids: {text id, alt id} per charId, two references (lui a1,0x8b0 / addiu a1,a1,-0x4918)
        reloc(0x08AF90F0, 0x3C0508B0, 0x08AF90F4, 0x24A5B6E8, d["DISSIDIA_TBL_OPPINFO"], "Opponent Info name text (FUN_08AF8F58)")
        reloc(0x08AF9DB0, 0x3C0508B0, 0x08AF9DB8, 0x24A5B6E8, d["DISSIDIA_TBL_OPPINFO"], "Opponent Info name text (FUN_08AF9C6C)")
    return BEGIN + "\n" + "\n".join(out) + END + "\n"


def main(argv):
    global _verify_dir
    argv = list(argv)
    if "--verify" in argv:
        i = argv.index("--verify")
        _verify_dir = argv[i + 1]
        del argv[i:i + 2]
    d = defines()
    rc = 0
    for ov in ("menu", "battle"):
        path = os.path.join(GAME, "overlay_%s.toml" % ov)
        new = block(ov, d)
        if not ("--write" in argv or "--check" in argv):
            print("# overlay_%s.toml\n%s" % (ov, new))
            continue
        txt = open(path).read()
        if BEGIN in txt:
            a, b = txt.index(BEGIN), txt.index(END) + len(END) + 1
            cur = txt[a:b]
            rest = txt[:a] + "%%BLOCK%%" + txt[b:]
        else:
            cur, rest = "", txt.rstrip("\n") + "\n\n%%BLOCK%%"
        if "--check" in argv:
            if cur != new:
                print("overlay_%s.toml: roster block is out of date" % ov)
                rc = 1
        else:
            open(path, "w").write(rest.replace("%%BLOCK%%", new))
            print("wrote", path)
    if PROBLEMS:
        print("\n".join(PROBLEMS))
        rc = 1
    elif _verify_dir:
        print("all patch addresses verified against the ELFs")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
