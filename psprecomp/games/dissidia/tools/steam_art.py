#!/usr/bin/env python3
"""steam_art.py - draw the Steam library artwork for the native build ("Dissidia Trēdecim").

    steam_art.py --pic1 <disc0>/PSP_GAME/PIC1.PNG [--out DIR]

Writes into DIR (default: games/dissidia/steam, next to this script's folder):

    capsule.png   600x900   library grid, portrait (the Deck's library shelves)
    wide.png      920x430   library grid, landscape ("recent games")
    hero.png     1920x620   the banner at the top of the game's page (no title: Steam draws logo.png over it)
    logo.png     1280x400   transparent title drawn over the hero
    icon.png      256x256   list / overlay icon

The pictures are drawn once (on the Mac) and the results are committed; the Deck only copies them
(steam_shortcut.py), so it needs no Pillow.

Art: the game's own XMB background (PIC1.PNG, the crystal sea) with its "Duodecim 012" logo painted out, enlarged
and softened, plus the DISSIDIA / TRĒDECIM title. Title font: Cinzel (SIL OFL, fonts/ next to the output folder).
Requires Pillow and numpy.
"""
import argparse
import os

if __name__ == "__main__":
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pydeps                    # numpy / Pillow / scipy at the pinned versions (tools/pydeps.py)
    pydeps.require()

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
STEAM = os.path.normpath(os.path.join(HERE, "..", "steam"))
FONTS = os.path.join(STEAM, "fonts")
TITLE, SUBTITLE = "DISSIDIA", "TRĒDECIM"

# ------------------------------------------------------------------ background


LOGO_BOX = (0.515, 0.0, 0.998, 0.355)     # PIC1's "DISSIDIA duodecim 012 FINAL FANTASY" logo, fractions of the picture


def _pyramid_fill(a, keep):
    """Fill where keep == 0 from the kept pixels around it: masked 2x2 means down to a few pixels, then each level's
    holes are taken from the (upsampled) coarser level. Smooth, no seams, any hole size."""
    h, w = keep.shape
    if min(h, w) <= 2 or keep.min() > 0.999:
        return a
    hh, ww = (h + 1) // 2, (w + 1) // 2
    pa = np.zeros((hh * 2, ww * 2, a.shape[2]), np.float32)
    pk = np.zeros((hh * 2, ww * 2), np.float32)
    pa[:h, :w] = a * keep[..., None]
    pk[:h, :w] = keep
    sa = pa.reshape(hh, 2, ww, 2, -1).sum((1, 3))
    sk = pk.reshape(hh, 2, ww, 2).sum((1, 3))
    coarse = _pyramid_fill(sa / np.maximum(sk, 1e-6)[..., None], np.clip(sk, 0, 1))
    up = np.asarray(Image.fromarray(np.uint8(np.clip(coarse, 0, 1) * 255)).resize((w, h), Image.BILINEAR)).astype(np.float32) / 255
    return a * keep[..., None] + up * (1 - keep[..., None])


def clean_pic1(path):
    """PIC1 without its logo: the logo's box (top right) is filled from the sky around it."""
    im = Image.open(path).convert("RGB")
    a = np.asarray(im).astype(np.float32) / 255
    h, w, _ = a.shape
    x0, y0, x1, y1 = (int(round(v)) for v in (LOGO_BOX[0] * w, LOGO_BOX[1] * h, LOGO_BOX[2] * w, LOGO_BOX[3] * h))
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rectangle((x0, y0, x1, y1), fill=255)
    m = m.filter(ImageFilter.GaussianBlur(4))
    keep = 1 - np.asarray(m).astype(np.float32) / 255
    keep = np.where(keep > 0.98, 1.0, keep * 0.0)               # hard hole, soft blend below
    filled = _pyramid_fill(a, keep)
    filled = np.asarray(Image.fromarray(np.uint8(filled * 255)).filter(ImageFilter.GaussianBlur(3))).astype(np.float32) / 255
    blend = (np.asarray(m).astype(np.float32) / 255)[..., None]
    out = a * (1 - blend) + filled * blend
    return Image.fromarray(np.uint8(np.clip(out, 0, 1) * 255))


