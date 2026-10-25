// games/dissidia/runtime/hooks_models.cpp — raise the battle model staging budget (PSPRECOMP_MODEL_BUDGET).
//
// The game loads every model file (p_*.gmo/.objx, the _a/_w animation files, ...) into a temporary
// "staging" block, builds the real model data from it and frees it. Findings (docs/roster.md, "Model size budget"):
//   * overlay_battle FUN_08B45310 registers the model slots (ids 0x54E, 0x134, 0x408, 0x548) with
//     FUN_0884417C(mgr, id, heap, size, 0), size = 0x264000 (lui 0x26 / addiu 0x4000);
//     FUN_08843014 allocates `size` bytes from that heap (top-down, align 0x40), reads the file there
//     (sceIoRead with the index size) and frees it again.
//   * the heap is the "FILE" heap: a 0x275800 block (staging + 0x11800) taken from the main BSS heap
//     (12.9 MB, nearly full) by FUN_0888204C and turned into a heap by FUN_0885C40C.
// A file larger than the 0x264000 staging block (2,506,752 B; the largest retail character GMO is
// 2,280,840 B) makes the allocation fail (null buffer) or, for a slightly smaller file, overrun the heap.
//
// Fix without touching the recompiled code, when the budget is raised: the 0x275800 allocation
// (FUN_0888204C) is served from the host heap with size 0x275800 + (budget - 0x264000) + slack,
// FUN_0885C40C gets the real size, FUN_0884417C swaps the 0x264000 slot size for the budget, and the
// block free (FUN_0888208C) keeps the host block for the next battle. Nothing changes while the budget
// is the retail value.
//
// The same budget raise also needs a bigger MODEL POOL (found 17 Oct, "both fighters invisible" regression):
//   * FUN_0888204C(0x780000) = 7,864,320 B is the heap that holds the built data of every fighter in the
//     battle (heap object 0x08C15F80, `used` at heap+8). It is sized for retail models; a replacement model
//     whose GMO is much bigger than retail (Noctis: 3.7 MB vs 2.1 MB) fills it as soon as the opponent has a
//     large model too (Vaan; Lightning fits), FUN_0885C6CC then returns null for the remaining allocations
//     (`[MODELS] heap ... FAILED`), the fighters' model objects are never built and nothing is drawn (no
//     skinned PRIMs, no GE_XFER into RAM); the Hellfire summon then shows Ifrit's model in place of both.
//   * It is NOT an overlap with the effect allocator (sceKernelAllocMemoryBlock "EFFECT_ALLOCATOR",
//     0x0A010000-0x0A3ED000): a canary-filled 4 MB gap between the effect block and the model heap stayed
//     untouched and the bug was unchanged; retail never writes behind the block either.
// With a raised budget the pool request is served from the host heap with kPoolExtra more bytes.
//
// Host heap layout (game.toml [runtime]): region 1 0x0A000000-0x0B800000 (24 MiB, stops where LIBSUPPREACC is
// loaded), region 2 0x0B810000-<host_heap2_end> (above that PRX, below the guest thread stacks). The pool and the
// model file heap are reserved TOGETHER, once, when the pool is requested at game start (the budget is known from
// the prescan of <mods>/obj/), after a fit check against the heap's real free space (including the effect allocator,
// which is requested later): a budget that cannot fit is
// lowered 1 MiB at a time (never below retail), so a too-big mod degrades to a warning instead of a failed
// allocation. All blocks come from the same bump allocator, so the effect allocator, the pool and the file heap
// cannot overlap; psp_kmem_log_layout() prints them ([MEM] lines) and flags any overlap.
#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "recomp.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

std::string dissidia_mods_dir();
uint32_t dissidia_model_budget_request(uint32_t bytes);

