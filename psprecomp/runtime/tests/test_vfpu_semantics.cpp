// VFPU semantics host test: differential check of the runtime's vfpu_* translations
// (runtime/src/psp_vfpu_*.cpp) against PPSSPP's interpreter (Core/MIPS/InterpreterVFPU.cpp and
// MIPSVFPUUtils.cpp), which is compiled unmodified from a local PPSSPP checkout through
// runtime/tests/vfpu_ref/ref_adapter.cpp. Both sides start from the same random register file,
// prefixes, CC and memory; every VFPU register, control register, GPR and memory byte is compared
// bit for bit afterwards.
//
// Each op is exercised with the opcode field combinations that exist on hardware (valid sizes, valid
// register encodings, every vcmp condition, every vrot immediate, ...) under three prefix regimes:
//   plain : no prefix pending
//   sane  : random S/T/D prefixes whose swizzles name only lanes < size (constants, abs, neg, sat, masks)
//   wild  : any prefix bits, including swizzles that name lanes past the vector (hardware quirks)
// A mismatch is reported with both values. "nanbits" mismatches (both sides NaN, payload differs) are
// counted separately; they never fail the run unless --strict-nan is given.
//
// Build/run: runtime/tests/run_vfpu_semantics.sh [--iters N] [--op NAME] [--strict-nan] [--wild-fails]
#include "psp_vfpu.h"
#include "recomp.h"
#include "vfpu_ref/ref_adapter.h"
#include <cinttypes>
#include <cstdarg>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

void psp_dump_func_ring(const char*) {}

// ---------------------------------------------------------------------------
// RNG and value pools
// ---------------------------------------------------------------------------
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static uint32_t rndn(uint32_t n) { return rnd() % n; }
static float bits2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
static uint32_t f2bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

enum class Pool { Mixed, Safe, IntBits, Fin };
static float rnd_value(Pool p) {
    switch (p) {
    case Pool::IntBits:
        switch (rndn(6)) {
        case 0: return bits2f(rndn(512));
        case 1: return bits2f((uint32_t)(-(int32_t)rndn(512)));
        case 2: return bits2f(rnd());
        case 3: return bits2f(0x7FFFFFFFu);
        case 4: return bits2f(0x80000000u);
        default: return bits2f(rnd() & 0xFF00FF00u);
        }
    case Pool::Fin:
        switch (rndn(10)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return (float)((int)rndn(9) - 4);
        case 3: return bits2f(0x3F000000u | (rnd() & 0x00FFFFFFu));              // [0.5, 2)
        case 4: return (float)((int)rndn(40001) - 20000) / 16.0f;
        case 5: return -bits2f(0x3F800000u | (rnd() & 0x007FFFFFu));
        default: return (float)((int)rndn(2001) - 1000) / 64.0f;
        }
    case Pool::Safe: {
        float v = (float)((int)rndn(2001) - 1000) / 64.0f;
        if (rndn(8) == 0) v = (float)((int)rndn(5) - 2);
        return v;
    }
    default: break;
    }
    switch (rndn(24)) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return bits2f(0x7F800000u);
    case 3: return bits2f(0xFF800000u);
    case 4: return bits2f(0x7FC00000u | rndn(16));
    case 5: return bits2f(rndn(0x7FFFFFu));          // denormal
    case 6: return bits2f(0x80000000u | rndn(0x7FFFFFu));
    case 7: return 1.0f;
    case 8: return -1.0f;
    case 9: return (float)((int)rndn(9) - 4);
    case 10: return bits2f(0x7F000000u | rndn(0x7FFFFFu)); // huge
    case 11: return bits2f(0x00800000u | rndn(0x100000u)); // tiny normal
    case 12: return (float)((int)rndn(2001) - 1000) / 8.0f;
    case 13: return 0.5f;
    case 14: return (float)rndn(256);
    default: return (float)((double)((int)rnd()) / 4294967296.0 * 8.0);
    }
}

// ---------------------------------------------------------------------------
// Test harness state
// ---------------------------------------------------------------------------
enum Regime { PLAIN = 0, SANE = 1, WILD = 2 };
static const char* kRegimeName[] = {"plain", "sane", "wild"};

struct Case {
    uint32_t op = 0;       // PPSSPP opcode word
    int size = 1;
    int vd = 0, vs = 0, vt = 0, imm = 0;
    int rs = 0;            // GPR for memory ops / mfv
    int regime = PLAIN;
};

struct OpSpec {
    const char* name;
    const char* refFn;                       // PPSSPP Int_ function
    uint32_t sizeMask;                       // bit n set = size n valid (n = 1..4)
    std::function<void(Case&)> setup;        // fill op / imm / extra fields (after size/vd/vs/vt chosen)
    std::function<void(recomp_context*, uint8_t*, const Case&)> ours;
    Pool pool = Pool::Mixed;
    bool matrix = false;                     // registers are matrix encodings
    bool noVd = false, noVs = false, noVt = false;
    bool nanTolerant = false;                // accept any NaN payload / sign
    bool noPrefixTest = false;               // run plain only
    bool quadSplit = false;                  // vd/vs encode sizes derived from 'size' differently
    int vsSizeOverride = 0;                  // size used to validate vs encodings (0 = size)
    int vdSizeOverride = 0;
    bool wildOnlyInformational = false;      // wild regime never fails
    bool dprefixOnly = false;
    bool noDPrefix = false;
    bool keepsPrefix = false;                // op leaves pending prefixes alone (loads/stores/mfv ...)
};