def backdrop(pic1, size, focus=(0.42, 0.5), zoom=1.0, blur=2.2):
    """Cover `size` with the cleaned PIC1 (cropped around `focus`, fractions of the source), softened and graded
    darker and colder so the characters and title stand out."""
    W, H = size
    s = max(W / pic1.width, H / pic1.height) * zoom
    big = pic1.resize((round(pic1.width * s), round(pic1.height * s)), Image.LANCZOS)
    x = int(np.clip(focus[0] * big.width - W / 2, 0, big.width - W))
    y = int(np.clip(focus[1] * big.height - H / 2, 0, big.height - H))
    bg = big.crop((x, y, x + W, y + H)).filter(ImageFilter.GaussianBlur(blur * s / 4))
    a = np.asarray(bg).astype(np.float32) / 255
    a = a ** 1.25 * np.array([0.82, 0.90, 1.0])                 # darker, colder
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    d = np.sqrt(((xx - W / 2) / (W * 0.62)) ** 2 + ((yy - H * 0.45) / (H * 0.68)) ** 2)
    a *= np.clip(1.15 - 0.55 * d ** 2, 0.25, 1)[..., None]     # vignette
    return Image.fromarray(np.uint8(np.clip(a, 0, 1) * 255)).convert("RGBA")


def fade_bottom(img, start, strength=0.9, color=(6, 8, 14)):
    """Darken towards the bottom from `start` (fraction of the height), for the title."""
    W, H = img.size
    t = np.clip((np.arange(H, dtype=np.float32) / H - start) / max(1e-3, 1 - start), 0, 1) ** 1.4 * strength
    al = Image.fromarray(np.uint8(np.repeat(t[:, None], W, 1) * 255))
    ov = Image.new("RGBA", img.size, color + (255,))
    ov.putalpha(al)
    return Image.alpha_composite(img, ov)


def fade_top(img, end, strength=0.85, color=(6, 8, 14)):
    W, H = img.size
    t = np.clip(1 - (np.arange(H, dtype=np.float32) / H) / end, 0, 1) ** 1.4 * strength
    al = Image.fromarray(np.uint8(np.repeat(t[:, None], W, 1) * 255))
    ov = Image.new("RGBA", img.size, color + (255,))
    ov.putalpha(al)
    return Image.alpha_composite(img, ov)

def _paste_clip(canvas, img, x, y):
    W, H = canvas.size
    x0, y0 = max(0, -x), max(0, -y)
    x1, y1 = min(img.width, W - x), min(img.height, H - y)
    if x1 > x0 and y1 > y0:
        canvas.alpha_composite(img.crop((x0, y0, x1, y1)), (x + x0, y + y0))

# ------------------------------------------------------------------ title


def _font(name, px):
    return ImageFont.truetype(os.path.join(FONTS, name), px)


def _spaced(draw, xy, text, font, tracking, fill):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking


def _spaced_w(text, font, tracking):
    d = ImageDraw.Draw(Image.new("L", (1, 1)))
    return sum(d.textlength(c, font=font) for c in text) + tracking * (len(text) - 1)


