#!/usr/bin/env python3
"""Encode PNG/WebP images into Dissidia 012 menu GIM textures (and decode them to PNG).

The menu GIMs used for character portraits are 'MIG.00.1PSP' files with one 8-bit indexed image
(format 5, not swizzled) plus one 256 entry RGBA8888 palette (format 3). In both blocks the data starts
0x40 bytes after the block's data header (the engine overwrites the 16 bytes before it with a pointer):

    Player Select slide-in art    256x256   (menu_pk_free_battle_gim_{0,1,2}.bin, one pack entry each)
    Player Select grid tile        32x64    (menu_pk_free_battle_gim_2.bin entries 12..43)
    battle loading portrait       see 'menu/jp/battle/chara_image/p_<code>_1p.gim'

An encoded file is made by patching a *template* GIM of the wanted size (any original of that size):
the header blocks stay byte for byte, only the index plane and the palette are replaced, so the
result has exactly the original layout and size.

    gim_encode.py decode  <in.gim> <out.png>
    gim_encode.py mirror  <in.gim> <out.gim>                    (left-right, inside the picture's bounding box)
    gim_encode.py name    <template.gim> "Noctis" <out.gim>       (name plate image, 128x32 lg0 style)
    gim_encode.py encode  <template.gim> <in.png|webp> <out.gim> [--mode art|tile|fit|stretch]
                          [--crop x0,y0,x1,y1] [--shift dx,dy]

modes
    art      source is a full body picture: crop the upper body (head at the top, cut near the waist),
             scale to the template square and push the figure to the right edge like the originals
    tile     tight face crop (head and a little neck) for the 32x64 tile texture
    hud      in-battle HUD portrait (128x128, chara_image): head-and-shoulders picture covered into the retail
             box, clipped by the retail ellipse; hud_figure = the same from a full body picture
    fit      keep the aspect ratio, centre, transparent padding
    stretch  scale to the template size
Requires Pillow.
"""
import os
import struct
import sys

if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pydeps                    # numpy / Pillow / scipy at the pinned versions (tools/pydeps.py)
    pydeps.require()

from PIL import Image


def blocks(d, off, end):
    while off < end:
        bid, _, size, nxt, doff = struct.unpack_from("<HHIII", d, off)
        yield bid, off, size
        if size == 0:
            break
        off += size


def parse(d):
    """Return {'img': (offset, w, h, fmt, order), 'pal': (offset, n, fmt)} (offsets of the pixel data)."""
    assert d[:11] == b"MIG.00.1PSP", "not a GIM"
    out = {}

    def walk(off, end):
        for bid, o, size in blocks(d, off, end):
            if bid in (2, 3):
                walk(o + 16, o + size)
            elif bid in (4, 5):
                h = o + 16
                hs, _, fmt, order, w, hh = struct.unpack_from("<HHHHHH", d, h)
                plane = struct.unpack_from("<I", d, h + 0x1C)[0]   # data starts 0x40 after the header start
                key = "img" if bid == 4 else "pal"
                out[key] = (h + plane, w, hh, fmt, order)

    walk(0x10, len(d))
    return out


def decode(d):
    info = parse(d)
    po, pw, _, pfmt, _ = info["pal"]
    io, w, h, fmt, _ = info["img"]
    assert pfmt == 3 and fmt == 5, "only I8 + RGBA8888 supported (fmt %d pal %d)" % (fmt, pfmt)
    pal = [tuple(d[po + 4 * i:po + 4 * i + 4]) for i in range(pw)]
    im = Image.new("RGBA", (w, h))
    im.putdata([pal[b] for b in d[io:io + w * h]])
    return im


