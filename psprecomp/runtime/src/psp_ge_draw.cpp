#include "hle/psp_savedata_dialog.h"
#include "psp_ge_draw.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "psp_ge_vertex.h"
#include "psp_ge_texture.h"
#include "psp_ge_shader.h"
#include "psp_fx_shader.h"
#include "psp_fep_log.h"
#include "psp_event_loop.h"
#include "psp_prof.h"
#include "psp_memory.h"

#include <SDL.h>
#include <glad/glad.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <algorithm>
#include <vector>
#include <cmath>
#include <random>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
void gl_stats_frame();   // psp_gl_stats.cpp

// ---- Generic FBO pool (issue #59 — honor fb_addr / double buffer) ----
// One GL FBO per distinct guest VRAM framebuffer address. The GE binds the
// FBO for the current FRAMEBUFPTR; present blits the FBO matching the
// displayed (sceDisplaySetFrameBuf) address. Keying on the guest's own
// addresses keeps this generic: a single-buffered title keys everything to
// one slot and degenerates to the old single-surface behavior.
static constexpr int GE_MAX_TARGETS = 8;  // Dissidia story scenes cycle through 5+ keys; 4 thrashed (evicted RTT contents)

struct GeTarget {
    uint32_t key = 0;          // fb_addr normalized to eDRAM offset (0 = empty)
    GLuint fbo = 0;
    GLuint color_tex = 0;
    GLuint depth_rb = 0;
    GLuint ab_fbo = 0, ab_tex = 0, ab_depth = 0;   // PSPRECOMP_GE_AB twin (legacy-path shadow)
    uint64_t last_use = 0;     // LRU stamp (monotonic counter)
};

// Render scale (PSPRECOMP_RENDER_SCALE, 1..8 or auto): targets are 480*g_rs x 272*g_rs.
static int g_rs = 1;
static GeTarget g_targets[GE_MAX_TARGETS];
static int g_current_target = -1;   // slot the GE is rendering into
static int g_front_target = -1;     // slot last presented / most recent render
static uint64_t g_target_clock = 0; // monotonic LRU counter

// Normalize any guest framebuffer address to its eDRAM 2 MB offset key.
// Strips the cached/uncached/eDRAM base via the canonical 0x07FFFFFF PSP
// mask, then takes the 16-byte-aligned eDRAM offset (PPSSPP model). Used by
// both ge_draw_select_target (FRAMEBUFPTR) and ge_present_frame
// (sceDisplaySetFrameBuf) so the two address spaces reconcile.
static inline uint32_t ge_fb_key(uint32_t addr) {
    return (addr & 0x07FFFFFFu) & 0x001FFFF0u;
}

// Framebuffer-as-texture: if the current texture's address is a VRAM address
// whose ge_fb_key matches a live pool target, return that target's colour
// texture (and its pixel size). Returns 0 when the texture should come from
// memory (not VRAM, no such target, or the target is the one being rendered
// into -- a feedback loop, logged once).
static void flush_batch();
static GLuint g_fb_scratch_tex = 0, g_fb_scratch_fbo = 0;

// The GE reads a texture from the very buffer it is rendering into (in-place
// blur / shift passes: Dissidia's bloom strips draw 0xCC000 into 0xCC000). The
// hardware samples the pre-draw contents; GL forbids sampling an attachment
// (undefined feedback loop), so snapshot the bound render target into a scratch
// texture first. Pending batched draws are flushed so the snapshot is current,
// and every feedback draw takes its own snapshot (find is called once per PRIM).
// PSPRECOMP_GE_FEEDBACK=0 restores the old behaviour (memory path).
static GLuint feedback_snapshot(int* w, int* h) {
    static const bool off = std::getenv("PSPRECOMP_GE_FEEDBACK") && std::getenv("PSPRECOMP_GE_FEEDBACK")[0] == '0';
    if (off) return 0;
    flush_batch();
    const int W = 480 * g_rs, H = 272 * g_rs;
    if (!g_fb_scratch_tex) {
        glGenTextures(1, &g_fb_scratch_tex);
        glBindTexture(GL_TEXTURE_2D, g_fb_scratch_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &g_fb_scratch_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fb_scratch_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fb_scratch_tex, 0);
        ge_texture_invalidate_bind_cache();
    }
    GLint cur = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)cur);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fb_scratch_fbo);
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)cur);
    if (w) *w = W;
    if (h) *h = H;
    return g_fb_scratch_tex;
}

unsigned int ge_draw_find_target_texture(uint32_t key, int* w, int* h) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo == 0 || g_targets[i].key != key) continue;
        if (i == g_current_target) {
            // PSPRECOMP_NO_FBSNAP=1 (diagnostic A/B): skip the whole-target copy and use the memory path.
            static const bool no_snap = [] { const char* v = std::getenv("PSPRECOMP_NO_FBSNAP"); return v && *v && *v != '0'; }();
            if (!no_snap)
                if (const GLuint t = feedback_snapshot(w, h)) {
                    psp_prof::fb_snaps++;
                    if (w && h) psp_prof::fb_snap_px += uint64_t(*w) * uint64_t(*h);
                    return t;
                }
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr, "[DRAW] texture key=0x%06X is the bound "
                    "render target (feedback); using memory path\n", key);
            }
            return 0;
        }
        g_targets[i].last_use = ++g_target_clock;
        if (w) *w = 480 * g_rs;
        if (h) *h = 272 * g_rs;
        return g_targets[i].color_tex;
    }
    return 0;
}

// ---- Liveness present (no-flip safety net) ----
static bool g_present_dirty = false;
static std::chrono::steady_clock::time_point g_last_present;
static int g_present_stale_ms = 100;   // PSPRECOMP_PRESENT_STALE_MS

// Stencil emulation mode (debug socket `ST <bits>`, env PSPRECOMP_GE_STENCIL=<bits>):
// bit 0 = stencil test + ops on the GL stencil buffer, bit 1 = keep the colour alpha
// channel in step with REPLACE/ZERO ("stencil to alpha"). Default 1: bit 1 breaks the
// Story free-roam scene (terrain vanishes) and is still under investigation. 0 = off.
static std::atomic<int> g_stencil_mode{[] {
    const char* e = std::getenv("PSPRECOMP_GE_STENCIL");
    return e ? std::atoi(e) : 1;
}()};
void ge_draw_set_stencil_mode(int m) { g_stencil_mode.store(m); }
// Debug socket `D <dir>`: write every live render target (colour + alpha plane) at the next present.
static std::mutex g_dump_mu;
static std::string g_dump_dir;
static std::atomic<bool> g_dump_armed{false};
void ge_draw_dump_targets(const char* dir) {
    { std::lock_guard<std::mutex> lk(g_dump_mu); g_dump_dir = dir; }
    g_dump_armed.store(true);
}
static std::atomic<int> g_dyn_trace_n{0};
static std::atomic<int> g_xp_nofilter{0};  // PSPRECOMP_XP_FILE mode bit 32: lift the PSPRECOMP_DT_ONLY_VT filter
static std::atomic<int> g_dyn_armed{0};
// Arm a trace of the next n prims, starting at the next display list.
void ge_draw_dyn_trace(int n) { g_dyn_armed.store(n); }
void ge_draw_set_fast(bool on);
bool ge_draw_dyn_trace_active() { return g_dyn_trace_n.load() > 0; }

// ---- Module state ----
static GLuint g_vao = 0;
static GLuint g_vbo = 0;
static uint32_t g_frame_counter = 0;
static bool g_has_drawn_prims = false;
static bool g_screenshot_enabled = false;
static bool g_screenshot_done = false;
static char g_screenshot_path[256] = {};
static std::atomic<bool> g_draw_ready{false};
// PSPRECOMP_NO_RASTER=1: GL_RASTERIZER_DISCARD (draws submitted, nothing
// rasterised); =2: skip glDrawArrays entirely. Measures CPU-side cost only.
static int g_no_raster = 0;
static int g_no_raster_mode = 0;       // requested mode (PSPRECOMP_NO_RASTER)
static double g_no_raster_after = 0.0; // PSPRECOMP_NO_RASTER_AFTER=<s>: enable only after s seconds
static std::chrono::steady_clock::time_point g_t_start = std::chrono::steady_clock::now();

// ---- PRIM counters (file scope so ge_draw_get_stats can read them) ----
// Written only by the render thread inside ge_draw_prim; read racily by
// the debug socket's I command (a few prims stale is fine).
static int g_prim_type_count[8] = {0};   // PRIM types 0-6 + slot 7 safety
static int g_prim_type_count_clear[8] = {0};
static int g_prim_type_zero_count = 0;   // count <= 0 (degenerate)
static int g_prim_total_observed = 0;

// ---- On-demand screenshot request (issue #35 debug socket S command) ----
// Requester (any thread) posts a path + waits on the condvar; the render
// thread polls ge_draw_service_screenshot_request() each event-loop pass
// and performs the GL readback there (GL never leaves the main thread).
static std::mutex g_ss_req_mutex;
static std::condition_variable g_ss_req_cv;
static char g_ss_req_path[512] = {};
enum SsReqState { SS_IDLE = 0, SS_PENDING = 1, SS_DONE = 2, SS_FAILED = 3 };
static int g_ss_req_state = SS_IDLE;

// ---- Packed vertex for VBO upload ----
// Layout: 3 floats (pos) + 2 floats (uv) + 4 bytes (color)
// Total: 12 + 8 + 4 = 24 bytes
struct PackedVertex {
    float pos[4];
    float uv[2];
    uint8_t color[4];
};
static_assert(sizeof(PackedVertex) == 28,
              "PackedVertex must be 28 bytes");

// ---- Screenshot (TGA format -- no external dependency) ----

static bool write_tga(
    const char* path,
    const uint8_t* pixels,
    int width,
    int height
) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr,
            "[DRAW] Cannot open %s for screenshot\n",
            path);
        return false;
    }

    // TGA header (18 bytes) -- uncompressed RGBA
    uint8_t header[18] = {};
    header[2]  = 2;  // Uncompressed true-color
    header[12] = width & 0xFF;
    header[13] = (width >> 8) & 0xFF;
    header[14] = height & 0xFF;
    header[15] = (height >> 8) & 0xFF;
    header[16] = 32;  // bits per pixel
    header[17] = 0x28; // origin upper-left + 8 alpha bits

    std::fwrite(header, 1, 18, f);

    // Write pixels (BGRA for TGA)
    // Input is RGBA, need to swap R and B
    std::vector<uint8_t> bgra(width * height * 4);
    for (int i = 0; i < width * height; i++) {
        bgra[i * 4 + 0] = pixels[i * 4 + 2];  // B
        bgra[i * 4 + 1] = pixels[i * 4 + 1];  // G
        bgra[i * 4 + 2] = pixels[i * 4 + 0];  // R
        bgra[i * 4 + 3] = pixels[i * 4 + 3];  // A
    }
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);

    std::fprintf(stderr,
        "[DRAW] Screenshot saved: %s (%dx%d)\n",
        path, width, height);
    return true;
}

// ---- Cached GL state setters ----
// Thousands of PRIMs per frame mostly repeat the previous state; skipping the
// redundant GL calls is a large part of the per-draw CPU cost on real drivers.
// Every GL state change in this file goes through these.
namespace glc {
static GLuint vao = ~0u;
static int vp[4] = {-1, -1, -1, -1};
static double dr[2] = {-1.0, -1.0};
static int blend_en = -1;
static GLenum bsrc = 0, bdst = 0, beq = 0;
static int bsep = -1;                       // 1: alpha uses (ONE, ZERO, ADD) - see stencil alpha override
static int st_en = -1, st_mask = -1;
static GLenum st_func = 0, st_sfail = 0, st_zfail = 0, st_zpass = 0;
static int st_ref = -1, st_fmask = -1;
static float bcol[4] = {-1, -1, -1, -1};
static int cmask = -1;
static int depth_en = -1, dmask = -1;
static GLenum dfunc = 0;
static int cull_en = -1;
static GLenum cull_mode = 0;
static bool tex0 = false;
static void invalidate() {
    vao = ~0u; vp[0] = vp[1] = vp[2] = vp[3] = -1; dr[0] = dr[1] = -1.0;
    bsep = -1; st_en = -1; st_mask = -1; st_func = st_sfail = st_zfail = st_zpass = 0; st_ref = st_fmask = -1;
    blend_en = -1; bsrc = bdst = beq = 0; bcol[0] = bcol[1] = bcol[2] = bcol[3] = -1;
    cmask = -1; depth_en = -1; dmask = -1; dfunc = 0; cull_en = -1; cull_mode = 0; tex0 = false;
}
}  // namespace glc

static inline void gl_bind_vao(GLuint v) {
    if (glc::vao != v) { glBindVertexArray(v); glc::vao = v; }
}
static inline void gl_viewport(int x, int y, int w, int h) {
    if (glc::vp[0] != x || glc::vp[1] != y || glc::vp[2] != w || glc::vp[3] != h) {
        glViewport(x, y, w, h);
        glc::vp[0] = x; glc::vp[1] = y; glc::vp[2] = w; glc::vp[3] = h;
    }
}
static inline void gl_depth_range(double n, double f) {
    if (glc::dr[0] != n || glc::dr[1] != f) {
        glDepthRange(n, f);
        glc::dr[0] = n; glc::dr[1] = f;
    }
}
static inline void gl_blend_enable(bool e) {
    if (glc::blend_en != (int)e) {
        if (e) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glc::blend_en = (int)e;
    }
}
// `alpha_set`: the destination alpha (== GE stencil) must be *replaced* by the
// fragment alpha, not blended (stencil REPLACE/ZERO emulation): alpha factors
// (ONE, ZERO) and equation ADD, independent of the colour blend.
static inline void gl_blend_func(GLenum s, GLenum d, bool alpha_set = false) {
    if (glc::bsrc != s || glc::bdst != d || glc::bsep != (int)alpha_set) {
        if (alpha_set) glBlendFuncSeparate(s, d, GL_ONE, GL_ZERO); else glBlendFunc(s, d);
        glc::bsrc = s; glc::bdst = d; glc::bsep = (int)alpha_set;
    }
}
static inline void gl_blend_equation(GLenum e, bool alpha_set = false) {
    const GLenum key = alpha_set ? (e | 0x10000u) : e;
    if (glc::beq != key) {
        if (alpha_set) glBlendEquationSeparate(e, GL_FUNC_ADD); else glBlendEquation(e);
        glc::beq = key;
    }
}
static inline void gl_stencil_enable(bool e) {
    if (glc::st_en != (int)e) { if (e) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST); glc::st_en = (int)e; }
}
static inline void gl_stencil_mask(unsigned m) {
    if (glc::st_mask != (int)m) { glStencilMask(m); glc::st_mask = (int)m; }
}
static inline void gl_stencil_func(GLenum f, int ref, int mask) {
    if (glc::st_func != f || glc::st_ref != ref || glc::st_fmask != mask) {
        glStencilFunc(f, ref, mask); glc::st_func = f; glc::st_ref = ref; glc::st_fmask = mask;
    }
}
static inline void gl_stencil_op(GLenum sf, GLenum zf, GLenum zp) {
    if (glc::st_sfail != sf || glc::st_zfail != zf || glc::st_zpass != zp) {
        glStencilOp(sf, zf, zp); glc::st_sfail = sf; glc::st_zfail = zf; glc::st_zpass = zp;
    }
}
static inline void gl_blend_color(float r, float g, float b, float a) {
    if (glc::bcol[0] != r || glc::bcol[1] != g || glc::bcol[2] != b || glc::bcol[3] != a) {
        glBlendColor(r, g, b, a);
        glc::bcol[0] = r; glc::bcol[1] = g; glc::bcol[2] = b; glc::bcol[3] = a;
    }
}
static inline void gl_color_mask(bool r, bool g, bool b, bool a) {
    const int m = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
    if (glc::cmask != m) { glColorMask(r, g, b, a); glc::cmask = m; }
}
static inline void gl_depth_enable(bool e) {
    if (glc::depth_en != (int)e) {
        if (e) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glc::depth_en = (int)e;
    }
}
static inline void gl_depth_func(GLenum f) {
    if (glc::dfunc != f) { glDepthFunc(f); glc::dfunc = f; }
}
static inline void gl_depth_mask(bool m) {
    if (glc::dmask != (int)m) { glDepthMask(m ? GL_TRUE : GL_FALSE); glc::dmask = (int)m; }
}
static inline void gl_cull_enable(bool e) {
    if (glc::cull_en != (int)e) {
        if (e) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        glc::cull_en = (int)e;
    }
}
static inline void gl_cull_face(GLenum m) {
    if (glc::cull_mode != m) { glCullFace(m); glc::cull_mode = m; }
}
static inline void gl_active_tex0() {
    if (!glc::tex0) { glActiveTexture(GL_TEXTURE0); glc::tex0 = true; }
}

// Resolve the FBO id of the most-recently-presented (front) target, or the
// current render target as a fallback. Returns 0 if no target exists yet.
static GLuint front_fbo_id() {
    if (g_front_target >= 0) return g_targets[g_front_target].fbo;
    if (g_current_target >= 0) return g_targets[g_current_target].fbo;
    return 0;
}

/// Read the front FBO back as a native 480x272 top-down RGBA image. At render
/// scale > 1 the n x n block of each native pixel is box-averaged (round to
/// nearest), so `S` captures stay comparable across scales; PSPRECOMP_SHOT_FULL=1
/// returns the full (480n x 272n) image instead (w/h report the size).
/// Render (GL) thread only.
static void read_fbo_native(GLuint fbo, std::vector<uint8_t>& out, int* ow = nullptr, int* oh = nullptr) {
    const int n = g_rs;
    const int fw = 480 * n, fh = 272 * n;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    std::vector<uint8_t> pixels(size_t(fw) * fh * 4);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    static const bool full = std::getenv("PSPRECOMP_SHOT_FULL") != nullptr;
    if (n == 1 || full) {
        // Flip vertically (GL origin is bottom-left)
        out.resize(pixels.size());
        for (int y = 0; y < fh; y++)
            std::memcpy(out.data() + size_t(y) * fw * 4,
                        pixels.data() + size_t(fh - 1 - y) * fw * 4, size_t(fw) * 4);
        if (ow) *ow = fw;
        if (oh) *oh = fh;
        return;
    }
    out.resize(size_t(480) * 272 * 4);
    const int area = n * n;
    for (int y = 0; y < 272; y++) {
        for (int x = 0; x < 480; x++) {
            int acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < n; j++) {
                // output row y is the top-down row; GL row index counts from the bottom
                const uint8_t* row = pixels.data() + (size_t(fh - 1 - (y * n + j)) * fw + size_t(x) * n) * 4;
                for (int i = 0; i < n; i++)
                    for (int c = 0; c < 4; c++) acc[c] += row[i * 4 + c];
            }
            for (int c = 0; c < 4; c++)
                out[(size_t(y) * 480 + x) * 4 + c] = uint8_t((acc[c] + area / 2) / area);
        }
    }
    if (ow) *ow = 480;
    if (oh) *oh = 272;
}

// Composite the active savedata-dialog overlay (rendered at an integer
// multiple of 480x272) into a top-down RGBA readback of w x h pixels, box
// filtering when the sizes differ. No-op when no dialog is active.
static void composite_dialog_overlay(std::vector<uint8_t>& rgba, int w, int h) {
    static std::vector<uint32_t> ovl;
    int ow = 0, oh = 0;
    if (!psp_sddialog::overlay_fetch(ovl, &ow, &oh) || ow <= 0 || oh <= 0 ||
        ovl.size() != size_t(ow) * size_t(oh) || rgba.size() < size_t(w) * h * 4)
        return;
    if (ow == w && oh == h) {
        std::memcpy(rgba.data(), ovl.data(), size_t(w) * h * 4);
        return;
    }
    for (int y = 0; y < h; y++) {
        int sy0 = int(int64_t(y) * oh / h), sy1 = std::max(sy0 + 1, int(int64_t(y + 1) * oh / h));
        for (int x = 0; x < w; x++) {
            int sx0 = int(int64_t(x) * ow / w), sx1 = std::max(sx0 + 1, int(int64_t(x + 1) * ow / w));
            unsigned acc[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1 && sy < oh; sy++)
                for (int sx = sx0; sx < sx1 && sx < ow; sx++) {
                    uint32_t p = ovl[size_t(sy) * ow + sx];
                    acc[0] += p & 255; acc[1] += (p >> 8) & 255; acc[2] += (p >> 16) & 255; n++;
                }
            uint8_t* d = rgba.data() + (size_t(y) * w + x) * 4;
            if (n) { d[0] = uint8_t(acc[0] / n); d[1] = uint8_t(acc[1] / n); d[2] = uint8_t(acc[2] / n); d[3] = 255; }
        }
    }
}

static bool capture_fbo_to_tga(const char* path) {
    ge_draw_flush();
    GLuint fbo = front_fbo_id();
    std::vector<uint8_t> flipped;
    int w = 480, h = 272;
    read_fbo_native(fbo, flipped, &w, &h);
    // An active savedata dialog is composited over the frame at present
    // time; include it so the capture matches the window.
    composite_dialog_overlay(flipped, w, h);
    return write_tga(path, flipped.data(), w, h);
}

// ---- GL state mapping ----

