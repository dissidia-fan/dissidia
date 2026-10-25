// psp_fx_manifest.cpp -- FX shader manifest parsing, matching, effect ages and
// the program registry. No GL here (host-tested by tests/test_fx_shader.cpp);
// the GL side is psp_fx_shader.cpp. Format: FX_SHADERS.md.
#include "psp_fx_shader.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace psp_fx {

// ------------------------------------------------------------ minimal JSON ----
namespace {

struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct JParser {
    const char* p;
    const char* e;
    std::string err;
    int depth = 0;
    void ws() {
        for (;;) {
            while (p < e && std::isspace((unsigned char)*p)) p++;
            // Comments are not JSON, but hand-edited manifests get them; accept // and /* */.
            if (p + 1 < e && p[0] == '/' && p[1] == '/') { while (p < e && *p != '\n') p++; continue; }
            if (p + 1 < e && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
                p = p + 1 < e ? p + 2 : e;
                continue;
            }
            break;
        }
    }
    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return fail("expected string");
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\') {
                if (++p >= e) return fail("bad escape");
                switch (*p) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (e - p < 5) return fail("bad \\u escape");
                    unsigned cp = (unsigned)std::strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16);
                    p += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
                    break;
                }
                default: out += *p; break;
                }
                p++;
            } else {
                out += *p++;
            }
        }
        if (p >= e) return fail("unterminated string");
        p++;
        return true;
    }
    bool val(JVal& v) {
        if (++depth > 64) return fail("nesting too deep");
        ws();
        if (p >= e) return fail("unexpected end");
        bool ok = true;
        if (*p == '{') {
            v.t = JVal::Obj;
            p++;
            ws();
            if (p < e && *p == '}') { p++; }
            else for (;;) {
                ws();
                std::string k;
                if (!str(k)) { ok = false; break; }
                ws();
                if (p >= e || *p != ':') { ok = fail("expected ':'"); break; }
                p++;
                JVal c;
                if (!val(c)) { ok = false; break; }
                v.o.emplace_back(std::move(k), std::move(c));
                ws();
                if (p < e && *p == ',') { p++; ws(); if (p < e && *p == '}') { p++; break; } continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = fail("expected ',' or '}'");
                break;
            }
        } else if (*p == '[') {
            v.t = JVal::Arr;
            p++;
            ws();
            if (p < e && *p == ']') { p++; }
            else for (;;) {
                JVal c;
                if (!val(c)) { ok = false; break; }
                v.a.push_back(std::move(c));
                ws();
                if (p < e && *p == ',') { p++; ws(); if (p < e && *p == ']') { p++; break; } continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = fail("expected ',' or ']'");
                break;
            }
        } else if (*p == '"') {
            v.t = JVal::Str;
            ok = str(v.s);
        } else if (e - p >= 4 && !std::strncmp(p, "true", 4)) {
            v.t = JVal::Bool; v.b = true; p += 4;
        } else if (e - p >= 5 && !std::strncmp(p, "false", 5)) {
            v.t = JVal::Bool; v.b = false; p += 5;
        } else if (e - p >= 4 && !std::strncmp(p, "null", 4)) {
            v.t = JVal::Null; p += 4;
        } else {
            std::string num;
            while (p < e && (std::isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E'))
                num += *p++;
            if (num.empty()) ok = fail("unexpected character");
            else {
                char* end = nullptr;
                v.t = JVal::Num;
                v.n = std::strtod(num.c_str(), &end);
                if (!end || *end) ok = fail("bad number");
            }
        }
        depth--;
        return ok;
    }
};

bool json_parse(const std::string& s, JVal& out, std::string* err) {
    JParser jp{s.data(), s.data() + s.size(), {}};
    bool ok = jp.val(out);
    if (ok) {
        jp.ws();
        if (jp.p != jp.e) ok = jp.fail("trailing data after the document");
    }
    if (!ok && err) {
        int line = 1;
        for (const char* q = s.data(); q < jp.p && q < s.data() + s.size(); q++) if (*q == '\n') line++;
        *err = jp.err + " (line " + std::to_string(line) + ")";
    }
    return ok;
}

// "#rrggbb" -> 3 floats
bool parse_hex_color(const std::string& s, float* rgb) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; i++) if (!std::isxdigit((unsigned char)s[i])) return false;
    for (int c = 0; c < 3; c++) rgb[c] = (float)std::strtoul(s.substr(1 + c * 2, 2).c_str(), nullptr, 16) / 255.0f;
    return true;
}

}  // namespace

