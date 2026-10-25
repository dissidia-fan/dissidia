#!/usr/bin/env python3
"""Prepare a work directory for the Dissidia 012 native build from the user's own ISO.

Usage: setup_from_iso.py <game.iso> <workdir> [options]

Creates, under <workdir>:
  disc0/PSP_GAME/SYSDIR/EBOOT.BIN   decrypted ELF (what the runtime boots)
  disc0/PSP_GAME/SYSDIR/BOOT.BIN    same decrypted ELF
  disc0/PSP_GAME/USRDIR/...         everything under USRDIR (PACKAGE.BIN, OVL_*.ELF, MODULE/, BGM/, MOVIE/ ...)
  disc0/PSP_GAME/PARAM.SFO, ICON0.PNG ... (small metadata files, if present)
  EBOOT.enc.bin                     original encrypted EBOOT.BIN from the ISO
  EBOOT.dec.elf                     decrypted EBOOT (input of `psprecomp analyze`)
  OVL_{BATTLE,MENU,EXTRA}_APP.bin   flat overlay blobs (ELF file offset 0x54 onward; load at 0x089DD4C0)
  eboot_image.bin                   flat EBOOT image 0x08804000..0x089DD4C0 (for jumptables.py / gapfuncs.py)
  ms0/PSP/{SAVEDATA,GAME}           Memory Stick root for the runtime
  tools/pspdecrypt                  built from source if not already available

Pure Python 3, standard library only. Supported inputs: a plain 2048-byte-sector ISO 9660 image, or a
CSO (CISO v1, zlib) image, which is read transparently. NOT supported: ZSO (LZ4), DAX, raw 2352-byte
(MODE2) dumps and encrypted/PBP "PSN" eboots - convert those to .iso first (e.g. `maxcso --decompress`).
Everything the game needs lives under PSP_GAME/USRDIR (DATA/ with PACKAGE.BIN, the OVL_*.ELF overlays,
MODULE/, BGM/ and MOVIE/) plus PSP_GAME/SYSDIR/EBOOT.BIN, so that is all that is extracted.
No game data is included with this repository; this reads only the ISO you provide.

EBOOT.BIN is encrypted on retail discs and needs `pspdecrypt` (C++, needs zlib + OpenSSL dev headers to
build; there is no pure-Python decryptor here). Alternatives: pass --pspdecrypt PATH, or --decrypted-eboot
FILE (e.g. dumped by PPSSPP: Settings > Tools > Developer tools > "Dump decrypted eboot" before booting the
game once). An EBOOT.BIN that is already a plain ELF is detected and used as is.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib

SECTOR = 2048
EBOOT_BASE = 0x08804000      # user-module load base of the decrypted EBOOT
OVL_BASE = 0x089DD4C0        # fixed load address of all three overlays (= end of the EBOOT image)
OVERLAYS = [                 # (ELF name under USRDIR/DATA, flat blob name, expected id word)
    ("OVL_BATTLE_APP.ELF", "OVL_BATTLE_APP.bin", 1),
    ("OVL_MENU_APP.ELF", "OVL_MENU_APP.bin", 2),
    ("OVL_EXTRA_APP.ELF", "OVL_EXTRA_APP.bin", 3),
]
KNOWN_PACKAGE_SIZE = 1034358784      # EU PSN ISO (ULES01505)
KNOWN_PACKAGE_MD5_PREFIX = "436a3db9"
KNOWN_DISC_ID = "ULES01505"
COMPATIBLE_DISC_IDS = {              # other releases known to work with the EU config: disc id -> summary note
    "ULUS10566": "US UMD: same code as the EU PSN image; works, less tested",
}
PSPDECRYPT_GIT = "https://github.com/John-K/pspdecrypt"

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


class IsoError(Exception):
    pass


# --------------------------------------------------------------------------- CSO

class CsoFile:
    """Read-only file-like view (seek/tell/read) of a CSO (CISO v1, zlib) image as a flat ISO."""

    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(24)
        if len(h) < 24 or h[:4] != b"CISO":
            raise IsoError("not a CSO file")
        _hs, total, self.block, ver, self.align = struct.unpack_from("<IQIBB", h, 4)
        if ver > 1:
            raise IsoError("CSO version %d is not supported (only v1); convert with maxcso --decompress" % ver)
        if self.block < SECTOR or self.block & (self.block - 1) or total == 0:
            raise IsoError("corrupt CSO header (block size %d)" % self.block)
        self.total = total
        n = (total + self.block - 1) // self.block + 1
        raw = self.f.read(4 * n)
        if len(raw) != 4 * n:
            raise IsoError("CSO index truncated")
        self.index = struct.unpack("<%dI" % n, raw)
        self.pos = 0
        self._cached = (-1, b"")

    def close(self):
        self.f.close()

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else (self.pos + off if whence == 1 else self.total + off)
        return self.pos

    def _block(self, i):
        if self._cached[0] == i:
            return self._cached[1]
        a, b = self.index[i], self.index[i + 1]
        start = (a & 0x7FFFFFFF) << self.align
        end = (b & 0x7FFFFFFF) << self.align
        self.f.seek(start)
        raw = self.f.read(end - start)
        if a & 0x80000000:                       # stored uncompressed
            data = raw[:self.block]
        else:
            try:
                data = zlib.decompressobj(-15).decompress(raw)
            except zlib.error as e:
                raise IsoError("CSO block %d is corrupt: %s" % (i, e))
        if len(data) < self.block and (i + 1) * self.block <= self.total:
            raise IsoError("CSO block %d is short (%d bytes)" % (i, len(data)))
        self._cached = (i, data)
        return data

    def read(self, n=-1):
        if n is None or n < 0 or self.pos + n > self.total:
            n = max(0, self.total - self.pos)
        out = []
        while n > 0:
            i, o = divmod(self.pos, self.block)
            d = self._block(i)[o:o + n]
            if not d:
                break
            out.append(d)
            n -= len(d)
            self.pos += len(d)
        return b"".join(out)


def open_image(path):
    """Open an ISO or CSO as a seekable binary file; raise IsoError with advice for unsupported formats."""
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic == b"CISO":
        return CsoFile(path)
    if magic == b"ZISO":
        raise IsoError("this is a ZSO (LZ4-compressed) image; convert it with `maxcso --decompress` to .iso first")
    if magic == b"DAX\0":
        raise IsoError("DAX images are not supported; convert to .iso first")
    if magic == b"\x7fPBP" or magic == b"\0PBP":
        raise IsoError("this is a PBP (PSN eboot) package, not a disc image; extract the ISO first")
    f = open(path, "rb")
    if not _has_cd001(f, 16 * SECTOR):
        for hdr in (16, 24):                     # MODE1 / MODE2-form1 raw sectors
            if _has_cd001(f, 16 * 2352 + hdr):
                f.close()
                raise IsoError("raw 2352-byte-sector image (BIN/MODE2) is not supported; "
                               "convert to a 2048-byte .iso first")
    f.seek(0)
    return f


def _has_cd001(f, off):
    f.seek(off)
    return f.read(6)[1:6] == b"CD001"


def parse_sfo(data):
    """Return {key: value} from a PARAM.SFO blob (strings and ints)."""
    if len(data) < 20 or data[:4] != b"\0PSF":
        return {}
    kt, dt, n = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(n):
        ko, fmt, ln, _mx, do = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        k = data[kt + ko:data.index(b"\0", kt + ko)].decode("ascii", "replace")
        v = data[dt + do:dt + do + ln]
        out[k] = struct.unpack_from("<I", v)[0] if fmt == 0x0404 and len(v) >= 4 else \
            v.split(b"\0", 1)[0].decode("utf-8", "replace")
    return out


# --------------------------------------------------------------------------- ISO 9660

class Iso9660:
    """Minimal read-only ISO 9660 (primary volume descriptor, no Joliet/Rock Ridge)."""

    def __init__(self, path):
        self.path = path
        self.f = open_image(path)
        self.f.seek(16 * SECTOR)
        pvd = self.f.read(SECTOR)
        if len(pvd) < SECTOR or pvd[1:6] != b"CD001":
            raise IsoError("not a 2048-byte-sector ISO 9660 image (no 'CD001' at sector 16)")
        # Walk volume descriptors until the primary (type 1) one.
        sec = 16
        while True:
            self.f.seek(sec * SECTOR)
            d = self.f.read(SECTOR)
            if len(d) < SECTOR or d[1:6] != b"CD001":
                raise IsoError("primary volume descriptor not found")
            if d[0] == 1:
                pvd = d
                break
            if d[0] == 255:
                raise IsoError("primary volume descriptor not found")
            sec += 1
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        self.block_size = struct.unpack_from("<H", pvd, 128)[0] or SECTOR
        if self.block_size != SECTOR:
            raise IsoError("unsupported logical block size %d" % self.block_size)
        self.root = self._parse_record(pvd, 156)[0]
        self._dir_cache = {}

    def close(self):
        self.f.close()

    @staticmethod
    def _parse_record(buf, off):
        """Return (record_dict, length) for the directory record at buf[off:]; length 0 = padding."""
        ln = buf[off]
        if ln == 0:
            return None, 0
        extent = struct.unpack_from("<I", buf, off + 2)[0]
        size = struct.unpack_from("<I", buf, off + 10)[0]
        flags = buf[off + 25]
        nlen = buf[off + 32]
        raw = buf[off + 33: off + 33 + nlen]
        if raw == b"\x00":
            name = "."
        elif raw == b"\x01":
            name = ".."
        else:
            name = raw.decode("ascii", "replace")
            if ";" in name:                 # strip ";1" version suffix
                name = name.split(";", 1)[0]
            if name.endswith(".") and len(name) > 1:
                name = name[:-1]
        return {"name": name, "extent": extent, "size": size, "is_dir": bool(flags & 2)}, ln

    def listdir(self, rec):
        key = rec["extent"]
        if key in self._dir_cache:
            return self._dir_cache[key]
        self.f.seek(rec["extent"] * SECTOR)
        data = self.f.read(rec["size"])
        out = []
        off = 0
        while off < len(data):
            r, ln = self._parse_record(data, off)
            if ln == 0:                      # records never straddle sectors; skip to next sector
                off = (off // SECTOR + 1) * SECTOR
                continue
            off += ln
            if r["name"] in (".", ".."):
                continue
            out.append(r)
        self._dir_cache[key] = out
        return out

    def find(self, path):
        """Case-insensitive lookup of 'A/B/C'; returns the record or None."""
        rec = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            if not rec["is_dir"]:
                return None
            for r in self.listdir(rec):
                if r["name"].upper() == part.upper():
                    rec = r
                    break
            else:
                return None
        return rec

    def walk(self, rec, prefix=""):
        """Yield (relative_path, record) for every file below rec."""
        for r in self.listdir(rec):
            p = prefix + r["name"]
            if r["is_dir"]:
                yield from self.walk(r, p + "/")
            else:
                yield p, r

    def read_file(self, rec, dest, want_md5=False, progress=None, chunk=8 << 20):
        """Stream a file record to the path dest (never holds the whole file in memory)."""
        h = hashlib.md5() if want_md5 else None
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        tmp = dest + ".part"
        self.f.seek(rec["extent"] * SECTOR)
        left = rec["size"]
        done = 0
        with open(tmp, "wb") as out:
            while left > 0:
                b = self.f.read(min(chunk, left))
                if not b:
                    raise IsoError("ISO truncated while reading %s" % dest)
                out.write(b)
                if h:
                    h.update(b)
                left -= len(b)
                done += len(b)
                if progress:
                    progress(done, rec["size"])
        os.replace(tmp, dest)
        return h.hexdigest() if h else None


def md5_file(path, chunk=8 << 20):
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


# --------------------------------------------------------------------------- ELF helpers

def elf_load_segments(data):
    """Return (e_type, [(p_offset, p_vaddr, p_filesz, p_memsz)]) for PT_LOAD entries of a 32-bit LE ELF."""
    if data[:4] != b"\x7fELF" or len(data) < 0x34:
        raise ValueError("not an ELF file")
    e_type = struct.unpack_from("<H", data, 16)[0]
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        if o + 32 > len(data):
            break
        p_type, p_off, p_va, _pa, p_fs, p_ms, _fl, _al = struct.unpack_from("<8I", data, o)
        if p_type == 1:
            segs.append((p_off, p_va, p_fs, p_ms))
    return e_type, segs


def build_eboot_image(elf_data, base=EBOOT_BASE, end=OVL_BASE):
    """Lay the ELF's PT_LOAD segments out flat from base to end (zero-filled gaps/bss, unrelocated)."""
    _t, segs = elf_load_segments(elf_data)
    img = bytearray(end - base)
    for p_off, p_va, p_fs, _ms in segs:
        va = p_va if p_va >= base else p_va + base      # PRX segments may be linked at 0
        a = va - base
        if a < 0 or a >= len(img):
            continue                                    # beyond the overlay window: not part of the image
        n = min(p_fs, len(img) - a)
        img[a:a + n] = elf_data[p_off:p_off + n]
    return bytes(img)


