#include "psp_ge_shader.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"

#include <glad/glad.h>
#include <cstdio>
#include <cstring>

// ---- Shader source ----

// The vertex stage is shared with the FX shader overrides (psp_fx_shader.cpp),
// which compile the same body with PSP_FX defined to get extra outputs. The
// #version line is passed as a separate source string so a #define can go
// between it and the body.
static const char* const k_vertex_version = "#version 330 core\n";
static const char* const k_vertex_shader_src = R"GLSL(
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_texcoord;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec3 a_normal;
layout(location = 4) in vec4 a_weights0;
layout(location = 5) in vec4 a_weights1;

out vec2 v_texcoord;
out vec4 v_color;
#ifdef PSP_FX
out vec3 v_fx_opos;     // model space (clip-space xyz on the passthrough path)
out vec3 v_fx_wpos;     // world space (0 on the passthrough path)
out vec3 v_fx_wnormal;  // world-space normal (0 without vertex normals)
out vec2 v_fx_uv;       // raw vertex UV
#endif

// u_xform == 0: a_position is already the final clip-space position (CPU
// transformed: through mode, rectangles, degenerate-matrix fallback).
// u_xform == 1: a_position is model space; the GE transform pipeline runs
// here (bone skinning -> world -> lighting -> view -> proj -> tex xform).
uniform int u_xform;
// GE matrices are 4x3; they are uploaded padded to square mat4 (last row
// 0,0,0,1). Square mat4 uniforms and plain vec4 math are the best-trodden
// path on every driver (Apple's GLSL compiler has a history of non-square
// matrix bugs); the padding costs 4 floats per matrix.
uniform mat4 u_world;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec4 u_uvxf;          // scale u, scale v, offset u, offset v
uniform int u_skin;           // number of skinning weights used (0 = none)
uniform mat4 u_bones[8];
uniform int u_lit;            // GE lighting enabled
uniform int u_lflags;         // bit0 matAmbient<-vertex, bit1 matDiffuse<-vertex, bit2 has normal
uniform int u_nl;             // number of enabled lights (packed first)
// u_lc: 0 emissive.rgb, 1 material ambient rgb + alpha, 2 material diffuse rgb,
//       3 global ambient rgb + ambient alpha
uniform vec4 u_lc[4];
// u_lt per light: 5 x vec4: pos.xyz+kind, dir.xyz+spotExp, att.xyz+spotCut,
//                 ambient.rgb, diffuse.rgb
uniform vec4 u_lt[20];