namespace {

constexpr uint32_t kRetailStaging = 0x264000u;
constexpr uint32_t kRetailFile = 0x275800u;  // the "FILE" heap holding the staging block: 0x264000 + 0x11800
constexpr uint32_t kMaxStaging = 0x1200000u;   // ceiling: 18 MiB, i.e. GMOs up to 16 MiB plus headroom. What really fits is decided by the fit check against the host heap (see the header comment); the heap is 24 MiB + region 2
constexpr uint32_t kRetailMaxGmo = 2280840u;   // largest retail character GMO (p_thr200_1p.gmo)
constexpr uint32_t kLaterReserve = 4050944u + 0x100000u;   // still to come from the host heap after the pool request: the EFFECT_ALLOCATOR block (4,050,944 B, seen in the boot log) + 1 MiB margin
constexpr uint32_t kPoolValidatedStaging = 0x440000u;   // the pool extra below is sized for models up to ~4.25 MB (Noctis 4.21 MB, 22 Oct); larger files add their growth 1:1
constexpr uint32_t kWmSlack = 0x20000u;       // headroom for the heap bookkeeping and the objects that share the heap
constexpr uint32_t kRetailPool = 0x780000u;   // battle model pool (FUN_0888204C request), see the header comment
// extra bytes for the pool when the budget is raised (PSPRECOMP_MODEL_POOL_EXTRA). 0x80000 ran out on 19 Oct with the
// 3.65 MB Noctis in a battle (pool full ~110 KB short: alloc(74208)/(29736)/(16448) FAILED, Noctis invisible), so 1.5 MiB;
// raised to 2.5 MiB on 22 Oct for the 4.21 MB Noctis (EX Burst + glyph), so no env override is needed by default.
uint32_t g_pool_extra = 0x280000u;
bool g_pool_extra_forced = false;             // PSPRECOMP_MODEL_POOL_EXTRA set: no scaling
bool g_reserved = false;                      // pool + file heap reserved together (budget locked)
uint32_t g_staging = kRetailStaging;
uint32_t g_pool_block = 0, g_pool_size = 0;
bool g_pool_in_use = false;
uint32_t g_wm_block = 0, g_wm_size = 0;      // host-heap block, allocated once and reused by every scene
bool g_wm_in_use = false;
FuncPtr g_orig_alloc = nullptr, g_orig_register = nullptr, g_orig_free = nullptr, g_orig_heap_init = nullptr;

// FUN_0888204C(size): block from the main BSS heap. The model file heap (always exactly 0x270800) comes
// from the host heap instead when the budget is raised.
const bool g_trace = std::getenv("PSPRECOMP_MODELS_TRACE") != nullptr;

// The model pool (FUN_0888204C(0x780000)) is allocated at game start, long before the package index is opened
// (hooks_io.cpp ensure_built raises the staging budget from the override sizes). Scan <mods>/obj/ for the
// biggest replacement file here so the pool can be sized for it.
uint32_t pool_extra_for(uint32_t staging) {
    if (g_pool_extra_forced) return g_pool_extra;
    uint32_t e = g_pool_extra;
    if (staging > kPoolValidatedStaging) e += ((staging - kPoolValidatedStaging) + 0xFFFu) & ~0xFFFu;   // all of the file growth (the 1/4 estimate under-sized it)
    // Both fighters can be the biggest modded character (a mirror match). One extra only covered one of them:
    // on 22 Oct Noctis vs Noctis ran the pool out by ~0.75 MB (his 512² body texture and the rest FAILED), so the
    // second Noctis drew untextured (a white, glowing silhouette). Size for two.
    return e * 2u;
}
uint32_t wm_total_for(uint32_t staging) { return kRetailFile + (staging - kRetailStaging) + kWmSlack; }
uint32_t rd256(uint32_t v) { return (v + 0xFFu) & ~0xFFu; }

// Reserve the pool and the model file heap together for the current budget, lowering the budget until both fit.
void reserve_blocks() {
    uint32_t s = g_staging;
    for (;; s -= 0x100000u) {
        const uint32_t sizes[3] = { rd256(kRetailPool + pool_extra_for(s)), rd256(wm_total_for(s)), kLaterReserve };
        if (psp_kmem_plan_fits(sizes, 3)) break;
        if (s <= kRetailStaging + 0x100000u) { s = kRetailStaging; break; }
    }
    if (s != g_staging)
        std::fprintf(stderr, "[MODELS] host heap too small for a %u B staging budget (%u B free): lowered to %u B\n",
                     g_staging, psp_kmem_free_total(), s);
    g_staging = s;
    if (g_staging == kRetailStaging) { std::fprintf(stderr, "[MODELS] staying at the retail budget\n"); return; }
    g_pool_size = kRetailPool + pool_extra_for(g_staging);
    g_pool_block = psp_kmem_bump_alloc_named(rd256(g_pool_size), "MODEL_POOL");
    g_wm_size = wm_total_for(g_staging);
    g_wm_block = psp_kmem_bump_alloc_named(rd256(g_wm_size), "MODEL_FILE_HEAP");
    if (g_pool_block == 0 || g_wm_block == 0) {   // cannot happen after a successful plan; stay safe anyway
        std::fprintf(stderr, "[MODELS] reservation failed after a successful fit check: staying at the retail budget\n");
        g_pool_block = g_wm_block = g_pool_size = g_wm_size = 0;
        g_staging = kRetailStaging;
        return;
    }
    g_reserved = true;
    std::fprintf(stderr, "[MODELS] model pool %u B at 0x%08X (retail %u B); model file heap %u B at 0x%08X (retail 0x%X), staging budget %u B\n",
                 g_pool_size, g_pool_block, kRetailPool, g_wm_size, g_wm_block, kRetailFile, g_staging);
    psp_kmem_log_layout("model budget");
}

void prescan_mods_for_budget() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string dir = dissidia_mods_dir();
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    uint64_t biggest = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        for (char& c : rel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (rel.compare(0, 4, "obj/") == 0) biggest = std::max<uint64_t>(biggest, it->file_size(ec));
    }
    if (biggest > g_staging) {
        const uint32_t b = dissidia_model_budget_request(static_cast<uint32_t>(std::min<uint64_t>(biggest, 0xFFFFFFFFull)));
        std::fprintf(stderr, "[MODELS] mods prescan: largest obj/ override %llu B, staging budget %u B\n", (unsigned long long)biggest, b);
    }
}

void alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_trace && static_cast<uint32_t>(ctx->r[4]) >= 0x100000u) std::fprintf(stderr, "[MODELS] alloc(%u) in_use=%d\n", (uint32_t)ctx->r[4], (int)g_wm_in_use);
    if (static_cast<uint32_t>(ctx->r[4]) == kRetailFile && g_staging != kRetailStaging && !g_wm_in_use) {
        uint32_t total = wm_total_for(g_staging);
        if (g_wm_block == 0) {   // budget raised after the pool was requested (no prescan hit): reserve the file heap now
            g_wm_block = psp_kmem_bump_alloc_named(rd256(total), "MODEL_FILE_HEAP");
            g_wm_size = total;
            if (g_wm_block == 0) {
                std::fprintf(stderr, "[MODELS] cannot reserve %u B for the model file heap: staying at the retail budget\n", total);
                g_staging = kRetailStaging;
            } else {
                std::fprintf(stderr, "[MODELS] model file heap %u B at 0x%08X (retail 0x%X), staging budget %u B\n",
                             total, g_wm_block, kRetailFile, g_staging);
            }
        }
        if (g_wm_block != 0) { g_wm_in_use = true; if (g_trace) std::fprintf(stderr, "[MODELS]   -> host block\n"); ctx->r[2] = static_cast<int32_t>(g_wm_block); return; }
    }
    if (static_cast<uint32_t>(ctx->r[4]) == kRetailPool && !g_pool_in_use) {
        if (g_pool_block == 0 && !g_reserved) {
            prescan_mods_for_budget();
            if (g_staging != kRetailStaging) reserve_blocks();
        }
        if (g_pool_block != 0) { g_pool_in_use = true; ctx->r[2] = static_cast<int32_t>(g_pool_block); return; }
    }
    const uint32_t req = static_cast<uint32_t>(ctx->r[4]);
    g_orig_alloc(rdram, ctx);
    if (g_trace && req >= 0x100000u) std::fprintf(stderr, "[MODELS]   -> 0x%08X\n", (uint32_t)ctx->r[2]);
}

// FUN_0885C40C(block, size, name, align): heap init over a block; give the host block its real size.
void heap_init_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_wm_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_wm_block && static_cast<uint32_t>(ctx->r[5]) == kRetailFile)
        ctx->r[5] = static_cast<int32_t>(g_wm_size);
    else if (g_pool_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_pool_block && static_cast<uint32_t>(ctx->r[5]) == kRetailPool)
        ctx->r[5] = static_cast<int32_t>(g_pool_size);
    g_orig_heap_init(rdram, ctx);
}

// FUN_0884417C(mgr, id, heap, size, 0): model slot registration; 0x264000 is the staging size.
void register_hook(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(ctx->r[7]) == kRetailStaging && g_staging != kRetailStaging)
        ctx->r[7] = static_cast<int32_t>(g_staging);
    g_orig_register(rdram, ctx);
}

