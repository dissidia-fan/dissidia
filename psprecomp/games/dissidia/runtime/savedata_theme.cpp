// games/dissidia/runtime/savedata_theme.cpp — builds the Load/Save dialog's SdTheme
// (runtime/src/hle/psp_savedata_dialog.h) from the game's own menu textures.
//
// Sources (menu pak entries in DATA/PACKAGE.BIN, read through read_asset so a
// loose file under <mods>/ restyles them):
//   menu/en/menu_lpk_label_gim_0.bin      entry 0  128x128 INDEX8  common UI sheet: dialog frame
//                                                  corners, pointing hand, silver + royal-blue chevron
//                                                  bars, the navy tab halves
//                                         entry 8  128x128 INDEX8  button/icon sheet: cross and
//                                                  circle glyphs, d-pad glyphs
//   menu/menu_pk_label_gim_0.bin          entry 1  16x16 INDEX4    scanline texture
//                                         entry 2  256x256 INDEX8  dark crescent ornament
//                                         entry 11 256x256 INDEX8  white wing ornament
// The menu backdrop itself is not a texture: the menu code draws a lavender
// gradient quad with the two ornaments and the scanline texture on top, which is
// reproduced here into one 480x272 image from the same pieces.
//
// The game's menu text is drawn through sceFont (the PSP firmware fonts, not
// part of PACKAGE.BIN), which this runtime serves from a host TrueType file
// (PSPRECOMP_FONT, DejaVu Sans by default); the dialog uses the same file so its
// body text matches the menus. There is no glyph atlas in the package to use.
#include "gim_decode.h"
#include "package_mods.h"
#include "hle/psp_hle_font.h"
#include "hle/psp_hle_io.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

std::string dissidia_mods_dir();   // hooks_io.cpp

namespace dissidia {

namespace {

using psp_sddialog::SdImage;
using psp_sddialog::SdMirror;
using psp_sddialog::SdSprite;
using psp_sddialog::SdTheme;

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | g << 8 | b << 16 | a << 24; }

bool load_gim(const std::string& data_dir, const std::string& mods, const std::string& pack, int entry,
              std::shared_ptr<SdImage>* out) {
    std::vector<uint8_t> bytes;
    if (!read_asset(data_dir, mods, pack, &bytes)) return false;
    const uint8_t* p = nullptr;
    size_t len = 0;
    if (!menu_pak_entry(bytes.data(), bytes.size(), entry, &p, &len) || len == 0) return false;
    GimImage g;
    std::string err;
    if (!gim_decode(p, len, 0, &g, &err)) {
        std::fprintf(stderr, "[SDTHEME] %s entry %d: %s\n", pack.c_str(), entry, err.c_str());
        return false;
    }
    auto im = std::make_shared<SdImage>();
    im->w = g.w;
    im->h = g.h;
    im->px = std::move(g.rgba);
    *out = im;
    return true;
}

SdSprite spr(const std::shared_ptr<const SdImage>& im, int x, int y, int w, int h) {
    SdSprite s;
    s.img = im;
    s.x = x; s.y = y; s.w = w; s.h = h;
    return s;
}

// Premultiplied bilinear sample of an image (u,v in pixels, clamped).
void sample(const SdImage& im, float u, float v, float out[4]) {
    float fx = std::clamp(u - 0.5f, 0.f, float(im.w - 1)), fy = std::clamp(v - 0.5f, 0.f, float(im.h - 1));
    int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, im.w - 1), y1 = std::min(y0 + 1, im.h - 1);
    float tx = fx - float(x0), ty = fy - float(y0);
    out[0] = out[1] = out[2] = out[3] = 0;
    const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            uint32_t q = im.px[size_t(ys[j]) * size_t(im.w) + size_t(xs[i])];
            float w = (i ? tx : 1 - tx) * (j ? ty : 1 - ty), a = float(q >> 24) / 255.f;
            out[0] += w * a * float(q & 255);
            out[1] += w * a * float((q >> 8) & 255);
            out[2] += w * a * float((q >> 16) & 255);
            out[3] += w * a;
        }
}