static GLenum map_blend_factor_src(int factor) {
    switch (factor) {
    case GE_SRCBLEND_DSTCOLOR:       return GL_DST_COLOR;
    case GE_SRCBLEND_INVDSTCOLOR:
        return GL_ONE_MINUS_DST_COLOR;
    case GE_SRCBLEND_SRCALPHA:       return GL_SRC_ALPHA;
    case GE_SRCBLEND_INVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_SRCBLEND_DSTALPHA:       return GL_DST_ALPHA;
    case GE_SRCBLEND_INVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    // 2*As / 1-2*As: GL has no such factor. The draw path doubles the
    // fragment alpha in the shader (u_alpha_double) and uses the plain
    // (1-)SRC_ALPHA factor; if that is not possible it degrades to the
    // single factor. Doubled DST alpha is always approximated by the single.
    case GE_SRCBLEND_DOUBLESRCALPHA:
        return GL_SRC_ALPHA;
    case GE_SRCBLEND_DOUBLEINVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_SRCBLEND_DOUBLEDSTALPHA:
        return GL_DST_ALPHA;
    case GE_SRCBLEND_DOUBLEINVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    case GE_SRCBLEND_FIXA:           return GL_CONSTANT_COLOR;
    default:                         return GL_SRC_ALPHA;
    }
}

static GLenum map_blend_factor_dst(int factor) {
    switch (factor) {
    case GE_DSTBLEND_SRCCOLOR:       return GL_SRC_COLOR;
    case GE_DSTBLEND_INVSRCCOLOR:
        return GL_ONE_MINUS_SRC_COLOR;
    case GE_DSTBLEND_SRCALPHA:       return GL_SRC_ALPHA;
    case GE_DSTBLEND_INVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_DSTBLEND_DSTALPHA:       return GL_DST_ALPHA;
    case GE_DSTBLEND_INVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    case GE_DSTBLEND_DOUBLESRCALPHA:
        return GL_SRC_ALPHA;
    case GE_DSTBLEND_DOUBLEINVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_DSTBLEND_DOUBLEDSTALPHA:
        return GL_DST_ALPHA;
    case GE_DSTBLEND_DOUBLEINVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    case GE_DSTBLEND_FIXB:           return GL_CONSTANT_COLOR;
    default:
        return GL_ONE_MINUS_SRC_ALPHA;
    }
}

// GE stencil test function (GE_COMP: 0 NEVER, 1 ALWAYS, 2 EQUAL, 3 NOTEQUAL,
// 4 LESS, 5 LEQUAL, 6 GREATER, 7 GEQUAL) and stencil op (0 KEEP, 1 ZERO,
// 2 REPLACE, 3 INVERT, 4 INCR, 5 DECR) -> GL. Same mapping PPSSPP's GL backend uses.
static GLenum map_stencil_func(int f) {
    static const GLenum m[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return m[f & 7];
}
static GLenum map_stencil_op(int o) {
    static const GLenum m[6] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR};
    return o >= 0 && o < 6 ? m[o] : GL_KEEP;
}

static GLenum map_blend_equation(int op) {
    switch (op) {
    case GE_BLENDMODE_MUL_AND_ADD:              return GL_FUNC_ADD;
    case GE_BLENDMODE_MUL_AND_SUBTRACT:
        return GL_FUNC_SUBTRACT;
    case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE:
        return GL_FUNC_REVERSE_SUBTRACT;
    case GE_BLENDMODE_MIN:                      return GL_MIN;
    case GE_BLENDMODE_MAX:                      return GL_MAX;
    case GE_BLENDMODE_ABSDIFF: {
        // |src*sf - dst*df| has no GL equivalent (needs MAX of both
        // subtractions); approximate with src-dst, clamped at 0.
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE_BLEND] ABSDIFF blend equation approximated as "
                "GL_FUNC_SUBTRACT\n");
        }
        return GL_FUNC_SUBTRACT;
    }
    default:                                    return GL_FUNC_ADD;
    }
}

// Map the PSP depth-compare to a GL depth func.
//
// NO direction flip, even for a reversed depth range (PSP ZSCALE<0 ->
// glDepthRange near>far). glDepthRange(n, f) maps NDC z to the window depth
// n + (f - n) * (z + 1) / 2, which for n = (zc - zs)/65535 and
// f = (zc + zs)/65535 is exactly the PSP's zscreen = zc + zs * z (/65535).
// The GL depth buffer therefore holds the SAME values the PSP's does, and the
// PSP compare (e.g. GEQUAL with a negative ZSCALE, which Dissidia uses) must
// be applied unchanged. An earlier version flipped the inequality here
// ("GL stores depth on the flipped axis") -- that is wrong, and turned
// Dissidia's GEQUAL into LEQUAL against a buffer cleared to the far value,
// so no 3D fragment ever passed the depth test (every battle draw produced
// 0 samples).
static GLenum map_depth_func(int func, bool /*reversed*/) {
    switch (func) {
    case GE_COMP_NEVER:    return GL_NEVER;
    case GE_COMP_ALWAYS:   return GL_ALWAYS;
    case GE_COMP_EQUAL:    return GL_EQUAL;
    case GE_COMP_NOTEQUAL: return GL_NOTEQUAL;
    case GE_COMP_LESS:     return GL_LESS;
    case GE_COMP_LEQUAL:   return GL_LEQUAL;
    case GE_COMP_GREATER:  return GL_GREATER;
    case GE_COMP_GEQUAL:   return GL_GEQUAL;
    default:               return GL_LEQUAL;
    }
}

static GLenum map_prim_type(int prim_type) {
    switch (prim_type) {
    case GE_PRIM_POINTS:         return GL_POINTS;
    case GE_PRIM_LINES:          return GL_LINES;
    case GE_PRIM_LINE_STRIP:     return GL_LINE_STRIP;
    case GE_PRIM_TRIANGLES:      return GL_TRIANGLES;
    case GE_PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case GE_PRIM_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    case GE_PRIM_RECTANGLES:     return GL_TRIANGLES;
    default:                     return GL_TRIANGLES;
    }
}

// PSP-native render-target dimensions. Targets are allocated at
// (480*g_rs) x (272*g_rs), g_rs = PSPRECOMP_RENDER_SCALE (1..8, default 1;
// PPSSPP-style "render resolution"). All geometry is expressed in PSP pixels
// (3D via the PSP viewport registers, 2D "through" vertices via the
// 240/136 NDC mapping in ge_transform_vertices), so only the GL viewport has
// to be multiplied by g_rs: through-mode NDC already spans the whole target,
// and FBO-as-texture UVs are normalised so they are scale independent.
// Window upscaling happens later at present (blit).
static constexpr int PSP_FB_WIDTH = 480;
static constexpr int PSP_FB_HEIGHT = 272;
int ge_draw_render_scale() { return g_rs; }

// Compute the PSP viewport scale/offset + depth-range -> GL viewport/depth
// range for the live state. Generic for all games (issue #23). The math lives
// in the pure ge_compute_viewport_depth (tests/test_ge_viewport.cpp) so it is
// unit-testable without a GL context; callers issue the GL calls themselves.
static GeViewportDepth compute_state_viewport_depth(const GeState& state) {
    return ge_compute_viewport_depth(
        state.viewport_x_scale, state.viewport_y_scale,
        state.viewport_x_center, state.viewport_y_center,
        state.viewport_z_scale, state.viewport_z_center,
        state.offset_x, state.offset_y, PSP_FB_HEIGHT);
}

// A reversed depth range (PSP ZSCALE<0 -> glDepthRange near>far, issue #23)
// means the depth axis is flipped: the GL "far" value is the smaller of the
// two and is what a depth clear must fill so geometry passes the test, and
// the depth-compare direction must flip to match.
static bool depth_range_reversed(const GeViewportDepth& vp) {
    return vp.near_z > vp.far_z;
}

// ---- PSPRECOMP_GE_AB=1: same-frame fast-vs-legacy comparison ----
// Every render target gets a twin. Each PRIM / clear is executed twice with
// identical GE state and guest memory: the fast path into the real target,
// the legacy path into the twin. At each present the front target (and all
// other live targets) are read back and compared; mismatches are logged and
// the worst frames dumped as TGA to PSPRECOMP_AB_DIR (default /tmp). This is
// the only way to compare the two paths on the SAME frame (menu / battle
// animation makes separate captures differ).
static bool g_ab = false;
static bool g_ab_shadow = false;     // inside the legacy re-execution of a PRIM
static int g_ab_dumped = 0;


// Create one render target (FBO + RGBA8 color texture + Depth24 renderbuffer,
// 480x272, NEAREST) for the given normalized key. Clears it to black. The
// pool owns the GL objects; ge_draw_shutdown deletes them.
static GeTarget make_target(uint32_t key) {
    GeTarget t;
    t.key = key;
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);

    glGenTextures(1, &t.color_tex);
    glBindTexture(GL_TEXTURE_2D, t.color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 480 * g_rs, 272 * g_rs, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, t.color_tex, 0);

    glGenRenderbuffers(1, &t.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, t.depth_rb);
    // Depth + 8-bit stencil: the GE stencil buffer is the framebuffer's alpha
    // channel (STENCILTEST / STENCILOP), emulated with a real GL stencil
    // buffer next to the colour alpha (see apply_gl_pipeline_impl).
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 480 * g_rs, 272 * g_rs);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, t.depth_rb);
    if (g_ab) {
        glGenFramebuffers(1, &t.ab_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.ab_fbo);
        glGenTextures(1, &t.ab_tex);
        glBindTexture(GL_TEXTURE_2D, t.ab_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 480 * g_rs, 272 * g_rs, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.ab_tex, 0);
        glGenRenderbuffers(1, &t.ab_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t.ab_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 480 * g_rs, 272 * g_rs);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.ab_depth);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[DRAW] FBO incomplete: 0x%04X (key=0x%06X)\n",
                     status, key);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);  // a draw may have narrowed it
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glc::invalidate();                 // raw GL state calls above
    ge_texture_invalidate_bind_cache();  // glBindTexture above
    t.last_use = ++g_target_clock;
    return t;
}

// Find the pool slot for key, or allocate one (LRU-evicting if full).
static int acquire_target(uint32_t key) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo != 0 && g_targets[i].key == key) {
            g_targets[i].last_use = ++g_target_clock;
            return i;
        }
    }
    int slot = -1;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo == 0) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;  // LRU evict
        for (int i = 1; i < GE_MAX_TARGETS; ++i) {
            if (g_targets[i].last_use < g_targets[slot].last_use) slot = i;
        }
        std::fprintf(stderr,
            "[DRAW] FBO pool overflow: evicting key=0x%06X for key=0x%06X\n",
            g_targets[slot].key, key);
        glDeleteFramebuffers(1, &g_targets[slot].fbo);
        glDeleteTextures(1, &g_targets[slot].color_tex);
        glDeleteRenderbuffers(1, &g_targets[slot].depth_rb);
        if (g_targets[slot].ab_fbo) {
            glDeleteFramebuffers(1, &g_targets[slot].ab_fbo);
            glDeleteTextures(1, &g_targets[slot].ab_tex);
            glDeleteRenderbuffers(1, &g_targets[slot].ab_depth);
        }
    }
    g_targets[slot] = make_target(key);
    std::fprintf(stderr, "[DRAW] FBO target created key=0x%06X slot=%d\n",
                 key, slot);
    return slot;
}

// Bind the FBO for the current FRAMEBUFPTR. A raw 0 (FRAMEBUFPTR not yet
// seen) keeps the current/last target so early lists still land somewhere.
static bool g_framebufptr_seen = false;   // any GE_CMD_FRAMEBUFPTR decoded yet?

void ge_draw_note_framebufptr() { g_framebufptr_seen = true; }

void ge_draw_flush();
void ge_draw_select_target(uint32_t fb_addr_raw) {
    ge_draw_flush();   // pending batch belongs to the previously bound target
    uint32_t key = ge_fb_key(fb_addr_raw);
    // Only before the first FRAMEBUFPTR command does key 0 mean "unset":
    // the command's data for VRAM base 0x04000000 is literally 0, and that
    // buffer is a real, common render target.
    if (key == 0 && !g_framebufptr_seen) {
        if (g_current_target < 0) g_current_target = acquire_target(0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_targets[g_current_target].fbo);
        return;
    }
    int slot = acquire_target(key);
    if (std::getenv("PSPRECOMP_MOVIE_TEXDBG"))
        std::fprintf(stderr, "[SELTGT] fb=%08X key=%06X slot=%d\n", fb_addr_raw, key, slot);
    g_current_target = slot;
    if (g_front_target < 0) g_front_target = slot;
    glBindFramebuffer(GL_FRAMEBUFFER, g_targets[slot].fbo);
}

void ge_draw_mark_dirty() {
    g_present_dirty = true;
}

// Apply viewport/depth-range, program + uniforms, texture and the blend /
// mask / depth / cull state for one draw (or batch). `gpu_xform` selects the
// vertex-shader transform path (the caller sets the xform uniforms itself).
// Returns whether the depth range is reversed.
static bool apply_gl_pipeline_impl(uint8_t* rdram, const GeState& state, int prim_type,
                              GLuint fbo_tex, bool gpu_xform);
static bool g_pipe_alpha_double = false;   // last apply_gl_pipeline: GE 2*As emulated by doubling alpha
static bool apply_gl_pipeline(uint8_t* rdram, const GeState& state, int prim_type,
                              GLuint fbo_tex, bool gpu_xform) {
    static const char* xv = std::getenv("PSPRECOMP_XP_VT");
    static unsigned xvt = xv ? (unsigned)std::strtoul(xv, nullptr, 16) : 0;
    static unsigned xm = std::getenv("PSPRECOMP_XP_MODE") ? (unsigned)std::atoi(std::getenv("PSPRECOMP_XP_MODE")) : 0;
    // PSPRECOMP_XP_FILE=<path>: file "<hexvt> <mode>" re-read every 32 draws so the
    // experiment can be changed without restarting (mode bits: 1 depth off, 2 alpha
    // test off, 4 blend off, 8 texture off, 16 cull off).
    static const char* xf = std::getenv("PSPRECOMP_XP_FILE");
    static bool xv_live = false;
    static bool xp_tex = false;
    if (xf) {
        static int xcnt = 0;
        if ((xcnt++ & 31) == 0) {
            if (FILE* f = std::fopen(xf, "r")) {
                char b1[32] = {}; int m = 0;
                if (std::fscanf(f, "%31s %d", b1, &m) == 2) {
                    // "tADDR": match on the 24-bit texture-0 address instead of the vertex type
                    xp_tex = (b1[0] == 't');
                    xvt = (unsigned)std::strtoul(b1 + (xp_tex ? 1 : 0), nullptr, 16); xm = (unsigned)m; xv_live = true; g_xp_nofilter = (m & 32) ? 1 : 0;
                }
                std::fclose(f);
            }
        }
    }
    if ((xv || xv_live) && (xp_tex ? ((state.texture_enable ? (state.tex_addr[0] & 0xFFFFFF) : 0xFFFFFFFFu) == xvt)
                                   : ((state.vertex_type & 0xFFFFFF) == xvt))) {
        GeState st2 = state;
        { static int xlog = 0; if (xlog++ < 3) std::fprintf(stderr, "[XP] applying vt=%06X mode=%u\n", xvt, xm); }
        if (xm & 16) st2.cull_enable = false;
        if (xm & 64) { st2.mask_rgb = 0xFFFFFF; st2.mask_alpha = 0xFF; }  // hide this vtype (write nothing)
        if (xm & 1) st2.depth_test_enable = false;
        if (xm & 2) st2.alpha_test_enable = false;
        if (xm & 4) st2.alpha_blend_enable = false;
        if (xm & 8) st2.texture_enable = false;
        return apply_gl_pipeline_impl(rdram, st2, prim_type, fbo_tex, gpu_xform);
    }
    return apply_gl_pipeline_impl(rdram, state, prim_type, fbo_tex, gpu_xform);
}
static bool apply_gl_pipeline_impl(uint8_t* rdram, const GeState& state, int prim_type,
                              GLuint fbo_tex, bool gpu_xform) {
    // Viewport / depth-range (issue #23). Transform-mode draws emit NDC via
    // the real MVP path, so they must map through the PSP viewport
    // scale/offset + depth range to land on screen. Through-mode (2D/sprite)
    // already bakes the full-screen 240/136 mapping into NDC in
    // ge_transform_vertices, so it keeps the full-buffer viewport set by
    // ge_draw_begin_list and an identity depth range.
    bool depth_reversed = false;
    if (!ge_vtype_through(state.vertex_type)) {
        const GeViewportDepth vp = compute_state_viewport_depth(state);
        gl_viewport(vp.x * g_rs, vp.y * g_rs, vp.w * g_rs, vp.h * g_rs);
        gl_depth_range(vp.near_z, vp.far_z);
        depth_reversed = depth_range_reversed(vp);
    } else {
        gl_viewport(0, 0, PSP_FB_WIDTH * g_rs, PSP_FB_HEIGHT * g_rs);
        gl_depth_range(0.0, 1.0);
    }


    {
        psp_prof::Scope ps(psp_prof::S_UNIFORM);
        ge_shader_use();
        ge_shader_set_uniforms(state);
        // gpu_xform: the caller sets the 3D transform uniforms afterwards.
        if (!gpu_xform) ge_shader_set_xform_passthrough();
    }
    {
        psp_prof::Scope ps(psp_prof::S_TEX);
    // Bind texture if enabled
    if (state.texture_enable) {
        gl_active_tex0();
        if (fbo_tex) {
            int dw = 1 << (state.tex_size[0] & 0xFF);
            int dh = 1 << ((state.tex_size[0] >> 8) & 0xFF);
            ge_texture_bind_external(fbo_tex, state);
            ge_shader_set_tex_remap(dw / 480.0f, dh / 272.0f, true);
        } else {
            ge_texture_bind(rdram, g_frame_counter);
            ge_shader_set_tex_remap(1.0f, 1.0f, false);
        }
    } else {
        ge_shader_set_tex_remap(1.0f, 1.0f, false);
    }
    }
    psp_prof::Scope ps_gs(psp_prof::S_GLSTATE);
    // Stencil (GE STENCILTEST 0xDC / STENCILOP 0xDD; PPSSPP GPU_GLES StateMapping).
    // The GE keeps the stencil in the framebuffer's alpha channel. The test and the
    // ops run on a real GL stencil buffer; the colour alpha channel is kept in step
    // the way PPSSPP's "stencil to alpha" does it: a zpass REPLACE writes the
    // reference value into alpha, ZERO writes 0, and ops that leave alpha alone
    // (KEEP / INVERT / INCR / DECR) mask the alpha write.
    //   STENCILTEST: bits 0-7 func, 8-15 ref, 16-23 mask
    //   STENCILOP:   bits 0-7 sfail, 8-15 zfail, 16-23 zpass
    float stencil_alpha = -1.0f;      // >= 0: fragment alpha is replaced by this
    bool stencil_alpha_keep = false;  // stencil enabled, op leaves dst alpha unchanged
    const int smode = g_stencil_mode.load(std::memory_order_relaxed);
    const bool stencil_on = state.stencil_test_enable && (smode & 1);
    if (stencil_on && (smode & 2)) {
        const int zpass = (state.stencil_op >> 16) & 0xFF;
        if (zpass == 2) stencil_alpha = (float)((state.stencil_test >> 8) & 0xFF) / 255.0f;
        else if (zpass == 1) stencil_alpha = 0.0f;
        else stencil_alpha_keep = true;
    }
    // Set GL state: alpha blend
    bool alpha_double = false;
    if (state.alpha_blend_enable) {
        gl_blend_enable(true);
        int src_factor = state.blend_mode & 0xF;
        int dst_factor = (state.blend_mode >> 4) & 0xF;
        int blend_op   = (state.blend_mode >> 8) & 0x7;

        // GE FIXA / FIXB are per-channel constant factors (0xBBGGRR) that
        // replace the source / destination factor. GL has ONE blend constant,
        // so map the common cases to ZERO / ONE / (1-)CONSTANT_COLOR and only
        // upload a constant when one is actually used. The constant is global
        // GL state: it MUST be (re)set by every draw that uses it, otherwise
        // the dst=FIXB / src=FIXA draw blends against whatever colour an
        // earlier unrelated draw left behind (0,0,0 by default).
        GLenum gl_src = map_blend_factor_src(src_factor);
        GLenum gl_dst = map_blend_factor_dst(dst_factor);

        // Doubled source-alpha factors (6: 2*As, 7: 1-2*As, on either side).
        // clamp(2*As) with SRC_ALPHA / ONE_MINUS_SRC_ALPHA reproduces both,
        // so double + clamp the fragment alpha in the shader. This is only
        // valid when every source-alpha factor in this blend is doubled
        // (the other side may use colour/dst/fixed factors); a mix of plain
        // and doubled src-alpha factors cannot share one alpha value, so
        // fall back to the single factors in that case. MIN/MAX ignore
        // factors, so no doubling there.
        auto uses_sa = [](int f) { return f == 2 || f == 3 || f == 6 || f == 7; };
        auto is_dbl  = [](int f) { return f == 6 || f == 7; };
        if (blend_op != GE_BLENDMODE_MIN && blend_op != GE_BLENDMODE_MAX
            && (is_dbl(src_factor) || is_dbl(dst_factor))) {
            const bool src_ok = !uses_sa(src_factor) || is_dbl(src_factor);
            const bool dst_ok = !uses_sa(dst_factor) || is_dbl(dst_factor);
            if (src_ok && dst_ok) {
                alpha_double = true;
            } else {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::fprintf(stderr,
                        "[GE_BLEND] mixed plain/doubled src-alpha factors "
                        "(src=%d dst=%d): doubling dropped\n",
                        src_factor, dst_factor);
                }
            }
        }
        if (dst_factor == 8 || dst_factor == 9 || src_factor == 8 || src_factor == 9) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[GE_BLEND] doubled dst-alpha factor approximated by the "
                    "single factor (src=%d dst=%d)\n", src_factor, dst_factor);
            }
        }
        const bool src_fix = (src_factor == GE_SRCBLEND_FIXA);
        const bool dst_fix = (dst_factor == GE_DSTBLEND_FIXB);
        if (src_fix || dst_fix) {
            const uint32_t fixa = state.blend_fix_a & 0xFFFFFFu;
            const uint32_t fixb = state.blend_fix_b & 0xFFFFFFu;
            bool have_const = false;
            uint32_t konst = 0;
            auto fix_factor = [&](uint32_t fix) -> GLenum {
                if (fix == 0x000000u) return GL_ZERO;
                if (fix == 0xFFFFFFu) return GL_ONE;
                if (!have_const) {
                    have_const = true;
                    konst = fix;
                    return GL_CONSTANT_COLOR;
                }
                if (konst == fix) return GL_CONSTANT_COLOR;
                if (konst == (~fix & 0xFFFFFFu)) return GL_ONE_MINUS_CONSTANT_COLOR;
                return GL_CONSTANT_COLOR;  // two distinct constants: approximate
            };
            if (src_fix) gl_src = fix_factor(fixa);
            if (dst_fix) gl_dst = fix_factor(fixb);
            if (have_const) {
                gl_blend_color((konst & 0xFF) / 255.0f,
                             ((konst >> 8) & 0xFF) / 255.0f,
                             ((konst >> 16) & 0xFF) / 255.0f,
                             1.0f);
            }
        }
        gl_blend_func(gl_src, gl_dst, stencil_alpha >= 0.0f);
        gl_blend_equation(map_blend_equation(blend_op), stencil_alpha >= 0.0f);
    } else {
        gl_blend_enable(false);
    }
    ge_shader_set_alpha_double(alpha_double);
    ge_shader_set_alpha_override(stencil_alpha);
    g_pipe_alpha_double = alpha_double;   // FX shader overrides re-apply it (psp_fx_shader.h)

    // Colour write mask (MASKRGB 0xBBGGRR / MASKALPHA): the PSP writes a
    // channel only where its mask bits are 0. GL can only mask whole
    // channels, so a channel is written iff its mask byte != 0xFF
    // (partial per-bit masks are approximated as fully writable).
    {
        const uint32_t mr = state.mask_rgb;
        const uint32_t ma = state.mask_alpha & 0xFF;
        const bool wr = (mr & 0xFF) != 0xFF;
        const bool wg = ((mr >> 8) & 0xFF) != 0xFF;
        const bool wb = ((mr >> 16) & 0xFF) != 0xFF;
        const bool wa = ma != 0xFF && !stencil_alpha_keep;
        gl_color_mask(wr, wg, wb, wa);
        static bool warned = false;
        const bool partial = (((mr & 0xFF) | ((mr >> 8) & 0xFF) | ((mr >> 16) & 0xFF) | ma) != 0)
            && (((mr & 0xFF) != 0 && (mr & 0xFF) != 0xFF)
                || (((mr >> 8) & 0xFF) != 0 && ((mr >> 8) & 0xFF) != 0xFF)
                || (((mr >> 16) & 0xFF) != 0 && ((mr >> 16) & 0xFF) != 0xFF)
                || (ma != 0 && ma != 0xFF));
        if (partial && !warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE_MASK] partial colour write mask rgb=0x%06X a=0x%02X "
                "approximated per channel\n", mr, ma);
        }
    }

    // Stencil test. The stencil write mask is the GE alpha mask (MASKALPHA: a set
    // bit protects that bit).
    if (stencil_on) {
        gl_stencil_enable(true);
        gl_stencil_func(map_stencil_func((int)(state.stencil_test & 0xFF)),
                        (int)((state.stencil_test >> 8) & 0xFF), (int)((state.stencil_test >> 16) & 0xFF));
        gl_stencil_op(map_stencil_op((int)(state.stencil_op & 0xFF)),
                      map_stencil_op((int)((state.stencil_op >> 8) & 0xFF)),
                      map_stencil_op((int)((state.stencil_op >> 16) & 0xFF)));
        gl_stencil_mask(~(unsigned)(state.mask_alpha & 0xFF) & 0xFFu);
    } else {
        gl_stencil_enable(false);
    }

    // Depth test
    if (state.depth_test_enable) {
        gl_depth_enable(true);
        gl_depth_func(map_depth_func(state.depth_func, depth_reversed));
        gl_depth_mask(
            state.depth_write_disable
            ? GL_FALSE : GL_TRUE);
    } else {
        gl_depth_enable(false);
    }

    // Cull face. The PSP never culls RECTANGLES (sprites are expanded by the
    // hardware with no winding) — a game may leave CULLFACE on while drawing
    // its 2D layer (Dissidia's movie strips).
    if (state.cull_enable && prim_type != 6) {
        gl_cull_enable(true);
        gl_cull_face(
            state.cull_face == 0
            ? GL_BACK : GL_FRONT);
    } else {
        gl_cull_enable(false);
    }

    return depth_reversed;
}


