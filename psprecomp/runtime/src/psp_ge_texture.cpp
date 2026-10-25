#include <cstdlib>
#include <cstring>
#include "psp_ge_texture.h"
#include "hle/psp_hle_mpeg.h"
#include "psp_ge.h"
#include "psp_prof.h"
#include "psp_ge_constants.h"
#include "psp_tex_pack.h"
#include "psp_fx_shader.h"
#include "psp_memory.h"
#include "recomp.h"

#include <glad/glad.h>
#include <array>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---- Module state ----
static std::array<TexCacheEntry, TEX_CACHE_SIZE> g_tex_cache;
static bool g_tex_initialized = false;

// ---- FNV-1a hash ----

static uint32_t fnv1a_hash(
    const uint8_t* data, size_t len
) {
    // Name kept for history; this is a 64-bit word-at-a-time multiply/xor
    // mix (~10x faster than byte-wise FNV over the 4 KiB texture window the
    // cache hashes per bind). The value is only used inside the cache key.
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)len;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        std::memcpy(&w, data + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    if (i < len) {
        uint64_t w = 0;
        std::memcpy(&w, data + i, len - i);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    h *= 0xC4CEB9FE1A85EC53ull;
    return static_cast<uint32_t>(h ^ (h >> 32));
}

// Texture binding state cache (unit 0 only). Everything that binds a
// GL_TEXTURE_2D in this module goes through bind_tex2d.
static GLuint g_bound_tex = 0;
static inline void bind_tex2d(GLuint t) {
    if (g_bound_tex != t) {
        glBindTexture(GL_TEXTURE_2D, t);
        g_bound_tex = t;
    }
}
static struct { GLuint tex; uint32_t key; } applied[8] = {};
void ge_texture_invalidate_bind_cache() {
    g_bound_tex = ~0u;
    for (auto& a : applied) a = {0, 0};
}

// ---- Unswizzle ----

/// Unswizzle PSP tiled texture to linear layout.
/// PSP tiles are 16 bytes wide by 8 rows tall.
static void unswizzle_texture(
    const uint8_t* src,
    uint8_t* dst,
    int width_bytes,
    int height
) {
    int bxc = width_bytes / 16;
    int byc = height / 8;

    if (bxc <= 0) bxc = 1;
    if (byc <= 0) byc = 1;

    uint32_t src_off = 0;
    for (int by = 0; by < byc; by++) {
        for (int bx = 0; bx < bxc; bx++) {
            for (int n = 0; n < 8; n++) {
                int dst_off = (by * 8 + n) * width_bytes
                              + bx * 16;
                if (dst_off + 16 <= width_bytes * height
                    && src_off + 16
                       <= static_cast<uint32_t>(
                              width_bytes * height)) {
                    std::memcpy(
                        dst + dst_off, src + src_off, 16);
                }
                src_off += 16;
            }
        }
    }
}

// ---- Format conversion helpers ----

static void convert_5650_to_rgba(
    const uint8_t* src, uint8_t* dst,
    int width, int height
) {
    int pixel_count = width * height;
    const uint16_t* src16 =
        reinterpret_cast<const uint16_t*>(src);
    for (int i = 0; i < pixel_count; i++) {
        uint16_t val = src16[i];
        dst[i * 4 + 0] = static_cast<uint8_t>(
            ((val & 0x1F) * 255) / 31);
        dst[i * 4 + 1] = static_cast<uint8_t>(
            (((val >> 5) & 0x3F) * 255) / 63);
        dst[i * 4 + 2] = static_cast<uint8_t>(
            (((val >> 11) & 0x1F) * 255) / 31);
        dst[i * 4 + 3] = 255;
    }
}

static void convert_5551_to_rgba(
    const uint8_t* src, uint8_t* dst,
    int width, int height
) {
    int pixel_count = width * height;
    const uint16_t* src16 =
        reinterpret_cast<const uint16_t*>(src);
    for (int i = 0; i < pixel_count; i++) {
        uint16_t val = src16[i];
        dst[i * 4 + 0] = static_cast<uint8_t>(
            ((val & 0x1F) * 255) / 31);
        dst[i * 4 + 1] = static_cast<uint8_t>(
            (((val >> 5) & 0x1F) * 255) / 31);
        dst[i * 4 + 2] = static_cast<uint8_t>(
            (((val >> 10) & 0x1F) * 255) / 31);
        dst[i * 4 + 3] = (val & 0x8000) ? 255 : 0;
    }
}

static void convert_4444_to_rgba(
    const uint8_t* src, uint8_t* dst,
    int width, int height
) {
    int pixel_count = width * height;
    const uint16_t* src16 =
        reinterpret_cast<const uint16_t*>(src);
    for (int i = 0; i < pixel_count; i++) {
        uint16_t val = src16[i];
        dst[i * 4 + 0] = static_cast<uint8_t>(
            ((val & 0xF) * 255) / 15);
        dst[i * 4 + 1] = static_cast<uint8_t>(
            (((val >> 4) & 0xF) * 255) / 15);
        dst[i * 4 + 2] = static_cast<uint8_t>(
            (((val >> 8) & 0xF) * 255) / 15);
        dst[i * 4 + 3] = static_cast<uint8_t>(
            (((val >> 12) & 0xF) * 255) / 15);
    }
}

// ---- CLUT decode ----

/// Guest address of the CLUT. PPSSPP semantics: CLUTADDR holds the low 24
/// bits; CLUTADDRUPPER's data word is shifted left 8 to form bits 24-27.
static uint32_t clut_address(const GeState& state) {
    uint32_t a = (state.clut_addr & 0xFFFFFF)
        | ((state.clut_addr_upper << 8) & 0x0F000000);
    return a & PSP_ADDR_MASK;
}

/// Hash the palette bytes a CLUT4 (16 entries) / CLUT8 (256 entries) texture
/// can reference, entry size taken from the CLUT format. Bounded: <= 1 KiB.
static inline int clut_start_index(const GeState& state) {
    return ((state.clut_format >> 16) & 0x1F) * 16;   // CLUTFORMAT bits 16-20: entry offset / 16
}

static uint32_t hash_clut(const uint8_t* rdram, const GeState& state, int fmt) {
    // CLUT4 indexes 16 entries, everything else up to 256, from entry `start`
    // (CLUTFORMAT bits 16-20 * 16); CLUT16/CLUT32 index values are masked to
    // the same 0..255 window by CLUTFORMAT's mask in practice.
    int entries = (fmt == GE_TFMT_CLUT4 ? 16 : 256) + clut_start_index(state);
    int entry_bytes = ((state.clut_format & 0x3) == GE_CMODE_32BIT_ABGR8888) ? 4 : 2;
    uint32_t addr = clut_address(state);
    size_t bytes = (size_t)entries * entry_bytes;
    if (addr + bytes > PSP_MEM_SIZE) return 0;
    return fnv1a_hash(rdram + addr, bytes);
}

/// Read CLUT palette into RGBA8888 buffer (up to 256 entries).
static void read_clut_palette(
    uint8_t* rdram,
    const GeState& state,
    uint8_t clut_rgba[512 * 4]
) {
    // PPSSPP semantics: CLUTADDR holds the low 24 bits;
    // CLUTADDRUPPER's data word is shifted left 8 to form
    // bits 24-27 (it arrives as e.g. 0x040000 -> 0x04000000).
    uint32_t clut_addr = clut_address(state);

    int clut_fmt = state.clut_format & 0x3;
    int clut_shift = (state.clut_format >> 2) & 0x1F;
    int clut_mask = (state.clut_format >> 8) & 0xFF;
    int clut_start = ((state.clut_format >> 16) & 0x1F)
                     * 16;
    (void)clut_shift; (void)clut_mask; (void)clut_start;   // applied per texel in the decoders

    for (int i = 0; i < 512; i++) {
        if (clut_addr + (uint32_t)(i + 1) * (clut_fmt == GE_CMODE_32BIT_ABGR8888 ? 4 : 2) > PSP_MEM_SIZE) {
            clut_rgba[i * 4] = clut_rgba[i * 4 + 1] = clut_rgba[i * 4 + 2] = clut_rgba[i * 4 + 3] = 0;
            continue;
        }
        if (clut_fmt == GE_CMODE_32BIT_ABGR8888) {
            uint32_t val = psp_mem_read<uint32_t>(
                rdram, clut_addr + i * 4);
            clut_rgba[i * 4 + 0] = val & 0xFF;
            clut_rgba[i * 4 + 1] = (val >> 8) & 0xFF;
            clut_rgba[i * 4 + 2] = (val >> 16) & 0xFF;
            clut_rgba[i * 4 + 3] = (val >> 24) & 0xFF;
        } else if (clut_fmt == GE_CMODE_16BIT_BGR5650) {
            uint16_t val = psp_mem_read<uint16_t>(
                rdram, clut_addr + i * 2);
            clut_rgba[i * 4 + 0] = static_cast<uint8_t>(
                ((val & 0x1F) * 255) / 31);
            clut_rgba[i * 4 + 1] = static_cast<uint8_t>(
                (((val >> 5) & 0x3F) * 255) / 63);
            clut_rgba[i * 4 + 2] = static_cast<uint8_t>(
                (((val >> 11) & 0x1F) * 255) / 31);
            clut_rgba[i * 4 + 3] = 255;
        } else if (clut_fmt == GE_CMODE_16BIT_ABGR5551) {
            uint16_t val = psp_mem_read<uint16_t>(
                rdram, clut_addr + i * 2);
            clut_rgba[i * 4 + 0] = static_cast<uint8_t>(
                ((val & 0x1F) * 255) / 31);
            clut_rgba[i * 4 + 1] = static_cast<uint8_t>(
                (((val >> 5) & 0x1F) * 255) / 31);
            clut_rgba[i * 4 + 2] = static_cast<uint8_t>(
                (((val >> 10) & 0x1F) * 255) / 31);
            clut_rgba[i * 4 + 3] = (val & 0x8000)
                                    ? 255 : 0;
        } else if (clut_fmt == GE_CMODE_16BIT_ABGR4444) {
            uint16_t val = psp_mem_read<uint16_t>(
                rdram, clut_addr + i * 2);
            clut_rgba[i * 4 + 0] = static_cast<uint8_t>(
                ((val & 0xF) * 255) / 15);
            clut_rgba[i * 4 + 1] = static_cast<uint8_t>(
                (((val >> 4) & 0xF) * 255) / 15);
            clut_rgba[i * 4 + 2] = static_cast<uint8_t>(
                (((val >> 8) & 0xF) * 255) / 15);
            clut_rgba[i * 4 + 3] = static_cast<uint8_t>(
                (((val >> 12) & 0xF) * 255) / 15);
        }
    }
}

/// CLUT index of a raw texel (PPSSPP GetClutIndex): ((raw >> CLUTFORMAT.shift)
/// & CLUTFORMAT.mask) | (CLUTFORMAT.start * 16). shift = bits 2-6, mask = bits
/// 8-15, start = bits 16-20 of GE_CMD_CLUTFORMAT.
struct ClutIdx {
    uint32_t shift, mask, start;
    explicit ClutIdx(const GeState& st)
        : shift((st.clut_format >> 2) & 0x1F), mask((st.clut_format >> 8) & 0xFF),
          start((uint32_t)clut_start_index(st)) {}
    inline uint32_t operator()(uint32_t raw) const { return (((raw >> shift) & mask) | start) & 0x1FF; }
};

/// Decode T8/T4/T16/T32 (palettised) texture through the CLUT index function.
static void decode_clut(
    const uint8_t* src, uint8_t* dst, int width, int height, int bits,
    const uint8_t clut_rgba[512 * 4], const ClutIdx& ci
) {
    const int pixel_count = width * height;
    for (int i = 0; i < pixel_count; i++) {
        uint32_t raw;
        if (bits == 4) { uint8_t b = src[i / 2]; raw = (i & 1) ? (b >> 4) : (b & 0xF); }
        else if (bits == 8) raw = src[i];
        else if (bits == 16) raw = (uint32_t)src[i * 2] | ((uint32_t)src[i * 2 + 1] << 8);
        else raw = (uint32_t)src[i * 4] | ((uint32_t)src[i * 4 + 1] << 8) | ((uint32_t)src[i * 4 + 2] << 16) | ((uint32_t)src[i * 4 + 3] << 24);
        const uint32_t idx = ci(raw);
        dst[i * 4 + 0] = clut_rgba[idx * 4 + 0];
        dst[i * 4 + 1] = clut_rgba[idx * 4 + 1];
        dst[i * 4 + 2] = clut_rgba[idx * 4 + 2];
        dst[i * 4 + 3] = clut_rgba[idx * 4 + 3];
    }
}

// ---- Bytes per pixel for a texture format ----

static int tex_format_bpp(int fmt) {
    switch (fmt) {
    case GE_TFMT_5650:
    case GE_TFMT_5551:
    case GE_TFMT_4444:  return 16;
    case GE_TFMT_8888:  return 32;
    case GE_TFMT_CLUT4: return 4;
    case GE_TFMT_CLUT8: return 8;
    case GE_TFMT_CLUT16: return 16;
    case GE_TFMT_CLUT32: return 32;
    case GE_TFMT_DXT1:  return 4;
    case GE_TFMT_DXT3:
    case GE_TFMT_DXT5:  return 8;
    default:            return 32;
    }
}

// ---- Cache operations ----

/// Everything that selects a distinct decoded texture besides the guest
/// address and the (bounded) content hash.
struct TexKey {
    uint32_t psp_addr, content_hash, clut_hash, clut_format;
    uint32_t size_reg, bufw, format, swizzle;
};

static inline uint32_t key_digest(const TexKey& k) {
    uint32_t h = k.psp_addr * 0x9E3779B1u;
    h = (h ^ k.content_hash) * 0x85EBCA6Bu;
    h = (h ^ k.clut_hash) * 0xC2B2AE35u;
    h = (h ^ k.clut_format) * 0x9E3779B1u;
    h = (h ^ k.size_reg) * 0x85EBCA6Bu;
    h = (h ^ k.bufw) * 0xC2B2AE35u;
    h = (h ^ k.format ^ (k.swizzle << 8)) * 0x9E3779B1u;
    return h ^ (h >> 15);
}

static bool key_matches(const TexCacheEntry& e, const TexKey& k) {
    return e.valid && e.psp_addr == k.psp_addr
        && e.content_hash == k.content_hash
        && e.clut_hash == k.clut_hash && e.clut_format == k.clut_format
        && e.size_reg == k.size_reg && e.bufw == k.bufw
        && (uint32_t)e.format == k.format && e.swizzle == k.swizzle;
}

/// Find the entry matching the key, else a free slot, else evict LRU.
static int find_or_evict(const TexKey& key) {
    int best_slot = 0;
    int free_slot = -1;
    uint32_t oldest_frame = UINT32_MAX;

    // Fast pass: compare the 32-bit digest first (one load per entry).
    const uint32_t dg = key_digest(key);
    for (int i = 0; i < TEX_CACHE_SIZE; i++) {
        const auto& e = g_tex_cache[i];
        if (e.digest == dg && key_matches(e, key)) return i;  // Exact match
    }
    for (int i = 0; i < TEX_CACHE_SIZE; i++) {
        auto& e = g_tex_cache[i];
        if (!e.valid) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (e.last_frame < oldest_frame) {
            oldest_frame = e.last_frame;
            best_slot = i;
        }
    }
    if (free_slot >= 0) return free_slot;

    // Evict oldest
    auto& evict = g_tex_cache[best_slot];
    if (evict.gl_tex != 0) {
        if (g_bound_tex == evict.gl_tex) g_bound_tex = ~0u;
        glDeleteTextures(1, &evict.gl_tex);
        evict.gl_tex = 0;
    }
    evict.valid = false;
    evict.digest = 0;
    return best_slot;
}

/// Apply TEXWRAP / TEXFILTER to the currently bound texture (level 0 only is
/// uploaded, so mipmap min modes map to their base-level filter). Cached per
/// entry so repeat draws cost nothing.
///   TEXWRAP:   bit0 = U clamp(1)/repeat(0), bit8 = V clamp/repeat
///   TEXFILTER: bits0-2 min (0 NEAREST, 1 LINEAR, 4/6 NEAREST_MIPMAP_*,
///              5/7 LINEAR_MIPMAP_*), bit8 mag (0 NEAREST, 1 LINEAR)
static void apply_tex_params(TexCacheEntry& e, const GeState& state) {
    uint32_t clamp_u = state.tex_wrap & 1;
    uint32_t clamp_v = (state.tex_wrap >> 8) & 1;
    uint32_t wrap_key = clamp_u | (clamp_v << 1);
    if (e.wrap != wrap_key) {
        e.wrap = wrap_key;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    uint32_t min_lin = state.tex_filter & 1;          // 1,5,7 -> linear base
    uint32_t mag_lin = (state.tex_filter >> 8) & 1;
    GLenum min_gl = min_lin ? GL_LINEAR : GL_NEAREST;
    GLenum mag_gl = mag_lin ? GL_LINEAR : GL_NEAREST;
    if (e.mips && (state.tex_filter & 4)) {
        // Texture-pack replacement with generated mip levels, and the game
        // asked for mipmapped minification: 4 NN, 5 LN, 6 NL, 7 LL.
        static const GLenum mm[4] = {GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
                                     GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
        min_gl = mm[state.tex_filter & 3];
    }
    // PSPRECOMP_TEX_FILTER=nearest|linear: force the sampling filter of every game texture (not of
    // render-target reads, see ge_texture_bind_external). nearest = the PSP's blocky "retro" look
    // (run.sh --retro); unset or "game" keeps whatever the game asks for.
    static const int force_filter = [] {
        const char* v = std::getenv("PSPRECOMP_TEX_FILTER");
        if (v && !std::strcmp(v, "nearest")) return 2;
        if (v && !std::strcmp(v, "linear")) return 1;
        return 0;
    }();
    if (force_filter == 2) {
        min_gl = GL_NEAREST; mag_gl = GL_NEAREST;
    } else if (force_filter == 1) {
        min_gl = (e.mips && (state.tex_filter & 4)) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
        mag_gl = GL_LINEAR;
    }
    if (e.replaced) {
        const int rf = psp_texpack_replace_filter();   // 1 = force linear, 2 = force nearest
        if (rf == 1) {
            min_gl = e.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
            mag_gl = GL_LINEAR;
        } else if (rf == 2) {
            min_gl = GL_NEAREST; mag_gl = GL_NEAREST;
        }
    }
    uint32_t filter_key = ((uint32_t)min_gl << 16) | (uint32_t)mag_gl;
    if (e.filter != filter_key) {
        e.filter = filter_key;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_gl);
    }
}

// ---- Public API ----

void ge_texture_init() {
    for (auto& e : g_tex_cache) {
        e = TexCacheEntry{};
    }
    g_tex_initialized = true;
    std::fprintf(stderr, "[TEX] Texture cache initialized "
                         "(%d entries)\n", TEX_CACHE_SIZE);
}

void ge_texture_shutdown() {
    psp_texpack_flush();
    g_bound_tex = ~0u;
    for (auto& e : g_tex_cache) {
        if (e.gl_tex != 0) {
            glDeleteTextures(1, &e.gl_tex);
            e.gl_tex = 0;
        }
        e.valid = false;
    }
    g_tex_initialized = false;
    std::fprintf(stderr, "[TEX] Texture cache shutdown\n");
}

void ge_texture_invalidate_all() {
    g_bound_tex = ~0u;
    for (auto& e : g_tex_cache) {
        if (e.gl_tex != 0) {
            glDeleteTextures(1, &e.gl_tex);
            e.gl_tex = 0;
        }
        e.valid = false;
    }
}

static uint64_t g_bound_pack_id = 0;
uint64_t ge_texture_bound_pack_id() { return g_bound_pack_id; }

GLuint ge_texture_bind_external(GLuint gl_tex, const GeState& state) {
    g_bound_pack_id = 0;
    bind_tex2d(gl_tex);
    // Render targets are always clamped (REPEAT would bleed the 480x272
    // image edge into the unused part of the declared 512x512 texture).
    // Parameters are remembered per texture so repeat binds cost nothing.
    uint32_t min_lin = state.tex_filter & 1;
    uint32_t mag_lin = (state.tex_filter >> 8) & 1;
    const uint32_t fkey = 0x100u | min_lin | (mag_lin << 1);
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (applied[i].tex == gl_tex) { slot = i; break; }
    }
    if (slot < 0) {
        static int next = 0;
        slot = next++ & 7;
        applied[slot] = {gl_tex, 0};
    }
    if (applied[slot].key != fkey) {
        applied[slot].key = fkey;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        min_lin ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        mag_lin ? GL_LINEAR : GL_NEAREST);
    }
    return gl_tex;
}