// Composites `im` (scaled to dw x dh, optionally mirrored) onto the backdrop at (dx,dy) with opacity `alpha`.
// `tint` > 0 pulls the sprite colour toward `tint_rgb` (the dark crescent is drawn washed out).
void blit(SdImage& dst, const SdImage& im, float dx, float dy, float dw, float dh, float alpha, bool mirror = false) {
    int x0 = std::max(0, int(std::floor(dx))), y0 = std::max(0, int(std::floor(dy)));
    int x1 = std::min(dst.w, int(std::ceil(dx + dw))), y1 = std::min(dst.h, int(std::ceil(dy + dh)));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float u = (float(x) + 0.5f - dx) / dw * float(im.w), v = (float(y) + 0.5f - dy) / dh * float(im.h);
            if (mirror) u = float(im.w) - u;
            float q[4];
            sample(im, u, v, q);
            if (q[3] <= 0.001f) continue;
            float a = q[3] * alpha, inv = 1.f / q[3];
            uint32_t& p = dst.px[size_t(y) * size_t(dst.w) + size_t(x)];
            float r = float(p & 255), g = float((p >> 8) & 255), b = float((p >> 16) & 255);
            r += (q[0] * inv - r) * a; g += (q[1] * inv - g) * a; b += (q[2] * inv - b) * a;
            p = rgba(uint32_t(r + 0.5f), uint32_t(g + 0.5f), uint32_t(b + 0.5f));
        }
}

float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

// Drops texels that have no opaque neighbour (the wing sheet carries one stray pixel, which
// would show as a speck once the ornament is scaled up).
void despeckle(SdImage& im) {
    std::vector<uint32_t> src = im.px;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            if ((src[size_t(y) * size_t(im.w) + size_t(x)] >> 24) < 20) continue;
            bool lonely = true;
            for (int dy = -1; dy <= 1 && lonely; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if ((dx || dy) && nx >= 0 && ny >= 0 && nx < im.w && ny < im.h &&
                        (src[size_t(ny) * size_t(im.w) + size_t(nx)] >> 24) >= 20) { lonely = false; break; }
                }
            if (lonely) im.px[size_t(y) * size_t(im.w) + size_t(x)] = 0;
        }
}

// The lavender menu backdrop (see the file comment).
std::shared_ptr<SdImage> build_background(const SdImage* stripes, const SdImage* crescent, const SdImage* wing) {
    auto bg = std::make_shared<SdImage>();
    bg->w = 480;
    bg->h = 272;
    bg->px.resize(size_t(bg->w) * size_t(bg->h));
    // Gradient sampled from the game's Extras / Data Install screens: near white at the top left,
    // deepening to periwinkle toward the bottom right.
    const float light[3] = {236.f, 243.f, 255.f}, deep[3] = {104.f, 114.f, 176.f};
    for (int y = 0; y < bg->h; ++y)
        for (int x = 0; x < bg->w; ++x) {
            float fx = (float(x) + 0.5f) / float(bg->w), fy = (float(y) + 0.5f) / float(bg->h);
            float t = smoothstep(0.50f, 1.12f, 0.58f * fx + 0.52f * fy);
            t = t * 0.92f + 0.08f * fx * fx;
            bg->px[size_t(y) * size_t(bg->w) + size_t(x)] =
                rgba(uint32_t(light[0] + (deep[0] - light[0]) * t), uint32_t(light[1] + (deep[1] - light[1]) * t),
                     uint32_t(light[2] + (deep[2] - light[2]) * t));
        }
    if (crescent) blit(*bg, *crescent, 70, 40, 240, 240, 0.10f);
    if (wing) {
        blit(*bg, *wing, 226, -22, 300, 300, 0.62f);
        blit(*bg, *wing, -130, 50, 270, 270, 0.30f, true);
    }
    if (stripes) {   // scanline texture: every other row of the tile is lit
        for (int y = 0; y < bg->h; ++y) {
            uint32_t tex = stripes->px[size_t(y % stripes->h) * size_t(stripes->w)];
            float lit = float(tex & 255) / 255.f * 0.10f;
            if (lit <= 0.f) continue;
            for (int x = 0; x < bg->w; ++x) {
                uint32_t& p = bg->px[size_t(y) * size_t(bg->w) + size_t(x)];
                float r = float(p & 255), g = float((p >> 8) & 255), b = float((p >> 16) & 255);
                r += (255.f - r) * lit; g += (255.f - g) * lit; b += (255.f - b) * lit;
                p = rgba(uint32_t(r), uint32_t(g), uint32_t(b));
            }
        }
    }
    return bg;
}

std::shared_ptr<std::vector<uint8_t>> read_font(const std::string& path) {
    if (path.empty()) return nullptr;
    std::ifstream f(path, std::ios::binary);
    if (!f) return nullptr;
    auto v = std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return v->empty() ? nullptr : v;
}

}  // namespace