// ============================================================================
// Batched fast path
//
// ge_draw_prim no longer issues one GL draw (plus ~40 GL calls) per PRIM.
// Consecutive PRIMs whose complete GL-visible state is identical are appended
// to a CPU staging buffer and submitted as ONE draw (glDrawArrays, or
// glMultiDrawArrays for strips/fans) when the state changes or the display
// list yields (ge_draw_flush). Non-through draws are decoded in model space
// and skinned / lit / transformed in the vertex shader; through-mode draws,
// rectangles and degenerate-matrix fallbacks keep the CPU transform and use a
// passthrough vertex stage.
// ============================================================================

struct Stream {
    GLuint vbo = 0;
    GLuint vao = 0;          // 2D: CPU-transformed; 3D: skinned layout
    GLuint vao_plain = 0;    // 3D: unskinned layout (weights disabled)
    size_t stride = 0;
    size_t cap = 0;          // ring capacity in vertices
    size_t write = 0;        // next free ring slot (vertices)
    std::vector<uint8_t> stage;
    size_t stage_n = 0;      // vertices staged for the active batch
};
// Ring buffer synchronisation is by ORPHANING, not fences: vertices are
// appended at increasing offsets (a region is never rewritten while its
// storage is live), and when the ring wraps the whole buffer is re-specified
// with glBufferData(NULL), which gives the driver fresh storage while any
// in-flight draw keeps the old one. No glFenceSync / glClientWaitSync, no
// persistent mapping (GL_ARB_buffer_storage does not exist on macOS), and it
// is the idiom every GL 3.x driver (Mesa/radeonsi, Apple, NVIDIA) optimises.
// PSPRECOMP_GE_RING=sub (glBufferSubData) or map (unsynchronised
// glMapBufferRange + explicit flush; same orphan-on-wrap safety). Default:
// map on macOS, sub elsewhere. On Apple's GL-over-Metal each glBufferSubData
// into the ring the GPU is still reading costs ~40 us (Sky Fortress Bahamut at
// 4x: 713 uploads = 29 ms/frame, 19.7 fps); map took that to 0.17 ms (30 fps).
// Measured 2026-10-22 on an M2 Pro.

// PSPRECOMP_GE_SYNC: 1 = glFinish after every batch, 2 = glFinish at the end
// of every display list (FINISH), 3 = glFinish once per present, flush =
// glFlush after every batch (diagnostics / escape hatch for drivers whose
// asynchronous behaviour differs from the one this was developed on).
#ifdef __APPLE__
static int g_ring_mode = 1;
#else
static int g_ring_mode = 0;
#endif
static int g_sync_mode = 0;
static bool g_sync_each = false;
static Stream g_s2d, g_s3d;
// Ring size in vertices. 3D vertices are 68 bytes, 2D 28: 8.9 MB + 3.6 MB.
// Sized for several frames of high-poly characters; small enough for an APU
// (Steam Deck) where it is carved from shared system memory. A single batch
// bigger than the ring grows it (see flush_batch).
static constexpr size_t RING_VERTS = 131072;

static bool g_fast_enabled = true;    // PSPRECOMP_GE_FAST=0 / PSPRECOMP_GE_LEGACY=1 disables
static bool g_cpu_xform = false;      // PSPRECOMP_GE_CPU3D=1: CPU transform, still batched
static bool g_nobatch = false;        // PSPRECOMP_GE_NOBATCH=1: submit every PRIM separately
static bool g_verify = false;         // PSPRECOMP_GE_VERIFY=1: compare GPU vertex stage to the CPU path
static bool g_debug_env = false;      // any per-draw diagnostic env set -> legacy path

struct KeyBuf {
    uint32_t w[512];
    int n = 0;
    inline void u(uint32_t v) { w[n++] = v; }
    inline void f(float v) { uint32_t b; std::memcpy(&b, &v, 4); w[n++] = b; }
    inline void a(const float* p, int c) { std::memcpy(&w[n], p, (size_t)c * 4); n += c; }
    bool operator==(const KeyBuf& o) const {
        return n == o.n && std::memcmp(w, o.w, (size_t)n * 4) == 0;
    }
};

struct Xf3D {
    float world[12], view[12], proj[16], uvxf[4];
    float bones[96];
    int skin_n = 0, lit = 0, lflags = 0, nl = 0;
    float lc[4][4];
    float lt[20][4];
};

static inline float rgb_f(uint32_t c, int ch) {
    return static_cast<float>((c >> (ch * 8)) & 0xFFu) * (1.0f / 255.0f);
}

static void build_xf(const GeState& st, Xf3D& x) {
    const uint32_t vt = st.vertex_type;
    std::memcpy(x.world, st.world_matrix, sizeof x.world);
    std::memcpy(x.view, st.view_matrix, sizeof x.view);
    std::memcpy(x.proj, st.proj_matrix, sizeof x.proj);
    if (ge_vtype_tc(vt) != GE_VTYPE_TC_NONE) {
        x.uvxf[0] = st.tex_scale_u; x.uvxf[1] = st.tex_scale_v;
        x.uvxf[2] = st.tex_offset_u; x.uvxf[3] = st.tex_offset_v;
    } else {
        x.uvxf[0] = 1.0f; x.uvxf[1] = 1.0f; x.uvxf[2] = 0.0f; x.uvxf[3] = 0.0f;
    }
    if (ge_vtype_weight(vt) != 0) {
        const int wcount = ((vt >> GE_VTYPE_WEIGHTCOUNT_SHIFT) & GE_VTYPE_WEIGHTCOUNT_MASK) + 1;
        x.skin_n = std::min(wcount, 8);
        std::memcpy(x.bones, st.bone_matrix, (size_t)x.skin_n * 12 * sizeof(float));
    } else {
        x.skin_n = 0;
    }
    x.lit = st.lighting_enable ? 1 : 0;
    x.lflags = 0;
    x.nl = 0;
    if (x.lit) {
        const bool has_col = ge_vtype_col(vt) != GE_VTYPE_COL_NONE;
        const bool has_nrm = ge_vtype_nrm(vt) != GE_VTYPE_NRM_NONE;
        if ((st.material_update & 1u) && has_col) x.lflags |= 1;
        if ((st.material_update & 2u) && has_col) x.lflags |= 2;
        if (has_nrm) x.lflags |= 4;
        for (int c = 0; c < 3; c++) {
            x.lc[0][c] = rgb_f(st.material_emissive, c);
            x.lc[1][c] = rgb_f(st.material_ambient, c);
            x.lc[2][c] = rgb_f(st.material_diffuse, c);
            x.lc[3][c] = rgb_f(st.ambient_color, c);
        }
        x.lc[0][3] = 0.0f;
        x.lc[1][3] = static_cast<float>(st.material_alpha & 0xFFu) / 255.0f;
        x.lc[2][3] = 0.0f;
        x.lc[3][3] = static_cast<float>(st.ambient_alpha & 0xFFu) / 255.0f;
        for (int i = 0; i < 4; i++) {
            if (!st.light_enable[i]) continue;
            float (*L)[4] = &x.lt[x.nl * 5];
            L[0][0] = st.light_pos[i][0]; L[0][1] = st.light_pos[i][1]; L[0][2] = st.light_pos[i][2];
            L[0][3] = static_cast<float>((st.light_type[i] >> 8) & 3);
            L[1][0] = st.light_dir[i][0]; L[1][1] = st.light_dir[i][1]; L[1][2] = st.light_dir[i][2];
            L[1][3] = st.light_spot_exp[i];
            L[2][0] = st.light_att[i][0]; L[2][1] = st.light_att[i][1]; L[2][2] = st.light_att[i][2];
            L[2][3] = st.light_spot_cut[i];
            for (int c = 0; c < 3; c++) {
                L[3][c] = rgb_f(st.light_amb[i], c);
                L[4][c] = rgb_f(st.light_dif[i], c);
            }
            L[3][3] = 0.0f; L[4][3] = 0.0f;
            x.nl++;
        }
    }
}

// Complete GL-visible state of a draw, as a word string. Two draws with equal
// keys render identically when merged into one GL draw call.
static void build_key(KeyBuf& k, const GeState& st, GLenum mode, int path, int prim_type,
                      GLuint fbo_tex, const Xf3D* xf) {
    const uint32_t vt_nidx = st.vertex_type & ~(uint32_t)(GE_VTYPE_IDX_MASK << GE_VTYPE_IDX_SHIFT);
    const bool through = ge_vtype_through(st.vertex_type);
    k.u(mode); k.u(path); k.u(fbo_tex); k.u(vt_nidx);
    k.u(st.texture_enable ? 1u : 0u);
    if (st.texture_enable) {
        k.u(st.tex_addr[0]); k.u(st.tex_bufw[0]); k.u(st.tex_size[0]);
        k.u(st.tex_format); k.u(st.tex_mode);
        k.u(st.clut_addr); k.u(st.clut_addr_upper); k.u(st.clut_format);
        k.u(st.tex_func | (st.tex_func_rgba ? 0x100u : 0u) | (st.tex_func_double ? 0x200u : 0u));
        k.u(st.tex_filter); k.u(st.tex_wrap); k.u(st.tex_env_color);
    }
    k.u(st.alpha_blend_enable ? 1u : 0u);
    if (st.alpha_blend_enable) { k.u(st.blend_mode); k.u(st.blend_fix_a); k.u(st.blend_fix_b); }
    k.u(st.depth_test_enable ? 1u : 0u);
    if (st.depth_test_enable) { k.u(st.depth_func); k.u(st.depth_write_disable ? 1u : 0u); }
    k.u((st.cull_enable && prim_type != GE_PRIM_RECTANGLES) ? (2u | (st.cull_face & 1u)) : 0u);
    k.u(st.mask_rgb); k.u(st.mask_alpha & 0xFFu);
    k.u(st.stencil_test_enable ? 1u : 0u);
    if (st.stencil_test_enable) { k.u(st.stencil_test & 0xFFFFFFu); k.u(st.stencil_op & 0xFFFFFFu); }
    k.u(st.alpha_test_enable ? 1u : 0u);
    if (st.alpha_test_enable) k.u(st.alpha_test);
    k.u(st.color_test_enable ? 1u : 0u);
    if (st.color_test_enable) { k.u(st.color_test & 3u); k.u(st.color_ref); k.u(st.color_test_mask); }
    if (!through) {
        k.f(st.viewport_x_scale); k.f(st.viewport_y_scale); k.f(st.viewport_z_scale);
        k.f(st.viewport_x_center); k.f(st.viewport_y_center); k.f(st.viewport_z_center);
        k.u(st.offset_x); k.u(st.offset_y);
    }
    if (xf) {
        k.a(xf->world, 12); k.a(xf->view, 12); k.a(xf->proj, 16); k.a(xf->uvxf, 4);
        k.u((uint32_t)xf->skin_n);
        if (xf->skin_n) k.a(xf->bones, xf->skin_n * 12);
        k.u((uint32_t)xf->lit);
        if (xf->lit) {
            k.u((uint32_t)xf->lflags); k.u((uint32_t)xf->nl);
            k.a(&xf->lc[0][0], 16);
            if (xf->nl) k.a(&xf->lt[0][0], xf->nl * 20);
        }
    }
}

struct Batch {
    bool active = false;
    KeyBuf key;
    GLenum mode = GL_TRIANGLES;
    Stream* s = nullptr;
    GLuint vao = 0;
    std::vector<GLint> firsts;
    std::vector<GLsizei> counts;
    bool fx_timed = false;   // FX shader override batch, GPU-timed (PSPRECOMP_FX_SHADER_LOG=2)
};
static Batch g_batch;
static GLuint g_copy_buf_bound = 0;

// ---- PSPRECOMP_GE_VERIFY: GPU vertex stage vs the CPU reference path ----
// Each GPU-transformed GL_TRIANGLES PRIM is flushed alone with transform
// feedback capturing gl_Position / v_texcoord / v_color; the capture is
// compared with ge_decode_vertices + ge_transform_vertices (the CPU path).
struct VerifyExpect { float pos[4], uv[2], col[4]; };
static std::vector<VerifyExpect> g_expect;
static bool g_expect_valid = false;
static GLuint g_tf_buf = 0;
static struct {
    uint64_t draws = 0, verts = 0, bad_pos = 0, bad_uv = 0, bad_col = 0;
    double max_pos = 0, max_uv = 0, max_col = 0;
    uint64_t lit_draws = 0, skin_draws = 0;
} g_vstat;
static bool g_expect_lit = false, g_expect_skin = false;

static void verify_report(const char* tag) {
    std::fprintf(stderr,
        "[VERIFY] %s draws=%llu (lit=%llu skinned=%llu) verts=%llu | max clip err %.3g (bad>1e-3: %llu) "
        "max uv err %.3g (bad>1e-4: %llu) max colour err %.2f LSB (bad>1.01: %llu)\n",
        tag, (unsigned long long)g_vstat.draws, (unsigned long long)g_vstat.lit_draws,
        (unsigned long long)g_vstat.skin_draws, (unsigned long long)g_vstat.verts,
        g_vstat.max_pos, (unsigned long long)g_vstat.bad_pos, g_vstat.max_uv,
        (unsigned long long)g_vstat.bad_uv, g_vstat.max_col, (unsigned long long)g_vstat.bad_col);
}

static void verify_compare(size_t n) {
    std::vector<float> got(n * 10);
    glBindBuffer(GL_COPY_READ_BUFFER, g_tf_buf);
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)(n * 10 * sizeof(float)), got.data());
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    g_vstat.draws++;
    static const bool fault = std::getenv("PSPRECOMP_GE_VERIFY_FAULT") != nullptr;  // harness self-check
    if (fault && !g_expect.empty()) g_expect[0].pos[0] += 0.5f;
    if (g_vstat.draws <= 3 && !g_expect.empty())
        std::fprintf(stderr, "[VERIFY] sample v0 gpu pos=(%g,%g,%g,%g) col=(%.1f,%.1f,%.1f,%.1f) | cpu pos=(%g,%g,%g,%g) col=(%.1f,%.1f,%.1f,%.1f)\n",
            got[0], got[1], got[2], got[3], got[6] * 255, got[7] * 255, got[8] * 255, got[9] * 255,
            g_expect[0].pos[0], g_expect[0].pos[1], g_expect[0].pos[2], g_expect[0].pos[3],
            g_expect[0].col[0], g_expect[0].col[1], g_expect[0].col[2], g_expect[0].col[3]);
    if (g_expect_lit) g_vstat.lit_draws++;
    if (g_expect_skin) g_vstat.skin_draws++;
    const size_t m = std::min(n, g_expect.size());
    for (size_t i = 0; i < m; i++) {
        const float* g = &got[i * 10];
        const VerifyExpect& e = g_expect[i];
        g_vstat.verts++;
        double pe = 0;
        for (int k = 0; k < 4; k++) {
            const double d = std::fabs((double)g[k] - e.pos[k]) / std::max(1.0, std::fabs((double)e.pos[k]));
            pe = std::max(pe, d);
        }
        g_vstat.max_pos = std::max(g_vstat.max_pos, pe);
        if (pe > 1e-3) g_vstat.bad_pos++;
        double ue = 0;
        for (int k = 0; k < 2; k++)
            ue = std::max(ue, std::fabs((double)g[4 + k] - e.uv[k]) / std::max(1.0, std::fabs((double)e.uv[k])));
        g_vstat.max_uv = std::max(g_vstat.max_uv, ue);
        if (ue > 1e-4) g_vstat.bad_uv++;
        double ce = 0;
        for (int k = 0; k < 4; k++) ce = std::max(ce, std::fabs((double)g[6 + k] * 255.0 - e.col[k]));
        g_vstat.max_col = std::max(g_vstat.max_col, ce);
        if (ce > 1.01) g_vstat.bad_col++;
        if ((pe > 1e-3 || ue > 1e-4 || ce > 1.01) && (g_vstat.bad_pos + g_vstat.bad_uv + g_vstat.bad_col) <= 8) {
            std::fprintf(stderr,
                "[VERIFY] MISMATCH v%zu/%zu gpu pos=(%g,%g,%g,%g) uv=(%g,%g) col=(%.1f,%.1f,%.1f,%.1f) | cpu pos=(%g,%g,%g,%g) uv=(%g,%g) col=(%.1f,%.1f,%.1f,%.1f)\n",
                i, n, g[0], g[1], g[2], g[3], g[4], g[5], g[6] * 255, g[7] * 255, g[8] * 255, g[9] * 255,
                e.pos[0], e.pos[1], e.pos[2], e.pos[3], e.uv[0], e.uv[1], e.col[0], e.col[1], e.col[2], e.col[3]);
        }
    }
    if (g_vstat.draws % 2000 == 0) verify_report("progress");
}

