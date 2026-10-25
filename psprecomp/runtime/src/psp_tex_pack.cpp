#include "psp_tex_pack.h"
#include "hle/psp_hle_io.h"

#include <zlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {

PspTexPackDirFn g_provider = nullptr;

// ---- xxHash64 (stable across runs/platforms; little-endian word reads) ----
constexpr uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull,
                   P3 = 1609587929392839161ull,  P4 = 9650029242287828579ull,
                   P5 = 2870177450012600261ull;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t xround(uint64_t acc, uint64_t in) { acc += in * P2; return rotl(acc, 31) * P1; }
inline uint64_t xmerge(uint64_t acc, uint64_t v) { return (acc ^ xround(0, v)) * P1 + P4; }

uint64_t xxh64(const uint8_t* p, size_t len, uint64_t seed) {
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* lim = end - 32;
        do {
            v1 = xround(v1, rd64(p)); v2 = xround(v2, rd64(p + 8));
            v3 = xround(v3, rd64(p + 16)); v4 = xround(v4, rd64(p + 24));
            p += 32;
        } while (p <= lim);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xmerge(h, v1); h = xmerge(h, v2); h = xmerge(h, v3); h = xmerge(h, v4);
    } else {
        h = seed + P5;
    }
    h += len;
    while (p + 8 <= end) { h ^= xround(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { h ^= (uint64_t)rd32(p) * P1; h = rotl(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (*p++) * P5; h = rotl(h, 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

// ---- State ----
struct Rec {
    int w = 0, h = 0, fmt = 0, clut_fmt = -1;
    uint32_t addr = 0;
    uint64_t seen = 0;
};

struct State {
    bool inited = false;
    bool dump = false, replace = false, upscale_on = false;
    int upscale = 0;               // 2..4
    int repl_filter = 0;
    std::string dir, dump_dir, index_path;
    std::unordered_set<uint64_t> pack;       // ids with <id>.png in the pack dir
    std::unordered_set<uint64_t> bad;        // replacement files that failed to load
    std::map<uint64_t, Rec> seen;            // sorted -> stable index file
    bool dirty = false;
    std::chrono::steady_clock::time_point last_flush = std::chrono::steady_clock::now();
    size_t dump_count = 0, dump_max = 50000, dumped_new = 0;
    struct UpEntry { std::vector<uint8_t> px; int w, h; };
    std::unordered_map<uint64_t, UpEntry> up_cache;
    size_t up_bytes = 0;
    size_t up_budget = 192u << 20;
    // Decoded replacement pixels (LRU). Without it every texture-cache miss re-read and re-decoded the PNG on the
    // render thread (a 1024x1024 PNG ~15-30 ms, the 4096x4096 one far more): a visible hitch whenever a replaced
    // texture came back on screen (22 Oct, "lag when battlegen occurs"). PSPRECOMP_TEXPACK_CACHE_MB, default 384.
    struct ReplEntry { std::vector<uint8_t> px; int w, h; std::list<uint64_t>::iterator lru; };
    std::unordered_map<uint64_t, ReplEntry> repl_cache;
    std::list<uint64_t> repl_lru;              // front = most recently used
    size_t repl_bytes = 0;
    size_t repl_budget = 384u << 20;
    double slow_ms = 4.0;                      // log replacement loads slower than this (PSPRECOMP_TEXPACK_SLOW_MS)
};
State g;

bool is_dir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
void mkdir_p(const std::string& p) {
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') ::mkdir(p.substr(0, i).c_str(), 0755);
}

bool parse_hex16(const char* s, uint64_t* out) {
    if (std::strlen(s) != 16) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return true;
}

const char* fmt_name(int f) {
    switch (f) {
    case 0: return "5650"; case 1: return "5551"; case 2: return "4444"; case 3: return "8888";
    case 4: return "CLUT4"; case 5: return "CLUT8"; case 6: return "CLUT16"; case 7: return "CLUT32";
    case 8: return "DXT1"; case 9: return "DXT3"; case 10: return "DXT5";
    }
    return "?";
}
const char* clut_name(int f) {
    switch (f) { case 0: return "5650"; case 1: return "5551"; case 2: return "4444"; case 3: return "8888"; }
    return "-";
}

void load_index() {
    FILE* f = std::fopen(g.index_path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char idb[32], dims[32], fm[16], cf[16];
        unsigned addr = 0;
        unsigned long long seen = 0;
        if (std::sscanf(line, "%16s %31s %15s %15s %x %llu", idb, dims, fm, cf, &addr, &seen) < 6) continue;
        uint64_t id;
        int w, h;
        if (!parse_hex16(idb, &id) || std::sscanf(dims, "%dx%d", &w, &h) != 2) continue;
        Rec r;
        r.w = w; r.h = h; r.addr = addr; r.seen = seen;
        for (int k = 0; k <= 10; k++) if (!std::strcmp(fmt_name(k), fm)) r.fmt = k;
        r.clut_fmt = -1;
        for (int k = 0; k <= 3; k++) if (!std::strcmp(clut_name(k), cf)) r.clut_fmt = k;
        g.seen[id] = r;
    }
    std::fclose(f);
}

void init_once() {
    if (g.inited) return;
    g.inited = true;
    const char* e;
    g.dump = (e = std::getenv("PSPRECOMP_TEX_DUMP")) && e[0] == '1';
    if (const char* m = std::getenv("PSPRECOMP_TEX_DUMP_MAX")) g.dump_max = (size_t)std::strtoull(m, nullptr, 10);
    if ((e = std::getenv("PSPRECOMP_TEX_UPSCALE")) && std::strlen(e) == 5 && !std::strncmp(e, "xbrz", 4) &&
        e[4] >= '2' && e[4] <= '4') {
        g.upscale = e[4] - '0';
        g.upscale_on = true;
    }
    if ((e = std::getenv("PSPRECOMP_TEX_REPLACE_FILTER"))) {
        if (!std::strcmp(e, "linear")) g.repl_filter = 1;
        else if (!std::strcmp(e, "nearest")) g.repl_filter = 2;
    }
    bool want_replace = !((e = std::getenv("PSPRECOMP_TEX_REPLACE")) && e[0] == '0');
    if ((e = std::getenv("PSPRECOMP_TEXPACK_CACHE_MB")) && e[0]) g.repl_budget = (size_t)std::strtoull(e, nullptr, 10) << 20;
    if ((e = std::getenv("PSPRECOMP_TEXPACK_SLOW_MS")) && e[0]) g.slow_ms = std::atof(e);

    if ((e = std::getenv("PSPRECOMP_TEXPACK")) && e[0]) {
        g.dir = e;
    } else if (g_provider) {
        g.dir = g_provider();
    } else {
        std::string ms0 = psp_io_ms0_host_path();
        while (ms0.size() > 1 && ms0.back() == '/') ms0.pop_back();
        size_t slash = ms0.find_last_of('/');
        g.dir = (slash == std::string::npos ? std::string(".") : ms0.substr(0, slash)) + "/mods/textures";
    }
    while (g.dir.size() > 1 && g.dir.back() == '/') g.dir.pop_back();
    g.dump_dir = g.dir + "/dump";
    if ((e = std::getenv("PSPRECOMP_TEX_DUMP_DIR")) && e[0]) g.dump_dir = e;   // keep dumps out of the pack
    g.index_path = g.dir + "/textures.txt";

    if (g.dump) {
        mkdir_p(g.dump_dir);
        load_index();
        std::fprintf(stderr, "[TEXPACK] dump on: %s (cap %zu)\n", g.dump_dir.c_str(), g.dump_max);
    }
    if (want_replace && is_dir(g.dir)) {
        if (DIR* d = opendir(g.dir.c_str())) {
            while (dirent* de = readdir(d)) {
                size_t n = std::strlen(de->d_name);
                if (n != 20 || std::strcmp(de->d_name + 16, ".png")) continue;
                std::string stem(de->d_name, 16);
                uint64_t id;
                if (parse_hex16(stem.c_str(), &id)) g.pack.insert(id);
            }
            closedir(d);
        }
        g.replace = true;
        std::fprintf(stderr, "[TEXPACK] replacement pack %s: %zu texture(s)%s\n", g.dir.c_str(),
                     g.pack.size(), (g.pack.empty() && !g.dump && !g.upscale_on) ? " (empty: ids not computed)" : "");
    }
    if (g.upscale_on) std::fprintf(stderr, "[TEXPACK] built-in upscaler: %dx\n", g.upscale);
}

}  // namespace

void psp_texpack_set_dir_provider(PspTexPackDirFn fn) { g_provider = fn; }

bool psp_texpack_active() {
    init_once();
    return g.dump || g.upscale_on || (g.replace && !g.pack.empty());
}

int psp_texpack_replace_filter() { init_once(); return g.repl_filter; }

uint64_t psp_texpack_make_id(const uint8_t* raw, size_t raw_len, int fmt, int w, int h,
                             int clut_bits, const uint8_t* clut_rgba) {
    uint64_t raw_h = xxh64(raw, raw_len, 0);
    uint64_t clut_h = 0;
    if (clut_bits && clut_rgba) {
        // Only the palette entries the texels reference, as (index, RGBA).
        bool used[256] = {};
        if (clut_bits == 8) {
            for (size_t i = 0; i < raw_len; i++) used[raw[i]] = true;
        } else {
            for (size_t i = 0; i < raw_len; i++) { used[raw[i] & 15] = true; used[raw[i] >> 4] = true; }
        }
        uint8_t buf[256 * 5];
        size_t n = 0;
        for (int i = 0; i < 256; i++)
            if (used[i]) { buf[n++] = (uint8_t)i; std::memcpy(buf + n, clut_rgba + i * 4, 4); n += 4; }
        clut_h = xxh64(buf, n, 1);
    }
    uint64_t rec[5] = {raw_h, clut_h, (uint64_t)(unsigned)fmt, (uint64_t)(unsigned)w, (uint64_t)(unsigned)h};
    uint8_t b[sizeof rec];
    std::memcpy(b, rec, sizeof rec);
    return xxh64(b, sizeof b, 0x70737074u);
}

// ---------------------------------------------------------------- PNG ----

namespace {
void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void put_chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t len) {
    put32be(out, (uint32_t)len);
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0, out.data() + start, (uInt)(4 + len));
    put32be(out, (uint32_t)crc);
}
inline int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}
}  // namespace

bool psp_png_encode_rgba(const char* path, const uint8_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    // Sub filter per row: cheap and noticeably smaller than filter 0 for textures.
    const size_t rb = (size_t)w * 4;
    std::vector<uint8_t> raw((rb + 1) * h);
    for (int y = 0; y < h; y++) {
        uint8_t* d = raw.data() + (size_t)y * (rb + 1);
        const uint8_t* s = rgba + (size_t)y * rb;
        d[0] = 1;
        for (size_t i = 0; i < rb; i++) d[1 + i] = (uint8_t)(s[i] - (i >= 4 ? s[i - 4] : 0));
    }
    uLongf clen = compressBound((uLong)raw.size());
    std::vector<uint8_t> comp(clen);
    if (compress2(comp.data(), &clen, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
    ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    put_chunk(out, "IHDR", ihdr, 13);
    put_chunk(out, "IDAT", comp.data(), clen);
    put_chunk(out, "IEND", nullptr, 0);
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path) == 0;
}

bool psp_png_decode_rgba(const uint8_t* d, size_t len, std::vector<uint8_t>& rgba, int& W, int& H,
                         std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (len < 8 || std::memcmp(d, sig, 8)) return fail("not a PNG");
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    std::vector<uint8_t> idat, plte, trns;
    bool seen_end = false;
    while (pos + 12 <= len && !seen_end) {
        uint32_t clen = (d[pos] << 24) | (d[pos + 1] << 16) | (d[pos + 2] << 8) | d[pos + 3];
        const uint8_t* type = d + pos + 4;
        const uint8_t* body = d + pos + 8;
        if ((size_t)clen > len - pos - 12) return fail("truncated chunk");
        if (!std::memcmp(type, "IHDR", 4) && clen >= 13) {
            w = (body[0] << 24) | (body[1] << 16) | (body[2] << 8) | body[3];
            h = (body[4] << 24) | (body[5] << 16) | (body[6] << 8) | body[7];
            depth = body[8]; ctype = body[9]; interlace = body[12];
        } else if (!std::memcmp(type, "PLTE", 4)) plte.assign(body, body + clen);
        else if (!std::memcmp(type, "tRNS", 4)) trns.assign(body, body + clen);
        else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), body, body + clen);
        else if (!std::memcmp(type, "IEND", 4)) seen_end = true;
        pos += 12 + clen;
    }
    if (!w || !h || w > 16384 || h > 16384 || (uint64_t)w * h * 4 > (256u << 20)) return fail("bad size");
    if (interlace) return fail("interlaced PNG not supported (re-save without Adam7)");
    int ch;
    switch (ctype) { case 0: ch = 1; break; case 2: ch = 3; break; case 3: ch = 1; break;
                     case 4: ch = 2; break; case 6: ch = 4; break; default: return fail("bad colour type"); }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return fail("bad depth");
    if (ctype == 3 && plte.empty()) return fail("missing PLTE");
    const size_t bits_pp = (size_t)ch * depth;
    const size_t rb = (w * bits_pp + 7) / 8;
    const size_t bpp = std::max<size_t>(1, bits_pp / 8);
    const size_t expect = (rb + 1) * h;
    std::vector<uint8_t> raw(expect);
    uLongf dl = (uLongf)expect;
    if (uncompress(raw.data(), &dl, idat.data(), (uLong)idat.size()) != Z_OK || dl != expect)
        return fail("IDAT inflate failed");
    // Unfilter in place.
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = raw.data() + (size_t)y * (rb + 1);
        const uint8_t* prev = y ? row - (rb + 1) + 1 : nullptr;
        uint8_t ft = row[0];
        uint8_t* p = row + 1;
        for (size_t i = 0; i < rb; i++) {
            int a = i >= bpp ? p[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int add = 0;
            switch (ft) {
            case 0: break; case 1: add = a; break; case 2: add = b; break;
            case 3: add = (a + b) >> 1; break; case 4: add = paeth(a, b, c); break;
            default: return fail("bad filter");
            }
            p[i] = (uint8_t)(p[i] + add);
        }
    }
    rgba.assign((size_t)w * h * 4, 0);
    auto sample = [&](const uint8_t* p, size_t idx) -> int {   // idx-th sample in the row, raw value
        if (depth == 8) return p[idx];
        if (depth == 16) return (p[idx * 2] << 8) | p[idx * 2 + 1];
        size_t bit = idx * depth;
        return (p[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
    };
    const int maxv = (1 << depth) - 1;
    auto to8 = [&](int v) -> uint8_t { return depth == 8 ? (uint8_t)v : (uint8_t)((v * 255 + maxv / 2) / maxv); };
    int trns_gray = -1, trns_rgb[3] = {-1, -1, -1};
    if (ctype == 0 && trns.size() >= 2) trns_gray = (trns[0] << 8) | trns[1];
    if (ctype == 2 && trns.size() >= 6) for (int k = 0; k < 3; k++) trns_rgb[k] = (trns[k * 2] << 8) | trns[k * 2 + 1];
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* p = raw.data() + (size_t)y * (rb + 1) + 1;
        uint8_t* o = rgba.data() + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            switch (ctype) {
            case 0: { int v = sample(p, x); o[0] = o[1] = o[2] = to8(v); o[3] = (v == trns_gray) ? 0 : 255; break; }
            case 2: { int r = sample(p, x * 3), gg = sample(p, x * 3 + 1), b = sample(p, x * 3 + 2);
                      o[0] = to8(r); o[1] = to8(gg); o[2] = to8(b);
                      o[3] = (r == trns_rgb[0] && gg == trns_rgb[1] && b == trns_rgb[2]) ? 0 : 255; break; }
            case 3: { size_t i = (size_t)sample(p, x);
                      if (i * 3 + 2 < plte.size()) { o[0] = plte[i * 3]; o[1] = plte[i * 3 + 1]; o[2] = plte[i * 3 + 2]; }
                      o[3] = i < trns.size() ? trns[i] : 255; break; }
            case 4: { uint8_t v = to8(sample(p, x * 2)); o[0] = o[1] = o[2] = v; o[3] = to8(sample(p, x * 2 + 1)); break; }
            case 6: o[0] = to8(sample(p, x * 4)); o[1] = to8(sample(p, x * 4 + 1));
                    o[2] = to8(sample(p, x * 4 + 2)); o[3] = to8(sample(p, x * 4 + 3)); break;
            }
        }
    }
    W = (int)w; H = (int)h;
    return true;
}