bool parse_hash(const std::string& s0, uint64_t* out) {
    std::string s = s0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint64_t)d;
    }
    if (v == 0) return false;   // 0 means "no id" in the runtime
    *out = v;
    return true;
}

std::string hash_str(uint64_t h) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

bool parse_blend(const std::string& s, Blend* out) {
    static const std::pair<const char*, Blend> k[] = {
        {"game", Blend::Game}, {"add", Blend::Add}, {"add_one", Blend::AddOne}, {"alpha", Blend::Alpha},
        {"premul", Blend::Premul}, {"screen", Blend::Screen}, {"off", Blend::Off}};
    for (const auto& kv : k) if (s == kv.first) { *out = kv.second; return true; }
    return false;
}

bool parse_depth(const std::string& s, Depth* out) {
    static const std::pair<const char*, Depth> k[] = {
        {"game", Depth::Game}, {"nowrite", Depth::NoWrite}, {"write", Depth::Write}, {"off", Depth::Off}};
    for (const auto& kv : k) if (s == kv.first) { *out = kv.second; return true; }
    return false;
}

bool parse_manifest(const std::string& json, Manifest& out, std::string* err) {
    out = Manifest{};
    JVal root;
    if (!json_parse(json, root, err)) return false;
    if (root.t != JVal::Obj) {
        if (err) *err = "manifest root must be an object";
        return false;
    }
    if (const JVal* v = root.get("version")) if (v->t == JVal::Num) out.version = (int)v->n;
    // Named textures shared by entries: "textures": { "crack": "tex/crack_01.png" }
    std::map<std::string, std::string> named;
    if (const JVal* t = root.get("textures")) {
        if (t->t == JVal::Obj)
            for (const auto& kv : t->o) if (kv.second.t == JVal::Str) named[kv.first] = kv.second.s;
    }
    // Named uniform sets: "params": { "alterna": { "u_x": 1 } } merged via "params": "alterna".
    const JVal* param_sets = root.get("params");
    const JVal* draws = root.get("draws");
    if (!draws) return true;   // an empty manifest is valid
    if (draws->t != JVal::Arr) {
        if (err) *err = "\"draws\" must be an array";
        return false;
    }
    int idx = -1;
    for (const JVal& d : draws->a) {
        idx++;
        DrawEntry en;
        auto warn = [&](const std::string& m) {
            out.warnings.push_back("draws[" + std::to_string(idx) + "]" + (en.name.empty() ? "" : " (" + en.name + ")") + ": " + m);
        };
        if (d.t != JVal::Obj) { warn("not an object, skipped"); continue; }
        if (const JVal* v = d.get("name")) if (v->t == JVal::Str) en.name = v->s;
        if (const JVal* v = d.get("enabled")) if (v->t == JVal::Bool) en.enabled = v->b;
        if (const JVal* v = d.get("hash")) {
            std::vector<const JVal*> hs;
            if (v->t == JVal::Arr) for (const JVal& x : v->a) hs.push_back(&x);
            else hs.push_back(v);
            for (const JVal* h : hs) {
                uint64_t id = 0;
                if (h->t == JVal::Str && parse_hash(h->s, &id)) en.hashes.push_back(id);
                else if (h->t == JVal::Str && !h->s.empty()) warn("hash \"" + h->s + "\" is not a texture id (16 hex digits), ignored");
            }
        }
        if (const JVal* v = d.get("frag")) if (v->t == JVal::Str) en.frag = v->s;
        if (const JVal* v = d.get("include")) {
            if (v->t == JVal::Str) en.includes.push_back(v->s);
            else if (v->t == JVal::Arr) for (const JVal& x : v->a) if (x.t == JVal::Str) en.includes.push_back(x.s);
        }
        if (const JVal* v = d.get("screen")) if (v->t == JVal::Bool) en.screen = v->b;
        if (const JVal* v = d.get("blend")) {
            if (v->t != JVal::Str || !parse_blend(v->s, &en.blend)) warn("unknown blend, using \"game\"");
        }
        if (const JVal* v = d.get("depth")) {
            if (v->t != JVal::Str || !parse_depth(v->s, &en.depth)) warn("unknown depth, using \"game\"");
        }
        if (const JVal* v = d.get("life_ticks")) if (v->t == JVal::Num) en.life_ticks = (float)v->n;
        if (const JVal* v = d.get("reset_gap_ticks")) if (v->t == JVal::Num) en.gap_ticks = (float)v->n;
        if (const JVal* v = d.get("age_offset_ticks")) if (v->t == JVal::Num) en.age_offset_ticks = (float)v->n;
        // Uniforms: shared set(s) first, then the entry's own values override.
        std::vector<const JVal*> uni_objs;
        if (const JVal* ps = d.get("params")) {
            std::vector<std::string> names;
            if (ps->t == JVal::Str) names.push_back(ps->s);
            else if (ps->t == JVal::Arr) for (const JVal& x : ps->a) if (x.t == JVal::Str) names.push_back(x.s);
            for (const auto& nm : names) {
                const JVal* set = param_sets && param_sets->t == JVal::Obj ? param_sets->get(nm.c_str()) : nullptr;
                if (set && set->t == JVal::Obj) uni_objs.push_back(set);
                else warn("params set \"" + nm + "\" not found");
            }
        }
        if (const JVal* u = d.get("uniforms")) if (u->t == JVal::Obj) uni_objs.push_back(u);
        for (const JVal* uo : uni_objs) {
            for (const auto& kv : uo->o) {
                UniformValue uv;
                uv.name = kv.first;
                const JVal& x = kv.second;
                if (x.t == JVal::Num) { uv.n = 1; uv.v[0] = (float)x.n; }
                else if (x.t == JVal::Bool) { uv.n = 1; uv.v[0] = x.b ? 1.0f : 0.0f; }
                else if (x.t == JVal::Str && parse_hex_color(x.s, uv.v)) { uv.n = 3; }
                else if (x.t == JVal::Arr && ((x.a.size() >= 1 && x.a.size() <= 4) || x.a.size() == 16)) {
                    bool ok = true;
                    for (const JVal& c : x.a) ok = ok && c.t == JVal::Num;
                    if (!ok) { warn("uniform " + kv.first + ": array must be numbers"); continue; }
                    uv.n = (int)x.a.size();
                    for (int i = 0; i < uv.n; i++) uv.v[i] = (float)x.a[(size_t)i].n;
                } else { warn("uniform " + kv.first + ": unsupported value"); continue; }
                bool replaced = false;
                for (auto& old : en.uniforms) if (old.name == uv.name) { old = uv; replaced = true; }
                if (!replaced) en.uniforms.push_back(uv);
            }
        }
        if (const JVal* t = d.get("textures")) {
            if (t->t == JVal::Obj) for (const auto& kv : t->o) {
                TexBinding tb;
                tb.uniform = kv.first;
                std::string ref;
                if (kv.second.t == JVal::Str) ref = kv.second.s;
                else if (kv.second.t == JVal::Obj) {
                    if (const JVal* f = kv.second.get("file")) if (f->t == JVal::Str) ref = f->s;
                    if (const JVal* c = kv.second.get("clamp")) if (c->t == JVal::Bool) tb.clamp = c->b;
                }
                auto it = named.find(ref);
                tb.path = it != named.end() ? it->second : ref;
                if (tb.path.empty()) { warn("texture " + kv.first + ": no file"); continue; }
                en.textures.push_back(tb);
            }
        }
        if (en.frag.empty()) { warn("no \"frag\" shader, skipped"); continue; }
        if (en.hashes.empty()) { if (en.enabled) warn("no texture hash yet, skipped"); continue; }
        if (en.textures.size() > 12) { warn("more than 12 textures, extra ones ignored"); en.textures.resize(12); }
        out.draws.push_back(std::move(en));
    }
    return true;
}