static void flush_batch() {
    if (!g_batch.active) return;
    Stream& s = *g_batch.s;
    g_batch.active = false;
    const bool fx_timed = g_batch.fx_timed;
    g_batch.fx_timed = false;
    const size_t n = s.stage_n;
    if (n == 0) { g_batch.firsts.clear(); g_batch.counts.clear(); return; }

    {
        psp_prof::Scope ps(psp_prof::S_UPLOAD);
        if (g_copy_buf_bound != s.vbo) {
            glBindBuffer(GL_COPY_WRITE_BUFFER, s.vbo);
            g_copy_buf_bound = s.vbo;
        }
        if (n > s.cap) {
            // Single batch larger than the ring: re-specify (orphan) at the new size.
            s.cap = n;
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(s.cap * s.stride), nullptr, GL_STREAM_DRAW);
            s.write = 0;
        } else if (s.write + n > s.cap) {
            // Ring wrapped: orphan so in-flight draws keep the old storage.
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(s.cap * s.stride), nullptr, GL_STREAM_DRAW);
            s.write = 0;
        }
        bool done = false;
        if (g_ring_mode == 1) {
            const GLsizeiptr len = (GLsizeiptr)(n * s.stride);
            void* dst = glMapBufferRange(GL_COPY_WRITE_BUFFER, (GLintptr)(s.write * s.stride), len,
                                         GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
            if (dst) {
                std::memcpy(dst, s.stage.data(), (size_t)len);
                glFlushMappedBufferRange(GL_COPY_WRITE_BUFFER, 0, len);
                glUnmapBuffer(GL_COPY_WRITE_BUFFER);
                done = true;
            }
        }
        if (!done)
            glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)(s.write * s.stride),
                            (GLsizeiptr)(n * s.stride), s.stage.data());
    }
    const GLint first = static_cast<GLint>(s.write);
    s.write += n;

    {
        psp_prof::Scope ps(psp_prof::S_DRAW);
        gl_bind_vao(g_batch.vao);
        if (fx_timed) psp_fx_timer_begin();
        if (g_no_raster != 2) {
            const size_t nseg = g_batch.firsts.size();
            const GLenum m = g_batch.mode;
            if (g_verify && g_expect_valid && g_tf_buf
                && (m == GL_TRIANGLES || m == GL_TRIANGLE_STRIP || m == GL_TRIANGLE_FAN)) {
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, g_tf_buf);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(GL_TRIANGLES);
                glDrawArrays(m, first, static_cast<GLsizei>(n));
                glEndTransformFeedback();
                if (g_no_raster != 1) glDisable(GL_RASTERIZER_DISCARD);
                verify_compare(g_expect.size());
            } else if (nseg == 1 || m == GL_TRIANGLES || m == GL_LINES || m == GL_POINTS) {
                glDrawArrays(m, first, static_cast<GLsizei>(n));
            } else {
                for (size_t i = 0; i < nseg; i++) g_batch.firsts[i] += first;
                glMultiDrawArrays(m, g_batch.firsts.data(), g_batch.counts.data(),
                                  static_cast<GLsizei>(nseg));
            }
        }
        if (fx_timed) psp_fx_timer_end();
    }
    if (g_sync_each) glFinish();
    else if (g_sync_mode == 4) glFlush();
    s.stage_n = 0;
    g_batch.firsts.clear();
    g_batch.counts.clear();
    g_expect_valid = false;
}

void ge_draw_flush() { flush_batch(); }
void ge_draw_list_end() { flush_batch(); if (g_sync_mode == 2) glFinish(); }
void ge_draw_set_fast(bool on) { flush_batch(); g_fast_enabled = on; std::fprintf(stderr, "[DRAW] fast path %s (runtime)\n", on ? "on" : "OFF"); }

static void setup_stream(Stream& s, size_t stride, bool is3d) {
    s.stride = stride;
    s.cap = RING_VERTS;
    s.write = 0;
    s.stage.resize(8192 * stride);
    glGenBuffers(1, &s.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s.cap * stride), nullptr, GL_STREAM_DRAW);
    const GLsizei st = static_cast<GLsizei>(stride);
    auto layout = [&](bool weights) {
        if (!is3d) {
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(16));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(24));
            glEnableVertexAttribArray(2);
        } else {
            // GeGpuVertex: pos3 @0, normal3 @12, uv2 @24, color4 @32, weights8 @36
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(12));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(24));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(32));
            glEnableVertexAttribArray(2);
            if (weights) {
                glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(36));
                glEnableVertexAttribArray(4);
                glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(52));
                glEnableVertexAttribArray(5);
            }
        }
    };
    glGenVertexArrays(1, &s.vao);
    glBindVertexArray(s.vao);
    layout(true);
    if (is3d) {
        glGenVertexArrays(1, &s.vao_plain);
        glBindVertexArray(s.vao_plain);
        layout(false);
    }
    glBindVertexArray(0);
}

static void teardown_stream(Stream& s) {
    if (s.vbo) glDeleteBuffers(1, &s.vbo);
    if (s.vao) glDeleteVertexArrays(1, &s.vao);
    if (s.vao_plain) glDeleteVertexArrays(1, &s.vao_plain);
    s = Stream{};
}

// ---- FX shader overrides (psp_fx_shader.h, FX_SHADERS.md) ----
// g_fx_on: -1 undecided, 0 no manifest (zero cost), 1 on.
static int g_fx_on = -1;
static GLuint g_fx_screen_tex = 0, g_fx_screen_fbo = 0;
static int g_fx_screen_w = 0, g_fx_screen_h = 0;

// Copy of the bound render target for the "screen" sampler (refraction /
// distortion). The batch about to draw is still empty, so every earlier draw
// is in it. psp_fx_shader.cpp calls this at most once per frame and target.
static unsigned fx_screen_copy(int* w, int* h) {
    const int W = 480 * g_rs, H = 272 * g_rs;
    if (!g_fx_screen_tex || g_fx_screen_w != W || g_fx_screen_h != H) {
        if (!g_fx_screen_tex) glGenTextures(1, &g_fx_screen_tex);
        glBindTexture(GL_TEXTURE_2D, g_fx_screen_tex);   // caller's unit (not unit 0)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!g_fx_screen_fbo) glGenFramebuffers(1, &g_fx_screen_fbo);
        GLint prev = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fx_screen_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fx_screen_tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
        g_fx_screen_w = W; g_fx_screen_h = H;
    }
    GLint cur = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)cur);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fx_screen_fbo);
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)cur);
    if (w) *w = W;
    if (h) *h = H;
    return g_fx_screen_tex;
}

// After the uber pipeline of a new batch: swap in the override program when
// the bound texture's pack id is in the manifest, then apply its blend/depth.
static void fx_batch(const GeState& state, GLuint fbo_tex, const GeXform3D* g) {
    if (g_fx_on < 0) g_fx_on = psp_fx_want_ids() ? 1 : 0;
    if (!g_fx_on || !state.texture_enable || fbo_tex) return;
    const uint64_t id = ge_texture_bound_pack_id();
    if (!id) return;
    PspFxDrawIn in{};
    in.tex_id = id;
    in.state = &state;
    in.xform = g;
    in.target_w = 480 * g_rs;
    in.target_h = 272 * g_rs;
    for (int i = 0; i < 4; i++) in.vp[i] = glc::vp[i];
    in.alpha_double = g_pipe_alpha_double;
    in.screen_copy = fx_screen_copy;
    PspFxDrawOut out{};
    if (!psp_fx_begin_batch(in, &out)) return;
    using psp_fx::Blend;
    switch (out.blend) {
    case Blend::Game: break;
    case Blend::Off: gl_blend_enable(false); break;
    default: {
        GLenum s = GL_SRC_ALPHA, d = GL_ONE;
        if (out.blend == Blend::AddOne) { s = GL_ONE; d = GL_ONE; }
        else if (out.blend == Blend::Alpha) { s = GL_SRC_ALPHA; d = GL_ONE_MINUS_SRC_ALPHA; }
        else if (out.blend == Blend::Premul) { s = GL_ONE; d = GL_ONE_MINUS_SRC_ALPHA; }
        else if (out.blend == Blend::Screen) { s = GL_ONE; d = GL_ONE_MINUS_SRC_COLOR; }
        gl_blend_enable(true);
        gl_blend_func(s, d);
        gl_blend_equation(GL_FUNC_ADD);
        break;
    }
    }
    using psp_fx::Depth;
    if (out.depth == Depth::NoWrite) gl_depth_mask(false);
    else if (out.depth == Depth::Write) gl_depth_mask(true);
    else if (out.depth == Depth::Off) gl_depth_enable(false);
    g_batch.fx_timed = out.timed;
}

// Make `k` the active batch (flushing a different one) with room for `need`
// more vertices in stream `s`, applying the GL pipeline when a new batch starts.
static void ensure_batch(const KeyBuf& k, Stream& s, GLuint vao, GLenum mode, size_t need,
                         uint8_t* rdram, const GeState& state, int prim_type,
                         GLuint fbo_tex, const Xf3D* xf) {
    if (!g_nobatch && g_batch.active && g_batch.s == &s && g_batch.key == k && s.stage_n + need <= s.cap)
        return;
    flush_batch();
    g_batch.key = k;
    g_batch.mode = mode;
    g_batch.s = &s;
    g_batch.vao = vao;
    g_batch.active = true;
    psp_prof::batches++;
    apply_gl_pipeline(rdram, state, prim_type, fbo_tex, xf != nullptr);
    GeXform3D g{};
    if (xf) {
        psp_prof::Scope ps(psp_prof::S_UNIFORM);
        g.world = xf->world; g.view = xf->view; g.proj = xf->proj;
        std::memcpy(g.uvxf, xf->uvxf, sizeof g.uvxf);
        g.skin_n = xf->skin_n; g.bones = xf->bones;
        g.lit = xf->lit; g.lflags = xf->lflags; g.nlights = xf->nl;
        std::memcpy(g.lc, xf->lc, sizeof g.lc);
        std::memcpy(g.lt, xf->lt, (size_t)xf->nl * 20 * sizeof(float));
        ge_shader_set_xform_3d(g);
    }
    if (g_fx_on != 0) fx_batch(state, fbo_tex, xf ? &g : nullptr);
}

static inline uint8_t* stage_reserve(Stream& s, size_t n) {
    const size_t need = (s.stage_n + n) * s.stride;
    if (s.stage.size() < need) s.stage.resize(std::max(need, s.stage.size() * 2));
    return s.stage.data() + s.stage_n * s.stride;
}

// RECTANGLES (sprites) in transform mode: ge_transform_vertices leaves positions
// in clip space (the GPU divides by w for triangles), but the rectangle
// expansion emits w=1 corners, so the perspective divide must happen here --
// otherwise every 3D sprite (Dissidia's glow / flash / impact particles) lands
// at clip coordinates, far outside the screen. Vertices behind the eye get
// w=0 and the expansion skips their rectangle.
extern uint32_t g_ge_cur_pc;
static void rect_clip_to_ndc(std::vector<DecodedVertex>& v, const GeState& state, int prim_type) {
    static const bool old_rect = std::getenv("PSPRECOMP_GE_RECT_OLD") != nullptr;  // A/B: pre-fix behaviour
    if (old_rect || prim_type != GE_PRIM_RECTANGLES || ge_vtype_through(state.vertex_type)) return;
    for (auto& d : v) {
        if (d.w == 1.0f) continue;  // NDC-direct fallback / already divided
        if (d.w > 1e-6f) {
            const float iw = 1.0f / d.w;
            d.pos[0] *= iw; d.pos[1] *= iw; d.pos[2] *= iw;
            d.w = 1.0f;
        } else {
            d.w = 0.0f;
        }
    }
}

static void fast_prim(uint8_t* rdram, const GeState& state, int prim_type, int count,
                      GLuint fbo_tex) {
    const bool through = ge_vtype_through(state.vertex_type);
    const GLenum mode = map_prim_type(prim_type);
    int n = count;
    if (prim_type == GE_PRIM_TRIANGLES) n -= n % 3;
    else if (prim_type == GE_PRIM_LINES || prim_type == GE_PRIM_RECTANGLES) n -= (n & 1);
    if (n <= 0) return;

    bool gpu = !through && prim_type != GE_PRIM_RECTANGLES && !g_cpu_xform
               && ge_gpu_transform_ok(state);
    if (gpu) {
        Xf3D xf;
        build_xf(state, xf);
        const int path = xf.skin_n > 0 ? 2 : 1;
        KeyBuf key;
        build_key(key, state, mode, path, prim_type, fbo_tex, &xf);
        ensure_batch(key, g_s3d, path == 2 ? g_s3d.vao : g_s3d.vao_plain, mode, (size_t)n,
                     rdram, state, prim_type, fbo_tex, &xf);
        // PSPRECOMP_FEP_LOG (psp_fep_log.h): the size / position this draw really has in the world; the batch's
        // texture is bound now, so its pack id is this draw's.
        static const bool fep_log = psp_fep_log_on();
        if (fep_log) psp_fep_log_draw(rdram, state, prim_type, n, ge_texture_bound_pack_id());
        uint8_t* dst = stage_reserve(g_s3d, (size_t)n);
        bool ok;
        {
            psp_prof::Scope ps(psp_prof::S_DECODE);
            ok = ge_decode_vertices_gpu(rdram, state, n, reinterpret_cast<GeGpuVertex*>(dst));
        }
        if (ok && g_verify && (mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN)) {
            static thread_local std::vector<DecodedVertex> ref;
            ge_decode_vertices(rdram, state, prim_type, n, ref);
            ge_transform_vertices(ref, state);
            auto ex = [&](const DecodedVertex& v) {
                return VerifyExpect{{v.pos[0], v.pos[1], v.pos[2], v.w}, {v.uv[0], v.uv[1]},
                                    {(float)v.color[0], (float)v.color[1], (float)v.color[2], (float)v.color[3]}};
            };
            g_expect.clear();
            if (ref.size() == (size_t)n) {
                // Transform feedback in GL_TRIANGLES mode captures strips/fans as
                // separate triangles (strip: odd triangles swap the first two).
                if (mode == GL_TRIANGLES) {
                    for (const auto& v : ref) g_expect.push_back(ex(v));
                } else if (mode == GL_TRIANGLE_STRIP) {
                    for (int t = 0; t + 2 < n; t++) {
                        const int a = (t & 1) ? t + 1 : t, b = (t & 1) ? t : t + 1;
                        g_expect.push_back(ex(ref[a])); g_expect.push_back(ex(ref[b])); g_expect.push_back(ex(ref[t + 2]));
                    }
                } else {
                    for (int t = 0; t + 2 < n; t++) {
                        g_expect.push_back(ex(ref[0])); g_expect.push_back(ex(ref[t + 1])); g_expect.push_back(ex(ref[t + 2]));
                    }
                }
            }
            g_expect_valid = !g_expect.empty();
            g_expect_lit = state.lighting_enable;
            g_expect_skin = xf.skin_n > 0;
        }
        if (ok) {
            g_batch.firsts.push_back(static_cast<GLint>(g_s3d.stage_n));
            g_batch.counts.push_back(n);
            g_s3d.stage_n += (size_t)n;
            psp_prof::verts += (uint64_t)n;
            g_has_drawn_prims = true;
            return;
        }
        gpu = false;   // unreadable guest memory: CPU path reproduces NULL-page semantics
    }

    // ---- CPU-transformed path ----
    static thread_local std::vector<DecodedVertex> decoded;
    static thread_local std::vector<PackedVertex> packed;
    {
        psp_prof::Scope ps(psp_prof::S_DECODE);
        ge_decode_vertices(rdram, state, prim_type, count, decoded);
    }
    if (decoded.empty()) return;
    {
        psp_prof::Scope ps(psp_prof::S_XFORM);
        ge_transform_vertices(decoded, state);
        rect_clip_to_ndc(decoded, state, prim_type);
    }
    packed.clear();
    {
        psp_prof::Scope ps(psp_prof::S_PACK);
        if (prim_type == GE_PRIM_RECTANGLES) {
            const int rect_count = count / 2;
            packed.reserve((size_t)rect_count * 6);
            for (int r = 0; r < rect_count; r++) {
                const int i0 = r * 2, i1 = r * 2 + 1;
                if (i1 >= static_cast<int>(decoded.size())) break;
                const auto& v0 = decoded[i0];
                const auto& v1 = decoded[i1];
                if (v0.w <= 0.0f || v1.w <= 0.0f) continue;
                PackedVertex c[4];
                c[0] = {{v0.pos[0], v0.pos[1], v0.pos[2], 1.0f}, {v0.uv[0], v0.uv[1]},
                        {v0.color[0], v0.color[1], v0.color[2], v0.color[3]}};
                c[1] = {{v1.pos[0], v0.pos[1], v0.pos[2], 1.0f}, {v1.uv[0], v0.uv[1]},
                        {v1.color[0], v1.color[1], v1.color[2], v1.color[3]}};
                c[2] = {{v1.pos[0], v1.pos[1], v1.pos[2], 1.0f}, {v1.uv[0], v1.uv[1]},
                        {v1.color[0], v1.color[1], v1.color[2], v1.color[3]}};
                c[3] = {{v0.pos[0], v1.pos[1], v1.pos[2], 1.0f}, {v0.uv[0], v1.uv[1]},
                        {v0.color[0], v0.color[1], v0.color[2], v0.color[3]}};
                packed.push_back(c[0]); packed.push_back(c[1]); packed.push_back(c[2]);
                packed.push_back(c[0]); packed.push_back(c[2]); packed.push_back(c[3]);
            }
        } else {
            size_t m = decoded.size();
            if (prim_type == GE_PRIM_TRIANGLES) m -= m % 3;
            else if (prim_type == GE_PRIM_LINES) m -= (m & 1);
            packed.resize(m);
            for (size_t i = 0; i < m; i++) {
                const auto& v = decoded[i];
                packed[i] = {{v.pos[0], v.pos[1], v.pos[2], v.w}, {v.uv[0], v.uv[1]},
                             {v.color[0], v.color[1], v.color[2], v.color[3]}};
            }
        }
    }
    if (packed.empty()) return;
    const size_t pn = packed.size();
    KeyBuf key;
    build_key(key, state, mode, 0, prim_type, fbo_tex, nullptr);
    ensure_batch(key, g_s2d, g_s2d.vao, mode, pn, rdram, state, prim_type, fbo_tex, nullptr);
    std::memcpy(stage_reserve(g_s2d, pn), packed.data(), pn * sizeof(PackedVertex));
    g_batch.firsts.push_back(static_cast<GLint>(g_s2d.stage_n));
    g_batch.counts.push_back(static_cast<GLsizei>(pn));
    g_s2d.stage_n += pn;
    psp_prof::verts += (uint64_t)pn;
    g_has_drawn_prims = true;
}