// ----------------------------------------------------- pack operations ----

bool psp_texpack_load_replacement(uint64_t id, std::vector<uint8_t>& rgba, int& w, int& h) {
    init_once();
    if (!g.replace || !g.pack.count(id) || g.bad.count(id)) return false;
    if (auto c = g.repl_cache.find(id); c != g.repl_cache.end()) {
        g.repl_lru.splice(g.repl_lru.begin(), g.repl_lru, c->second.lru);
        rgba = c->second.px; w = c->second.w; h = c->second.h;
        return true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    char name[40];
    std::snprintf(name, sizeof name, "/%016llx.png", (unsigned long long)id);
    std::string path = g.dir + name;
    std::vector<uint8_t> file;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) { file.resize((size_t)n); if (std::fread(file.data(), 1, file.size(), f) != file.size()) file.clear(); }
        std::fclose(f);
    }
    std::string err = "unreadable";
    if (!file.empty() && psp_png_decode_rgba(file.data(), file.size(), rgba, w, h, &err)) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms >= g.slow_ms)
            std::fprintf(stderr, "[TEXPACK] slow load %016llx: %dx%d, %zu B png, %.1f ms (cached now, %zu MB in cache)\n",
                         (unsigned long long)id, w, h, file.size(), ms, (g.repl_bytes + rgba.size()) >> 20);
        const size_t sz = rgba.size();
        if (sz <= g.repl_budget) {
            while (g.repl_bytes + sz > g.repl_budget && !g.repl_lru.empty()) {
                auto old_it = g.repl_cache.find(g.repl_lru.back());
                g.repl_bytes -= old_it->second.px.size();
                g.repl_cache.erase(old_it);
                g.repl_lru.pop_back();
            }
            g.repl_lru.push_front(id);
            g.repl_cache[id] = State::ReplEntry{rgba, w, h, g.repl_lru.begin()};
            g.repl_bytes += sz;
        }
        return true;
    }
    std::fprintf(stderr, "[TEXPACK] bad replacement %s: %s (using original)\n", path.c_str(), err.c_str());
    g.bad.insert(id);
    return false;
}