static uint32_t size_bits(int n) {
    return (((n - 1) & 1) << 7) | ((((n - 1) >> 1) & 1) << 15);
}

static int gen_reg(int n, bool matrix) {
    int r = (int)rndn(128);
    if (n == 4) r &= ~0x40;           // quad vectors/matrices start at row 0
    if (matrix && n == 4) r &= ~0x03; // and column 0
    return r;
}

// Build a random prefix. valid lanes < n unless wild.
static uint32_t gen_st_prefix(int n, Regime reg) {
    if (reg == PLAIN || rndn(5) == 0) return 0xE4;
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sw = reg == WILD ? rndn(4) : rndn(n < 1 ? 1 : n);
        uint32_t ab = rndn(2), cn = rndn(4) == 0, ng = rndn(3) == 0;
        p |= sw << (i * 2) | ab << (8 + i) | cn << (12 + i) | ng << (16 + i);
    }
    return p;
}
static uint32_t gen_d_prefix(Regime reg) {
    if (reg == PLAIN || rndn(5) == 0) return 0;
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sat = rndn(3);                   // 0, 1 or 3
        if (sat == 2) sat = 3;
        if (reg == WILD && rndn(8) == 0) sat = 2;
        p |= sat << (i * 2);
        if (rndn(4) == 0) p |= 1u << (8 + i);
    }
    return p;
}

static void randomize(RefState& st, Pool pool) {
    for (int i = 0; i < 128; i++) st.v[i] = rnd_value(pool);
    for (int i = 0; i < 16; i++) st.ctrl[i] = 0;
    st.ctrl[3] = rnd() & 0x3F;
    for (int i = 0; i < 32; i++) st.r[i] = rnd();
}

struct Stats { long runs = 0, bad = 0, nanbits = 0; };
static std::map<std::string, Stats> g_stats;
static std::map<std::string, int> g_printed;
static bool g_strict_nan = false;
static bool g_wild_fails = false;
static const char* g_only = nullptr;
static long g_total_bad = 0;
static int g_maxprint = 2;

static const char* ctrl_name(int i) {
    static char b[16];
    switch (i) { case 0: return "SPFX"; case 1: return "TPFX"; case 2: return "DPFX"; case 3: return "CC"; default: snprintf(b, 16, "ctrl%d", i); return b; }
}

