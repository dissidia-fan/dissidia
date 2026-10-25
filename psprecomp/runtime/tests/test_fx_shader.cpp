// Host tests for the FX shader overrides (psp_fx_manifest.cpp): manifest
// parsing, hash matching, effect-age tracking and the compile-failure
// fallback. No GL: the registry takes a compiler callback, faked here.
//
//   test_fx_shader [fx_shaders_dir] [--glslang <glslangValidator>]
// With a directory, its manifest.json is also parsed and every entry built
// against the real files: each assembled fragment source (preamble + includes +
// file + main) is linted (a non-void function must contain a return: Apple's
// GLSL compiler rejects it, Mesa accepts it), and with --glslang compiled by the
// Khronos reference compiler, which is as strict as the Mac on these rules.
#include "psp_fx_shader.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

static int failures = 0, tests_run = 0;
#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        tests_run++;                                                       \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    } while (0)

using namespace psp_fx;

static void test_hash() {
    uint64_t h = 0;
    CHECK(parse_hash("0006a676ba63b104", &h) && h == 0x0006a676ba63b104ull, "16-digit id");
    CHECK(parse_hash("0x0006A676BA63B104", &h) && h == 0x0006a676ba63b104ull, "0x prefix, upper case");
    CHECK(!parse_hash("", &h), "empty");
    CHECK(!parse_hash("TODO", &h), "placeholder");
    CHECK(!parse_hash("00000000000000000", &h), "17 digits");
    CHECK(!parse_hash("0000000000000000", &h), "zero is 'no id'");
    CHECK(hash_str(0x0006a676ba63b104ull) == "0006a676ba63b104", "hash_str round trip");
    Blend b; Depth d;
    CHECK(parse_blend("add", &b) && b == Blend::Add, "blend add");
    CHECK(!parse_blend("additive", &b), "unknown blend");
    CHECK(parse_depth("nowrite", &d) && d == Depth::NoWrite, "depth nowrite");
}

static const char* kManifest = R"JSON(
{
  // comments are accepted
  "version": 1,
  "textures": { "crack": "tex/crack_01.png" },
  "params": { "common": { "u_hue": 0.7, "u_tint": "#ff0000" } },
  "draws": [
    { "name": "a", "hash": "00000000000000aa", "frag": "a.frag", "include": "common.glsl",
      "params": "common", "uniforms": { "u_hue": 0.5, "u_v2": [1, 2], "u_on": true },
      "textures": { "u_crack": "crack", "u_raw": { "file": "tex/raw.png", "clamp": true } },
      "blend": "add", "depth": "nowrite", "life_ticks": 154, "reset_gap_ticks": 12, "age_offset_ticks": 3 },
    { "name": "b", "hash": ["0xbb", "00000000000000bc"], "frag": "b.frag", "screen": true },
    { "name": "shared", "hash": "cc", "frag": "a.frag", "include": ["common.glsl"] },
    { "name": "waiting", "hash": "TODO", "frag": "a.frag" },
    { "name": "nofrag", "hash": "dd" },
    { "name": "off", "hash": "ee", "frag": "a.frag", "enabled": false },
    { "name": "badblend", "hash": "ff", "frag": "a.frag", "blend": "nope" },
    { "name": "dup", "hash": "aa", "frag": "b.frag" },
  ]
}
)JSON";