void psp_texpack_flush() {
    if (!g.inited || !g.dump || !g.dirty) return;
    std::string tmp = g.index_path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# id size format clut-format first-addr seen\n");
    for (auto& kv : g.seen) {
        const Rec& r = kv.second;
        std::fprintf(f, "%016llx %dx%d %s %s %08x %llu\n", (unsigned long long)kv.first, r.w, r.h,
                     fmt_name(r.fmt), clut_name(r.clut_fmt), r.addr, (unsigned long long)r.seen);
    }
    std::fclose(f);
    std::rename(tmp.c_str(), g.index_path.c_str());
    g.dirty = false;
    g.last_flush = std::chrono::steady_clock::now();
}

void psp_texpack_note(uint64_t id, const PspTexMeta& m, const uint8_t* rgba) {
    init_once();
    if (!g.dump) return;
    auto it = g.seen.find(id);
    static std::unordered_set<uint64_t> done_this_run;
    if (it == g.seen.end()) {
        Rec r; r.w = m.w; r.h = m.h; r.fmt = m.fmt; r.clut_fmt = m.clut_fmt; r.addr = m.addr;
        it = g.seen.emplace(id, r).first;
    }
    it->second.seen++;
    g.dirty = true;
    if (done_this_run.insert(id).second && g.dump_count < g.dump_max) {
        char name[48];
        std::snprintf(name, sizeof name, "/%016llx.png", (unsigned long long)id);
        std::string path = g.dump_dir + name;
        if (!file_exists(path)) {
            if (psp_png_encode_rgba(path.c_str(), rgba, m.w, m.h)) { g.dump_count++; g.dumped_new++; }
        } else {
            g.dump_count++;
        }
        if (g.dump_count == g.dump_max)
            std::fprintf(stderr, "[TEXPACK] dump cap %zu reached (PSPRECOMP_TEX_DUMP_MAX)\n", g.dump_max);
    }
    auto now = std::chrono::steady_clock::now();
    if (g.dirty && now - g.last_flush > std::chrono::seconds(3)) {
        psp_texpack_flush();
        std::fprintf(stderr, "[TEXPACK] %zu distinct texture(s) seen, %zu new PNG(s) written\n", g.seen.size(),
                     g.dumped_new);
    }
}