vec4 ge_lighting(vec3 wpos, vec3 nrm, vec4 vcol) {
    bool upd_amb = (u_lflags & 1) != 0;
    bool upd_dif = (u_lflags & 2) != 0;
    bool have_n  = (u_lflags & 4) != 0;
    vec3 mamb = upd_amb ? vcol.rgb : u_lc[1].rgb;
    float malpha = upd_amb ? vcol.a : u_lc[1].a;
    vec3 mdif = upd_dif ? vcol.rgb : u_lc[2].rgb;
    vec3 outc = u_lc[0].rgb + u_lc[3].rgb * mamb;
    float alpha = u_lc[3].a * malpha;

    vec3 n = vec3(0.0);
    if (have_n) {
        n = mat3(u_world) * nrm;
        float len = length(n);
        if (len > 1e-8) n /= len;
    }
    for (int i = 0; i < u_nl; i++) {
        vec4 lp = u_lt[i * 5 + 0];
        vec4 ld = u_lt[i * 5 + 1];
        vec4 la = u_lt[i * 5 + 2];
        int kind = int(lp.w + 0.5);
        vec3 L;
        float scale = 1.0;
        if (kind == 0) {
            L = lp.xyz;
            float len = length(L);
            if (len > 1e-8) L /= len;
        } else {
            L = lp.xyz - wpos;
            float d = length(L);
            if (d > 1e-8) L /= d;
            float att = la.x + la.y * d + la.z * d * d;
            scale = att > 1e-8 ? min(1.0 / att, 1.0) : 1.0;
            if (kind == 2) {
                float dl = length(ld.xyz);
                float ang = 0.0;
                if (dl > 1e-8) ang = -dot(L, ld.xyz) / dl;
                if (ang >= la.w) {
                    float a = max(ang, 0.0);
                    scale *= (ld.w == 0.0) ? 1.0 : pow(a, ld.w);
                } else {
                    scale = 0.0;
                }
            }
        }
        float ndl = have_n ? max(dot(n, L), 0.0) : 0.0;
        outc += scale * (u_lt[i * 5 + 3].rgb * mamb + u_lt[i * 5 + 4].rgb * mdif * ndl);
    }
    // Quantise to 8 bits exactly like the CPU path did.
    outc = floor(clamp(outc, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
    alpha = floor(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
    return vec4(outc, alpha);
}

void main() {
    if (u_xform == 0) {
        gl_Position = a_position;
        v_texcoord = a_texcoord;
        v_color = a_color;
#ifdef PSP_FX
        v_fx_opos = a_position.xyz;
        v_fx_wpos = vec3(0.0);
        v_fx_wnormal = vec3(0.0);
        v_fx_uv = a_texcoord;
#endif
        return;
    }
    vec3 p = a_position.xyz;
    vec3 n = a_normal;
    if (u_skin > 0) {
        vec3 sp = vec3(0.0);
        vec3 sn = vec3(0.0);
        for (int k = 0; k < u_skin; k++) {
            float w = (k < 4) ? a_weights0[k] : a_weights1[k - 4];
            if (w == 0.0) continue;
            mat4 m = u_bones[k];
            sp += w * (m * vec4(p, 1.0)).xyz;
            sn += w * (mat3(m) * n);
        }
        p = sp;
        n = sn;
    }
    vec3 wpos = (u_world * vec4(p, 1.0)).xyz;
    vec4 col = a_color;
    if (u_lit != 0) col = ge_lighting(wpos, n, a_color);
    vec3 vpos = (u_view * vec4(wpos, 1.0)).xyz;
    gl_Position = u_proj * vec4(vpos, 1.0);
    v_texcoord = a_texcoord * u_uvxf.xy + u_uvxf.zw;
    v_color = col;
#ifdef PSP_FX
    v_fx_opos = p;
    v_fx_wpos = wpos;
    v_fx_wnormal = mat3(u_world) * n;
    v_fx_uv = a_texcoord;
#endif
}
)GLSL";

static const char* const k_fragment_shader_src = R"GLSL(
#version 330 core

in vec2 v_texcoord;
in vec4 v_color;

uniform bool u_texture_enable;
uniform sampler2D u_texture;
uniform int u_tex_func;
uniform bool u_tex_rgba;
uniform bool u_tex_double;
uniform vec3 u_tex_env;
uniform bool u_alpha_test_enable;
uniform int u_alpha_test_ref;    // 0..255
uniform int u_alpha_test_mask;   // 0..255
uniform int u_alpha_test_func;
uniform bool u_color_test_enable;
uniform int u_color_test_func;   // 0 NEVER 1 ALWAYS 2 EQUAL 3 NOTEQUAL
uniform ivec3 u_color_test_ref;  // 0..255 per channel
uniform ivec3 u_color_test_mask;
uniform bool u_alpha_double;
// Stencil-to-alpha: >= 0 replaces the fragment alpha (GE stencil REPLACE/ZERO).
uniform float u_alpha_override;   // 0 = none, else 1 + replacement alpha (default-safe)
// FBO-as-texture remap: (sx, sy, flip, 0). Identity = (1,1,0,0).
uniform vec4 u_tex_remap;

out vec4 frag_color;

void main() {
    vec4 color = v_color;

    if (u_texture_enable) {
        vec2 tc = v_texcoord * u_tex_remap.xy;
        if (u_tex_remap.z > 0.5) tc.y = 1.0 - tc.y;
        vec4 tex = texture(u_texture, tc);

        // Texture function (GE tex func), PPSSPP semantics: the RGBA
        // bit says whether texture alpha participates; colour doubling
        // scales the result (0x80 == 1.0 convention, used by Square Enix).
        if (u_tex_func == 0) {
            // MODULATE
            color.rgb = color.rgb * tex.rgb;
            if (u_tex_rgba) color.a *= tex.a;
        } else if (u_tex_func == 1) {
            // DECAL
            color.rgb = u_tex_rgba ? mix(color.rgb, tex.rgb, tex.a) : tex.rgb;
        } else if (u_tex_func == 2) {
            // BLEND with env colour
            color.rgb = mix(color.rgb, u_tex_env, tex.rgb);
            if (u_tex_rgba) color.a *= tex.a;
        } else if (u_tex_func == 3) {
            // REPLACE
            color.rgb = tex.rgb;
            if (u_tex_rgba) color.a = tex.a;
        } else if (u_tex_func == 4) {
            // ADD
            color.rgb = color.rgb + tex.rgb;
            if (u_tex_rgba) color.a *= tex.a;
        }
        if (u_tex_double) color.rgb = min(color.rgb * 2.0, vec3(1.0));
    }

    // Alpha test: (alpha & mask) <func> (ref & mask), integer 0..255.
    if (u_alpha_test_enable) {
        int a = int(clamp(color.a, 0.0, 1.0) * 255.0 + 0.5) & u_alpha_test_mask;
        int ref = u_alpha_test_ref & u_alpha_test_mask;
        bool pass = true;

        if (u_alpha_test_func == 0) {
            pass = false;                  // NEVER
        } else if (u_alpha_test_func == 1) {
            pass = true;                   // ALWAYS
        } else if (u_alpha_test_func == 2) {
            pass = (a == ref);             // EQUAL
        } else if (u_alpha_test_func == 3) {
            pass = (a != ref);             // NOTEQUAL
        } else if (u_alpha_test_func == 4) {
            pass = (a < ref);              // LESS
        } else if (u_alpha_test_func == 5) {
            pass = (a <= ref);             // LEQUAL
        } else if (u_alpha_test_func == 6) {
            pass = (a > ref);              // GREATER
        } else if (u_alpha_test_func == 7) {
            pass = (a >= ref);             // GEQUAL
        }

        if (!pass) discard;
    }

    // Colour test: (rgb & mask) <func> (ref & mask), compared as a whole
    // 24-bit colour; only NEVER/ALWAYS/EQUAL/NOTEQUAL exist.
    if (u_color_test_enable) {
        ivec3 c = ivec3(clamp(color.rgb, vec3(0.0), vec3(1.0)) * 255.0 + 0.5)
                  & u_color_test_mask;
        ivec3 r = u_color_test_ref & u_color_test_mask;
        bool eq = (c == r);
        bool pass = true;
        if (u_color_test_func == 0) pass = false;
        else if (u_color_test_func == 2) pass = eq;
        else if (u_color_test_func == 3) pass = !eq;
        if (!pass) discard;
    }

    // GE 2*src-alpha blend factors: GL_SRC_ALPHA with doubled, clamped alpha.
    if (u_alpha_double) color.a = min(color.a * 2.0, 1.0);
    if (u_alpha_override > 0.5) color.a = u_alpha_override - 1.0;

    frag_color = color;
}
)GLSL";

// ---- Module state ----
static GLuint g_program = 0;
static GLuint g_used_program = 0;
static ShaderUniforms g_uniforms{};

// ---- Uniform value cache ----
// Redundant glUniform* calls are a measurable per-draw cost (thousands of
// draws per frame); each uniform keeps its last uploaded bytes and skips the
// GL call when nothing changed. Indexed by uniform location.
struct UCache { unsigned char data[512]; unsigned size; bool valid; };
static UCache g_ucache[128];
static bool u_changed(GLint loc, const void* d, size_t n) {
    if (loc < 0) return false;
    if (loc >= 128 || n > sizeof(g_ucache[0].data)) return true;  // uncached
    UCache& c = g_ucache[loc];
    if (c.valid && c.size == n && std::memcmp(c.data, d, n) == 0) return false;
    std::memcpy(c.data, d, n);
    c.size = (unsigned)n;
    c.valid = true;
    return true;
}
void ge_shader_invalidate_cache() { std::memset(g_ucache, 0, sizeof g_ucache); }
static void u1i(GLint loc, int v) { if (u_changed(loc, &v, sizeof v)) glUniform1i(loc, v); }
static void u3i(GLint loc, int a, int b, int c) {
    int v[3] = {a, b, c};
    if (u_changed(loc, v, sizeof v)) glUniform3i(loc, a, b, c);
}
static void u3f(GLint loc, float a, float b, float c) {
    float v[3] = {a, b, c};
    if (u_changed(loc, v, sizeof v)) glUniform3f(loc, a, b, c);
}
static void u4f(GLint loc, float a, float b, float c, float d) {
    float v[4] = {a, b, c, d};
    if (u_changed(loc, v, sizeof v)) glUniform4f(loc, a, b, c, d);
}

// ---- Helpers ----

static GLuint compile_shader_n(GLenum type, const char* const* srcs, int n);
static GLuint compile_shader(
    GLenum type, const char* src
) {
    return compile_shader_n(type, &src, 1);
}
static GLuint compile_shader_n(GLenum type, const char* const* srcs, int n) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, n, srcs, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log),
                           nullptr, log);
        std::fprintf(stderr,
            "[SHADER] Compile error (%s):\n%s\n",
            type == GL_VERTEX_SHADER ? "vert" : "frag",
            log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// ---- Public API ----

void ge_shader_init() {
    const char* const vsrc[2] = {k_vertex_version, k_vertex_shader_src};
    GLuint vert = compile_shader_n(GL_VERTEX_SHADER, vsrc, 2);
    GLuint frag = compile_shader(
        GL_FRAGMENT_SHADER, k_fragment_shader_src);

    if (!vert || !frag) {
        std::fprintf(stderr,
            "[SHADER] Failed to compile shaders\n");
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return;
    }

    g_program = glCreateProgram();
    glAttachShader(g_program, vert);
    glAttachShader(g_program, frag);
    {
        // Capture the vertex-stage outputs for PSPRECOMP_GE_VERIFY (no cost
        // unless a transform-feedback pass is actually run).
        static const char* const tf_varyings[] = {"gl_Position", "v_texcoord", "v_color"};
        glTransformFeedbackVaryings(g_program, 3, tf_varyings, GL_INTERLEAVED_ATTRIBS);
    }
    glLinkProgram(g_program);

    GLint success = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(g_program, sizeof(log),
                            nullptr, log);
        std::fprintf(stderr,
            "[SHADER] Link error:\n%s\n", log);
        glDeleteProgram(g_program);
        g_program = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    if (g_program) {
        ge_shader_query_locations(g_program, &g_uniforms);
        ge_shader_invalidate_cache();

        std::fprintf(stderr,
            "[SHADER] Uber-shader compiled and linked "
            "(program=%u)\n", g_program);
    }
}

void ge_shader_shutdown() {
    if (g_program) {
        glDeleteProgram(g_program);
        g_program = 0;
        g_used_program = 0;
    }
}

void ge_shader_use() {
    if (g_program && g_used_program != g_program) {
        glUseProgram(g_program);
        g_used_program = g_program;
    }
}

void ge_shader_set_uniforms(const GeState& state) {
    if (!g_program) return;

    u1i(g_uniforms.u_texture_enable, state.texture_enable ? 1 : 0);
    u1i(g_uniforms.u_texture, 0);  // unit 0
    u1i(g_uniforms.u_tex_func, static_cast<int>(state.tex_func));
    u1i(g_uniforms.u_tex_rgba, state.tex_func_rgba ? 1 : 0);
    u1i(g_uniforms.u_tex_double, state.tex_func_double ? 1 : 0);
    u3f(g_uniforms.u_tex_env,
        (state.tex_env_color & 0xFF) / 255.0f,
        ((state.tex_env_color >> 8) & 0xFF) / 255.0f,
        ((state.tex_env_color >> 16) & 0xFF) / 255.0f);

    // Alpha test
    u1i(g_uniforms.u_alpha_test_enable, state.alpha_test_enable ? 1 : 0);
    if (state.alpha_test_enable) {
        int func = state.alpha_test & 0x7;
        int ref_byte = (state.alpha_test >> 8) & 0xFF;
        int mask_byte = (state.alpha_test >> 16) & 0xFF;
        u1i(g_uniforms.u_alpha_test_ref, ref_byte);
        u1i(g_uniforms.u_alpha_test_mask, mask_byte);
        u1i(g_uniforms.u_alpha_test_func, func);
    }

    // Colour test (0xBBGGRR ref/mask)
    u1i(g_uniforms.u_color_test_enable, state.color_test_enable ? 1 : 0);
    if (state.color_test_enable) {
        const uint32_t r = state.color_ref, m = state.color_test_mask;
        u1i(g_uniforms.u_color_test_func, state.color_test & 0x3);
        u3i(g_uniforms.u_color_test_ref,
            r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF);
        u3i(g_uniforms.u_color_test_mask,
            m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF);
    }
}

void ge_shader_set_tex_remap(float sx, float sy, bool flip) {
    if (!g_program) return;
    u4f(g_uniforms.u_tex_remap, sx, sy, flip ? 1.0f : 0.0f, 0.0f);
}

static void u1f(GLint loc, float v) { if (u_changed(loc, &v, sizeof v)) glUniform1f(loc, v); }
void ge_shader_set_alpha_override(float a) { u1f(g_uniforms.u_alpha_override, a >= 0.0f ? a + 1.0f : 0.0f); }

void ge_shader_set_alpha_double(bool enable) {
    if (!g_program) return;
    u1i(g_uniforms.u_alpha_double, enable ? 1 : 0);
}

void ge_shader_set_xform_passthrough() {
    if (!g_program) return;
    u1i(g_uniforms.u_xform, 0);
}

// 4x3 column-major (12 floats) -> padded 4x4 column-major (16 floats).
static inline void pad43(const float* m, float* o) {
    for (int c = 0; c < 4; c++) {
        o[c * 4 + 0] = m[c * 3 + 0];
        o[c * 4 + 1] = m[c * 3 + 1];
        o[c * 4 + 2] = m[c * 3 + 2];
        o[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
}

void ge_shader_set_xform_3d(const GeXform3D& x) {
    if (!g_program) return;
    u1i(g_uniforms.u_xform, 1);
    float m4[16];
    pad43(x.world, m4);
    if (u_changed(g_uniforms.u_world, m4, sizeof m4))
        glUniformMatrix4fv(g_uniforms.u_world, 1, GL_FALSE, m4);
    pad43(x.view, m4);
    if (u_changed(g_uniforms.u_view, m4, sizeof m4))
        glUniformMatrix4fv(g_uniforms.u_view, 1, GL_FALSE, m4);
    if (u_changed(g_uniforms.u_proj, x.proj, 16 * sizeof(float)))
        glUniformMatrix4fv(g_uniforms.u_proj, 1, GL_FALSE, x.proj);
    u4f(g_uniforms.u_uvxf, x.uvxf[0], x.uvxf[1], x.uvxf[2], x.uvxf[3]);
    u1i(g_uniforms.u_skin, x.skin_n);
    if (x.skin_n > 0) {
        // Only the bones in use are uploaded (up to 8 mat4 = 512 bytes, the
        // u_changed cache limit).
        float b4[8 * 16];
        for (int k = 0; k < x.skin_n; k++) pad43(x.bones + k * 12, b4 + k * 16);
        if (u_changed(g_uniforms.u_bones, b4, (size_t)x.skin_n * 16 * sizeof(float)))
            glUniformMatrix4fv(g_uniforms.u_bones, x.skin_n, GL_FALSE, b4);
    }
    u1i(g_uniforms.u_lit, x.lit);
    if (x.lit) {
        u1i(g_uniforms.u_lflags, x.lflags);
        u1i(g_uniforms.u_nl, x.nlights);
        if (u_changed(g_uniforms.u_lc, x.lc, sizeof x.lc))
            glUniform4fv(g_uniforms.u_lc, 4, &x.lc[0][0]);
        if (x.nlights > 0
            && u_changed(g_uniforms.u_lt, x.lt, (size_t)x.nlights * 5 * 4 * sizeof(float)))
            glUniform4fv(g_uniforms.u_lt, x.nlights * 5, &x.lt[0][0]);
    }
}

GLuint ge_shader_get_program() {
    return g_program;
}

// ---- Shared with the FX shader overrides (psp_fx_shader.cpp) ----

const char* ge_shader_vertex_version() { return k_vertex_version; }
const char* ge_shader_vertex_body() { return k_vertex_shader_src; }

void ge_shader_query_locations(GLuint prog, ShaderUniforms* u) {
    auto L = [&](const char* n) { return glGetUniformLocation(prog, n); };
    u->u_texture_enable = L("u_texture_enable");
    u->u_texture = L("u_texture");
    u->u_tex_func = L("u_tex_func");
    u->u_tex_rgba = L("u_tex_rgba");
    u->u_tex_double = L("u_tex_double");
    u->u_tex_env = L("u_tex_env");
    u->u_alpha_test_enable = L("u_alpha_test_enable");
    u->u_alpha_test_ref = L("u_alpha_test_ref");
    u->u_alpha_test_func = L("u_alpha_test_func");
    u->u_alpha_test_mask = L("u_alpha_test_mask");
    u->u_color_test_enable = L("u_color_test_enable");
    u->u_color_test_func = L("u_color_test_func");
    u->u_color_test_ref = L("u_color_test_ref");
    u->u_color_test_mask = L("u_color_test_mask");
    u->u_tex_remap = L("u_tex_remap");
    u->u_alpha_double = L("u_alpha_double");
    u->u_alpha_override = L("u_alpha_override");
    u->u_xform = L("u_xform");
    u->u_world = L("u_world");
    u->u_view = L("u_view");
    u->u_proj = L("u_proj");
    u->u_uvxf = L("u_uvxf");
    u->u_skin = L("u_skin");
    u->u_bones = L("u_bones[0]");
    u->u_lit = L("u_lit");
    u->u_lflags = L("u_lflags");
    u->u_nl = L("u_nl");
    u->u_lc = L("u_lc[0]");
    u->u_lt = L("u_lt[0]");
}

void ge_shader_note_external_program() { g_used_program = 0; }

void ge_shader_upload_to(const ShaderUniforms& u, const GeState& state, const GeXform3D* x,
                         bool alpha_double) {
    // Uncached: an override program is bound for a handful of batches a frame.
    if (u.u_texture_enable >= 0) glUniform1i(u.u_texture_enable, state.texture_enable ? 1 : 0);
    if (u.u_texture >= 0) glUniform1i(u.u_texture, 0);
    if (u.u_tex_func >= 0) glUniform1i(u.u_tex_func, static_cast<int>(state.tex_func));
    if (u.u_tex_rgba >= 0) glUniform1i(u.u_tex_rgba, state.tex_func_rgba ? 1 : 0);
    if (u.u_tex_double >= 0) glUniform1i(u.u_tex_double, state.tex_func_double ? 1 : 0);
    if (u.u_tex_env >= 0)
        glUniform3f(u.u_tex_env, (state.tex_env_color & 0xFF) / 255.0f,
                    ((state.tex_env_color >> 8) & 0xFF) / 255.0f,
                    ((state.tex_env_color >> 16) & 0xFF) / 255.0f);
    if (u.u_alpha_double >= 0) glUniform1i(u.u_alpha_double, alpha_double ? 1 : 0);
    if (!x) {
        if (u.u_xform >= 0) glUniform1i(u.u_xform, 0);
        return;
    }
    if (u.u_xform >= 0) glUniform1i(u.u_xform, 1);
    float m4[16];
    pad43(x->world, m4);
    if (u.u_world >= 0) glUniformMatrix4fv(u.u_world, 1, GL_FALSE, m4);
    pad43(x->view, m4);
    if (u.u_view >= 0) glUniformMatrix4fv(u.u_view, 1, GL_FALSE, m4);
    if (u.u_proj >= 0) glUniformMatrix4fv(u.u_proj, 1, GL_FALSE, x->proj);
    if (u.u_uvxf >= 0) glUniform4f(u.u_uvxf, x->uvxf[0], x->uvxf[1], x->uvxf[2], x->uvxf[3]);
    if (u.u_skin >= 0) glUniform1i(u.u_skin, x->skin_n);
    if (x->skin_n > 0 && u.u_bones >= 0) {
        float b4[8 * 16];
        for (int k = 0; k < x->skin_n; k++) pad43(x->bones + k * 12, b4 + k * 16);
        glUniformMatrix4fv(u.u_bones, x->skin_n, GL_FALSE, b4);
    }
    if (u.u_lit >= 0) glUniform1i(u.u_lit, x->lit);
    if (x->lit) {
        if (u.u_lflags >= 0) glUniform1i(u.u_lflags, x->lflags);
        if (u.u_nl >= 0) glUniform1i(u.u_nl, x->nlights);
        if (u.u_lc >= 0) glUniform4fv(u.u_lc, 4, &x->lc[0][0]);
        if (x->nlights > 0 && u.u_lt >= 0) glUniform4fv(u.u_lt, x->nlights * 5, &x->lt[0][0]);
    }
}
