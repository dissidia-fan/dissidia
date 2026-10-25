#!/usr/bin/env python3
"""Strip / hydrate PSPRecomp analysis JSON so it can be shared without any game code in it.

`psprecomp analyze` writes each binary's PT_LOAD bytes into analysis*.json (`segments[].data_b64`, ~96% of the
file). That is a copy of the game's machine code and must not be redistributed. Everything else (function
boundaries and names, imported NIDs, module facts) is metadata derived by Ghidra. This tool lets a maintainer
publish only the metadata, and lets a user put the bytes back from their OWN, already-extracted binary:

  ship_analysis.py strip   <analysis.json> <out.json.gz> [--name NAME]
  ship_analysis.py hydrate <shipped.json.gz> <binary> <out.json>
  ship_analysis.py check   <shipped.json.gz> <binary>
  ship_analysis.py strip-all <workdir> <outdir>      # the five Dissidia analyses of a pipeline workdir

`strip` records the SHA-256 and length of every segment, then drops the bytes. `hydrate` rebuilds the segment
bytes from the binary (PT_LOAD data zero-padded to p_memsz; for relocatable PRX modules the Type-A relocations
are applied exactly as psp-parser does) and refuses to write anything unless every SHA-256 matches, so a
different region/revision/dump fails loudly instead of producing a subtly wrong recompile.

Python 3 standard library only.
"""
import argparse
import base64
import gzip
import hashlib
import json
import os
import struct
import sys

SHIP_FORMAT = 1
SHT_PSPREL_A = 0x700000A0
R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 0, 1, 2, 4, 5, 6


class ShipError(Exception):
    pass


# --------------------------------------------------------------------------- ELF

def load_segments(elf):
    if elf[:4] != b"\x7fELF" or len(elf) < 0x34:
        raise ShipError("not an ELF file")
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    segs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        t, off, va, _pa, fs, ms, _fl, _al = struct.unpack_from("<8I", elf, o)
        if t == 1:
            segs.append((off, va, fs, ms))
    return segs


def reloc_entries(elf):
    """Concatenated Type-A (sh_type 0x700000A0) relocation entries: (offset, type, ofs_base, addr_base)."""
    shoff = struct.unpack_from("<I", elf, 32)[0]
    shentsize, shnum = struct.unpack_from("<HH", elf, 46)
    out = []
    if not shoff:
        return out
    for i in range(shnum):
        _n, t, _f, _a, off, size, _l, _i, _al, _es = struct.unpack_from("<10I", elf, shoff + i * shentsize)
        if t == SHT_PSPREL_A:
            for j in range(off, off + size - size % 8, 8):
                o, info = struct.unpack_from("<II", elf, j)
                out.append((o, info & 0xF, (info >> 8) & 0xFF, (info >> 16) & 0xFF))
    return out


def apply_relocations(bufs, bases, entries):
    """Port of psp-parser reloc.rs apply_relocations (two-pass, PPSSPP-faithful)."""
    def s16(v):
        v &= 0xFFFF
        return v - 0x10000 if v & 0x8000 else v
    cached = []
    for off, rt, seg, _ab in entries:
        ok = seg < len(bufs) and off + 4 <= len(bufs[seg]) and (rt == R_MIPS_32 or (bases[seg] + off) & 3 == 0)
        cached.append(struct.unpack_from("<I", bufs[seg], off)[0] if ok else None)
    for i, (off, rt, seg, ab) in enumerate(entries):
        op = cached[i]
        if op is None:
            continue
        to = bases[ab] if ab < len(bases) else 0
        if rt == R_MIPS_NONE:
            continue
        if rt in (R_MIPS_16, R_MIPS_LO16):
            w = (op & 0xFFFF0000) | (((op & 0xFFFF) + to) & 0xFFFF)
        elif rt == R_MIPS_32:
            w = (op + to) & 0xFFFFFFFF
        elif rt == R_MIPS_26:
            w = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (to >> 2)) & 0x03FFFFFF)
        elif rt == R_MIPS_HI16:
            partner = None
            for t in range(i + 1, len(entries)):
                if entries[t][1] in (R_MIPS_LO16, R_MIPS_16):
                    partner = t
                    break
            if partner is None:
                w = op & 0xFFFF0000
            else:
                lo = s16(cached[partner] or 0)
                cur = (((op & 0xFFFF) << 16) + lo + to) & 0xFFFFFFFF
                hi16 = ((cur - (s16(cur) & 0xFFFFFFFF)) & 0xFFFFFFFF) >> 16
                w = (op & 0xFFFF0000) | (hi16 & 0xFFFF)
        else:
            continue
        struct.pack_into("<I", bufs[seg], off, w)


