#!/usr/bin/env python3
"""Scaffold (or update, or remove) one complete new roster slot of the Dissidia 012 native build.

    new_character.py --work <psprecomp work dir> --mods <mods dir> --slot 35 --code viv100 --like eht110 \\
        --name "Vivi" --series "FINAL FANTASY IX" --logo ff9_logo.png \\
        --portrait vivi_portrait.png --slidein vivi_render.png [--model body.gmo --objx moves.objx]
    new_character.py --work W --mods M --slot 35 --remove
    add --dry-run to print the plan and write nothing

What it does for the slot (details and file list: docs/roster.md, "new_character.py"):

  * validates the slot (33 = the retail SEV120 slot, taken over; 34 = Noctis's slot; 35..42 are default-on
    (a roster.txt line is the opt-in); 1..32 are retail characters and refused; ROSTER_MAX comes from
    runtime/roster_max.h) and the code (<= 6 characters: chrp holds 'P_' + 6)
  * adds/replaces the line in <mods>/roster.txt  ("<slot> <code> <like charId> tile= art= artl=|artr= name= text= logo=")
  * writes every file the engine requests for the code. Files that are not generated are byte copies ("aliases") of
    <like>'s retail files (obj/*.gmo, *.objx, voice banks, HUD portraits, arcade select gims, ...), except that
    --model replaces the body gmos (1p/2p/3p/zako) and --objx the move set p_<code>.objx
  * generates from your pictures: grid tile (--portrait, head and shoulders filling the box, --tile-crop x0,y0,x1,y1
    in picture pixels), slide-in (--slidein, framed like the retail ones, --art-crop), name plate, game logo plate
    (--logo picture, or text from --series), VS screen name/face gims, face strips, encounter name text, and the
    name record of the info panels (text/<lang>/mess_pk_chara_select_0.bin)
  * rebuilds <mods>/by_crc/93307c1c.bin (main.bin: chrp + objentry records) with roster_mainbin.py, and with it the
    extras' own ability names / help texts from games/dissidia/extra_abilities.json (general_archive/main/<lang>/
    main_lang.bin and text/<lang>/mess_pk_customize_ability_0.bin; docs/roster.md)
  * keeps <mods>/readme_newchars.json (ignored by the loose-file layer: the name starts with "readme"), which makes
    a re-run idempotent (same slot -> updated in place, options you leave out are inherited) and lets --remove
    take everything out again, including the slot's entries in the shared menu pack and text packs.
Requires Pillow. Standard library otherwise.
"""
import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
if __name__ == "__main__":
    import pydeps                    # numpy / Pillow / scipy at the pinned versions (tools/pydeps.py)
    pydeps.require()
import character_art as ca            # noqa: E402
import gim_encode as ge               # noqa: E402
import menu_pack as mp                # noqa: E402
import mess_text as mt                # noqa: E402
import package_extract as pe          # noqa: E402
import roster_mainbin as rmb          # noqa: E402
from PIL import Image                 # noqa: E402

MANIFEST = "readme_newchars.json"
FIRST_SLOT, FIRST_RETAIL_FREE = 33, 33
RETAIL_SLOTS = 35                      # slots 0..34 work without the experimental gate
LANGS = ca.LANGS
TEXT_PACK = "text/%s/mess_pk_chara_select_0.bin"
OVERLAY_LANG = "jp"                    # the game spells it JP, the package keeps these gims under jp

# Files the engine requests for a character code, besides the paths recovered in package_paths.txt (which is
# incomplete: the arcade gims and the face strips are missing from it). {L} = language folder, {c} = code.
PATH_TEMPLATES = [
    "menu/{L}/battle/encount/encount_text_tim2/encount_{c}.gim",
    "menu/{L}/common/chara_name/p_{c}_name.gim",
    "menu/{L}/character_select/chara2/p_{c}_1p.gim",
    "menu/{L}/arcademode/arcade_last_chara_tim2/arcade_last_{c}.gim",
    "menu/{L}/story_mode/chara2/p_{c}_1p.gim",
    "menu/{L}/common/chara_face_in_name/p_{c}_face_in_name.gim",
    "menu/{L}/common/chara_face_in_name_r/p_{c}_face_in_name_r.gim",
    "menu/{L}/battle/chara_image/p_{c}_1p.gim",
    "menu/{L}/battle/chara_image/p_{c}_2p.gim",
    "menu/{L}/battle/chara_image/p_{c}_3p.gim",
    "menu/{L}/talkevent/tim2/chara/b_talk_{c}.gim",
    "menu/{L}/talkevent/tim2/chara/t_talk_{c}.gim",
    "voice/{L}/battle/voice_p_{c}_{N}.scd",
    "voice/{L}/battle/voice_p_{c}_a_{N}.scd",
]


def die(msg):
    print("error: " + msg, file=sys.stderr)
    raise SystemExit(1)


# ---------------------------------------------------------------------------------------------
# small pure helpers (unit tested in test_new_character.py)

def split_series(series):
    """'FINAL FANTASY IX' -> ('FINAL FANTASY', 'IX'); 'KINGDOM HEARTS' -> ('KINGDOM HEARTS', '')."""
    m = re.match(r"^(.*\S)\s+((?:[IVX]{1,5})|\d{1,2})$", series.strip())
    return (m.group(1), m.group(2)) if m else (series.strip(), "")


ROMAN = {"I": 1, "II": 2, "III": 3, "IV": 4, "V": 5, "VI": 6, "VII": 7, "VIII": 8, "IX": 9, "X": 10, "XI": 11, "XII": 12, "XIII": 13}