def overlay_blob(elf_data):
    """Overlay section data: the first PT_LOAD segment's file bytes (offset 0x54 for these ELFs)."""
    try:
        _t, segs = elf_load_segments(elf_data)
        off = segs[0][0] if segs else 0x54
    except ValueError:
        raise
    if off != 0x54:
        print("  note: first PT_LOAD at file offset 0x%X (expected 0x54); using it" % off)
    return elf_data[off:]


# --------------------------------------------------------------------------- pspdecrypt

def find_pspdecrypt(workdir, explicit=None):
    cands = []
    if explicit:
        cands.append(explicit)
    if os.environ.get("PSPDECRYPT"):
        cands.append(os.environ["PSPDECRYPT"])
    cands.append(os.path.join(workdir, "tools", "pspdecrypt"))
    w = shutil.which("pspdecrypt")
    if w:
        cands.append(w)
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    return None


def openssl_prefix():
    """Where OpenSSL's headers are when the compiler won't find them by itself (Homebrew's openssl@3 is not linked
    into /opt/homebrew/include): OPENSSL_ROOT_DIR, else `brew --prefix openssl@3`, else the usual Homebrew places."""
    env = os.environ.get("OPENSSL_ROOT_DIR")
    if env and os.path.isfile(os.path.join(env, "include", "openssl", "sha.h")):
        return env
    if sys.platform != "darwin":
        return None
    cands = []
    if shutil.which("brew"):
        for f in ("openssl@3", "openssl"):
            try:
                r = subprocess.run(["brew", "--prefix", f], capture_output=True, text=True, timeout=30)
                if r.returncode == 0 and r.stdout.strip():
                    cands.append(r.stdout.strip())
            except (OSError, subprocess.SubprocessError):
                pass
    cands += ["/opt/homebrew/opt/openssl@3", "/usr/local/opt/openssl@3", "/opt/homebrew/opt/openssl", "/usr/local/opt/openssl"]
    return next((c for c in cands if os.path.isfile(os.path.join(c, "include", "openssl", "sha.h"))), None)


