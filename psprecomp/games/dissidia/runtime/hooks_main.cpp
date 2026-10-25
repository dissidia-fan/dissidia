#include <cmath>
// games/dissidia/runtime/hooks_main.cpp — Dissidia 012 Duodecim game module.
//
// Overlays. The game keeps one of three flat code+data overlays resident at
// OVL_BASE (0x089DD4C0): OVL_BATTLE_APP (id 1), OVL_MENU_APP (id 2),
// OVL_EXTRA_APP (id 3). It reads the blob into guest memory with sceIoRead,
// calls sceKernelIcacheInvalidateRange, and then the EBOOT jumps straight
// into the window (852 direct call sites) — the whole game was linked as one
// program. Each overlay is recompiled as its own prefixed function set
// (games/dissidia/tools/build_overlay.py) with its own lookup table; the
// resolver below picks the table by the id word at the head of the window,
// which the game itself wrote there. No IO hook needed.

#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "recomp.h"   // psp_mem_read
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);
extern void ovl_battle_init_dispatch_table();
extern void ovl_menu_init_dispatch_table();
extern void ovl_extra_init_dispatch_table();

// LIBSUPPREACC.PRX (scesupPreAcc_library): a relocatable Square Enix support
// module shipped in USRDIR/DATA/MODULE and loaded by the game with
// sceKernelLoadModule. It is recompiled as its own set (build_overlay.py
// --module) rebased to SUPACC_BASE; we load its data sections, bind its own
// import stubs, and point the EBOOT's scesupPreAcc import stubs at its exports.
extern FuncPtr supacc_lookup(uint32_t vaddr);
extern void supacc_init_dispatch_table();
extern void supacc_init_data_sections(uint8_t* rdram);
extern "C" {
extern const RecompNidStub supacc_nid_stubs[];
extern const int supacc_nid_stub_count;
}

// hooks_io.cpp: PSPRECOMP_MODS loose-file overrides of PACKAGE.BIN entries.
extern void dissidia_install_mod_io();
extern void dissidia_index_install();
// savedata_theme.cpp: Load/Save dialog drawn from the game's own menu textures.
extern void dissidia_install_savedata_theme();
// hooks_models.cpp: battle model staging budget (PSPRECOMP_MODEL_BUDGET, mods larger than the retail files).
extern void dissidia_models_install();
// roster.cpp: extra roster slots (charId 34) by data patching (see docs/roster.md).
extern void dissidia_roster_init(uint8_t* rdram);
extern void dissidia_roster_boot_patch(uint8_t* rdram);
extern void dissidia_roster_grid_install();
extern void dissidia_wpnp_install();
// camera.cpp: analogue right-stick battle camera.
extern void dissidia_camera_install(uint8_t* rdram);
// input.cpp: continuous analogue stick direction (the game's own processing is 8-way).
extern void dissidia_input_install(uint8_t* rdram);

namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t OVL_END  = 0x08B63170u;
constexpr uint32_t SUPACC_BASE = 0x0B800000u;   // --load-base used for analysis_supacc.json

// scesupPreAcc exports (NID -> offset from module base, from .rodata.sceResident)
// and the EBOOT's import stub for each (from analysis.json imports[]).
struct SupaccBinding { uint32_t nid; uint32_t export_off; uint32_t eboot_stub; };
constexpr SupaccBinding k_supacc[] = {
    { 0x110E318Bu, 0x60Cu, 0x089C5D10u },
    { 0x13AE25B3u, 0x804u, 0x089C5D18u },
    { 0x28C5F696u, 0xEE0u, 0x089C5D20u },
    { 0x2EC3F4D9u, 0x370u, 0x089C5D28u },
    { 0x348BA3E2u, 0x24Cu, 0x089C5D30u },
    { 0x86DEBD66u, 0x160u, 0x089C5D38u },
    { 0xA0EAF444u, 0x6B0u, 0x089C5D40u },
    { 0xB03FF882u, 0xA28u, 0x089C5D48u },
};

