#!/usr/bin/env python3
"""Self-test for voice_bank.py on a synthetic SSCF bank (no game data). Run: python3 -I test_voice_bank.py"""
import json
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voice_bank as vb


def tone(f, n, rate=16000):
    return [int(0.5 * 32767 * math.sin(2 * math.pi * f * i / rate)) for i in range(n)]


def snr(a, b):
    n = min(len(a), len(b))
    return 10 * math.log10(sum(v * v for v in a[:n]) / max(1, sum((x - y) ** 2 for x, y in zip(a[:n], b[:n]))))


def synthetic_bank():
    """slot 0 clip, slot 1 empty, slot 2 alias of 0, slot 3 link to 0, slot 4 clip at 14 kHz."""
    clips = [vb.encode_vag(tone(300, 5000)), vb.encode_vag(tone(900, 3000, 14000))]
    ents = []
    pos = vb.HDR + vb.ENT * 5

    def ent(off, size, rate, vol, flag2=0, flag3=0):
        r = bytearray(vb.DEFAULT_ENTRY)
        struct.pack_into("<II", r, 4, off, size)
        struct.pack_into("<H", r, 0xC, rate)
        r[0xE], r[2], r[3] = vol, flag2, flag3
        return bytes(r)
    ents.append(ent(pos, len(clips[0]), 16000, 50))
    ents.append(ent(0, 0, 0, 0, flag2=0x20))
    ents.append(ent(pos, len(clips[0]), 16000, 60))
    ents.append(ent(0, 2, 0, 0, flag3=0x10))
    ents[3] = ents[3][:4] + struct.pack("<I", 0) + ents[3][8:]
    ents.append(ent(pos + len(clips[0]), len(clips[1]), 14000, 70))
    total = pos + sum(len(c) for c in clips)
    hdr = b"SSCF" + bytes([0x10, 2, 7, 2]) + struct.pack("<II", total, 5) + struct.pack("<HH", 1, 12345) + bytes(12)
    return hdr + b"".join(ents) + b"".join(clips)


def main():
    # ADPCM round trip
    x = tone(440, 4000)
    y, info = vb.decode_vag(vb.encode_vag(x))
    assert info["lead_block"] and abs(len(y) - len(x)) < 28
    assert snr(x, y) > 40, snr(x, y)
    blob = vb.encode_vag([0] * 10)
    assert blob[:16] == bytes(16) and blob[-16:] == vb.SENTINEL and blob[-32 + 1] == 1   # retail framing

    raw = synthetic_bank()
    b = vb.Bank.parse(raw)
    assert [e["kind"] for e in b.entries] == ["data", "empty", "alias", "link", "data"]
    assert b.to_bytes() == raw                                           # parse/serialise is lossless
    assert b.bank_id == 12345 and b.rate(4) == 14000

    with tempfile.TemporaryDirectory() as t:
        src = os.path.join(t, "a.scd")
        open(src, "wb").write(raw)
        out = os.path.join(t, "d")
        assert vb.main(["dump", src, out]) == 0
        man = json.load(open(os.path.join(out, "manifest.json")))
        assert man["slots"][2]["alias_of"] == 0 and man["slots"][4]["rate"] == 14000
        re1 = os.path.join(t, "b.scd")
        assert vb.main(["build", out, re1]) == 0
        assert open(re1, "rb").read() == raw                             # dump -> build is byte-identical

        # an edited WAV is re-encoded at its own rate, the others keep their bytes
        vb.write_wav(os.path.join(out, "004.wav"), tone(1200, 2000, 11025), 11025)
        re2 = os.path.join(t, "c.scd")
        assert vb.main(["build", out, re2, "--bank-id", "777"]) == 0
        c = vb.Bank.parse(open(re2, "rb").read())
        assert c.rate(4) == 11025 and c.bank_id == 777
        assert c.entries[0]["blob"] == b.entries[0]["blob"] and c.entries[2]["owner"] == 0

        # no manifest: numbered WAVs on top of --like; unlisted slots keep like's clip, --fill empty clears them
        nf = os.path.join(t, "new")
        os.makedirs(nf)
        vb.write_wav(os.path.join(nf, "001_grunt.wav"), tone(500, 3000), 16000)
        re3 = os.path.join(t, "d.scd")
        assert vb.main(["build", nf, re3, "--like", src]) == 0
        d = vb.Bank.parse(open(re3, "rb").read())
        assert d.entries[1]["kind"] == "data" and d.rate(1) == 16000 and d.entries[0]["blob"] == b.entries[0]["blob"]
        re4 = os.path.join(t, "e.scd")
        assert vb.main(["build", nf, re4, "--like", src, "--fill", "empty"]) == 0
        e = vb.Bank.parse(open(re4, "rb").read())
        assert [k["kind"] for k in e.entries] == ["empty", "data", "empty", "link", "empty"]
        # a vag replacement (pre-encoded) is taken as is
        nf2 = os.path.join(t, "new2")
        os.makedirs(nf2)
        pre = vb.encode_vag(tone(700, 2800))
        open(os.path.join(nf2, "000.vag"), "wb").write(vb.vag_file(pre, 12000, "x"))
        re5 = os.path.join(t, "f.scd")
        assert vb.main(["build", nf2, re5, "--like", src]) == 0
        f = vb.Bank.parse(open(re5, "rb").read())
        assert f.entries[0]["blob"] == pre and f.rate(0) == 12000 and f.entries[2]["owner"] == 0

    # ARC container round trip with a snd_ bank
    arc = vb.arc_join(bytes(8), [[7, b"p_xx", b"abcd"], [14, b"snd_", raw], [1, b"comb", b"\0" * 12]])
    bank, kind = None, None
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "x.objx")
        open(p, "wb").write(arc)
        bank, kind = vb.load_bank_file(p)
        assert kind == "objx" and bank.to_bytes() == raw
    print("ok")


if __name__ == "__main__":
    main()
