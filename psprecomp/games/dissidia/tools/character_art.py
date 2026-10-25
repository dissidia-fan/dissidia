#!/usr/bin/env python3
"""Build the Quick Battle "Player Select" assets of a new character from one full body picture.

    character_art.py <disc0|DATA dir> <mods dir> <image.png|webp> <Name> [--tile-crop x0,y0,x1,y1]
                     [--art-crop x0,y0,x1,y1] [--face-crop x0,y0,x1,y1] [--hud-crop x0,y0,x1,y1] [--hud-figure 1] [--vs-only 1] [--full-name "Noctis Lucis Caelum"] [--like 9] [--logo XV] [--logo-image logo.webp] [--code noc100]

Reads the original packs from PACKAGE.BIN, then writes into <mods>:

    by_crc/<crc of menu/menu_pk_free_battle_gim_3.bin>.bin
        the original pack with three entries appended (texture entity ids 0x7101, 0x7102, 0x7103), four with --logo:
            tile  32x64 grid tile        (face crop, silhouette copied from an original tile)
            art   256x256 slide-in       (upper body, right aligned)
            name  128x32 name plate      ("Name" rendered like the lg0 plates)
            logo  128x64 game logo plate ("FINAL FANTASY <numeral>", --logo only; the roster.txt line gets logo=N,
                  N = entity id - 0x706F, because the grid record's f2 selects the logo entity 0x706F + f2)
    menu/jp/common/chara_face_in_name/p_<code>_face_in_name.gim and
    menu/jp/common/chara_face_in_name_r/p_<code>_face_in_name_r.gim   (--code only: the 128x32 face strip of the VS /
        battle prep screens, mirrored for the right-hand side)
    menu/jp/battle/chara_image/p_<code>_{1p,2p,3p}.gim   (--code only: the in-battle HUD portrait, a cut-out in the retail box)
    text/<lang>/mess_pk_chara_select_0.bin
        entry 0 with the name appended (record N), used by the info/loading panels

and prints the roster.txt line to use. Entity ids are looked up as 0x704F + name (plates),
0x700C + art and 0x702F + tile, so the indices below are derived, not guessed.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if __name__ == "__main__":
    import pydeps                    # numpy / Pillow / scipy at the pinned versions (tools/pydeps.py)
    pydeps.require()
import gim_encode as ge          # noqa: E402
import menu_pack as mp           # noqa: E402
import mess_text as mt           # noqa: E402
import package_extract as pe     # noqa: E402
from PIL import Image            # noqa: E402

G3 = "menu/menu_pk_free_battle_gim_3.bin"
BASE_LOGO = 0x706F                                          # entity id of logo index 0 (f2 of the grid record; FFII is 1)
BASE_ART, BASE_TILE, BASE_NAME = 0x700C, 0x702F, 0x704F      # entity id of index 0 for f0 (art/tile) and f1 (plate)
LANGS = ("en", "fr", "de", "it", "es", "jp")
FACE_IN_TEMPLATE = "fiv100"                                  # a retail character with both face-in textures


def _crop(opts, k):
    return tuple(int(v) for v in opts[k].split(",")) if k in opts else None


def vs_assets(pkg, mods, src, name, code, opts):
    # The story-style VS screen (battle overlay FUN_08AF6B70) loads, per side, the name text menu/<lang>/common/chara_name/
    # p_<code>_name.gim (128x32) and the face tile menu/jp/story_mode/chara2/p_<code>_1p.gim (64x64); without them the
    # left box stays empty. The language folder is the one the retail text pack exists for.
    for lang in LANGS:
        t_path = "menu/%s/common/chara_name/p_%s_name.gim" % (lang, FACE_IN_TEMPLATE)
        try:
            t_nm = pkg.read(t_path)
        except KeyError:
            continue
        dst = os.path.join(mods, "menu/%s/common/chara_name/p_%s_name.gim" % (lang, code))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(ge.encode(t_nm, ge.render_name(opts.get("full-name", name), (128, 32))))
        print("wrote", dst)
    t_path = "menu/jp/story_mode/chara2/p_%s_1p.gim" % FACE_IN_TEMPLATE
    try:
        t_1p = pkg.read(t_path)
    except KeyError:
        t_1p = None
    if t_1p is not None:
        dec = ge.decode(t_1p)
        face = ge.prepare(src, dec.size, "tile", _crop(opts, "tile-crop"))   # no template: a plain opaque square
        dst = os.path.join(mods, "menu/jp/story_mode/chara2/p_%s_1p.gim" % code)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(ge.encode(t_1p, face))
        print("wrote", dst)


def hud_assets(pkg, mods, src, code, opts, template=FACE_IN_TEMPLATE):
    # In-battle HUD portrait menu/jp/battle/chara_image/p_<code>_{1p,2p,3p}.gim (128x128 cut-out in the retail box, see
    # gim_encode.frame_hud). --hud-crop is in source pixels; --hud-figure 1 treats the picture as a full body render.
    im = ge.frame_hud(src, (128, 128), _crop(opts, "hud-crop"), head_picture="hud-figure" not in opts)
    for n in (1, 2, 3):
        t_path = "menu/jp/battle/chara_image/p_%s_%dp.gim" % (template, n)
        try:
            t = pkg.read(t_path)
        except KeyError:
            continue
        dst = os.path.join(mods, "menu/jp/battle/chara_image/p_%s_%dp.gim" % (code, n))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(ge.encode(t, im))
        print("wrote", dst)


def main(argv):
    pos, opts, i = [], {}, 0
    while i < len(argv):
        if argv[i].startswith("--"):
            opts[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            pos.append(argv[i])
            i += 1
    if len(pos) < 4:
        print(__doc__)
        return 2
    disc, mods, image, name = pos[:4]
    pkg = pe.Package(disc)
    g0 = mp.parse(pkg.read("menu/menu_pk_free_battle_gim_0.bin"))
    g2 = mp.parse(pkg.read("menu/menu_pk_free_battle_gim_2.bin"))
    g3d = pkg.read(G3)
    g3 = mp.parse(g3d)
    lg = mp.parse(pkg.read("menu/en/menu_lpk_free_battle_gim_0.bin"))
    if "vs-only" in opts:                                  # only the VS screen textures (no pack rebuild)
        vs_assets(pkg, mods, Image.open(image), name, opts["code"], opts)
        hud_assets(pkg, mods, Image.open(image), opts["code"], opts)
        return 0
    like = int(opts.get("like", 9))                       # original character whose gims are the templates
    t_tile, t_art, t_name = mp.entry(g2, 12 + like), mp.entry(g0, 12 + like), mp.entry(lg, 79 + like - 1)
    crop = lambda k: tuple(int(v) for v in opts[k].split(",")) if k in opts else None
    src = Image.open(image)
    tile = ge.encode(t_tile, ge.prepare(src, (32, 64), "tile", crop("tile-crop"), template=ge.decode(t_tile)))
    art = ge.encode(t_art, ge.prepare(src, (256, 256), "art", crop("art-crop")))
    plate = ge.encode(t_name, ge.render_name(name, (128, 32)))
    blobs = [mp.entry(g3, k) for k in range(len(g3["ents"]))]
    n0 = len(blobs)
    blobs += [tile, art, plate]
    logo_blob = None
    if "logo" in opts:                                     # --logo XV : game logo plate (128x64, g2 entries 77..)
        t_logo = mp.entry(g2, 77 + 3)                      # FFV's plate is the template (any 128x64 plate works)
        logo_blob = ge.encode(t_logo, ge.render_logo(opts["logo"], (128, 64), ge.decode(t_logo), image=Image.open(opts["logo-image"]) if "logo-image" in opts else None))
        blobs.append(logo_blob)
    pk_id = lambda k: g3["base"] + k                        # entity id of g3 entry k
    tile_idx = pk_id(n0) - BASE_TILE
    art_idx = pk_id(n0 + 1) - BASE_ART
    name_idx = pk_id(n0 + 2) - BASE_NAME
    out = os.path.join(mods, "by_crc", "%08x.bin" % pe.package_crc(G3))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(mp.build(g3, blobs))
    if "code" in opts:                                     # VS / battle prep face strips (loaded by path)
        code = opts["code"]
        for sub, suffix, flip in (("chara_face_in_name", "face_in_name", False), ("chara_face_in_name_r", "face_in_name_r", True)):
            t_path = "menu/jp/common/%s/p_%s_%s.gim" % (sub, FACE_IN_TEMPLATE, suffix)
            try:
                t_fi = pkg.read(t_path)
            except KeyError:
                continue
            face = ge.prepare(src, (128, 32), "face_in", crop("face-crop"), template=ge.decode(t_fi))
            if flip:
                face = face.transpose(Image.FLIP_LEFT_RIGHT)
            dst = os.path.join(mods, "menu/jp/common/%s/p_%s_%s.gim" % (sub, code, suffix))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(ge.encode(t_fi, face))
            print("wrote", dst)
        vs_assets(pkg, mods, src, name, code, opts)
        hud_assets(pkg, mods, src, code, opts)
    rec = None
    for lang in LANGS:
        p = "text/%s/mess_pk_chara_select_0.bin" % lang
        try:
            data = pkg.read(p)
        except KeyError:
            continue
        n = len(mt.records(mp.entry(mp.parse(data), 0)))
        newd, rec = mt.set_text(data, 0, n, name)
        dst = os.path.join(mods, p)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(newd)
    print("wrote", out)
    extra = ""
    if logo_blob is not None:
        extra = " logo=%d" % (pk_id(n0 + 3) - BASE_LOGO)
    print("roster.txt: <charId> <code> %d tile=%d art=%d name=%d text=%s%s" % (like, tile_idx, art_idx, name_idx, rec, extra))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