def retail_logo(series):
    """logo= value of the game's own plate for 'FINAL FANTASY <I..XIII>' (the grid record's f2: entity 0x706F + f2,
    FF I = 0 ... XIII = 12), or None when the game has no logo for that series."""
    game, numeral = split_series(series or "")
    if game.upper() != "FINAL FANTASY":
        return None
    n = int(numeral) if numeral.isdigit() else ROMAN.get(numeral.upper())
    return n - 1 if n and 1 <= n <= 13 else None


def shared_logo(man, slot, series):
    """The logo entry of another slot with the same series and a generated plate (nothing of its own), or None."""
    if not series:
        return None
    for s, v in sorted(man["slots"].items()):
        if s != str(slot) and (v.get("series") or "") == series and "logo" in v.get("g3", {}) and not v.get("logo_own"):
            return v["g3"]["logo"]
    return None


def max_slots(repo_root):
    try:
        for line in open(os.path.join(repo_root, "runtime", "roster_max.h")):
            m = re.match(r"#define\s+DISSIDIA_ROSTER_MAX\s+(\d+)", line)
            if m:
                return int(m.group(1))
    except OSError:
        pass
    return rmb.ROSTER_MAX


def check_slot(slot, rmax):
    """(ok, notes). Slots 1..32 are retail characters."""
    notes = []
    if slot < FIRST_SLOT:
        return False, ["slot %d is a retail character; the usable slots are %d..%d" % (slot, FIRST_SLOT, rmax - 1)]
    if slot >= rmax:
        return False, ["slot %d is outside the roster (ROSTER_MAX %d: slots up to %d)" % (slot, rmax, rmax - 1)]
    if slot == 33:
        notes.append("slot 33 is the retail SEV120 slot (not listed in the Quick Battle grid): its chrp record and objentry records are replaced")
    if slot == 34:
        notes.append("slot 34 is the slot Noctis (noc100) normally uses; fine while no noc100 line exists")
    if slot >= RETAIL_SLOTS:
        notes.append("slot %d is above 34: an extra charId, handled by the roster.cpp hooks (see docs/roster.md); a roster.txt line enables it" % slot)
    return True, notes


def roster_edit(lines, slot, new_line):
    """Replace the line of `slot` (first token), or append; new_line None removes it. Comments and other lines are kept."""
    lines = list(lines)
    while lines and not lines[-1].strip():
        lines.pop()
    out, done = [], False
    for ln in lines:
        tok = ln.split("#")[0].split()
        if tok and tok[0] == str(slot):
            if not done and new_line is not None:
                out.append(new_line)
            done = True
            continue
        out.append(ln)
    if not done and new_line is not None:
        out.append(new_line)
    return out


def roster_find(lines, slot):
    for ln in lines:
        tok = ln.split("#")[0].split()
        if len(tok) >= 2 and tok[0] == str(slot):
            return tok[1]
    return None


# ---------------------------------------------------------------------------------------------
# writer: every file goes through here so that --dry-run only reports

class Out:
    def __init__(self, mods, dry):
        self.mods, self.dry = mods, dry
        self.written, self.unchanged, self.removed, self.bytes = [], [], [], 0

    def path(self, rel):
        return os.path.join(self.mods, rel)

    def write(self, rel, data):
        p = self.path(rel)
        self.bytes += len(data)
        try:
            if os.path.exists(p) and os.path.getsize(p) == len(data) and open(p, "rb").read() == data:
                self.unchanged.append(rel)
                return
        except OSError:
            pass
        self.written.append(rel)
        if self.dry:
            return
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def copy_file(self, rel, src):
        self.write(rel, open(src, "rb").read())

    def remove(self, rel):
        p = self.path(rel)
        if not os.path.exists(p):
            return False
        self.removed.append(rel)
        if not self.dry:
            os.remove(p)
            d = os.path.dirname(p)
            while os.path.abspath(d) != os.path.abspath(self.mods) and not os.listdir(d):
                os.rmdir(d)
                d = os.path.dirname(d)
        return True


# ---------------------------------------------------------------------------------------------
# game data lookups

def chrp_table(pkg):
    """{charId: 'fiv100'} from main.bin's chrp records."""
    data = pkg.read_crc(rmb.MAIN_CRC)
    _, ents = rmb.mpk_unpack(data)
    by = {e[0]: e for e in ents}
    _, aents = rmb.arc_unpack(by["system.bin"][2])
    chrp = next(e for e in aents if e[0] == "chrp")[2]
    n = struct.unpack_from("<I", chrp, 4)[0]
    out = {}
    for i in range(n):
        r = chrp[8 + rmb.CHRP_REC * i: 8 + rmb.CHRP_REC * (i + 1)]
        out[r[0]] = bytes(r[0x74:0x7C]).rstrip(b"\0").decode()[2:].lower()
    return out


def resolve_like(pkg, like):
    table = chrp_table(pkg)
    if str(like).isdigit():
        lid = int(like)
        if lid not in table:
            die("--like %s: no chrp record for charId %d (known: %s)" % (like, lid, sorted(table)))
        return lid, table[lid]
    for cid, code in table.items():
        if code == str(like).lower():
            return cid, code
    die("--like %s: unknown character code (known: %s)" % (like, ", ".join(sorted(table.values()))))


def alias_sources(pkg, like_code):
    """[(like asset path)] of every file that exists in PACKAGE.BIN for the like code (recovered paths + templates)."""
    cands = []
    for line in open(os.path.join(HERE, "..", "package_paths.txt")):
        p = line.strip().lower()
        if like_code in p:
            cands.append(p)
    langs = ["en", "jp", "fr", "de", "it", "es"]
    for t in PATH_TEMPLATES:
        for L in langs:
            if "{N}" in t:
                cands += [t.format(L=L, c=like_code, N="%02d" % n) for n in range(10)]
            else:
                cands.append(t.format(L=L, c=like_code))
    seen, out = set(), []
    for p in cands:
        if p in seen:
            continue
        seen.add(p)
        if pe.package_crc(p) in pkg.entries:
            out.append(p)
    return sorted(out)


