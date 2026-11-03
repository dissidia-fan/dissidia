// char_light_preview -- headless before/after of the character lighting (EGL, Mesa: Linux / Steam Deck desktop
// mode), built and run by games/dissidia/tools/char_light_preview.sh. Links the runtime's own uber-shader and FX
// shader code, so both rows are drawn exactly as the game draws them:
//   top row    the uber-shader with the game's character state (lit, ambient 0x808080 x vertex colour, 2x texture)
//   bottom row the same draws through psp_fx_begin_batch (fx_shaders/lit/character.frag)
// from several camera angles around the body (bind pose).
//   char_light_preview <mesh.bin> <out.png> [degrees...]   (PSPRECOMP_FX_SHADERS_DIR = a folder with manifest.json)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "psp_ge_shader.h"
#include "psp_fx_shader.h"
#include "psp_tex_pack.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

std::string psp_io_ms0_host_path() { return "/tmp/ms0x/ms0"; }

struct V { float p[4], uv[2], c[4], n[3]; };
struct Tex { unsigned long long id; GLuint gl; std::vector<V> v; };

static void pack43(const float m[16], float* o) {
    for (int c = 0; c < 4; c++) for (int r = 0; r < 3; r++) o[c * 3 + r] = m[c * 4 + r];
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: char_light_preview mesh.bin out.png [deg...]\n"); return 2; }
    std::vector<float> angles;
    for (int i = 3; i < argc; i++) angles.push_back((float)std::atof(argv[i]));
    if (angles.empty()) angles = {0, 35, 90, 150, 200};

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
    std::printf("GL: %s\n", glGetString(GL_RENDERER));
    ge_shader_init();
    if (!ge_shader_get_program()) { std::fprintf(stderr, "uber-shader failed\n"); return 1; }
    psp_fx_want_ids();

    // mesh
    std::vector<Tex> texs;
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) { std::fprintf(stderr, "no %s\n", argv[1]); return 2; }
        uint32_t nt = 0; if (std::fread(&nt, 4, 1, f) != 1) return 2;
        for (uint32_t t = 0; t < nt; t++) {
            Tex tx{}; uint32_t w, h;
            if (std::fread(&tx.id, 8, 1, f) != 1 || std::fread(&w, 4, 1, f) != 1 || std::fread(&h, 4, 1, f) != 1) return 2;
            std::vector<uint8_t> px((size_t)w * h * 4);
            if (std::fread(px.data(), 1, px.size(), f) != px.size()) return 2;
            glGenTextures(1, &tx.gl); glBindTexture(GL_TEXTURE_2D, tx.gl);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            uint32_t ntri = 0; if (std::fread(&ntri, 4, 1, f) != 1) return 2;
            std::vector<float> c((size_t)ntri * 3 * 12);
            if (std::fread(c.data(), 4, c.size(), f) != c.size()) return 2;
            for (size_t i = 0; i < (size_t)ntri * 3; i++) {
                const float* s = &c[i * 12];
                V q{}; q.p[0] = s[0]; q.p[1] = s[1]; q.p[2] = s[2]; q.p[3] = 1;
                q.n[0] = s[3]; q.n[1] = s[4]; q.n[2] = s[5]; q.uv[0] = s[6]; q.uv[1] = s[7];
                for (int k = 0; k < 4; k++) q.c[k] = s[8 + k];
                for (int k = 0; k < 3; k++) { lo[k] = std::fmin(lo[k], s[k]); hi[k] = std::fmax(hi[k], s[k]); }
                tx.v.push_back(q);
            }
            texs.push_back(std::move(tx));
        }
        std::fclose(f);
    }
    bool has_nrm = false;
    for (auto& t : texs) for (auto& v : t.v) if (v.n[0] != 0 || v.n[1] != 0 || v.n[2] != 0) { has_nrm = true; break; }

    const int TW = 300, TH = 420, COLS = (int)angles.size(), ROWS = 2;
    const int W = TW * COLS, H = TH * ROWS;
    GLuint fbo, ctex, dep;
    glGenTextures(1, &ctex); glBindTexture(GL_TEXTURE_2D, ctex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenRenderbuffers(1, &dep); glBindRenderbuffer(GL_RENDERBUFFER, dep);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, dep);
    // a stage-like backdrop: dusk gradient per tile is too much; flat dark blue-grey
    glClearColor(0.16f, 0.17f, 0.21f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, p)); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, uv)); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, c)); glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)offsetof(V, n)); glEnableVertexAttribArray(3);

    const float cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
    const float hgt = hi[1] - lo[1];
    float world4[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float fov = 35.0f * 3.14159265f / 180.0f, fy = 1.0f / std::tan(fov / 2), zn = 0.05f, zf = 50.0f;
    const float aspect = (float)TW / TH;
    float proj[16] = {fy / aspect,0,0,0, 0,fy,0,0, 0,0,(zf+zn)/(zn-zf),-1, 0,0,2*zf*zn/(zn-zf),0};

    GeState st; std::memset(&st, 0, sizeof st);
    st.texture_enable = true; st.tex_func = 0; st.tex_func_rgba = true; st.tex_func_double = true;
    st.lighting_enable = true; st.material_update = 1; st.ambient_color = 0x808080; st.ambient_alpha = 0xFF;
    st.material_alpha = 0xFF;
    st.vertex_type = (uint32_t)((has_nrm ? GE_VTYPE_NRM_8BIT : GE_VTYPE_NRM_NONE) << GE_VTYPE_NRM_SHIFT);
    pack43(world4, st.world_matrix); std::memcpy(st.proj_matrix, proj, sizeof proj);

    int matched = 0, draws = 0;
    for (int row = 0; row < ROWS; row++)
    for (int col = 0; col < COLS; col++) {
        // CL_ZOOM (default 1) / CL_FOCUS (height fraction to frame, default the middle): e.g. 4 / 0.9 for the head
        const float zoom = std::getenv("CL_ZOOM") ? (float)std::atof(std::getenv("CL_ZOOM")) : 1.0f;
        const float ty = std::getenv("CL_FOCUS") ? lo[1] + hgt * (float)std::atof(std::getenv("CL_FOCUS")) : cy;
        const float a = angles[col] * 3.14159265f / 180.0f, dist = hgt * 2.2f / zoom;
        const float ex = cx + std::sin(a) * dist, ey = ty + hgt * 0.08f / zoom, ez = cz + std::cos(a) * dist;
        float fz[3] = {ex - cx, ey - ty, ez - cz}; float l = std::sqrt(fz[0]*fz[0]+fz[1]*fz[1]+fz[2]*fz[2]);
        for (float& v : fz) v /= l;
        float up[3] = {0, 1, 0};
        float fx_[3] = {up[1]*fz[2]-up[2]*fz[1], up[2]*fz[0]-up[0]*fz[2], up[0]*fz[1]-up[1]*fz[0]};
        l = std::sqrt(fx_[0]*fx_[0]+fx_[1]*fx_[1]+fx_[2]*fx_[2]); for (float& v : fx_) v /= l;
        float fyv[3] = {fz[1]*fx_[2]-fz[2]*fx_[1], fz[2]*fx_[0]-fz[0]*fx_[2], fz[0]*fx_[1]-fz[1]*fx_[0]};
        float view4[16] = {fx_[0], fyv[0], fz[0], 0, fx_[1], fyv[1], fz[1], 0, fx_[2], fyv[2], fz[2], 0,
                           -(fx_[0]*ex+fx_[1]*ey+fx_[2]*ez), -(fyv[0]*ex+fyv[1]*ey+fyv[2]*ez), -(fz[0]*ex+fz[1]*ey+fz[2]*ez), 1};
        pack43(view4, st.view_matrix);
        GeXform3D x{}; x.world = st.world_matrix; x.view = st.view_matrix; x.proj = st.proj_matrix;
        x.uvxf[0] = 1; x.uvxf[1] = 1; x.lit = 1; x.lflags = 1 | (has_nrm ? 4 : 0); x.nlights = 0;
        x.lc[1][3] = 1.0f; x.lc[3][0] = x.lc[3][1] = x.lc[3][2] = 128.0f / 255.0f; x.lc[3][3] = 1.0f;
        int vp[4] = {col * TW, H - (row + 1) * TH, TW, TH};
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        for (auto& t : texs) {
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t.gl);
            ge_shader_use(); ge_shader_invalidate_cache(); ge_shader_set_uniforms(st); ge_shader_set_tex_remap(1, 1, false); ge_shader_set_xform_3d(x);
            if (row == 1) {
                PspFxDrawIn in{}; in.tex_id = t.id; in.state = &st; in.xform = &x;
                in.target_w = W; in.target_h = H; std::memcpy(in.vp, vp, sizeof vp);
                PspFxDrawOut out{};
                draws++;
                if (psp_fx_begin_batch(in, &out)) matched++;
                else std::fprintf(stderr, "NO MATCH for %016llx\n", t.id);
            }
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(t.v.size() * sizeof(V)), t.v.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)t.v.size());
        }
    }
    GLenum err = glGetError();
    if (err) { std::fprintf(stderr, "GL error 0x%x\n", err); }
    glFinish();
    psp_fx_end_frame();
    std::vector<uint8_t> rgba((size_t)W * H * 4), flip((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    for (int y = 0; y < H; y++) std::memcpy(&flip[(size_t)y * W * 4], &rgba[(size_t)(H - 1 - y) * W * 4], (size_t)W * 4);
    for (size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
    psp_png_encode_rgba(argv[2], flip.data(), W, H);
    std::printf("normals %s, FX matched %d/%d -> %s\n", has_nrm ? "yes" : "no", matched, draws, argv[2]);
    return (err || matched != draws) ? 1 : 0;
}
