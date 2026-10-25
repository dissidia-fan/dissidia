// Host test for runtime/gim_decode.{h,cpp}: synthetic GIMs (every pixel format,
// swizzled and linear) plus, when the disc is present, the menu textures the
// savedata theme uses, compared against hashes of the Python decoder's output
// (tools/gim_encode.py semantics).
//   tests/run_gim_decode_test.sh
#include "../runtime/gim_decode.h"
#include "../runtime/package_mods.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dissidia;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void p16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 255); v.push_back((x >> 8) & 255); }
static void p32(std::vector<uint8_t>& v, uint32_t x) { p16(v, x & 0xFFFF); p16(v, x >> 16); }

// One block: 16 byte header + payload (data header 0x30 bytes with the plane offset 0x40 at +0x1C).
static std::vector<uint8_t> block(int id, int fmt, int order, int w, int h, const std::vector<uint8_t>& pixels) {
    std::vector<uint8_t> dh(0x40, 0);
    dh[0] = 0x30;
    dh[4] = fmt & 255; dh[6] = order & 255; dh[8] = w & 255; dh[9] = w >> 8; dh[10] = h & 255; dh[11] = h >> 8;
    dh[0x1C] = 0x40;
    std::vector<uint8_t> b;
    p16(b, id); p16(b, 0); p32(b, uint32_t(16 + dh.size() + pixels.size())); p32(b, 16); p32(b, 16);
    b.insert(b.end(), dh.begin(), dh.end());
    b.insert(b.end(), pixels.begin(), pixels.end());
    return b;
}
static std::vector<uint8_t> gim(const std::vector<std::vector<uint8_t>>& blocks) {
    std::vector<uint8_t> inner;
    for (auto& b : blocks) inner.insert(inner.end(), b.begin(), b.end());
    std::vector<uint8_t> pic;
    p16(pic, 3); p16(pic, 0); p32(pic, uint32_t(16 + inner.size())); p32(pic, 16); p32(pic, 16);
    pic.insert(pic.end(), inner.begin(), inner.end());
    std::vector<uint8_t> file;
    p16(file, 2); p16(file, 0); p32(file, uint32_t(16 + pic.size())); p32(file, 16); p32(file, 16);
    file.insert(file.end(), pic.begin(), pic.end());
    std::vector<uint8_t> out = {'M', 'I', 'G', '.', '0', '0', '.', '1', 'P', 'S', 'P', 0, 0, 0, 0, 0};
    out.insert(out.end(), file.begin(), file.end());
    return out;
}
// Swizzle linear rows (pitch bytes) into PSP 16x8 blocks.
static std::vector<uint8_t> swizzle(const std::vector<uint8_t>& lin, size_t pitch, int h) {
    std::vector<uint8_t> out;
    for (int by = 0; by < h; by += 8)
        for (size_t bx = 0; bx < pitch / 16; ++bx)
            for (int r = 0; r < 8; ++r) {
                int y = by + r;
                for (int i = 0; i < 16; ++i) out.push_back(y < h ? lin[size_t(y) * pitch + bx * 16 + size_t(i)] : 0);
            }
    return out;
}
static uint64_t fnv(const uint32_t* px, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n * 4; ++i) { h ^= ((const uint8_t*)px)[i]; h *= 0x100000001b3ULL; }
    return h;
}
int main() {
    GimImage im;
    std::string err;
    // RGBA8888 linear 2x1.
    {
        auto g = gim({block(4, 3, 0, 2, 1, {1, 2, 3, 4, 5, 6, 7, 8})});
        CHECK(gim_image_count(g.data(), g.size()) == 1);
        CHECK(gim_decode(g.data(), g.size(), 0, &im, &err));
        CHECK(im.w == 2 && im.h == 1 && im.rgba[0] == 0x04030201u && im.rgba[1] == 0x08070605u);
        CHECK(!gim_decode(g.data(), g.size(), 1, &im, &err));
    }
    // 16 bit formats.
    {
        auto g565 = gim({block(4, 0, 0, 1, 1, {0x1F, 0x00})});          // pure red in 565
        CHECK(gim_decode(g565.data(), g565.size(), 0, &im) && im.rgba[0] == 0xFF0000FFu);
        auto g5551 = gim({block(4, 1, 0, 2, 1, {0x00, 0x80, 0x00, 0x00})});   // alpha set / clear
        CHECK(gim_decode(g5551.data(), g5551.size(), 0, &im) && (im.rgba[0] >> 24) == 255 && (im.rgba[1] >> 24) == 0);
        auto g4444 = gim({block(4, 2, 0, 1, 1, {0x1F, 0x8F})});         // r=F g=1 b=F a=8
        CHECK(gim_decode(g4444.data(), g4444.size(), 0, &im) && im.rgba[0] == (0x88u << 24 | 0xFFu << 16 | 0x11u << 8 | 0xFFu));
    }
    // INDEX4 swizzled 32x16 with a 16 entry RGBA8888 palette: pixel (x,y) = (x + y) & 15.
    {
        const int W = 32, H = 16;
        std::vector<uint8_t> lin(size_t(W / 2) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                uint8_t v = uint8_t((x + y) & 15);
                lin[size_t(y) * (W / 2) + size_t(x / 2)] |= (x & 1) ? v << 4 : v;
            }
        std::vector<uint8_t> pal;
        for (int i = 0; i < 16; ++i) { pal.push_back(uint8_t(i * 16)); pal.push_back(uint8_t(255 - i)); pal.push_back(7); pal.push_back(255); }
        auto g = gim({block(4, 4, 1, W, H, swizzle(lin, W / 2, H)), block(5, 3, 0, 16, 1, pal)});
        CHECK(gim_decode(g.data(), g.size(), 0, &im, &err));
        bool ok = im.w == W && im.h == H;
        for (int y = 0; ok && y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int i = (x + y) & 15;
                ok = ok && im.rgba[size_t(y) * W + size_t(x)] == (uint32_t(i * 16) | uint32_t(255 - i) << 8 | 7u << 16 | 255u << 24);
            }
        CHECK(ok);
    }
    // INDEX8 linear with an RGBA5551 palette; an index beyond the palette is transparent.
    {
        auto g = gim({block(4, 5, 0, 3, 1, {0, 1, 9}), block(5, 1, 0, 2, 1, {0x1F, 0x80, 0xE0, 0x83})});
        CHECK(gim_decode(g.data(), g.size(), 0, &im, &err));
        CHECK(im.rgba[0] == 0xFF0000FFu && im.rgba[1] == 0xFF00FF00u && im.rgba[2] == 0u);
    }
    // Truncated / foreign data is rejected.
    {
        auto g = gim({block(4, 3, 0, 64, 64, std::vector<uint8_t>(16, 0))});
        CHECK(!gim_decode(g.data(), g.size(), 0, &im, &err));
        std::vector<uint8_t> junk(64, 7);
        CHECK(gim_image_count(junk.data(), junk.size()) == 0);
        CHECK(!gim_decode(junk.data(), junk.size(), 0, &im, &err));
    }
    // menu pak entry access.
    {
        std::vector<uint8_t> pak = {'m', 'e', 'n', 'u', ' ', 'p', 'a', 'k'};
        p16(pak, 1); p16(pak, 2); p32(pak, 0);
        p32(pak, 0x20); p32(pak, 4);       // entry 0: 4 bytes at 0x20
        p32(pak, 0x20); p32(pak, 0);       // entry 1: empty
        pak.resize(0x24, 0xAB);
        const uint8_t* p; size_t len;
        CHECK(menu_pak_entry(pak.data(), pak.size(), 0, &p, &len) && len == 4 && p == pak.data() + 0x20);
        CHECK(menu_pak_entry(pak.data(), pak.size(), 1, &p, &len) && len == 0);
        CHECK(!menu_pak_entry(pak.data(), pak.size(), 2, &p, &len));
    }

    // Real assets (skipped without the disc).
    {
        const char* d0 = std::getenv("PSPRECOMP_DISC0");
        std::string data = std::string(d0 ? d0 : "/home/claude/recomp/psprecomp_work/disc0") + "/PSP_GAME/USRDIR/DATA";
        struct Case { const char* pack; int entry; int w, h; uint64_t hash; } cases[] = {
            {"menu/en/menu_lpk_label_gim_0.bin", 0, 128, 128, 0x2147e6885dab0c04ULL},
            {"menu/en/menu_lpk_label_gim_0.bin", 8, 128, 128, 0x1d66dfa8877d95e2ULL},
            {"menu/menu_pk_label_gim_0.bin", 1, 16, 16, 0x57da7db5f5997b25ULL},
            {"menu/menu_pk_label_gim_0.bin", 2, 256, 256, 0xf2b0c1682918c038ULL},
            {"menu/menu_pk_label_gim_0.bin", 11, 256, 256, 0x74824a454e463661ULL},
        };
        int ran = 0;
        for (auto& c : cases) {
            std::vector<uint8_t> pak;
            if (!read_asset(data, "", c.pack, &pak)) continue;
            const uint8_t* p; size_t len;
            CHECK(menu_pak_entry(pak.data(), pak.size(), c.entry, &p, &len) && len > 0);
            CHECK(gim_decode(p, len, 0, &im, &err));
            CHECK(im.w == c.w && im.h == c.h);
            CHECK(fnv(im.rgba.data(), im.rgba.size()) == c.hash);
            ran++;
        }
        std::printf("real assets checked: %d of %zu\n", ran, sizeof cases / sizeof cases[0]);
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "test_gim_decode: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
