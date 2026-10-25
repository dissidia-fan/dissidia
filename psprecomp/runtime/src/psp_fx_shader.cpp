// psp_fx_shader.cpp -- GL side of the FX shader overrides (psp_fx_shader.h,
// FX_SHADERS.md): compile the manifest's programs, load their PNG textures,
// bind program + uniforms + textures for a matching batch, GPU timing.
// Render thread only. Nothing here runs when there is no manifest:
// psp_fx_want_ids() is false and psp_ge_draw.cpp never calls in.
#include "psp_fx_shader.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "psp_ge_shader.h"
#include "psp_tex_pack.h"
#include "hle/psp_hle_io.h"

#include <glad/glad.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

using Clock = std::chrono::steady_clock;

std::string (*g_provider)() = nullptr;
int g_want = -1;            // -1 undecided, 0 off, 1 manifest present
int g_log = 0;              // PSPRECOMP_FX_SHADER_LOG
bool g_reload = false;      // PSPRECOMP_FX_SHADER_RELOAD
std::string g_dir;
Clock::time_point g_t0;
bool g_loaded = false;
long long g_manifest_mtime = 0;   // signature: manifest + shader file mtimes

psp_fx::Registry g_reg;
psp_fx::AgeTracker g_ages;
std::unordered_set<uint64_t> g_first_logged;   // ids whose first match was logged (without PSPRECOMP_FX_SHADER_LOG)

struct ProgInfo {
    ShaderUniforms ge{};
    GLint time, ticks, age, age_ticks, life, progress, world, view, proj, camera, origin, screen_size,
        viewport, has_world, has_normal, screen, env_scale;
    std::unordered_map<std::string, GLint> named;
    GLint loc(const std::string& n, GLuint prog) {
        auto it = named.find(n);
        if (it != named.end()) return it->second;
        GLint l = glGetUniformLocation(prog, n.c_str());
        named[n] = l;
        return l;
    }
};
std::unordered_map<GLuint, ProgInfo> g_prog;

struct Tex { GLuint id = 0; bool ok = false; };
std::unordered_map<std::string, Tex> g_tex;   // by relative path

// stats
uint64_t g_frames = 0, g_batches = 0, g_copies = 0;
std::unordered_set<uint64_t> g_seen_frame;      // hashes overridden this stats window
uint64_t g_screen_serial = 1, g_screen_done = 0;
GLuint g_screen_fbo_done = 0;
GLuint g_screen_tex_cached = 0;
int g_screen_w = 0, g_screen_h = 0;
Clock::time_point g_stats_t;
// GPU timing (log >= 2)
std::vector<GLuint> g_q_free, g_q_pending;
GLuint g_q_active = 0;
uint64_t g_gpu_ns = 0, g_gpu_n = 0;

double now_s() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

bool env_off(const char* n) { const char* v = std::getenv(n); return v && v[0] == '0'; }

std::string resolve_dir() {
    if (const char* e = std::getenv("PSPRECOMP_FX_SHADERS_DIR")) if (*e) return e;
    if (g_provider) return g_provider();
    std::string ms0 = psp_io_ms0_host_path();
    while (ms0.size() > 1 && ms0.back() == '/') ms0.pop_back();
    size_t slash = ms0.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : ms0.substr(0, slash)) + "/mods/fx_shaders";
}

long long file_mtime(const std::string& p) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return -1;
#ifdef __APPLE__
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

bool read_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

void log_line(const std::string& m) { std::fprintf(stderr, "[FXSHADER] %s\n", m.c_str()); }

