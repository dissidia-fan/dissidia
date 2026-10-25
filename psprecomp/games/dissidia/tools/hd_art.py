#!/usr/bin/env python3
"""hd_art.py - high resolution texture-pack replacements for a new character's pictures (and, with --redraw, the
game-size pictures themselves from a new source picture).

new_character.py draws the game's textures at the PSP's size (slide-in 256x256, HUD portrait 128x128, grid tile
32x64, ...). With a texture pack the retail characters are drawn from hand-made high resolution files instead, which
makes ours look soft next to them. This draws the same pictures again from the same source pictures with the same
framing (crops, mirrored copies, HUD clip), `--scale` times larger, and names each file after the texture-pack id of
the game texture it stands in for (texpack_id.py), so the runtime swaps it in wherever that texture is drawn.

    hd_art.py --mods work/mods --slot 34 --out work/mods_textures_pack [--slidein PIC] [--portrait PIC] [--scale 4]
              [--redraw [--art-faces left|right]]

Settings (pictures, crops, --hud-flip, --hud-from, --art-faces) come from <mods>/readme_newchars.json; --slidein /
--portrait replace the recorded picture paths. Without --redraw the pictures must be the ones the slot was built from,
or the replacements will not line up with the textures they replace. With --redraw the game-size pictures are drawn
again from the pictures given (same pack entries and files, nothing else of the slot is touched) and the manifest and
roster.txt are updated; a backup is the caller's job.

Textures covered (each only when its source picture is available; a slot built with a --portrait needs it for the
grid tile, the face strips and the VS face, and for the HUD portrait unless that came from the slide-in):

    grid tile 32x64 (g3 'tile')            slide-in 256x256 (g3 'art') and its mirrored copy (g3 'artl' / 'artr')
    face strips 128x32 (chara_face_in_name, _r)   VS face 64x64 (story_mode/chara2)   HUD portrait 128x128 (chara_image)

Name plates and the logo are text / supplied artwork and are not covered. Requires Pillow, numpy.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
if __name__ == "__main__":
    import pydeps                    # numpy / Pillow / scipy at the pinned versions (tools/pydeps.py)
    pydeps.require()
import character_art as ca      # noqa: E402
import gim_encode as ge         # noqa: E402
import menu_pack as mp          # noqa: E402
import new_character as nc      # noqa: E402
import package_extract as pe    # noqa: E402
import texpack_id as tp         # noqa: E402
from PIL import Image           # noqa: E402

FACE_IN = "menu/jp/common/chara_face_in_name/p_%s_face_in_name.gim"
FACE_IN_R = "menu/jp/common/chara_face_in_name_r/p_%s_face_in_name_r.gim"
VS_FACE = "menu/jp/story_mode/chara2/p_%s_1p.gim"
HUD = "menu/%s/battle/chara_image/p_%s_%dp.gim"


def mirror_like(hd, low_gim, scale):
    """Mirror `hd` the way gim_encode.mirror mirrored the game texture: inside the game texture's bounding box."""
    info = ge.parse(low_gim)
    io, w, h, _, _ = info["img"]
    po, pw = info["pal"][0], info["pal"][1]
    solid = [low_gim[po + 4 * i + 3] > 0 for i in range(pw)]
    plane = low_gim[io:io + w * h]
    cols = [x for x in range(w) if any(solid[plane[y * w + x]] for y in range(h))]
    if not cols:
        return hd
    x0, x1 = cols[0] * scale, (cols[-1] + 1) * scale
    out = hd.copy()
    out.paste(hd.crop((x0, 0, x1, hd.height)).transpose(Image.FLIP_LEFT_RIGHT), (x0, 0))
    return out