static void test_parse() {
    Manifest m;
    std::string err;
    CHECK(parse_manifest(kManifest, m, &err), "manifest parses");
    if (!err.empty()) std::fprintf(stderr, "  err: %s\n", err.c_str());
    // waiting (no hash) and nofrag are skipped; off is kept (disabled).
    CHECK(m.draws.size() == 6, "entry count");
    if (m.draws.size() < 6) return;
    const DrawEntry& a = m.draws[0];
    CHECK(a.name == "a" && a.hashes.size() == 1 && a.hashes[0] == 0xaa, "a hash");
    CHECK(a.includes.size() == 1 && a.includes[0] == "common.glsl", "a include");
    CHECK(a.blend == Blend::Add && a.depth == Depth::NoWrite, "a blend/depth");
    CHECK(a.life_ticks == 154.0f && a.gap_ticks == 12.0f && a.age_offset_ticks == 3.0f, "a timing");
    std::map<std::string, UniformValue> u;
    for (const auto& x : a.uniforms) u[x.name] = x;
    CHECK(u.count("u_hue") && u["u_hue"].n == 1 && u["u_hue"].v[0] == 0.5f, "entry uniform overrides the params set");
    CHECK(u.count("u_tint") && u["u_tint"].n == 3 && u["u_tint"].v[0] == 1.0f && u["u_tint"].v[1] == 0.0f, "#rrggbb -> vec3");
    CHECK(u.count("u_v2") && u["u_v2"].n == 2 && u["u_v2"].v[1] == 2.0f, "vec2");
    CHECK(u.count("u_on") && u["u_on"].v[0] == 1.0f, "bool -> float");
    CHECK(a.textures.size() == 2 && a.textures[0].path == "tex/crack_01.png" && !a.textures[0].clamp, "named texture");
    CHECK(a.textures.size() == 2 && a.textures[1].path == "tex/raw.png" && a.textures[1].clamp, "texture object form");
    const DrawEntry& b = m.draws[1];
    CHECK(b.hashes.size() == 2 && b.hashes[0] == 0xbb && b.hashes[1] == 0xbc && b.screen, "b: hash list + screen");
    CHECK(b.blend == Blend::Game && b.depth == Depth::Game && b.gap_ticks == 8.0f, "defaults");
    CHECK(!m.draws[3].enabled, "disabled entry kept, not enabled");
    CHECK(m.draws[4].blend == Blend::Game, "bad blend falls back to game");
    int warn_hash = 0, warn_frag = 0, warn_blend = 0;
    for (const auto& w : m.warnings) {
        if (w.find("waiting") != std::string::npos) warn_hash++;
        if (w.find("nofrag") != std::string::npos) warn_frag++;
        if (w.find("badblend") != std::string::npos) warn_blend++;
    }
    CHECK(warn_hash >= 1 && warn_frag == 1 && warn_blend == 1, "warnings name the entries");

    Manifest bad;
    CHECK(!parse_manifest("{ \"draws\": [ { \"name\": } ] }", bad, &err) && err.find("line") != std::string::npos,
          "broken JSON is rejected with a line number");
    CHECK(!parse_manifest("[1, 2]", bad, &err), "root must be an object");
    CHECK(parse_manifest("{}", bad, &err) && bad.draws.empty(), "empty manifest");
}

static void test_registry() {
    Manifest m;
    std::string err;
    parse_manifest(kManifest, m, &err);
    std::map<std::string, std::string> files = {
        {"a.frag", "vec4 fx_shade() { return vec4(1.0); }"},
        {"b.frag", "vec4 fx_shade() { this does not compile }"},
        {"common.glsl", "float helper() { return 1.0; }"},
    };
    int compiles = 0;
    unsigned next = 10;
    std::string last_src;
    Registry r;
    std::vector<std::string> logs;
    const int n = r.build(
        m,
        [&](const std::string& rel, std::string* d) {
            auto it = files.find(rel);
            if (it == files.end()) return false;
            *d = it->second;
            return true;
        },
        [&](const ProgramSource& ps, std::string* log) -> unsigned {
            compiles++;
            last_src = ps.frag;
            if (ps.frag.find("does not compile") != std::string::npos) { *log = "0:1: syntax error"; return 0; }
            return next++;
        },
        [&](const std::string& l) { logs.push_back(l); });
    // a, shared (same program), badblend (a.frag without include = 2nd program) usable;
    // b and dup fail to compile; off is disabled.
    CHECK(n == 3, "usable entries");
    // a.frag+common (a, shared) once, a.frag alone (badblend) once, b.frag with
    // screen (b) and without (dup) once each; the failed builds are not retried.
    CHECK(compiles == 4, "one compile per distinct source");
    const auto* ma = r.match(0xaa);
    CHECK(ma && ma->entry->name == "a", "match a");
    const auto* mc = r.match(0xcc);
    CHECK(ma && mc && ma->program == mc->program, "entries share a compiled program");
    CHECK(r.match(0xbb) == nullptr && r.match(0xbc) == nullptr, "compile failure -> no override (uber-shader)");
    CHECK(r.match(0xee) == nullptr, "disabled entry never matches");
    CHECK(r.match(0x1234) == nullptr, "unknown hash");
    CHECK(r.match(0xff) && r.match(0xff)->program != ma->program, "different includes -> different program");
    bool logged_fail = false, logged_dup = false;
    for (const auto& l : logs) {
        if (l.find("compile failed") != std::string::npos && l.find("syntax error") != std::string::npos) logged_fail = true;
        if (l.find("listed twice") != std::string::npos) logged_dup = true;
    }
    CHECK(logged_fail, "compile error is logged with the driver log");
    CHECK(!logged_dup, "dup entry failed to compile, so no duplicate claim");
    CHECK(last_src.rfind("#version 330 core", 0) == 0, "source starts with the preamble's #version");
    CHECK(last_src.find("fx_shade()") != std::string::npos && last_src.find("frag_color = c;") != std::string::npos,
          "source ends with the epilogue");

    // Missing shader file: entry dropped, logged.
    files.erase("a.frag");
    Registry r2;
    logs.clear();
    r2.build(m, [&](const std::string& rel, std::string* d) {
            auto it = files.find(rel); if (it == files.end()) return false; *d = it->second; return true; },
        [&](const ProgramSource&, std::string*) -> unsigned { return next++; },
        [&](const std::string& l) { logs.push_back(l); });
    CHECK(r2.match(0xaa) == nullptr || r2.match(0xaa)->entry->name != "a", "missing shader file -> entry dropped");
    CHECK(r2.match(0xcc) == nullptr, "missing shader file -> no override (uber-shader)");
    bool nf = false;
    for (const auto& l : logs) nf = nf || l.find("not found") != std::string::npos;
    CHECK(nf, "missing file logged");

    // Duplicate claims: the first entry keeps the hash.
    Manifest d;
    parse_manifest(R"({"draws":[{"name":"x","hash":"aa","frag":"a"},{"name":"y","hash":"aa","frag":"b"}]})", d, &err);
    Registry r3;
    logs.clear();
    r3.build(d, [](const std::string&, std::string* s) { *s = "x"; return true; },
             [&](const ProgramSource&, std::string*) -> unsigned { return next++; },
             [&](const std::string& l) { logs.push_back(l); });
    CHECK(r3.match(0xaa) && r3.match(0xaa)->entry->name == "x", "first claim wins");
    CHECK(!logs.empty() && logs.back().find("listed twice") != std::string::npos, "duplicate logged");

    Registry empty;
    CHECK(empty.empty() && empty.match(0xaa) == nullptr, "empty registry");
}