GLuint compile_stage(GLenum type, const char* const* src, int n, std::string* log) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, n, src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string l((size_t)std::max(len, 1), '\0');
        glGetShaderInfoLog(s, (GLsizei)l.size(), nullptr, &l[0]);
        *log += (type == GL_VERTEX_SHADER ? "vertex: " : "fragment: ") + std::string(l.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

unsigned compile_program(const psp_fx::ProgramSource& ps, std::string* log) {
    const char* vs[3] = {ge_shader_vertex_version(), "#define PSP_FX 1\n", ge_shader_vertex_body()};
    GLuint v = compile_stage(GL_VERTEX_SHADER, vs, 3, log);
    const char* fs[1] = {ps.frag.c_str()};
    GLuint f = v ? compile_stage(GL_FRAGMENT_SHADER, fs, 1, log) : 0;
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char l[2048] = {};
        glGetProgramInfoLog(p, sizeof l, nullptr, l);
        *log += std::string("link: ") + l;
        glDeleteProgram(p);
        return 0;
    }
    ProgInfo pi{};
    ge_shader_query_locations(p, &pi.ge);
    auto L = [&](const char* n) { return glGetUniformLocation(p, n); };
    pi.time = L("u_fx_time"); pi.ticks = L("u_fx_ticks"); pi.age = L("u_fx_age");
    pi.age_ticks = L("u_fx_age_ticks"); pi.life = L("u_fx_life_ticks"); pi.progress = L("u_fx_progress");
    pi.world = L("u_fx_world"); pi.view = L("u_fx_view"); pi.proj = L("u_fx_proj");
    pi.camera = L("u_fx_camera"); pi.origin = L("u_fx_origin"); pi.screen_size = L("u_fx_screen_size");
    pi.viewport = L("u_fx_viewport"); pi.has_world = L("u_fx_has_world"); pi.has_normal = L("u_fx_has_normal");
    pi.screen = L("u_fx_screen");
    pi.env_scale = L("u_fx_env_scale");
    g_prog[p] = std::move(pi);
    return p;
}

void unload() {
    for (unsigned p : g_reg.programs()) glDeleteProgram(p);
    for (auto& kv : g_tex) if (kv.second.id) glDeleteTextures(1, &kv.second.id);
    g_prog.clear();
    g_tex.clear();
    g_reg.clear();
    ge_shader_note_external_program();
}

// Reload signature: the manifest's and every referenced shader file's mtime.
long long files_signature() {
    long long sig = file_mtime(g_dir + "/manifest.json");
    for (const auto& d : g_reg.manifest().draws) {
        sig = sig * 31 + file_mtime(g_dir + "/" + d.frag);
        for (const auto& inc : d.includes) sig = sig * 31 + file_mtime(g_dir + "/" + inc);
    }
    return sig;
}

void load() {
    g_loaded = true;
    const std::string mpath = g_dir + "/manifest.json";
    g_manifest_mtime = file_mtime(mpath);
    std::string text, err;
    if (!read_file(mpath, &text)) { log_line("cannot read " + mpath + ": FX shaders off"); return; }
    psp_fx::Manifest m;
    if (!psp_fx::parse_manifest(text, m, &err)) {
        log_line("manifest.json: " + err + ": FX shaders off");
        return;
    }
    for (const auto& w : m.warnings) log_line("manifest: " + w);
    const auto t0 = Clock::now();
    const int n = g_reg.build(
        m,
        [](const std::string& rel, std::string* data) { return read_file(g_dir + "/" + rel, data); },
        compile_program,
        log_line);
    g_manifest_mtime = files_signature();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    char b[256];
    std::snprintf(b, sizeof b, "%s: %d of %zu override(s) ready, %zu program(s), compiled in %.1f ms",
                  mpath.c_str(), n, m.draws.size(), g_reg.programs().size(), ms);
    log_line(b);
    if (g_log)
        for (const auto& d : g_reg.manifest().draws)
            for (uint64_t h : d.hashes)
                if (const auto* bd = g_reg.match(h); bd && bd->entry == &d)
                    log_line("  " + psp_fx::hash_str(h) + " -> " + d.frag + " (" + d.name + ")");
}