// --------------------------------------------------------- upscaler ----
// Clean-room edge-directed pixel-art scaler (Scale2x/Scale3x rule family with a
// colour-distance equality test and alpha awareness). Not the xBRZ code (GPL);
// the xbrzN names select the scale factor. 4x = 2x applied twice.

namespace {
struct Px { uint8_t c[4]; };
inline bool near_eq(const uint8_t* a, const uint8_t* b) {
    int da = std::abs(a[3] - b[3]);
    if (da > 24) return false;
    if (a[3] < 8 && b[3] < 8) return true;
    int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    // weighted squared distance (red-mean-ish weights), threshold ~ 30/255 per channel
    return 2 * dr * dr + 4 * dg * dg + 3 * db * db < 3 * 30 * 30 * 3;
}

void scale2(const uint8_t* s, int w, int h, uint8_t* d) {
    auto at = [&](int x, int y) { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); return s + ((size_t)y * w + x) * 4; };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *B = at(x, y - 1), *D = at(x - 1, y), *E = at(x, y), *F = at(x + 1, y), *H = at(x, y + 1);
            bool bd = near_eq(B, D), bf = near_eq(B, F), dh = near_eq(D, H), hf = near_eq(H, F);
            const uint8_t* o[4] = {
                (bd && !bf && !dh) ? D : E, (bf && !bd && !hf) ? F : E,
                (dh && !bd && !hf) ? D : E, (hf && !dh && !bf) ? F : E};
            for (int k = 0; k < 4; k++)
                std::memcpy(d + (((size_t)(2 * y + (k >> 1)) * 2 * w) + 2 * x + (k & 1)) * 4, o[k], 4);
        }
}