// ------------------------------------------------------------ effect ages ----
float AgeTracker::touch(uint64_t h, double now_s, float gap_ticks, bool* fresh) {
    auto it = m_.find(h);
    bool f = false;
    if (it == m_.end()) {
        if (m_.size() > 4096) m_.clear();   // bound memory (hashes of long-gone effects)
        it = m_.emplace(h, S{now_s, now_s}).first;
        f = true;
    } else if ((now_s - it->second.last) * 60.0 > (double)gap_ticks) {
        it->second.first = now_s;
        f = true;
    }
    it->second.last = now_s;
    if (fresh) *fresh = f;
    return (float)((now_s - it->second.first) * 60.0);
}

// ------------------------------------------------------------ registry ----
int Registry::build(const Manifest& m, const ReadFn& read, const CompileFn& compile, const LogFn& log) {
    clear();
    m_ = m;
    std::map<std::string, unsigned> cache;   // source key -> program (0 = failed)
    for (size_t i = 0; i < m_.draws.size(); i++) {
        const DrawEntry& en = m_.draws[i];
        if (!en.enabled) continue;
        std::string key = en.frag + (en.screen ? "|screen" : "");
        for (const auto& inc : en.includes) key += "|" + inc;
        unsigned prog = 0;
        auto it = cache.find(key);
        if (it != cache.end()) {
            prog = it->second;
        } else {
            ProgramSource ps;
            ps.screen = en.screen;
            ps.frag = fragment_preamble();
            bool ok = true;
            for (const auto& inc : en.includes) {
                std::string text;
                if (!read(inc, &text)) { log("include " + inc + " not found (" + en.name + ")"); ok = false; break; }
                ps.frag += "#line 1\n" + text + "\n";
            }
            std::string body;
            if (ok && !read(en.frag, &body)) { log("shader " + en.frag + " not found (" + en.name + ")"); ok = false; }
            if (ok) {
                ps.frag += "#line 1\n" + body + "\n" + fragment_epilogue();
                std::string clog;
                prog = compile(ps, &clog);
                if (!prog) log("compile failed: " + en.frag + " (" + en.name + "), using the uber-shader:\n" + clog);
                else programs_.push_back(prog);
            }
            cache[key] = prog;
        }
        if (!prog) continue;
        Bound b;
        b.program = prog;
        b.index = (int)i;
        b.entry = &m_.draws[i];
        const size_t bi = bound_.size();
        bound_.push_back(b);
        for (uint64_t h : en.hashes) {
            if (by_hash_.count(h)) { log("hash " + hash_str(h) + " listed twice; keeping the first (" + en.name + " ignored for it)"); continue; }
            by_hash_[h] = bi;
        }
    }
    return (int)bound_.size();
}

