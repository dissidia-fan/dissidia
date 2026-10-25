// psp_tex_pack.h -- PPSSPP-style texture packs (dump / replace / upscale).
//
// Stable texture id: 64-bit xxhash of the decoded SOURCE texels (unswizzled,
// repacked to width x height, so independent of guest address, swizzling and
// row stride), the palette entries the texels actually reference (CLUT4/CLUT8
// only, as RGBA, so every palette variant of an image gets its own id and
// unused palette garbage does not), the GE format and the texture size.
//
//   <pack>/dump/<id>.png   written by PSPRECOMP_TEX_DUMP=1 (first sight)
//   <pack>/<id>.png        replacement (any size, RGBA/RGB/gray/palette PNG)
//   <pack>/textures.txt    index: id WxH format clut-format first-addr seen
//
// <pack> comes from PSPRECOMP_TEXPACK, else the game module's provider
// (psp_texpack_set_dir_provider), else <ms0 parent>/mods/textures.
// Everything here runs on the render thread (ge_texture_bind).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using PspTexPackDirFn = std::string (*)();
/// Game-module seam: return the texture pack directory (called lazily, once,
/// after psp_io_init). Without a provider the default is <ms0 parent>/mods/textures.
void psp_texpack_set_dir_provider(PspTexPackDirFn fn);

/// True when any of dump / replacement (non-empty pack) / upscale is on.
/// Cheap after the first call; when false ge_texture_bind computes no ids.
bool psp_texpack_active();

/// clut_bits: 0 (not palettised), 4 or 8. clut_rgba: 256 RGBA entries (used
/// only when clut_bits != 0). raw: width*height texels at `bpp` bits,
/// unswizzled and packed (stride == width).
uint64_t psp_texpack_make_id(const uint8_t* raw, size_t raw_len, int fmt, int w, int h,
                             int clut_bits, const uint8_t* clut_rgba);

/// Load <pack>/<id>.png as RGBA8 if it exists (file list cached at start, no
/// stat per call). False when there is no replacement or the file is bad
/// (a bad file is reported once and treated as a miss).
bool psp_texpack_load_replacement(uint64_t id, std::vector<uint8_t>& rgba, int& w, int& h);

struct PspTexMeta {
    int w, h, fmt;
    int clut_fmt;       // CLUTFORMAT & 3 for palettised textures, else -1
    uint32_t addr;      // guest address at first sight
};
/// Record one decode of `id` (index counts); dump the PNG on first sight if
/// PSPRECOMP_TEX_DUMP=1. rgba is w*h*4 (the original, un-upscaled decode).
void psp_texpack_note(uint64_t id, const PspTexMeta& m, const uint8_t* rgba);

/// PSPRECOMP_TEX_UPSCALE=xbrz2|xbrz3|xbrz4 fallback for textures without a
/// replacement. Returns false when off/not applicable. Results are cached in
/// memory per id (bounded budget).
bool psp_texpack_upscale(uint64_t id, const uint8_t* rgba, int w, int h,
                         std::vector<uint8_t>& out, int& ow, int& oh);

/// PSPRECOMP_TEX_REPLACE_FILTER: 0 = keep the game's filter, 1 = linear, 2 = nearest.
int psp_texpack_replace_filter();

/// Write textures.txt now (also called periodically and at shutdown).
void psp_texpack_flush();

// Exposed for tests / tooling.
bool psp_png_encode_rgba(const char* path, const uint8_t* rgba, int w, int h);
bool psp_png_decode_rgba(const uint8_t* file, size_t len, std::vector<uint8_t>& rgba, int& w, int& h,
                         std::string* err);
void psp_texpack_scale_rgba(const uint8_t* src, int w, int h, int factor, std::vector<uint8_t>& out);