// PSPRECOMP_GE_SELFTEST=<iterations>: synthetic draws with random vertex
// formats (weights / normals / colours / texcoords / indices), random bones,
// matrices, materials and lights, run through the GPU path and compared with
// the CPU reference. Covers lighting + skinning variants the game's own
// draws never use (it has no normals).
static void verify_selftest(int iters) {
    uint8_t* ram = static_cast<uint8_t*>(mmap(nullptr, PSP_MEM_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (ram == MAP_FAILED) { std::fprintf(stderr, "[SELFTEST] mmap failed\\n"); return; }
    std::mt19937 rng(0xC0FFEE);
    auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto I = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
    const uint32_t vaddr = 0x08100000u, iaddr = 0x08300000u;
    const bool saved_nobatch = g_nobatch;
    g_nobatch = true;
    int ran = 0;
    for (int it = 0; it < iters; it++) {
        GeState st;
        st.reset();
        static const int cols[5] = {0, 4, 5, 6, 7};
        uint32_t vt = (uint32_t)I(0, 3)                      // tc
                    | ((uint32_t)cols[I(0, 4)] << 2)         // colour
                    | ((uint32_t)I(0, 3) << 5)               // normal
                    | ((uint32_t)I(1, 3) << 7)               // position
                    | ((uint32_t)I(0, 3) << 9)               // weights
                    | ((uint32_t)I(0, 3) << 11)              // index
                    | ((uint32_t)I(0, 7) << 14);             // weight count - 1
        st.vertex_type = vt;
        st.vertex_addr = vaddr;
        st.index_addr = iaddr;
        auto M = [&](float* m, int n, float lo, float hi) { for (int i = 0; i < n; i++) m[i] = U(lo, hi); };
        M(st.world_matrix, 12, -1.f, 1.f); M(st.view_matrix, 12, -1.f, 1.f);
        st.world_matrix[9] = U(-3, 3); st.world_matrix[10] = U(-3, 3); st.world_matrix[11] = U(-3, 3);
        st.view_matrix[9] = U(-3, 3); st.view_matrix[10] = U(-3, 3); st.view_matrix[11] = U(-3, 3);
        M(st.proj_matrix, 16, -0.05f, 0.05f);
        st.proj_matrix[0] = U(0.8f, 2.f); st.proj_matrix[5] = U(0.8f, 2.f);
        st.proj_matrix[10] = -1.1f; st.proj_matrix[11] = -1.f; st.proj_matrix[14] = -0.5f; st.proj_matrix[15] = 0.f;
        M(st.bone_matrix, 96, -1.f, 1.f);
        st.tex_scale_u = U(0.2f, 3.f); st.tex_scale_v = U(0.2f, 3.f);
        st.tex_offset_u = U(-1.f, 1.f); st.tex_offset_v = U(-1.f, 1.f);
        st.lighting_enable = I(0, 4) != 0;
        st.ambient_color = (uint32_t)I(0, 0xFFFFFF); st.ambient_alpha = (uint32_t)I(0, 255);
        st.material_emissive = (uint32_t)I(0, 0xFFFFFF); st.material_ambient = (uint32_t)I(0, 0xFFFFFF);
        st.material_diffuse = (uint32_t)I(0, 0xFFFFFF); st.material_alpha = (uint32_t)I(0, 255);
        st.material_update = (uint32_t)I(0, 7);
        for (int l = 0; l < 4; l++) {
            st.light_enable[l] = I(0, 3) != 0;
            st.light_type[l] = (uint32_t)(I(0, 2) << 8) | (uint32_t)I(0, 3);
            for (int k = 0; k < 3; k++) {
                st.light_pos[l][k] = U(-5.f, 5.f); st.light_dir[l][k] = U(-1.f, 1.f);
            }
            st.light_att[l][0] = U(0.3f, 2.f); st.light_att[l][1] = U(0.f, 0.5f); st.light_att[l][2] = U(0.f, 0.1f);
            st.light_spot_exp[l] = U(0.f, 8.f); st.light_spot_cut[l] = U(-1.f, 1.f);
            st.light_amb[l] = (uint32_t)I(0, 0xFFFFFF); st.light_dif[l] = (uint32_t)I(0, 0xFFFFFF);
        }
        st.viewport_x_scale = 240.f; st.viewport_y_scale = -136.f; st.viewport_z_scale = -32767.5f;
        st.viewport_x_center = 2048.f; st.viewport_y_center = 2048.f; st.viewport_z_center = 32767.5f;
        st.offset_x = 0x8000; st.offset_y = 0x8000;
        if (!ge_gpu_transform_ok(st)) continue;

        const int stride = ge_vertex_stride(vt);
        const int idx_type = ge_vtype_idx(vt);
        const int pool = I(3, 40);
        float* vf = reinterpret_cast<float*>(ram + (vaddr & PSP_ADDR_MASK));
        for (size_t i = 0; i < (size_t)pool * stride / 4 + 8; i++) vf[i] = U(-2.f, 2.f);
        const int prim_pick = I(0, 2);
        const int prim = prim_pick == 0 ? GE_PRIM_TRIANGLES : prim_pick == 1 ? GE_PRIM_TRIANGLE_STRIP : GE_PRIM_TRIANGLE_FAN;
        int n = prim == GE_PRIM_TRIANGLES ? 3 * I(1, 12) : I(3, 30);
        if (idx_type == GE_VTYPE_IDX_NONE) n = std::min(n, pool);
        if (idx_type != GE_VTYPE_IDX_NONE) {
            uint8_t* ip = ram + (iaddr & PSP_ADDR_MASK);
            for (int i = 0; i < n; i++) {
                const uint32_t v = (uint32_t)I(0, pool - 1);
                if (idx_type == GE_VTYPE_IDX_8BIT) ip[i] = (uint8_t)v;
                else if (idx_type == GE_VTYPE_IDX_16BIT) std::memcpy(ip + i * 2, &v, 2);
                else std::memcpy(ip + i * 4, &v, 4);
            }
        }
        if (n < 3) continue;
        fast_prim(ram, st, prim, n, 0);
        flush_batch();
        ran++;
    }
    g_nobatch = saved_nobatch;
    glc::invalidate();
    ge_shader_invalidate_cache();
    ge_texture_invalidate_bind_cache();
    std::fprintf(stderr, "[SELFTEST] %d synthetic draws run\\n", ran);
    verify_report("selftest");
    munmap(ram, PSP_MEM_SIZE);
}

// ---- Public API ----

void ge_draw_init() {
    // Check screenshot env var
    const char* ss_env =
        std::getenv("PSPRECOMP_SCREENSHOT");
    if (ss_env && ss_env[0]) {
        g_screenshot_enabled = true;
        std::strncpy(g_screenshot_path, ss_env,
                     sizeof(g_screenshot_path) - 1);
    }

    if (const char* rs = std::getenv("PSPRECOMP_RENDER_SCALE")) {
        int v = std::atoi(rs);
        if (!std::strcmp(rs, "auto")) {
            // PSPRECOMP_RENDER_SCALE=auto: nearest whole multiple of the
            // drawable height / 272, clamped to 1..3 (1280x800 Steam Deck
            // panel -> 3 = 1440x816; a 960x544 window -> 2). Capped at 3 so
            // a Retina/4K window does not silently allocate 8 huge targets.
            int dw = 0, dh = 0;
            if (SDL_Window* w = psp_get_sdl_window()) SDL_GL_GetDrawableSize(w, &dw, &dh);
            v = dh > 0 ? (int)((dh + 136) / 272) : 1;
            v = v < 1 ? 1 : v > 3 ? 3 : v;
            std::fprintf(stderr, "[DRAW] render scale auto: drawable %dx%d -> %d\n", dw, dh, v);
        }
        g_rs = v < 1 ? 1 : v > 8 ? 8 : v;
        GLint maxtex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
        while (g_rs > 1 && (480 * g_rs > maxtex || 272 * g_rs > maxtex)) g_rs--;
    }
    if (g_rs > 1) {
        // Keep 1-pixel PSP lines/points the same visual size (best effort:
        // core profiles may clamp wide lines to 1).
        glLineWidth(float(g_rs));
        glPointSize(float(g_rs));
        while (glGetError() != GL_NO_ERROR) {}
    }
    std::fprintf(stderr, "[DRAW] render scale %dx (targets %dx%d)\n", g_rs, 480 * g_rs, 272 * g_rs);

    // Liveness-present budget (no-flip safety net). Env-overridable.
    const char* stale_env = std::getenv("PSPRECOMP_PRESENT_STALE_MS");
    if (stale_env && stale_env[0]) {
        int v = std::atoi(stale_env);
        if (v > 0) g_present_stale_ms = v;
    }
    g_last_present = std::chrono::steady_clock::now();

    if (const char* nr = std::getenv("PSPRECOMP_NO_RASTER")) {
        g_no_raster_mode = std::atoi(nr);
        if (const char* af = std::getenv("PSPRECOMP_NO_RASTER_AFTER")) g_no_raster_after = std::atof(af);
        if (g_no_raster_after <= 0.0) {
            g_no_raster = g_no_raster_mode;
            if (g_no_raster == 1) glEnable(GL_RASTERIZER_DISCARD);
        }
        std::fprintf(stderr, "[DRAW] PSPRECOMP_NO_RASTER=%d after %.0fs\n", g_no_raster_mode, g_no_raster_after);
    }
    // FBO targets are created lazily per guest framebuffer address by
    // ge_draw_select_target. No target is pre-created here.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Create VAO + VBO
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    // Vertex attributes for PackedVertex layout
    // location 0: position (3 floats, offset 0)
    glVertexAttribPointer(
        0, 4, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // location 1: texcoord (2 floats, offset 16)
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(16));
    glEnableVertexAttribArray(1);

    // location 2: color (4 bytes normalized, offset 24)
    glVertexAttribPointer(
        2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    // Init shader
    ge_shader_init();

    // Batched fast path (see fast_prim): ON by default on every platform.
    // PSPRECOMP_GE_FAST=0 (or the older PSPRECOMP_GE_LEGACY=1) selects the
    // legacy per-PRIM path with CPU skinning. The fast path needs only GL 3.3
    // core (no persistent mapping, SSBOs, compute or GLSL > 330), so macOS's
    // 4.1 core context is supported; see DEBUGGING.md "GE fast path" for the
    // fast-vs-legacy A/B tool (PSPRECOMP_GE_AB) and GL error polling
    // (PSPRECOMP_GL_STATS=2) to run on a new driver.
    {
        const char* fe = std::getenv("PSPRECOMP_GE_FAST");
        const bool fast_off = fe && (fe[0] == '0' || fe[0] == 'n' || fe[0] == 'N' || fe[0] == 'f' || fe[0] == 'F');
        g_fast_enabled = !fast_off && !std::getenv("PSPRECOMP_GE_LEGACY");
    }
    g_ab = std::getenv("PSPRECOMP_GE_AB") != nullptr;
    if (g_ab) std::fprintf(stderr, "[AB] fast-vs-legacy dual rendering on (compare logged per present)\n");
    g_cpu_xform = std::getenv("PSPRECOMP_GE_CPU3D") != nullptr;
    g_nobatch = std::getenv("PSPRECOMP_GE_NOBATCH") != nullptr;
    if (const char* sm = std::getenv("PSPRECOMP_GE_SYNC")) {
        g_sync_mode = !std::strcmp(sm, "flush") ? 4 : std::atoi(sm);
        if (g_sync_mode <= 0) g_sync_mode = 1;
        g_sync_each = g_sync_mode == 1;
        std::fprintf(stderr, "[DRAW] GE sync mode %d (%s)\n", g_sync_mode,
                     g_sync_mode == 1 ? "glFinish per batch" : g_sync_mode == 2 ? "glFinish per list"
                     : g_sync_mode == 3 ? "glFinish per present" : "glFlush per batch");
    }
    if (const char* rm = std::getenv("PSPRECOMP_GE_RING")) {
        if (!std::strcmp(rm, "map")) g_ring_mode = 1;
        else if (!std::strcmp(rm, "sub")) g_ring_mode = 0;

    }
    g_verify = std::getenv("PSPRECOMP_GE_VERIFY") != nullptr;
    if (g_verify) {
        g_nobatch = true;   // one PRIM per draw so each capture maps to one expectation
        glGenBuffers(1, &g_tf_buf);
        glBindBuffer(GL_COPY_WRITE_BUFFER, g_tf_buf);
        glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(70000 * 10 * sizeof(float)), nullptr, GL_STREAM_READ);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        std::fprintf(stderr, "[VERIFY] GPU vertex stage verification on (one PRIM per draw)\n");
    }

    g_debug_env = std::getenv("PSPRECOMP_DRAW_TRACE") || std::getenv("PSPRECOMP_3D_DIAG")
               || std::getenv("PSPRECOMP_3D_DIAG_BIG");
    setup_stream(g_s2d, sizeof(PackedVertex), false);
    setup_stream(g_s3d, sizeof(GeGpuVertex), true);
    glc::invalidate();
    ge_shader_invalidate_cache();
    std::fprintf(stderr, "[DRAW] fast path %s%s%s\n", g_fast_enabled ? "on" : "OFF (legacy)",
                 g_cpu_xform ? ", CPU 3D transform" : ", GPU 3D transform",
                 g_debug_env ? ", debug env -> legacy draws" : "");
    if (const char* stn = std::getenv("PSPRECOMP_GE_SELFTEST")) {
        if (!g_verify) {
            g_verify = true; g_nobatch = true;
            glGenBuffers(1, &g_tf_buf);
            glBindBuffer(GL_COPY_WRITE_BUFFER, g_tf_buf);
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(70000 * 10 * sizeof(float)), nullptr, GL_STREAM_READ);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }
        verify_selftest(std::max(1, std::atoi(stn)));
    }

    g_frame_counter = 0;
    g_draw_ready.store(true, std::memory_order_release);

    std::fprintf(stderr,
        "[DRAW] Draw infrastructure initialized\n");
}

void ge_draw_shutdown() {
    g_draw_ready.store(false, std::memory_order_release);
    ge_draw_flush();
    // Auto-capture final frame if any PRIMs were rendered and no screenshot taken yet
    if (g_has_drawn_prims && !g_screenshot_done) {
        std::vector<uint8_t> flipped;
        int cw = 480, chh = 272;
        read_fbo_native(front_fbo_id(), flipped, &cw, &chh);
        const char* out_path = (g_screenshot_path[0] != '\0')
            ? g_screenshot_path : "frame.tga";
        write_tga(out_path, flipped.data(), cw, chh);
        g_screenshot_done = true;
        std::fprintf(stderr, "[DRAW] Auto-capture: %s\n", out_path);
    }

    flush_batch();
    if (g_verify) verify_report("final");
    teardown_stream(g_s2d);
    teardown_stream(g_s3d);
    psp_fx_shutdown();
    if (g_fx_screen_tex) { glDeleteTextures(1, &g_fx_screen_tex); g_fx_screen_tex = 0; }
    if (g_fx_screen_fbo) { glDeleteFramebuffers(1, &g_fx_screen_fbo); g_fx_screen_fbo = 0; }
    ge_shader_shutdown();

    if (g_vbo) {
        glDeleteBuffers(1, &g_vbo);
        g_vbo = 0;
    }
    if (g_vao) {
        glDeleteVertexArrays(1, &g_vao);
        g_vao = 0;
    }
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (t.depth_rb) glDeleteRenderbuffers(1, &t.depth_rb);
        if (t.color_tex) glDeleteTextures(1, &t.color_tex);
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        t = GeTarget{};
    }
    g_current_target = -1;
    g_front_target = -1;

    std::fprintf(stderr,
        "[DRAW] Draw infrastructure shutdown\n");
}

void ge_draw_begin_list() {
    // Bind the FBO for the list's current render-target address (FRAMEBUFPTR).
    // Geometry and clears in this list now target the per-address FBO, so a
    // clear to address X cannot wipe geometry on address Y (issue #59).
    ge_draw_select_target(ge_get_state().framebuf_ptr);
    if (int a = g_dyn_armed.exchange(0)) g_dyn_trace_n.store(a);
    if (g_dyn_trace_n.load() > 0)
        std::fprintf(stderr, "[DT] ---- BEGIN_LIST fb=0x%08X\n", ge_get_state().framebuf_ptr);
    // Full-buffer default; the precise PSP viewport (scale/offset + depth
    // range) is applied per-PRIM in ge_draw_prim from the live registers,
    // which may be set after begin_list (issue #23).
    gl_viewport(0, 0, PSP_FB_WIDTH * g_rs, PSP_FB_HEIGHT * g_rs);
    g_frame_counter++;
}

void ge_draw_end_list() {
    ge_draw_flush();
    // Mark the front buffer dirty so the liveness net can publish content
    // even for a title that draws but never calls sceDisplaySetFrameBuf.
    ge_draw_mark_dirty();
}

static void ge_draw_prim_one(uint8_t* rdram, int prim_type, int count);

void ge_draw_prim(uint8_t* rdram, int prim_type, int count) {
    if (!g_ab || !g_fast_enabled || g_current_target < 0 || !g_targets[g_current_target].ab_fbo) {
        ge_draw_prim_one(rdram, prim_type, count);
        return;
    }
    ge_draw_prim_one(rdram, prim_type, count);          // fast path -> real target
    flush_batch();
    GeTarget& t = g_targets[g_current_target];
    glBindFramebuffer(GL_FRAMEBUFFER, t.ab_fbo);
    g_ab_shadow = true;
    static const bool fault = std::getenv("PSPRECOMP_AB_FAULT") != nullptr;   // harness self-check
    static unsigned fault_n = 0;
    if (!(fault && (++fault_n % 7) == 0))
        ge_draw_prim_one(rdram, prim_type, count);      // legacy path -> twin
    g_ab_shadow = false;
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
}