// ------------------------------------------------------------ GLSL frame ----
const char* fragment_preamble() {
    return R"GLSL(#version 330 core
// ---- psprecomp FX shader preamble (psp_fx_manifest.cpp) ----
in vec2 v_texcoord;     // GE texture coordinates (after the GE UV scale/offset)
in vec4 v_color;        // GE vertex colour (after lighting): the FEP colour/alpha keys
in vec3 v_fx_opos;      // model-space position (GPU-transform draws; clip-space xyz otherwise)
in vec3 v_fx_wpos;      // world-space position (0 on CPU-transformed draws)
in vec3 v_fx_wnormal;   // world-space vertex normal (0 when the vertex has none)
in vec2 v_fx_uv;        // raw vertex UV (before the GE UV scale/offset)

uniform sampler2D u_texture;          // the draw's own texture (unit 0)
uniform bool u_texture_enable;
uniform int u_tex_func;
uniform bool u_tex_rgba;
uniform bool u_tex_double;
uniform vec3 u_tex_env;
uniform bool u_alpha_double;          // GE 2*src-alpha blend: alpha is doubled after fx_shade()

uniform float u_fx_time;              // seconds since the FX system started
uniform float u_fx_ticks;             // the same in ticks (60 per second)
uniform float u_fx_age;               // seconds since this effect instance first drew
uniform float u_fx_age_ticks;         // the same in ticks, + the entry's age_offset_ticks
uniform float u_fx_life_ticks;        // the entry's life_ticks (0 = not given)
uniform float u_fx_progress;          // clamp(u_fx_age_ticks / u_fx_life_ticks, 0, 1); 0 if no life
uniform mat4 u_fx_world;              // GE world matrix (padded to mat4)
uniform mat4 u_fx_view;               // GE view matrix
uniform mat4 u_fx_proj;               // GE projection matrix
uniform vec3 u_fx_camera;             // world-space eye position
uniform vec3 u_fx_origin;             // world-space origin of the mesh (world matrix translation)
uniform vec2 u_fx_screen_size;        // render-target size in pixels
uniform vec4 u_fx_viewport;           // GL viewport of the draw (x, y, w, h) in pixels
uniform int u_fx_has_world;           // 1 on GPU-transform draws (v_fx_wpos/v_fx_opos valid)
uniform int u_fx_has_normal;          // 1 when the vertices carry normals
uniform sampler2D u_fx_screen;        // copy of the render target (entries with "screen": true)
uniform float u_fx_env_scale;         // 2 when the game blends with 2*src alpha but the entry overrides blend

out vec4 frag_color;

#define texture2D texture

// The colour the uber-shader would produce before alpha/colour test.
vec4 fx_ge_color() {
    vec4 color = v_color;
    if (u_texture_enable) {
        vec4 tex = texture(u_texture, v_texcoord);
        if (u_tex_func == 0) { color.rgb *= tex.rgb; if (u_tex_rgba) color.a *= tex.a; }
        else if (u_tex_func == 1) { color.rgb = u_tex_rgba ? mix(color.rgb, tex.rgb, tex.a) : tex.rgb; }
        else if (u_tex_func == 2) { color.rgb = mix(color.rgb, u_tex_env, tex.rgb); if (u_tex_rgba) color.a *= tex.a; }
        else if (u_tex_func == 3) { color.rgb = tex.rgb; if (u_tex_rgba) color.a = tex.a; }
        else if (u_tex_func == 4) { color.rgb += tex.rgb; if (u_tex_rgba) color.a *= tex.a; }
        if (u_tex_double) color.rgb = min(color.rgb * 2.0, vec3(1.0));
    }
    return color;
}
// Envelope / tint driven by the FEP colour keys, in the game's own scale: the
// GE's colour doubling (0x80 = 1.0) and 2*src-alpha blend factors applied.
float fx_env() { return clamp(v_color.a * u_fx_env_scale, 0.0, 1.0); }
vec3 fx_tint() { return u_tex_double ? v_color.rgb * 2.0 : v_color.rgb; }
// Lab-style UV: the mesh's own UV on 3D draws, the GE texture coordinates on sprites.
vec2 fx_uv() { return u_fx_has_world != 0 ? v_fx_uv : v_texcoord; }
// Eye direction (towards the camera) and surface normal in world space. Without
// vertex normals the normal is taken from the mesh origin (right for spheres).
vec3 fx_view_dir() { return u_fx_has_world != 0 ? normalize(u_fx_camera - v_fx_wpos) : vec3(0.0, 0.0, 1.0); }
vec3 fx_normal() {
    if (u_fx_has_normal != 0 && dot(v_fx_wnormal, v_fx_wnormal) > 1e-12) return normalize(v_fx_wnormal);
    if (u_fx_has_world != 0) { vec3 d = v_fx_wpos - u_fx_origin; if (dot(d, d) > 1e-12) return normalize(d); }
    return vec3(0.0, 0.0, 1.0);
}
// 1 at the silhouette, 0 facing the camera (three.js labs: 1 - |dot(N, V)|).
float fx_rim() { return 1.0 - abs(dot(fx_normal(), fx_view_dir())); }
vec2 fx_screen_uv() { return gl_FragCoord.xy / u_fx_screen_size; }
float fx_saturate(float x) { return clamp(x, 0.0, 1.0); }
float fx_ease(float t) { t = clamp(t, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
// ---- end of preamble ----
)GLSL";
}

const char* fragment_epilogue() {
    return R"GLSL(
// ---- psprecomp FX shader epilogue ----
void main() {
    vec4 c = fx_shade();
    if (u_alpha_double) c.a = min(c.a * 2.0, 1.0);
    frag_color = c;
}
)GLSL";
}

}  // namespace psp_fx
