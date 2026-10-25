#!/usr/bin/env python3
"""Self-test for ship_analysis.py using synthetic ELFs (no game data). Run: python3 -I test_ship_analysis.py"""
import base64, gzip, json, os, struct, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ship_analysis as S

BASE = 0x0B800000


def words(*w):
    return b"".join(struct.pack("<I", x) for x in w)


def build_elf(etype, vaddr, body, memsz_extra=0, relocs=()):
    """ET with one PT_LOAD at file offset 0x54 and (optionally) one 0x700000A0 reloc section."""
    rel = b"".join(struct.pack("<II", off, t | (0 << 8) | (0 << 16)) for off, t in relocs)
    data_end = 0x54 + len(body)
    rel_off = data_end
    shoff = rel_off + len(rel)
    shnum = 2 if relocs else 0
    hdr = bytearray(0x54)
    hdr[0:4] = b"\x7fELF"; hdr[4:7] = bytes([1, 1, 1])
    struct.pack_into("<HH", hdr, 16, etype, 8)
    struct.pack_into("<I", hdr, 28, 0x34)
    struct.pack_into("<I", hdr, 32, shoff if relocs else 0)
    struct.pack_into("<HHH", hdr, 40, 0x34, 0x20, 1)
    struct.pack_into("<HH", hdr, 46, 40, shnum)
    struct.pack_into("<8I", hdr, 0x34, 1, 0x54, vaddr, vaddr, len(body), len(body) + memsz_extra, 7, 4)
    out = bytes(hdr) + body + rel
    if relocs:
        out += b"\0" * 40 + struct.pack("<10I", 0, S.SHT_PSPREL_A, 0, 0, rel_off, len(rel), 0, 0, 4, 8)
    return out


def analysis_for(elf, vaddr, memsz, prx=False):
    a = {"binary_path": "/somewhere/else/X.ELF", "module_name": "x", "heap_base": "0x0",
         "functions": [{"name": "f", "address": "0x%08X" % vaddr, "size": 8}],
         "imports": [{"nid": "0x1"}],
         "segments": [{"p_vaddr": "0x%08X" % vaddr, "p_filesz": memsz, "p_memsz": memsz, "p_flags": 7,
                       "data_b64": ""}]}
    if prx:
        a["prx"] = {"load_base": "0x%08X" % BASE}
    return a


def main():
    d = tempfile.mkdtemp(prefix="shiptest_")
    w = lambda n, b, m="wb": (open(os.path.join(d, n), m).write(b), os.path.join(d, n))[1]

    # ---- plain ET_EXEC-style binary (overlay): hydrate is a straight copy, padded to memsz
    body = b"SECRETCODE" * 20
    elf = build_elf(0xffa8, 0x089DD4C0, body, memsz_extra=16)
    a = analysis_for(elf, 0x089DD4C0, len(body) + 16)
    a["segments"][0]["data_b64"] = base64.b64encode(body + b"\0" * 16).decode()
    src = w("a.json", json.dumps(a).encode())
    shipped = os.path.join(d, "a.json.gz")
    S.strip(src, shipped)
    raw = gzip.open(shipped).read()
    assert b"SECRETCODE" not in raw and base64.b64encode(b"SECRETCODE").decode()[:8].encode() not in raw
    assert b"data_b64" not in raw and b"/somewhere/else" not in raw
    bin_ = w("a.elf", elf)
    out = os.path.join(d, "a.hyd.json")
    h = S.hydrate(shipped, bin_, out)
    assert json.load(open(out)) == h
    assert base64.b64decode(h["segments"][0]["data_b64"]) == body + b"\0" * 16
    assert h["binary_path"] == bin_ and "ship_format" not in h and "data_sha256" not in h["segments"][0]
    assert open(shipped, "rb").read() == open(shipped, "rb").read()
    # reproducible bytes
    S.strip(src, shipped + "2"); assert open(shipped, "rb").read() == open(shipped + "2", "rb").read()

    # ---- different binary (other region/dump) is refused, nothing written
    other = w("b.elf", build_elf(0xffa8, 0x089DD4C0, body[:-1] + b"X", memsz_extra=16))
    try:
        S.hydrate(shipped, other, os.path.join(d, "never.json")); assert False
    except S.ShipError as e:
        assert "SHA-256" in str(e)
    assert not os.path.exists(os.path.join(d, "never.json"))
    # wrong segment layout / not an ELF / not a shipped file
    for bad in (build_elf(0xffa8, 0x089DD4C0, body, memsz_extra=32), b"notanelf" * 10):
        try: S.hydrate(shipped, w("c.elf", bad)); assert False
        except S.ShipError: pass
    try: S.hydrate(src, bin_); assert False
    except S.ShipError as e: assert "not a shipped analysis" in str(e)

    # ---- relocatable PRX: relocations applied exactly like psp-parser (hand-computed expectations)
    code = words(0x0000010,                       # 0x00 R_MIPS_32   -> +BASE
                 0x08000010,                      # 0x04 R_MIPS_26   -> +(BASE>>2)
                 0x3C040000, 0x24840010,          # 0x08/0x0C HI16/LO16 pair, no carry
                 0x3C050000, 0x24A58010,          # 0x10/0x14 HI16/LO16 pair, negative lo (address BASE-0x7FF0, hi stays 0x0B80)
                 0xDEADBEEF)                      # 0x18 untouched
    relocs = [(0, S.R_MIPS_32), (4, S.R_MIPS_26), (8, S.R_MIPS_HI16), (12, S.R_MIPS_LO16),
              (16, S.R_MIPS_HI16), (20, S.R_MIPS_LO16)]
    prx = build_elf(0xffa0, 0, code, relocs=relocs)
    expect = words(0x0B800010, 0x08000010 + (BASE >> 2) & 0x03FFFFFF | 0x08000000,
                   0x3C040B80, 0x24840010, 0x3C050B80, 0x24A58010, 0xDEADBEEF)
    a = analysis_for(prx, BASE, len(code), prx=True)
    a["segments"][0]["data_b64"] = base64.b64encode(expect).decode()
    src = w("p.json", json.dumps(a).encode())
    shipped = os.path.join(d, "p.json.gz"); S.strip(src, shipped)
    h = S.hydrate(shipped, w("p.prx", prx))
    assert base64.b64decode(h["segments"][0]["data_b64"]) == expect
    # without the prx marker the same bytes would not verify
    a.pop("prx"); src2 = w("p2.json", json.dumps(a).encode()); S.strip(src2, shipped + "x")
    try: S.hydrate(shipped + "x", w("p.prx", prx)); assert False
    except S.ShipError: pass

    # ---- CLI
    assert S.main(["hydrate", shipped, os.path.join(d, "p.prx"), os.path.join(d, "o.json")]) == 0
    assert S.main(["check", shipped, os.path.join(d, "a.elf")]) == 1
    print("ok")


if __name__ == "__main__":
    main()
