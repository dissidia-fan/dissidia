// PSPRECOMP_FN_WATCH=<hexaddr>[,<hexaddr>...]: wrap the listed recompiled functions and log
// their inputs (a0-a3, t0-t3, stack args, float args, 4 words behind pointer args) on entry and
// their results (v0, v1, f0, same pointer words) on exit.  PSPRECOMP_FN_WATCH_MAX caps the number
// of logged calls per function (default 300).  PSPRECOMP_FN_WATCH_SKIP skips the first N calls.
#include "recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <unistd.h>

void psp_dispatch_register(uint32_t vaddr, FuncPtr fn);
extern uint8_t* g_dbg_rdram;
void psp_dump_func_ring(const char* tag);

bool g_force_log = false;
void psp_label_trace(recomp_context* c, uint32_t lbl) {
    if (!g_force_log) return;
    std::fprintf(stderr, "[LT] %08X r2=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r16=%08X r17=%08X r18=%08X r19=%08X r20=%08X r21=%08X f12=%g f13=%g f20=%g f22=%g f24=%g cc=%d\n",
        lbl, (uint32_t)c->r[2], (uint32_t)c->r[4], (uint32_t)c->r[5], (uint32_t)c->r[6], (uint32_t)c->r[7], (uint32_t)c->r[8],
        (uint32_t)c->r[16], (uint32_t)c->r[17], (uint32_t)c->r[18], (uint32_t)c->r[19], (uint32_t)c->r[20], (uint32_t)c->r[21],
        c->f[12], c->f[13], c->f[20], c->f[22], c->f[24], (int)c->fpu_cc);
}
namespace {
struct Slot { uint32_t addr = 0; FuncPtr orig = nullptr; int n = 0; };
Slot g_slots[16];
int g_max = 300, g_skip = 0;
thread_local int t_depth = 0;

bool ptr_ok(uint32_t a) {
    uint32_t hi = a & 0x0F000000u;
    return a >= 0x08000000u && (hi == 0x08000000u || hi == 0x09000000u || hi == 0x0A000000u || hi == 0x0B000000u);
}
void words(const char* tag, uint32_t a) {
    if (!ptr_ok(a) || !g_dbg_rdram) return;
    std::fprintf(stderr, "    %s[%08X]:", tag, a);
    for (int i = 0; i < 8; i++) {
        uint32_t w; std::memcpy(&w, g_dbg_rdram + ((a + i * 4) & 0x07FFFFFFu), 4);
        float f; std::memcpy(&f, &w, 4);
        std::fprintf(stderr, " %08X(%g)", w, f);
    }
    std::fprintf(stderr, "\n");
}
void dump_ptrs(recomp_context* c) {
    words("a0", (uint32_t)c->r[4]); words("a1", (uint32_t)c->r[5]);
    words("a2", (uint32_t)c->r[6]); words("a3", (uint32_t)c->r[7]);
    words("t0", (uint32_t)c->r[8]); words("t1", (uint32_t)c->r[9]); words("t2", (uint32_t)c->r[10]);
}


static void coll_one(FuncPtr fn, uint8_t* rdram, recomp_context* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                     float x, float y, float z, float dx, float dz);

static void coll_scan(FuncPtr fn, uint8_t* rdram, recomp_context* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a3;
    const uint32_t sp = (uint32_t)c->r[29];
    const uint32_t base = (sp - 8192u) & ~15u;
    const uint32_t A1 = base, A2 = base + 256, A3 = base + 512, T0 = base + 768;
    const uint32_t t0 = (uint32_t)c->r[8], t1 = (uint32_t)c->r[9], t2 = (uint32_t)c->r[10], t3 = (uint32_t)c->r[11];
    auto rd = [&](uint32_t a) { uint32_t w; std::memcpy(&w, rdram + (a & 0x07FFFFFFu), 4); return w; };
    auto wr = [&](uint32_t a, uint32_t w) { std::memcpy(rdram + (a & 0x07FFFFFFu), &w, 4); };
    auto wf = [&](uint32_t a, float f) { std::memcpy(rdram + (a & 0x07FFFFFFu), &f, 4); };
    // Original argument words (the calling function's stack arg slot is read from sp+16 by the callee)
    uint32_t sa1[32], sa2[32], st0[32];
    for (int i = 0; i < 32; i++) { sa1[i] = rd(a1 + i * 4); sa2[i] = rd(a2 + i * 4); st0[i] = rd(t0 + i * 4); }
    std::fprintf(stderr, "[COLL] scan start world=%08X sp=%08X radius=%g\n", a0, sp, *(float*)&sa2[3]);
    const float rad = *(float*)&sa2[3];
    static const float dirs[4][3] = {{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
    static const char* dn[4] = {"+x","-x","+z","-z"};
    const float levels[3] = {30.6f, 10.0f, -3.537f};
    for (int lv = 0; lv < 3; lv++)
    for (int d = 0; d < 4; d++) {
        std::fprintf(stderr, "[COLL] level y=%g dir=%s (rows z=-36..36, cols x=-36..36)\n", levels[lv], dn[d]);
        for (int zi = -36; zi <= 36; zi += 2) {
            char row[80]; int n = 0;
            for (int xi = -36; xi <= 36; xi += 2) {
                for (int i = 0; i < 32; i++) { wr(A1 + i * 4, sa1[i]); wr(A2 + i * 4, sa2[i]); wr(T0 + i * 4, st0[i]); }
                wf(A2 + 0, (float)xi); wf(A2 + 4, levels[lv]); wf(A2 + 8, (float)zi);
                wf(A2 + 16, (float)xi); wf(A2 + 20, levels[lv]); wf(A2 + 24, (float)zi);
                const float L = 0.3f;
                wf(A3 + 0, dirs[d][0] * L); wf(A3 + 4, 0.f); wf(A3 + 8, dirs[d][2] * L);
                wf(T0 + 12, dirs[d][0] * L); wf(T0 + 16, 0.f); wf(T0 + 20, dirs[d][2] * L);
                recomp_context cc = *c;
                cc.r[4] = (int32_t)a0; cc.r[5] = (int32_t)A1; cc.r[6] = (int32_t)A2; cc.r[7] = (int32_t)A3;
                cc.r[8] = (int32_t)T0; cc.r[9] = (int32_t)t1; cc.r[10] = (int32_t)t2; cc.r[11] = (int32_t)t3;
                (void)t0;
                cc.preempt_budget = 1 << 30; fn(rdram, &cc);
                row[n++] = cc.r[2] ? 'X' : '.';
            }
            row[n] = 0;
            std::fprintf(stderr, "[COLL] z=%3d %s\n", zi, row);
        }
    }
    (void)rad;
    std::fprintf(stderr, "[COLL] scan done\n");
}

static void coll_one(FuncPtr fn, uint8_t* rdram, recomp_context* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                     float x, float y, float z, float dx, float dz) {
    (void)a3;
    const uint32_t sp = (uint32_t)c->r[29];
    const uint32_t base = (sp - 8192u) & ~15u;
    const uint32_t A1 = base, A2 = base + 256, A3 = base + 512, T0 = base + 768;
    const uint32_t t0 = (uint32_t)c->r[8];
    auto rd = [&](uint32_t a) { uint32_t w; std::memcpy(&w, rdram + (a & 0x07FFFFFFu), 4); return w; };
    auto wr = [&](uint32_t a, uint32_t w) { std::memcpy(rdram + (a & 0x07FFFFFFu), &w, 4); };
    auto wf = [&](uint32_t a, float f) { std::memcpy(rdram + (a & 0x07FFFFFFu), &f, 4); };
    for (int i = 0; i < 32; i++) { wr(A1 + i * 4, rd(a1 + i * 4)); wr(A2 + i * 4, rd(a2 + i * 4)); wr(T0 + i * 4, rd(t0 + i * 4)); }
    wf(A2 + 0, x); wf(A2 + 4, y); wf(A2 + 8, z); wf(A2 + 16, x); wf(A2 + 20, y); wf(A2 + 24, z);
    wf(A3 + 0, dx); wf(A3 + 4, 0.f); wf(A3 + 8, dz);
    wf(T0 + 12, dx); wf(T0 + 16, 0.f); wf(T0 + 20, dz);
    recomp_context cc = *c;
    cc.r[4] = (int32_t)a0; cc.r[5] = (int32_t)A1; cc.r[6] = (int32_t)A2; cc.r[7] = (int32_t)A3; cc.r[8] = (int32_t)T0;
    cc.preempt_budget = 1 << 30;
    std::fprintf(stderr, "[COLL] single test at (%g,%g,%g) disp (%g,0,%g)\n", x, y, z, dx, dz);
    g_force_log = true;
    fn(rdram, &cc);
    g_force_log = false;
    std::fprintf(stderr, "[COLL] single result v0=%d\n", cc.r[2]);
}

template <int N>
void wrap(uint8_t* rdram, recomp_context* c) {
    Slot& s = g_slots[N];
    static const char* gate = std::getenv("PSPRECOMP_FN_WATCH_GATE");  // log only while this file exists
    bool on = g_force_log || !gate || access(gate, F_OK) == 0;
    int idx = on ? s.n++ : -1;
    bool log = on && idx >= g_skip && idx < g_skip + g_max;
    int d = t_depth;
    static const bool brief = std::getenv("PSPRECOMP_FN_WATCH_BRIEF") != nullptr;
    if (log && brief) {
        if (s.addr == 0x0889CF04u && g_dbg_rdram) {
            auto rf = [&](uint32_t a) { float f; std::memcpy(&f, g_dbg_rdram + (a & 0x07FFFFFFu), 4); return f; };
            uint32_t a2 = (uint32_t)c->r[6], a3 = (uint32_t)c->r[7];
            static const auto t0 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "[POS] t=%.0f pos=(%.2f,%.2f,%.2f) r=%.2f disp=(%.3f,%.3f,%.3f) a2=%08X\n", ms,
                rf(a2), rf(a2 + 4), rf(a2 + 8), rf(a2 + 12), rf(a3), rf(a3 + 4), rf(a3 + 8), a2);
        }
        log = false;
    }
    if (log) {
        uint32_t sp = (uint32_t)c->r[29];
        uint32_t sw[5] = {0};
        if (g_dbg_rdram && ptr_ok(sp)) std::memcpy(sw, g_dbg_rdram + (sp & 0x07FFFFFFu), sizeof sw);
        std::fprintf(stderr, "[FW] %*s>%08X #%d a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X t2=%08X t3=%08X sp=%08X [sp]=%08X %08X %08X %08X %08X f12=%g f13=%g f14=%g ra=%08X\n",
            d * 2, "", s.addr, idx, (uint32_t)c->r[4], (uint32_t)c->r[5], (uint32_t)c->r[6], (uint32_t)c->r[7],
            (uint32_t)c->r[8], (uint32_t)c->r[9], (uint32_t)c->r[10], (uint32_t)c->r[11], sp,
            sw[0], sw[1], sw[2], sw[3], sw[4], c->f[12], c->f[13], c->f[14], (uint32_t)c->r[31]);
        dump_ptrs(c);
        static const bool ring = std::getenv("PSPRECOMP_FN_WATCH_RING") != nullptr;   // + the 12 most recently entered functions (caller hint)
        if (ring) psp_dump_func_ring("FW-RING");
    }
    uint32_t sa0 = (uint32_t)c->r[4], sa1 = (uint32_t)c->r[5], sa2 = (uint32_t)c->r[6], sa3 = (uint32_t)c->r[7];
    t_depth++;
    s.orig(rdram, c);
    t_depth--;
    if (s.addr == 0x0889CF04u && t_depth == 0 && access("/tmp/claude-0/coll_test", F_OK) == 0) {
        float sx = 0, sy = 0, sz = 0, sdx = 0, sdz = 0; int single = 0;
        if (FILE* f = std::fopen("/tmp/claude-0/coll_test", "r")) {
            single = std::fscanf(f, "%f %f %f %f %f", &sx, &sy, &sz, &sdx, &sdz) == 5;
            std::fclose(f);
        }
        unlink("/tmp/claude-0/coll_test");
        if (single) { coll_one(s.orig, rdram, c, sa0, sa1, sa2, sa3, sx, sy, sz, sdx, sdz); }
        else coll_scan(s.orig, rdram, c, sa0, sa1, sa2, sa3);
    }
    if (brief && on && s.addr == 0x0889CF04u && g_dbg_rdram) {
        std::fprintf(stderr, "[POS]   -> hit=%d\n", c->r[2]);
    }
    if (log) {
        std::fprintf(stderr, "[FW] %*s<%08X #%d v0=%08X v1=%08X f0=%g\n", d * 2, "", s.addr, idx,
            (uint32_t)c->r[2], (uint32_t)c->r[3], c->f[0]);
        words("a0'", sa0); words("a1'", sa1); words("a2'", sa2); words("a3'", sa3);
    }
}
typedef void (*W)(uint8_t*, recomp_context*);
const W k_wrappers[16] = { wrap<0>, wrap<1>, wrap<2>, wrap<3>, wrap<4>, wrap<5>, wrap<6>, wrap<7>,
                           wrap<8>, wrap<9>, wrap<10>, wrap<11>, wrap<12>, wrap<13>, wrap<14>, wrap<15> };
}

void psp_fn_watch_install() {
    const char* e = std::getenv("PSPRECOMP_FN_WATCH");
    if (!e || !*e) return;
    if (const char* m = std::getenv("PSPRECOMP_FN_WATCH_MAX")) g_max = std::atoi(m);
    if (const char* m = std::getenv("PSPRECOMP_FN_WATCH_SKIP")) g_skip = std::atoi(m);
    std::string list(e);
    size_t i = 0; int n = 0;
    while (i < list.size() && n < 16) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        uint32_t a = (uint32_t)std::strtoul(list.substr(i, j - i).c_str(), nullptr, 16);
        if (a) {
            g_slots[n].addr = a;
            g_slots[n].orig = RECOMP_LOOKUP(a);
            psp_dispatch_register(a, k_wrappers[n]);
            std::fprintf(stderr, "[FW] watching %08X\n", a);
            n++;
        }
        i = j + 1;
    }
}