def segment_bytes(elf, analysis):
    """Rebuild each analysis segment's bytes from the ELF, in analysis order."""
    segs = load_segments(elf)
    jsegs = analysis["segments"]
    if len(segs) != len(jsegs):
        raise ShipError("binary has %d PT_LOAD segments, analysis expects %d" % (len(segs), len(jsegs)))
    bufs = []
    for (off, _va, fs, ms), js in zip(segs, jsegs):
        if ms != js["p_memsz"]:
            raise ShipError("segment size mismatch (binary p_memsz %d, analysis %d)" % (ms, js["p_memsz"]))
        bufs.append(bytearray(elf[off:off + fs]).ljust(ms, b"\0"))
    if analysis.get("prx"):
        bases = [int(js["p_vaddr"], 16) for js in jsegs]
        apply_relocations(bufs, bases, reloc_entries(elf))
    return bufs


# --------------------------------------------------------------------------- commands

def read_json(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    return json.loads(raw.decode("utf-8"))


def strip(src, dst, name=None):
    a = read_json(src)
    for s in a["segments"]:
        raw = base64.b64decode(s.pop("data_b64"))
        s["data_sha256"] = hashlib.sha256(raw).hexdigest()
        s["data_len"] = len(raw)
    a["binary_path"] = name or os.path.basename(a["binary_path"])
    a["ship_format"] = SHIP_FORMAT
    blob = json.dumps(a, separators=(",", ":")).encode("utf-8")
    with open(dst, "wb") as raw_out:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_out, compresslevel=9, mtime=0) as g:
            g.write(blob)                       # mtime 0 + no name: reproducible bytes
    return len(blob), os.path.getsize(dst)


def hydrate(shipped, binary, dst=None):
    a = read_json(shipped)
    if a.get("ship_format") != SHIP_FORMAT:
        raise ShipError("%s is not a shipped analysis (ship_format %r)" % (shipped, a.get("ship_format")))
    with open(binary, "rb") as f:
        elf = f.read()
    bufs = segment_bytes(elf, a)
    for i, (b, js) in enumerate(zip(bufs, a["segments"])):
        if hashlib.sha256(b).hexdigest() != js["data_sha256"]:
            raise ShipError(
                "segment %d of %s does not match the binary this analysis was made from (SHA-256 differs). "
                "This analysis is for the European PSN image (ULES01505); another region, revision or dump "
                "needs its own analysis: re-run pipeline.sh with --ghidra." % (i, binary))
        js["data_b64"] = base64.b64encode(bytes(b)).decode("ascii")
        del js["data_sha256"], js["data_len"]
    del a["ship_format"]
    a["binary_path"] = os.path.abspath(binary)
    if dst:
        tmp = dst + ".part"
        with open(tmp, "w") as f:
            json.dump(a, f, separators=(",", ":"))
        os.replace(tmp, dst)
    return a


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("strip"); s.add_argument("src"); s.add_argument("dst"); s.add_argument("--name")
    h = sp.add_parser("hydrate"); h.add_argument("shipped"); h.add_argument("binary"); h.add_argument("dst")
    sa = sp.add_parser("strip-all"); sa.add_argument("workdir"); sa.add_argument("outdir")
    c = sp.add_parser("check"); c.add_argument("shipped"); c.add_argument("binary")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "strip":
            n, z = strip(args.src, args.dst, args.name)
            print("%s: %d bytes json, %d gzipped" % (args.dst, n, z))
        elif args.cmd == "strip-all":
            os.makedirs(args.outdir, exist_ok=True)
            for n in ("analysis", "analysis_battle", "analysis_menu", "analysis_extra", "analysis_supacc"):
                z = strip(os.path.join(args.workdir, n + ".json"), os.path.join(args.outdir, n + ".json.gz"))[1]
                print("%s.json.gz: %d bytes" % (n, z))
        elif args.cmd == "hydrate":
            a = hydrate(args.shipped, args.binary, args.dst)
            print("%s: %d functions, segments verified" % (args.dst, len(a["functions"])))
        else:
            a = hydrate(args.shipped, args.binary)
            print("ok: %d functions, segments verified" % len(a["functions"]))
    except (ShipError, OSError, ValueError, KeyError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