static void test_ages() {
    AgeTracker t;
    bool fresh = false;
    CHECK(t.touch(1, 10.0, 8.0f, &fresh) == 0.0f && fresh, "first sight: age 0, new instance");
    float a = 0.0f;
    for (int i = 1; i <= 30; i++) a = t.touch(1, 10.0 + i / 60.0, 8.0f, &fresh);   // drawn every tick
    CHECK(!fresh && a > 29.9f && a < 30.1f, "0.5 s of continuous draws: 30 ticks");
    a = t.touch(1, 10.6, 8.0f, &fresh);   // 6 ticks since last sight: same instance
    CHECK(!fresh && a > 35.9f && a < 36.1f, "within the gap: same instance");
    a = t.touch(1, 11.0, 8.0f, &fresh);   // 24 ticks unseen > 8: new instance
    CHECK(fresh && a == 0.0f, "after the gap: new instance, age 0");
    t.touch(2, 11.0, 8.0f, &fresh);
    CHECK(fresh && t.size() == 2, "hashes are tracked separately");
}

static bool read_file(const std::string& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

// Non-void functions whose body has no `return` (crude brace matcher; GLSL has no
// lambdas or strings, comments are stripped first). Returns the offending names.
static std::vector<std::string> missing_returns(const std::string& src0) {
    std::string src;
    for (size_t i = 0; i < src0.size(); i++) {
        if (src0.compare(i, 2, "//") == 0) { while (i < src0.size() && src0[i] != '\n') i++; src += '\n'; continue; }
        if (src0.compare(i, 2, "/*") == 0) { size_t e = src0.find("*/", i + 2); i = e == std::string::npos ? src0.size() : e + 1; continue; }
        src += src0[i];
    }
    static const char* types[] = {"float", "int", "uint", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
                                  "mat2", "mat3", "mat4"};
    std::vector<std::string> bad;
    for (size_t i = 0; i < src.size(); i++) {
        if (i && (std::isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) continue;
        for (const char* t : types) {
            const size_t n = std::strlen(t);
            if (src.compare(i, n, t) || !std::isspace((unsigned char)src[i + n])) continue;
            size_t j = i + n;
            while (j < src.size() && std::isspace((unsigned char)src[j])) j++;
            size_t k = j;
            while (k < src.size() && (std::isalnum((unsigned char)src[k]) || src[k] == '_')) k++;
            if (k == j) break;
            const std::string name = src.substr(j, k - j);
            while (k < src.size() && std::isspace((unsigned char)src[k])) k++;
            if (k >= src.size() || src[k] != '(') break;
            size_t close = src.find(')', k);
            if (close == std::string::npos) break;
            size_t b = close + 1;
            while (b < src.size() && std::isspace((unsigned char)src[b])) b++;
            if (b >= src.size() || src[b] != '{') break;   // prototype or call
            int depth = 0;
            size_t e = b;
            for (; e < src.size(); e++) {
                if (src[e] == '{') depth++;
                else if (src[e] == '}' && --depth == 0) break;
            }
            const std::string body = src.substr(b, e - b);
            bool has = false;
            for (size_t r = body.find("return"); r != std::string::npos; r = body.find("return", r + 1))
                if ((r == 0 || !(std::isalnum((unsigned char)body[r - 1]) || body[r - 1] == '_')) &&
                    !(std::isalnum((unsigned char)body[r + 6]) || body[r + 6] == '_')) { has = true; break; }
            if (!has) bad.push_back(name);
            break;
        }
    }
    return bad;
}

static void test_lint() {
    CHECK(missing_returns("vec4 fx_shade() { discard; }").size() == 1, "lint: missing return found");
    CHECK(missing_returns("vec4 fx_shade() { discard; return vec4(0.0); }").empty(), "lint: return present");
    CHECK(missing_returns("float f(float x) { if (x > 0.0) { return 1.0; } return 0.0; }\nvoid main() { }").empty(),
          "lint: nested braces, void main");
    CHECK(missing_returns("vec3 g(); // vec3 h() { }\n").empty(), "lint: prototypes and comments ignored");
    CHECK(missing_returns(std::string(fragment_preamble()) + fragment_epilogue()).empty(), "lint: preamble is clean");
}

static std::string g_glslang;

static void test_shipped(const std::string& dir) {
    std::string text, err;
    if (!read_file(dir + "/manifest.json", &text)) {
        std::printf("  SKIP shipped manifest check: no %s/manifest.json\n", dir.c_str());
        return;
    }
    Manifest m;
    CHECK(parse_manifest(text, m, &err), "shipped manifest parses");
    if (!err.empty()) std::fprintf(stderr, "  %s\n", err.c_str());
    for (const auto& w : m.warnings) std::printf("  note: %s\n", w.c_str());
    Registry r;
    std::vector<std::string> logs;
    int built = r.build(m, [&](const std::string& rel, std::string* d) { return read_file(dir + "/" + rel, d); },
                        [](const ProgramSource& ps, std::string* log) -> unsigned {
                            // Static checks in place of a GL compile.
                            if (ps.frag.find("fx_shade()") == std::string::npos) { *log = "no fx_shade()"; return 0; }
                            if (ps.frag.find("gl_FragColor") != std::string::npos) { *log = "gl_FragColor (use the fx_shade() return value)"; return 0; }
                            for (const auto& f : missing_returns(ps.frag)) *log += "function " + f + " has no return (Mac GL rejects it); ";
                            if (!log->empty()) return 0;
                            if (!g_glslang.empty()) {
                                static int n = 0;
                                const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                                                        "/test_fx_shader_" + std::to_string(n++) + ".frag";
                                { std::ofstream o(tmp); o << ps.frag; }
                                const std::string cmd = "\"" + g_glslang + "\" \"" + tmp + "\" > \"" + tmp + ".log\" 2>&1";
                                const int rc = std::system(cmd.c_str());
                                if (rc != 0) {
                                    std::string out;
                                    read_file(tmp + ".log", &out);
                                    *log = "glslang: " + out;
                                    return 0;
                                }
                            }
                            return 1;
                        },
                        [&](const std::string& l) { logs.push_back(l); });
    for (const auto& l : logs) std::printf("  log: %s\n", l.c_str());
    int enabled = 0;
    for (const auto& d : m.draws) {
        if (!d.enabled) continue;
        enabled++;
        for (const auto& t : d.textures) {
            std::string png;
            CHECK(read_file(dir + "/" + t.path, &png) && png.size() > 8 && !std::memcmp(png.data() + 1, "PNG", 3),
                  ("texture file exists: " + t.path).c_str());
        }
    }
    CHECK(built == enabled, "every enabled shipped entry builds");
    std::printf("  shipped manifest: %zu entries (%d enabled, %d built)\n", m.draws.size(), enabled, built);
}

int main(int argc, char** argv) {
    test_hash();
    test_parse();
    test_registry();
    test_ages();
    test_lint();
    std::string dir;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--glslang") && i + 1 < argc) g_glslang = argv[++i];
        else dir = argv[i];
    }
    if (!g_glslang.empty()) std::printf("  compiling shipped shaders with %s\n", g_glslang.c_str());
    if (!dir.empty()) test_shipped(dir);
    std::printf("fx_shader tests: %d run, %d failed\n", tests_run, failures);
    return failures ? 1 : 0;
}
