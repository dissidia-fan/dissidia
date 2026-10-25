"""PSP GIM texture codec (decode to RGBA, encode from RGBA).

Layout: 'MIG.00.1PSP\\0' + 4 pad, then blocks {u16 type, u16 0, u32 size,
u32 next, u32 data_off}. 0x02 root > 0x03 picture > 0x04 image, 0x05 palette,
0xFF file info. Image/palette data header (0x30 bytes):
  u16 hdr_size, u16 ref, u16 format, u16 order, u16 width, u16 height,
  u16 bpp, u16 pitch_align, u16 height_align, u16 dims, u16 0, u16 0,
  u32 index_start, u32 pixels_start, u32 pixels_end, u32 plane_mask,
  u16 level_type, u16 level_count, u16 frame_type, u16 frame_count, u32 offsets[]
Formats: 0 RGBA5650, 1 RGBA5551, 2 RGBA4444, 3 RGBA8888, 4 INDEX4, 5 INDEX8.
order 1 = PSP swizzled (16-byte x 8-row tiles).
"""
from __future__ import annotations
import struct
import numpy as np

MAGIC = b'MIG.00.1PSP\x00'


def _unswizzle(data: bytes, width_bytes: int, height: int) -> bytes:
    src = np.frombuffer(data, np.uint8)
    out = np.zeros(width_bytes * height, np.uint8)
    bw, bh = width_bytes // 16, height // 8
    tiles = src[:bw * bh * 128].reshape(bh, bw, 8, 16)
    out = tiles.transpose(0, 2, 1, 3).reshape(height, width_bytes)
    return out.tobytes()