static void ge_draw_prim_one(
    uint8_t* rdram,
    int prim_type,
    int count
) {
    {
        // PSPRECOMP_SKIN_DIAG=<n>[,<from>]: for the first n skinned draws (after prim #from) log vtype, weight count,
        // index count and for each matrix slot in use a short fingerprint plus how many PRIMs ago it was last uploaded.
        extern uint32_t g_ge_bone_slot_last[8], g_ge_skin_prim_seq;
        static const char* sd = std::getenv("PSPRECOMP_SKIN_DIAG");
        static int sd_n = sd ? std::atoi(sd) : 0;
        static int sd_from = (sd && std::strchr(sd, ',')) ? std::atoi(std::strchr(sd, ',') + 1) : 0;
        const GeState& s0 = ge_get_state();
        if (sd_n > 0 && g_ge_skin_prim_seq >= (uint32_t)sd_from && ge_vtype_weight(s0.vertex_type) != 0 && !ge_vtype_through(s0.vertex_type)) {
            sd_n--;
            const int wc = ((s0.vertex_type >> GE_VTYPE_WEIGHTCOUNT_SHIFT) & GE_VTYPE_WEIGHTCOUNT_MASK) + 1;
            std::fprintf(stderr, "[SKIN] prim#%u vt=%08X nw=%d wt=%d stride=%d type=%d count=%d idx=%08X bonenum=%d |", g_ge_skin_prim_seq,
                         s0.vertex_type, wc, ge_vtype_weight(s0.vertex_type), ge_vertex_stride(s0.vertex_type), prim_type, count, s0.index_addr, s0.bone_mtx_num);
            for (int k = 0; k < std::min(wc, 8); k++) {
                const float* m = &s0.bone_matrix[k * 12];
                uint32_t h = 2166136261u; for (int i = 0; i < 12; i++) { uint32_t u; std::memcpy(&u, &m[i], 4); h = (h ^ u) * 16777619u; }
                std::fprintf(stderr, " s%d:%08X(age %u,t=%.2f,%.2f,%.2f)", k, h, g_ge_skin_prim_seq - g_ge_bone_slot_last[k], m[9], m[10], m[11]);
            }
            std::fprintf(stderr, "\n");
            {   // raw first vertices (as the GE would read them)
                const int stv = ge_vertex_stride(s0.vertex_type);
                const uint32_t va = s0.vertex_addr & 0x07FFFFFFu;
                std::fprintf(stderr, "[SKIN]   va=%08X raw:", s0.vertex_addr);
                for (int v = 0; v < 3; v++) { std::fprintf(stderr, " "); for (int i = 0; i < stv; i++) std::fprintf(stderr, "%02X", rdram[va + v * stv + i]); }
                std::fprintf(stderr, "\n");
            }
        }
        g_ge_skin_prim_seq++;
    }
    // Phase 11.4 / Plan 02 Task 2 — alternate-path GE PRIM investigation.
    //
    // Even when count <= 0 we tally [GE_PRIM_DETAIL] before the early-return
    // so the totals match the GE_SUMMARY total_prims figure exactly. The
    // counters here are NEVER rate-limited (they are scalar increments, no
    // I/O per call); the dump happens once in ge_prim_detail_atexit_summary.
    //
    // Tertiary finding from Plan 01 (DIAGNOSTIC §"Raw Log Excerpt"): the
    // GE pipeline processes 243 PRIM commands but only 57 [DRAW_PRIM] log
    // lines reach stderr (all clear=1). The pre-existing [DRAW_PRIM] log
    // rate-limit (first 50 + every 25th + first 5 non-clear) hides the
    // breakdown. This block counts EVERY call by (prim_type, clear) bucket.
    // (Counter definitions moved to file scope -- g_prim_* above -- so the
    // debug socket's I command can snapshot them via ge_draw_get_stats.)
    static bool prim_detail_atexit_installed = false;
    if (!prim_detail_atexit_installed) {
        std::atexit([]() {
            std::fprintf(stderr,
                "[GE_PRIM_DETAIL_SUMMARY] total_observed=%d zero_count=%d\n",
                g_prim_total_observed, g_prim_type_zero_count);
            int real_ns = 0, sprite_nc = 0, clears = 0;
            for (int t = 0; t < 8; ++t) {
                int normal = g_prim_type_count[t];
                int clear = g_prim_type_count_clear[t];
                clears += clear;
                if (t == 6) sprite_nc += normal; else real_ns += normal;
                if (normal == 0 && clear == 0) continue;
                std::fprintf(stderr,
                    "[GE_PRIM_DETAIL] type=%d normal=%d clear=%d total=%d\n",
                    t, normal, clear, normal + clear);
            }
            // Canonical one-line verdict for the verification harness.
            std::fprintf(stderr,
                "[GE_GEOM_VERDICT] real_nonsprite=%d sprite_nonclear=%d "
                "clears=%d => %s\n",
                real_ns, sprite_nc, clears,
                real_ns > 0 ? "GRAPHICS" : "NO-GRAPHICS");
        });
        prim_detail_atexit_installed = true;
    }

    // Self-test for the verification harness (scripts/verify_geometry.sh):
    // PSPRECOMP_GEOM_SELFTEST=1 synthesizes ONE real type-4 (TRIANGLE_STRIP,
    // non-clear, non-sprite) draw through the exact counter + sentinel path, so
    // the GRAPHICS detection can be proven end-to-end without the real asset bug
    // fixed. Proves the detector is not a constant-false oracle. No GL side
    // effects; runs once.
    static bool selftest_done = false;
    if (!selftest_done && std::getenv("PSPRECOMP_GEOM_SELFTEST")) {
        selftest_done = true;
        g_prim_type_count[GE_PRIM_TRIANGLE_STRIP]++;
        std::fprintf(stderr,
            "[GE_GEOM_REAL_DRAW] FIRST real geometry draw: "
            "type=%d count=%d (success-bar (b) MET) [SELFTEST]\n",
            GE_PRIM_TRIANGLE_STRIP, 3);
        std::fflush(stderr);
    }

    psp_prof::Scope prof_prim(psp_prof::S_PRIM);
    g_prim_total_observed++;
    if (count <= 0) {
        g_prim_type_zero_count++;
        return;
    }

    static int prim_call_count = 0;
    prim_call_count++;

    const GeState& state = ge_get_state();
    {
        int t_idx = (prim_type >= 0 && prim_type < 8) ? prim_type : 7;
        if (state.clear_mode) {
            g_prim_type_count_clear[t_idx]++;
        } else {
            g_prim_type_count[t_idx]++;
        }

        // [GE_GEOM_VERDICT] live verification sentinels — the definitive answer
        // to "are graphics being set up?". These print DURING the run and are
        // flushed immediately, so they survive ANY exit path (SIGTERM, SIGKILL,
        // or a hung shutdown) — unlike the atexit [GE_PRIM_DETAIL_SUMMARY],
        // which only fires on a clean return from main(). Pure diagnostic
        // output: changes no game behavior.
        //
        // Success-bar half (b) == ONE real draw with type!=6 (non-sprite) AND
        // clear==0 (non-clear) AND normal>0. The line below appears in the log
        // if and only if that condition is ever met.
        static bool geom_first_real_logged = false;
        if (!state.clear_mode && t_idx != 6 && !geom_first_real_logged) {
            geom_first_real_logged = true;
            std::fprintf(stderr,
                "[GE_GEOM_REAL_DRAW] FIRST real geometry draw: "
                "type=%d count=%d (success-bar (b) MET)\n",
                prim_type, count);
            std::fflush(stderr);
        }

        // Periodic heartbeat: a killed or hung run still leaves a current tally
        // in the log (every 256 PRIMs observed). real_nonsprite>0 == graphics.
        // Off by default since 2026-10-22 (PSPRECOMP_DRAW_LOG=1): with fflush this and the [DRAW_PRIM] lines below
        // were ~4,000 unbuffered stderr writes/s in battle (piped through run.sh's tee to the terminal), which
        // stalled the GE thread on busy frames.
        static const bool draw_log = [] { const char* v = std::getenv("PSPRECOMP_DRAW_LOG"); return v && *v && *v != '0'; }();
        if (draw_log && g_prim_total_observed % 256 == 0) {
            int real_ns = 0, sprite_nc = 0, clears = 0;
            for (int t = 0; t < 8; ++t) {
                clears += g_prim_type_count_clear[t];
                if (t == 6) sprite_nc += g_prim_type_count[t];
                else real_ns += g_prim_type_count[t];
            }
            std::fprintf(stderr,
                "[GE_GEOM_HEARTBEAT] prims=%d real_nonsprite=%d "
                "sprite_nonclear=%d clears=%d\n",
                g_prim_total_observed, real_ns, sprite_nc, clears);
            std::fflush(stderr);
        }
    }

    // Log a wider window so non-clear PRIMs in the middle of a run are
    // not silently dropped. Also log every 25th PRIM after the first 50
    // and log the first non-clear PRIM unconditionally.
    static int non_clear_logged = 0;
    bool is_non_clear_first = (!state.clear_mode && non_clear_logged < 5);
    static const bool draw_log_every = [] { const char* v = std::getenv("PSPRECOMP_DRAW_LOG"); return v && *v && *v != '0'; }();
    if (prim_call_count <= 50
        || (draw_log_every && prim_call_count % 25 == 0)
        || is_non_clear_first) {
        std::fprintf(stderr,
            "[DRAW_PRIM] #%d type=%d count=%d clear=%d "
            "vtype=0x%06X vaddr=0x%08X\n",
            prim_call_count, prim_type, count,
            state.clear_mode ? 1 : 0,
            state.vertex_type, state.vertex_addr);
        if (is_non_clear_first) non_clear_logged++;
    }
    // PSPRECOMP_3D_SKIP_VTYPE=<hex>[,<hex>...]: debug aid, drop non-clear draws
    // with these exact vertex types (isolate which layer produces an artefact).
    if (!state.clear_mode) {
        static const char* skip_env = std::getenv("PSPRECOMP_3D_SKIP_VTYPE");
        if (skip_env) {
            char buf[256]; std::snprintf(buf, sizeof buf, "%s", skip_env);
            for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ","))
                if (static_cast<uint32_t>(std::strtoul(tok, nullptr, 16)) == state.vertex_type) return;
        }
    }
    // Framebuffer-as-texture lookup (VRAM texture address == a pool target).
    GLuint fbo_tex = 0;
    if (state.texture_enable) {
        uint32_t ta = state.tex_addr[0] |
                      (((state.tex_bufw[0] >> 16) & 0xFFu) << 24);
        if ((ta & 0x07E00000u) == 0x04000000u) {
            int tw = 0, th = 0;
            fbo_tex = ge_draw_find_target_texture(ge_fb_key(ta), &tw, &th);
        }
    }
    // PSPRECOMP_DRAW_TRACE=1: GE state behind each non-clear PRIM (first 64).
    // PSPRECOMP_DRAW_TRACE_FROM=<draw#>: only trace draws numbered >= N (to
    // reach the battle), PSPRECOMP_DRAW_TRACE_MAX=<n>: how many to trace.
    static int draw_trace_n = 0;
    static const int trace_from = std::getenv("PSPRECOMP_DRAW_TRACE_FROM")
        ? std::atoi(std::getenv("PSPRECOMP_DRAW_TRACE_FROM")) : 0;
    static const int trace_max = std::getenv("PSPRECOMP_DRAW_TRACE_MAX")
        ? std::atoi(std::getenv("PSPRECOMP_DRAW_TRACE_MAX")) : 100000;
    static const bool draw_trace_env = std::getenv("PSPRECOMP_DRAW_TRACE") != nullptr;
    const bool draw_trace = draw_trace_env && prim_call_count >= trace_from;
    if (draw_trace && !state.clear_mode && draw_trace_n < trace_max) {
        draw_trace_n++;
        std::fprintf(stderr,
            "[DRAW_TRACE] #%d type=%d count=%d vtype=0x%06X tex=%d fmt=%u addr=0x%08X size=0x%X clutfmt=0x%X clutaddr=0x%08X blend=%d atest=%d(0x%X) fb=0x%08X tfunc=%u rgba=%d dbl=%d mask=%06X/%02X vp=%u%s\n",
            prim_call_count, prim_type, count, state.vertex_type,
            state.texture_enable ? 1 : 0, (unsigned)state.tex_format, state.tex_addr[0], (unsigned)state.tex_size[0],
            (unsigned)state.clut_format, (unsigned)((state.clut_addr & 0xFFFFFF) | ((state.clut_addr_upper << 8) & 0x0F000000)), state.alpha_blend_enable ? 1 : 0,
            state.alpha_test_enable ? 1 : 0, (unsigned)state.alpha_test, state.framebuf_ptr, (unsigned)state.tex_func, state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0,
            state.mask_rgb, state.mask_alpha & 0xFF, (unsigned)state.viewport_x_scale,
            fbo_tex ? " fbotex=1" : "");
    }

    static const bool draw_trace_mat_env = std::getenv("PSPRECOMP_DRAW_TRACE_MAT") != nullptr;
    if (!state.clear_mode && prim_type == 3 && count > 30 && draw_trace_mat_env) {
        static int mat_n = 0;
        if ((mat_n++ % 3000) == 0 && mat_n < 200000) {
            std::fprintf(stderr, "[MAT] #%d W:", prim_call_count);
            for (int i = 0; i < 12; i++) std::fprintf(stderr, " %g", state.world_matrix[i]);
            std::fprintf(stderr, "\n[MAT]   V:");
            for (int i = 0; i < 12; i++) std::fprintf(stderr, " %g", state.view_matrix[i]);
            std::fprintf(stderr, "\n[MAT]   P:");
            for (int i = 0; i < 16; i++) std::fprintf(stderr, " %g", state.proj_matrix[i]);
            std::fprintf(stderr, "\n[MAT]   vp scale %g %g %g ctr %g %g %g\n", state.viewport_x_scale, state.viewport_y_scale, state.viewport_z_scale, state.viewport_x_center, state.viewport_y_center, state.viewport_z_center);
        }
    }
    // Clear mode: use glClear instead of drawing
    if (state.clear_mode) {
        flush_batch();
        psp_prof::Scope prof_clear(psp_prof::S_CLEAR);
        psp_prof::clears++;
        // Decode first vertex for clear color
        std::vector<DecodedVertex> clear_verts;
        ge_decode_vertices(
            rdram, state, prim_type, count, clear_verts);
        if (!clear_verts.empty()) {
            float r = clear_verts[0].color[0] / 255.0f;
            float g = clear_verts[0].color[1] / 255.0f;
            float b = clear_verts[0].color[2] / 255.0f;
            float a = clear_verts[0].color[3] / 255.0f;
            if (prim_call_count <= 20 || draw_trace) {
                std::fprintf(stderr,
                    "[DRAW_CLEAR] #%d color=(%0.2f,%0.2f,%0.2f,%0.2f) "
                    "flags=0x%X verts=%zu fb=0x%08X x[%.0f,%.0f] y[%.0f,%.0f]\n",
                    prim_call_count, r, g, b, a, state.clear_flags,
                    clear_verts.size(), state.framebuf_ptr,
                    clear_verts[0].pos[0], clear_verts.back().pos[0], clear_verts[0].pos[1], clear_verts.back().pos[1]);
            }
            glClearColor(r, g, b, a);
            if (prim_call_count <= 5) {
                std::fprintf(stderr,
                    "[DRAW_CLEAR] glClearColor(%.3f, %.3f, %.3f, %.3f) "
                    "flags=0x%X\n",
                    r, g, b, a, state.clear_flags);
            }
        }

        GLbitfield clear_bits = 0;
        if (state.clear_flags & 1)
            clear_bits |= GL_COLOR_BUFFER_BIT;
        if (state.clear_flags & 2)
            clear_bits |= GL_STENCIL_BUFFER_BIT;
        if (state.clear_flags & 4)
            clear_bits |= GL_DEPTH_BUFFER_BIT;
        if (clear_bits == 0)
            clear_bits = GL_COLOR_BUFFER_BIT
                         | GL_DEPTH_BUFFER_BIT;

        // Clear depth: the PSP writes the clear rectangle's Z into the depth
        // buffer. GL's window depth uses the same 0..65535 encoding as the
        // PSP (see map_depth_func), so clear to that Z directly. (Falls back
        // to the active range's far value if the clear verts are unavailable.)
        if (clear_bits & GL_DEPTH_BUFFER_BIT) {
            double cz;
            if (!clear_verts.empty()) {
                cz = clear_verts[0].pos[2] / 65535.0;
            } else {
                cz = compute_state_viewport_depth(state).far_z;
            }
            if (cz < 0.0) cz = 0.0;
            if (cz > 1.0) cz = 1.0;
            glClearDepth(cz);
        }

        // Temporarily disable depth mask restriction. In clear mode the PSP
        // ignores MASKRGB/MASKALPHA (the clear flags pick the channels), so
        // reset the colour write mask a previous draw may have narrowed.
        gl_depth_mask(true);
        gl_color_mask(true, true, true, true);
        gl_stencil_mask(0xFF);
        gl_stencil_enable(false);
        if (clear_bits & GL_STENCIL_BUFFER_BIT) {
            // The clear writes the clear colour's alpha into the stencil/alpha plane.
            const int sv = clear_verts.empty() ? 0 : (int)clear_verts[0].color[3];
            glClearStencil(sv & 0xFF);
        }
        glClear(clear_bits);
        if (g_dyn_trace_n.load() > 0) {
            g_dyn_trace_n--;
            std::fprintf(stderr, "[DT] #%d CLEAR flags=%X bits=%X fb=0x%08X z=%.1f n=%d\n", prim_call_count,
                state.clear_flags, (unsigned)clear_bits, state.framebuf_ptr,
                clear_verts.empty() ? -1.0f : clear_verts[0].pos[2], count);
        }
        g_has_drawn_prims = true;  // Clear counts as drawing
        return;
    }

    if (g_fast_enabled && !g_ab_shadow && !g_debug_env && g_dyn_trace_n.load() <= 0) {
        psp_prof::draws++;
        fast_prim(rdram, state, prim_type, count, fbo_tex);
        return;
    }
    flush_batch();   // the immediate legacy path below must not be reordered

    // Decode vertices
    std::vector<DecodedVertex> decoded;
    {
        psp_prof::Scope ps(psp_prof::S_DECODE);
        ge_decode_vertices(
            rdram, state, prim_type, count, decoded);
    }
    if (decoded.empty()) return;

    {   // PSPRECOMP_CAM_LOG=1: log camera pose each time the view matrix changes
        static const bool cam_on = std::getenv("PSPRECOMP_CAM_LOG") != nullptr;
        static float last_v[12] = {};
        static int cam_n = 0;
        if (cam_on && !ge_vtype_through(state.vertex_type) && cam_n < 40000 &&
            std::memcmp(last_v, state.view_matrix, sizeof last_v) != 0) {
            std::memcpy(last_v, state.view_matrix, sizeof last_v);
            const float* M = state.view_matrix;
            float px = -(M[9]*M[0] + M[10]*M[1] + M[11]*M[2]);
            float py = -(M[9]*M[3] + M[10]*M[4] + M[11]*M[5]);
            float pz = -(M[9]*M[6] + M[10]*M[7] + M[11]*M[8]);
            static const auto t0 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            cam_n++;
            std::fprintf(stderr, "[CAM] t=%.0fms pos=(%.1f,%.1f,%.1f) zaxis=(%.3f,%.3f,%.3f) n=%d\n",
                ms, px, py, pz, M[2], M[5], M[8], count);
        }
    }

    // Transform
    {
        psp_prof::Scope ps(psp_prof::S_XFORM);
        ge_transform_vertices(decoded, state);
        rect_clip_to_ndc(decoded, state, prim_type);
    }
    static int vtx_trace_n = 0;
    if (draw_trace && vtx_trace_n < trace_max) {
        vtx_trace_n++;
        if (!ge_vtype_through(state.vertex_type)) {
            std::fprintf(stderr, "[MTX] #%d world=", prim_call_count);
            for (int k = 0; k < 12; k++) std::fprintf(stderr, "%g ", state.world_matrix[k]);
            std::fprintf(stderr, "\n[MTX] #%d view=", prim_call_count);
            for (int k = 0; k < 12; k++) std::fprintf(stderr, "%g ", state.view_matrix[k]);
            std::fprintf(stderr, "\n[MTX] #%d proj=", prim_call_count);
            for (int k = 0; k < 16; k++) std::fprintf(stderr, "%g ", state.proj_matrix[k]);
            std::fprintf(stderr, "\n[MTX] #%d vpz=%g/%g zrange=%u..%u cullface=%u\n", prim_call_count,
                state.viewport_z_scale, state.viewport_z_center, state.min_z, state.max_z, state.cull_face);
        }
        {
            uint32_t va = state.vertex_addr & 0x07FFFFFFu;
            std::fprintf(stderr, "[VTX_RAW] #%d vaddr=0x%08X stride=%d bytes=", prim_call_count, state.vertex_addr, ge_vertex_stride(state.vertex_type));
            for (int b = 0; b < 32; b++) std::fprintf(stderr, "%02X%s", rdram[va + b], (b % 4 == 3) ? " " : "");
            std::fprintf(stderr, "\n");
        }
        for (size_t i = 0; i < decoded.size() && i < 4; i++) {
            const DecodedVertex& v = decoded[i];
            std::fprintf(stderr, "[VTX_TRACE] #%d v%zu pos=(%.3f,%.3f,%.3f,w=%.3f) uv=(%.3f,%.3f) rgba=%02X%02X%02X%02X through=%d vp=(%.0f,%.0f,%.0f,%.0f) sc=(%u,%u,%u,%u) depth=%d(%u) cull=%d\n",
                prim_call_count, i, v.pos[0], v.pos[1], v.pos[2], v.w, v.uv[0], v.uv[1],
                v.color[0], v.color[1], v.color[2], v.color[3],
                (state.vertex_type & 0x800000) ? 1 : 0,
                state.viewport_x_center, state.viewport_y_center, state.viewport_x_scale, state.viewport_y_scale,
                (unsigned)(state.scissor1 & 0x3FF), (unsigned)((state.scissor1 >> 10) & 0x3FF), (unsigned)(state.scissor2 & 0x3FF), (unsigned)((state.scissor2 >> 10) & 0x3FF),
                state.depth_test_enable ? 1 : 0, (unsigned)state.depth_func, state.cull_enable ? 1 : 0);
        }
    }

    // Handle RECTANGLES: expand 2 verts to 6 (two tris)
    std::vector<PackedVertex> packed;
    const uint64_t prof_pack_t0 = psp_prof::enabled ? psp_prof::now() : 0;
    if (prim_type == GE_PRIM_RECTANGLES) {
        // Each rectangle is 2 vertices (top-left, bottom-right)
        int rect_count = count / 2;
        packed.reserve(rect_count * 6);
        for (int r = 0; r < rect_count; r++) {
            int i0 = r * 2;
            int i1 = r * 2 + 1;
            if (i1 >= static_cast<int>(decoded.size()))
                break;

            const auto& v0 = decoded[i0];
            const auto& v1 = decoded[i1];
            if (v0.w <= 0.0f || v1.w <= 0.0f) continue;

            // Build 4 corners from 2 vertices
            PackedVertex corners[4];
            // v0 (top-left)
            corners[0] = {
                {v0.pos[0], v0.pos[1], v0.pos[2], 1.0f},
                {v0.uv[0], v0.uv[1]},
                {v0.color[0], v0.color[1],
                 v0.color[2], v0.color[3]}
            };
            // top-right
            corners[1] = {
                {v1.pos[0], v0.pos[1], v0.pos[2], 1.0f},
                {v1.uv[0], v0.uv[1]},
                {v1.color[0], v1.color[1],
                 v1.color[2], v1.color[3]}
            };
            // bottom-right (v1)
            corners[2] = {
                {v1.pos[0], v1.pos[1], v1.pos[2], 1.0f},
                {v1.uv[0], v1.uv[1]},
                {v1.color[0], v1.color[1],
                 v1.color[2], v1.color[3]}
            };
            // bottom-left
            corners[3] = {
                {v0.pos[0], v1.pos[1], v1.pos[2], 1.0f},
                {v0.uv[0], v1.uv[1]},
                {v0.color[0], v0.color[1],
                 v0.color[2], v0.color[3]}
            };

            // Two triangles: 0-1-2 and 0-2-3
            packed.push_back(corners[0]);
            packed.push_back(corners[1]);
            packed.push_back(corners[2]);
            packed.push_back(corners[0]);
            packed.push_back(corners[2]);
            packed.push_back(corners[3]);
        }
    } else {
        // Pack decoded vertices
        packed.resize(decoded.size());
        for (size_t i = 0; i < decoded.size(); i++) {
            const auto& v = decoded[i];
            packed[i] = {
                {v.pos[0], v.pos[1], v.pos[2], v.w},
                {v.uv[0], v.uv[1]},
                {v.color[0], v.color[1],
                 v.color[2], v.color[3]}
            };
        }
    }

    if (psp_prof::enabled) psp_prof::ns[psp_prof::S_PACK] += psp_prof::now() - prof_pack_t0;
    if (packed.empty()) return;
    psp_prof::draws++;
    psp_prof::verts += packed.size();
    if (psp_prof::enabled) {
        const bool thr = ge_vtype_through(state.vertex_type);
        const bool skin = ge_vtype_weight(state.vertex_type) != 0;
        const int c = thr ? 0 : (state.lighting_enable ? (skin ? 4 : 3) : (skin ? 2 : 1));
        psp_prof::cat_draws[c]++; psp_prof::cat_verts[c] += packed.size();
        {
            static GeState prev{}; static bool have = false;
            auto d = [&](const void* a, const void* b, size_t n, int g) { if (have && std::memcmp(a, b, n)) psp_prof::grp_changes[g]++; };
            const GeState& a = state; const GeState& b = prev;
            d(a.world_matrix, b.world_matrix, sizeof a.world_matrix, psp_prof::G_WORLD);
            d(a.view_matrix, b.view_matrix, sizeof a.view_matrix, psp_prof::G_VIEW);
            d(a.proj_matrix, b.proj_matrix, sizeof a.proj_matrix, psp_prof::G_PROJ);
            if (skin) d(a.bone_matrix, b.bone_matrix, sizeof a.bone_matrix, psp_prof::G_BONES);
            { uint32_t x[8] = {a.tex_addr[0], a.tex_bufw[0], a.tex_size[0], a.tex_format, a.tex_mode, a.clut_addr, a.clut_addr_upper, a.clut_format};
              uint32_t y[8] = {b.tex_addr[0], b.tex_bufw[0], b.tex_size[0], b.tex_format, b.tex_mode, b.clut_addr, b.clut_addr_upper, b.clut_format};
              d(x, y, sizeof x, psp_prof::G_TEX); }
            { uint32_t x[6] = {a.tex_func | (a.tex_func_rgba << 8) | (a.tex_func_double << 9), a.tex_filter, a.tex_wrap, a.tex_env_color, a.texture_enable, 0};
              uint32_t y[6] = {b.tex_func | (b.tex_func_rgba << 8) | (b.tex_func_double << 9), b.tex_filter, b.tex_wrap, b.tex_env_color, b.texture_enable, 0};
              d(x, y, sizeof x, psp_prof::G_TEXFN); }
            { uint32_t x[5] = {a.alpha_blend_enable, a.blend_mode, a.blend_fix_a, a.blend_fix_b, 0};
              uint32_t y[5] = {b.alpha_blend_enable, b.blend_mode, b.blend_fix_a, b.blend_fix_b, 0};
              d(x, y, sizeof x, psp_prof::G_BLEND); }
            { uint32_t x[10] = {a.depth_test_enable, a.depth_func, a.depth_write_disable, a.cull_enable, a.cull_face, a.mask_rgb, a.mask_alpha, a.alpha_test_enable, a.alpha_test, a.color_test_enable};
              uint32_t y[10] = {b.depth_test_enable, b.depth_func, b.depth_write_disable, b.cull_enable, b.cull_face, b.mask_rgb, b.mask_alpha, b.alpha_test_enable, b.alpha_test, b.color_test_enable};
              d(x, y, sizeof x, psp_prof::G_DEPTH); }
            d(&a.lighting_enable, &b.lighting_enable, (const char*)&a.light_spc[4] - (const char*)&a.lighting_enable, psp_prof::G_LIGHT);
            d(&a.vertex_type, &b.vertex_type, 4, psp_prof::G_VTYPE);
            d(&a.viewport_x_scale, &b.viewport_x_scale, 24, psp_prof::G_VP);
            d(&a.tex_scale_u, &b.tex_scale_u, 16, psp_prof::G_UVX);
            prev = a; have = true;
        }
        static uint64_t last_tex_key = ~0ull;
        const uint64_t k = ((uint64_t)state.tex_addr[0] << 32) ^ state.tex_size[0] ^ ((uint64_t)state.clut_addr << 8) ^ state.tex_format;
        if (k == last_tex_key) psp_prof::same_tex_draws++;
        last_tex_key = k;
    }

    // Upload to VBO
    const uint64_t prof_up_t0 = psp_prof::enabled ? psp_prof::now() : 0;
    gl_bind_vao(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 packed.size() * sizeof(PackedVertex),
                 packed.data(),
                 GL_STREAM_DRAW);
    if (psp_prof::enabled) psp_prof::ns[psp_prof::S_UPLOAD] += psp_prof::now() - prof_up_t0;

    // Pipeline state (program/uniforms/texture/blend/depth/cull) via the
    // cached setters shared with the batched path.
    apply_gl_pipeline(rdram, state, prim_type, fbo_tex, false);
    // Draw
    GLenum gl_prim = map_prim_type(prim_type);

    // PSPRECOMP_3D_DIAG=<period>: for every <period>-th non-through draw,
    // log the full pipeline state, the first vertices' clip/NDC position and
    // (via an occlusion query) how many fragments actually passed.
    static const int diag_period = std::getenv("PSPRECOMP_3D_DIAG")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG")) : 0;
    static const int diag_from = std::getenv("PSPRECOMP_3D_DIAG_FROM")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG_FROM")) : 0;
    static const int diag_minn = std::getenv("PSPRECOMP_3D_DIAG_MINN")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG_MINN")) : 0;
    static int diag_n = 0, diag_logged = 0;
    const bool diag = diag_period > 0 && !ge_vtype_through(state.vertex_type)
        && prim_call_count >= diag_from && count >= diag_minn
        && (diag_n++ % diag_period) == 0 && diag_logged < 600;
    GLuint diag_q = 0;
    if (diag) {
        diag_logged++;
        const GeViewportDepth vp = compute_state_viewport_depth(state);
        std::fprintf(stderr,
            "[D3] #%d type=%d n=%d vtype=0x%06X fb=0x%08X glvp=(%d,%d,%d,%d) z=%.3f..%.3f off=(0x%X,0x%X) "
            "zt=%d zf=%u zwd=%d cull=%d/%u tex=%d fmt=%u addr=0x%08X fbotex=%d light=%d blend=%d atest=%d "
            "mask=%06X/%02X bm=0x%X atest=0x%X tfunc=%u rgba=%d dbl=%d tsz=0x%X lmode?\n",
            prim_call_count, prim_type, count, state.vertex_type, state.framebuf_ptr,
            vp.x, vp.y, vp.w, vp.h, vp.near_z, vp.far_z, state.offset_x, state.offset_y,
            state.depth_test_enable ? 1 : 0, (unsigned)state.depth_func,
            state.depth_write_disable ? 1 : 0, state.cull_enable ? 1 : 0, state.cull_face,
            state.texture_enable ? 1 : 0, (unsigned)state.tex_format, state.tex_addr[0], fbo_tex ? 1 : 0,
            state.lighting_enable ? 1 : 0, state.alpha_blend_enable ? 1 : 0,
            state.alpha_test_enable ? 1 : 0, state.mask_rgb, state.mask_alpha & 0xFF,
            (unsigned)state.blend_mode, (unsigned)state.alpha_test, (unsigned)state.tex_func,
            state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0, (unsigned)state.tex_size[0]);
        for (size_t i = 0; i < packed.size() && i < 3; i++) {
            const PackedVertex& p = packed[i];
            const float w = p.pos[3];
            std::fprintf(stderr,
                "[D3]   v%zu clip=(%.3f,%.3f,%.3f,%.3f) ndc=(%.3f,%.3f,%.3f) uv=(%.3f,%.3f) rgba=%d,%d,%d,%d\n",
                i, p.pos[0], p.pos[1], p.pos[2], w,
                p.pos[0] / w, p.pos[1] / w, p.pos[2] / w, p.uv[0], p.uv[1],
                (int)p.color[0], (int)p.color[1], (int)p.color[2], (int)p.color[3]);
        }
        if (std::getenv("PSPRECOMP_3D_DIAG_MAT")) {
            auto pm = [&](const char* n, const float* m, int c) {
                std::fprintf(stderr, "[D3]   %s:", n);
                for (int k = 0; k < c; k++) std::fprintf(stderr, " %g", m[k]);
                std::fprintf(stderr, "\n");
            };
            pm("W", state.world_matrix, 12); pm("V", state.view_matrix, 12); pm("P", state.proj_matrix, 16);
            for (int b = 0; b < 3; b++) { char nm[8]; std::snprintf(nm, 8, "B%d", b); pm(nm, &state.bone_matrix[b * 12], 12); }
        }
        glGenQueries(1, &diag_q);
        glBeginQuery(GL_SAMPLES_PASSED, diag_q);
    }
    static const int big_thresh = std::getenv("PSPRECOMP_3D_DIAG_BIG")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG_BIG")) : 0;
    static int big_logged = 0;
    GLuint big_q = 0;
    const bool big = big_thresh > 0 && !diag && !ge_vtype_through(state.vertex_type)
        && prim_call_count >= diag_from && big_logged < 300;
    if (big) { glGenQueries(1, &big_q); glBeginQuery(GL_SAMPLES_PASSED, big_q); }
    GLuint dt_q = 0;
    static const char* dt_only_env = std::getenv("PSPRECOMP_DT_ONLY_VT");
    static const uint32_t dt_only_vt = dt_only_env ? (uint32_t)std::strtoul(dt_only_env, nullptr, 16) : 0;
    static const uint32_t dt_only_vt2 = (dt_only_env && std::strchr(dt_only_env, ',')) ? (uint32_t)std::strtoul(std::strchr(dt_only_env, ',') + 1, nullptr, 16) : dt_only_vt;
    const bool dt = g_dyn_trace_n.load() > 0 && (!dt_only_env || g_xp_nofilter.load() || state.vertex_type == dt_only_vt || state.vertex_type == dt_only_vt2);
    if (dt) { g_dyn_trace_n--; glGenQueries(1, &dt_q); glBeginQuery(GL_SAMPLES_PASSED, dt_q); }
    {
        psp_prof::Scope ps(psp_prof::S_DRAW);
        if (g_no_raster != 2)
            glDrawArrays(gl_prim, 0,
                         static_cast<GLsizei>(packed.size()));
    }
    if (dt) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint passed = 0;
        glGetQueryObjectuiv(dt_q, GL_QUERY_RESULT, &passed);
        glDeleteQueries(1, &dt_q);
        const GeViewportDepth vp = compute_state_viewport_depth(state);
        const PackedVertex& p0 = packed[0];
        if (state.lighting_enable != 0 || true) {
            static uint64_t last_hash = 0;
            uint64_t h = state.ambient_color * 31u + state.ambient_alpha * 7u + state.material_emissive * 13u + state.material_ambient * 17u + state.material_diffuse * 19u + state.material_alpha * 23u + state.material_update * 29u + (state.lighting_enable?1:0) + (state.light_enable[0]?2:0) + (state.light_enable[1]?4:0)+ (state.light_enable[2]?8:0)+ (state.light_enable[3]?16:0) + state.light_amb[0] * 37u + state.light_dif[0] * 41u;
            if (h != last_hash) {
                last_hash = h;
                std::fprintf(stderr, "[DT]   LIGHT lit=%d en=%d%d%d%d amb=%06X/%02X emis=%06X mamb=%06X mdif=%06X malpha=%02X mupd=%u l0amb=%06X l0dif=%06X l0pos=%g,%g,%g type=%X\n",
                    state.lighting_enable?1:0, state.light_enable[0]?1:0, state.light_enable[1]?1:0, state.light_enable[2]?1:0, state.light_enable[3]?1:0,
                    state.ambient_color&0xFFFFFF, state.ambient_alpha&0xFF, state.material_emissive&0xFFFFFF, state.material_ambient&0xFFFFFF,
                    state.material_diffuse&0xFFFFFF, state.material_alpha&0xFF, state.material_update, state.light_amb[0]&0xFFFFFF, state.light_dif[0]&0xFFFFFF,
                    state.light_pos[0][0], state.light_pos[0][1], state.light_pos[0][2], (unsigned)state.light_type[0]);
            }
        }
        {
            static uint64_t last_m = 0;
            uint64_t mh = 1469598103934665603ull;
            auto mix = [&](const float* f, int n) { for (int i = 0; i < n; i++) { uint32_t u; std::memcpy(&u, &f[i], 4); mh = (mh ^ u) * 1099511628211ull; } };
            mix(state.world_matrix, 12); mix(state.view_matrix, 12); mix(state.proj_matrix, 16);
            if (mh != last_m) {
                last_m = mh;
                std::fprintf(stderr, "[DT]   MTX world=[");
                for (int i = 0; i < 12; i++) std::fprintf(stderr, "%g ", state.world_matrix[i]);
                std::fprintf(stderr, "] view=[");
                for (int i = 0; i < 12; i++) std::fprintf(stderr, "%g ", state.view_matrix[i]);
                std::fprintf(stderr, "] proj=[");
                for (int i = 0; i < 16; i++) std::fprintf(stderr, "%g ", state.proj_matrix[i]);
                std::fprintf(stderr, "]\n");
            }
        }
        if (ge_vtype_weight(state.vertex_type) != 0) {
            static uint64_t last_b = 0;
            const int nb = std::min<int>(((state.vertex_type >> GE_VTYPE_WEIGHTCOUNT_SHIFT) & GE_VTYPE_WEIGHTCOUNT_MASK) + 1, 8);
            uint64_t bh = 1469598103934665603ull;
            for (int i = 0; i < nb * 12; i++) { uint32_t u; std::memcpy(&u, &state.bone_matrix[i], 4); bh = (bh ^ u) * 1099511628211ull; }
            if (bh != last_b) {
                last_b = bh;
                for (int b = 0; b < nb; b++) {
                    std::fprintf(stderr, "[DT]   BONE%d=[", b);
                    for (int i = 0; i < 12; i++) std::fprintf(stderr, "%g ", state.bone_matrix[b * 12 + i]);
                    std::fprintf(stderr, "]\n");
                }
            }
        }
        if (state.texture_enable && (state.tex_format == 4 || state.tex_format == 5)) {
            static uint64_t seen[64]; static int nseen = 0;
            uint32_t tad = (state.tex_addr[0] | (((state.tex_bufw[0] >> 16) & 0xFFu) << 24)) & 0x07FFFFFFu;
            uint32_t cad = ((state.clut_addr & 0xFFFFFF) | ((state.clut_addr_upper << 8) & 0x0F000000)) & 0x07FFFFFFu;
            uint64_t key = ((uint64_t)tad << 32) | cad;
            bool is_new = true;
            for (int i = 0; i < nseen; i++) if (seen[i] == key) is_new = false;
            if (is_new && nseen < 64) {
                seen[nseen++] = key;
                std::fprintf(stderr, "[DT]   TEX addr=%08X fmt=%u size=%X bufw=%X mode=%X clut=%08X clf=%X texdata:", tad, (unsigned)state.tex_format,
                             (unsigned)state.tex_size[0], (unsigned)state.tex_bufw[0] & 0x7FF, (unsigned)state.tex_mode, cad, (unsigned)state.clut_format);
                for (int i = 0; i < 32; i++) std::fprintf(stderr, "%02X", rdram[tad + i]);
                std::fprintf(stderr, " clutdata:");
                for (int i = 0; i < 32; i++) std::fprintf(stderr, "%02X", rdram[cad + i]);
                unsigned nz = 0; for (int i = 0; i < 4096; i++) nz += rdram[tad + i] != 0;
                unsigned cz = 0; for (int i = 0; i < 1024; i++) cz += rdram[cad + i] != 0;
                std::fprintf(stderr, " nz_tex4k=%u nz_clut1k=%u atest=%X ablend=%d bmode=%X wrap=%X filt=%X tfunc=%u/%d/%d\n", nz, cz,
                             (unsigned)state.alpha_test, state.alpha_blend_enable ? 1 : 0, (unsigned)state.blend_mode, (unsigned)state.tex_wrap, (unsigned)state.tex_filter,
                             (unsigned)state.tex_func, state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0);
            }
        }
        std::fprintf(stderr, "[DT]   FB fbfmt=%u fbw=%u tex=%d texfmt=%u texbufw=0x%X texsize=0x%X\n", (unsigned)state.framebuf_format,
                     (unsigned)state.framebuf_width, state.texture_enable ? 1 : 0, (unsigned)state.tex_format,
                     (unsigned)state.tex_bufw[0], (unsigned)state.tex_size[0]);
        if (state.tex_map_mode != 0 || state.tex_shade_ls != 0)
            std::fprintf(stderr, "[DT]   TMAP mode=%06X shadels=%06X tgen=[%g %g %g %g | %g %g %g %g | %g %g %g %g]\n",
                         (unsigned)state.tex_map_mode, (unsigned)state.tex_shade_ls,
                         state.tgen_matrix[0], state.tgen_matrix[1], state.tgen_matrix[2], state.tgen_matrix[3],
                         state.tgen_matrix[4], state.tgen_matrix[5], state.tgen_matrix[6], state.tgen_matrix[7],
                         state.tgen_matrix[8], state.tgen_matrix[9], state.tgen_matrix[10], state.tgen_matrix[11]);
        if (packed.size() <= 6) {
            std::fprintf(stderr, "[DT]   VERTS");
            for (size_t i = 0; i < packed.size(); i++) {
                const PackedVertex& q = packed[i];
                std::fprintf(stderr, " (%.2f,%.2f,%.2f,%.2f|uv %.2f,%.2f|c %02X%02X%02X%02X)", q.pos[0], q.pos[1], q.pos[2], q.pos[3], q.uv[0], q.uv[1], q.color[0], q.color[1], q.color[2], q.color[3]);
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "[DT] #%d pc=%08X pass=%u t=%d n=%d vt=%06X fb=%08X vp=(%d,%d,%d,%d) z=%.3f..%.3f zt=%d zf=%u zwd=%d cull=%d/%u tex=%d fmt=%u ta=%08X cl=%08X/%X fbotex=%d bl=%d bm=%X at=%d mask=%06X/%02X v0=(%.1f,%.1f,%.4f,%.2f) col=%02X%02X%02X%02X va=%08X ia=%08X clf=%X ts=%X raw=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X st=%d/%06X/%06X tf=%u/%d/%d bfa=%06X bfb=%06X\n",
            prim_call_count, g_ge_cur_pc, passed, prim_type, count, state.vertex_type, state.framebuf_ptr,
            vp.x, vp.y, vp.w, vp.h, vp.near_z, vp.far_z, state.depth_test_enable?1:0, (unsigned)state.depth_func,
            state.depth_write_disable?1:0, state.cull_enable?1:0, state.cull_face, state.texture_enable?1:0,
            (unsigned)state.tex_format, state.tex_addr[0], (unsigned)((state.clut_addr & 0xFFFFFF) | ((state.clut_addr_upper << 8) & 0x0F000000)), (unsigned)state.clut_format, fbo_tex?1:0, state.alpha_blend_enable?1:0,
            (unsigned)state.blend_mode, state.alpha_test_enable?1:0, state.mask_rgb, state.mask_alpha&0xFF,
            p0.pos[0], p0.pos[1], p0.pos[2], p0.pos[3], p0.color[0], p0.color[1], p0.color[2], p0.color[3],
            state.vertex_addr, state.index_addr, (unsigned)state.clut_format, (unsigned)state.tex_size[0],
            rdram[(state.vertex_addr&0x07FFFFFF)+0], rdram[(state.vertex_addr&0x07FFFFFF)+1], rdram[(state.vertex_addr&0x07FFFFFF)+2], rdram[(state.vertex_addr&0x07FFFFFF)+3],
            rdram[(state.vertex_addr&0x07FFFFFF)+4], rdram[(state.vertex_addr&0x07FFFFFF)+5], rdram[(state.vertex_addr&0x07FFFFFF)+6], rdram[(state.vertex_addr&0x07FFFFFF)+7],
            rdram[(state.vertex_addr&0x07FFFFFF)+8], rdram[(state.vertex_addr&0x07FFFFFF)+9], rdram[(state.vertex_addr&0x07FFFFFF)+10], rdram[(state.vertex_addr&0x07FFFFFF)+11],
            state.stencil_test_enable?1:0, (unsigned)state.stencil_test&0xFFFFFF, (unsigned)state.stencil_op&0xFFFFFF, (unsigned)state.tex_func, state.tex_func_rgba?1:0, state.tex_func_double?1:0, (unsigned)state.blend_fix_a&0xFFFFFF, (unsigned)state.blend_fix_b&0xFFFFFF);
    }
    if (big) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint passed = 0;
        glGetQueryObjectuiv(big_q, GL_QUERY_RESULT, &passed);
        glDeleteQueries(1, &big_q);
        if (passed >= (GLuint)big_thresh) {
            big_logged++;
            const PackedVertex& p0 = packed[0];
            std::fprintf(stderr,
                "[BIG] #%d passed=%u type=%d n=%zu vtype=0x%06X fb=0x%08X tex=%d fmt=%u addr=0x%08X tsz=0x%X "
                "bm=0x%X blend=%d zt=%d zf=%u zwd=%d atest=%d/0x%X tfunc=%u rgba=%d dbl=%d light=%d "
                "v0 clip=(%.2f,%.2f,%.2f,%.2f) uv=(%.3f,%.3f) col=%d,%d,%d,%d amb=%06X/%02X mem=%06X mamb=%06X mdif=%06X malpha=%02X\n",
                prim_call_count, passed, prim_type, packed.size(), state.vertex_type, state.framebuf_ptr,
                state.texture_enable ? 1 : 0, (unsigned)state.tex_format, state.tex_addr[0],
                (unsigned)state.tex_size[0], (unsigned)state.blend_mode, state.alpha_blend_enable ? 1 : 0,
                state.depth_test_enable ? 1 : 0, (unsigned)state.depth_func, state.depth_write_disable ? 1 : 0,
                state.alpha_test_enable ? 1 : 0, (unsigned)state.alpha_test, (unsigned)state.tex_func,
                state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0, state.lighting_enable ? 1 : 0,
                p0.pos[0], p0.pos[1], p0.pos[2], p0.pos[3], p0.uv[0], p0.uv[1],
                (int)p0.color[0], (int)p0.color[1], (int)p0.color[2], (int)p0.color[3],
                state.ambient_color & 0xFFFFFF, state.ambient_alpha & 0xFF, state.material_emissive & 0xFFFFFF,
                state.material_ambient & 0xFFFFFF, state.material_diffuse & 0xFFFFFF, state.material_alpha & 0xFF);
        }
    }
    if (diag) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint passed = 0;
        glGetQueryObjectuiv(diag_q, GL_QUERY_RESULT, &passed);
        glDeleteQueries(1, &diag_q);
        std::fprintf(stderr, "[D3]   samples_passed=%u\n", passed);
    }
    g_has_drawn_prims = true;
}

