// sceLibFont HLE backed by FreeType.
//
// Generic (no title-specific logic). The PSP's PGF fonts are not parsed; a
// host TrueType font stands in for them. Struct layouts follow PPSSPP
// (Core/HLE/sceFont.cpp + Core/Font/PGF.h):
//   PGFFontStyle 0xA8, PGFFontInfo 0x108, PGFCharInfo 0x40, GlyphImage 0x14.
#include "hle/psp_hle.h"
#include "hle/psp_hle_font.h"
#include "psp_memory.h"
#include "recomp.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

bool psp_font_host_files(std::string* regular, std::string* bold) {
    auto exists = [](const std::string& p) {
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) return false;
        fclose(f);
        return true;
    };
    std::string reg;
    const char* env = getenv("PSPRECOMP_FONT");
    if (env && *env) {
        if (exists(env)) reg = env;
        else fprintf(stderr, "[HLE] sceFont: PSPRECOMP_FONT=%s not readable\n", env);
    }
    if (reg.empty()) {
        static const char* dirs[] = {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/TTF/",
                                     "/usr/share/fonts/dejavu/", "/usr/share/fonts/truetype/"};
        for (const char* d : dirs) {
            std::string p = std::string(d) + "DejaVuSans.ttf";
            if (exists(p)) { reg = p; break; }
        }
    }
    if (reg.empty()) return false;
    std::string b;
    size_t dot = reg.rfind(".ttf");
    if (dot != std::string::npos) {
        std::string cand = reg.substr(0, dot) + "-Bold.ttf";
        if (exists(cand)) b = cand;
    }
    if (regular) *regular = reg;
    if (bold) *bold = b;
    return true;
}