static void run_op(const OpSpec& spec, int fnid, long iters) {
    for (long it = 0; it < iters; it++) {
        Case c;
        // pick size
        int size;
        do { size = 1 + (int)rndn(4); } while (!(spec.sizeMask & (1u << size)));
        c.size = size;
        int vsz = spec.vsSizeOverride ? spec.vsSizeOverride : size;
        int dsz = spec.vdSizeOverride ? spec.vdSizeOverride : size;
        c.vd = gen_reg(dsz, spec.matrix);
        c.vs = gen_reg(vsz, spec.matrix);
        c.vt = gen_reg(vsz, spec.matrix);
        c.rs = 1 + (int)rndn(30);
        c.regime = spec.noPrefixTest ? PLAIN : (int)rndn(3);
        spec.setup(c);

        const bool fin = spec.pool == Pool::Mixed && rndn(10) < 6;
        const Pool pool = fin ? Pool::Fin : spec.pool;
        RefState a;
        randomize(a, pool);
        // memory: random bytes, same for both
        unsigned char* refmem = ref_mem();
        const int memBytes = spec.keepsPrefix ? 0x10000 : 0x400;   // memory ops need the whole window
        for (int i = 0; i < memBytes; i += 4) { uint32_t u = f2bits(rnd_value(pool)); std::memcpy(refmem + i, &u, 4); }
        // base register for loads/stores: address inside the 64KiB window, 16-byte aligned, with room
        if (spec.keepsPrefix) {
            a.r[c.rs] = 0x08000000u + 0x400 + (rnd() & 0x3FF0);
        }
        if (c.regime != PLAIN || rndn(4) == 0) {
            Regime rg = (Regime)c.regime;
            int pn = size;
            a.ctrl[0] = gen_st_prefix(pn, rg);
            a.ctrl[1] = gen_st_prefix(pn, rg);
            a.ctrl[2] = spec.noDPrefix ? 0 : gen_d_prefix(rg);
            if (rg == PLAIN) { a.ctrl[0] = 0xE4; a.ctrl[1] = 0xE4; a.ctrl[2] = 0; }
        } else {
            a.ctrl[0] = 0xE4; a.ctrl[1] = 0xE4; a.ctrl[2] = 0;
        }
        if (spec.noPrefixTest) { a.ctrl[0] = 0xE4; a.ctrl[1] = 0xE4; a.ctrl[2] = 0; }

        // ours
        recomp_context cc;
        std::memset(&cc, 0, sizeof(cc));
        std::memcpy(cc.vfpu, a.v, sizeof(a.v));
        std::memcpy(cc.vfpu_ctrl, a.ctrl, sizeof(a.ctrl));
        for (int i = 0; i < 32; i++) cc.r[i] = (int32_t)a.r[i];
        static std::vector<uint8_t> rdram;
        if (rdram.empty()) rdram.assign(0x10000, 0);
        std::memcpy(rdram.data(), refmem, memBytes);

        RefState before = a;
        spec.ours(&cc, rdram.data(), c);

        ref_run(fnid, c.op, &a);

        // compare
        Stats& s = g_stats[spec.name];
        s.runs++;
        bool bad = false, nanonly = true;
        char msg[1024]; int mp = 0;
        auto note = [&](const char* fmt, ...) {
            va_list ap; va_start(ap, fmt);
            if (mp < (int)sizeof(msg) - 200) mp += vsnprintf(msg + mp, sizeof(msg) - mp, fmt, ap);
            va_end(ap);
        };
        for (int i = 0; i < 128; i++) {
            uint32_t x = f2bits(cc.vfpu[i]), y = f2bits(a.v[i]);
            if (x == y) continue;
            bool bothnan = std::isnan(cc.vfpu[i]) && std::isnan(a.v[i]);
            if (bothnan || (spec.nanTolerant && (std::isnan(cc.vfpu[i]) || std::isnan(a.v[i])) && false)) {
                s.nanbits++;
                if (g_strict_nan) { bad = true; nanonly = false; note(" v[%02X..] ours=%08X ref=%08X;", i, x, y); }
                continue;
            }
            bad = true; nanonly = false;
            // report as logical reg name of physical index i: invert vfpu_single_index
            int mtx = i / 16, col = (i / 4) & 3, row = i & 3;
            int logical = row * 32 + mtx * 4 + col;
            note(" reg 0x%02X ours=%08X(%g) ref=%08X(%g);", logical, x, cc.vfpu[i], y, a.v[i]);
        }
        for (int i = 0; i < 16; i++) {
            if (cc.vfpu_ctrl[i] != a.ctrl[i]) { bad = true; nanonly = false; note(" %s ours=%08X ref=%08X;", ctrl_name(i), cc.vfpu_ctrl[i], a.ctrl[i]); }
        }
        for (int i = 0; i < 32; i++) {
            if ((uint32_t)cc.r[i] != a.r[i]) { bad = true; nanonly = false; note(" r%d ours=%08X ref=%08X;", i, (uint32_t)cc.r[i], a.r[i]); }
        }
        if (std::memcmp(rdram.data(), refmem, memBytes) != 0) {
            bad = true; nanonly = false;
            for (int i = 0; i < memBytes; i += 4)
                if (std::memcmp(rdram.data() + i, refmem + i, 4)) { uint32_t x, y; std::memcpy(&x, rdram.data() + i, 4); std::memcpy(&y, refmem + i, 4); note(" mem[%04X] ours=%08X ref=%08X;", i, x, y); break; }
        }
        (void)nanonly;
        if (!bad) continue;
        bool informational = spec.wildOnlyInformational && c.regime == WILD;
        if (c.regime == WILD && !g_wild_fails) informational = true;
        std::string key = std::string(spec.name) + "/" + kRegimeName[c.regime] + (fin ? "/fin" : "/any");
        Stats& sk = g_stats[key];
        sk.runs++; sk.bad++;
        if (!informational) { s.bad++; g_total_bad++; }
        if (g_printed[key]++ < g_maxprint) {
            std::printf("MISMATCH %s %s size=%d vd=%02X vs=%02X vt=%02X imm=%d op=%08X S=%08X T=%08X D=%08X CC=%02X\n   %s\n",
                        spec.name, kRegimeName[c.regime], c.size, c.vd, c.vs, c.vt, c.imm, c.op,
                        before.ctrl[0], before.ctrl[1], before.ctrl[2], before.ctrl[3], msg);
        }
    }
}

// ---------------------------------------------------------------------------
// Op table
// ---------------------------------------------------------------------------
#define SZ(...) szmask({__VA_ARGS__})
static uint32_t szmask(std::initializer_list<int> l) { uint32_t m = 0; for (int n : l) m |= 1u << n; return m; }

using Ctx = recomp_context*;
using Mem = uint8_t*;