GLuint get_tex(const std::string& rel) {
    auto it = g_tex.find(rel);
    if (it != g_tex.end()) return it->second.id;
    Tex t;
    std::string data, err;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!read_file(g_dir + "/" + rel, &data)) {
        log_line("texture " + rel + " not found (sampler reads black)");
    } else if (!psp_png_decode_rgba((const uint8_t*)data.data(), data.size(), rgba, w, h, &err)) {
        log_line("texture " + rel + ": " + err);
    } else {
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        t.ok = true;
        if (g_log) log_line("texture " + rel + " " + std::to_string(w) + "x" + std::to_string(h));
    }
    g_tex[rel] = t;
    return t.id;
}

// GE 4x3 column-major -> mat4 column-major.
void pad43(const float* m, float* o) {
    for (int c = 0; c < 4; c++) {
        o[c * 4 + 0] = m[c * 3 + 0];
        o[c * 4 + 1] = m[c * 3 + 1];
        o[c * 4 + 2] = m[c * 3 + 2];
        o[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
}

}  // namespace

void psp_fx_set_dir_provider(std::string (*fn)()) { g_provider = fn; }

bool psp_fx_want_ids() {
    if (g_want >= 0) return g_want == 1;
    g_want = 0;
    g_t0 = Clock::now();
    g_stats_t = g_t0;
    if (const char* e = std::getenv("PSPRECOMP_FX_SHADER_LOG")) g_log = std::atoi(e);
    g_reload = std::getenv("PSPRECOMP_FX_SHADER_RELOAD") && !env_off("PSPRECOMP_FX_SHADER_RELOAD");
    if (env_off("PSPRECOMP_FX_SHADERS")) {
        std::fprintf(stderr, "[FXSHADER] disabled (PSPRECOMP_FX_SHADERS=0)\n");
        return false;
    }
    g_dir = resolve_dir();
    while (g_dir.size() > 1 && g_dir.back() == '/') g_dir.pop_back();
    // On only when the manifest has at least one enabled entry with a texture
    // id: otherwise texture binds compute no ids and draws never call in.
    std::string text, err;
    if (!read_file(g_dir + "/manifest.json", &text)) {
        if (g_log) std::fprintf(stderr, "[FXSHADER] no %s/manifest.json: off\n", g_dir.c_str());
        return false;
    }
    psp_fx::Manifest m;
    if (!psp_fx::parse_manifest(text, m, &err)) {
        std::fprintf(stderr, "[FXSHADER] %s/manifest.json: %s: off\n", g_dir.c_str(), err.c_str());
        return false;
    }
    size_t usable = 0;
    for (const auto& d : m.draws) if (d.enabled && !d.hashes.empty()) usable++;
    if (!usable) {
        std::fprintf(stderr, "[FXSHADER] %s/manifest.json has no enabled entry with a texture id yet: off\n",
                     g_dir.c_str());
        return false;
    }
    g_want = 1;
    std::fprintf(stderr, "[FXSHADER] %s/manifest.json: %zu override(s) listed (texture ids on)\n", g_dir.c_str(), usable);
    return true;
}

bool psp_fx_begin_batch(const PspFxDrawIn& in, PspFxDrawOut* out) {
    if (g_want != 1) return false;
    if (!g_loaded) load();
    if (!in.tex_id || g_reg.empty()) return false;
    const psp_fx::Registry::Bound* b = g_reg.match(in.tex_id);
    if (!b) return false;
    const psp_fx::DrawEntry& e = *b->entry;
    auto pit = g_prog.find(b->program);
    if (pit == g_prog.end()) return false;
    ProgInfo& pi = pit->second;
    const GeState& st = *in.state;

    const double t = now_s();
    bool fresh = false;
    const float age_ticks = g_ages.touch(in.tex_id, t, e.gap_ticks, &fresh) + e.age_offset_ticks;
    if (fresh && !g_log && g_first_logged.insert(in.tex_id).second)
        // Once per override per run even without PSPRECOMP_FX_SHADER_LOG: any log shows whether the ids matched.
        log_line("first match " + psp_fx::hash_str(in.tex_id) + " " + e.name + " (" + e.frag + ", " +
                 (in.xform ? "gpu-transform" : "passthrough") + " draw)");
    if (fresh && g_log)
        log_line("match " + psp_fx::hash_str(in.tex_id) + " " + e.name + " (" + e.frag + "): new instance at t=" +
                 std::to_string(t) + "s, " + (in.xform ? "gpu-transform" : "passthrough") + " draw");
    g_batches++;
    g_seen_frame.insert(in.tex_id);

    glUseProgram(b->program);
    ge_shader_note_external_program();
    // GE 2*As emulation only applies while the game's own blend is used.
    ge_shader_upload_to(pi.ge, st, in.xform, in.alpha_double && e.blend == psp_fx::Blend::Game);

    auto u1f = [](GLint l, float v) { if (l >= 0) glUniform1f(l, v); };
    u1f(pi.time, (float)t);
    u1f(pi.ticks, (float)(t * 60.0));
    u1f(pi.age_ticks, age_ticks);
    u1f(pi.age, age_ticks / 60.0f);
    u1f(pi.life, e.life_ticks);
    u1f(pi.env_scale, (in.alpha_double && e.blend != psp_fx::Blend::Game) ? 2.0f : 1.0f);
    float prog = e.life_ticks > 0.0f ? age_ticks / e.life_ticks : 0.0f;
    u1f(pi.progress, prog < 0.0f ? 0.0f : (prog > 1.0f ? 1.0f : prog));
    float m4[16];
    pad43(st.world_matrix, m4);
    if (pi.world >= 0) glUniformMatrix4fv(pi.world, 1, GL_FALSE, m4);
    if (pi.origin >= 0) glUniform3f(pi.origin, st.world_matrix[9], st.world_matrix[10], st.world_matrix[11]);
    pad43(st.view_matrix, m4);
    if (pi.view >= 0) glUniformMatrix4fv(pi.view, 1, GL_FALSE, m4);
    if (pi.proj >= 0) glUniformMatrix4fv(pi.proj, 1, GL_FALSE, st.proj_matrix);
    if (pi.camera >= 0) {
        // view = R p + t  ->  eye = -R^T t (R columns are view_matrix[0..8]).
        const float* v = st.view_matrix;
        float c[3];
        for (int j = 0; j < 3; j++) c[j] = -(v[j * 3 + 0] * v[9] + v[j * 3 + 1] * v[10] + v[j * 3 + 2] * v[11]);
        glUniform3f(pi.camera, c[0], c[1], c[2]);
    }
    if (pi.screen_size >= 0) glUniform2f(pi.screen_size, (float)in.target_w, (float)in.target_h);
    if (pi.viewport >= 0) glUniform4f(pi.viewport, (float)in.vp[0], (float)in.vp[1], (float)in.vp[2], (float)in.vp[3]);
    if (pi.has_world >= 0) glUniform1i(pi.has_world, in.xform ? 1 : 0);
    if (pi.has_normal >= 0) glUniform1i(pi.has_normal, ge_vtype_nrm(st.vertex_type) != GE_VTYPE_NRM_NONE ? 1 : 0);

    for (const auto& uv : e.uniforms) {
        const GLint l = pi.loc(uv.name, b->program);
        if (l < 0) continue;
        switch (uv.n) {
        case 1: glUniform1f(l, uv.v[0]); break;
        case 2: glUniform2f(l, uv.v[0], uv.v[1]); break;
        case 3: glUniform3f(l, uv.v[0], uv.v[1], uv.v[2]); break;
        case 4: glUniform4f(l, uv.v[0], uv.v[1], uv.v[2], uv.v[3]); break;
        case 16: glUniformMatrix4fv(l, 1, GL_FALSE, uv.v); break;
        default: break;
        }
    }

    // Extra textures on units 1..n, the screen copy after them. Unit 0 keeps
    // the draw's own texture (bound by ge_texture_bind) and is left active.
    int unit = 1;
    for (const auto& tb : e.textures) {
        const GLint l = pi.loc(tb.uniform, b->program);
        if (l < 0) continue;
        glActiveTexture(GL_TEXTURE0 + unit);
        const GLuint id = get_tex(tb.path);
        glBindTexture(GL_TEXTURE_2D, id);
        if (id) {
            const GLint wrap = tb.clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        }
        glUniform1i(l, unit);
        unit++;
    }
    if (e.screen && pi.screen >= 0 && in.screen_copy) {
        glActiveTexture(GL_TEXTURE0 + unit);
        GLint cur_fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur_fbo);
        // One copy per presented frame and render target: later overrides in
        // the same frame see the frame as it was at the first one.
        if (g_screen_done != g_screen_serial || g_screen_fbo_done != (GLuint)cur_fbo || !g_screen_tex_cached) {
            g_screen_tex_cached = in.screen_copy(&g_screen_w, &g_screen_h);
            g_screen_done = g_screen_serial;
            g_screen_fbo_done = (GLuint)cur_fbo;
            g_copies++;
        }
        glBindTexture(GL_TEXTURE_2D, g_screen_tex_cached);
        glUniform1i(pi.screen, unit);
    } else if (pi.screen >= 0) {
        glUniform1i(pi.screen, 0);
    }
    glActiveTexture(GL_TEXTURE0);

    out->blend = e.blend;
    out->depth = e.depth;
    out->timed = g_log >= 2;
    return true;
}