namespace {

// ---- error codes (PPSSPP Core/HLE/ErrorCodes.h) ----
constexpr int32_t FONT_ERR_OOM         = (int32_t)0x80460001U;
constexpr int32_t FONT_ERR_INVALID_LIB = (int32_t)0x80460002U;
constexpr int32_t FONT_ERR_INVALID_PAR = (int32_t)0x80460003U;

constexpr float kPointDPI = 72.0f;
constexpr float kDefaultRes = 128.0f;  // PPSSPP FontLib default resolution

// ---- PGFFontStyle (0xA8) offsets ----
constexpr uint32_t ST_FONTH = 0x00, ST_FONTV = 0x04, ST_HRES = 0x08, ST_VRES = 0x0C,
                   ST_WEIGHT = 0x10, ST_FAMILY = 0x14, ST_STYLE = 0x16, ST_SUB = 0x18,
                   ST_LANG = 0x1A, ST_REGION = 0x1C, ST_COUNTRY = 0x1E, ST_NAME = 0x20,
                   ST_FILE = 0x60, ST_ATTR = 0xA0, ST_EXPIRE = 0xA4, ST_SIZE = 0xA8;

enum { LANG_JAPANESE = 1, LANG_LATIN = 2, LANG_KOREAN = 3 };
enum { FAMILY_SANS = 1, FAMILY_SERIF = 2 };
enum { STYLE_REGULAR = 1, STYLE_ITALIC = 2, STYLE_BOLD = 5, STYLE_BOLD_ITALIC = 6, STYLE_DB = 103 };

// Mirror of PPSSPP's fontRegistry (minus zh_gb / kr0): index == font list index.
struct FontEntry {
    float hSize, vSize;  // points
    float hRes, vRes;    // dpi
    int family, style, lang;
    const char* file;
    const char* name;
};
const FontEntry kFonts[] = {
    {10.125f, 10.125f, 128, 128, FAMILY_SANS,  STYLE_DB,          LANG_JAPANESE, "jpn0.pgf",  "FTT-NewRodin Pro DB"},
    {10.125f, 10.125f, 128, 128, FAMILY_SANS,  STYLE_REGULAR,     LANG_LATIN,    "ltn0.pgf",  "FTT-NewRodin Pro Latin"},
    {10.125f, 10.125f, 128, 128, FAMILY_SERIF, STYLE_REGULAR,     LANG_LATIN,    "ltn1.pgf",  "FTT-Matisse Pro Latin"},
    {10.125f, 10.125f, 128, 128, FAMILY_SANS,  STYLE_ITALIC,      LANG_LATIN,    "ltn2.pgf",  "FTT-NewRodin Pro Latin"},
    {10.125f, 10.125f, 128, 128, FAMILY_SERIF, STYLE_ITALIC,      LANG_LATIN,    "ltn3.pgf",  "FTT-Matisse Pro Latin"},
    {10.125f, 10.125f, 128, 128, FAMILY_SANS,  STYLE_BOLD,        LANG_LATIN,    "ltn4.pgf",  "FTT-NewRodin Pro Latin"},
    {10.125f, 10.125f, 128, 128, FAMILY_SERIF, STYLE_BOLD,        LANG_LATIN,    "ltn5.pgf",  "FTT-Matisse Pro Latin"},
    {10.125f, 10.125f, 128, 128, FAMILY_SANS,  STYLE_BOLD_ITALIC, LANG_LATIN,    "ltn6.pgf",  "FTT-NewRodin Pro Latin"},
    {10.125f, 10.125f, 128, 128, FAMILY_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN,    "ltn7.pgf",  "FTT-Matisse Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SANS,  STYLE_REGULAR,     LANG_LATIN, "ltn8.pgf",  "FTT-NewRodin Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SERIF, STYLE_REGULAR,     LANG_LATIN, "ltn9.pgf",  "FTT-Matisse Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SANS,  STYLE_ITALIC,      LANG_LATIN, "ltn10.pgf", "FTT-NewRodin Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SERIF, STYLE_ITALIC,      LANG_LATIN, "ltn11.pgf", "FTT-Matisse Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SANS,  STYLE_BOLD,        LANG_LATIN, "ltn12.pgf", "FTT-NewRodin Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SERIF, STYLE_BOLD,        LANG_LATIN, "ltn13.pgf", "FTT-Matisse Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SANS,  STYLE_BOLD_ITALIC, LANG_LATIN, "ltn14.pgf", "FTT-NewRodin Pro Latin"},
    {7.0f, 7.0f, 128, 128, FAMILY_SERIF, STYLE_BOLD_ITALIC, LANG_LATIN, "ltn15.pgf", "FTT-Matisse Pro Latin"},
};
constexpr int kNumFonts = (int)(sizeof(kFonts) / sizeof(kFonts[0]));

bool style_is_bold(int s) { return s == STYLE_BOLD || s == STYLE_BOLD_ITALIC || s == STYLE_DB; }
bool style_is_italic(int s) { return s == STYLE_ITALIC || s == STYLE_BOLD_ITALIC; }

struct FontLib {
    float hRes = kDefaultRes, vRes = kDefaultRes;
    uint32_t altChar = 0;
};
struct OpenFont {
    uint32_t lib = 0;
    int index = 0;
};

std::mutex g_mtx;
std::map<uint32_t, FontLib> g_libs;
std::map<uint32_t, OpenFont> g_fonts;
uint32_t g_next_lib = 0x0F0F0001;
uint32_t g_next_font = 0x0F0F1000;

// ---- FreeType state ----
FT_Library g_ft = nullptr;
FT_Face g_face_regular = nullptr;
FT_Face g_face_bold = nullptr;
bool g_ft_tried = false;
bool g_ft_ok = false;

void log_once(const char* name) {
    static std::map<std::string, bool> seen;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (seen.emplace(name, true).second) fprintf(stderr, "[HLE] %s (FreeType libfont)\n", name);
}

// Called with g_mtx held.
bool ft_init() {
    if (g_ft_tried) return g_ft_ok;
    g_ft_tried = true;
    std::string regular, boldp;
    if (!psp_font_host_files(&regular, &boldp)) {
        fprintf(stderr, "[HLE] sceFont: no TrueType font found (set PSPRECOMP_FONT); keeping blank-glyph stub behaviour\n");
        return false;
    }
    if (FT_Init_FreeType(&g_ft)) {
        fprintf(stderr, "[HLE] sceFont: FT_Init_FreeType failed\n");
        return false;
    }
    if (FT_New_Face(g_ft, regular.c_str(), 0, &g_face_regular)) {
        fprintf(stderr, "[HLE] sceFont: cannot load %s\n", regular.c_str());
        return false;
    }
    // Bold: sibling "<name>-Bold.ttf" if present, otherwise the regular face.
    if (!boldp.empty()) FT_New_Face(g_ft, boldp.c_str(), 0, &g_face_bold);
    if (!g_face_bold) g_face_bold = g_face_regular;
    fprintf(stderr, "[HLE] sceFont: using host font %s\n", regular.c_str());
    g_ft_ok = true;
    return true;
}

struct GlyphData {
    bool ok = false;
    int bmpW = 0, bmpH = 0, left = 0, top = 0;
    int32_t width = 0, height = 0, asc = 0, desc = 0, bhx = 0, bhy = 0, bvx = 0, bvy = 0, advH = 0, advV = 0;
    std::vector<uint8_t> cov;  // bmpW*bmpH 8-bit coverage
};

// Selects face + size for (font, lib); g_mtx held. Returns null if FreeType unavailable.
FT_Face select_face(const OpenFont& f, const FontLib& lib) {
    if (!ft_init()) return nullptr;
    const FontEntry& e = kFonts[std::clamp(f.index, 0, kNumFonts - 1)];
    FT_Face face = style_is_bold(e.style) ? g_face_bold : g_face_regular;
    // 26.6 point size, resolution from the lib (default 128 dpi, like PPSSPP).
    // The PGF point size is the em box; the PSP's bitmap glyphs sit well
    // inside it (ltn0 caps are ~11 px in an 18 px em, with a baseline that
    // fits a 16-row line buffer). A host TTF at the nominal em renders ~20%
    // too large and overflows the game's glyph buffers, so scale the em.
    static const float em_scale = [] {
        const char* v = std::getenv("PSPRECOMP_FONT_SCALE");
        return v ? (float)atof(v) : 0.8f;
    }();
    FT_Set_Char_Size(face, (FT_F26Dot6)lround(e.hSize * em_scale * 64.0f), (FT_F26Dot6)lround(e.vSize * em_scale * 64.0f),
                     (FT_UInt)lround(lib.hRes), (FT_UInt)lround(lib.vRes));
    FT_Matrix shear = {0x10000, 0x3000, 0, 0x10000};  // ~0.19 slant for italic
    if (style_is_italic(e.style)) FT_Set_Transform(face, &shear, nullptr);
    else FT_Set_Transform(face, nullptr, nullptr);
    return face;
}

// Blank glyph with a given horizontal advance (26.6 px): no ink, no bearings.
// Vertical advance is taken from the space glyph so line metrics stay sane.
void make_blank(FT_Face face, int32_t adv26_6, GlyphData& g) {
    int32_t advV = 0;
    if (FT_Load_Char(face, ' ', FT_LOAD_TARGET_LIGHT) == 0) advV = (int32_t)face->glyph->metrics.vertAdvance;
    if (advV <= 0) advV = (int32_t)face->size->metrics.height;
    g = GlyphData();
    g.advH = adv26_6;
    g.advV = advV;
    g.ok = true;  // drawn as nothing; metrics still reported
}

// Class of a code point the host font cannot show, decided the way the firmware
// font behaves for characters outside the PGF (nothing is drawn, never a box).
//   ZERO  formatting / control codes: zero advance
//   EM    full-width space and other full-width blanks: one em
//   HALF  anything else unmapped: half an em
enum class Miss { ZERO, EM, HALF };
Miss classify_missing(uint32_t c) {
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) return Miss::ZERO;
    if (c == 0x00AD || (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E) ||
        (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF || (c >= 0xFE00 && c <= 0xFE0F))
        return Miss::ZERO;
    if (c == 0x3000 || c == 0x2003 || c == 0x2001 || (c >= 0x2E80 && c < 0xA000) || (c >= 0xAC00 && c < 0xD7B0) ||
        (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xFF00 && c <= 0xFFEF))
        return Miss::EM;
    return Miss::HALF;
}

bool render_glyph(FT_Face face, uint32_t lib_alt, uint32_t code, GlyphData& g) {
    code &= 0xFFFF;
    FT_UInt gi = FT_Get_Char_Index(face, code);
    static const bool flog = std::getenv("PSPRECOMP_FONT_LOG") != nullptr;
    int32_t em26 = (int32_t)FT_MulFix(face->units_per_EM, face->size->metrics.x_scale);
    int32_t emShift26 = 0;  // centring shift for full-width forms drawn from ASCII glyphs
    if (gi == 0) {
        // Full-width ASCII forms (U+FF01..FF5E): draw the half-width glyph in a one-em cell.
        if (code >= 0xFF01 && code <= 0xFF5E) {
            gi = FT_Get_Char_Index(face, code - 0xFF01 + 0x21);
            if (gi) emShift26 = 1;  // flag, resolved below once the advance is known
        }
    }
    if (gi == 0 && code == 0xA0) gi = FT_Get_Char_Index(face, ' ');
    if (gi == 0 && lib_alt && (lib_alt & 0xFFFF) != code && classify_missing(code) == Miss::HALF) gi = FT_Get_Char_Index(face, lib_alt & 0xFFFF);
    if (gi == 0) {
        // Not in the host font and no usable alternate: advance by a blank, never .notdef.
        Miss m = classify_missing(code);
        if (flog) {
            static std::map<uint32_t, bool> seen_missing;
            if (seen_missing.emplace(code, true).second)
                fprintf(stderr, "[FONTLOG] code U+%04X not in host font -> blank (%s)\n", code,
                        m == Miss::ZERO ? "zero-width" : m == Miss::EM ? "1em" : "0.5em");
        }
        make_blank(face, m == Miss::ZERO ? 0 : m == Miss::EM ? em26 : em26 / 2, g);
        return true;
    }
    if (FT_Load_Glyph(face, gi, FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER)) return false;
    FT_GlyphSlot s = face->glyph;
    g.bmpW = (int)s->bitmap.width;
    g.bmpH = (int)s->bitmap.rows;
    g.left = s->bitmap_left;
    g.top = s->bitmap_top;
    g.width = (int32_t)s->metrics.width;
    g.height = (int32_t)s->metrics.height;
    g.asc = (int32_t)s->metrics.horiBearingY;
    g.desc = (int32_t)(s->metrics.horiBearingY - s->metrics.height);  // negative below baseline
    g.bhx = (int32_t)s->metrics.horiBearingX;
    g.bhy = (int32_t)s->metrics.horiBearingY;
    g.bvx = (int32_t)s->metrics.vertBearingX;
    g.bvy = (int32_t)s->metrics.vertBearingY;
    g.advH = (int32_t)s->metrics.horiAdvance;
    g.advV = (int32_t)s->metrics.vertAdvance;
    if (emShift26 && em26 > g.advH) {  // one-em cell, glyph centred
        int32_t d = (em26 - g.advH) / 2;
        g.bhx += d;
        g.left += (d + 32) >> 6;
        g.advH = em26;
    }
    g.cov.assign((size_t)g.bmpW * g.bmpH, 0);
    const FT_Bitmap& b = s->bitmap;
    for (int y = 0; y < g.bmpH; y++) {
        const uint8_t* row = b.buffer + (ptrdiff_t)y * b.pitch;
        for (int x = 0; x < g.bmpW; x++) {
            uint8_t v = 0;
            if (b.pixel_mode == FT_PIXEL_MODE_GRAY) v = row[x];
            else if (b.pixel_mode == FT_PIXEL_MODE_MONO) v = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
            g.cov[(size_t)y * g.bmpW + x] = v;
        }
    }
    g.ok = true;
    return true;
}

// ---- guest helpers ----
uint32_t A(recomp_context* c, int n) { return (uint32_t)c->r[4 + n]; }
void put_err(uint8_t* rd, uint32_t p, int32_t v) { if (p) psp_mem_write<int32_t>(rd, p, v); }
void ret(recomp_context* c, int32_t v) { c->r[2] = v; }

bool lookup_font(uint32_t h, OpenFont& f, FontLib& l) {
    auto it = g_fonts.find(h);
    if (it == g_fonts.end()) return false;
    f = it->second;
    auto lt = g_libs.find(f.lib);
    l = lt != g_libs.end() ? lt->second : FontLib{};
    return true;
}

void write_style(uint8_t* rd, uint32_t p, const FontEntry& e, const FontLib* lib) {
    for (uint32_t i = 0; i < ST_SIZE; i += 4) psp_mem_write<uint32_t>(rd, p + i, 0);
    psp_mem_write<float>(rd, p + ST_FONTH, e.hSize);
    psp_mem_write<float>(rd, p + ST_FONTV, e.vSize);
    psp_mem_write<float>(rd, p + ST_HRES, lib ? lib->hRes : e.hRes);
    psp_mem_write<float>(rd, p + ST_VRES, lib ? lib->vRes : e.vRes);
    psp_mem_write<uint16_t>(rd, p + ST_FAMILY, (uint16_t)e.family);
    psp_mem_write<uint16_t>(rd, p + ST_STYLE, (uint16_t)e.style);
    psp_mem_write<uint16_t>(rd, p + ST_LANG, (uint16_t)e.lang);
    for (int i = 0; e.name[i] && i < 63; i++) psp_mem_write<uint8_t>(rd, p + ST_NAME + i, (uint8_t)e.name[i]);
    for (int i = 0; e.file[i] && i < 63; i++) psp_mem_write<uint8_t>(rd, p + ST_FILE + i, (uint8_t)e.file[i]);
}

// Score a font entry against a requested style in guest memory (0 = don't care).
int style_score(uint8_t* rd, uint32_t sp, const FontEntry& e, bool exact, bool& matches) {
    uint16_t fam = psp_mem_read<uint16_t>(rd, sp + ST_FAMILY);
    uint16_t sty = psp_mem_read<uint16_t>(rd, sp + ST_STYLE);
    uint16_t lang = psp_mem_read<uint16_t>(rd, sp + ST_LANG);
    float h = psp_mem_read<float>(rd, sp + ST_FONTH);
    matches = (!lang || lang == e.lang) && (!fam || fam == e.family) && (!sty || sty == e.style);
    if (exact) return matches ? 1 : -1;
    int score = 0;
    if (lang) score += (lang == e.lang) ? 1000 : 0;
    if (fam) score += (fam == e.family) ? 100 : 0;
    if (sty) score += (sty == e.style) ? 50 : 0;
    if (h > 0.0f) score += 40 - std::min(40, (int)std::fabs(h - e.hSize) * 4);
    return score;
}

// ---- handlers ----
void h_NewLib(uint8_t* rd, recomp_context* c) {
    log_once("sceFontNewLib");
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t h = g_next_lib++;
    g_libs[h] = FontLib{};
    put_err(rd, A(c, 1), 0);
    ret(c, (int32_t)h);
}
void h_DoneLib(uint8_t*, recomp_context* c) {
    log_once("sceFontDoneLib");
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t h = A(c, 0);
    g_libs.erase(h);
    for (auto it = g_fonts.begin(); it != g_fonts.end();) it = (it->second.lib == h) ? g_fonts.erase(it) : std::next(it);
    ret(c, SCE_OK);
}
void open_common(uint8_t* rd, recomp_context* c, uint32_t lib, int index, uint32_t errp) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_libs.count(lib)) { put_err(rd, errp, FONT_ERR_INVALID_LIB); ret(c, 0); return; }
    if (index < 0 || index >= kNumFonts) { put_err(rd, errp, FONT_ERR_INVALID_PAR); ret(c, 0); return; }
    uint32_t h = g_next_font++;
    g_fonts[h] = OpenFont{lib, index};
    put_err(rd, errp, 0);
    ret(c, (int32_t)h);
}
void h_Open(uint8_t* rd, recomp_context* c) {  // (lib, index, mode, *err)
    log_once("sceFontOpen");
    open_common(rd, c, A(c, 0), (int)A(c, 1), A(c, 3));
}
// User-supplied PGF data (memory / file) cannot be parsed; map to the default Latin font.
void h_OpenUserMemory(uint8_t* rd, recomp_context* c) {  // (lib, mem, size, *err)
    log_once("sceFontOpenUserMemory");
    open_common(rd, c, A(c, 0), 1, A(c, 3));
}
void h_OpenUserFile(uint8_t* rd, recomp_context* c) {  // (lib, filename, mode, *err)
    log_once("sceFontOpenUserFile");
    open_common(rd, c, A(c, 0), 1, A(c, 3));
}
void h_Close(uint8_t*, recomp_context* c) {
    log_once("sceFontClose");
    std::lock_guard<std::mutex> lk(g_mtx);
    ret(c, g_fonts.erase(A(c, 0)) ? SCE_OK : 0);
}
void find_common(uint8_t* rd, recomp_context* c, bool exact) {  // (lib, *style, *err)
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t errp = A(c, 2), sp = A(c, 1);
    if (!g_libs.count(A(c, 0))) { put_err(rd, errp, FONT_ERR_INVALID_LIB); ret(c, 0); return; }
    if (!sp) { put_err(rd, errp, FONT_ERR_INVALID_PAR); ret(c, 0); return; }
    int best = -1, best_score = -1;
    for (int i = 0; i < kNumFonts; i++) {
        bool m;
        int s = style_score(rd, sp, kFonts[i], exact, m);
        if (exact && !m) continue;
        if (s > best_score) { best_score = s; best = i; }
    }
    put_err(rd, errp, 0);
    ret(c, exact ? best : (best < 0 ? 0 : best));  // FindFont: -1 when no match
}
void h_FindFont(uint8_t* rd, recomp_context* c) { log_once("sceFontFindFont"); find_common(rd, c, true); }
void h_FindOptimumFont(uint8_t* rd, recomp_context* c) { log_once("sceFontFindOptimumFont"); find_common(rd, c, false); }
void h_GetNumFontList(uint8_t* rd, recomp_context* c) {
    log_once("sceFontGetNumFontList");
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_libs.count(A(c, 0))) { put_err(rd, A(c, 1), FONT_ERR_INVALID_LIB); ret(c, 0); return; }
    put_err(rd, A(c, 1), 0);
    ret(c, kNumFonts);
}
void h_GetFontList(uint8_t* rd, recomp_context* c) {  // (lib, *styles, numFonts)
    log_once("sceFontGetFontList");
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t p = A(c, 1);
    int n = std::min((int)A(c, 2), kNumFonts);
    auto it = g_libs.find(A(c, 0));
    if (it == g_libs.end() || !p) { ret(c, FONT_ERR_INVALID_PAR); return; }
    for (int i = 0; i < n; i++) write_style(rd, p + (uint32_t)i * ST_SIZE, kFonts[i], &it->second);
    ret(c, SCE_OK);
}
void h_GetFontInfoByIndexNumber(uint8_t* rd, recomp_context* c) {  // (lib, *style, index)
    log_once("sceFontGetFontInfoByIndexNumber");
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_libs.find(A(c, 0));
    int idx = (int)A(c, 2);
    if (it == g_libs.end()) { ret(c, FONT_ERR_INVALID_LIB); return; }
    if (idx < 0 || idx >= kNumFonts || !A(c, 1)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    write_style(rd, A(c, 1), kFonts[idx], &it->second);
    ret(c, SCE_OK);
}
void h_GetFontInfo(uint8_t* rd, recomp_context* c) {  // (font, *PGFFontInfo 0x108)
    log_once("sceFontGetFontInfo");
    std::lock_guard<std::mutex> lk(g_mtx);
    OpenFont f; FontLib l;
    uint32_t p = A(c, 1);
    if (!p || !lookup_font(A(c, 0), f, l)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    const FontEntry& e = kFonts[f.index];
    for (uint32_t i = 0; i < 0x108; i += 4) psp_mem_write<uint32_t>(rd, p + i, 0);
    int32_t asc, desc, advx, maxw;
    FT_Face face = select_face(f, l);
    if (face) {
        asc = (int32_t)face->size->metrics.ascender;
        desc = (int32_t)face->size->metrics.descender;  // negative
        advx = (int32_t)face->size->metrics.max_advance;
    } else {
        float px = e.vSize * l.vRes / kPointDPI;
        asc = (int32_t)(px * 64.0f * 0.8f);
        desc = -(int32_t)(px * 64.0f * 0.2f);
        advx = (int32_t)(px * 64.0f);
    }
    maxw = advx;
    int32_t hgt = asc - desc;
    // 26.6 block (PGFFontInfo 0x00..0x27): width, height, ascender, descender,
    // leftX, baseY, minCenterX, topY, advanceX, advanceY
    // Descender is NEGATIVE (below the baseline), as in the PGF header:
    // Dissidia sizes its line buffers as (baseline - maxGlyphDescender) + 3.
    int32_t fx[10] = {maxw, hgt, asc, desc, 0, asc, 0, asc, advx, hgt};
    for (int i = 0; i < 10; i++) {
        psp_mem_write<int32_t>(rd, p + 4 * i, fx[i]);
        psp_mem_write<float>(rd, p + 0x28 + 4 * i, fx[i] / 64.0f);
    }
    if (std::getenv("PSPRECOMP_FONT_TRACE"))
        std::fprintf(stderr, "[FONT] fontinfo asc=%d desc=%d hgt=%d advx=%d (26.6)\n", asc, desc, hgt, advx);
    psp_mem_write<int16_t>(rd, p + 0x50, (int16_t)((maxw + 63) >> 6));
    psp_mem_write<int16_t>(rd, p + 0x52, (int16_t)((hgt + 63) >> 6));
    psp_mem_write<int32_t>(rd, p + 0x54, 0xFFFF);  // numGlyphs
    psp_mem_write<int32_t>(rd, p + 0x58, 0);       // shadowMapLength
    write_style(rd, p + 0x5C, e, &l);
    psp_mem_write<uint8_t>(rd, p + 0x104, 4);  // BPP
    ret(c, SCE_OK);
}

bool glyph_for(uint32_t fh, uint32_t code, GlyphData& g, uint32_t* alt_unused = nullptr) {
    (void)alt_unused;
    OpenFont f; FontLib l;
    if (!lookup_font(fh, f, l)) return false;
    FT_Face face = select_face(f, l);
    if (!face) return true;  // stub behaviour: blank glyph below
    return render_glyph(face, l.altChar, code, g);
}

void write_charinfo(uint8_t* rd, uint32_t p, const GlyphData& g, bool blank) {
    for (uint32_t i = 0; i < 0x40; i += 4) psp_mem_write<uint32_t>(rd, p + i, 0);
    if (blank) {  // no host font: legacy stub metrics (16x16 box, advance 16)
        psp_mem_write<uint32_t>(rd, p + 0, 16); psp_mem_write<uint32_t>(rd, p + 4, 16);
        psp_mem_write<uint32_t>(rd, p + 0x10, 16 << 6); psp_mem_write<uint32_t>(rd, p + 0x14, 16 << 6);
        psp_mem_write<uint32_t>(rd, p + 0x30, 16 << 6); psp_mem_write<uint32_t>(rd, p + 0x34, 16 << 6);
        return;
    }
    psp_mem_write<uint32_t>(rd, p + 0x00, (uint32_t)g.bmpW);
    psp_mem_write<uint32_t>(rd, p + 0x04, (uint32_t)g.bmpH);
    psp_mem_write<int32_t>(rd, p + 0x08, g.left);
    psp_mem_write<int32_t>(rd, p + 0x0C, g.top);
    psp_mem_write<int32_t>(rd, p + 0x10, g.width);
    psp_mem_write<int32_t>(rd, p + 0x14, g.height);
    psp_mem_write<int32_t>(rd, p + 0x18, g.asc);
    psp_mem_write<int32_t>(rd, p + 0x1C, g.desc);
    psp_mem_write<int32_t>(rd, p + 0x20, g.bhx);
    psp_mem_write<int32_t>(rd, p + 0x24, g.bhy);
    psp_mem_write<int32_t>(rd, p + 0x28, g.bvx);
    psp_mem_write<int32_t>(rd, p + 0x2C, g.bvy);
    psp_mem_write<int32_t>(rd, p + 0x30, g.advH);
    psp_mem_write<int32_t>(rd, p + 0x34, g.advV);
    // 0x38 shadowFlags u16, 0x3A shadowId u16 left zero
}

void h_GetCharInfo(uint8_t* rd, recomp_context* c) {  // (font, charCode, *PGFCharInfo)
    log_once("sceFontGetCharInfo");
    std::lock_guard<std::mutex> lk(g_mtx);
    GlyphData g;
    uint32_t p = A(c, 2);
    if (!p || !glyph_for(A(c, 0), A(c, 1) & 0xFFFF, g)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    write_charinfo(rd, p, g, !g.ok);
    ret(c, SCE_OK);
}
void h_GetShadowInfo(uint8_t* rd, recomp_context* c) {  // blank
    log_once("sceFontGetShadowInfo");
    std::lock_guard<std::mutex> lk(g_mtx);
    OpenFont f; FontLib l;
    if (!A(c, 2) || !lookup_font(A(c, 0), f, l)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    for (uint32_t i = 0; i < 0x40; i += 4) psp_mem_write<uint32_t>(rd, A(c, 2) + i, 0);
    ret(c, SCE_OK);
}
// FontImageRect: s16 width, s16 height
void h_GetCharImageRect(uint8_t* rd, recomp_context* c) {
    log_once("sceFontGetCharImageRect");
    std::lock_guard<std::mutex> lk(g_mtx);
    GlyphData g;
    uint32_t p = A(c, 2);
    if (!p || !glyph_for(A(c, 0), A(c, 1) & 0xFFFF, g)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    psp_mem_write<int16_t>(rd, p, (int16_t)g.bmpW);
    psp_mem_write<int16_t>(rd, p + 2, (int16_t)g.bmpH);
    ret(c, SCE_OK);
}
void h_GetShadowImageRect(uint8_t* rd, recomp_context* c) {
    log_once("sceFontGetShadowImageRect");
    std::lock_guard<std::mutex> lk(g_mtx);
    OpenFont f; FontLib l;
    uint32_t p = A(c, 2);
    if (!p || !lookup_font(A(c, 0), f, l)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    psp_mem_write<int16_t>(rd, p, 0);
    psp_mem_write<int16_t>(rd, p + 2, 0);
    ret(c, SCE_OK);
}

// Blit coverage into the guest GlyphImage buffer. clip* < 0 (w/h) => no clip.
// GlyphImage: u32 pixelFormat, s32 xPos64, s32 yPos64, u16 bufW, u16 bufH, u16 bytesPerLine, u16 pad, u32 buffer
void draw_glyph(uint8_t* rd, uint32_t gp, const GlyphData& g, bool have_clip, int cx, int cy, int cw, int ch) {
    uint32_t fmt = psp_mem_read<uint32_t>(rd, gp + 0);
    int32_t xp = psp_mem_read<int32_t>(rd, gp + 4);
    int32_t yp = psp_mem_read<int32_t>(rd, gp + 8);
    int bw = psp_mem_read<uint16_t>(rd, gp + 12);
    int bh = psp_mem_read<uint16_t>(rd, gp + 14);
    int bpl = psp_mem_read<uint16_t>(rd, gp + 16);
    uint32_t buf = psp_mem_read<uint32_t>(rd, gp + 20);
    static const bool trace = std::getenv("PSPRECOMP_FONT_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "[FONT] glyph fmt=%u pos=(%d,%d)/64 buf=%dx%d bpl=%d @0x%08X bmp=%dx%d left=%d top=%d advH=%d asc=%d desc=%d bhy=%d h=%d\n",
                            fmt, xp, yp, bw, bh, bpl, buf, g.bmpW, g.bmpH, g.left, g.top, g.advH, g.asc, g.desc, g.bhy, g.height);
    if (!buf || !g.ok) return;
    // 26.6 position of the bitmap's top-left corner inside the buffer.
    int ox = (xp >= 0) ? (xp >> 6) : -((-xp + 63) >> 6);
    int oy = (yp >= 0) ? (yp >> 6) : -((-yp + 63) >> 6);
    int x0 = 0, y0 = 0, x1 = bw, y1 = bh;
    if (have_clip) {
        x0 = std::max(x0, cx); y0 = std::max(y0, cy);
        x1 = std::min(x1, cx + cw); y1 = std::min(y1, cy + ch);
    }
    for (int y = 0; y < g.bmpH; y++) {
        int dy = oy + y;
        if (dy < y0 || dy >= y1) continue;
        for (int x = 0; x < g.bmpW; x++) {
            int dx = ox + x;
            if (dx < x0 || dx >= x1) continue;
            uint8_t v = g.cov[(size_t)y * g.bmpW + x];
            switch (fmt) {
            case 0: case 1: {  // 4bpp (0: low nibble first, 1: high nibble first)
                uint32_t a = buf + (uint32_t)dy * bpl + (uint32_t)(dx >> 1);
                uint8_t cur = psp_mem_read<uint8_t>(rd, a);
                bool lo = ((dx & 1) == 0) == (fmt == 0);
                uint8_t n = v >> 4;
                cur = lo ? (uint8_t)((cur & 0xF0) | n) : (uint8_t)((cur & 0x0F) | (n << 4));
                psp_mem_write<uint8_t>(rd, a, cur);
                break;
            }
            case 2:  // 8bpp
                psp_mem_write<uint8_t>(rd, buf + (uint32_t)dy * bpl + (uint32_t)dx, v);
                break;
            case 3: {  // 24bpp
                uint32_t a = buf + (uint32_t)dy * bpl + (uint32_t)dx * 3;
                for (int k = 0; k < 3; k++) psp_mem_write<uint8_t>(rd, a + k, v);
                break;
            }
            case 4:  // 32bpp
                psp_mem_write<uint32_t>(rd, buf + (uint32_t)dy * bpl + (uint32_t)dx * 4,
                                        ((uint32_t)v << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v);
                break;
            default: return;
            }
        }
    }
    static int dumped = 0;
    if (trace && dumped < 6 && fmt == 0) {
        dumped++;
        std::fprintf(stderr, "[FONT] buffer after blit (%dx%d):\n", bw, bh);
        for (int yy = 0; yy < bh; yy++) {
            char line[520]; int n = 0;
            for (int xx = 0; xx < bw && n < 500; xx++) {
                uint8_t b = psp_mem_read<uint8_t>(rd, buf + (uint32_t)yy * bpl + (uint32_t)(xx >> 1));
                uint8_t v = (xx & 1) ? (b >> 4) : (b & 0xF);
                line[n++] = v == 0 ? '.' : v < 8 ? '+' : '#';
            }
            line[n] = 0;
            std::fprintf(stderr, "[FONT] |%s|\n", line);
        }
    }
}

void glyph_image_common(uint8_t* rd, recomp_context* c, bool clip, bool shadow) {
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t gp = A(c, 2);
    if (!gp) { ret(c, FONT_ERR_INVALID_PAR); return; }
    OpenFont f; FontLib l;
    if (!lookup_font(A(c, 0), f, l)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    if (shadow) { ret(c, SCE_OK); return; }  // blank shadow: leave buffer untouched
    GlyphData g;
    if (!glyph_for(A(c, 0), A(c, 1) & 0xFFFF, g)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    if (g.ok) {
        if (clip) {
            uint32_t sp = (uint32_t)c->r[29];  // 5th..7th args live at sp+16..24 (o32)
            int cx = (int)A(c, 3);
            int cy = psp_mem_read<int32_t>(rd, sp + 16);
            int cw = psp_mem_read<int32_t>(rd, sp + 20);
            int ch = psp_mem_read<int32_t>(rd, sp + 24);
            draw_glyph(rd, gp, g, true, cx, cy, cw, ch);
        } else {
            draw_glyph(rd, gp, g, false, 0, 0, 0, 0);
        }
    }
    ret(c, SCE_OK);
}
void h_GetCharGlyphImage(uint8_t* rd, recomp_context* c) { log_once("sceFontGetCharGlyphImage"); glyph_image_common(rd, c, false, false); }
void h_GetCharGlyphImage_Clip(uint8_t* rd, recomp_context* c) { log_once("sceFontGetCharGlyphImage_Clip"); glyph_image_common(rd, c, true, false); }
void h_GetShadowGlyphImage(uint8_t* rd, recomp_context* c) { log_once("sceFontGetShadowGlyphImage"); glyph_image_common(rd, c, false, true); }
void h_GetShadowGlyphImage_Clip(uint8_t* rd, recomp_context* c) { log_once("sceFontGetShadowGlyphImage_Clip"); glyph_image_common(rd, c, true, true); }

void h_SetAltCharacterCode(uint8_t*, recomp_context* c) {
    log_once("sceFontSetAltCharacterCode");
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_libs.find(A(c, 0));
    if (it == g_libs.end()) { ret(c, FONT_ERR_INVALID_LIB); return; }
    it->second.altChar = A(c, 1);
    ret(c, SCE_OK);
}
void h_SetResolution(uint8_t*, recomp_context* c) {  // (lib, float hRes=f12, float vRes=f13)
    log_once("sceFontSetResolution");
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_libs.find(A(c, 0));
    if (it == g_libs.end()) { ret(c, FONT_ERR_INVALID_LIB); return; }
    float h = c->f[12], v = c->f[13];
    if (!(h > 0.0f) || !(v > 0.0f)) { ret(c, FONT_ERR_INVALID_PAR); return; }
    it->second.hRes = h;
    it->second.vRes = v;
    ret(c, SCE_OK);
}
void h_Flush(uint8_t*, recomp_context* c) {
    log_once("sceFontFlush");
    std::lock_guard<std::mutex> lk(g_mtx);
    ret(c, g_fonts.count(A(c, 0)) ? SCE_OK : FONT_ERR_INVALID_PAR);
}
void h_CalcMemorySize(uint8_t*, recomp_context* c) { log_once("sceFontCalcMemorySize"); ret(c, 0); }

// float sceFont{Pixel,Point}To{Point,Pixel}{H,V}(int lib, float v (f12), u32 *err (a1)) -> f0
void conv_common(uint8_t* rd, recomp_context* c, bool vertical, bool to_point) {
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t errp = A(c, 1);
    auto it = g_libs.find(A(c, 0));
    if (it == g_libs.end()) { put_err(rd, errp, FONT_ERR_INVALID_LIB); c->f[0] = 0.0f; return; }
    float res = vertical ? it->second.vRes : it->second.hRes;
    float v = c->f[12];
    put_err(rd, errp, 0);
    c->f[0] = to_point ? v * kPointDPI / res : v * res / kPointDPI;
}
void h_PixelToPointH(uint8_t* rd, recomp_context* c) { log_once("sceFontPixelToPointH"); conv_common(rd, c, false, true); }
void h_PixelToPointV(uint8_t* rd, recomp_context* c) { log_once("sceFontPixelToPointV"); conv_common(rd, c, true, true); }
void h_PointToPixelH(uint8_t* rd, recomp_context* c) { log_once("sceFontPointToPixelH"); conv_common(rd, c, false, false); }
void h_PointToPixelV(uint8_t* rd, recomp_context* c) { log_once("sceFontPointToPixelV"); conv_common(rd, c, true, false); }

}  // namespace

void psp_hle_register_font() {
    psp_hle_register("sceFontNewLib", h_NewLib);
    psp_hle_register("sceFontDoneLib", h_DoneLib);
    psp_hle_register("sceFontOpen", h_Open);
    psp_hle_register("sceFontOpenUserMemory", h_OpenUserMemory);
    psp_hle_register("sceFontOpenUserFile", h_OpenUserFile);
    psp_hle_register("sceFontClose", h_Close);
    psp_hle_register("sceFontFindFont", h_FindFont);
    psp_hle_register("sceFontFindOptimumFont", h_FindOptimumFont);
    psp_hle_register("sceFontGetNumFontList", h_GetNumFontList);
    psp_hle_register("sceFontGetFontList", h_GetFontList);
    psp_hle_register("sceFontGetFontInfo", h_GetFontInfo);
    psp_hle_register("sceFontGetFontInfoByIndexNumber", h_GetFontInfoByIndexNumber);
    psp_hle_register("sceFontGetCharInfo", h_GetCharInfo);
    psp_hle_register("sceFontGetShadowInfo", h_GetShadowInfo);
    psp_hle_register("sceFontGetCharImageRect", h_GetCharImageRect);
    psp_hle_register("sceFontGetShadowImageRect", h_GetShadowImageRect);
    psp_hle_register("sceFontGetCharGlyphImage", h_GetCharGlyphImage);
    psp_hle_register("sceFontGetCharGlyphImage_Clip", h_GetCharGlyphImage_Clip);
    psp_hle_register("sceFontGetShadowGlyphImage", h_GetShadowGlyphImage);
    psp_hle_register("sceFontGetShadowGlyphImage_Clip", h_GetShadowGlyphImage_Clip);
    psp_hle_register("sceFontSetAltCharacterCode", h_SetAltCharacterCode);
    psp_hle_register("sceFontSetResolution", h_SetResolution);
    psp_hle_register("sceFontFlush", h_Flush);
    psp_hle_register("sceFontCalcMemorySize", h_CalcMemorySize);
    psp_hle_register("sceFontPixelToPointH", h_PixelToPointH);
    psp_hle_register("sceFontPixelToPointV", h_PixelToPointV);
    psp_hle_register("sceFontPointToPixelH", h_PointToPixelH);
    psp_hle_register("sceFontPointToPixelV", h_PointToPixelV);
}