/// Upload the first `rows` rows of a bufw-wide RGBA8888 guest buffer into the
/// entry's texture (already bound), allocating the width x height storage the
/// first time. See the movie fast path in ge_texture_bind.
static void upload_movie_rows(TexCacheEntry& e, const uint8_t* rdram, uint32_t masked_addr,
                              int width, int height, int bufw, int rows, uint32_t serial) {
    const uint64_t t0 = psp_prof::enabled ? psp_prof::now() : 0;
    if (e.width != width || e.height != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        e.width = width;
        e.height = height;
    }
    rows = std::max(0, std::min(rows, height));
    if (rows > 0 && (uint64_t)masked_addr + (uint64_t)rows * bufw * 4 <= PSP_MEM_SIZE) {
        GLint prev_row = 0, prev_align = 4;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, bufw);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, rows, GL_RGBA, GL_UNSIGNED_BYTE,
                        rdram + masked_addr);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row);
        glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    }
    e.movie_serial = serial;
    psp_prof::tex_bytes += (uint64_t)width * rows * 4;
    if (psp_prof::enabled) { psp_prof::movie_uploads++; psp_prof::movie_ns += psp_prof::now() - t0; }
}

GLuint ge_texture_bind(
    uint8_t* rdram,
    uint32_t frame_num
) {
    const GeState& state = ge_get_state();

    g_bound_pack_id = 0;
    if (!state.texture_enable) {
        bind_tex2d(0);
        return 0;
    }

    // Extract texture parameters
    int log2_w = state.tex_size[0] & 0xFF;
    int log2_h = (state.tex_size[0] >> 8) & 0xFF;
    int width  = 1 << log2_w;
    int height = 1 << log2_h;
    int fmt    = state.tex_format;

    // Compute PSP address
    uint32_t psp_addr = state.tex_addr[0];
    // Combine with TEXBUFWIDTH upper bits for high address
    uint32_t bufw_upper =
        (state.tex_bufw[0] >> 16) & 0xFF;
    psp_addr |= (bufw_upper << 24);
    uint32_t masked_addr = psp_addr & PSP_ADDR_MASK;

    // Compute data size and hash. TEXBUFWIDTH is the row stride in texels
    // and may differ from the power-of-two texture width (fonts and render
    // targets use e.g. 384-wide rows in a 512-wide texture); the source data
    // is bufw texels per row, height rows.
    int bpp = tex_format_bpp(fmt);
    int bufw = state.tex_bufw[0] & 0x7FF;
    if (bufw == 0) bufw = width;
    int stride_bytes = (bufw * bpp + 7) / 8;
    int data_bytes = stride_bytes * height;
    // Content hash window. A cache key that sees only the first 4 KiB misses
    // a game rewriting the rest of a texture in place (dynamic text / glow
    // sprites) and keeps serving the stale decode. Small textures are hashed
    // whole; big ones get the 4 KiB head plus strided windows through the body.
    // PSPRECOMP_TEX_HASH=head restores the old head-only key, =full hashes everything.
    static const int hash_mode = [] {
        const char* e = std::getenv("PSPRECOMP_TEX_HASH");
        return !e ? 1 : (e[0] == 'h' ? 0 : (e[0] == 'f' ? 2 : 1));
    }();
    int hash_bytes = hash_mode == 2 ? data_bytes : (hash_mode == 0 ? std::min(data_bytes, 4096) : std::min(data_bytes, 16384));

    if (masked_addr + hash_bytes
        > static_cast<int>(PSP_MEM_SIZE)) {
        bind_tex2d(0);
        return 0;
    }

    // Movie frame fast path. sceMpeg writes each decoded frame to a known
    // guest buffer and bumps a serial per write, so the texture never needs
    // hashing (the 4 KiB window covers only the top two rows of a 512-px RGBA
    // frame, which can stay identical across frames), and an update uploads
    // only the decoded rows straight from guest memory into the existing GL
    // texture (no raw/unswizzle/pack/RGBA staging copies, no reallocation).
    uint32_t mv_serial = 0;
    int mv_rows = 0;
    // PSPRECOMP_MOVIE_TEXFAST=0 disables the fast path (diagnostic).
    static const bool movie_fast = !(std::getenv("PSPRECOMP_MOVIE_TEXFAST") &&
                                     std::getenv("PSPRECOMP_MOVIE_TEXFAST")[0] == '0');
    const bool movie = movie_fast && fmt == GE_TFMT_8888 && !(state.tex_mode & 1) && bufw == width &&
                       psp_mpeg_frame_info(masked_addr, 3, &mv_serial, &mv_rows);
    if (movie) mv_rows = std::min(mv_rows, height);
    {
        static const bool dbg = std::getenv("PSPRECOMP_MOVIE_TEXDBG") != nullptr;
        uint32_t s2 = 0; int r2 = 0;
        if (dbg && fmt == GE_TFMT_8888 && width == 512 && height == 512) {
            bool reg = psp_mpeg_frame_info(masked_addr, 3, &s2, &r2);
            uint32_t hh = 2166136261u;
            for (int y = 0; y < 272; y += 3) for (int x = 0; x < 2048; x += 61) hh = (hh ^ rdram[masked_addr + y * 2048 + x]) * 16777619u;
            std::fprintf(stderr, "[MVBIND] addr=%08X reg=%d serial=%u rows=%d fast=%d hash=%08X\n", masked_addr, (int)reg, s2, r2, (int)movie, hh);
        }
    }
    uint32_t content_hash;
    if (movie) {
        content_hash = 0x4D56'4946u;
    } else {
        content_hash = fnv1a_hash(rdram + masked_addr, hash_bytes);
        if (hash_mode == 1 && data_bytes > hash_bytes && (uint64_t)masked_addr + data_bytes <= PSP_MEM_SIZE) {
            // 48 strided 64-byte windows over the remainder of the texture.
            const int rest = data_bytes - hash_bytes;
            const int step = std::max(64, rest / 48);
            for (int off = hash_bytes; off + 64 <= data_bytes; off += step)
                content_hash = content_hash * 0x9E3779B1u ^ fnv1a_hash(rdram + masked_addr + off, 64);
        }
    }

    // Cache lookup. The key also covers texture size/format/stride/swizzle and,
    // for palettised formats, the palette the texture was decoded with.
    TexKey key{};
    key.psp_addr = psp_addr;
    key.content_hash = content_hash;
    key.size_reg = state.tex_size[0] & 0xFFFF;
    key.bufw = state.tex_bufw[0] & 0x7FF;
    key.format = (uint32_t)fmt;
    key.swizzle = state.tex_mode & 1;
    if (fmt == GE_TFMT_CLUT4 || fmt == GE_TFMT_CLUT8 || fmt == GE_TFMT_CLUT16 || fmt == GE_TFMT_CLUT32) {
        key.clut_hash = hash_clut(rdram, state, fmt);
        key.clut_format = state.clut_format;
    }
    psp_prof::tex_binds++;
    int slot = find_or_evict(key);
    auto& entry = g_tex_cache[slot];

    if (movie && psp_prof::enabled) psp_prof::movie_binds++;
    if (key_matches(entry, key)) {
        // Cache hit
        entry.last_frame = frame_num;
        g_bound_pack_id = entry.pack_id;
        bind_tex2d(entry.gl_tex);
        if (movie && entry.movie_serial != mv_serial) {
            upload_movie_rows(entry, rdram, masked_addr, width, height, bufw, mv_rows, mv_serial);
        }
        apply_tex_params(entry, state);
        return entry.gl_tex;
    }

    if (movie) {
        // First sight of this movie buffer (or its slot was evicted).
        if (entry.gl_tex == 0) glGenTextures(1, &entry.gl_tex);
        bind_tex2d(entry.gl_tex);
        entry.width = 0; entry.height = 0;   // force storage allocation
        upload_movie_rows(entry, rdram, masked_addr, width, height, bufw, mv_rows, mv_serial);
        entry.psp_addr = psp_addr;
        entry.content_hash = content_hash;
        entry.clut_hash = key.clut_hash;
        entry.clut_format = key.clut_format;
        entry.size_reg = key.size_reg;
        entry.bufw = key.bufw;
        entry.swizzle = key.swizzle;
        entry.wrap = 0xFFFFFFFFu;
        entry.filter = 0xFFFFFFFFu;
        entry.mips = false;
        entry.replaced = false;
        entry.pack_id = 0;
        entry.format = fmt;
        entry.last_frame = frame_num;
        entry.valid = true;
        entry.digest = key_digest(key);
        apply_tex_params(entry, state);
        return entry.gl_tex;
    }

    // Cache miss -- decode texture
    psp_prof::tex_miss++;
    psp_prof::tex_bytes += (uint64_t)width * height * 4;
    bool swizzle = (state.tex_mode & 1) != 0;
    int width_bytes = (width * bpp + 7) / 8;

    // Read raw texture data from rdram
    std::vector<uint8_t> raw_data(data_bytes);
    if (masked_addr + data_bytes
        <= static_cast<int>(PSP_MEM_SIZE)) {
        std::memcpy(
            raw_data.data(),
            rdram + masked_addr,
            data_bytes);
    }

    // Unswizzle if needed
    std::vector<uint8_t> linear_data;
    const uint8_t* src_data = raw_data.data();
    if (swizzle && stride_bytes >= 16 && height >= 8) {
        linear_data.resize(data_bytes);
        unswizzle_texture(
            raw_data.data(), linear_data.data(),
            stride_bytes, height);
        src_data = linear_data.data();
    }

    // Repack rows from the bufw stride to the texture width the converters
    // expect; texels past the stride (bufw < width) are undefined on the PSP,
    // left zero here.
    std::vector<uint8_t> packed_data;
    if (stride_bytes != width_bytes) {
        packed_data.assign((size_t)width_bytes * height, 0);
        int copy = std::min(stride_bytes, width_bytes);
        for (int y = 0; y < height; y++)
            std::memcpy(packed_data.data() + (size_t)y * width_bytes,
                        src_data + (size_t)y * stride_bytes, copy);
        src_data = packed_data.data();
    }

    // Texture pack: stable id over the decoded source (only when dump /
    // replacement / upscale is on; fmt <= CLUT8 are the formats decoded here).
    const bool is_pal = (fmt == GE_TFMT_CLUT4 || fmt == GE_TFMT_CLUT8 || fmt == GE_TFMT_CLUT16 || fmt == GE_TFMT_CLUT32);
    uint8_t clut_rgba[512 * 4];
    if (is_pal) read_clut_palette(rdram, state, clut_rgba);
    const bool pack_on = fmt <= GE_TFMT_CLUT8 && psp_texpack_active();
    // The FX shader overrides match draws by the same id (psp_fx_shader.h).
    const bool id_on = pack_on || (fmt <= GE_TFMT_CLUT8 && psp_fx_want_ids());
    uint64_t tid = 0;
    std::vector<uint8_t> pack_px;      // replacement / upscaled pixels
    int pack_w = 0, pack_h = 0;
    bool replaced = false;
    if (id_on)
        tid = psp_texpack_make_id(src_data, (size_t)width_bytes * height, fmt, width, height,
                                  fmt == GE_TFMT_CLUT4 ? 4 : fmt == GE_TFMT_CLUT8 ? 8 : 0, clut_rgba);
    if (pack_on)
        replaced = psp_texpack_load_replacement(tid, pack_px, pack_w, pack_h);

    // Convert to RGBA8888
    std::vector<uint8_t> rgba;
    if (!replaced) {
    rgba.resize((size_t)width * height * 4);

    switch (fmt) {
    case GE_TFMT_8888:
        std::memcpy(rgba.data(), src_data,
                    width * height * 4);
        break;
    case GE_TFMT_5650:
        convert_5650_to_rgba(
            src_data, rgba.data(), width, height);
        break;
    case GE_TFMT_5551:
        convert_5551_to_rgba(
            src_data, rgba.data(), width, height);
        break;
    case GE_TFMT_4444:
        convert_4444_to_rgba(
            src_data, rgba.data(), width, height);
        break;
    case GE_TFMT_CLUT4: case GE_TFMT_CLUT8: case GE_TFMT_CLUT16: case GE_TFMT_CLUT32:
        decode_clut(src_data, rgba.data(), width, height,
                    fmt == GE_TFMT_CLUT4 ? 4 : fmt == GE_TFMT_CLUT8 ? 8 : fmt == GE_TFMT_CLUT16 ? 16 : 32,
                    clut_rgba, ClutIdx(state));
        break;
    default:
        // Unsupported format: magenta fill
        for (int i = 0; i < width * height; i++) {
            rgba[i * 4 + 0] = 0xFF;
            rgba[i * 4 + 1] = 0x00;
            rgba[i * 4 + 2] = 0xFF;
            rgba[i * 4 + 3] = 0xFF;
        }
        break;
    }
    if (pack_on) {
        PspTexMeta meta{width, height, fmt, is_pal ? (int)(state.clut_format & 3) : -1, psp_addr};
        psp_texpack_note(tid, meta, rgba.data());
        std::vector<uint8_t> up;
        int uw = 0, uh = 0;
        if (psp_texpack_upscale(tid, rgba.data(), width, height, up, uw, uh)) {
            pack_px.swap(up); pack_w = uw; pack_h = uh;
        }
    }
    }   // !replaced

    // PSPRECOMP_TEX_WHITE=1: log every decoded texture that is (almost) uniformly
    // opaque white - the signature of a mis-decoded CLUT / format.
    {
        static const bool tex_white = std::getenv("PSPRECOMP_TEX_WHITE") != nullptr;
        if (tex_white && !rgba.empty()) {
            long w = 0, n = (long)width * height;
            for (long i = 0; i < n; i++)
                if (rgba[i * 4] > 250 && rgba[i * 4 + 1] > 250 && rgba[i * 4 + 2] > 250 && rgba[i * 4 + 3] > 250) w++;
            if (w * 100 >= n * 85)
                std::fprintf(stderr, "[TEX_WHITE] fmt=%d %dx%d bufw=%d addr=0x%08X swz=%d clut=0x%08X clf=0x%X white=%ld/%ld func=%u\n",
                    fmt, width, height, bufw, psp_addr, (state.tex_mode & 1) ? 1 : 0, clut_address(state),
                    (unsigned)state.clut_format, w, n, (unsigned)state.tex_func);
        }
    }

    // PSPRECOMP_TEX_DUMP=<hex addr>[,<hex addr>...]: write each decode of those guest
    // addresses to $PSPRECOMP_TEX_DUMP_DIR (default /tmp)/tex_<addr>_<n>.tga.
    {
        static const char* dump_env = std::getenv("PSPRECOMP_TEX_DUMP");
        if (dump_env && !rgba.empty()) {
            char tok[16]; snprintf(tok, sizeof tok, "%X", psp_addr);
            std::string list = std::string(",") + dump_env + ",";
            if (list.find(std::string(",") + tok + ",") != std::string::npos) {
                static int dn = 0;
                const char* dir = std::getenv("PSPRECOMP_TEX_DUMP_DIR");
                char path[512]; snprintf(path, sizeof path, "%s/tex_%X_%d.tga", dir ? dir : "/tmp", psp_addr, dn++);
                if (FILE* f = fopen(path, "wb")) {
                    uint8_t h[18] = {0, 0, 2, 0,0,0,0,0, 0,0,0,0, (uint8_t)(width & 255), (uint8_t)(width >> 8),
                                     (uint8_t)(height & 255), (uint8_t)(height >> 8), 32, 8 | 0x20};
                    fwrite(h, 1, 18, f);
                    for (long i = 0; i < (long)width * height; i++) {
                        uint8_t px[4] = {rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4], rgba[i * 4 + 3]};
                        fwrite(px, 1, 4, f);
                    }
                    fclose(f);
                    std::fprintf(stderr, "[TEX_DUMP] %s fmt=%d %dx%d clut=0x%08X clf=0x%X\n", path, fmt, width, height,
                                 clut_address(state), (unsigned)state.clut_format);
                }
            }
        }
    }

    // PSPRECOMP_DRAW_TRACE: summarise the first decoded textures.
    static int tex_trace_n = 0;
    static const bool tex_trace = std::getenv("PSPRECOMP_DRAW_TRACE") != nullptr;
    if (tex_trace && tex_trace_n < 24 && !rgba.empty()) {
        tex_trace_n++;
        long nonblack = 0, opaque = 0, n = (long)width * height;
        for (long i = 0; i < n; i++) {
            if (rgba[i * 4] | rgba[i * 4 + 1] | rgba[i * 4 + 2]) nonblack++;
            if (rgba[i * 4 + 3] > 8) opaque++;
        }
        std::fprintf(stderr, "[TEX_TRACE] fmt=%d %dx%d addr=0x%08X swz=%d nonblack=%ld/%ld opaque=%ld px0=%02X%02X%02X%02X\n",
            (int)state.tex_format, width, height, psp_addr, (state.tex_mode & 1) ? 1 : 0,
            nonblack, n, opaque, rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    // Upload to GL
    if (entry.gl_tex == 0) {
        glGenTextures(1, &entry.gl_tex);
    }
    bind_tex2d(entry.gl_tex);
    const bool pack_px_used = !pack_px.empty();
    if (pack_px_used) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pack_w, pack_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pack_px.data());
        // Minified replacements alias without mips; generate them when the
        // image is more than 2x the original (the game picks whether they are
        // sampled, see apply_tex_params).
        entry.mips = pack_w > 2 * width || pack_h > 2 * height;
        if (entry.mips) glGenerateMipmap(GL_TEXTURE_2D);
    } else {
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8,
            width, height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE,
            rgba.data());
        entry.mips = false;
    }
    entry.replaced = pack_px_used;
    entry.pack_id = tid;
    g_bound_pack_id = tid;

    // Store entry
    entry.psp_addr = psp_addr;
    entry.content_hash = content_hash;
    entry.clut_hash = key.clut_hash;
    entry.clut_format = key.clut_format;
    entry.size_reg = key.size_reg;
    entry.bufw = key.bufw;
    entry.swizzle = key.swizzle;
    entry.wrap = 0xFFFFFFFFu;
    entry.filter = 0xFFFFFFFFu;
    entry.width = pack_px_used ? pack_w : width;
    entry.height = pack_px_used ? pack_h : height;
    entry.format = fmt;
    entry.last_frame = frame_num;
    entry.valid = true;
    entry.digest = key_digest(key);

    apply_tex_params(entry, state);
    return entry.gl_tex;
}