def build_pspdecrypt(workdir, src_hint=None):
    """Build pspdecrypt into <workdir>/tools/pspdecrypt from a local source dir or a git clone."""
    tools = os.path.join(workdir, "tools")
    os.makedirs(tools, exist_ok=True)
    srcs = [src_hint, os.environ.get("PSPDECRYPT_SRC"), os.path.join(REPO_ROOT, "..", "pspdecrypt")]
    src = next((s for s in srcs if s and os.path.isfile(os.path.join(s, "Makefile"))
                and os.path.isfile(os.path.join(s, "pspdecrypt.cpp"))), None)
    build_dir = os.path.join(tools, "pspdecrypt-src")
    if os.path.isdir(build_dir):
        shutil.rmtree(build_dir)
    if src:
        print("  building pspdecrypt from local source %s" % os.path.abspath(src))
        shutil.copytree(src, build_dir, ignore=shutil.ignore_patterns("*.o", "pspdecrypt", ".git"))
    else:
        if not shutil.which("git"):
            raise RuntimeError("pspdecrypt not found and git is unavailable; install pspdecrypt "
                               "(%s) and pass --pspdecrypt PATH" % PSPDECRYPT_GIT)
        print("  cloning %s" % PSPDECRYPT_GIT)
        subprocess.run(["git", "clone", "--depth", "1", PSPDECRYPT_GIT, build_dir], check=True)
    env = dict(os.environ)
    if not shutil.which("clang") or not shutil.which("clang++"):   # Makefile defaults to clang
        env.update(CC=shutil.which("gcc") and "gcc" or "cc", CXX=shutil.which("g++") and "g++" or "c++")
    make_args = ["make", "-j%d" % min(4, os.cpu_count() or 1)]
    if "CC" in env and not shutil.which("clang"):
        make_args += ["CC=" + env["CC"], "CXX=" + env["CXX"]]
    ssl = openssl_prefix()
    if ssl:   # Homebrew's OpenSSL is not on the default include/library path; the Makefile sets its flags itself
        print("  using OpenSSL from %s" % ssl)
        make_args += ["CFLAGS=-O3 -I%s/include" % ssl, "CXXFLAGS=-O3 -I%s/include" % ssl, "EXTRA_FLAG=-L%s/lib" % ssl]
    r = subprocess.run(make_args, cwd=build_dir, env=env)
    exe = os.path.join(build_dir, "pspdecrypt")
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError("building pspdecrypt failed (needs a C/C++ compiler, make, zlib and OpenSSL "
                           "dev headers: e.g. `sudo apt install build-essential zlib1g-dev libssl-dev`, "
                           "or on a Mac `brew install openssl@3`)")
    dest = os.path.join(tools, "pspdecrypt")
    shutil.copy2(exe, dest)
    return os.path.abspath(dest)