def _swizzle(data: bytes, width_bytes: int, height: int) -> bytes:
    src = np.frombuffer(data, np.uint8).reshape(height // 8, 8, width_bytes // 16, 16)
    return src.transpose(0, 2, 1, 3).reshape(-1).tobytes()


def _decode_direct(px: np.ndarray, fmt: int) -> np.ndarray:
    if fmt == 3:
        return px.reshape(-1, 4).astype(np.uint8)
    v = px.view(np.uint16).astype(np.uint32)
    if fmt == 2:
        ch = [((v >> s) & 15) * 17 for s in (0, 4, 8, 12)]
    elif fmt == 1:
        ch = [((v & 31) * 255) // 31, (((v >> 5) & 31) * 255) // 31, (((v >> 10) & 31) * 255) // 31, ((v >> 15) & 1) * 255]
    else:
        ch = [((v & 31) * 255) // 31, (((v >> 5) & 63) * 255) // 63, (((v >> 11) & 31) * 255) // 31, np.full_like(v, 255)]
    return np.stack(ch, 1).astype(np.uint8)


_BPP = {0: 16, 1: 16, 2: 16, 3: 32, 4: 4, 5: 8}


def _blocks(g: bytes, o: int, end: int):
    while o + 16 <= end:
        t, _, size, nxt, dat = struct.unpack_from('<HHIII', g, o)
        if size == 0:
            break
        yield t, o, size, dat
        o += size


def _find(g: bytes, t_want: int):
    stack = [(16, len(g))]
    found = []
    while stack:
        s, e = stack.pop()
        for t, o, size, dat in _blocks(g, s, e):
            if t == t_want:
                found.append(o)
            if t in (2, 3):
                stack.append((o + dat, o + size))
    return found


def _read_image_block(g: bytes, o: int):
    dat = struct.unpack_from('<I', g, o + 12)[0]
    h = o + dat
    (hs, ref, fmt, order, w, hgt, bpp, palign, halign, dims, _, _,
     istart, pstart, pend, pmask, lt, lc, ft, fc) = struct.unpack_from('<12H4I4H', g, h)
    off0 = struct.unpack_from('<I', g, h + 0x40 - 0x10 + 0x10)[0] if False else struct.unpack_from('<I', g, h + 0x30)[0]
    pitch = ((w * bpp + 7) // 8 + palign - 1) // palign * palign
    rows = (hgt + halign - 1) // halign * halign
    raw = g[h + off0:h + off0 + pitch * rows]
    return dict(format=fmt, order=order, width=w, height=hgt, bpp=bpp, pitch=pitch, rows=rows, raw=raw)


def decode(gim: bytes) -> np.ndarray:
    """Return an (H, W, 4) uint8 RGBA array."""
    if gim[4:8] == b'MIG.' and gim[:4] != b'MIG.':
        gim = gim[4:]                                  # GMO FileImage has a u32 size prefix
    img = _read_image_block(gim, _find(gim, 4)[0])
    raw = img['raw']
    if img['order'] == 1:
        raw = _unswizzle(raw, img['pitch'], img['rows'])
    px = np.frombuffer(raw, np.uint8).reshape(img['rows'], img['pitch'])[:img['height']]
    w, h, fmt = img['width'], img['height'], img['format']
    if fmt in (4, 5):
        pal_blocks = _find(gim, 5)
        pal = _read_image_block(gim, pal_blocks[0])
        praw = np.frombuffer(pal['raw'], np.uint8)
        colours = _decode_direct(praw[:pal['width'] * _BPP[pal['format']] // 8], pal['format'])
        if fmt == 5:
            idx = px[:, :w].astype(np.int32)
        else:
            lo = px & 15
            hi = px >> 4
            idx = np.stack([lo, hi], 2).reshape(h, -1)[:, :w].astype(np.int32)
        return colours[idx.clip(0, len(colours) - 1)]
    bytes_w = w * _BPP[fmt] // 8
    return _decode_direct(px[:, :bytes_w].copy().reshape(-1), fmt).reshape(h, w, 4)


def _image_block(btype: int, fmt: int, w: int, h: int, pixel_bytes: bytes, order=0, palign=16, halign=1):
    bpp = _BPP[fmt]
    hdr = struct.pack('<12H4I4HI', 0x30, 0, fmt, order, w, h, bpp, palign, halign, 2, 0, 0,
                      0x30, 0x40, 0x40 + len(pixel_bytes), 0, 1, 1, 3, 1, 0x40)
    hdr += b'\0' * (0x40 - len(hdr))
    body = hdr + pixel_bytes
    size = 16 + len(body)
    return struct.pack('<HHIII', btype, 0, size, size, 16) + body


def _quantize(rgba):
    """-> (idx (H,W) uint8, palette (n<=256, 4) uint8). Pillow when it is there, else a numpy median cut + k-means
    (Blender's own Python has no Pillow)."""
    opaque = rgba[..., 3].min() == 255
    try:
        from PIL import Image
    except ImportError:
        return _quantize_np(rgba[..., :3] if opaque else rgba, opaque)
    if opaque:
        # median cut refined by k-means is far cleaner than the octree (about +10 dB PSNR)
        im = Image.fromarray(np.ascontiguousarray(rgba[..., :3]), 'RGB').quantize(
            256, method=Image.Quantize.MEDIANCUT, kmeans=4, dither=Image.Dither.NONE)
        pal = np.array(im.getpalette()[:256 * 3], np.uint8).reshape(-1, 3)
        pal = np.hstack([pal, np.full((len(pal), 1), 255, np.uint8)])
    else:
        im = Image.fromarray(rgba, 'RGBA').quantize(256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
        pal = np.array(im.getpalette(rawmode='RGBA')[:256 * 4], np.uint8).reshape(-1, 4)
    return np.array(im, np.uint8), pal


def _nearest(px, cen, chunk=32768):
    c2 = (cen * cen).sum(1)
    out = np.empty(len(px), np.int64)
    for s in range(0, len(px), chunk):
        p = px[s:s + chunk]
        out[s:s + chunk] = np.argmin(c2[None, :] - 2.0 * p @ cen.T, axis=1)
    return out


def _quantize_np(img, opaque, k=256, iters=6):
    h, w, ch = img.shape
    px = img.reshape(-1, ch)
    cols, inv, cnt = np.unique(px, axis=0, return_inverse=True, return_counts=True)
    inv = inv.reshape(-1)
    if len(cols) <= k:                                   # few colours: exact
        pal = cols
        idx = inv
    else:
        cf_all = cols.astype(np.float64)
        cf, wt = cf_all, cnt.astype(np.float64)
        if len(cols) > 65536:                            # cluster on 6-bit colours, assign the real ones at the end
            red, rinv = np.unique(cols >> 2, axis=0, return_inverse=True)
            cf = (red.astype(np.float64) * 4 + 1.5)
            wt = np.bincount(rinv.reshape(-1), wt, len(red))
        boxes = [np.arange(len(cf))]
        while len(boxes) < k:                            # median cut on the distinct colours, weighted by use
            best, bi, ax = -1.0, -1, 0
            for i, b in enumerate(boxes):
                if len(b) < 2:
                    continue
                rng = cf[b].max(0) - cf[b].min(0)
                a = int(rng.argmax())
                score = rng[a] * np.sqrt(wt[b].sum())
                if score > best:
                    best, bi, ax = score, i, a
            if bi < 0:
                break
            b = boxes.pop(bi)
            b = b[np.argsort(cf[b, ax], kind='stable')]
            cw = np.cumsum(wt[b])
            m = int(np.searchsorted(cw, cw[-1] / 2)) + 1
            m = min(max(m, 1), len(b) - 1)
            boxes += [b[:m], b[m:]]
        cen = np.array([(cf[b] * wt[b, None]).sum(0) / wt[b].sum() for b in boxes])
        for _ in range(iters):                           # k-means on the distinct colours
            lab = _nearest(cf, cen)
            sw = np.bincount(lab, wt, len(cen))
            new = np.stack([np.bincount(lab, wt * cf[:, j], len(cen)) for j in range(ch)], 1)
            keep = sw > 0
            cen[keep] = new[keep] / sw[keep, None]
        pal = np.clip(np.rint(cen), 0, 255).astype(np.uint8)
        idx = _nearest(cf_all, pal.astype(np.float64))[inv]
    if opaque:
        pal = np.hstack([pal, np.full((len(pal), 1), 255, np.uint8)])
    return idx.reshape(h, w).astype(np.uint8), pal


def encode(rgba: np.ndarray, indexed=True, swizzle=False) -> bytes:
    """Encode RGBA (H,W,4 uint8) to a GIM. Default: INDEX8 + RGBA8888 palette (what Dissidia uses)."""
    h, w = rgba.shape[:2]
    if indexed:
        idx, pal = _quantize(rgba)
        pal = np.vstack([pal, np.zeros((256 - len(pal), 4), np.uint8)])
        pitch = (w + 15) // 16 * 16
        rows = np.zeros((h, pitch), np.uint8)
        rows[:, :w] = idx
        pix = rows.tobytes()
        if swizzle:
            pix = _swizzle(pix, pitch, (h + 7) // 8 * 8)
        img = _image_block(4, 5, w, h, pix, order=1 if swizzle else 0)
        palb = _image_block(5, 3, 256, 1, pal.tobytes())
        inner = img + palb
    else:
        pitch = w * 4
        pix = rgba.astype(np.uint8).tobytes()
        if swizzle:
            pix = _swizzle(pix, pitch, h)
        inner = _image_block(4, 3, w, h, pix, order=1 if swizzle else 0)
    pic = struct.pack('<HHIII', 3, 0, 16 + len(inner), 16, 16) + inner
    root = struct.pack('<HHIII', 2, 0, 16 + len(pic), 16, 16) + pic
    return MAGIC + b'\0' * 4 + root


def wrap_for_gmo(gim: bytes) -> bytes:
    """GMO FileImage payload = u32 total size + GIM bytes (padded to 4)."""
    body = gim + b'\0' * ((-len(gim)) % 4)
    return struct.pack('<I', len(body)) + body


def encode_indexed(idx: np.ndarray, palette: np.ndarray) -> bytes:
    """Encode an (H,W) uint8 index image with a (256,4) uint8 RGBA palette (no quantising)."""
    h, w = idx.shape
    pitch = (w + 15) // 16 * 16
    rows = np.zeros((h, pitch), np.uint8); rows[:, :w] = idx
    pal = np.zeros((256, 4), np.uint8); pal[:len(palette)] = palette
    inner = _image_block(4, 5, w, h, rows.tobytes()) + _image_block(5, 3, 256, 1, pal.tobytes())
    pic = struct.pack('<HHIII', 3, 0, 16 + len(inner), 16, 16) + inner
    root = struct.pack('<HHIII', 2, 0, 16 + len(pic), 16, 16) + pic
    return MAGIC + b'\0' * 4 + root