def title_block(width, ss=3):
    """The title as an RGBA image `width` px wide (height follows): DISSIDIA in a gold bevel over TRĒDECIM in silver,
    letterspaced between two fine rules. Drawn at `ss`x and reduced for clean edges."""
    W = width * ss
    big = _font("CinzelDecorative-Bold.ttf", 10)
    tr_big = 0.06
    # size DISSIDIA to 92 % of the width
    px = 10
    while True:
        f = _font("CinzelDecorative-Bold.ttf", px + 4)
        if _spaced_w(TITLE, f, (px + 4) * tr_big) > W * 0.92:
            break
        px += 4
    big = _font("CinzelDecorative-Bold.ttf", px)
    sub_px = int(px * 0.34)
    sub = _font("Cinzel-Bold.ttf", sub_px)
    tr_sub = sub_px * 0.55
    asc_b = big.getbbox(TITLE)
    asc_s = sub.getbbox("TREDECIM")
    mac = sub.getbbox("Ē")
    top_pad = int(px * 0.12)
    h_big = asc_b[3] - asc_b[1]
    gap = int(px * 0.20)
    h_sub = asc_s[3] - mac[1]
    H = top_pad + h_big + gap + h_sub + int(px * 0.16)
    # masks
    m_big = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(m_big)
    wb = _spaced_w(TITLE, big, px * tr_big)
    _spaced(d, ((W - wb) / 2, top_pad - asc_b[1]), TITLE, big, px * tr_big, 255)
    m_sub = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(m_sub)
    ws = _spaced_w(SUBTITLE, sub, tr_sub)
    sy = top_pad + h_big + gap - mac[1]
    _spaced(d, ((W - ws) / 2, sy), SUBTITLE, sub, tr_sub, 255)
    # rules either side of the subtitle, at the cap-height middle
    ry = sy + (asc_s[1] + asc_s[3]) / 2
    lw = max(2, int(sub_px * 0.07))
    gapx = sub_px * 0.9
    for x0, x1 in (((W - ws) / 2 - gapx - W * 0.16, (W - ws) / 2 - gapx), ((W + ws) / 2 + gapx, (W + ws) / 2 + gapx + W * 0.16)):
        d.rectangle((x0, ry - lw / 2, x1, ry + lw / 2), fill=255)
        cx = x1 if x1 < W / 2 else x0                            # small diamond at the inner end
        r = lw * 2.4
        d.polygon(((cx, ry - r), (cx + r, ry), (cx, ry + r), (cx - r, ry)), fill=255)
    # fills: gold gradient with a hard horizon (metal bevel look), silver for the subtitle
    yy = np.linspace(0, 1, H, dtype=np.float32)[:, None]
    t = np.clip((yy * H - top_pad) / max(1, h_big), 0, 1)
    top = np.array([255, 246, 214], np.float32)
    mid = np.array([226, 176, 84], np.float32)
    low = np.array([150, 96, 34], np.float32)
    up = np.array([246, 214, 140], np.float32)
    g = np.where(t < 0.52, top + (mid - top) * (t / 0.52), low + (up - low) * ((t - 0.52) / 0.48))
    gold = Image.fromarray(np.uint8(np.repeat(g[:, None, :], W, 1).reshape(H, W, 3)))
    sv = np.array([236, 240, 248], np.float32) + (np.array([168, 182, 205], np.float32) - [236, 240, 248]) * yy
    silver = Image.fromarray(np.uint8(np.repeat(sv[:, None, :], W, 1).reshape(H, W, 3)))
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    # dark outline + drop shadow + faint warm glow under everything
    both = ImageChops.lighter(m_big, m_sub)
    shadow = both.filter(ImageFilter.MaxFilter(ss * 2 + 1)).filter(ImageFilter.GaussianBlur(ss * 5))
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    sh.putalpha(shadow.point(lambda v: int(v * 0.85)))
    out.alpha_composite(sh, (0, ss * 3))
    glow = m_big.filter(ImageFilter.GaussianBlur(ss * 14))
    gl = Image.new("RGBA", (W, H), (255, 190, 90, 255))
    gl.putalpha(glow.point(lambda v: int(v * 0.35)))
    out.alpha_composite(gl)
    stroke = both.filter(ImageFilter.MaxFilter(ss * 2 + 1))
    st = Image.new("RGBA", (W, H), (28, 16, 6, 255))
    st.putalpha(stroke)
    out.alpha_composite(st)
    gb = gold.convert("RGBA")
    gb.putalpha(m_big)
    out.alpha_composite(gb)
    # thin bright bevel line along the top edge of the gold letters
    edge = ImageChops.subtract(m_big, ImageChops.offset(m_big, 0, ss * 2)).filter(ImageFilter.GaussianBlur(ss * 0.6))
    hl = Image.new("RGBA", (W, H), (255, 252, 236, 255))
    hl.putalpha(edge.point(lambda v: int(v * 0.6)))
    out.alpha_composite(hl)
    sb = silver.convert("RGBA")
    sb.putalpha(m_sub)
    out.alpha_composite(sb)
    return out.resize((width, round(H / ss)), Image.LANCZOS)


def put_title(canvas, width, center_x, top=None, bottom=None):
    t = title_block(width)
    y = top if top is not None else bottom - t.height
    _paste_clip(canvas, t, int(center_x - t.width / 2), int(y))
    return t.height

# ------------------------------------------------------------------ the five pictures