def decrypt_eboot(pspdecrypt, enc_path, out_path):
    r = subprocess.run([pspdecrypt, "-o", out_path, enc_path], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(out_path):
        raise RuntimeError("pspdecrypt failed:\n%s%s" % (r.stdout, r.stderr))
    with open(out_path, "rb") as f:
        if f.read(4) != b"\x7fELF":
            raise RuntimeError("decrypted EBOOT is not an ELF (unsupported/corrupt EBOOT?)")
    return (r.stdout + r.stderr).strip()


# --------------------------------------------------------------------------- main flow

def fmt_size(n):
    return "%.1f MB" % (n / 1048576.0)


def disc_note(disc_id):
    """What the summary prints after the disc id: nothing for the EU PSN image, a note for a release known to
    work (COMPATIBLE_DISC_IDS), a warning for anything else."""
    if disc_id == KNOWN_DISC_ID:
        return ""
    if disc_id in COMPATIBLE_DISC_IDS:
        return "   (%s)" % COMPATIBLE_DISC_IDS[disc_id]
    return ("   <-- NOT the EU PSN image %s: the recompile config targets that build only (other regions are "
            "untested and the shipped analysis will not match)" % KNOWN_DISC_ID)


def extract(iso, workdir, force):
    """Extract SYSDIR/EBOOT.BIN (to EBOOT.enc.bin), PARAM.SFO-style metadata and all of USRDIR."""
    disc = os.path.join(workdir, "disc0", "PSP_GAME")
    result = {"missing": [], "disc_id": None, "title": None, "package_md5": None, "package_size": None, "files": 0, "bytes": 0}

    eb = iso.find("PSP_GAME/SYSDIR/EBOOT.BIN")
    if eb is None:
        raise IsoError("PSP_GAME/SYSDIR/EBOOT.BIN not found in the ISO - is this a PSP game disc image?")
    iso.read_file(eb, os.path.join(workdir, "EBOOT.enc.bin"))
    print("  EBOOT.BIN (encrypted) %s" % fmt_size(eb["size"]))

    sfo = iso.find("PSP_GAME/PARAM.SFO")
    if sfo is not None and sfo["size"] <= 1 << 20:
        iso.f.seek(sfo["extent"] * SECTOR)
        info = parse_sfo(iso.f.read(sfo["size"]))
        result["disc_id"] = info.get("DISC_ID")
        result["title"] = info.get("TITLE")
    for name in ("PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG", "PIC1.PNG", "SND0.AT3"):
        r = iso.find("PSP_GAME/" + name)
        if r is not None and not r["is_dir"]:
            iso.read_file(r, os.path.join(disc, name))
    sysdir = iso.find("PSP_GAME/SYSDIR")
    if sysdir is not None:        # other small SYSDIR files (OPNSSMP.BIN, UPDATE/...) - not EBOOT/BOOT
        for rel, r in iso.walk(sysdir):
            if rel.upper() in ("EBOOT.BIN", "BOOT.BIN"):
                continue
            if r["size"] <= 64 << 20:
                iso.read_file(r, os.path.join(disc, "SYSDIR", rel))

    usr = iso.find("PSP_GAME/USRDIR")
    if usr is None:
        raise IsoError("PSP_GAME/USRDIR not found in the ISO")
    files = sorted(iso.walk(usr), key=lambda x: x[0].upper())
    total = sum(r["size"] for _, r in files)
    print("  USRDIR: %d files, %s" % (len(files), fmt_size(total)))
    todo = 0
    for rel, r in files:
        dest = os.path.join(disc, "USRDIR", *rel.split("/"))
        if force or not (os.path.isfile(dest) and os.path.getsize(dest) == r["size"]):
            todo += r["size"]
    free = shutil.disk_usage(workdir).free
    if free < todo + (256 << 20):
        raise IsoError("not enough free disk space in %s: need about %s more, %s free"
                       % (workdir, fmt_size(todo + (256 << 20)), fmt_size(free)))
    last_pct = [-1]
    for rel, r in files:
        dest = os.path.join(disc, "USRDIR", *rel.split("/"))
        is_pkg = rel.upper() == "DATA/PACKAGE.BIN"
        if (not force and os.path.isfile(dest) and os.path.getsize(dest) == r["size"]):
            if is_pkg:
                result["package_size"] = r["size"]
                result["package_md5"] = md5_file(dest)
            continue                                   # already extracted (resume)

        def prog(done, size, rel=rel):
            if size >= (64 << 20):
                pct = done * 100 // size
                if pct != last_pct[0] and pct % 5 == 0:
                    last_pct[0] = pct
                    print("    %s %3d%%" % (rel, pct), flush=True)
        md5 = iso.read_file(r, dest, want_md5=is_pkg, progress=prog)
        last_pct[0] = -1
        result["files"] += 1
        result["bytes"] += r["size"]
        if is_pkg:
            result["package_size"] = r["size"]
            result["package_md5"] = md5

    for need in ["DATA/PACKAGE.BIN", "DATA/PACKAGE_INFO.BIN", "DATA/MODULE/LIBFONT.PRX",
                 "DATA/MODULE/LIBSUPPREACC.PRX"] + ["DATA/" + o[0] for o in OVERLAYS]:
        if iso.find("PSP_GAME/USRDIR/" + need) is None:
            result["missing"].append(need)
    bgm = iso.find("PSP_GAME/USRDIR/DATA/BGM")
    result["bgm_count"] = sum(1 for _ in iso.walk(bgm)) if bgm is not None else 0
    result["movie_count"] = 0
    for mp in ("PSP_GAME/USRDIR/DATA/MOVIE", "PSP_GAME/USRDIR/MOVIE"):     # EU disc: DATA/MOVIE
        mov = iso.find(mp)
        if mov is not None:
            result["movie_count"] += sum(1 for _ in iso.walk(mov))
    return result


def eboot_is_plain_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def postprocess(workdir, pspdecrypt, decrypted=None):
    """Decrypt the EBOOT (or take an already-decrypted one) and write the flat blobs."""
    facts = {}
    dec = os.path.join(workdir, "EBOOT.dec.elf")
    enc = os.path.join(workdir, "EBOOT.enc.bin")
    if decrypted:
        if not eboot_is_plain_elf(decrypted):
            raise RuntimeError("--decrypted-eboot %s is not an ELF" % decrypted)
        if os.path.abspath(decrypted) != os.path.abspath(dec):
            shutil.copyfile(decrypted, dec)
    elif eboot_is_plain_elf(enc):
        print("  EBOOT.BIN on the disc is already a plain ELF; no decryption needed")
        shutil.copyfile(enc, dec)
    else:
        note = decrypt_eboot(pspdecrypt(), enc, dec)
        if note:
            print("  pspdecrypt: " + note.splitlines()[-1])
    sysdir = os.path.join(workdir, "disc0", "PSP_GAME", "SYSDIR")
    os.makedirs(sysdir, exist_ok=True)
    shutil.copy2(dec, os.path.join(sysdir, "EBOOT.BIN"))
    shutil.copy2(dec, os.path.join(sysdir, "BOOT.BIN"))

    with open(dec, "rb") as f:
        edata = f.read()
    etype, segs = elf_load_segments(edata)
    facts["eboot_type"] = etype
    img = build_eboot_image(edata)
    with open(os.path.join(workdir, "eboot_image.bin"), "wb") as f:
        f.write(img)
    facts["eboot_image"] = len(img)
    if len(img) != OVL_BASE - EBOOT_BASE:
        raise RuntimeError("internal: eboot image size mismatch")

    facts["overlays"] = {}
    data_dir = os.path.join(workdir, "disc0", "PSP_GAME", "USRDIR", "DATA")
    for elf_name, bin_name, want_id in OVERLAYS:
        p = os.path.join(data_dir, elf_name)
        if not os.path.isfile(p):
            facts["overlays"][bin_name] = None
            continue
        with open(p, "rb") as f:
            d = f.read()
        et, osegs = elf_load_segments(d)
        blob = overlay_blob(d)
        with open(os.path.join(workdir, bin_name), "wb") as f:
            f.write(blob)
        ovl_id = struct.unpack_from("<I", blob, 0)[0] if len(blob) >= 4 else None
        va = osegs[0][1] if osegs else None
        facts["overlays"][bin_name] = (len(blob), ovl_id, want_id, et, va)
    return facts


def main(argv=None):
    ap = argparse.ArgumentParser(description="Extract and prepare Dissidia 012 data from your own ISO.")
    ap.add_argument("iso", help="path to the game .iso or .cso")
    ap.add_argument("workdir", help="output directory (created if missing)")
    ap.add_argument("--pspdecrypt", help="path to an existing pspdecrypt binary")
    ap.add_argument("--pspdecrypt-src", help="pspdecrypt source dir to build (default: clone from GitHub)")
    ap.add_argument("--decrypted-eboot", help="use this already-decrypted EBOOT ELF instead of running pspdecrypt")
    ap.add_argument("--force", action="store_true", help="re-extract files even if present with the same size")
    args = ap.parse_args(argv)

    iso_path = os.path.abspath(args.iso)
    workdir = os.path.abspath(args.workdir)
    if not os.path.isfile(iso_path):
        print("error: ISO not found: %s" % iso_path, file=sys.stderr)
        return 2
    os.makedirs(workdir, exist_ok=True)

    print("[1/4] Reading ISO %s" % iso_path)
    try:
        iso = Iso9660(iso_path)
    except IsoError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    print("  volume: %s" % (iso.volume_id or "(none)"))
    if isinstance(iso.f, CsoFile):
        print("  (CSO image, decompressed on the fly)")
    try:
        ext = extract(iso, workdir, args.force)
    except IsoError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    finally:
        iso.close()

    print("[2/4] Decrypting EBOOT.BIN")

    def get_pspdecrypt():
        pd = find_pspdecrypt(workdir, args.pspdecrypt)
        if pd is None:
            try:
                pd = build_pspdecrypt(workdir, args.pspdecrypt_src)
            except (subprocess.CalledProcessError, OSError) as e:
                raise RuntimeError("%s\n  (alternatives: --pspdecrypt PATH, or --decrypted-eboot FILE)" % e)
        print("  using %s" % pd)
        return pd
    try:
        facts = postprocess(workdir, get_pspdecrypt, args.decrypted_eboot)
    except (RuntimeError, ValueError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 3

    print("[3/4] Memory Stick directories")
    for sub in ("SAVEDATA", "GAME"):
        os.makedirs(os.path.join(workdir, "ms0", "PSP", sub), exist_ok=True)

    print("[4/4] Summary")
    if ext.get("disc_id"):
        print("  disc: %s %s%s" % (ext["disc_id"], ext.get("title") or "", disc_note(ext["disc_id"])))
    print("  workdir:            %s" % workdir)
    print("  extracted this run: %d files, %s" % (ext["files"], fmt_size(ext["bytes"])))
    print("  BGM files: %d (expected 134 on the EU ISO)   MOVIE files: %d" % (ext["bgm_count"], ext["movie_count"]))
    if ext["package_size"] is not None:
        ok_sz = ext["package_size"] == KNOWN_PACKAGE_SIZE
        ok_md = (ext["package_md5"] or "").startswith(KNOWN_PACKAGE_MD5_PREFIX)
        print("  PACKAGE.BIN: %d bytes (%s), md5 %s (%s)" % (
            ext["package_size"], "matches EU PSN ISO" if ok_sz else "differs from EU PSN ISO %d" % KNOWN_PACKAGE_SIZE,
            ext["package_md5"], "matches EU PSN ISO" if ok_md else "differs from EU PSN ISO %s..." % KNOWN_PACKAGE_MD5_PREFIX))
        if not (ok_sz and ok_md):
            print("  note: other regions/dumps legitimately differ; the recompile config targets the EU build "
                  "and may not apply to a different region.")
    print("  EBOOT: ELF type 0x%04X, flat image %d bytes (0x%08X..0x%08X)" % (
        facts["eboot_type"], facts["eboot_image"], EBOOT_BASE, OVL_BASE))
    for name, v in facts["overlays"].items():
        if v is None:
            print("  %s: SOURCE ELF MISSING" % name)
        else:
            size, got, want, et, va = v
            flag = "" if got == want else "  (WARNING: id word %s, expected %d)" % (got, want)
            print("  %s: %d bytes, id %s%s" % (name, size, got, flag))
            if va is not None and va != OVL_BASE:
                print("    WARNING: segment vaddr 0x%08X, expected 0x%08X" % (va, OVL_BASE))
    if ext["missing"]:
        print("  WARNING: expected files missing from ISO: " + ", ".join(ext["missing"]))
    print()
    print("Next:  games/dissidia/tools/pipeline.sh %s" % workdir)
    print("  (uses the shipped analysis in games/dissidia/analysis/ if present; otherwise add")
    print("   --ghidra <ghidra install dir>; see games/dissidia/README.md)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
