#pragma once
#include "psp_ge.h"
#include <glad/glad.h>
#include <cstdint>
#include <array>

/// Single entry in the texture cache.
struct TexCacheEntry {
    GLuint gl_tex = 0;
    uint32_t psp_addr = 0;
    uint32_t content_hash = 0;
    uint32_t clut_hash = 0;     // palette hash for CLUT4/CLUT8, else 0
    uint32_t clut_format = 0;   // CLUTFORMAT word for CLUT4/CLUT8, else 0
    uint32_t size_reg = 0;      // TEXSIZE0 (log2 w | log2 h << 8)
    uint32_t bufw = 0;          // TEXBUFWIDTH0 row stride (texels)
    uint32_t swizzle = 0;       // TEXMODE bit 0
    uint32_t wrap = 0xFFFFFFFFu;    // currently applied GL wrap (S|T<<1), ~0 = none
    uint32_t filter = 0xFFFFFFFFu;  // currently applied GL filter key, ~0 = none
    int width = 0;
    int height = 0;
    int format = 0;
    uint32_t last_frame = 0;
    uint32_t digest = 0;        // key_digest of the key fields (fast pre-compare)
    uint32_t movie_serial = 0;  // sceMpeg frame serial last uploaded (movie fast path)
    bool valid = false;
    bool mips = false;      // texture-pack replacement uploaded with generated mip levels
    bool replaced = false;  // texture-pack replacement / upscaled image (not the game's own texels)
    uint64_t pack_id = 0;   // texture-pack id (psp_tex_pack.h); 0 = not computed / not a game texture
};

/// Fixed-capacity LRU texture cache. Stores GL texture objects
/// keyed by (psp_addr, content hash, size, bufw, format, swizzle, CLUT hash). When full, evicts the
/// least-recently-used entry.
static constexpr int TEX_CACHE_SIZE = 256;

/// Initialize the texture cache (call after GL context is ready).
void ge_texture_init();

/// Shutdown the texture cache and release all GL textures.
void ge_texture_shutdown();

/// Look up or create a GL texture from current GE texture state.
/// Binds the result to GL_TEXTURE_2D. Returns the GL texture ID
/// (0 if texture is disabled or decode fails).
GLuint ge_texture_bind(
    uint8_t* rdram,
    uint32_t frame_num
);

/// Bind an externally owned GL texture (a render-target colour texture) as
/// the current texture: clamp-to-edge wrap, filter from GE TEXFILTER. Does not
/// touch the texture cache.
GLuint ge_texture_bind_external(GLuint gl_tex, const GeState& state);

/// Invalidate the entire texture cache (debug/reset).
void ge_texture_invalidate_all();

/// Forget the cached "currently bound texture" (call after any other code
/// bound a GL_TEXTURE_2D behind this module's back, e.g. render-target setup).
void ge_texture_invalidate_bind_cache();

/// Texture-pack id (psp_tex_pack.h) of the texture the last ge_texture_bind /
/// ge_texture_bind_external bound; 0 when none, a render target, a movie, or
/// when neither the texture pack nor the FX shaders (psp_fx_shader.h) are on.
uint64_t ge_texture_bound_pack_id();