def make_all(a):
    pic1 = clean_pic1(a.pic1)
    os.makedirs(a.out, exist_ok=True)

    # portrait capsule 600x900: the light shafts and crystals, title in the sky
    W, H = 600, 900
    c = backdrop(pic1, (W, H), focus=(0.24, 0.5), zoom=1.0, blur=1.6)
    c = fade_top(c, 0.45, 0.55)
    put_title(c, 540, W / 2, top=150)
    c.convert("RGB").save(os.path.join(a.out, "capsule.png"), optimize=True)

    # landscape capsule 920x430: the whole crystal sea, title in the middle of the sky
    W, H = 920, 430
    c = backdrop(pic1, (W, H), focus=(0.5, 0.5), zoom=1.0, blur=1.6)
    c = fade_top(c, 0.6, 0.45)
    put_title(c, 520, W / 2, top=58)
    c.convert("RGB").save(os.path.join(a.out, "wide.png"), optimize=True)

    # hero 1920x620: no title (Steam draws logo.png bottom left over it)
    W, H = 1920, 620
    c = backdrop(pic1, (W, H), focus=(0.5, 0.56), zoom=1.0, blur=2.0)
    c = fade_left(c, 0.45, 0.45)
    c.convert("RGB").save(os.path.join(a.out, "hero.png"), optimize=True)

    # logo 1280x400 transparent
    t = title_block(1240)
    lg = Image.new("RGBA", (1280, max(400, t.height + 40)), (0, 0, 0, 0))
    lg.alpha_composite(t, ((1280 - t.width) // 2, (lg.height - t.height) // 2))
    lg.save(os.path.join(a.out, "logo.png"), optimize=True)

    # icon 256x256: gold "D" monogram over the crystal sea
    S = 256
    c = backdrop(pic1, (S, S), focus=(0.22, 0.42), zoom=1.6)
    ss = 4
    m = Image.new("L", (S * ss, S * ss), 0)
    f = _font("CinzelDecorative-Bold.ttf", int(S * ss * 0.86))
    d = ImageDraw.Draw(m)
    bb = d.textbbox((0, 0), "D", font=f)
    d.text(((S * ss - (bb[2] - bb[0])) / 2 - bb[0], (S * ss - (bb[3] - bb[1])) / 2 - bb[1]), "D", font=f, fill=255)
    m = m.resize((S, S), Image.LANCZOS)
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 255))
    sh.putalpha(m.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(5)))
    c.alpha_composite(sh, (0, 3))
    yy = np.linspace(0, 1, S, dtype=np.float32)[:, None]
    g = np.where(yy < 0.5, np.array([255, 244, 205]) + (np.array([224, 172, 80]) - [255, 244, 205]) * (yy / 0.5),
                 np.array([150, 96, 34]) + (np.array([246, 212, 136]) - [150, 96, 34]) * ((yy - 0.5) / 0.5))
    gold = Image.fromarray(np.uint8(np.repeat(g[:, None, :], S, 1).reshape(S, S, 3))).convert("RGBA")
    gold.putalpha(m)
    st = Image.new("RGBA", (S, S), (28, 16, 6, 255))
    st.putalpha(m.filter(ImageFilter.MaxFilter(3)))
    c.alpha_composite(st)
    c.alpha_composite(gold)
    c.save(os.path.join(a.out, "icon.png"), optimize=True)
    for n in ("capsule", "wide", "hero", "logo", "icon"):
        p = os.path.join(a.out, n + ".png")
        print("wrote %s %s" % (p, Image.open(p).size))


def fade_left(img, end, strength=0.8, color=(6, 8, 14)):
    W, H = img.size
    t = np.clip(1 - (np.arange(W, dtype=np.float32) / W) / end, 0, 1) ** 1.2 * strength
    al = Image.fromarray(np.uint8(np.repeat(t[None, :], H, 0) * 255))
    ov = Image.new("RGBA", img.size, color + (255,))
    ov.putalpha(al)
    return Image.alpha_composite(img, ov)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pic1", required=True, help="disc0/PSP_GAME/PIC1.PNG")
    ap.add_argument("--out", default=STEAM)
    make_all(ap.parse_args())


if __name__ == "__main__":
    main()