void psp_fx_timer_begin() {
    if (g_q_active) return;
    if (g_q_free.empty()) {
        GLuint q[8];
        glGenQueries(8, q);
        g_q_free.assign(q, q + 8);
    }
    g_q_active = g_q_free.back();
    g_q_free.pop_back();
    glBeginQuery(GL_TIME_ELAPSED, g_q_active);
}

void psp_fx_timer_end() {
    if (!g_q_active) return;
    glEndQuery(GL_TIME_ELAPSED);
    g_q_pending.push_back(g_q_active);
    g_q_active = 0;
}

void psp_fx_end_frame() {
    if (g_want != 1) return;
    g_screen_serial++;
    g_frames++;
    // Collect finished GPU timer queries.
    for (size_t i = 0; i < g_q_pending.size();) {
        GLint avail = 0;
        glGetQueryObjectiv(g_q_pending[i], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) { i++; continue; }
        GLuint64 ns = 0;
        glGetQueryObjectui64v(g_q_pending[i], GL_QUERY_RESULT, &ns);
        g_gpu_ns += ns;
        g_gpu_n++;
        g_q_free.push_back(g_q_pending[i]);
        g_q_pending[i] = g_q_pending.back();
        g_q_pending.pop_back();
    }
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - g_stats_t).count();
    if (dt >= 1.0) {
        if (g_log && g_batches) {
            char b[256];
            int n = std::snprintf(b, sizeof b, "stats: %.1f frames/s, %.1f override batches/frame (%zu textures), %.2f screen copies/frame",
                                  g_frames / dt, (double)g_batches / std::max<uint64_t>(g_frames, 1), g_seen_frame.size(),
                                  (double)g_copies / std::max<uint64_t>(g_frames, 1));
            if (g_log >= 2 && g_gpu_n)
                std::snprintf(b + n, sizeof b - (size_t)n, ", GPU %.3f ms/frame (%.1f us/batch)",
                              g_gpu_ns / 1e6 / std::max<uint64_t>(g_frames, 1), g_gpu_ns / 1e3 / g_gpu_n);
            log_line(b);
        }
        g_frames = g_batches = g_copies = g_gpu_ns = g_gpu_n = 0;
        g_seen_frame.clear();
        g_stats_t = now;
        if (g_reload && g_loaded) {
            const long long mt = files_signature();
            if (mt != g_manifest_mtime) {
                log_line("manifest.json or a shader changed: reloading");
                unload();
                load();
            }
        }
    }
}

void psp_fx_shutdown() {
    if (g_want != 1) return;
    unload();
    for (GLuint q : g_q_free) glDeleteQueries(1, &q);
    g_q_free.clear();
}