def retail_tile(slot):
    """The decoded retail grid tile of the character the slot was cloned from (its silhouette), or None."""
    try:
        pkg = pe.Package(os.path.join(HERE, "..", "..", "..", "work", "disc0"))
        like = nc.resolve_like(pkg, slot["like_code"])[0] if slot.get("like_code") else 9
        like = like if 1 <= like <= 32 else 9
        return ge.decode(mp.entry(mp.parse(pkg.read("menu/menu_pk_free_battle_gim_2.bin")), 12 + like))
    except Exception:                    # noqa: BLE001
        return None


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--mods", required=True)
    p.add_argument("--slot", type=int, required=True)
    p.add_argument("--out", required=True, help="texture pack folder (<mods>/textures, or the PSPRECOMP_TEXPACK folder)")
    p.add_argument("--slidein", help="full body picture (default: the recorded path)")
    p.add_argument("--portrait", help="head-and-shoulders picture (default: the recorded path)")
    p.add_argument("--scale", type=int, default=4)
    p.add_argument("--redraw", action="store_true", help="draw the game-size pictures again from the pictures given")
    p.add_argument("--only", help="comma list of tile, art, strips, vsface, hud: limit the redraw and the HD files to these")
    p.add_argument("--tile-crop", dest="tile_crop", help="with --redraw: new x0,y0,x1,y1 for the grid tile, in the picture's pixels")
    p.add_argument("--vs-crop", dest="vs_crop", help="with --redraw: x0,y0,x1,y1 (square) for the 64x64 VS face, in the picture's pixels")
    p.add_argument("--tile-flip", dest="tile_flip", action=argparse.BooleanOptionalAction, default=None,
                   help="with --redraw: mirror the picture for the grid tile")
    p.add_argument("--tile-cutout", dest="tile_cutout", action=argparse.BooleanOptionalAction, default=None,
                   help="with --redraw: retail-style tile, the figure cut out over the grid (no dark box)")
    p.add_argument("--art-faces", dest="art_faces", choices=("left", "right"),
                   help="with --redraw: which way the new slide-in looks (puts the mirrored copy on the other side)")
    ns = p.parse_args(argv)
    mods = os.path.abspath(ns.mods)
    man = nc.load_manifest(mods)
    slot = man["slots"].get(str(ns.slot))
    if not slot:
        nc.die("slot %d is not in %s" % (ns.slot, os.path.join(mods, nc.MANIFEST)))
    code, s = slot["code"], ns.scale
    args = dict(slot.get("args", {}))
    had_portrait = bool(args.get("portrait"))
    only = set((ns.only or "tile,art,strips,vsface,hud").replace(" ", "").split(","))
    bad = only - {"tile", "art", "strips", "vsface", "hud"}
    if bad:
        nc.die("--only: unknown %s" % ", ".join(sorted(bad)))
    for k in ("tile_crop", "tile_flip", "tile_cutout", "vs_crop"):
        v = getattr(ns, k)
        if v is not None:
            if not ns.redraw:
                nc.die("--%s needs --redraw" % k.replace("_", "-"))
            args[k] = v
    for k in ("slidein", "portrait"):
        v = getattr(ns, k)
        if v:
            args[k] = os.path.abspath(v)
        elif args.get(k) and not os.path.exists(args[k]):
            print("%s %s is not here (pass --%s)" % (k, args[k], k))
            args.pop(k)
    art = nc.Art(args)
    if not art.any:
        nc.die("no source picture: pass --slidein and/or --portrait")
    face_ok = art.portrait is not None or not had_portrait        # tile / strips / VS face come from the portrait
    hud_ok = art.slidein is not None if (art.hud_from == "slidein" or not had_portrait) else art.portrait is not None
    if ns.redraw and not (face_ok and hud_ok):
        nc.die("--redraw needs every picture the slot uses (it was built with a --portrait too)")

    g3_path = os.path.join(mods, "by_crc", "%08x.bin" % pe.package_crc(ca.G3))
    g3 = mp.parse(open(g3_path, "rb").read())
    blobs = [mp.entry(g3, k) for k in range(len(g3["ents"]))]
    ent = slot.setdefault("g3", {})
    files = {}
    for rel in [FACE_IN % code, FACE_IN_R % code, VS_FACE % code] + [HUD % (L, code, n) for L in ca.LANGS for n in (1, 2, 3)]:
        f = os.path.join(mods, rel)
        if os.path.exists(f):
            files[rel] = open(f, "rb").read()

    if ns.redraw:
        faces = ns.art_faces or args.get("art_faces") or "left"
        if "tile" in ent and "tile" in only:
            tpl = ge.decode(blobs[ent["tile"]])
            if args.get("tile_cutout"):          # the silhouette comes from the retail tile this one was cloned from
                tpl = retail_tile(slot) or tpl
            blobs[ent["tile"]] = ge.encode(blobs[ent["tile"]], art.tile(tpl))
        if "art" in ent and "art" in only:
            blobs[ent["art"]] = ge.encode(blobs[ent["art"]], art.art((256, 256)))
            side, other = "artl", "artr"    # the setup panel's player side; was the side the picture looks away from
            if other in ent and side not in ent:
                ent[side] = ent.pop(other)
            if side in ent:
                blobs[ent[side]] = blobs[ent["art"]]     # an exact copy: own entry for the player's side, not mirrored (22 Oct)
                lines = nc.read_roster_lines(mods)
                for i, ln in enumerate(lines):
                    tok = ln.split("#")[0].split()
                    if tok and tok[0] == str(ns.slot):
                        ln = re.sub(r"\s+art[lr]=\d+", "", ln)
                        lines[i] = re.sub(r"(\bart=\d+)", r"\1 %s=%d" % (side, g3["base"] + ent[side] - ca.BASE_ART), ln, count=1)
                        print("roster.txt: " + lines[i])
                open(os.path.join(mods, "roster.txt"), "w").write("\n".join(l for l in lines).rstrip("\n") + "\n")
        open(g3_path, "wb").write(mp.build(g3, blobs))
        for rel, d in list(files.items()):
            if rel == FACE_IN % code or rel == FACE_IN_R % code:
                if "strips" not in only:
                    continue
                im = art.strip(ge.decode(d), rel == FACE_IN_R % code)
            elif rel == VS_FACE % code:
                if "vsface" not in only:
                    continue
                im = art.square_face(ge.decode(d).size)
            else:
                if "hud" not in only:
                    continue
                im = art.hud()
            files[rel] = ge.encode(d, im)
            open(os.path.join(mods, rel), "wb").write(files[rel])
        for k in ("slidein", "portrait", "tile_crop", "tile_flip", "tile_cutout", "vs_crop"):
            if args.get(k):
                slot["args"][k] = args[k]
            elif k in ("tile_flip", "tile_cutout") and getattr(ns, k) is False:
                slot["args"].pop(k, None)
        slot["args"]["art_faces"] = faces
        man["hud_convention"] = nc.HUD_CONVENTION
        open(os.path.join(mods, nc.MANIFEST), "w").write(json.dumps(man, indent=1, sort_keys=True) + "\n")
        print("redrawn: the slot's pictures in %s, manifest and roster.txt updated" % g3_path)

    os.makedirs(ns.out, exist_ok=True)
    wrote = {}

    def put(low_gim, make, what):
        tid, w, h = tp.gim_id(low_gim)
        name = "%016x.png" % tid
        if name in wrote:
            return
        im = make()
        assert im.size == (w * s, h * s), (what, im.size, w, h)
        im.save(os.path.join(ns.out, name))
        wrote[name] = what
        print("  %s  %dx%d -> %dx%d  %s" % (name, w, h, im.width, im.height, what))

    if face_ok and "tile" in ent and "tile" in only:
        tpl = (retail_tile(slot) if args.get("tile_cutout") else None) or ge.decode(blobs[ent["tile"]])
        put(blobs[ent["tile"]], lambda: art.tile(tpl, scale=s), "grid tile")
    if "art" in ent and "art" in only:
        low = blobs[ent["art"]]
        hd = art.art((256, 256), scale=s)
        put(low, lambda: hd, "slide-in")
        for side in ("artl", "artr"):
            if side in ent:
                put(blobs[ent[side]], lambda: hd, "slide-in copy (%s)" % side)
    for rel, d in files.items():
        if rel in (FACE_IN % code, FACE_IN_R % code):
            if face_ok and "strips" in only:
                put(d, lambda: art.strip(ge.decode(d), rel == FACE_IN_R % code, scale=s), "face strip" + (" (right)" if rel == FACE_IN_R % code else ""))
        elif rel == VS_FACE % code:
            if face_ok and "vsface" in only:
                put(d, lambda: art.square_face(ge.decode(d).size, scale=s), "VS face")
        elif hud_ok and "hud" in only:
            put(d, lambda: art.hud(scale=s), "HUD portrait")
    print("%d file(s) in %s" % (len(wrote), ns.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