void scale3(const uint8_t* s, int w, int h, uint8_t* d) {
    auto at = [&](int x, int y) { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); return s + ((size_t)y * w + x) * 4; };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *A = at(x - 1, y - 1), *B = at(x, y - 1), *C = at(x + 1, y - 1), *D = at(x - 1, y),
                          *E = at(x, y), *F = at(x + 1, y), *G = at(x - 1, y + 1), *H = at(x, y + 1), *I = at(x + 1, y + 1);
            auto eq = [](const uint8_t* p, const uint8_t* q) { return near_eq(p, q); };
            bool bd = eq(D, B), bf = eq(B, F), hd = eq(H, D), hf = eq(H, F);
            bool B_F = !bf, D_H = !hd;   // (placeholders kept for readability below)
            (void)B_F; (void)D_H;
            const uint8_t* o[9];
            o[0] = (bd && !bf && !hd) ? D : E;
            o[1] = ((bd && !bf && !hd && !eq(E, C)) || (bf && !bd && !hf && !eq(E, A))) ? B : E;
            o[2] = (bf && !bd && !hf) ? F : E;
            o[3] = ((hd && !bd && !hf && !eq(E, A)) || (bd && !hd && !bf && !eq(E, G))) ? D : E;
            o[4] = E;
            o[5] = ((bf && !bd && !hf && !eq(E, I)) || (hf && !bf && !hd && !eq(E, C))) ? F : E;
            o[6] = (hd && !hf && !bd) ? D : E;
            o[7] = ((hd && !hf && !bd && !eq(E, I)) || (hf && !hd && !bf && !eq(E, G))) ? H : E;
            o[8] = (hf && !hd && !bf) ? F : E;
            for (int k = 0; k < 9; k++)
                std::memcpy(d + (((size_t)(3 * y + k / 3) * 3 * w) + 3 * x + (k % 3)) * 4, o[k], 4);
        }
}
}  // namespace