uint8_t* g_rdram = nullptr;
uint32_t g_last_id = 0;

constexpr uint32_t SUPACC_END = SUPACC_BASE + 0x10000u;

FuncPtr dissidia_overlay_resolver(uint32_t vaddr) {
    // LIBSUPPREACC's own internal indirect calls (function-pointer tables)
    // land here too: its set is not in the EBOOT dispatch map.
    if (vaddr >= SUPACC_BASE && vaddr < SUPACC_END) return supacc_lookup(vaddr);
    if (vaddr < OVL_BASE || vaddr >= OVL_END || !g_rdram) return nullptr;
    uint32_t id;
    std::memcpy(&id, g_rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (id != g_last_id) {
        std::fprintf(stderr, "[OVL] resident overlay id=%u (%s)\n", id,
                     id == 1 ? "battle" : id == 2 ? "menu" : id == 3 ? "extra" : "?");
        g_last_id = id;
    }
    FuncPtr fn = nullptr;
    switch (id) {
        case 1: fn = ovl_battle_lookup(vaddr); break;
        case 2: fn = ovl_menu_lookup(vaddr); break;
        case 3: fn = ovl_extra_lookup(vaddr); break;
        default: break;
    }
    static int logged = 0;
    if (logged < 40 || !fn) {
        if (logged < 40 || (logged < 400 && !fn)) {
            std::fprintf(stderr, "[OVL] resolve 0x%08X (id %u) -> %s\n", vaddr, id, fn ? "ok" : "MISSING");
            logged++;
        }
    }
    return fn;
}


// Story-intro movie wait (data/movie/LOP.pmf).  The talk-event script VM
// (FUN_088E150C) blocks a command until FUN_088EA530(movie_obj) reports that
// the movie clock (movie_obj+0x48, seconds as float, driven by the SQEXTEC
// movie player from the MPEG decode thread) passed 2.0.  The runtime has no
// sceMpeg decode, so the player never starts, the clock stays 0.0 and the
// script (and the white screen) waits forever.  Treat an idle movie object
// (state word == 0: not started / finished / failed) as "wait satisfied" so
// the script proceeds with the movie skipped.
void dissidia_movie_time_wait(uint8_t* rdram, recomp_context* ctx) {
    uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    uint32_t state = psp_mem_read<uint32_t>(rdram, obj);
    uint32_t bits = psp_mem_read<uint32_t>(rdram, obj + 0x48);
    float t; std::memcpy(&t, &bits, 4);
    bool done = (t > 2.0f) || state == 0;
    static bool logged = false;
    if (done && state == 0 && !logged) {
        std::fprintf(stderr, "[MOVIE] idle movie object 0x%08X: treating 0x088EA530 wait as satisfied (movie skipped)\n", obj);
        logged = true;
    }
    ctx->r[2] = done ? 1 : 0;
}

// Talk-event timeline.  Script waits (FUN_088E150C: "0 < entry[1] and
// scene+0x13A4 < entry[1]") are keyed to the movie clock at movie_obj+0x48
// (movie_obj = scene+0x135C), e.g. 190 s, 1250 s.  With the movie skipped the
// clock sits at 0.0 and the script stalls at its first timed wait.  Before each
// scene tick, if the movie object is idle (state 0), pin the clock far in the
// future so every timeline wait is satisfied.
FuncPtr g_orig_scene_tick = nullptr;
void dissidia_scene_tick(uint8_t* rdram, recomp_context* ctx) {
    uint32_t scene = static_cast<uint32_t>(ctx->r[4]);
    uint32_t movie = scene + 0x135Cu;
    if (psp_mem_read<uint32_t>(rdram, movie) == 0) {
        const float big = 1.0e9f;
        uint32_t bits; std::memcpy(&bits, &big, 4);
        psp_mem_write<uint32_t>(rdram, movie + 0x48, bits);
    }
    g_orig_scene_tick(rdram, ctx);
}

// Debug aid (PSPRECOMP_TRACE_DRAWARRAY=<n>): log the first n non-through calls
// of the engine's sceGuDrawArray equivalent (FUN_089A0520: r4=prim, r5=vtype,
// r6=count, r7=indices, r8=vertices) to see which pointers the engine passes.
FuncPtr g_orig_drawarray = nullptr;
int g_drawarray_left = 0;
int g_drawarray_skip = 0;
void dissidia_drawarray_trace(uint8_t* rdram, recomp_context* ctx) {
    uint32_t vtype = static_cast<uint32_t>(ctx->r[5]);
    if (g_drawarray_left > 0 && !(vtype & 0x800000u) && g_drawarray_skip-- <= 0) {
        g_drawarray_left--;
        std::fprintf(stderr, "[DRAWARRAY] prim=%u vtype=0x%06X count=%u idx=0x%08X verts=0x%08X ra=0x%08X\n",
            (unsigned)(ctx->r[4] & 7), vtype, (unsigned)ctx->r[6], (unsigned)ctx->r[7], (unsigned)ctx->r[8],
            (unsigned)ctx->r[31]);
    }
    g_orig_drawarray(rdram, ctx);
}

// Debug aid (PSPRECOMP_TRACE_CALLS=hexaddr,hexaddr,...): log (first 40 calls
// each) the entry registers a0..a5 and ra of up to 8 guest functions.
constexpr int kMaxTraceCalls = 8;
uint32_t g_tc_addr[kMaxTraceCalls];
FuncPtr g_tc_orig[kMaxTraceCalls];
int g_tc_count[kMaxTraceCalls];
template <int I>
void dissidia_trace_call(uint8_t* rdram, recomp_context* ctx) {
    static const bool vram_only = std::getenv("PSPRECOMP_TRACE_CALLS_VRAM") != nullptr;
    const uint32_t a0 = static_cast<uint32_t>(ctx->r[4]) & 0x0FFFFFFFu;
    const bool in_vram = a0 >= 0x04000000u && a0 < 0x04800000u;
    if ((!vram_only || in_vram) && g_tc_count[I]++ < 400) {
        std::fprintf(stderr, "[CALL 0x%08X #%d] a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X ra=%08X\n",
            g_tc_addr[I], g_tc_count[I], (unsigned)ctx->r[4], (unsigned)ctx->r[5], (unsigned)ctx->r[6],
            (unsigned)ctx->r[7], (unsigned)ctx->r[8], (unsigned)ctx->r[9], (unsigned)ctx->r[31]);
    }
    g_tc_orig[I](rdram, ctx);
}
using TraceFn = void (*)(uint8_t*, recomp_context*);
const TraceFn k_trace_fns[kMaxTraceCalls] = {
    dissidia_trace_call<0>, dissidia_trace_call<1>, dissidia_trace_call<2>, dissidia_trace_call<3>,
    dissidia_trace_call<4>, dissidia_trace_call<5>, dissidia_trace_call<6>, dissidia_trace_call<7>};


// Debug aid (PSPRECOMP_TRACE_POOL=1): log model-pool sizing (FUN_089AB938) and
// allocation (FUN_089ABA0C) for sub-pool kind 1 (macros + vertex buffers).
FuncPtr g_orig_pool_size = nullptr, g_orig_pool_alloc = nullptr, g_orig_pool_build = nullptr;
int g_pool_log = 0;
void dissidia_pool_size(uint8_t* rdram, recomp_context* ctx) {
    uint32_t pool = (uint32_t)ctx->r[4], kind = (uint32_t)ctx->r[5], al = (uint32_t)ctx->r[6], sz = (uint32_t)ctx->r[7];
    g_orig_pool_size(rdram, ctx);
    if (kind == 1 && g_pool_log < 300) { g_pool_log++;
        std::fprintf(stderr, "[POOLSZ] pool=%08X kind=%u al=%u size=%u (0x%X) ra=%08X\n", pool, kind, al, sz, sz, (unsigned)ctx->r[31]); }
}
void dissidia_pool_alloc(uint8_t* rdram, recomp_context* ctx) {
    uint32_t pool = (uint32_t)ctx->r[4], kind = (uint32_t)ctx->r[5], al = (uint32_t)ctx->r[6], sz = (uint32_t)ctx->r[7];
    uint32_t rem = 0;
    if (pool && kind == 1) { uint32_t cls = al <= 4 ? 3 : (al <= 16 ? 2 : (al < 65 ? 1 : 0));
        rem = psp_mem_read<uint32_t>(rdram, pool + (kind * 4 + cls) * 4 + 0xc); }
    g_orig_pool_alloc(rdram, ctx);
    if (kind == 1 && g_pool_log < 300) { g_pool_log++;
        std::fprintf(stderr, "[POOLAL] pool=%08X kind=%u al=%u size=%u rem_before=%u -> %08X\n", pool, kind, al, sz, rem, (unsigned)ctx->r[2]); }
}
void dissidia_pool_build(uint8_t* rdram, recomp_context* ctx) {
    uint32_t pool = (uint32_t)ctx->r[4];
    if (g_pool_log < 300) { g_pool_log++;
        uint32_t w[28]; for (int i = 0; i < 28; i++) w[i] = psp_mem_read<uint32_t>(rdram, pool + i * 4);
        std::fprintf(stderr, "[POOLBLD-pre] pool=%08X sizes:", pool);
        for (int i = 3; i < 19; i++) std::fprintf(stderr, " %X", w[i]);
        std::fprintf(stderr, " | flag089DAF0B=%u\n", psp_mem_read<uint8_t>(rdram, 0x089DAF0Bu)); }
    g_orig_pool_build(rdram, ctx);
    if (g_pool_log < 300) {
        std::fprintf(stderr, "[POOLBLD] ret=%u arenas:", (unsigned)ctx->r[2]);
        for (int i = 0; i < 3; i++) std::fprintf(stderr, " %08X", psp_mem_read<uint32_t>(rdram, pool + i * 4));
        std::fprintf(stderr, " ptrs:");
        for (int i = 19; i < 23; i++) std::fprintf(stderr, " %08X", psp_mem_read<uint32_t>(rdram, pool + i * 4));
        std::fprintf(stderr, "\n"); }
}


// Debug aid (PSPRECOMP_TRACE_BONE=1): dump inputs/outputs of FUN_089B2974 (skeleton matrix builder).
FuncPtr g_orig_bone = nullptr;
int g_bone_log = 0;
static void bone_dump(uint8_t* rdram, uint32_t base, const char* tag, int idx) {
    auto f = [&](uint32_t o){ uint32_t u = psp_mem_read<uint32_t>(rdram, base + o); float v; std::memcpy(&v, &u, 4); return v; };
    std::fprintf(stderr, "[BONE] %s b%d @%08X q=%g %g %g %g | s80=%g %g %g %g | s112=%g %g %g %g | M=%g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n", tag, idx, base,
        f(96),f(100),f(104),f(108), f(80),f(84),f(88),f(92), f(112),f(116),f(120),f(124),
        f(128),f(132),f(136),f(140), f(144),f(148),f(152),f(156), f(160),f(164),f(168),f(172), f(176),f(180),f(184),f(188));
}
void dissidia_bone_trace(uint8_t* rdram, recomp_context* ctx) {
    uint32_t node = (uint32_t)ctx->r[4];
    uint32_t b = node ? psp_mem_read<uint32_t>(rdram, node + 12) : 0;
    uint32_t n = node ? psp_mem_read<uint16_t>(rdram, node + 32) : 0;
    bool log = b && g_bone_log < 12;
    if (log) { g_bone_log++;
        std::fprintf(stderr, "[BONE] call node=%08X base=%08X nbones=%u flags=%08X\n", node, b, n, psp_mem_read<uint32_t>(rdram, node + 48));
        for (uint32_t i = 0; i < n && i < 3; i++) bone_dump(rdram, b + 256 * i, "pre ", i); }
    g_orig_bone(rdram, ctx);
    if (log) for (uint32_t i = 0; i < n && i < 3; i++) bone_dump(rdram, b + 256 * i, "post", i);
}

// Debug aid (PSPRECOMP_TRACE_ATAN=1): log FUN_089A457C (VFPU atan2) args/result.
FuncPtr g_orig_atan = nullptr; int g_atan_log = 0, g_atan_bad = 0;
void dissidia_atan_trace(uint8_t* rdram, recomp_context* ctx) {
    float a = ctx->f[12], b = ctx->f[13];
    g_orig_atan(rdram, ctx);
    float r = ctx->f[0];
    bool bad = !std::isfinite(r);
    static const int atan_max = std::atoi(std::getenv("PSPRECOMP_TRACE_ATAN") ? std::getenv("PSPRECOMP_TRACE_ATAN") : "20") > 1 ? std::atoi(std::getenv("PSPRECOMP_TRACE_ATAN")) : 20;
    if ((g_atan_log < atan_max) || (bad && g_atan_bad < 30)) { g_atan_log++; if (bad) g_atan_bad++;
        std::fprintf(stderr, "[ATAN] f12=%g f13=%g -> f0=%g%s ra=%08X\n", a, b, r, bad ? " BAD" : "", (unsigned)ctx->r[31]); }
}

// Menu-overlay completion callbacks that the recompiled overlay set does not contain (no function
// boundary was found for them: tiny leaf functions reached only through a callback pointer, so the
// call from FUN_088740DC ended in a LOOKUP_MISS and the main menu never accepted input).
// 0x089E0A88: scene->flags(+4) |= 4 and 0x089E0AA4: |= 2, where scene = *(*(0x089E75C4) + 0x140).
// Reimplemented here; the proper fix is a force_entries line in overlay_menu.toml.
template <uint32_t BIT>
void dissidia_menu_flag_cb(uint8_t* rdram, recomp_context* ctx) {
    uint32_t holder = psp_mem_read<uint32_t>(rdram, 0x089E75C4u);
    uint32_t obj = psp_mem_read<uint32_t>(rdram, holder + 0x140u);
    psp_mem_write<uint32_t>(rdram, obj + 4u, psp_mem_read<uint32_t>(rdram, obj + 4u) | BIT);
    (void)ctx;
}

void dissidia_register_hooks(uint8_t* rdram) {
    psp_dispatch_register(0x089E0A88u, dissidia_menu_flag_cb<4>);
    psp_dispatch_register(0x089E0AA4u, dissidia_menu_flag_cb<2>);
    dissidia_index_install();    // package-index buffer relocation (hooks_index.cpp); before the first build
    dissidia_install_mod_io();   // loose-file mods (hooks_io.cpp)
    dissidia_install_savedata_theme();   // savedata dialog theme (savedata_theme.cpp)
    dissidia_models_install();   // model staging budget (hooks_models.cpp)
    dissidia_roster_init(rdram); // extra roster slots (roster.cpp)
    dissidia_wpnp_install();     // battle.bin wpnp table bigger than its retail buffer (roster.cpp)
    if (std::getenv("PSPRECOMP_TRACE_ATAN")) { g_orig_atan = psp_dispatch_probe_lookup(0x089A457Cu); if (g_orig_atan) psp_dispatch_register(0x089A457Cu, dissidia_atan_trace); }
    if (std::getenv("PSPRECOMP_TRACE_BONE")) { g_orig_bone = psp_dispatch_probe_lookup(0x089B2974u); if (g_orig_bone) psp_dispatch_register(0x089B2974u, dissidia_bone_trace); }
    if (std::getenv("PSPRECOMP_TRACE_POOL")) {
        g_orig_pool_size = psp_dispatch_probe_lookup(0x089AB938u); g_orig_pool_alloc = psp_dispatch_probe_lookup(0x089ABA0Cu); g_orig_pool_build = psp_dispatch_probe_lookup(0x089ABD28u);
        if (g_orig_pool_size) psp_dispatch_register(0x089AB938u, dissidia_pool_size);
        if (g_orig_pool_alloc) psp_dispatch_register(0x089ABA0Cu, dissidia_pool_alloc);
        if (g_orig_pool_build) psp_dispatch_register(0x089ABD28u, dissidia_pool_build);
    }
    g_rdram = rdram;
    if (const char* e = std::getenv("PSPRECOMP_TRACE_CALLS")) {
        int n = 0;
        const char* p = e;
        while (*p && n < kMaxTraceCalls) {
            char* end = nullptr;
            uint32_t a = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p) break;
            g_tc_addr[n] = a;
            g_tc_orig[n] = psp_dispatch_probe_lookup(a);
            if (g_tc_orig[n]) { psp_dispatch_register(a, k_trace_fns[n]); n++; }
            p = (*end == ',') ? end + 1 : end;
            if (*end != ',') break;
        }
    }
    if (const char* e = std::getenv("PSPRECOMP_TRACE_DRAWARRAY")) {
        g_drawarray_left = std::atoi(e);
        if (const char* k = std::getenv("PSPRECOMP_TRACE_DRAWARRAY_SKIP")) g_drawarray_skip = std::atoi(k);
        g_orig_drawarray = psp_dispatch_probe_lookup(0x089A0520u);
        if (g_orig_drawarray) psp_dispatch_register(0x089A0520u, dissidia_drawarray_trace);
    }
    psp_dispatch_register(0x088EA530u, dissidia_movie_time_wait);
    g_orig_scene_tick = psp_dispatch_probe_lookup(0x088E0F14u);   // must precede register
    if (g_orig_scene_tick) psp_dispatch_register(0x088E0F14u, dissidia_scene_tick);
    ovl_battle_init_dispatch_table();
    ovl_menu_init_dispatch_table();
    ovl_extra_init_dispatch_table();
    psp_dispatch_set_overlay_resolver(dissidia_overlay_resolver);
    dissidia_roster_grid_install();   // wraps overlay functions: needs the overlay dispatch tables
    dissidia_camera_install(rdram);   // camera.cpp (wraps a battle-overlay function too)
    dissidia_input_install(rdram);    // input.cpp

    supacc_init_dispatch_table();
    supacc_init_data_sections(rdram);
    psp_hle_bind_stub_table(supacc_nid_stubs, supacc_nid_stub_count);
    int bound = 0;
    for (const auto& b : k_supacc) {
        FuncPtr fn = supacc_lookup(SUPACC_BASE + b.export_off);
        if (fn) { psp_dispatch_register(b.eboot_stub, fn); bound++; }
        else std::fprintf(stderr, "[SUPACC] no function at export 0x%08X (NID 0x%08X)\n", SUPACC_BASE + b.export_off, b.nid);
    }
    std::fprintf(stderr, "[SUPACC] LIBSUPPREACC loaded at 0x%08X; %d/8 exports bound to EBOOT stubs\n", SUPACC_BASE, bound);
    std::fprintf(stderr, "[OVL] overlay resolver installed (window 0x%08X..0x%08X)\n", OVL_BASE, OVL_END);
}

void dissidia_on_boot_context(uint8_t* rdram, recomp_context* /*ctx*/) {
    dissidia_roster_boot_patch(rdram);   // EBOOT data is initialised by now
}
void dissidia_on_thread_start(uint8_t* /*rdram*/, uint32_t /*k0_addr*/) {}

const PspGameModule g_module = {
    /* id              */ "dissidia",
    /* register_hooks  */ dissidia_register_hooks,
    /* on_boot_context */ dissidia_on_boot_context,
    /* on_thread_start */ dissidia_on_thread_start,
};

}  // namespace

const PspGameModule* psp_game_module() {
    return &g_module;
}