// FUN_0888208C(block): free to the main heap.
void free_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_trace && static_cast<uint32_t>(ctx->r[4]) >= 0x08C00000u) std::fprintf(stderr, "[MODELS] free(0x%08X)\n", (uint32_t)ctx->r[4]);
    if (g_wm_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_wm_block) { g_wm_in_use = false; ctx->r[2] = 0; return; }
    if (g_pool_block != 0 && static_cast<uint32_t>(ctx->r[4]) == g_pool_block) { g_pool_in_use = false; ctx->r[2] = 0; return; }
    g_orig_free(rdram, ctx);
}

// FUN_0885C6CC(heap, size, align, ...): the game's generic heap allocator. Diagnostic: log every request that
// comes back null (the caller then reads / builds into guest address 0: an invisible model, not a crash), with
// the heap's end and used bytes (heap+4 / heap+8) so an exhausted pool is obvious.
FuncPtr g_orig_heap_alloc = nullptr;
void heap_alloc_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t heap = static_cast<uint32_t>(ctx->r[4]), size = static_cast<uint32_t>(ctx->r[5]);
    g_orig_heap_alloc(rdram, ctx);
    if (ctx->r[2] == 0 && size != 0) {
        static int n = 0;
        if (n++ < 100) {
            uint32_t used = 0, end = 0;
            if (heap >= 0x08000000u && heap < 0x0C000000u) {
                const uint32_t* h = reinterpret_cast<const uint32_t*>(rdram + (heap & 0x07FFFFFFu));
                end = h[1]; used = h[2];
            }
            std::fprintf(stderr, "[MODELS] heap 0x%08X alloc(%u) FAILED (heap end 0x%08X, used 0x%X)\n", heap, size, end, used);
        }
    }
}

// The effect allocator is the last big host-heap block requested at boot: print the final layout after it.
void partition_alloc_observer(const char* name, uint32_t, uint32_t) {
    if (name && std::strcmp(name, "EFFECT_ALLOCATOR") == 0) psp_kmem_log_layout("final, after the effect allocator");
}

}  // namespace

// hooks_io.cpp (largest model override) and PSPRECOMP_MODEL_BUDGET. Returns the budget in effect.
uint32_t dissidia_model_budget_request(uint32_t bytes) {
    if (bytes > g_staging) {
        uint32_t b = (bytes + 0xFFFu) & ~0xFFFu;
        b = b > kMaxStaging ? kMaxStaging : b;
        if (g_reserved) {   // the pool and file heap are already reserved for g_staging: it cannot grow any more
            if (b > g_staging) std::fprintf(stderr, "[MODELS] budget request %u B exceeds the reserved budget %u B (ignored)\n", b, g_staging);
        } else {
            g_staging = b;
        }
    }
    return g_staging;
}
uint32_t dissidia_model_budget() { return g_staging; }
constexpr uint32_t dissidia_model_budget_retail() { return kRetailStaging; }

void dissidia_models_install() {
    psp_kmem_set_partition_alloc_observer(partition_alloc_observer);
    if (const char* e = std::getenv("PSPRECOMP_MODEL_POOL_EXTRA")) { g_pool_extra = static_cast<uint32_t>(std::strtoul(e, nullptr, 0)); g_pool_extra_forced = true; }
    if (const char* e = std::getenv("PSPRECOMP_MODEL_BUDGET")) dissidia_model_budget_request(static_cast<uint32_t>(std::strtoul(e, nullptr, 0)));
    g_orig_alloc = psp_dispatch_probe_lookup(0x0888204Cu);
    g_orig_register = psp_dispatch_probe_lookup(0x0884417Cu);
    g_orig_free = psp_dispatch_probe_lookup(0x0888208Cu);
    g_orig_heap_init = psp_dispatch_probe_lookup(0x0885C40Cu);
    if (!g_orig_alloc || !g_orig_register || !g_orig_free || !g_orig_heap_init) {
        std::fprintf(stderr, "[MODELS] model budget hooks NOT installed (a target function is missing)\n");
        g_staging = kRetailStaging;
        return;
    }
    g_orig_heap_alloc = psp_dispatch_probe_lookup(0x0885C6CCu);
    if (g_orig_heap_alloc) psp_dispatch_register(0x0885C6CCu, heap_alloc_hook);
    psp_dispatch_register(0x0888204Cu, alloc_hook);
    psp_dispatch_register(0x0885C40Cu, heap_init_hook);
    psp_dispatch_register(0x0884417Cu, register_hook);
    psp_dispatch_register(0x0888208Cu, free_hook);
}