# ---------------------------------------------------------------------------------------------
# image generation

def load_image(path, what):
    try:
        return ge.knockout_white(Image.open(path).convert("RGBA"))   # a render on a white ground gets an alpha channel
    except Exception as e:                       # noqa: BLE001
        die("cannot read %s %r: %s" % (what, path, e))


def parse_crop(s, what):
    try:
        v = tuple(int(x) for x in s.split(","))
        assert len(v) == 4 and v[2] > v[0] and v[3] > v[1]
        return v
    except Exception:                            # noqa: BLE001
        die("%s must be x0,y0,x1,y1 in picture pixels, got %r" % (what, s))


class Art:
    """Everything drawn from the pictures. `portrait` is a head-and-shoulders picture (covered by a crop), `slidein` a full
    body render (the retail-style head detection frames it); either can stand in for the other."""

    def __init__(self, args):
        self.portrait = load_image(args["portrait"], "--portrait") if args.get("portrait") else None
        self.slidein = load_image(args["slidein"], "--slidein") if args.get("slidein") else None
        self.tile_crop = parse_crop(args["tile_crop"], "--tile-crop") if args.get("tile_crop") else None
        self.art_crop = parse_crop(args["art_crop"], "--art-crop") if args.get("art_crop") else None
        self.face_crop = parse_crop(args["face_crop"], "--face-crop") if args.get("face_crop") else None
        self.hud_crop = parse_crop(args["hud_crop"], "--hud-crop") if args.get("hud_crop") else None
        self.hud_flip = bool(args.get("hud_flip"))
        self.tile_flip = bool(args.get("tile_flip"))         # mirror the picture for the grid tile (face the way the retail tiles do)
        self.tile_cutout = bool(args.get("tile_cutout"))     # retail-style tile: the figure cut out over the grid, no dark box
        self.vs_crop = parse_crop(args["vs_crop"], "--vs-crop") if args.get("vs_crop") else None   # square, for the 64x64 VS face
        self.hud_from = args.get("hud_from") or "portrait"
        if self.hud_from not in ("portrait", "slidein"):
            die("--hud-from must be portrait or slidein")

    @property
    def any(self):
        return self.portrait is not None or self.slidein is not None

    def tile(self, template, scale=1):
        w, h = 32, 64
        fh = ge.tile_box_height(template, (w, h))
        crop = self.tile_crop
        src = self.portrait if self.portrait is not None else self.slidein
        if self.portrait is not None and crop is None:
            crop = ge.cover_crop(self.portrait, w / fh, 0.20)
        if self.tile_flip:                     # --tile-crop stays in the coordinates of the picture as given
            src = src.transpose(Image.FLIP_LEFT_RIGHT)
            if crop is not None:
                crop = (src.width - crop[2], crop[1], src.width - crop[0], crop[3])
        return ge.prepare(src, (w, h), "tile_cutout" if self.tile_cutout else "tile", crop, template=template, scale=scale)

    def square_face(self, size, scale=1):
        """64x64 VS face: --vs-crop, else the tile's crop made square around its centre, or a head crop of the portrait.
        Follows the tile's flip and cut-out settings (the retail VS faces are cut-outs too)."""
        src = self.portrait if self.portrait is not None else self.slidein
        if self.vs_crop is not None:
            crop = self.vs_crop
        elif self.tile_crop is not None:
            x0, y0, x1, y1 = self.tile_crop
            s, cx, cy = max(x1 - x0, y1 - y0), (x0 + x1) // 2, (y0 + y1) // 2
            crop = (cx - s // 2, cy - s // 2, cx - s // 2 + s, cy - s // 2 + s)
        elif self.portrait is not None:
            crop = ge.cover_crop(self.portrait, 1.0, 0.15)
        else:
            crop = None
        if self.tile_flip:
            src = src.transpose(Image.FLIP_LEFT_RIGHT)
            if crop is not None:
                crop = (src.width - crop[2], crop[1], src.width - crop[0], crop[3])
        return ge.prepare(src, size, "tile_cutout" if self.tile_cutout else "tile", crop, scale=scale)

    def strip(self, template, flip, scale=1):
        w, h = 128, 32
        crop = self.face_crop
        if self.portrait is not None:
            if crop is None:
                crop = ge.cover_crop(self.portrait, w / h, 0.30, zoom=1.6)
            im = ge.prepare(self.portrait, (w, h), "face_in", crop, template=template, scale=scale)
        else:
            im = ge.prepare(self.slidein, (w, h), "face_in", crop, template=template, scale=scale)
        return im.transpose(Image.FLIP_LEFT_RIGHT) if flip else im

    def hud(self, scale=1):
        """128x128 in-battle HUD portrait (chara_image), see gim_encode.frame_hud. Source: the portrait (a head-and-shoulders picture,
        default) or, with --hud-from slidein or no portrait, the full body render (head found like the tile does). --hud-crop is in
        the source picture's pixels. A picture with no transparency gets an oval cameo instead of a cut-out."""
        use_slide = self.slidein is not None and (self.hud_from == "slidein" or self.portrait is None)
        src = self.slidein if use_slide else self.portrait
        crop = self.hud_crop
        if self.hud_flip:                      # mirror the source first; --hud-crop stays in the coordinates of the picture as given
            src = src.transpose(Image.FLIP_LEFT_RIGHT)
            if crop is not None:
                crop = (src.width - crop[2], crop[1], src.width - crop[0], crop[3])
        return ge.frame_hud(src, (128, 128), crop, head_picture=not use_slide, scale=scale)

    def art(self, size, scale=1):
        if self.art_crop is None and self.slidein is None:      # portrait only: a square cover crop, top aligned
            return ge.prepare(self.portrait, size, "art", ge.cover_crop(self.portrait, 1.0, 0.0), scale=scale)
        return ge.prepare(self.slidein, size, "art", self.art_crop, scale=scale)


def encount_image(name):
    """256x64 encounter text like the retail ones (name in the glowing serif, left aligned, lower half)."""
    big = ge.render_name(name, (256, 64), pt=34)
    bb = big.getchannel("A").getbbox()
    out = Image.new("RGBA", (256, 64), (255, 255, 255, 0))
    if bb:
        crop = big.crop(bb)
        out.paste(crop, (10, 38 - crop.height // 2))
    return out


# ---------------------------------------------------------------------------------------------
# shared packs (one file for all characters: the slot owns entries in them)

def shared_g3(mods, pkg):
    g3d = pkg.read(ca.G3)
    cur = os.path.join(mods, "by_crc", "%08x.bin" % pe.package_crc(ca.G3))
    base = mp.parse(g3d)
    orig = len(base["ents"])
    if os.path.exists(cur):
        pk = mp.parse(open(cur, "rb").read())
        blobs = [mp.entry(pk, k) for k in range(len(pk["ents"]))]
    else:
        pk = base
        blobs = [mp.entry(base, k) for k in range(orig)]
    return base, orig, blobs


def free_entry(blobs, orig, taken):
    for k in range(orig, len(blobs)):
        if not blobs[k] and k not in taken:
            return k
    blobs.append(b"")
    return len(blobs) - 1


def trim(blobs, orig):
    while len(blobs) > orig and not blobs[-1]:
        blobs.pop()


def text_packs(mods, pkg):
    """{lang: (orig texts, current texts)} of entry 0 of the chara select mess pack, for each language that exists."""
    out = {}
    for lang in LANGS:
        p = TEXT_PACK % lang
        try:
            orig = pkg.read(p)
        except KeyError:
            continue
        cur = os.path.join(mods, p)
        data = open(cur, "rb").read() if os.path.exists(cur) else orig
        out[lang] = (mt.records(mp.entry(mp.parse(orig), 0)), mt.records(mp.entry(mp.parse(data), 0)), mp.parse(orig))
    return out


def text_blob(pk, texts):
    blobs = [mp.entry(pk, k) for k in range(len(pk["ents"]))]
    blobs[0] = mt.build(texts)
    return mp.build(pk, blobs)


# ---------------------------------------------------------------------------------------------

def load_manifest(mods):
    p = os.path.join(mods, MANIFEST)
    if os.path.exists(p):
        return json.load(open(p))
    return {"version": 1, "slots": {}}


HUD_CONVENTION = ("The in-battle HUD (menu/jp/battle/chara_image/p_<code>_{1p,2p,3p}.gim) never mirrors a portrait: the same texture is drawn on the "
                  "P1 frame (bottom left) and on the CPU frame (bottom right). Retail portraits face either way. A source gim facing right looks "
                  "towards the arena as P1 and outward as the CPU. --hud-flip mirrors the source picture before it is cut.")


def save_manifest(out, man):
    man["hud_convention"] = HUD_CONVENTION
    if man["slots"]:
        out.write(MANIFEST, (json.dumps(man, indent=1, sort_keys=True) + "\n").encode())
    else:
        out.remove(MANIFEST)


def read_roster_lines(mods):
    p = os.path.join(mods, "roster.txt")
    return open(p).read().split("\n") if os.path.exists(p) else []


def write_roster(out, lines):
    while lines and not lines[-1].strip():
        lines.pop()
    if any(ln.split("#")[0].strip() for ln in lines):
        out.write("roster.txt", ("\n".join(lines) + "\n").encode())
    elif os.path.exists(out.path("roster.txt")):
        if all(not ln.split("#")[0].strip() for ln in lines):
            out.remove("roster.txt")


def rebuild_mainbin(out, pkg, mods, lines, log):
    rel = "by_crc/%08x.bin" % rmb.MAIN_CRC
    tmp = os.path.join(out.mods, ".roster_tmp.txt")
    chars = []
    for ln in lines:
        tok = ln.split("#")[0].split()
        if len(tok) >= 2 and tok[0].isdigit() and int(tok[0]) >= FIRST_SLOT:
            like = 9
            for t in tok[2:]:
                if "=" not in t and t.isdigit():
                    like = int(t)
            chars.append((int(tok[0]), tok[1], like))
    if not chars:
        if out.remove(rel):
            log("removed " + rel)
        for trel in (rmb.LANG_ARCHIVE % g for g in rmb.TEXT_LANGS):      # the ability-name overrides go with main.bin
            if out.remove(trel):
                log("removed " + trel)
        for trel in (rmb.HELP_PACK % g for g in rmb.TEXT_LANGS):
            if out.remove(trel):
                log("removed " + trel)
        return None
    objs, objs_char = [], None                      # objs=new:src,... clones (Y'shtola's aeons): keep them on every rebuild
    for ln in lines:
        toks = ln.split("#")[0].split()
        for t in toks:
            if t.startswith("objs="):
                objs += rmb.parse_objs(t[5:])
                if objs_char is None and toks[0].isdigit():
                    objs_char = int(toks[0])
    data = pkg.read_crc(rmb.MAIN_CRC)
    names, texts = rmb.load_names_and_texts(pkg.read)        # extra_abilities.json (if present): the extras' own ability names
    new = rmb.build(data, chars, max_slots(os.path.join(HERE, "..")), log=lambda *a: None, min_id=FIRST_SLOT,
                    objs=objs or None, objs_char=objs_char, names=names, texts=texts)
    text_files = texts.files() if texts else {}
    out.write(rel, new)
    for trel, blob in sorted(text_files.items()):             # main.bin's new name / help ids point into these
        out.write(trel, blob)
    if objs:
        log("main.bin keeps the objs= clones; the declaring character's body objx must be repointed to the copied wpnp "
            "rows (roster_mainbin.py --repoint IN.objx:OUT.objx)")
    return len(new)


def apply_slot(a, pkg, out, man, log):
    mods = a["mods"]
    slot, code = a["slot"], a["code"]
    like_id, like_code = resolve_like(pkg, a["like"])
    name = a["name"]
    art = Art(a)
    prev = man["slots"].get(str(slot), {})
    files = []

    def put(rel, data):
        out.write(rel, data)
        files.append(rel)

    # --- aliases and replacements -----------------------------------------------------------
    model, objx = a.get("model"), a.get("objx")
    sources = alias_sources(pkg, like_code)
    generated = set()                                   # filled below: paths this tool draws itself
    gen_lang = [L for L in LANGS if pe.package_crc("menu/%s/common/chara_name/p_%s_name.gim" % (L, like_code)) in pkg.entries]
    for L in gen_lang:
        generated.add("menu/%s/common/chara_name/p_%s_name.gim" % (L, like_code))
    generated.add("menu/jp/story_mode/chara2/p_%s_1p.gim" % like_code)
    generated.add("menu/jp/common/chara_face_in_name/p_%s_face_in_name.gim" % like_code)
    generated.add("menu/jp/common/chara_face_in_name_r/p_%s_face_in_name_r.gim" % like_code)
    for L in LANGS:
        generated.add("menu/%s/battle/encount/encount_text_tim2/encount_%s.gim" % (L, like_code))
    hud_paths = ["menu/%s/battle/chara_image/p_%s_%dp.gim" % (L, like_code, n) for L in LANGS for n in (1, 2, 3)]
    generated.update(hud_paths)
    has_images = art.any
    n_alias, placeholders = 0, []
    for src in sources:
        dst = src.replace(like_code, code)
        if src in generated and (has_images or src.endswith("encount_%s.gim" % like_code) or "chara_name" in src) \
                and not (src in hud_paths and not has_images):
            continue                                    # drawn below
        if model and re.match(r"^obj/p_%s_(1p|2p|3p|zako)\.gmo$" % like_code, src):
            put(dst, open(model, "rb").read())
            continue
        if objx and src == "obj/p_%s.objx" % like_code:
            put(dst, open(objx, "rb").read())
            continue
        data = pkg.read(src)
        put(dst, data)
        n_alias += 1

    # --- generated menu gims ----------------------------------------------------------------------
    def tmpl(path_fmt, fallback_fmt=None):
        for fmt in (path_fmt, fallback_fmt):
            if fmt and pe.package_crc(fmt.format(c=like_code)) in pkg.entries:
                return pkg.read(fmt.format(c=like_code))
            if fmt and pe.package_crc(fmt.format(c="fiv100")) in pkg.entries:
                return pkg.read(fmt.format(c="fiv100"))
        return None

    full = a.get("full_name") or name
    for L in gen_lang:
        t = pkg.read("menu/%s/common/chara_name/p_%s_name.gim" % (L, like_code))
        put("menu/%s/common/chara_name/p_%s_name.gim" % (L, code), ge.encode(t, ge.render_name(full, (128, 32))))
    t_enc = pkg.read("menu/en/battle/encount/encount_text_tim2/encount_%s.gim" % like_code) \
        if pe.package_crc("menu/en/battle/encount/encount_text_tim2/encount_%s.gim" % like_code) in pkg.entries else None
    if t_enc is not None:
        put("menu/en/battle/encount/encount_text_tim2/encount_%s.gim" % code, ge.encode(t_enc, encount_image(full)))
    if has_images:
        t_fi = tmpl("menu/jp/common/chara_face_in_name/p_{c}_face_in_name.gim")
        t_fr = tmpl("menu/jp/common/chara_face_in_name_r/p_{c}_face_in_name_r.gim")
        t_1p = tmpl("menu/jp/story_mode/chara2/p_{c}_1p.gim")
        if t_fi:
            put("menu/jp/common/chara_face_in_name/p_%s_face_in_name.gim" % code,
                ge.encode(t_fi, art.strip(ge.decode(t_fi), False)))
        if t_fr:
            put("menu/jp/common/chara_face_in_name_r/p_%s_face_in_name_r.gim" % code,
                ge.encode(t_fr, art.strip(ge.decode(t_fr), True)))
        if t_1p:
            put("menu/jp/story_mode/chara2/p_%s_1p.gim" % code, ge.encode(t_1p, art.square_face(ge.decode(t_1p).size)))
        hud_img = art.hud()                              # the same picture for the three costume files (retail: 2p/3p differ only in art)
        for src in hud_paths:
            if pe.package_crc(src) in pkg.entries:
                put(src.replace(like_code, code), ge.encode(pkg.read(src), hud_img))

    # --- shared pack entries (tile, slide-in, name plate, logo) and the text record ---------------
    tokens = []
    like_tpl = like_id if 1 <= like_id <= 32 else 9
    g0 = mp.parse(pkg.read("menu/menu_pk_free_battle_gim_0.bin"))
    g2 = mp.parse(pkg.read("menu/menu_pk_free_battle_gim_2.bin"))
    lg = mp.parse(pkg.read("menu/en/menu_lpk_free_battle_gim_0.bin"))
    base, orig, blobs = shared_g3(mods, pkg)
    ent = dict(prev.get("g3", {}))
    taken = {k for s, v in man["slots"].items() if s != str(slot) for k in v.get("g3", {}).values()}

    def slot_entry(key, blob):
        k = ent.get(key)
        if k is None or k >= len(blobs):
            k = free_entry(blobs, orig, taken | set(ent.values()))
            ent[key] = k
        blobs[k] = blob
        return base["base"] + k

    t_name = mp.entry(lg, 79 + like_tpl - 1)
    name_id = slot_entry("name", ge.encode(t_name, ge.render_name(name, (128, 32))))
    tokens.append("name=%d" % (name_id - ca.BASE_NAME))
    if has_images:
        t_tile, t_art = mp.entry(g2, 12 + like_tpl), mp.entry(g0, 12 + 9)      # any 32x64 / 256x256 retail gim is a template
        tile_id = slot_entry("tile", ge.encode(t_tile, art.tile(ge.decode(t_tile))))
        art_blob = ge.encode(t_art, art.art((256, 256)))
        art_id = slot_entry("art", art_blob)
        tokens = ["tile=%d" % (tile_id - ca.BASE_TILE), "art=%d" % (art_id - ca.BASE_ART)] + tokens
        # The battle setup panel shows the player's portrait on the left and the opponent's on the right, from the
        # same texture and never mirrored. A mirrored copy goes to the side the slide-in looks away on, so the two
        # face each other: artl= (player, left) for a slide-in looking left (the default), artr= for one looking right.
        # Not mirrored any more (22 Oct): the player's side gets its own entry holding an exact copy. With both sides on
        # the art= entry the player's portrait drew faded (that entry is also the grid slide-in's).
        side = "artl"
        if "artl" in ent and side != "artl":
            blobs[ent.pop("artl")] = b""
        if "artr" in ent and side != "artr":
            blobs[ent.pop("artr")] = b""
        mir_id = slot_entry(side, art_blob)
        tokens.insert(2, "%s=%d" % (side, mir_id - ca.BASE_ART))
    logo_img = load_image(a["logo"], "--logo") if a.get("logo") else None
    own_logo = logo_img is not None or bool(a.get("logo_own"))
    series = a.get("series") or ""

    def release(key):                                    # give up an entry, unless another slot shares it
        k = ent.pop(key)
        if k not in taken:
            blobs[k] = b""
    f2 = None if own_logo else retail_logo(series)
    if f2 is not None:                                   # the game has this series' logo: use it, no entry of our own
        if "logo" in ent:
            release("logo")
        tokens.append("logo=%d" % f2)
    elif logo_img is not None or series:
        t_logo = mp.entry(g2, 77 + 3)
        game, numeral = split_series(series)
        plate = ge.render_logo(numeral, (128, 64), ge.decode(t_logo), game=game or "FINAL FANTASY", image=logo_img)
        shared = None if own_logo else shared_logo(man, slot, series)
        if shared is not None and ent.get("logo") != shared:   # another character of this series has the same plate
            if "logo" in ent:
                release("logo")
            ent["logo"] = shared
        logo_id = slot_entry("logo", ge.encode(t_logo, plate))
        tokens.append("logo=%d" % (logo_id - ca.BASE_LOGO))
    elif "logo" in ent:                                  # logo dropped on a re-run
        release("logo")
    trim(blobs, orig)
    out.write("by_crc/%08x.bin" % pe.package_crc(ca.G3), mp.build(base, blobs))

    texts = text_packs(mods, pkg)
    rec = prev.get("text")
    if texts:
        if rec is None:
            used = {v.get("text") for s, v in man["slots"].items() if s != str(slot)}
            rec = next(i for i in range(max(len(c) for _, c, _ in texts.values()) + 1)
                       if i >= max(len(o) for o, _, _ in texts.values()) and i not in used)
        for lang, (o, c, pk) in texts.items():
            c = list(c)
            while len(c) <= rec:
                c.append("")
            c[rec] = name
            out.write(TEXT_PACK % lang, text_blob(pk, c))
        tokens.append("text=%d" % rec)

    # --- roster.txt, main.bin ------------------------------------------------------------------------
    tokens.append("nm=" + name.replace(" ", "_"))   # label for text the game has no record of (Opponent Info)
    line = "%d %s %d %s" % (slot, code, like_id, " ".join(tokens))
    lines = roster_edit(read_roster_lines(mods), slot, line)
    write_roster(out, lines)
    mb = rebuild_mainbin(out, pkg, mods, lines, log)

    man["slots"][str(slot)] = {
        "code": code, "like_id": like_id, "like_code": like_code, "name": name, "series": a.get("series"),
        "full_name": a.get("full_name"), "files": sorted(set(files)), "g3": ent, "text": rec, "roster_line": line,
        "logo_own": own_logo,
        "args": {k: a.get(k) for k in ("portrait", "slidein", "logo", "model", "objx", "tile_crop", "art_crop", "face_crop", "hud_crop", "hud_from", "hud_flip", "art_faces", "tile_flip", "tile_cutout", "vs_crop") if a.get(k)},
    }
    return {"line": line, "like_id": like_id, "like_code": like_code, "files": sorted(set(files)), "n_alias": n_alias,
            "mainbin": mb, "has_images": has_images, "model": bool(model), "objx": bool(objx),
            "logo": bool(logo_img is not None or a.get("series")), "logo_image": logo_img is not None}


def remove_slot(slot, pkg, out, man, mods, log):
    info = man["slots"].get(str(slot))
    lines = read_roster_lines(mods)
    code = roster_find(lines, slot)
    if info is None and code is None:
        die("slot %d is not in %s and not in roster.txt: nothing to remove" % (slot, MANIFEST))
    if info is None:
        print("note: slot %d (%s) has no manifest: only its roster line is removed (files of a hand-made slot are left alone)" % (slot, code))
    else:
        for rel in info["files"]:
            if out.remove(rel):
                log("removed " + rel)
        base, orig, blobs = shared_g3(mods, pkg)
        shared = {k for s, v in man["slots"].items() if s != str(slot) for k in v.get("g3", {}).values()}
        for k in info.get("g3", {}).values():
            if k < len(blobs) and k not in shared:      # a logo another character shares stays
                blobs[k] = b""
        trim(blobs, orig)
        others = any(v.get("g3") for s, v in man["slots"].items() if s != str(slot))
        g3rel = "by_crc/%08x.bin" % pe.package_crc(ca.G3)
        if others or len(blobs) > orig:
            out.write(g3rel, mp.build(base, blobs))
        elif out.remove(g3rel):
            log("removed " + g3rel)
        rec = info.get("text")
        if rec is not None:
            others_t = [v.get("text") for s, v in man["slots"].items() if s != str(slot) and v.get("text") is not None]
            for lang, (o, c, pk) in text_packs(mods, pkg).items():
                c = list(c)
                if rec < len(c):
                    c[rec] = ""
                while len(c) > len(o) and not c[-1]:
                    c.pop()
                rel = TEXT_PACK % lang
                if others_t or len(c) > len(o) or c != o:
                    out.write(rel, text_blob(pk, c))
                elif out.remove(rel):
                    log("removed " + rel)
        del man["slots"][str(slot)]
    lines = roster_edit(lines, slot, None)
    write_roster(out, lines)
    rebuild_mainbin(out, pkg, mods, lines, log)
    save_manifest(out, man)


# ---------------------------------------------------------------------------------------------

def checklist(a, r):
    code, like = a["code"], r["like_code"]
    items = []
    items.append(("body models (obj/p_%s_{1p,2p,3p,zako}.gmo)" % code, bool(r["model"]), "copy of %s's models (--model body.gmo)" % like))
    items.append(("move set (obj/p_%s.objx)" % code, bool(r["objx"]), "copy of %s's moves and skeleton (--objx moves.objx)" % like))
    items.append(("grid tile, slide-in, VS face and face strips", r["has_images"],
                  "%s's art (give --portrait / --slidein)" % like))
    items.append(("in-battle HUD portraits (chara_image/p_%s_{1p,2p,3p}.gim)" % code, r["has_images"],
                  "%s's face (give --portrait / --slidein; --hud-crop, --hud-from)" % like))
    items.append(("game logo plate", r["logo"], "%s's logo (give --series and/or --logo)" % like))
    if r["logo"] and not r["logo_image"]:
        items.append(("logo artwork", False, "plain text plate from --series; --logo picture.png gives real artwork"))
    for what, why in (
            ("voice banks (voice_p_%s_00.scd, _a_00.scd, event .at3)" % code, "%s's voice (no tool for new banks)" % like),
            ("talk event portraits (menu/jp/talkevent/tim2/chara/{b,t}_talk_%s.gim, framed 128x128, story talk events only)" % code, "%s's face" % like),
            ("arcade select and arcade last gims", "%s's pictures" % like),
            ("d_%s.gmo/.objx and the _a/_w assist/weapon files" % code, "%s's" % like)):
        items.append((what, False, why))
    return items


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--work", required=True, help="psprecomp work directory (holds disc0/)")
    p.add_argument("--mods", required=True, help="mods directory (PSPRECOMP_MODS)")
    p.add_argument("--slot", type=int, required=True, help="charId slot: 33 (retail SEV120), 34 (Noctis), 35..42 (experimental)")
    p.add_argument("--code", help="character code, up to 6 letters/digits, e.g. viv100")
    p.add_argument("--like", help="retail character to copy: code (eht110) or charId")
    p.add_argument("--name", help="display name")
    p.add_argument("--full-name", help="name for the VS / encounter text when it differs from --name")
    p.add_argument("--series", help='e.g. "FINAL FANTASY IX" (logo plate text)')
    p.add_argument("--logo", help="logo artwork picture (89x62 block)")
    p.add_argument("--portrait", help="head-and-shoulders picture: grid tile, VS face, face strips")
    p.add_argument("--slidein", help="full body render for the Player Select slide-in (also used for the face when no portrait)")
    p.add_argument("--model", help="body .gmo replacing the 1p/2p/3p/zako models")
    p.add_argument("--objx", help="move/skeleton .objx replacing p_<code>.objx")
    p.add_argument("--tile-crop", dest="tile_crop", help="x0,y0,x1,y1 in portrait pixels for the grid tile")
    p.add_argument("--art-crop", dest="art_crop", help="x0,y0,x1,y1 in slide-in picture pixels")
    p.add_argument("--face-crop", dest="face_crop", help="x0,y0,x1,y1 for the 128x32 face strips")
    p.add_argument("--hud-crop", dest="hud_crop", help="x0,y0,x1,y1 in source pixels for the in-battle HUD portrait (default: head found automatically)")
    p.add_argument("--hud-from", dest="hud_from", choices=("portrait", "slidein"), help="picture the HUD portrait is cut from (default portrait; a "
                   "render with a transparent background gives a retail style cut-out, a portrait with a background an oval cameo)")
    p.add_argument("--hud-flip", dest="hud_flip", action=argparse.BooleanOptionalAction, default=None,
                   help="mirror the HUD portrait source (--hud-crop stays in the unmirrored picture's pixels). The HUD never mirrors a portrait: "
                        "the same texture shows on the P1 frame (bottom left) and on the CPU frame (bottom right), so a figure looking right "
                        "faces the arena on the P1 side and looks outward on the CPU side. --no-hud-flip undoes it")
    p.add_argument("--art-faces", dest="art_faces", choices=("left", "right"), help="which way the slide-in figure looks (default left, like "
                   "most retail slide-ins). The battle setup panel draws the player's portrait on the left and the opponent's on the right "
                   "without mirroring either; a mirrored copy is made for the side the figure would look away on (roster.txt artl= / artr=)")
    p.add_argument("--remove", action="store_true", help="take the slot out again")
    p.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    p.add_argument("--force", action="store_true", help="take over a slot whose roster.txt line has another code")
    return p.parse_args(argv)


def main(argv):
    ns = parse_args(argv)
    work = os.path.abspath(ns.work)
    mods = os.path.abspath(ns.mods)
    disc = os.path.join(work, "disc0")
    if not os.path.exists(os.path.join(disc, "PSP_GAME", "USRDIR", "DATA", "PACKAGE_INFO.BIN")):
        die("%s has no PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN (is --work a psprecomp work directory?)" % disc)
    pkg = pe.Package(disc)
    rmax = max_slots(os.path.join(HERE, ".."))
    ok, notes = check_slot(ns.slot, rmax)
    if not ok:
        die(notes[0])
    man = load_manifest(mods)
    out = Out(mods, ns.dry_run)
    prefix = "[dry-run] " if ns.dry_run else ""
    log = lambda s: print(prefix + s)       # noqa: E731

    if ns.remove:
        remove_slot(ns.slot, pkg, out, man, mods, log)
        print("%sslot %d removed: %d files deleted, %d shared files rewritten" % (prefix, ns.slot, len(out.removed), len(out.written)))
        return 0

    a = vars(ns).copy()
    prev = man["slots"].get(str(ns.slot))
    if prev:                                  # inherit what the earlier run was given
        for k in ("code", "like", "name", "series", "full_name"):
            if a.get(k) is None:
                a[k] = prev["like_code" if k == "like" else k]
        for k, v in prev.get("args", {}).items():
            if a.get(k) is None:
                a[k] = v
    for k in ("code", "like", "name"):
        if not a.get(k):
            die("--%s is required for a new slot" % k)
    a["code"] = a["code"].lower()
    if not re.match(r"^[a-z][a-z0-9]{0,5}$", a["code"]):
        die("--code %r: 1..6 lower case letters/digits, starting with a letter (chrp holds 'P_' + 6 characters)" % a["code"])
    if len(a["name"]) > 28:
        die("--name is too long for the name plate")
    for k in ("portrait", "slidein", "logo", "model", "objx"):
        if a.get(k):
            a[k] = os.path.abspath(a[k])
            if not os.path.exists(a[k]):
                die("--%s: %s does not exist" % (k, a[k]))
    lines = read_roster_lines(mods)
    taken = roster_find(lines, ns.slot)
    if taken and taken.lower() != a["code"] and not ns.force:
        die("slot %d is already used by %s in roster.txt (use --force to replace it, or --remove first)" % (ns.slot, taken))
    for s, v in man["slots"].items():
        if s != str(ns.slot) and v["code"] == a["code"]:
            die("code %s is already used by slot %s" % (a["code"], s))
    a["slot"] = ns.slot
    a["mods"] = mods
    r = apply_slot(a, pkg, out, man, log)
    if not ns.dry_run:
        save_manifest(out, man)

    print()
    print("%sslot %d  %s  (%s)  like %s = charId %d" % (prefix, ns.slot, a["code"], a["name"], r["like_code"], r["like_id"]))
    print("roster.txt: " + r["line"])
    for n in notes:
        print("note: " + n)
    print("note: the slot is listed in Quick Battle only when %s (charId %d) is in the player's selectable list: the runtime clones its "
          "entry as the unlock. Locked in the save -> no tile (test with PSPRECOMP_ROSTER_UNLOCK_ALL=1)." % (r["like_code"], r["like_id"]))
    if ns.slot == 33:
        print("note: runtime/roster.cpp treats charId 33 as a retail slot (kFirstNew = 34): only its code string is overridden, no grid row/list "
              "entry is added, so it appears only if the save already lists charId 33. Use 34..42 for a visible slot.")
    print("%d files for the slot, %d of them copies of %s's retail files; %d file writes in all (%d unchanged), %d bytes" % (
        len(r["files"]), r["n_alias"], r["like_code"], len(out.written) + len(out.unchanged), len(out.unchanged), out.bytes))
    print("shared: by_crc/%08x.bin (menu pack, tile/art/name/logo), by_crc/%08x.bin (main.bin chrp/objentry, %s bytes), "
          "text/*/mess_pk_chara_select_0.bin (name record)" % (pe.package_crc(ca.G3), rmb.MAIN_CRC, r["mainbin"]))
    if ns.dry_run:
        for f in r["files"]:
            print("  " + f)
    print("\nplaceholder checklist (what is still %s's):" % r["like_code"])
    for what, done, why in checklist(a, r):
        print("  [%s] %s%s" % ("x" if done else " ", what, "" if done else " - " + why))
    env = "PSPRECOMP_MODS=%s" % mods
    print("\nrun with: " + env)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