static std::vector<OpSpec> build_specs() {
    std::vector<OpSpec> v;
    auto vecop = [](uint32_t base, const Case& c) {
        return base | size_bits(c.size) | ((uint32_t)c.vt << 16) | ((uint32_t)c.vs << 8) | (uint32_t)c.vd;
    };
    auto unop = [](uint32_t base, const Case& c) {
        return base | size_bits(c.size) | ((uint32_t)c.vs << 8) | (uint32_t)c.vd;
    };
    auto binary = [&](const char* name, const char* fn, uint32_t base, uint32_t sizes,
                      void (*call)(Ctx, Mem, uint8_t, uint8_t, uint8_t, uint8_t)) {
        OpSpec s;
        s.name = name; s.refFn = fn; s.sizeMask = sizes;
        s.setup = [=](Case& c) { c.op = vecop(base, c); };
        s.ours = [=](Ctx x, Mem m, const Case& c) { call(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.vt, (uint8_t)c.size); };
        v.push_back(s);
    };
    auto unary = [&](const char* name, const char* fn, uint32_t base, uint32_t sizes,
                     void (*call)(Ctx, Mem, uint8_t, uint8_t, uint8_t)) {
        OpSpec s;
        s.name = name; s.refFn = fn; s.sizeMask = sizes;
        s.setup = [=](Case& c) { c.op = unop(base, c); };
        s.ours = [=](Ctx x, Mem m, const Case& c) { call(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.size); };
        v.push_back(s);
    };
    auto unary_imm = [&](const char* name, const char* fn, uint32_t base, uint32_t sizes, Pool pool,
                         void (*call)(Ctx, Mem, uint8_t, uint8_t, uint8_t, uint8_t)) {
        OpSpec s;
        s.name = name; s.refFn = fn; s.sizeMask = sizes; s.pool = pool;
        s.setup = [=](Case& c) { c.imm = (int)rndn(32); c.op = unop(base, c) | ((uint32_t)c.imm << 16); };
        s.ours = [=](Ctx x, Mem m, const Case& c) { call(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.imm, (uint8_t)c.size); };
        v.push_back(s);
    };

    // ---- binary vector ops
    binary("vadd", "Int_VecDo3", (24u << 26) | (0u << 23), SZ(1, 2, 3, 4), vfpu_vadd);
    binary("vsub", "Int_VecDo3", (24u << 26) | (1u << 23), SZ(1, 2, 3, 4), vfpu_vsub);
    binary("vdiv", "Int_VecDo3", (24u << 26) | (7u << 23), SZ(1, 2, 3, 4), vfpu_vdiv);
    binary("vmul", "Int_VecDo3", (25u << 26) | (0u << 23), SZ(1, 2, 3, 4), vfpu_vmul);
    binary("vmin", "Int_Vminmax", (27u << 26) | (2u << 23), SZ(1, 2, 3, 4), vfpu_vmin);
    binary("vmax", "Int_Vminmax", (27u << 26) | (3u << 23), SZ(1, 2, 3, 4), vfpu_vmax);
    binary("vscmp", "Int_Vscmp", (27u << 26) | (5u << 23), SZ(1, 2, 3, 4), vfpu_vscmp);
    binary("vsge", "Int_Vsge", (27u << 26) | (6u << 23), SZ(1, 2, 3, 4), vfpu_vsge);
    binary("vslt", "Int_Vslt", (27u << 26) | (7u << 23), SZ(1, 2, 3, 4), vfpu_vslt);
    binary("vdot", "Int_VDot", (25u << 26) | (1u << 23), SZ(2, 3, 4), vfpu_vdot);
    binary("vhdp", "Int_VHdp", (25u << 26) | (4u << 23), SZ(2, 3, 4), vfpu_vhdp);
    binary("vcrs", "Int_Vcrs", (25u << 26) | (5u << 23), SZ(3), vfpu_vcrs);
    binary("vdet", "Int_Vdet", (25u << 26) | (6u << 23), SZ(2), vfpu_vdet);
    {
        // vscl: vt is a single register; pick the T lane via vt's own row bits
        OpSpec s;
        s.name = "vscl"; s.refFn = "Int_VScl"; s.sizeMask = SZ(2, 3, 4);
        s.setup = [=](Case& c) { c.op = vecop((25u << 26) | (2u << 23), c); };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vscl(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.vt, (uint8_t)c.size); };
        v.push_back(s);
    }
    {
        OpSpec s;
        s.name = "vcmp"; s.refFn = "Int_Vcmp"; s.sizeMask = SZ(1, 2, 3, 4);
        s.setup = [=](Case& c) { c.imm = (int)rndn(16); c.op = vecop((27u << 26) | (0u << 23), c) | (uint32_t)c.imm; c.op &= ~0x7Fu; c.op |= (uint32_t)c.imm; };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vcmp(x, m, (uint8_t)c.vs, (uint8_t)c.vt, (uint8_t)c.imm, (uint8_t)c.size); };
        v.push_back(s);
    }
    {
        OpSpec s;
        s.name = "vcmov"; s.refFn = "Int_Vcmov"; s.sizeMask = SZ(1, 2, 3, 4);
        s.setup = [=](Case& c) {
            int imm3 = (int)rndn(8), tf = (int)rndn(2);
            c.imm = imm3 | (tf << 3);
            c.op = (52u << 26) | (21u << 21) | size_bits(c.size) | ((uint32_t)imm3 << 16) | ((uint32_t)tf << 19) | ((uint32_t)c.vs << 8) | (uint32_t)c.vd;
        };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vcmov(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.imm, (uint8_t)c.size); };
        v.push_back(s);
    }

    // ---- unary / math
    struct U { const char* n; int sub; void (*f)(Ctx, Mem, uint8_t, uint8_t, uint8_t); };
    static const U us[] = {
        {"vmov", 0, vfpu_vmov}, {"vabs", 1, vfpu_vabs}, {"vneg", 2, vfpu_vneg}, {"vsat0", 4, vfpu_vsat0},
        {"vsat1", 5, vfpu_vsat1}, {"vrcp", 16, vfpu_vrcp}, {"vrsq", 17, vfpu_vrsq}, {"vsin", 18, vfpu_vsin},
        {"vcos", 19, vfpu_vcos}, {"vexp2", 20, vfpu_vexp2}, {"vlog2", 21, vfpu_vlog2}, {"vsqrt", 22, vfpu_vsqrt},
        {"vasin", 23, vfpu_vasin}, {"vnrcp", 24, vfpu_vnrcp}, {"vnsin", 26, vfpu_vnsin}, {"vrexp2", 28, vfpu_vrexp2},
    };
    for (const U& u : us)
        unary(u.n, "Int_VV2Op", (52u << 26) | ((uint32_t)u.sub << 16), SZ(1, 2, 3, 4), u.f);
    unary("vocp", "Int_Vocp", (52u << 26) | (2u << 21) | (4u << 16), SZ(1, 2, 3, 4), vfpu_vocp);
    unary("vsgn", "Int_Vsgn", (52u << 26) | (2u << 21) | (10u << 16), SZ(1, 2, 3, 4), vfpu_vsgn);
    unary("vfad", "Int_Vfad", (52u << 26) | (2u << 21) | (6u << 16), SZ(2, 3, 4), vfpu_vfad);
    unary("vavg", "Int_Vavg", (52u << 26) | (2u << 21) | (7u << 16), SZ(2, 3, 4), vfpu_vavg);
    unary("vsrt1", "Int_Vsrt1", (52u << 26) | (2u << 21) | (0u << 16), SZ(4), vfpu_vsrt1);
    unary("vsrt2", "Int_Vsrt2", (52u << 26) | (2u << 21) | (1u << 16), SZ(4), vfpu_vsrt2);
    unary("vsrt3", "Int_Vsrt3", (52u << 26) | (2u << 21) | (8u << 16), SZ(4), vfpu_vsrt3);
    unary("vsrt4", "Int_Vsrt4", (52u << 26) | (2u << 21) | (9u << 16), SZ(4), vfpu_vsrt4);
    unary("vbfy1", "Int_Vbfy", (52u << 26) | (2u << 21) | (2u << 16), SZ(2, 4), vfpu_vbfy1);
    unary("vbfy2", "Int_Vbfy", (52u << 26) | (2u << 21) | (3u << 16), SZ(4), vfpu_vbfy2);
    unary("vsocp", "Int_Vsocp", (52u << 26) | (2u << 21) | (5u << 16), SZ(1, 2), vfpu_vsocp);
    {
        OpSpec s;
        s.name = "vcst"; s.refFn = "Int_Vcst"; s.sizeMask = SZ(1, 2, 3, 4); s.noVs = true;
        s.setup = [=](Case& c) { c.imm = 1 + (int)rndn(19); c.op = (52u << 26) | (3u << 21) | ((uint32_t)c.imm << 16) | size_bits(c.size) | (uint32_t)c.vd; };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vcst(x, m, (uint8_t)c.vd, (uint8_t)c.imm, (uint8_t)c.size); };
        v.push_back(s);
    }
    {
        struct I { const char* n; uint32_t idx; void (*f)(Ctx, Mem, uint8_t, uint8_t, uint8_t, uint8_t); };
        static const I is[] = {{"vf2in", 16, vfpu_vf2in}, {"vf2iz", 17, vfpu_vf2iz}, {"vf2iu", 18, vfpu_vf2iu}, {"vf2id", 19, vfpu_vf2id}};
        for (const I& i : is)
            unary_imm(i.n, "Int_Vf2i", (52u << 26) | (i.idx << 21), SZ(1, 2, 3, 4), Pool::Mixed, i.f);
        unary_imm("vi2f", "Int_Vi2f", (52u << 26) | (20u << 21), SZ(1, 2, 3, 4), Pool::IntBits, vfpu_vi2f);
    }
    {
        struct X { const char* n; const char* fn; uint32_t sub; uint32_t sizes; Pool pool; void (*f)(Ctx, Mem, uint8_t, uint8_t, uint8_t); };
        static const X xs[] = {
            {"vf2h", "Int_Vf2h", 18, SZ(2, 4), Pool::Mixed, vfpu_vf2h}, {"vh2f", "Int_Vh2f", 19, SZ(1, 2), Pool::IntBits, vfpu_vh2f},
            {"vuc2i", "Int_Vx2i", 24, SZ(1), Pool::IntBits, vfpu_vuc2i}, {"vc2i", "Int_Vx2i", 25, SZ(1), Pool::IntBits, vfpu_vc2i},
            {"vus2i", "Int_Vx2i", 26, SZ(1, 2), Pool::IntBits, vfpu_vus2i}, {"vs2i", "Int_Vx2i", 27, SZ(1, 2), Pool::IntBits, vfpu_vs2i},
            {"vi2uc", "Int_Vi2x", 28, SZ(4), Pool::IntBits, vfpu_vi2uc}, {"vi2c", "Int_Vi2x", 29, SZ(4), Pool::IntBits, vfpu_vi2c},
            {"vi2us", "Int_Vi2x", 30, SZ(2, 4), Pool::IntBits, vfpu_vi2us}, {"vi2s", "Int_Vi2x", 31, SZ(2, 4), Pool::IntBits, vfpu_vi2s},
        };
        for (const X& x : xs) {
            OpSpec s;
            s.name = x.n; s.refFn = x.fn; s.sizeMask = x.sizes; s.pool = x.pool;
            uint32_t base = (52u << 26) | (1u << 21) | (x.sub << 16);
            auto f = x.f;
            s.setup = [=](Case& c) { c.op = unop(base, c); };
            s.ours = [=](Ctx cx, Mem m, const Case& c) { f(cx, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.size); };
            // result sizes differ from source sizes: derive vd validity from the destination size
            if (!strcmp(x.n, "vuc2i") || !strcmp(x.n, "vc2i")) s.vdSizeOverride = 4;
            else if (!strcmp(x.n, "vus2i") || !strcmp(x.n, "vs2i") || !strcmp(x.n, "vh2f")) s.vdSizeOverride = 0;  // set below
            if (!strcmp(x.n, "vi2uc") || !strcmp(x.n, "vi2c")) s.vdSizeOverride = 1;
            if (!strcmp(x.n, "vi2us") || !strcmp(x.n, "vi2s") || !strcmp(x.n, "vf2h")) s.vdSizeOverride = 0;
            v.push_back(s);
        }
    }
    {
        OpSpec s;
        s.name = "vwbn"; s.refFn = "Int_Vwbn"; s.sizeMask = SZ(1, 2, 3, 4); s.pool = Pool::Mixed;
        s.setup = [=](Case& c) { c.imm = (int)rndn(256); c.op = (52u << 26) | ((uint32_t)c.imm << 16) | size_bits(c.size) | ((uint32_t)c.vs << 8) | (uint32_t)c.vd; c.op = (c.op & ~(0x1Fu << 21)) | (0x18u << 21) | ((uint32_t)(c.imm & 0xFF) << 16); };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vwbn(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.imm, (uint8_t)c.size); };
        v.push_back(s);
    }

    // ---- vidt / vzero / vone, vector init
    {
        OpSpec s;
        s.name = "vidt"; s.refFn = "Int_Vidt"; s.sizeMask = SZ(2, 3, 4); s.noVs = true;
        s.setup = [=](Case& c) { c.op = (52u << 26) | (3u << 16) | size_bits(c.size) | (uint32_t)c.vd; };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vidt(x, m, (uint8_t)c.vd, (uint8_t)c.size); };
        v.push_back(s);
    }
    {
        OpSpec s;
        s.name = "vzero"; s.refFn = "Int_VVectorInit"; s.sizeMask = SZ(1, 2, 3, 4);
        s.setup = [=](Case& c) { c.op = (52u << 26) | (6u << 16) | size_bits(c.size) | (uint32_t)c.vd; };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vzero(x, m, (uint8_t)c.vd, (uint8_t)c.size); };
        v.push_back(s);
        OpSpec o = s;
        o.name = "vone";
        o.setup = [=](Case& c) { c.op = (52u << 26) | (7u << 16) | size_bits(c.size) | (uint32_t)c.vd; };
        o.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vone(x, m, (uint8_t)c.vd, (uint8_t)c.size); };
        v.push_back(o);
    }

    // ---- matrix ops
    {
        struct M { const char* n; const char* fn; uint32_t base; void (*f)(Ctx, Mem, uint8_t, uint8_t, uint8_t, uint8_t); };
        static const M ms[] = {
            {"vmmul", "Int_Vmmul", (60u << 26) | (0u << 23), vfpu_vmmul},
            {"vmscl", "Int_Vmscl", (60u << 26) | (4u << 23), vfpu_vmscl},
        };
        for (const M& m : ms) {
            OpSpec s;
            s.name = m.n; s.refFn = m.fn; s.sizeMask = SZ(2, 3, 4); s.matrix = true;
            uint32_t base = m.base;
            auto f = m.f;
            bool scl = !strcmp(m.n, "vmscl");
            s.setup = [=](Case& c) {
                if (scl) c.vt = (int)rndn(128);   // single register
                c.op = vecop(base, c);
            };
            s.ours = [=](Ctx x, Mem mm, const Case& c) { f(x, mm, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.vt, (uint8_t)c.size); };
            v.push_back(s);
        }
        OpSpec mov;
        mov.name = "vmmov"; mov.refFn = "Int_Vmmov"; mov.sizeMask = SZ(2, 3, 4); mov.matrix = true;
        mov.setup = [=](Case& c) { c.op = vecop((60u << 26) | (3u << 23), c) & ~(0x7Fu << 16); };
        mov.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vmmov(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.size); };
        v.push_back(mov);
        struct MI { const char* n; int sub; void (*f)(Ctx, Mem, uint8_t, uint8_t); };
        static const MI mis[] = {{"vmidt", 3, vfpu_vmidt}, {"vmzero", 6, vfpu_vmzero}, {"vmone", 7, vfpu_vmone}};
        for (const MI& m : mis) {
            OpSpec s;
            s.name = m.n; s.refFn = "Int_VMatrixInit"; s.sizeMask = SZ(2, 3, 4); s.matrix = true; s.noVs = true;
            int sub = m.sub; auto f = m.f;
            s.setup = [=](Case& c) { c.op = (60u << 26) | (7u << 23) | ((uint32_t)sub << 16) | size_bits(c.size) | (uint32_t)c.vd; };
            s.ours = [=](Ctx x, Mem mm, const Case& c) { f(x, mm, (uint8_t)c.vd, (uint8_t)c.size); };
            v.push_back(s);
        }
        // vtfm/vhtfm: vtfmN: ins=N-1, size N; vhtfmN: ins=N, size N. Source matrix is (ins+1)^2, vector size N
        struct T { const char* n; int ins; int n_; void (*f)(Ctx, Mem, uint8_t, uint8_t, uint8_t); };
        static const T ts[] = {
            {"vtfm2", 1, 2, vfpu_vtfm2}, {"vtfm3", 2, 3, vfpu_vtfm3}, {"vtfm4", 3, 4, vfpu_vtfm4},
            {"vhtfm2", 2, 2, vfpu_vhtfm2}, {"vhtfm3", 3, 3, vfpu_vhtfm3},
        };
        for (const T& t : ts) {
            OpSpec s;
            s.name = t.n; s.refFn = "Int_Vtfm"; s.sizeMask = 1u << t.n_; s.matrix = false;
            int ins = t.ins; auto f = t.f; int nn = t.n_;
            s.setup = [=](Case& c) {
                // vs is a (ins+1)x(ins+1) matrix; vt, vd are vectors of size n (vd has ins+1 lanes)
                int msz = ins + 1;
                c.vs = gen_reg(msz, true);
                c.vt = gen_reg(nn, false);
                c.vd = gen_reg(ins + 1, false);
                c.op = (60u << 26) | ((uint32_t)ins << 23) | size_bits(nn) | ((uint32_t)c.vt << 16) | ((uint32_t)c.vs << 8) | (uint32_t)c.vd;
            };
            s.ours = [=](Ctx x, Mem m, const Case& c) { f(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.vt); };
            v.push_back(s);
        }
    }
    {
        OpSpec s;
        s.name = "vcrsp"; s.refFn = "Int_CrossQuat"; s.sizeMask = SZ(3);
        s.setup = [=](Case& c) { c.op = vecop((60u << 26) | (5u << 23), c); };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vcrsp(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.vt); };
        v.push_back(s);
        OpSpec q = s;
        q.name = "vqmul"; q.sizeMask = SZ(4);
        q.setup = [=](Case& c) { c.op = vecop((60u << 26) | (5u << 23), c); };
        q.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vqmul(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint8_t)c.vt); };
        v.push_back(q);
    }
    {
        OpSpec s;
        s.name = "vrot"; s.refFn = "Int_Vrot"; s.sizeMask = SZ(2, 3, 4); s.noVt = true;
        s.setup = [=](Case& c) {
            int n = c.size;
            int cosl = (int)rndn(n), sinl = (int)rndn(n);
            if (rndn(4) == 0) sinl = cosl;          // the "all other lanes carry sin" form
            c.imm = (rndn(2) << 4) | (sinl << 2) | cosl;
            c.vs = (int)rndn(128);
            c.op = (60u << 26) | (3u << 21) | ((uint32_t)c.imm << 16) | size_bits(n) | ((uint32_t)c.vs << 8) | (uint32_t)c.vd;
        };
        s.ours = [=](Ctx x, Mem m, const Case& c) { vfpu_vrot(x, m, (uint8_t)c.vd, (uint8_t)c.vs, (uint16_t)c.imm, (uint8_t)c.size); };
        s.pool = Pool::Mixed;
        v.push_back(s);
    }

    // ---- immediates
    {
        OpSpec s;
        s.name = "viim"; s.refFn = "Int_Viim"; s.sizeMask = SZ(1); s.noVs = true;
        s.setup = [=](Case& c) { c.imm = (int)(rnd() & 0xFFFF); c.op = (55u << 26) | (6u << 23) | ((uint32_t)(c.vt = c.vd) << 16 & 0x7F0000u) | (uint32_t)c.imm; c.op = (55u << 26) | (6u << 23) | ((uint32_t)(c.vd & 0x7F) << 16) | (uint32_t)c.imm; };
        s.ours = [=](Ctx x, Mem, const Case& c) { vfpu_viim(x, (uint8_t)c.vd, (uint16_t)c.imm); };
        v.push_back(s);
        OpSpec f = s;
        f.name = "vfim";
        f.setup = [=](Case& c) { c.imm = (int)(rnd() & 0xFFFF); c.op = (55u << 26) | (7u << 23) | ((uint32_t)(c.vd & 0x7F) << 16) | (uint32_t)c.imm; };
        f.ours = [=](Ctx x, Mem, const Case& c) { vfpu_vfim(x, (uint8_t)c.vd, (uint16_t)c.imm); };
        v.push_back(f);
    }
    {
        OpSpec s;
        s.name = "vpfx"; s.refFn = "Int_VPFX"; s.sizeMask = SZ(1); s.noPrefixTest = true; s.keepsPrefix = false;
        s.setup = [=](Case& c) {
            int which = (int)rndn(3);
            uint32_t data = rnd() & 0xFFFFF;
            c.imm = which;
            c.op = (55u << 26) | ((uint32_t)which << 24) | data;
            c.vd = (int)data;
        };
        s.ours = [=](Ctx x, Mem, const Case& c) { vfpu_set_prefix(x, c.imm, c.op & 0xFFFFFu); };
        // vpfxd keeps only 12 bits on hardware
        s.ours = [=](Ctx x, Mem, const Case& c) { uint32_t d = c.op & 0xFFFFFu; if (c.imm == 2) d &= 0xFFFu; vfpu_set_prefix(x, c.imm, d); };
        v.push_back(s);
    }

    // ---- memory ops: vt in lv/sv encodings; address from rs + offset, 16-byte aligned for .q
    auto memop = [&](const char* name, const char* fn, uint32_t major, bool quad, uint32_t lowbits, bool unaligned,
                     void (*call)(Ctx, Mem, uint8_t, uint8_t, int16_t)) {
        OpSpec s;
        s.name = name; s.refFn = fn; s.sizeMask = SZ(quad ? 4 : 1); s.keepsPrefix = true; s.noPrefixTest = false;
        s.matrix = false;
        s.setup = [=](Case& c) {
            int off = quad ? ((int)rndn(64) - 32) * 16 : ((int)rndn(64) - 32) * 4;
            if (unaligned) off = ((int)rndn(64) - 32) * 4;   // lvl/lvr/svl/svr take any word offset
            c.imm = off;
            uint32_t vt = (uint32_t)(quad ? (c.vd & 0x7F) : (c.vd & 0x7F));
            c.vd = (int)vt;
            c.op = (major << 26) | ((uint32_t)c.rs << 21) | ((vt & 0x1F) << 16) | ((uint32_t)off & 0xFFFC) | (quad ? ((vt >> 5) & 1) : ((vt >> 5) & 3)) | lowbits;
            if (!quad) c.op = (major << 26) | ((uint32_t)c.rs << 21) | ((vt & 0x1F) << 16) | ((uint32_t)off & 0xFFFC) | ((vt >> 5) & 3);
        };
        s.ours = [=](Ctx x, Mem m, const Case& c) { call(x, m, (uint8_t)c.vd, (uint8_t)c.rs, (int16_t)c.imm); };
        v.push_back(s);
    };
    memop("lv.s", "Int_SV", 50, false, 0, false, vfpu_lv_s);
    memop("sv.s", "Int_SV", 58, false, 0, false, vfpu_sv_s);
    memop("lv.q", "Int_SVQ", 54, true, 0, false, vfpu_lv_q);
    memop("sv.q", "Int_SVQ", 62, true, 0, false, vfpu_sv_q);
    memop("lvl.q", "Int_SVQ", 53, true, 0, true, vfpu_lvl_q);
    memop("lvr.q", "Int_SVQ", 53, true, 2, true, vfpu_lvr_q);
    memop("svl.q", "Int_SVQ", 61, true, 0, true, vfpu_svl_q);
    memop("svr.q", "Int_SVQ", 61, true, 2, true, vfpu_svr_q);

    // ---- control-register moves
    {
        OpSpec s;
        s.name = "mfv"; s.refFn = "Int_Mftv"; s.sizeMask = SZ(1); s.keepsPrefix = true; s.noPrefixTest = false;
        s.setup = [=](Case& c) { c.imm = (int)rndn(128); c.op = (18u << 26) | (3u << 21) | ((uint32_t)c.rs << 16) | (uint32_t)c.imm; };
        s.ours = [=](Ctx x, Mem, const Case& c) { vfpu_mfv(x, c.rs, (uint8_t)c.imm); };
        v.push_back(s);
        OpSpec t = s;
        t.name = "mtv";
        t.setup = [=](Case& c) { c.imm = (int)rndn(128); c.op = (18u << 26) | (7u << 21) | ((uint32_t)c.rs << 16) | (uint32_t)c.imm; };
        t.ours = [=](Ctx x, Mem, const Case& c) { vfpu_mtv(x, c.rs, (uint8_t)c.imm); };
        v.push_back(t);
        OpSpec fc = s;
        fc.name = "mfvc";
        fc.setup = [=](Case& c) { c.imm = (int)rndn(16); c.op = (18u << 26) | (3u << 21) | ((uint32_t)c.rs << 16) | (uint32_t)(128 + c.imm); };
        fc.ours = [=](Ctx x, Mem, const Case& c) { vfpu_mfvc(x, c.rs, c.imm); };
        v.push_back(fc);
        OpSpec tc = s;
        tc.name = "mtvc";
        tc.setup = [=](Case& c) { c.imm = (int)rndn(16); c.op = (18u << 26) | (7u << 21) | ((uint32_t)c.rs << 16) | (uint32_t)(128 + c.imm); };
        tc.ours = [=](Ctx x, Mem, const Case& c) { vfpu_mtvc(x, c.rs, c.imm); };
        v.push_back(tc);
    }
    return v;
}