// Service the pending screenshot/auto-capture from the given front FBO.
static void present_service_screenshot(GLuint fbo) {
    if (!g_screenshot_enabled || g_screenshot_done || !g_has_drawn_prims) {
        return;
    }
    std::vector<uint8_t> flipped;
    int cw = 480, chh = 272;
    read_fbo_native(fbo, flipped, &cw, &chh);
    // An active savedata dialog is composited over the frame at present time;
    // include it in the capture so screenshots match what the window shows.
    composite_dialog_overlay(flipped, cw, chh);
    write_tga(g_screenshot_path, flipped.data(), cw, chh);
    g_screenshot_done = true;
}

struct PresentRect { int x0, y0, x1, y1; GLenum filter; bool bars; };

// ---- Window presentation: aspect fit / integer scale / stretch ------------
// PSPRECOMP_STRETCH=1 fills the window; PSPRECOMP_FILTER=nearest|linear|integer.
// PSPRECOMP_INTEGER_SCALE=1 (generic "retro" presentation): the picture is the
// largest whole multiple k of 480x272 that fits the drawable (k=2 -> 960x544
// centred with black borders on a 1280x800 Steam Deck panel), and the blit is
// nearest-neighbour whenever the render target is a whole multiple of the
// output (or vice versa), bilinear otherwise. PSPRECOMP_FILTER=integer gives the
// same layout but always nearest. Default: 480:272 aspect-fit with black bars;
// nearest when the fit is an exact integer multiple, bilinear otherwise. Computed on the *drawable* size
// (SDL_GL_GetDrawableSize: 2x the window size on Retina) and cached per size.
static bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