void psp_texpack_scale_rgba(const uint8_t* src, int w, int h, int factor, std::vector<uint8_t>& out) {
    if (factor == 2) { out.resize((size_t)w * h * 16); scale2(src, w, h, out.data()); }
    else if (factor == 3) { out.resize((size_t)w * h * 36); scale3(src, w, h, out.data()); }
    else if (factor == 4) {
        std::vector<uint8_t> t((size_t)w * h * 16);
        scale2(src, w, h, t.data());
        out.resize((size_t)w * h * 64);
        scale2(t.data(), w * 2, h * 2, out.data());
    } else { out.assign(src, src + (size_t)w * h * 4); }
}

bool psp_texpack_upscale(uint64_t id, const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out,
                         int& ow, int& oh) {
    init_once();
    if (!g.upscale_on) return false;
    if ((int64_t)w * g.upscale > 8192 || (int64_t)h * g.upscale > 8192) return false;
    auto it = g.up_cache.find(id);
    if (it != g.up_cache.end()) { out = it->second.px; ow = it->second.w; oh = it->second.h; return true; }
    psp_texpack_scale_rgba(rgba, w, h, g.upscale, out);
    ow = w * g.upscale; oh = h * g.upscale;
    if (g.up_bytes + out.size() <= g.up_budget) {
        g.up_bytes += out.size();
        g.up_cache[id] = {out, ow, oh};
    }
    return true;
}
