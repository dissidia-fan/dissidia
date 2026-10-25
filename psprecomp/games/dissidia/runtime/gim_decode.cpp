// games/dissidia/runtime/gim_decode.cpp — see gim_decode.h.
#include "gim_decode.h"

#include <cstring>

namespace dissidia {

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

struct Plane {
    size_t data = 0;          // absolute offset of the pixel data
    int w = 0, h = 0, fmt = 0, order = 0;
    bool ok = false;
};

struct Gim {
    std::vector<Plane> images;
    std::vector<Plane> palettes;
};

void walk(const uint8_t* d, size_t n, size_t off, size_t end, Gim* g, int depth) {
    if (depth > 8) return;
    while (off + 16 <= end && off + 16 <= n) {
        const uint16_t id = rd16(d + off);
        const uint32_t size = rd32(d + off + 4);
        if (size < 16 || off + size > n) return;
        if (id == 2 || id == 3) {
            walk(d, n, off + 16, off + size, g, depth + 1);
        } else if (id == 4 || id == 5) {
            const size_t h = off + 16;
            if (h + 0x20 <= n) {
                Plane p;
                p.fmt = rd16(d + h + 4);
                p.order = rd16(d + h + 6);
                p.w = rd16(d + h + 8);
                p.h = rd16(d + h + 10);
                p.data = h + rd32(d + h + 0x1C);
                p.ok = p.w > 0 && p.h > 0 && p.data < n;
                (id == 4 ? g->images : g->palettes).push_back(p);
            }
        }
        off += size;
    }
}

bool parse(const uint8_t* d, size_t n, Gim* g) {
    if (n < 0x20 || std::memcmp(d, "MIG.00.1PSP", 11) != 0) return false;
    walk(d, n, 0x10, n, g, 0);
    return true;
}

int bits_per_pixel(int fmt) {
    static const int bpp[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    return fmt >= 0 && fmt < 8 ? bpp[fmt] : 0;
}

uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

uint32_t color(int fmt, uint32_t v) {
    switch (fmt) {
        case 0: return pack((v & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, ((v >> 11) & 31) * 255 / 31, 255);
        case 1: return pack((v & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, ((v >> 10) & 31) * 255 / 31, (v & 0x8000) ? 255 : 0);
        case 2: return pack((v & 15) * 17, ((v >> 4) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 12) & 15) * 17);
        default: return v;   // RGBA8888 is already R,G,B,A in memory order
    }
}

}  // namespace

int gim_image_count(const uint8_t* d, size_t n) {
    Gim g;
    return parse(d, n, &g) ? int(g.images.size()) : 0;
}

bool gim_decode(const uint8_t* d, size_t n, int index, GimImage* out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    Gim g;
    if (!parse(d, n, &g)) return fail("not a GIM");
    if (index < 0 || index >= int(g.images.size())) return fail("image index out of range");
    const Plane& im = g.images[size_t(index)];
    if (!im.ok) return fail("bad image block");
    const int bpp = bits_per_pixel(im.fmt);
    if (!bpp) return fail("unsupported pixel format");
    const size_t rowb = (size_t(im.w) * size_t(bpp) + 7) / 8;
    const size_t pitch = im.order ? (rowb + 15) / 16 * 16 : rowb;
    const size_t need = pitch * size_t(im.h);
    if (im.data + need > n) return fail("image data truncated");
    // Linear rows (undo the PSP swizzle when set).
    std::vector<uint8_t> lin(pitch * size_t(im.h));
    if (im.order) {
        const uint8_t* src = d + im.data;
        const size_t bw = pitch / 16;
        size_t i = 0;
        for (int by = 0; by < im.h; by += 8)
            for (size_t bx = 0; bx < bw; ++bx)
                for (int r = 0; r < 8; ++r, i += 16) {
                    const int y = by + r;
                    if (y < im.h) std::memcpy(&lin[size_t(y) * pitch + bx * 16], src + i, 16);
                }
    } else {
        std::memcpy(lin.data(), d + im.data, need);
    }
    std::vector<uint32_t> pal;
    if (im.fmt >= 4) {
        if (g.palettes.empty() || !g.palettes[0].ok) return fail("indexed image without palette");
        const Plane& pp = g.palettes[0];
        const int pbpp = bits_per_pixel(pp.fmt);
        if (pbpp < 16 || pp.fmt >= 4) return fail("unsupported palette format");
        const size_t entries = size_t(pp.w), bytes = size_t(pbpp / 8);
        if (pp.data + entries * bytes > n) return fail("palette truncated");
        pal.resize(entries);
        for (size_t i = 0; i < entries; ++i) {
            uint32_t v = bytes == 2 ? rd16(d + pp.data + i * 2) : rd32(d + pp.data + i * 4);
            pal[i] = color(pp.fmt, v);
        }
    }
    out->w = im.w;
    out->h = im.h;
    out->rgba.assign(size_t(im.w) * size_t(im.h), 0);
    for (int y = 0; y < im.h; ++y) {
        const uint8_t* row = &lin[size_t(y) * pitch];
        uint32_t* dst = &out->rgba[size_t(y) * size_t(im.w)];
        for (int x = 0; x < im.w; ++x) {
            uint32_t v;
            switch (im.fmt) {
                case 4: { uint8_t b = row[x / 2]; v = (x & 1) ? (b >> 4) : (b & 15); break; }
                case 5: v = row[x]; break;
                case 6: v = rd16(row + x * 2); break;
                case 7: v = rd32(row + x * 4); break;
                case 3: v = rd32(row + x * 4); dst[x] = v; continue;
                default: v = rd16(row + x * 2); dst[x] = color(im.fmt, v); continue;
            }
            dst[x] = v < pal.size() ? pal[v] : 0;
        }
    }
    return true;
}

bool menu_pak_entry(const uint8_t* d, size_t n, int i, const uint8_t** p, size_t* len) {
    if (n < 0x10 || (std::memcmp(d, "menu pak", 8) != 0 && std::memcmp(d, "mess pak", 8) != 0)) return false;
    const int count = rd16(d + 0x0A);
    if (i < 0 || i >= count || 0x10 + size_t(i + 1) * 8 > n) return false;
    const uint32_t off = rd32(d + 0x10 + size_t(i) * 8), sz = rd32(d + 0x14 + size_t(i) * 8);
    if (sz == 0) { *p = d; *len = 0; return true; }
    if (size_t(off) + sz > n) return false;
    *p = d + off;
    *len = sz;
    return true;
}

}  // namespace dissidia