static PresentRect present_layout(SDL_Window* window) {
    static int cw = -1, ch = -1;
    static PresentRect cached{0, 0, 480, 272, GL_NEAREST, false};
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) { dw = 480; dh = 272; }
    if (dw == cw && dh == ch) return cached;
    cw = dw; ch = dh;
    const char* st = std::getenv("PSPRECOMP_STRETCH");
    const bool stretch = st && st[0] && st[0] != '0';
    const char* fl = std::getenv("PSPRECOMP_FILTER");
    std::string filter = fl ? fl : "";
    if (filter.empty() && env_on("PSPRECOMP_INTEGER_SCALE")) filter = "integer-scale";
    PresentRect r{0, 0, dw, dh, GL_NEAREST, false};
    bool exact_int = false;
    if (filter == "integer" || filter == "integer-scale") {
        int k = std::min(dw / 480, dh / 272);
        if (k >= 1) {
            int w = 480 * k, h = 272 * k;
            r.x0 = (dw - w) / 2; r.y0 = (dh - h) / 2; r.x1 = r.x0 + w; r.y1 = r.y0 + h;
            exact_int = true;
        } else {  // window smaller than native: fall back to a linear fit
            double s = std::min(dw / 480.0, dh / 272.0);
            int w = std::max(1, int(480 * s + 0.5)), h = std::max(1, int(272 * s + 0.5));
            r.x0 = (dw - w) / 2; r.y0 = (dh - h) / 2; r.x1 = r.x0 + w; r.y1 = r.y0 + h;
        }
    } else if (!stretch) {
        double s = std::min(dw / 480.0, dh / 272.0);
        int w = std::min(dw, std::max(1, int(480 * s + 0.5)));
        int h = std::min(dh, std::max(1, int(272 * s + 0.5)));
        r.x0 = (dw - w) / 2; r.y0 = (dh - h) / 2; r.x1 = r.x0 + w; r.y1 = r.y0 + h;
        exact_int = (w % 480 == 0 && h % 272 == 0 && w / 480 == h / 272);
    } else {
        exact_int = (dw % 480 == 0 && dh % 272 == 0 && dw / 480 == dh / 272);
    }
    if (filter == "nearest" || exact_int) r.filter = GL_NEAREST;
    else if (filter == "integer" && exact_int) r.filter = GL_NEAREST;
    else r.filter = GL_LINEAR;   // (integer-scale: present_blit decides from the render-scale ratio)
    r.bars = (r.x0 != 0 || r.y0 != 0 || r.x1 != dw || r.y1 != dh);
    cached = r;
    std::fprintf(stderr, "[PRESENT] drawable %dx%d -> dst (%d,%d)-(%d,%d) %s%s\n", dw, dh,
                 r.x0, r.y0, r.x1, r.y1, r.filter == GL_LINEAR ? "linear" : "nearest",
                 r.bars ? " +bars" : "");
    return r;
}

// Debug socket 'V <path.tga>': capture the whole window (drawable size).
static std::mutex g_wc_mutex;
static std::condition_variable g_wc_cv;
static int g_wc_state = 0;  // 0 idle, 1 pending, 2 done, 3 failed
static char g_wc_path[256];

bool ge_draw_capture_window(const char* path, int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_wc_mutex);
    if (g_wc_state == 1) return false;
    std::strncpy(g_wc_path, path, sizeof(g_wc_path) - 1);
    g_wc_path[sizeof(g_wc_path) - 1] = '\0';
    g_wc_state = 1;
    bool fin = g_wc_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [] { return g_wc_state >= 2; });
    bool ok = fin && g_wc_state == 2;
    g_wc_state = 0;
    return ok;
}

static void present_service_window_capture() {
    char path[sizeof(g_wc_path)];
    {
        std::lock_guard<std::mutex> lock(g_wc_mutex);
        if (g_wc_state != 1) return;
        std::memcpy(path, g_wc_path, sizeof(path));
    }
    SDL_Window* window = psp_get_sdl_window();
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    std::vector<uint8_t> px(size_t(dw) * dh * 4), fl(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < dh; y++)
        std::memcpy(fl.data() + size_t(y) * dw * 4, px.data() + size_t(dh - 1 - y) * dw * 4, size_t(dw) * 4);
    write_tga(path, fl.data(), dw, dh);
    std::lock_guard<std::mutex> lock(g_wc_mutex);
    if (g_wc_state == 1) { g_wc_state = 2; g_wc_cv.notify_all(); }
}

// Utility-dialog overlay (sceUtilitySavedata list): the HLE renders the
// dialog into an RGBA canvas at an integer multiple (1..4) of 480x272 chosen
// from the output size (psp_sddialog::set_output_scale, updated each present)
// and publishes it; while one is active it is blitted opaquely over the game
// frame, filtered linearly when it is not a 1:1 blit. GL texture/pixel-store
// state is saved and restored so the GE's own state tracking is not disturbed.
static void present_dialog_overlay(const PresentRect& pr) {
    static GLuint ovl_tex = 0, ovl_fbo = 0;
    static int tex_w = 0, tex_h = 0;
    static std::vector<uint32_t> px;
    int ow = 0, oh = 0;
    if (!psp_sddialog::overlay_fetch(px, &ow, &oh) || ow <= 0 || oh <= 0 ||
        px.size() != size_t(ow) * size_t(oh))
        return;
    GLint prev_tex = 0, prev_align = 4, prev_row_len = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_len);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (!ovl_tex) {
        glGenTextures(1, &ovl_tex);
        glBindTexture(GL_TEXTURE_2D, ovl_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &ovl_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, ovl_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, ovl_tex, 0);
    }
    glBindTexture(GL_TEXTURE_2D, ovl_tex);
    if (tex_w != ow || tex_h != oh) {   // (re)allocate when the overlay scale changes
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        tex_w = ow; tex_h = oh;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ovl_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    const bool one_to_one = (pr.x1 - pr.x0) == ow && (pr.y1 - pr.y0) == oh;
    // Canvas row 0 is the top; GL texture row 0 is the bottom => flip Y.
    glBlitFramebuffer(0, 0, ow, oh, pr.x0, pr.y1, pr.x1, pr.y0, GL_COLOR_BUFFER_BIT,
                      one_to_one ? GL_NEAREST : GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_len);
}

// PSPRECOMP_FRAME_DUMP=<dir>,<n>: write the next n presented frames (every
// present_blit, including liveness-net presents) as <dir>/f_<k>.tga, with the
// present reason in the log, to inspect frame-to-frame alternation. Capture
// starts once the file <dir>/go exists.
static void present_dump_frame(GLuint fbo, const char* why) {
    static const char* spec = std::getenv("PSPRECOMP_FRAME_DUMP");
    static std::string dir; static int remaining = -1, k = 0;
    if (!spec) return;
    if (remaining < 0) {
        std::string sp(spec); size_t c = sp.rfind(',');
        dir = c == std::string::npos ? sp : sp.substr(0, c);
        remaining = c == std::string::npos ? 100 : std::atoi(sp.c_str() + c + 1);
    }
    if (remaining <= 0) return;
    // Capture starts when <dir>/go exists (so a script can arm it mid-run).
    static bool started = false;
    if (!started) {
        std::string go = dir + "/go";
        if (access(go.c_str(), F_OK) != 0) return;
        started = true;
    }
    remaining--;
    std::vector<uint8_t> fl;
    int cw = 480, chh = 272;
    read_fbo_native(fbo, fl, &cw, &chh);
    char path[600];
    std::snprintf(path, sizeof path, "%s/f_%04d.tga", dir.c_str(), k);
    write_tga(path, fl.data(), cw, chh);
    long sum = 0; for (size_t i = 0; i < fl.size(); i += 4 * 61) sum += fl[i] + fl[i + 1] + fl[i + 2];
    std::fprintf(stderr, "[FDUMP] %d %s fbo=%u t=%.1fms meansum=%ld\n", k, why, fbo,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(), sum);
    k++;
}

// Blit the given front FBO to the window and swap. Common to the routine
// (page-flip) present and the liveness net.
static void present_blit(GLuint fbo, const char* why = "flip") {
    SDL_Window* window = psp_get_sdl_window();
    if (!window) return;
    present_dump_frame(fbo, why);
    present_service_screenshot(fbo);
    const PresentRect pr = present_layout(window);
    // Tell the savedata-dialog renderer how big the output is so the overlay
    // is rendered at (at least) output resolution rather than upscaled.
    psp_sddialog::set_output_scale(std::max({g_rs, (pr.x1 - pr.x0 + 479) / 480, 1}));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (pr.bars) {
        GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        GLfloat cc[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        GLboolean cm[4]; glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        if (sc) glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        glColorMask(cm[0], cm[1], cm[2], cm[3]);
        if (sc) glEnable(GL_SCISSOR_TEST);
    }
    // Source is the 480n x 272n target. A nearest blit that is not 1:1 would
    // alias the supersampled image, so use linear unless nearest is forced.
    GLenum pf = pr.filter;
    static const char* fl = std::getenv("PSPRECOMP_FILTER");
    if (!(fl && fl[0]) && env_on("PSPRECOMP_INTEGER_SCALE")) {
        // Integer-scale presentation: crisp only when source and output differ by a whole factor
        // (render scale 2 shown at 2x = 1:1; render scale 1 shown at 2x; render scale 2 at 1x is a 2:1
        // downscale, also whole). Otherwise (e.g. 2x target on a 3x output) bilinear avoids uneven pixels.
        const int sw = 480 * g_rs, ow = pr.x1 - pr.x0;
        const bool whole = (ow >= sw) ? (ow % sw == 0) : (sw % ow == 0);
        pf = whole ? GL_NEAREST : GL_LINEAR;
    } else if (g_rs > 1 && pf == GL_NEAREST && (pr.x1 - pr.x0) != 480 * g_rs) {
        if (!(fl && (!std::strcmp(fl, "nearest") || !std::strcmp(fl, "integer")))) pf = GL_LINEAR;
    }
    glBlitFramebuffer(0, 0, 480 * g_rs, 272 * g_rs, pr.x0, pr.y0, pr.x1, pr.y1,
                      GL_COLOR_BUFFER_BIT, pf);
    present_dialog_overlay(pr);
    present_service_window_capture();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (g_sync_mode == 3) glFinish();
    SDL_GL_SwapWindow(window);
    g_present_dirty = false;
    g_last_present = std::chrono::steady_clock::now();
}


// PSPRECOMP_GE_AB: compare every live target with its legacy twin (native res).
static void ab_compare_all() {
    static uint64_t frame = 0, bad_frames = 0, cmp = 0;
    static const char* dir = std::getenv("PSPRECOMP_AB_DIR") ? std::getenv("PSPRECOMP_AB_DIR") : "/tmp";
    static const int thr = std::getenv("PSPRECOMP_AB_THR") ? std::atoi(std::getenv("PSPRECOMP_AB_THR")) : 4;
    frame++;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        const GeTarget& t = g_targets[i];
        if (!t.fbo || !t.ab_fbo) continue;
        std::vector<uint8_t> a, b;
        int w = 0, h = 0;
        read_fbo_native(t.fbo, a, &w, &h);
        read_fbo_native(t.ab_fbo, b, &w, &h);
        size_t nbad = 0; int maxd = 0;
        std::vector<uint8_t> d(a.size(), 255);
        for (size_t k = 0; k < a.size(); k += 4) {
            int m = 0;
            for (int c = 0; c < 3; c++) m = std::max(m, std::abs((int)a[k + c] - (int)b[k + c]));
            if (m > thr) { nbad++; d[k] = 255; d[k + 1] = 0; d[k + 2] = 0; }
            else { d[k] = d[k + 1] = d[k + 2] = (uint8_t)(a[k] / 2 + 127); }
            maxd = std::max(maxd, m);
        }
        cmp++;
        if (nbad) {
            bad_frames++;
            std::fprintf(stderr, "[AB] present#%llu target key=%06X: %zu px differ (>%d), max channel delta %d\n",
                         (unsigned long long)frame, t.key, nbad, thr, maxd);
            if (nbad >= 20 && g_ab_dumped < 24) {
                char path[512];
                const int n = g_ab_dumped++;
                std::snprintf(path, sizeof path, "%s/ab_%02d_fast.tga", dir, n); write_tga(path, a.data(), w, h);
                std::snprintf(path, sizeof path, "%s/ab_%02d_legacy.tga", dir, n); write_tga(path, b.data(), w, h);
                std::snprintf(path, sizeof path, "%s/ab_%02d_diff.tga", dir, n); write_tga(path, d.data(), w, h);
                std::fprintf(stderr, "[AB] dumped %s/ab_%02d_{fast,legacy,diff}.tga\n", dir, n);
            }
        }
    }
    if (frame % 120 == 0)
        std::fprintf(stderr, "[AB] summary: %llu presents, %llu target comparisons, %llu with differences\n",
                     (unsigned long long)frame, (unsigned long long)cmp, (unsigned long long)bad_frames);
}

void ge_present_frame(
    uint8_t* rdram,
    uint32_t fb_addr,
    uint32_t fb_stride,
    uint32_t fb_format
) {
    (void)rdram;
    (void)fb_stride;
    (void)fb_format;
    ge_draw_flush();
    if (g_fx_on == 1) psp_fx_end_frame();
    if (g_ab) ab_compare_all();
    if (g_dump_armed.exchange(false)) {
        std::string dir;
        { std::lock_guard<std::mutex> lk(g_dump_mu); dir = g_dump_dir; }
        for (int i = 0; i < GE_MAX_TARGETS; ++i) {
            if (!g_targets[i].fbo) continue;
            std::vector<uint8_t> a; int w = 0, h = 0;
            read_fbo_native(g_targets[i].fbo, a, &w, &h);
            char path[512];
            std::snprintf(path, sizeof path, "%s/tgt_%06X.tga", dir.c_str(), g_targets[i].key);
            std::vector<uint8_t> rgb = a;
            for (size_t k = 3; k < rgb.size(); k += 4) rgb[k] = 255;
            write_tga(path, rgb.data(), w, h);
            for (size_t k = 0; k < a.size(); k += 4) { const uint8_t v = a[k + 3]; rgb[k] = rgb[k + 1] = rgb[k + 2] = v; rgb[k + 3] = 255; }
            std::snprintf(path, sizeof path, "%s/tgt_%06X_alpha.tga", dir.c_str(), g_targets[i].key);
            write_tga(path, rgb.data(), w, h);
            std::fprintf(stderr, "[DUMP] target key=%06X -> %s\n", g_targets[i].key, path);
        }
    }
    psp_prof::Scope prof_present(psp_prof::S_PRESENT);
    if (g_no_raster_mode && g_no_raster == 0 && g_no_raster_after > 0.0
        && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t_start).count()
               >= g_no_raster_after) {
        g_no_raster = g_no_raster_mode;
        if (g_no_raster == 1) glEnable(GL_RASTERIZER_DISCARD);
        std::fprintf(stderr, "[DRAW] raster discard mode %d now active\n", g_no_raster);
    }

    // [FPS] GE frames (display flips) per second, logged every 5 s.
    {
        static auto t0 = std::chrono::steady_clock::now();
        static uint32_t n = 0;
        n++;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - t0).count();
        if (dt >= 5.0) {
            std::fprintf(stderr, "[FPS] %.1f GE frames/s (%u in %.1fs)\n", n / dt, n, dt);
            psp_prof::report(dt, n);
            n = 0;
            t0 = now;
        }
    }
    gl_stats_frame();

    // Select the FBO matching the displayed (scanned-out) VRAM address — the
    // front buffer. On key miss for a real (non-zero) address the guest is
    // scanning out a VRAM buffer the GE has not rendered into yet (e.g. the
    // first flip to the back buffer before any list targets it). Create that
    // buffer's own FBO — cleared to black — and present THAT, rather than
    // blitting an unrelated sibling slot (which would publish a different
    // buffer's stale/cleared contents as if they were this frame). This keeps
    // present strictly per-address: a fresh flip shows a clean buffer, and any
    // geometry the GE later renders into this address lands in the same FBO.
    uint32_t key = ge_fb_key(fb_addr);
    if (g_dyn_trace_n.load() > 0)
        std::fprintf(stderr, "[DT] ---- PRESENT fb=0x%08X\n", fb_addr);
    int slot = -1;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo != 0 && g_targets[i].key == key) { slot = i; break; }
    }
    if (slot < 0) {
        if (key != 0) {
            // Materialize the displayed buffer's own (cleared) FBO.
            slot = acquire_target(key);
        } else {
            // key==0 (FRAMEBUFPTR/displayed addr not yet meaningful): keep the
            // most-recently-rendered target so present never blanks.
            slot = (g_front_target >= 0) ? g_front_target : g_current_target;
            static int fallback_count = 0;
            if (++fallback_count <= 20) {
                std::fprintf(stderr,
                    "[PRESENT] fallback: no FBO for fb=0x%08X key=0x%06X "
                    "(using slot=%d)\n", fb_addr, key, slot);
            }
        }
    }
    if (slot < 0) return;  // nothing rendered yet

    g_front_target = slot;
    g_targets[slot].last_use = ++g_target_clock;
    if (std::getenv("PSPRECOMP_MOVIE_TEXDBG"))
        std::fprintf(stderr, "[DISPSET] fb=%08X key=%06X slot=%d\n", fb_addr, key, slot);

    static int present_count = 0;
    if (++present_count <= 5) {
        std::fprintf(stderr,
            "[PRESENT] Frame %d (fb=0x%08X key=0x%06X stride=%u fmt=%u)\n",
            present_count, fb_addr, key, fb_stride, fb_format);
    }
    present_blit(g_targets[slot].fbo);
}

// Liveness net (§2.1): if lists were processed but no real page-flip arrived
// within the stale budget, present the current front buffer once. Preserves
// "content becomes visible even without a flip" without making every list a
// frame boundary. Main (GL) thread only — same thread as the routine present.
void ge_draw_present_if_stale(uint8_t* rdram) {
    (void)rdram;
    if (!g_present_dirty) return;
    auto age = std::chrono::steady_clock::now() - g_last_present;
    if (age < std::chrono::milliseconds(g_present_stale_ms)) return;
    GLuint fbo = front_fbo_id();
    if (fbo == 0) { g_present_dirty = false; return; }
    present_blit(fbo, "stale");
}

// ---- Debug socket accessors (issue #35) ----

void ge_draw_get_stats(
    uint32_t* frames,
    int* prims_total,
    int* real_nonsprite,
    int* sprite_nonclear,
    int* clears
) {
    // Racy reads of render-thread counters -- diagnostics only.
    if (frames) *frames = g_frame_counter;
    if (prims_total) *prims_total = g_prim_total_observed;
    int real_ns = 0, sprite_nc = 0, clr = 0;
    for (int t = 0; t < 8; ++t) {
        clr += g_prim_type_count_clear[t];
        if (t == 6) sprite_nc += g_prim_type_count[t];
        else real_ns += g_prim_type_count[t];
    }
    if (real_nonsprite) *real_nonsprite = real_ns;
    if (sprite_nonclear) *sprite_nonclear = sprite_nc;
    if (clears) *clears = clr;
}

bool ge_draw_capture_screenshot(const char* path, int timeout_ms) {
    if (!g_draw_ready.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
            "[DRAW] Screenshot request before GL init: %s\n", path);
        return false;
    }
    std::unique_lock<std::mutex> lock(g_ss_req_mutex);
    if (g_ss_req_state == SS_PENDING) {
        // Another client's capture is in flight -- refuse instead of
        // overwriting its path.
        return false;
    }
    std::strncpy(g_ss_req_path, path, sizeof(g_ss_req_path) - 1);
    g_ss_req_path[sizeof(g_ss_req_path) - 1] = '\0';
    g_ss_req_state = SS_PENDING;
    bool finished = g_ss_req_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [] { return g_ss_req_state == SS_DONE
                    || g_ss_req_state == SS_FAILED; });
    bool ok = finished && g_ss_req_state == SS_DONE;
    g_ss_req_state = SS_IDLE;   // reset (also cancels on timeout)
    return ok;
}

void ge_draw_service_screenshot_request() {
    char path[sizeof(g_ss_req_path)];
    {
        std::lock_guard<std::mutex> lock(g_ss_req_mutex);
        if (g_ss_req_state != SS_PENDING) return;
        std::memcpy(path, g_ss_req_path, sizeof(path));
    }
    // GL work outside the request lock (requester only waits on the cv).
    bool ok = g_draw_ready.load(std::memory_order_acquire)
              && capture_fbo_to_tga(path);
    {
        std::lock_guard<std::mutex> lock(g_ss_req_mutex);
        // Only publish if the requester hasn't timed out and reset state.
        if (g_ss_req_state == SS_PENDING) {
            g_ss_req_state = ok ? SS_DONE : SS_FAILED;
        }
    }
    g_ss_req_cv.notify_all();
}