// Builds the theme from `data_dir` (the DATA directory holding PACKAGE.BIN). Null on failure.
std::shared_ptr<const SdTheme> build_savedata_theme(const std::string& data_dir, const std::string& mods_dir) {
    std::shared_ptr<SdImage> sheet, buttons, stripes, crescent, wing;
    static const char* kLangs[] = {"en", "jp", "fr", "de", "it", "es"};
    bool ok = false;
    for (const char* lang : kLangs) {
        std::string pack = std::string("menu/") + lang + "/menu_lpk_label_gim_0.bin";
        if (load_gim(data_dir, mods_dir, pack, 0, &sheet) && load_gim(data_dir, mods_dir, pack, 8, &buttons)) { ok = true; break; }
    }
    if (!ok) return nullptr;
    // Optional backdrop ornaments: the theme still works without them.
    load_gim(data_dir, mods_dir, "menu/menu_pk_label_gim_0.bin", 1, &stripes);
    load_gim(data_dir, mods_dir, "menu/menu_pk_label_gim_0.bin", 2, &crescent);
    load_gim(data_dir, mods_dir, "menu/menu_pk_label_gim_0.bin", 11, &wing);
    if (wing) despeckle(*wing);
    if (sheet->w != 128 || sheet->h != 128 || buttons->w != 128 || buttons->h != 128) {
        std::fprintf(stderr, "[SDTHEME] unexpected UI sheet size (%dx%d, %dx%d)\n", sheet->w, sheet->h, buttons->w, buttons->h);
        return nullptr;
    }

    auto t = std::make_shared<SdTheme>();
    auto S = std::shared_ptr<const SdImage>(sheet);
    auto B = std::shared_ptr<const SdImage>(buttons);
    t->background = spr(build_background(stripes.get(), crescent.get(), wing.get()), 0, 0, 480, 272);

    // Common sheet layout (pixels): see tools notes in the file comment.
    t->tab = spr(S, 2, 72, 15, 25);                    // navy hexagon: pointed light-blue tip + dark body
    t->tab.l = 10; t->tab.t = 8; t->tab.b = 8; t->tab.mirror = SdMirror::RightFromLeft;
    // The chevron bars are drawn from their right end only (the left end is its mirror); the stretched
    // middle must be a plain column range, as the sheet's full bars carry diagonal facets.
    t->row_selected = spr(S, 111, 107, 17, 20);        // royal blue: columns 111..117 uniform, tip 118..127
    t->row_selected.r = 10; t->row_selected.t = 4; t->row_selected.b = 5; t->row_selected.mirror = SdMirror::LeftFromRight;
    t->row_normal = spr(S, 41, 101, 20, 25);           // silver: columns 41..48 uniform, tip 49..60
    t->row_normal.r = 12; t->row_normal.t = 6; t->row_normal.b = 9; t->row_normal.mirror = SdMirror::LeftFromRight;
    t->cursor = spr(S, 2, 49, 32, 26);
    t->corner[0] = spr(S, 3, 2, 17, 17);               // the four corner stars of the dialog frame
    t->corner[1] = spr(S, 25, 2, 17, 17);              // (the 1px edge arms between them are drawn as the border)
    t->corner[2] = spr(S, 3, 24, 17, 17);
    t->corner[3] = spr(S, 25, 24, 17, 17);
    t->glyph_cross = spr(B, 109, 109, 16, 16);
    t->glyph_circle = spr(B, 73, 109, 16, 16);
    t->dpad_up = spr(B, 72, 88, 17, 19);
    t->dpad_down = spr(B, 89, 88, 17, 19);

    t->text = rgba(48, 50, 84);
    t->text_dim = rgba(98, 102, 140);
    t->text_selected = rgba(255, 255, 255);
    t->text_selected_dim = rgba(214, 220, 255);
    t->hint_text = rgba(255, 255, 255);
    t->title = rgba(246, 247, 255);
    t->title_edge = rgba(54, 56, 128);
    t->panel_fill = rgba(222, 224, 240, 224);
    t->panel_edge = rgba(255, 255, 255);
    t->panel_text = rgba(54, 54, 86);

    std::string reg, bold;
    if (psp_font_host_files(&reg, &bold)) {
        t->font_regular = read_font(reg);
        t->font_bold = read_font(bold.empty() ? reg : bold);
        t->font_scale = 0.86f;   // the host face is wider than the PSP's at the same size
    }
    return t->valid() ? t : nullptr;
}

}  // namespace dissidia

// Called lazily by the dialog the first time it draws.
static std::shared_ptr<const psp_sddialog::SdTheme> dissidia_theme_provider() {
    std::string data = psp_path_to_host("disc0:/PSP_GAME/USRDIR/DATA");
    if (data.empty()) return nullptr;
    auto t = dissidia::build_savedata_theme(data, dissidia_mods_dir());
    std::fprintf(stderr, "[SDTHEME] savedata dialog theme %s\n", t ? "built from the game's menu textures" : "unavailable (generic look)");
    return t;
}

void dissidia_install_savedata_theme() {
    if (const char* e = std::getenv("PSPRECOMP_SDTHEME")) { if (std::string(e) == "0") return; }
    psp_sddialog::set_theme_provider(dissidia_theme_provider);
}
