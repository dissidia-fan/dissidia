// fx_shader_preview -- headless GL (EGL, Mesa: Linux / Steam Deck desktop mode)
// preview + GPU bench of the FX shader overrides, built and run by
// games/dissidia/tools/fx_shader_preview.sh. Links the runtime's own
// psp_fx_shader.cpp / psp_fx_manifest.cpp / psp_ge_shader.cpp, so it compiles
// the uber-shader and every manifest program exactly as the game does, draws
// each sample (a manifest entry at one lab tick, with its FEP colour/alpha)
// through psp_fx_begin_batch into a contact sheet, checks the compile-error
// fallback, the screen-copy sampler and that texture unit 0 is left active.
//   fx_shader_preview <samples.txt> <out.png>      (PSPRECOMP_FX_SHADERS_DIR = the test folder)
//   FX_PREVIEW_BENCH=1280x800: also time one full-screen pass of each shader.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#include "psp_ge.h"
#include "psp_ge_shader.h"
#include "psp_fx_shader.h"
#include "psp_tex_pack.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

std::string psp_io_ms0_host_path() { return "/tmp/ms0x/ms0"; }

struct V { float p[4], uv[2], c[4], n[3]; };
static GLuint g_vao, g_vbo, g_fbo, g_sheet_tex, g_copy_fbo, g_copy_tex;
static int W, H;
static unsigned screen_copy(int* w, int* h) {
    GLint cur; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, cur); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, cur);
    *w = W; *h = H; return g_copy_tex;
}
static void pack43(const float m[16], float* o) {  // column-major 4x4 -> 4x3
    for (int c = 0; c < 4; c++) for (int r = 0; r < 3; r++) o[c * 3 + r] = m[c * 4 + r];
}
int main(int argc, char** argv) {
    const char* samples_path = argv[1];
    const char* out_png = argv[2];
    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto getPlat = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlat) dpy = getPlat(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { std::fprintf(stderr, "eglInitialize failed\n"); return 2; }
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfga[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n;
    eglChooseConfig(dpy, cfga, &cfg, 1, &n);
    EGLint ctxa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                     EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : nullptr, EGL_NO_CONTEXT, ctxa);
    if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { std::fprintf(stderr, "no GL 3.3 core context\n"); return 2; }
    gladLoadGL((GLADloadfunc)eglGetProcAddress);
    std::printf("GL: %s | %s | GLSL %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    ge_shader_init();
    if (!ge_shader_get_program()) { std::fprintf(stderr, "uber-shader failed\n"); return 1; }
    if (!psp_fx_want_ids()) { std::fprintf(stderr, "want_ids false\n"); return 1; }

    // samples
    struct Smp { unsigned long long id; std::string kind; float c[4]; int t; std::string layer; };
    std::vector<Smp> smp;
    { std::ifstream f(samples_path); std::string line;
      while (std::getline(f, line)) { std::istringstream ss(line); Smp s; ss >> std::hex >> s.id >> std::dec >> s.kind >> s.c[0] >> s.c[1] >> s.c[2] >> s.c[3] >> s.t >> s.layer; if (!s.kind.empty()) smp.push_back(s); } }
    const int T = 160, COLS = 6;
    const int ROWS = (int)(smp.size() + COLS - 1) / COLS + 1;
    W = T * COLS; H = T * ROWS;
    glGenTextures(1, &g_sheet_tex); glBindTexture(GL_TEXTURE_2D, g_sheet_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &g_fbo); glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sheet_tex, 0);
    glGenTextures(1, &g_copy_tex); glBindTexture(GL_TEXTURE_2D, g_copy_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &g_copy_fbo); glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_copy_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glClearColor(0.02f, 0.02f, 0.04f, 1); glClear(GL_COLOR_BUFFER_BIT);
    // the "game" texture on unit 0 (white)
    GLuint white; glGenTextures(1, &white); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, white);
    unsigned char px[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);

    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, p)); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, uv)); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, c)); glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, n)); glEnableVertexAttribArray(3);

    // camera: eye (0, 0.4, 3.4) looking at the origin; sphere radius 1
    float world4[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float ex = 0, ey = 0.4f, ez = 3.4f;
    float fz[3] = {ex, ey, ez}; float l = std::sqrt(ex*ex+ey*ey+ez*ez); for (float& v : fz) v /= l;   // backward axis
    float fx_[3] = {1, 0, 0};   // right
    float fy[3] = {fz[1]*fx_[2]-fz[2]*fx_[1], fz[2]*fx_[0]-fz[0]*fx_[2], fz[0]*fx_[1]-fz[1]*fx_[0]};
    float view4[16] = {fx_[0], fy[0], fz[0], 0, fx_[1], fy[1], fz[1], 0, fx_[2], fy[2], fz[2], 0,
                       -(fx_[0]*ex+fx_[1]*ey+fx_[2]*ez), -(fy[0]*ex+fy[1]*ey+fy[2]*ez), -(fz[0]*ex+fz[1]*ey+fz[2]*ez), 1};
    const float fov = 50.0f * 3.14159265f / 180.0f, f = 1.0f / std::tan(fov / 2), zn = 0.1f, zf = 100.0f;
    float proj[16] = {f,0,0,0, 0,f,0,0, 0,0,(zf+zn)/(zn-zf),-1, 0,0,2*zf*zn/(zn-zf),0};
    GeState st; std::memset(&st, 0, sizeof st);
    pack43(world4, st.world_matrix); pack43(view4, st.view_matrix); std::memcpy(st.proj_matrix, proj, sizeof proj);
    st.texture_enable = true; st.tex_func = 0; st.tex_func_rgba = true;
    GeXform3D x{}; x.world = st.world_matrix; x.view = st.view_matrix; x.proj = st.proj_matrix;
    x.uvxf[0] = 1; x.uvxf[1] = 1;

    auto sphere = [&](const float* c) {
        std::vector<V> v; const int NU = 64, NV = 40;
        auto P = [&](int i, int j) { float u = (float)i / NU, w = (float)j / NV; float th = u * 6.2831853f, ph = w * 3.14159265f;
            V q{}; q.n[0] = std::sin(ph) * std::cos(th); q.n[1] = -std::cos(ph); q.n[2] = std::sin(ph) * std::sin(th);
            q.p[0] = q.n[0]; q.p[1] = q.n[1]; q.p[2] = q.n[2]; q.p[3] = 1; q.uv[0] = u; q.uv[1] = 1 - w; std::memcpy(q.c, c, 16); return q; };
        for (int i = 0; i < NU; i++) for (int j = 0; j < NV; j++) {
            V a = P(i, j), b = P(i + 1, j), cc = P(i + 1, j + 1), d = P(i, j + 1);
            v.push_back(a); v.push_back(b); v.push_back(cc); v.push_back(a); v.push_back(cc); v.push_back(d); }
        return v; };
    auto quad = [&](const float* c, float sx, float sy) {
        std::vector<V> v; float xs[4] = {-sx, sx, sx, -sx}, ys[4] = {-sy, -sy, sy, sy}, us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
        int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int k : idx) { V q{}; q.p[0] = xs[k]; q.p[1] = ys[k]; q.p[3] = 1; q.uv[0] = us[k]; q.uv[1] = vs[k]; std::memcpy(q.c, c, 16); v.push_back(q); }
        return v; };

    int matched = 0, row = 0, col = 0, failed_expect = 0;
    std::string last_layer;
    for (size_t i = 0; i < smp.size(); i++) {
        const Smp& s = smp[i];
        if (s.layer != last_layer && i) { if (col) { row++; col = 0; } }
        last_layer = s.layer;
        int vp[4] = {col * T, H - (row + 1) * T, T, T};
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        std::vector<V> v = s.kind == "sphere" ? sphere(s.c) : s.kind == "line" ? quad(s.c, 1.0f, 0.06f) : s.kind == "arc" ? quad(s.c, 1.0f, 0.26f) : quad(s.c, 1.0f, 1.0f);
        PspFxDrawIn in{}; in.tex_id = s.id; in.state = &st; in.xform = s.kind == "sphere" ? &x : nullptr;
        in.target_w = W; in.target_h = H; std::memcpy(in.vp, vp, sizeof vp); in.alpha_double = false; in.screen_copy = screen_copy;
        ge_shader_use(); ge_shader_set_uniforms(st);
        PspFxDrawOut out{};
        if (!psp_fx_begin_batch(in, &out)) { std::fprintf(stderr, "NO MATCH for %llx (%s)\n", s.id, s.layer.c_str()); failed_expect++; }
        else matched++;
        using psp_fx::Blend;
        glEnable(GL_BLEND);
        if (out.blend == Blend::AddOne) glBlendFunc(GL_ONE, GL_ONE);
        else if (out.blend == Blend::Alpha) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        else if (out.blend == Blend::Add) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else if (out.blend == Blend::Off) glDisable(GL_BLEND);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(V)), v.data(), GL_STREAM_DRAW);
        if (out.timed) psp_fx_timer_begin();
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());
        if (out.timed) psp_fx_timer_end();
        if (++col == COLS) { col = 0; row++; }
    }
    // GL-level fallback: the broken shader must not match.
    { PspFxDrawIn in{}; in.tex_id = 0xbad; in.state = &st; in.target_w = W; in.target_h = H; PspFxDrawOut out{};
      if (psp_fx_begin_batch(in, &out)) { std::fprintf(stderr, "FAIL: broken shader matched\n"); failed_expect++; }
      else std::printf("ok: broken shader falls back to the uber-shader\n"); }
    // Screen copy: last row, a refracted copy of the sheet's first tile region.
    { if (col) { row++; col = 0; }
      int vp[4] = {0, 0, W, T}; glViewport(0, 0, W, T);
      float c[4] = {1, 1, 1, 1}; std::vector<V> v = quad(c, 1, 1);
      PspFxDrawIn in{}; in.tex_id = 0x5c; in.state = &st; in.target_w = W; in.target_h = H; std::memcpy(in.vp, vp, sizeof vp); in.screen_copy = screen_copy;
      PspFxDrawOut out{};
      if (!psp_fx_begin_batch(in, &out)) { std::fprintf(stderr, "FAIL: screen entry no match\n"); failed_expect++; }
      glDisable(GL_BLEND);
      glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(V)), v.data(), GL_STREAM_DRAW);
      glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size()); }
    { GLenum e0 = glGetError(); if (e0) std::fprintf(stderr, "GL error before checks 0x%x\n", e0); }
    // FX_PREVIEW_BENCH=WxH: GPU cost of each layer's shader covering the whole
    // WxH target (spheres fill the view), 30 passes each, GL_TIME_ELAPSED.
    if (const char* bench = std::getenv("FX_PREVIEW_BENCH")) {
        int bw = 1280, bh = 800;
        std::sscanf(bench, "%dx%d", &bw, &bh);
        GLuint btex, bfbo; glGenTextures(1, &btex); glBindTexture(GL_TEXTURE_2D, btex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &bfbo); glBindFramebuffer(GL_FRAMEBUFFER, bfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, btex, 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, white);
        glViewport(0, 0, bw, bh);
        GLuint q; glGenQueries(1, &q);
        // sphere filling the view: camera close enough that radius 1 covers the frame
        float close_view[12]; std::memcpy(close_view, st.view_matrix, sizeof close_view);
        GeState bst = st; bst.view_matrix[11] = -1.25f; bst.view_matrix[9] = 0; bst.view_matrix[10] = 0;
        for (int i = 0; i < 9; i++) bst.view_matrix[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        GeXform3D bx = x; bx.view = bst.view_matrix;
        std::string last;
        std::printf("bench %dx%d (%s): GPU time of one full-screen pass\n", bw, bh, glGetString(GL_RENDERER));
        for (const Smp& s : smp) {
            if (s.layer == last) continue;
            last = s.layer;
            std::vector<V> v = s.kind == "sphere" ? sphere(s.c) : quad(s.c, 1.0f, 1.0f);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(V)), v.data(), GL_STREAM_DRAW);
            PspFxDrawIn in{}; in.tex_id = s.id; in.state = &bst; in.xform = s.kind == "sphere" ? &bx : nullptr;
            in.target_w = bw; in.target_h = bh; in.vp[2] = bw; in.vp[3] = bh; in.screen_copy = screen_copy;
            PspFxDrawOut out{};
            glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
            if (!psp_fx_begin_batch(in, &out)) continue;
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());   // warm-up
            glFinish();
            glBeginQuery(GL_TIME_ELAPSED, q);
            for (int k = 0; k < 30; k++) glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());
            glEndQuery(GL_TIME_ELAPSED);
            GLuint64 ns = 0; glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
            // the uber-shader on the same geometry, for reference
            ge_shader_use(); ge_shader_set_uniforms(bst);
            if (in.xform) ge_shader_set_xform_3d(bx); else ge_shader_set_xform_passthrough();
            glBeginQuery(GL_TIME_ELAPSED, q);
            for (int k = 0; k < 30; k++) glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());
            glEndQuery(GL_TIME_ELAPSED);
            GLuint64 ns_uber = 0; glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns_uber);
            std::printf("  %-16s %-6s %7.3f ms/pass (uber-shader %6.3f ms)\n", s.layer.c_str(), s.kind.c_str(),
                        ns / 30.0 / 1e6, ns_uber / 30.0 / 1e6);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glViewport(0, 0, W, H);
    }
    GLint act = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    if (act != GL_TEXTURE0) { std::fprintf(stderr, "FAIL: active texture unit left at %d\n", act - GL_TEXTURE0); failed_expect++; }
    GLenum err = glGetError();
    if (err) { std::fprintf(stderr, "FAIL: GL error 0x%x\n", err); failed_expect++; }
    glFinish();
    psp_fx_end_frame();
    std::vector<uint8_t> rgba((size_t)W * H * 4), flip((size_t)W * H * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (int y = 0; y < H; y++) std::memcpy(&flip[(size_t)y * W * 4], &rgba[(size_t)(H - 1 - y) * W * 4], (size_t)W * 4);
    for (size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
    psp_png_encode_rgba(out_png, flip.data(), W, H);
    std::printf("matched %d/%zu samples, %d failures -> %s\n", matched, smp.size(), failed_expect, out_png);
    return failed_expect ? 1 : 0;
}
