#!/usr/bin/env python3
"""Dissidia 012 voice banks: dump, build and alias.  See games/dissidia/docs/audio.md for the formats.

    voice_bank.py dump  <bank.scd | p_xxx.objx | event.at3> <outdir> [--vold voice_load_p_xxx.bin] [--no-wav]
    voice_bank.py build <dir> <out.scd> [--like like.scd] [--fill like|empty] [--rate HZ] [--bank-id N]
                        [--objx in.objx --out-objx out.objx]
    voice_bank.py alias <disc0|DATA> <like code> <new code> <mods dir> [--lang en] [--events]
    voice_bank.py at3   <in.wav> <out.at3>            # encode an event voice line (needs atracdenc)
    voice_bank.py info  <bank.scd | event.at3>

A battle voice bank (`voice/en/battle/voice_p_<code>_00.scd`, `_a_00.scd` for the second voice set) is an
`SSCF` container of 16-bit mono PlayStation ADPCM ("VAG") clips played through the SAS, so the whole
cycle is pure Python: `dump` decodes every clip to a WAV (the raw ADPCM is kept next to it as a playable
.vag), `build` re-encodes edited WAVs and packs the container.  A clip whose WAV was not touched keeps its
retail bytes, which is what makes `dump` followed by `build` byte-identical.

The event voice lines (`voice/en/event/<series>/NNN_<code>_NN.at3`) are plain RIFF ATRAC3 (66 kbps joint
stereo, 44.1 kHz).  ffmpeg decodes them; encoding needs an ATRAC3 encoder (ffmpeg has none):
`atracdenc` (https://github.com/dcherednik/atracdenc, `-e atrac3_lp4 --container riff`) is what `at3`
and `build --events` call.  Point $ATRACDENC at the binary or put it on PATH.

Only the standard library is used (plus ffmpeg and atracdenc when present, both optional).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SSCF_MAGIC = b"SSCF"
HDR = 0x20
ENT = 0x20
BLK = 16                       # bytes of one PS-ADPCM block = 28 samples
FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))
SENTINEL = bytes([0x00, 0x07]) + b"\x77" * 14      # every retail clip ends with this block (flag 7)
DEFAULT_ENTRY = bytes.fromhex("00000000" "00000000" "00000000" "803e5000" "6400ff00" "ca5f0000" "d2000000" "00000000")


def die(msg):
    print("error: " + msg, file=sys.stderr)
    raise SystemExit(1)


# ---------------------------------------------------------------------------------------------
# PS-ADPCM

def decode_vag(data):
    """Raw PS-ADPCM clip -> (int16 samples, info). The leading silent block and the sentinel are dropped.

    Decoding stops after the first block whose flag has bit 0 (end).  info['loop'] = [start block, end block]
    when the clip carries loop flags (flag bit 2 starts a loop; only the sound-effect banks inside .objx
    do): the WAV then holds one pass."""
    nblk = len(data) // BLK
    s1 = s2 = 0
    out = []
    first = 1 if nblk and data[:BLK] == bytes(BLK) else 0
    loop_start = None
    info = {"lead_block": bool(first)}
    for b in range(first, nblk):
        blk = data[b * BLK:(b + 1) * BLK]
        shift, filt, flag = blk[0] & 15, (blk[0] >> 4) & 7, blk[1]
        if filt > 4:
            filt = 4
        if flag & 4 and loop_start is None:
            loop_start = len(out)
        f0, f1 = FILTERS[filt]
        for i in range(28):
            nib = (blk[2 + (i >> 1)] >> (4 * (i & 1))) & 15
            if nib & 8:
                nib -= 16
            s = ((nib << 12) >> shift) + ((s1 * f0 + s2 * f1) >> 6)
            s = -32768 if s < -32768 else 32767 if s > 32767 else s
            out.append(s)
            s2, s1 = s1, s
        if flag & 1:
            if flag & 2 and loop_start is not None:
                info["loop"] = [loop_start, len(out)]
            break
    return out, info


def _encode_block(x, s1, s2, filt, shift):
    """Closed-loop encode of 28 samples with a given filter/shift -> (nibbles, error, s1, s2)."""
    f0, f1 = FILTERS[filt]
    scale = 1 << (12 - shift)
    nibs = []
    err = 0
    for v in x:
        pred = (s1 * f0 + s2 * f1) >> 6
        q = (v - pred) / scale
        q = int(q + 0.5) if q >= 0 else -int(-q + 0.5)
        q = -8 if q < -8 else 7 if q > 7 else q
        y = q * scale + pred
        y = -32768 if y < -32768 else 32767 if y > 32767 else y
        e = v - y
        err += e * e
        nibs.append(q & 15)
        s2, s1 = s1, y
    return nibs, err, s1, s2


def encode_vag(samples):
    """int16 samples -> retail-framed PS-ADPCM: silent lead block, data blocks (last one flag 1), sentinel block."""
    n = len(samples)
    nb = max(1, (n + 27) // 28)
    pad = list(samples) + [0] * (nb * 28 - n)
    out = bytearray(BLK)                                   # silent lead block
    s1 = s2 = 0
    for b in range(nb):
        x = pad[b * 28:(b + 1) * 28]
        best = None
        for filt in range(5):
            f0, f1 = FILTERS[filt]
            # open-loop residual picks the shift, then the neighbours are tried closed-loop
            a, c = s1, s2
            mx = 0
            for v in x:
                r = v - ((a * f0 + c * f1) >> 6)
                mx = max(mx, abs(r))
                c, a = a, v
            sh = 12
            while sh > 0 and (7 * (1 << (12 - sh))) < mx:
                sh -= 1
            for cand in {max(0, sh - 1), sh, min(12, sh + 1)}:
                nibs, err, e1, e2 = _encode_block(x, s1, s2, filt, cand)
                if best is None or err < best[0]:
                    best = (err, filt, cand, nibs, e1, e2)
        _, filt, sh, nibs, s1, s2 = best
        blk = bytearray(BLK)
        blk[0] = (filt << 4) | sh
        blk[1] = 1 if b == nb - 1 else 0
        for i in range(14):
            blk[2 + i] = nibs[2 * i] | (nibs[2 * i + 1] << 4)
        out += blk
    out += SENTINEL
    return bytes(out)


# ---------------------------------------------------------------------------------------------
# WAV helpers

def write_wav(path, samples, rate, channels=1):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def read_wav(path):
    """-> (mono int16 list, rate, channels, interleaved frames list) via the wave module, ffmpeg as a fallback."""
    def load(p):
        with wave.open(p, "rb") as w:
            ch, sw, rate, nf = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
            raw = w.readframes(nf)
        if sw == 2:
            vals = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
        elif sw == 1:
            vals = [(b - 128) << 8 for b in raw]
        elif sw == 3:
            vals = [int.from_bytes(raw[i:i + 3], "little", signed=True) >> 8 for i in range(0, len(raw), 3)]
        elif sw == 4:
            vals = [int.from_bytes(raw[i:i + 4], "little", signed=True) >> 16 for i in range(0, len(raw), 4)]
        else:
            raise wave.Error("sample width %d" % sw)
        return vals, rate, ch
    try:
        vals, rate, ch = load(path)
    except (wave.Error, EOFError):
        ff = shutil.which("ffmpeg")
        if not ff:
            die("%s is not a PCM WAV the wave module can read, and ffmpeg is not installed to convert it" % path)
        tmp = tempfile.mkdtemp()
        try:
            conv = os.path.join(tmp, "c.wav")
            subprocess.run([ff, "-loglevel", "error", "-y", "-i", path, "-c:a", "pcm_s16le", conv], check=True)
            vals, rate, ch = load(conv)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    mono = vals if ch == 1 else [sum(vals[i:i + ch]) // ch for i in range(0, len(vals) - ch + 1, ch)]
    return mono, rate, ch, vals


def resample(x, r_in, r_out):
    """Linear resampler (box-filtered when decimating). Good enough for voice clips; use ffmpeg for anything fancy."""
    if r_in == r_out or not x:
        return list(x)
    n = max(1, int(len(x) * r_out / r_in))
    ratio = r_in / r_out
    out = []
    if ratio > 1.0:
        for i in range(n):
            a, b = i * ratio, min(len(x), (i + 1) * ratio)
            lo, hi = int(a), max(int(a) + 1, int(b))
            seg = x[lo:hi]
            out.append(sum(seg) // len(seg))
    else:
        for i in range(n):
            p = i * ratio
            k = int(p)
            f = p - k
            v0 = x[k]
            v1 = x[k + 1] if k + 1 < len(x) else v0
            out.append(int(v0 + (v1 - v0) * f))
    return out


# ---------------------------------------------------------------------------------------------
# SSCF container

class Bank:
    """Parsed SSCF bank. `entries[i]` = dict(raw=bytearray(32), kind, owner, blob).

    kind: 'empty' (flag 0x20 in byte 2), 'link' (flag 0x10 in byte 3: byte 4 holds another slot index, size field 2),
    'data' (owns its ADPCM) or 'alias' (same offset/size as an earlier slot: `owner` is that slot)."""

    def __init__(self, header, entries):
        self.header = bytearray(header)
        self.entries = entries

    @classmethod
    def parse(cls, d):
        if d[:4] != SSCF_MAGIC:
            die("not an SSCF bank (magic %r)" % bytes(d[:4]))
        total, n = struct.unpack_from("<II", d, 8)
        if total != len(d):
            die("SSCF size field %d != file size %d" % (total, len(d)))
        ents, seen = [], {}
        for i in range(n):
            raw = bytearray(d[HDR + ENT * i:HDR + ENT * (i + 1)])
            off, size = struct.unpack_from("<II", raw, 4)
            e = {"raw": raw, "owner": None, "blob": None}
            if raw[3] & 0x10:
                e["kind"] = "link"
            elif size == 0:
                e["kind"] = "empty"
            elif (off, size) in seen:
                e["kind"], e["owner"] = "alias", seen[(off, size)]
            else:
                e["kind"] = "data"
                e["blob"] = bytes(d[off:off + size])
                seen[(off, size)] = i
            ents.append(e)
        b = cls(d[:HDR], ents)
        pos = HDR + ENT * n                                    # canonical layout check
        for i, e in enumerate(ents):
            if e["kind"] == "data":
                off, size = struct.unpack_from("<II", e["raw"], 4)
                if off != pos:
                    die("slot %d: data at 0x%X, expected 0x%X: this bank is not packed in slot order "
                        "(sound/snd_menu.scd is one); not supported" % (i, off, pos))
                pos += size
        if pos != len(d):
            die("bank has %d trailing bytes after the last clip" % (len(d) - pos))
        return b

    def to_bytes(self):
        n = len(self.entries)
        pos = HDR + ENT * n
        offs = {}
        body = bytearray()
        for i, e in enumerate(self.entries):
            if e["kind"] == "data":
                offs[i] = (pos + len(body), len(e["blob"]))
                body += e["blob"]
        table = bytearray()
        for i, e in enumerate(self.entries):
            raw = bytearray(e["raw"])
            if e["kind"] == "data":
                struct.pack_into("<II", raw, 4, *offs[i])
            elif e["kind"] == "alias":
                struct.pack_into("<II", raw, 4, *offs[e["owner"]])
            table += raw
        hdr = bytearray(self.header)
        struct.pack_into("<II", hdr, 8, HDR + len(table) + len(body), n)
        return bytes(hdr) + bytes(table) + bytes(body)

    # --- fields of a slot -------------------------------------------------------------------
    def rate(self, i):
        return struct.unpack_from("<H", self.entries[i]["raw"], 0xC)[0]

    def volume(self, i):
        return self.entries[i]["raw"][0xE]

    def set_rate(self, i, hz):
        struct.pack_into("<H", self.entries[i]["raw"], 0xC, hz)

    @property
    def bank_id(self):
        return struct.unpack_from("<H", self.header, 0x12)[0]

    @bank_id.setter
    def bank_id(self, v):
        struct.pack_into("<H", self.header, 0x12, v)


# --- ARC container of .objx (type 14 = the `snd_` SSCF sound bank) ------------------------------

def arc_split(d):
    if d[:4] != b"ARC\x01":
        die("not an ARC/objx file")
    n = struct.unpack_from("<I", d, 4)[0]
    ents = []
    for i in range(n):
        t, tag, o, s = struct.unpack_from("<I4sII", d, 16 + 16 * i)
        ents.append([t, tag, bytes(d[o:o + s])])
    return bytes(d[8:16]), ents


def arc_join(extra, ents):
    align = {14: 64, 2: 16, 3: 16}
    table, body = bytearray(), bytearray()
    o = 16 + 16 * len(ents)
    for t, tag, data in ents:
        pad = (-o) % align.get(t, 4)
        body += b"\0" * pad
        o += pad
        table += struct.pack("<I4sII", t, tag, o, len(data))
        body += data
        o += len(data)
    return b"ARC\x01" + struct.pack("<I", len(ents)) + extra + bytes(table) + bytes(body)


def load_bank_file(path):
    d = open(path, "rb").read()
    if d[:4] == b"ARC\x01":
        _, ents = arc_split(d)
        snd = [e for e in ents if e[0] == 14]
        if not snd:
            die("%s has no type-14 snd_ bank" % path)
        return Bank.parse(snd[0][2]), "objx"
    return Bank.parse(d), "scd"


# ---------------------------------------------------------------------------------------------
# slot roles (what the manifest says about a slot)

def vold_read(path):
    """voice_load_p_<code>.bin ('VOLD'): 24-byte records name[16], u32 cue, u8 kind, u8 0, u16 index."""
    d = open(path, "rb").read()
    if d[:4] != b"VOLD":
        die("%s is not a VOLD file" % path)
    n = struct.unpack_from("<I", d, 8)[0]
    rows = []
    for i in range(n):
        r = d[16 + 24 * i:16 + 24 * (i + 1)]
        rows.append({"name": r[:16].split(b"\0")[0].decode(), "cue": struct.unpack_from("<I", r, 16)[0],
                     "kind": r[20], "index": struct.unpack_from("<H", r, 22)[0]})
    return rows


def role_of(bank, i, vold):
    e = bank.entries[i]
    k = e["kind"]
    if k == "empty":
        return "unused slot"
    if k == "link":
        return "link to slot %d (no clip of its own)" % struct.unpack_from("<I", e["raw"], 4)[0]
    tag = ""
    if k == "alias":
        tag = "same clip as slot %d; " % e["owner"]
    return tag + SLOT_ROLES.get(i, "")


# slot -> event, for slots whose event was observed at runtime. None is pinned down yet: docs/audio.md "Slot roles"
# gives only statistical roles (no per-action attribution).
SLOT_ROLES = {}


# ---------------------------------------------------------------------------------------------
# dump

def sha1(b):
    return hashlib.sha1(b).hexdigest()


def vag_file(blob, rate, name):
    """VAGp wrapper (big endian header) so vgmstream / foobar can open the dumped raw clip."""
    h = b"VAGp" + struct.pack(">IIIII", 3, 0, len(blob), rate, 0)[0:20]
    h = h.ljust(0x20, b"\0") + name.encode()[:16].ljust(16, b"\0")
    return h.ljust(0x30, b"\0") + blob


def cmd_dump(a):
    src = a.src
    os.makedirs(a.outdir, exist_ok=True)
    raw = open(src, "rb").read()
    if raw[:4] == b"RIFF":
        return dump_event(src, a.outdir, a.no_wav)
    bank, kind = load_bank_file(src)
    vold = vold_read(a.vold) if a.vold else None
    man = {"format": "SSCF", "source": os.path.basename(src), "container": kind,
           "header": bytes(bank.header).hex(), "bank_id": bank.bank_id, "slots": []}
    for i, e in enumerate(bank.entries):
        s = {"slot": i, "kind": e["kind"], "raw": bytes(e["raw"]).hex(), "role": role_of(bank, i, vold)}
        if e["kind"] == "alias":
            s["alias_of"] = e["owner"]
        if e["kind"] in ("data", "alias"):
            s["rate"], s["volume"] = bank.rate(i), bank.volume(i)
        if e["kind"] == "data":
            pcm, info = decode_vag(e["blob"])
            base = "%03d" % i
            s["vag"] = base + ".vag"
            open(os.path.join(a.outdir, s["vag"]), "wb").write(vag_file(e["blob"], bank.rate(i), base))
            s["vag_sha1"] = sha1(e["blob"])
            s["samples"], s["seconds"] = len(pcm), round(len(pcm) / max(1, bank.rate(i)), 3)
            s.update(info)
            if not a.no_wav:
                s["wav"] = base + ".wav"
                write_wav(os.path.join(a.outdir, s["wav"]), pcm, bank.rate(i))
                s["wav_sha1"] = sha1(open(os.path.join(a.outdir, s["wav"]), "rb").read())
        man["slots"].append(s)
    if vold:
        man["vold"] = vold
    json.dump(man, open(os.path.join(a.outdir, "manifest.json"), "w"), indent=1)
    nd = sum(1 for e in bank.entries if e["kind"] == "data")
    print("%s: %d slots (%d clips, %d aliases, %d links, %d empty) -> %s"
          % (src, len(bank.entries), nd, sum(e["kind"] == "alias" for e in bank.entries),
             sum(e["kind"] == "link" for e in bank.entries), sum(e["kind"] == "empty" for e in bank.entries), a.outdir))
    return 0


def dump_event(src, outdir, no_wav):
    base = os.path.splitext(os.path.basename(src))[0]
    shutil.copyfile(src, os.path.join(outdir, base + ".at3"))
    if not no_wav:
        if not decode_at3(src, os.path.join(outdir, base + ".wav")):
            print("note: no ffmpeg with an ATRAC3 decoder; only the .at3 was copied")
    print_info(src)
    return 0


# ---------------------------------------------------------------------------------------------
# build

def slot_files(d):
    """{slot: path} for NNN.wav / NNN_name.wav / NNN.vag in a folder (wav wins over vag when both exist)."""
    found = {}
    for f in sorted(os.listdir(d)):
        m = re.match(r"^(\d+)(?:[_\-. ].*)?\.(wav|vag)$", f, re.I)
        if m:
            found.setdefault(int(m.group(1)), {})[m.group(2).lower()] = os.path.join(d, f)
    return found


def read_vag_file(path):
    d = open(path, "rb").read()
    if d[:4] == b"VAGp":
        rate = struct.unpack_from(">I", d, 0x10)[0]
        size = struct.unpack_from(">I", d, 0xC)[0]
        return d[0x30:0x30 + size], rate
    return d, None


def pcm_for_slot(path, rate_opt, template_rate):
    mono, rate, _, _ = read_wav(path)
    target = rate_opt or (rate if rate <= 22050 else 22050)
    if target != rate:
        mono = resample(mono, rate, target)
    return mono, target


def cmd_build(a):
    d = a.dir
    mpath = os.path.join(d, "manifest.json")
    man = json.load(open(mpath)) if os.path.exists(mpath) else None
    like = None
    if a.like:
        like, _ = load_bank_file(a.like)
    files = slot_files(d)
    if man:
        header = bytes.fromhex(man["header"])
        bank = Bank(header, [])
        for s in man["slots"]:
            bank.entries.append({"raw": bytearray.fromhex(s["raw"]), "kind": s["kind"],
                                 "owner": s.get("alias_of"), "blob": None})
        # unchanged clips keep the dumped retail bytes
        for s in man["slots"]:
            if s["kind"] != "data":
                continue
            i = s["slot"]
            vag = os.path.join(d, s.get("vag", "%03d.vag" % i))
            wav = os.path.join(d, s["wav"]) if s.get("wav") else None
            blob = None
            if wav and os.path.exists(wav):
                if sha1(open(wav, "rb").read()) == s.get("wav_sha1") and os.path.exists(vag):
                    blob = read_vag_file(vag)[0]
                else:
                    mono, rate = pcm_for_slot(wav, a.rate, s.get("rate"))
                    blob = encode_vag(mono)
                    bank.set_rate(i, rate)
                    print("slot %d: re-encoded %s (%d samples @ %d Hz)" % (i, os.path.basename(wav), len(mono), rate))
            elif os.path.exists(vag):
                blob, vr = read_vag_file(vag)
                if vr and sha1(blob) != s.get("vag_sha1"):
                    bank.set_rate(i, vr)
            if blob is None:
                die("slot %d: neither %s nor %s found" % (i, s.get("wav"), s.get("vag")))
            bank.entries[i]["blob"] = blob
        # slots added to the folder that the manifest does not have as data (empty / new)
        for i, fs in files.items():
            if i < len(bank.entries) and bank.entries[i]["kind"] in ("empty",):
                _fill_new(bank, i, fs, a, like)
        if a.bank_id is not None:
            bank.bank_id = a.bank_id
    else:
        if not like:
            die("%s has no manifest.json: pass --like <retail.scd> (header, slot count and per-slot parameters come from it)" % d)
        bank = Bank(like.header, [{"raw": bytearray(e["raw"]), "kind": e["kind"], "owner": e["owner"],
                                   "blob": e["blob"]} for e in like.entries])
        if a.fill == "empty":
            for i, e in enumerate(bank.entries):
                if e["kind"] in ("data", "alias"):
                    e["raw"][4:12] = bytes(8)
                    e["raw"][2] = 0x20
                    e["kind"], e["owner"], e["blob"] = "empty", None, None
        for i, fs in files.items():
            if i >= len(bank.entries):
                if a.fill != "empty" and not a.grow:
                    die("slot %d is beyond the %d slots of --like (pass --grow to append slots)" % (i, len(bank.entries)))
                while len(bank.entries) <= i:
                    bank.entries.append({"raw": bytearray(DEFAULT_ENTRY), "kind": "empty", "owner": None, "blob": None})
                    bank.entries[-1]["raw"][2] = 0x20
            _fill_new(bank, i, fs, a, like, replace=True)
        if a.bank_id is not None:
            bank.bank_id = a.bank_id
    _fix_aliases(bank)
    out = bank.to_bytes()
    if a.objx:
        base = open(a.objx, "rb").read()
        extra, ents = arc_split(base)
        for e in ents:
            if e[0] == 14:
                e[2] = out
                break
        else:
            die("--objx has no snd_ bank to replace")
        out_path = a.out_objx or a.out
        open(out_path, "wb").write(arc_join(extra, ents))
        print("wrote %s (objx with new snd_ bank, %d bytes)" % (out_path, os.path.getsize(out_path)))
    else:
        open(a.out, "wb").write(out)
        print("wrote %s: %d bytes, %d slots, bank id %d" % (a.out, len(out), len(bank.entries), bank.bank_id))
    return 0


def _fill_new(bank, i, fs, a, like, replace=False):
    e = bank.entries[i]
    if "wav" in fs:
        mono, rate = pcm_for_slot(fs["wav"], a.rate, None)
        blob = encode_vag(mono)
        print("slot %d: encoded %s (%d samples @ %d Hz, %d bytes)" % (i, os.path.basename(fs["wav"]), len(mono), rate, len(blob)))
    else:
        blob, vr = read_vag_file(fs["vag"])
        rate = vr or bank.rate(i) or 16000
    raw = e["raw"]
    if e["kind"] in ("empty", "link") or raw[2] & 0x20:
        # a slot that was empty in the template: borrow the default parameter block, keep the template's tail byte
        tail = raw[0x18:0x20]
        newraw = bytearray(DEFAULT_ENTRY)
        newraw[0x18:0x20] = tail if any(tail) else DEFAULT_ENTRY[0x18:0x20]
        for ent in bank.entries:
            if ent["kind"] == "data":
                newraw[0x18:0x20] = ent["raw"][0x18:0x20]
                break
        e["raw"] = raw = newraw
    raw[2], raw[3] = 0, 0
    e["kind"], e["owner"], e["blob"] = "data", None, blob
    struct.pack_into("<H", raw, 0xC, rate)


def _fix_aliases(bank):
    for i, e in enumerate(bank.entries):
        if e["kind"] == "alias" and bank.entries[e["owner"]]["kind"] != "data":
            # the owner was replaced by an empty slot: the alias becomes empty too
            e["raw"][4:12] = bytes(8)
            e["raw"][2] = 0x20
            e["kind"], e["owner"] = "empty", None


# ---------------------------------------------------------------------------------------------
# events (ATRAC3 RIFF)

def find_atracdenc():
    cand = os.environ.get("ATRACDENC") or shutil.which("atracdenc")
    return cand if cand and os.path.exists(cand) else None


def decode_at3(src, dst):
    ff = shutil.which("ffmpeg")
    if not ff:
        return False
    r = subprocess.run([ff, "-loglevel", "error", "-y", "-i", src, "-c:a", "pcm_s16le", dst])
    return r.returncode == 0


def print_info(path):
    d = open(path, "rb").read()
    if d[:4] != b"RIFF":
        return
    p = 12
    info = {}
    while p + 8 <= len(d):
        tag, sz = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
        if tag == b"fmt ":
            info["format"], info["channels"], info["rate"], info["bytes/s"], info["frame"] = struct.unpack_from("<HHIIH", d, p + 8)
        elif tag == b"fact":
            info["samples"] = struct.unpack_from("<I", d, p + 8)[0]
        elif tag == b"data":
            info["data"] = sz
        p += 8 + sz + (sz & 1)
    if info.get("format") == 0x270:
        info["codec"] = "ATRAC3, %d kbps" % round(info["bytes/s"] * 8 / 1000)
        if "samples" in info:
            info["seconds"] = round(info["samples"] / info["rate"], 2)
    print("%s: %s" % (path, ", ".join("%s=%s" % kv for kv in info.items())))


def encode_at3(wav, out):
    enc = find_atracdenc()
    if not enc:
        die("no ATRAC3 encoder: build atracdenc (github.com/dcherednik/atracdenc, needs libsndfile) and set $ATRACDENC "
            "or put it on PATH; or pass a pre-encoded .at3 (RIFF ATRAC3, 66 kbps, 44.1 kHz stereo)")
    mono, rate, ch, vals = read_wav(wav)
    tmp = tempfile.mkdtemp()
    try:
        # atracdenc wants 44.1 kHz; the retail event lines are stereo, so feed stereo
        if ch == 2 and rate == 44100:
            left, right = vals[0::2], vals[1::2]
        else:
            left = right = resample(mono, rate, 44100)
        inter = [0] * (2 * len(left))
        inter[0::2], inter[1::2] = left, right
        src = os.path.join(tmp, "in.wav")
        write_wav(src, inter, 44100, 2)
        dst = os.path.join(tmp, "out.at3")
        r = subprocess.run([enc, "-e", "atrac3_lp4", "--container", "riff", "-i", src, "-o", dst],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0 or not os.path.exists(dst):
            die("atracdenc failed: " + r.stderr.decode(errors="replace")[-300:])
        shutil.copyfile(dst, out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%s -> %s" % (wav, out))


def cmd_at3(a):
    encode_at3(a.wav, a.out)
    print_info(a.out)
    return 0


def cmd_info(a):
    raw = open(a.src, "rb").read(4)
    if raw == b"RIFF":
        print_info(a.src)
        return 0
    bank, kind = load_bank_file(a.src)
    print("%s: SSCF (%s), %d slots, bank id %d" % (a.src, kind, len(bank.entries), bank.bank_id))
    for i, e in enumerate(bank.entries):
        if e["kind"] in ("data", "alias"):
            n = len(decode_vag(e["blob"] if e["kind"] == "data" else bank.entries[e["owner"]]["blob"])[0])
            print("  %3d %-5s %5d Hz vol %3d %6.2fs %s" % (i, e["kind"], bank.rate(i), bank.volume(i), n / max(1, bank.rate(i)),
                                                           role_of(bank, i, None)))
        else:
            print("  %3d %-5s" % (i, e["kind"]))
    return 0


# ---------------------------------------------------------------------------------------------
# alias

def cmd_alias(a):
    import package_extract as pe
    pkg = pe.Package(a.disc)
    like, code = a.like.lower(), a.code.lower()
    paths = [l.strip().lower() for l in open(os.path.join(HERE, "..", "package_paths.txt"))]
    want = []
    for n in range(10):
        for suf in ("", "_a"):
            want.append("voice/%s/battle/voice_p_%s%s_%02d.scd" % (a.lang, like, suf, n))
    if a.events:
        want += [p for p in paths if p.startswith("voice/%s/event/" % a.lang) and "_%s_" % like in os.path.basename(p)]
    n = 0
    for p in want:
        if pe.package_crc(p) not in pkg.entries:
            continue
        dst = p.replace("voice_p_%s" % like, "voice_p_%s" % code).replace("_%s_" % like, "_%s_" % code)
        out = os.path.join(a.mods, dst)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, "wb").write(pkg.read(p))
        print("%s -> %s" % (p, out))
        n += 1
    if not n:
        print("nothing found for code %r" % like)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("dump")
    p.add_argument("src"), p.add_argument("outdir")
    p.add_argument("--vold", help="voice_load_p_<code>.bin of the character, to annotate slots")
    p.add_argument("--no-wav", action="store_true")
    p.set_defaults(fn=cmd_dump)
    p = sp.add_parser("build")
    p.add_argument("dir"), p.add_argument("out")
    p.add_argument("--like", help="retail bank to take the header, slot count and parameters from")
    p.add_argument("--fill", choices=("like", "empty"), default="like",
                   help="slots without a file: keep --like's clip (default) or leave them empty")
    p.add_argument("--grow", action="store_true", help="allow slots beyond --like's slot count")
    p.add_argument("--rate", type=int, help="resample new clips to this rate (default: keep, capped at 22050)")
    p.add_argument("--bank-id", type=int, dest="bank_id", help="header bank id (default: unchanged)")
    p.add_argument("--objx"), p.add_argument("--out-objx", dest="out_objx")
    p.set_defaults(fn=cmd_build)
    p = sp.add_parser("alias")
    p.add_argument("disc"), p.add_argument("like"), p.add_argument("code"), p.add_argument("mods")
    p.add_argument("--lang", default="en")
    p.add_argument("--events", action="store_true", help="also alias the event voice .at3 files")
    p.set_defaults(fn=cmd_alias)
    p = sp.add_parser("at3")
    p.add_argument("wav"), p.add_argument("out")
    p.set_defaults(fn=cmd_at3)
    p = sp.add_parser("info")
    p.add_argument("src")
    p.set_defaults(fn=cmd_info)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