int main(int argc, char** argv) {
    long iters = 20000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atol(argv[++i]);
        else if (!strcmp(argv[i], "--op") && i + 1 < argc) g_only = argv[++i];
        else if (!strcmp(argv[i], "--strict-nan")) g_strict_nan = true;
        else if (!strcmp(argv[i], "--wild-fails")) g_wild_fails = true;
        else if (!strcmp(argv[i], "--print") && i + 1 < argc) g_maxprint = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) g_rng = strtoull(argv[++i], nullptr, 0) | 1;
    }
    ref_init();
    ref_set_accurate_dot(std::getenv("VFPU_REF_FAST_DOT") ? 0 : 1);   // runtime default is the bit-exact dot
    std::vector<OpSpec> specs = build_specs();
    for (const OpSpec& s : specs) {
        if (g_only && strcmp(g_only, s.name)) continue;
        int fn = ref_fn_find(s.refFn);
        if (fn < 0) { std::printf("no ref fn %s\n", s.refFn); return 2; }
        run_op(s, fn, iters);
    }
    std::printf("\n%-8s %10s %8s %8s\n", "op", "runs", "bad", "nanbits");
    long nrun = 0;
    for (const OpSpec& s : specs) {
        if (g_only && strcmp(g_only, s.name)) continue;
        Stats& st = g_stats[s.name];
        nrun += st.runs;
        if (st.bad || st.nanbits) {
            std::printf("%-8s %10ld %8ld %8ld", s.name, st.runs, st.bad, st.nanbits);
            for (int r = 0; r < 3; r++)
                for (int f = 0; f < 2; f++) {
                    std::string key = std::string(s.name) + "/" + kRegimeName[r] + (f ? "/fin" : "/any");
                    auto it = g_stats.find(key);
                    if (it != g_stats.end()) std::printf("  %s/%s:%ld", kRegimeName[r], f ? "fin" : "any", it->second.bad);
                }
            std::printf("\n");
        }
    }
    std::printf("\n%ld total runs, %ld failing (wild-prefix quirks %s)\n", nrun, g_total_bad,
                g_wild_fails ? "counted" : "informational");
    return g_total_bad ? 1 : 0;
}