def quantize(im):
    """RGBA -> (palette[256] of RGBA tuples, index bytes).

    Palette entry 255 is reserved as the fully transparent colour (255,255,255,0), the same value the
    original tiles use for their background, and every texel with (almost) no alpha maps to it. Without
    this the quantiser lets transparent texels share an entry with dark opaque colours and the game
    draws a dark box around the picture.

    A white halo (semi-transparent near-white texels, `add_glow`) gets its own ramp of HALO_LEVELS white entries just
    below 255: left to the octree, its faint outer rings merge with the transparent white and the halo is cut short."""
    import numpy as np
    im = im.convert("RGBA")
    src = np.asarray(im, dtype=np.int32).reshape(-1, 4)
    clear_m = src[:, 3] < 10
    halo_m = (~clear_m) & (src[:, 3] < 235) & (src[:, :3].min(1) >= 235)
    ramp = []
    if halo_m.sum() >= 64:
        ramp = [(255, 255, 255, int(round(v))) for v in np.linspace(10, 232, HALO_LEVELS)]
    ncol = 255 - len(ramp)
    # give transparent (and halo) texels a neutral colour so they do not pull the palette around
    keep = ~clear_m & ~(halo_m if ramp else np.zeros_like(halo_m))
    flat_a = np.asarray(im).copy().reshape(-1, 4)
    flat_a[~keep] = (255, 255, 255, 0)
    flat = Image.fromarray(flat_a.reshape(im.size[1], im.size[0], 4), "RGBA")
    q = flat.quantize(colors=ncol, method=Image.FASTOCTREE, dither=Image.NONE)
    raw = q.getpalette("RGBA") or []
    pal = [tuple(raw[4 * i:4 * i + 4]) for i in range(len(raw) // 4)][:ncol]
    pal += [(0, 0, 0, 0)] * (ncol - len(pal))
    # keep opaque texels exactly opaque (the quantiser averages alpha of near-opaque colours)
    pal = [(r, g, b, 255 if a >= 235 else (0 if a < 10 else a)) for r, g, b, a in pal]
    pal += ramp
    pal.append((255, 255, 255, 0))
    ix = np.frombuffer(bytes(q.getdata()), dtype=np.uint8).astype(np.int32).copy()
    ix[clear_m] = 255
    if ramp:
        lv = np.asarray([c[3] for c in ramp])
        ix[halo_m] = ncol + np.abs(src[halo_m, 3][:, None] - lv[None, :]).argmin(1)
    # The octree works on RGBA, so a pure-white highlight inside the figure can share a node with the transparent white
    # ground: that entry's alpha is averaged down and the highlight turns into a see-through hole (Vivi's hat, 22 Oct).
    # Every visible texel whose entry is off by more than a quarter in alpha is moved to the nearest entry that matches.
    P = np.asarray(pal, dtype=np.int32)
    bad = (~clear_m) & (np.abs(P[ix, 3] - src[:, 3]) > 64)
    if bad.any():
        cand = P[:255]
        cols, inv = np.unique(src[bad], axis=0, return_inverse=True)
        d = ((cols[:, None, :3] - cand[None, :, :3]) ** 2).sum(-1) + 4 * (cols[:, None, 3] - cand[None, :, 3]) ** 2
        ix[np.flatnonzero(bad)] = d.argmin(1)[inv.reshape(-1)]
    return pal, ix.astype(np.uint8).tobytes()


HALO_LEVELS = 16


def encode(template, im):
    info = parse(template)
    io, w, h, fmt, _ = info["img"]
    po, pw, _, pfmt, _ = info["pal"]
    assert pfmt == 3 and fmt == 5 and pw == 256
    if im.size != (w, h):
        raise ValueError("image is %dx%d, template wants %dx%d" % (im.size[0], im.size[1], w, h))
    pal, idx = quantize(im.convert("RGBA"))
    out = bytearray(template)
    out[io:io + w * h] = idx
    for i, c in enumerate(pal):
        out[po + 4 * i:po + 4 * i + 4] = bytes(c)
    return bytes(out)


def _resize(im, size):
    return im.resize(size, Image.LANCZOS)


def _head_centre(src, y0, ya, yb):
    """x centre of the figure between rows ya..yb (taken from the top of the figure: the head, which
    ignores weapons held out to the side lower down)."""
    band = src.crop((0, ya, src.width, max(yb, ya + 1))).getchannel("A").point(lambda a: 255 if a > 16 else 0)
    bb = band.getbbox()
    return (bb[0] + bb[2]) // 2 if bb else src.width // 2


def frame_art(src, size, crop=None, shift=(0, 0), glow=True):
    """Slide-in framing like Cloud's original (menu_pk_free_battle_gim_1 entry 1): the head sits fully
    inside the frame near the top (hair top at ~12% of the square), the head is ~27% of the square tall,
    the body is cut around the waist at the bottom edge, and the head sits at ~65% from the left so the
    figure occupies the right part of the square. Everything is derived from the figure's bounding box
    (head height ~ 16.6% of a full body picture); pass --crop to override."""
    src = src.convert("RGBA")
    if crop is None:
        bbox = src.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox() or (0, 0) + src.size
        x0, y0, x1, y1 = bbox
        hgt = y1 - y0
        side = int(hgt * 0.60)                               # source pixels covered by the square
        cx = _head_centre(src, y0, y0 + int(hgt * 0.045), y0 + int(hgt * 0.14))
        left = cx - int(side * 0.62)
        top = y0 - int(side * 0.125)
        crop = (left, top, left + side, top + side)
    x0, y0, x1, y1 = crop
    canvas = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
    canvas.paste(src, (-x0 + shift[0], -y0 + shift[1]))
    out = _resize(canvas, size)
    return add_glow(out, size[0] / 256.0) if glow else out


# The retail slide-ins (menu_pk_free_battle_gim_0..2, all 30 measured 22 Oct) sit on a soft white halo: mean alpha of the ring
# at distance d (pixels of the 256x256 texture) from the solid figure, colour 250-255 grey, the same on every character:
#   d   1    2   3   4   5  6  7
#   a ~150  70  44  21   7  1  0      (d 1 is the figure's own anti-aliased edge)
GLOW_PROFILE = ((0, 255), (1, 120), (2, 70), (3, 44), (4, 21), (5, 7), (6, 1), (7, 0))


def add_glow(img, scale=1.0):
    """Put the retail white halo under a cut-out (alpha) picture. `scale` = texture size / 256 (HD copies)."""
    import numpy as np
    from scipy import ndimage
    im = img.convert("RGBA")
    a = np.asarray(im)[..., 3].astype(np.float32)
    d = ndimage.distance_transform_edt(a < 128) / scale
    xs, ys = zip(*GLOW_PROFILE)
    g = np.interp(d, xs, ys).astype(np.float32)
    halo = np.zeros(a.shape + (4,), np.uint8)
    halo[..., :3] = 255
    halo[..., 3] = np.clip(g, 0, 255).astype(np.uint8)
    out = Image.fromarray(halo, "RGBA")
    out.alpha_composite(im)
    return out


def tile_box_height(template, size):
    """Height of the opaque face box of a grid tile texture (rows of the template with a solid run), w when unknown."""
    w, h = size
    if template is None:
        return w
    al = template.getchannel("A")
    rows = [y for y in range(h) if sum(1 for x in range(w) if al.getpixel((x, y)) > 128) >= 4]
    return (max(r for r in rows if r < len(rows) + 4) + 1) if rows else w


def cover_crop(src, aspect, anchor_y=0.30, zoom=1.0):
    """Crop rectangle (x0, y0, x1, y1, source pixels) of aspect w/h that covers a head-and-shoulders picture: the largest
    such rectangle inside the picture's opaque bounding box (the whole picture when it has no transparency), centred
    horizontally, placed at anchor_y of the spare height (0 = top). zoom > 1 tightens the crop around the same centre."""
    src = src.convert("RGBA")
    bb = src.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox() or (0, 0) + src.size
    bw, bh = bb[2] - bb[0], bb[3] - bb[1]
    cw, ch = bw, bw / aspect
    if ch > bh:
        ch, cw = bh, bh * aspect
    cw, ch = cw / zoom, ch / zoom
    cx = (bb[0] + bb[2]) / 2
    top = bb[1] + (bh - ch) * anchor_y
    return (int(round(cx - cw / 2)), int(round(top)), int(round(cx + cw / 2)), int(round(top + ch)))


def frame_tile(src, size, crop=None, shift=(0, 0), template=None, scale=1, cutout=False):
    """Tight face crop (top of the hair to the collar/upper shoulders, face centred) for a grid tile texture (32x64: the face fills the
    opaque top block, rows 0..~46; the rest of the texture holds a small marker).

    `template` (decoded RGBA of an original tile) only gives the box height and the bottom marker block. `scale` > 1
    draws the same tile that many times larger (a texture-pack replacement); size stays the game's size.
    `cutout`: like the retail tiles, the figure keeps its own transparency (the grid shows through around it) and is
    clipped to the template's silhouette (the box with its rounded bottom); without it the box is filled edge to
    edge and empty texels get a dark backdrop."""
    src = src.convert("RGBA")
    w, h = size
    fw, fh = w, tile_box_height(template, size)
    if crop is None:
        bbox = src.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox() or (0, 0) + src.size
        x0, y0, x1, y1 = bbox
        hgt = y1 - y0
        ch = int(hgt * 0.215)                              # hairline to chin
        cw = int(ch * fw / fh)
        cx = _head_centre(src, y0, y0 + int(hgt * 0.045), y0 + int(hgt * 0.14)) - int(cw * 0.10)
        top = y0 - int(hgt * 0.004)
        crop = (cx - cw // 2, top, cx - cw // 2 + cw, top + ch)
    cw, ch = crop[2] - crop[0], crop[3] - crop[1]
    canvas = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    canvas.paste(src, (-crop[0] + shift[0], -crop[1] + shift[1]))
    w, h, fw, fh = w * scale, h * scale, fw * scale, fh * scale
    face = _resize(canvas, (fw, fh))
    # a 32 pixel wide face needs crisper detail and more punch than a straight downscale gives
    from PIL import ImageEnhance, ImageFilter
    fa = face.getchannel("A")
    rgb = face.convert("RGB").filter(ImageFilter.UnsharpMask(radius=1.0 * scale, percent=140, threshold=2))
    rgb = ImageEnhance.Contrast(rgb).enhance(1.15)
    rgb = ImageEnhance.Brightness(rgb).enhance(1.03)
    face = rgb.convert("RGBA")
    face.putalpha(fa)
    if scale > 1:      # the HD copy has the detail already: sharpening/contrast there looked harsh (Celes, Auron, 22 Oct)
        face = _resize(canvas, (fw, fh))
    # plain rectangle: the face covers the whole box (rows 0..fh-1, full width) edge to edge, opaque; no
    # silhouette from the source tile. Texels the picture leaves empty get a dark backdrop. Transparent
    # only below the box (rows >= fh), plus the template's marker block at the bottom.
    out = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    if cutout:
        box = Image.new("RGBA", (w, fh), (255, 255, 255, 0))
        box.alpha_composite(face)
        # Clip to the face box all retail tiles share: tile_box_mask.png = the union of the alpha of the 34 retail grid tiles
        # (menu_pk_free_battle_gim_0..3). Not the template's own alpha: that is the like character's figure, which cut the
        # new figure to its outline (Noctis's hair spikes, holes in Celes / Auron / Clive, 22 Oct).
        boxmask = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tile_box_mask.png")
        if os.path.exists(boxmask) or template is not None:
            mask = (Image.open(boxmask).convert("L") if os.path.exists(boxmask) else template.getchannel("A"))
            mask = mask.crop((0, 0, mask.width, fh // scale))
            mask = mask.resize((w, fh), Image.NEAREST if scale == 1 else Image.BILINEAR)
            from PIL import ImageChops
            box.putalpha(ImageChops.multiply(box.getchannel("A"), mask))
    else:
        box = Image.new("RGBA", (w, fh), (24, 24, 34, 255))
        box.alpha_composite(face)
    out.paste(box, (0, 0))
    if template is not None:
        th = template.height
        mark = template.crop((0, th - 8, template.width, th))
        if scale > 1:
            mark = mark.resize((w, 8 * scale), Image.NEAREST)
        out.paste(mark, (0, h - 8 * scale))
    return out


# In-battle HUD portrait (menu/jp/battle/chara_image/p_<code>_{1p,2p,3p}.gim, 128x128, the same layout for all three costumes).
# Measured on the retail files (17 Oct): the HUD draws the star frame itself, the texture only holds a free-form cut-out of the
# head and shoulders (no background) in the box x 41..85, y 50..101 of the 128x128 texture: the hair top sits at y 52..53 (hair and
# a spilling lock may reach x 40 / y 48), the right edge is 83..84, and below the middle of the figure the picture is clipped by an
# ellipse (centre 66.5, 79.6, half axes 18 x 21.4, bottom row 100) with a 1 px anti-aliased edge. Everything else is alpha 0,
# palette entry 255 (255,255,255,0) like every retail gim.
HUD_BOX = (41, 52, 85, 101)
HUD_ELLIPSE = (66.5, 79.6, 18.0, 21.4)           # lower clip of a cut-out (cx, cy, a, b)
def knockout_white(img, tol=22):
    """A render on a plain white ground (RGB or opaque RGBA, white corners) -> RGBA with the ground transparent: the near-white area
    connected to the picture border is removed (white inside the figure stays), the edge is eroded 1 px and feathered. Pictures that
    already have transparency, or no white corners, are returned unchanged."""
    from PIL import ImageDraw, ImageFilter
    im = img.convert("RGBA")
    w, h = im.size
    if im.getchannel("A").getextrema()[0] < 255:
        return img
    lo = 255 - tol
    corners = [im.getpixel(c)[:3] for c in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))]
    if not all(min(c) >= lo for c in corners):
        return img
    near = Image.merge("RGB", [c.point(lambda v: 255 if v >= lo else 0) for c in im.convert("RGB").split()]).convert("L").point(lambda v: 255 if v == 255 else 0)
    for xy in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        if near.getpixel(xy) == 255:
            ImageDraw.floodfill(near, xy, 128)
    keep = near.point(lambda v: 0 if v == 128 else 255)       # 255 = figure
    try:                       # enclosed gaps of pure white (between a staff and a body ...) are ground too, when large enough
        import numpy as np
        from scipy import ndimage
        rgb = np.asarray(im.convert("RGB"))
        pure = (rgb.min(axis=2) >= 252) & (np.asarray(keep) == 255)
        lab, n = ndimage.label(pure)
        if n:
            sizes = ndimage.sum(pure, lab, range(1, n + 1))
            gap = np.isin(lab, [i + 1 for i, a in enumerate(sizes) if a >= 600])
            keep = Image.fromarray(np.where(gap, 0, np.asarray(keep)).astype("uint8"), "L")
    except ImportError:
        pass
    keep = keep.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.8))
    out = im.copy()
    out.putalpha(keep)
    return out


HUD_OVAL = (65.0, 76.5, 19.5, 24.5)              # clip of a picture with its own background (a cameo in the star)


def hud_mask(size, full_oval=False, ss=4, scale=1):
    """Anti-aliased coverage mask (L) of the HUD clip: the lower ellipse below its centre only (a cut-out keeps its own outline
    above), or the whole oval for a picture that has a background. `scale`: the same clip for a texture `scale` times the size."""
    w, h = size
    cx, cy, a, b = (v * scale for v in (HUD_OVAL if full_oval else HUD_ELLIPSE))
    if scale > 1:
        ss = max(1, ss // 2)
    m = Image.new("L", size, 0)
    px = m.load()
    for y in range(h):
        for x in range(w):
            n = 0
            for sy in range(ss):
                for sx in range(ss):
                    fx, fy = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    if (fy <= cy and not full_oval) or ((fx - cx) / a) ** 2 + ((fy - cy) / b) ** 2 <= 1.0:
                        n += 1
            px[x, y] = round(255 * n / (ss * ss))
    return m


def hud_default_crop(src, head_picture=True):
    """Source crop (x0,y0,x1,y1) for the HUD box. A head-and-shoulders picture is covered top aligned; a full body figure is cropped
    like the tile does it (hair top to ~27% of the figure height, centred on the head)."""
    src = src.convert("RGBA")
    bx0, by0, bx1, by1 = HUD_BOX
    aspect = (bx1 - bx0) / (by1 - by0)
    if head_picture:
        return cover_crop(src, aspect, 0.0)
    bb = src.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox() or (0, 0) + src.size
    x0, y0, x1, y1 = bb
    hgt = y1 - y0
    ch = int(hgt * 0.27)
    cw = int(ch * aspect)
    cx = _head_centre(src, y0, y0 + int(hgt * 0.045), y0 + int(hgt * 0.14))
    top = y0 - int(hgt * 0.004)
    return (cx - cw // 2, top, cx - cw // 2 + cw, top + ch)


def frame_hud(src, size=(128, 128), crop=None, head_picture=True, scale=1):
    """HUD portrait texture: `crop` (source pixels) is scaled into HUD_BOX, the picture's own alpha is kept and the lower part is
    clipped by the retail ellipse (a picture with no transparency in the crop gets the full oval instead). `scale` > 1 draws the
    same framing into a texture that many times larger (a texture-pack replacement, hd_art.py); size stays the game's size."""
    from PIL import ImageEnhance, ImageFilter
    src = src.convert("RGBA")
    if crop is None:
        crop = hud_default_crop(src, head_picture)
    bx0, by0, bx1, by1 = (v * scale for v in HUD_BOX)
    size = (size[0] * scale, size[1] * scale)
    cw, ch = crop[2] - crop[0], crop[3] - crop[1]
    canvas = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    canvas.paste(src, (-crop[0], -crop[1]))
    full_oval = canvas.getchannel("A").point(lambda a: 255 if a > 240 else 0).resize((16, 16), Image.BOX).getextrema()[0] > 250
    pic = _resize(canvas, (bx1 - bx0, by1 - by0))
    fa = pic.getchannel("A")
    rgb = pic.convert("RGB").filter(ImageFilter.UnsharpMask(radius=0.8 * scale, percent=90, threshold=2))
    rgb = ImageEnhance.Contrast(rgb).enhance(1.08)
    pic = rgb.convert("RGBA")
    pic.putalpha(fa)
    out = Image.new("RGBA", size, (255, 255, 255, 0))
    out.paste(pic, (bx0, by0))
    mask = hud_mask(size, full_oval, scale=scale)
    import numpy as np
    al = Image.fromarray(np.minimum(np.array(out.getchannel("A")), np.array(mask)))       # alpha = picture alpha x clip
    out.putalpha(al)
    return out


def frame_face_in(src, size, crop=None, shift=(0, 0), template=None, scale=1):
    """Wide face strip for the VS / battle-prep "face in" texture (p_<code>_face_in_name.gim, 128x32, a 4:1 crop of the
    eyes and nose of the character). Opaque rectangle, dark backdrop where the picture is empty; the template's bottom
    two rows (corner markers) are kept."""
    src = src.convert("RGBA")
    w, h = size
    if crop is None:
        bbox = src.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox() or (0, 0) + src.size
        x0, y0, x1, y1 = bbox
        hgt = y1 - y0
        ch = int(hgt * 0.075)                               # eyes to just under the nose
        cw = int(ch * w / h)
        cx = _head_centre(src, y0, y0 + int(hgt * 0.045), y0 + int(hgt * 0.14)) + int(cw * 0.16)
        top = y0 + int(hgt * 0.052)
        crop = (cx - cw // 2, top, cx - cw // 2 + cw, top + ch)
    cw, ch = crop[2] - crop[0], crop[3] - crop[1]
    canvas = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    canvas.paste(src, (-crop[0] + shift[0], -crop[1] + shift[1]))
    face = _resize(canvas, (w * scale, h * scale))
    out = Image.new("RGBA", (w * scale, h * scale), (24, 24, 34, 255))
    out.alpha_composite(face)
    if template is not None:
        mark = template.crop((0, h - 2, w, h))
        if scale > 1:
            mark = mark.resize((w * scale, 2 * scale), Image.NEAREST)
        out.paste(mark, (0, (h - 2) * scale))
    return out


def render_name(text, size, font=None, pt=None):
    """Name plate like the originals (lg0 entries 79..110, 128x32): white serif text, soft dark glow."""
    from PIL import ImageDraw, ImageFilter, ImageFont
    w, h = size
    cands = [font] if font else []
    cands += ["/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"]
    pt = pt or int(h * 0.56)
    f = None
    for c in cands:
        try:
            f = ImageFont.truetype(c, pt)
            break
        except Exception:
            continue
    f = f or ImageFont.load_default()
    # shrink until it fits the plate
    while True:
        bb = f.getbbox(text)
        if bb[2] - bb[0] <= w - 8 or pt <= 8:
            break
        pt -= 1
        f = f.font_variant(size=pt)
    x = (w - (bb[2] - bb[0])) // 2 - bb[0]
    y = (h - (bb[3] - bb[1])) // 2 - bb[1]
    glow = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).text((x, y), text, font=f, fill=(28, 30, 96, 255), stroke_width=1, stroke_fill=(28, 30, 96, 255))
    glow = glow.filter(ImageFilter.GaussianBlur(1.2))
    out = glow
    top = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(top).text((x, y), text, font=f, fill=(232, 238, 255, 255))
    out.alpha_composite(top)
    return out


def render_logo(numeral, size=(128, 64), template=None, game="FINAL FANTASY", image=None):
    """Game logo plate like the originals (menu_pk_free_battle_gim_2 entries 77..91, 128x64): the picture
    sits in the top left 89x62 block on an opaque white ground, the rest is transparent, and the
    bottom rows keep the template's small solid marker block. "FINAL FANTASY" in a dark condensed
    serif with the numeral set larger below it and a thin rule, in the manner of the FFIV/FFV plates."""
    from PIL import ImageDraw, ImageFont
    w, h = size
    out = Image.new("RGBA", size, (255, 255, 255, 0))
    bw, bh = 89, 62
    ImageDraw.Draw(out).rectangle((0, 1, bw - 1, bh), fill=(255, 255, 255, 255))
    if image is not None:        # supplied artwork: fitted (aspect kept, 3 px margin) and centred in the white block
        art = image.convert("RGBA")
        sc = min((bw - 6) / art.width, (bh - 6) / art.height)
        art = art.resize((max(1, round(art.width * sc)), max(1, round(art.height * sc))), Image.LANCZOS)
        out.alpha_composite(art, ((bw - art.width) // 2, 1 + (bh - art.height) // 2))
        if template is not None:
            out.paste(template.crop((0, h - 8, w, h)), (0, h - 8))
        return out
    navy, rule = (26, 28, 66, 255), (150, 152, 170, 255)
    cands = ["/usr/share/fonts/truetype/dejavu/DejaVuSerifCondensed-Bold.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"]

    def font(pt):
        for c in cands:
            try:
                return ImageFont.truetype(c, pt)
            except Exception:
                continue
        return ImageFont.load_default()

    d = ImageDraw.Draw(out)
    pt = 13
    f = font(pt)
    while f.getbbox(game)[2] - f.getbbox(game)[0] > bw - 8 and pt > 6:
        pt -= 1
        f = font(pt)
    if not numeral:                  # series without a numeral: the title alone, wrapped over two lines when long, centred
        words, lines = game.split(), [game]
        if f.getbbox(game)[2] - f.getbbox(game)[0] > bw - 8 and len(words) > 1:
            half = max(range(1, len(words)), key=lambda i: -abs(len(" ".join(words[:i])) - len(" ".join(words[i:]))))
            lines = [" ".join(words[:half]), " ".join(words[half:])]
        pt = 16
        while pt > 6 and max(font(pt).getbbox(t)[2] - font(pt).getbbox(t)[0] for t in lines) > bw - 8:
            pt -= 1
        f = font(pt)
        lh = f.getbbox("Ag")[3] - f.getbbox("Ag")[1] + 4
        y = 1 + (bh - lh * len(lines)) // 2
        for t in lines:
            b = f.getbbox(t)
            d.text(((bw - (b[2] - b[0])) // 2 - b[0], y - f.getbbox("Ag")[1]), t, font=f, fill=navy)
            y += lh
        if template is not None:
            out.paste(template.crop((0, h - 8, w, h)), (0, h - 8))
        return out
    bb = f.getbbox(game)
    d.text(((bw - (bb[2] - bb[0])) // 2 - bb[0], 11 - bb[1]), game, font=f, fill=navy)
    d.line((4, 11 + (bb[3] - bb[1]) + 3, bw - 5, 11 + (bb[3] - bb[1]) + 3), fill=rule)
    nf = font(26)
    nb = nf.getbbox(numeral)
    ny = 11 + (bb[3] - bb[1]) + 7
    d.text(((bw - (nb[2] - nb[0])) // 2 - nb[0], ny - nb[1]), numeral, font=nf, fill=navy)
    if template is not None:
        out.paste(template.crop((0, h - 8, w, h)), (0, h - 8))
    return out


def mirror(d):
    """The same GIM with the picture mirrored left-right inside its own bounding box (the columns that hold any
    non-transparent texel), so a right-aligned slide-in stays right-aligned. Index plane only: the palette and
    every texel value are kept, nothing is re-quantised. Used for the player's side of the battle setup panel,
    which draws the same portrait texture on both sides and never mirrors it (roster.txt artl= / artr=)."""
    info = parse(d)
    io, w, h, fmt, _ = info["img"]
    po, pw, _, pfmt, _ = info["pal"]
    assert pfmt == 3 and fmt == 5, "only I8 + RGBA8888 supported (fmt %d pal %d)" % (fmt, pfmt)
    solid = [d[po + 4 * i + 3] > 0 for i in range(pw)]
    plane = d[io:io + w * h]
    cols = [x for x in range(w) if any(solid[plane[y * w + x]] for y in range(h))]
    out = bytearray(d)
    if not cols:
        return bytes(out)
    x0, x1 = cols[0], cols[-1] + 1
    for y in range(h):
        r = io + y * w
        out[r + x0:r + x1] = bytes(reversed(d[r + x0:r + x1]))
    return bytes(out)


def prepare(im, size, mode, crop=None, shift=(0, 0), template=None, scale=1):
    """`scale` > 1: the same picture `scale` times larger than `size` (texture-pack replacements, hd_art.py)."""
    if mode == "art":
        return frame_art(im, (size[0] * scale, size[1] * scale), crop, shift)
    if mode == "tile":
        return frame_tile(im, size, crop, shift, template, scale)
    if mode == "tile_cutout":
        return frame_tile(im, size, crop, shift, template, scale, cutout=True)
    if mode == "face_in":
        return frame_face_in(im, size, crop, shift, template, scale)
    if mode in ("hud", "hud_figure"):
        return frame_hud(im, size, crop, head_picture=(mode == "hud"))
    if mode == "stretch":
        return _resize(im.convert("RGBA"), size)
    # fit
    im = im.convert("RGBA")
    s = min(size[0] / im.width, size[1] / im.height)
    r = _resize(im, (max(1, round(im.width * s)), max(1, round(im.height * s))))
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.paste(r, ((size[0] - r.width) // 2, (size[1] - r.height) // 2))
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    opts = {}
    pos = []
    i = 0
    while i < len(argv):
        if argv[i].startswith("--"):
            opts[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            pos.append(argv[i])
            i += 1
    if pos[0] == "decode":
        decode(open(pos[1], "rb").read()).save(pos[2])
        return 0
    if pos[0] == "mirror":    # mirror <in.gim> <out.gim>
        open(pos[2], "wb").write(mirror(open(pos[1], "rb").read()))
        print("wrote", pos[2])
        return 0
    if pos[0] == "name":      # name <template.gim> "Noctis" <out.gim>
        tpl = open(pos[1], "rb").read()
        size = parse(tpl)["img"][1:3]
        open(pos[3], "wb").write(encode(tpl, render_name(pos[2], size)))
        print("wrote", pos[3], "%dx%d" % size)
        return 0
    if pos[0] == "encode":
        tpl = open(pos[1], "rb").read()
        info = parse(tpl)
        size = info["img"][1:3]
        crop = tuple(int(v) for v in opts["crop"].split(",")) if "crop" in opts else None
        shift = tuple(int(v) for v in opts["shift"].split(",")) if "shift" in opts else (0, 0)
        im = prepare(Image.open(pos[2]), size, opts.get("mode", "fit"), crop, shift, decode(tpl))
        open(pos[3], "wb").write(encode(tpl, im))
        print("wrote", pos[3], "%dx%d" % size)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
