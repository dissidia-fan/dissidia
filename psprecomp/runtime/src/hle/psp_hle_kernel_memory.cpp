#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include "psp_memory.h"
#include "psp_scheduler.h"
#include "recomp.h"
#include "recomp_module.h"  // generated module facts (issue #47 Phase 2)

// Generated per-game choices (issues #46/#47 Phase 4): RECOMP_HEAP_OVERRIDE
// pins the heap base when a game's manifest sets [module] heap_base.
// Guarded so output dirs generated before Phase 4 still build.
#if __has_include("recomp_game_config.h")
#include "recomp_game_config.h"
#endif
#ifndef RECOMP_HEAP_OVERRIDE
#define RECOMP_HEAP_OVERRIDE 0x0U
#endif
// Host heap limits ([runtime] host_heap_end / host_heap2_base / host_heap2_end
// in game.toml). Defaults reproduce the historical single region ending at
// 0x0B000000 and no second region.
#ifndef RECOMP_HOST_HEAP_START
#define RECOMP_HOST_HEAP_START 0x0U
#endif
#ifndef RECOMP_HOST_HEAP_END
#define RECOMP_HOST_HEAP_END 0x0B000000U
#endif
#ifndef RECOMP_HOST_HEAP2_BASE
#define RECOMP_HOST_HEAP2_BASE 0x0U
#endif
#ifndef RECOMP_HOST_HEAP2_END
#define RECOMP_HOST_HEAP2_END 0x0U
#endif

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <pthread.h>


// ---- Heap Allocator ----
// Bump allocator for PSP user memory. Grows upward from the heap start.
// Heap-base policy (#47 P2, decision P11): the generated fact
// RECOMP_HEAP_BASE (analysis.json heap_base — first 64K-aligned address past
// the loaded image) aligned UP to 16 MB. Game-agnostic rule, no manifest pin;
// for Patapon the alignment lands exactly on the historical hardcoded heap
// base, keeping the fragile boot layout bit-identical (plan risk R4).
// Revisit (drop the alignment) only when a game actually needs the headroom
// below the next 16 MB boundary.
// A manifest [module] heap_base pin (RECOMP_HEAP_OVERRIDE, issue #46)
// bypasses the align policy when a game needs tighter packing.
// [runtime] host_heap_start (RECOMP_HOST_HEAP_START) takes precedence over both.
static constexpr uint32_t k_heap_start =
    (RECOMP_HOST_HEAP_START != 0x0U)
        ? RECOMP_HOST_HEAP_START
        : (RECOMP_HEAP_OVERRIDE != 0x0U)
              ? RECOMP_HEAP_OVERRIDE
              : ((RECOMP_HEAP_BASE + 0x00FFFFFFU) & ~0x00FFFFFFU);
static uint32_t g_heap_pos = k_heap_start;
// Cap dlmalloc at RECOMP_HOST_HEAP_END (default 0x0B000000) so it cannot grow
// into the guest thread-stack region (psp_alloc_stack carves DOWN from
// PSP_USER_MEM_END - 0x1000 = 0x0BFFF000). Historically this was
// PSP_BND_ARENA_BASE, but the BND arena has moved above PSP_USER_MEM_END (see
// psp_hle.h) — keep the default cap at the same address so heap layout is
// unchanged and stacks stay safe.
//
// A game can raise the cap and/or add a second region (both from game.toml
// [runtime], see crates/psp-cli/src/config.rs). Region 1 is
// [k_heap_start, g_heap_end); region 2 [g_heap2_base, g_heap2_end) is used by
// kmem_alloc only when a request does not fit what is left of region 1 (so a
// module loaded inside region 1's former range, e.g. Dissidia's LIBSUPPREACC at
// 0x0B800000, can be skipped over). Every address stays below
// PSP_USER_MEM_END - 0x1000, i.e. inside the 0x07FFFFFF mask and clear of the
// stack floor.
static constexpr uint32_t g_heap_end = RECOMP_HOST_HEAP_END;
static constexpr uint32_t g_heap2_base = RECOMP_HOST_HEAP2_BASE;
static constexpr uint32_t g_heap2_end = RECOMP_HOST_HEAP2_END;
static uint32_t g_heap2_pos = g_heap2_base;
static_assert(g_heap_end <= PSP_USER_MEM_END - 0x1000U,
              "heap must stay below the guest thread-stack region");
static_assert(k_heap_start < g_heap_end,
              "16MB-aligned RECOMP_HEAP_BASE must leave room below the "
              "stack-region floor — image too large for the P11 alignment "
              "policy; revisit #47 P11");
static_assert((g_heap2_base == 0) == (g_heap2_end == 0),
              "host_heap2_base and host_heap2_end must be set together");
static_assert(g_heap2_end == 0 || (g_heap2_base >= g_heap_end &&
                                   g_heap2_base < g_heap2_end &&
                                   g_heap2_end <= PSP_USER_MEM_END - 0x1000U),
              "host heap region 2 must lie above region 1 and below the "
              "guest thread-stack region");
static_assert(((g_heap2_end ? g_heap2_end : g_heap_end) - 1) <= PSP_ADDR_MASK + PSP_RAM_BASE,
              "host heap must fit the 0x07FFFFFF address mask");

// Block book-keeping for the [MEM] layout log (name, address, size).
struct KmemBlock { std::string name; uint32_t addr; uint32_t size; };
static std::vector<KmemBlock> g_kmem_log;

// Bump-allocate `aligned` bytes: region 1 first, then region 2. 0 = out of memory.
static uint32_t kmem_alloc(uint32_t aligned, const char* name) {
    uint32_t addr = 0;
    if (aligned != 0 && g_heap_pos + aligned <= g_heap_end) {
        addr = g_heap_pos;
        g_heap_pos += aligned;
    } else if (aligned != 0 && g_heap2_end != 0 && g_heap2_pos + aligned <= g_heap2_end) {
        addr = g_heap2_pos;
        g_heap2_pos += aligned;
    }
    if (addr != 0) g_kmem_log.push_back({name ? name : "", addr, aligned});
    return addr;
}

static uint32_t kmem_free_r1() { return g_heap_pos < g_heap_end ? g_heap_end - g_heap_pos : 0; }
static uint32_t kmem_free_r2() { return g_heap2_pos < g_heap2_end ? g_heap2_end - g_heap2_pos : 0; }

static std::unordered_map<int, uint32_t> g_mem_blocks;   // uid -> addr
static std::unordered_map<int, uint32_t> g_mem_sizes;    // uid -> size

// Game-module partition-alloc observer slot (#47 P5 seam, installed via
// psp_kmem_set_partition_alloc_observer below).
static PspPartitionAllocObserver g_partition_alloc_observer = nullptr;

// ---- HLE Functions ----

static void hle_sceKernelAllocPartitionMemory(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t partition = ctx->r[4];
    uint32_t name_ptr = static_cast<uint32_t>(ctx->r[5]);
    int32_t type = ctx->r[6];
    uint32_t size = static_cast<uint32_t>(ctx->r[7]);

    (void)partition;
    (void)type;

    const char* name = "";
    if (name_ptr != 0) {
        name = reinterpret_cast<const char*>(
            rdram + (name_ptr & PSP_ADDR_MASK));
    }

    // Align to 256 bytes
    uint32_t aligned = (size + 0xFF) & ~0xFFU;

    uint32_t block_addr = kmem_alloc(aligned, name);
    if (block_addr == 0) {
        std::fprintf(stderr,
            "[HLE] sceKernelAllocPartitionMemory(\"%s\", %u) "
            "OUT OF MEMORY\n", name, size);
        ctx->r[2] = SCE_KERNEL_ERROR_NO_MEMORY;
        return;
    }

    int uid = psp_next_uid();
    g_mem_blocks[uid] = block_addr;
    g_mem_sizes[uid] = aligned;

    std::fprintf(stderr,
        "[HLE] sceKernelAllocPartitionMemory(\"%s\", %u) "
        "-> uid=%d addr=0x%08X\n",
        name, size, uid, block_addr);

    // Game-module seam (issue #47 Phase 5): the observer lets a game module
    // watch partition allocations (e.g. Patapon captures the UserSbrk block
    // as its dlmalloc-override arena) without a title branch in core.
    if (g_partition_alloc_observer != nullptr) {
        g_partition_alloc_observer(name, block_addr, aligned);
    }

    ctx->r[2] = uid;
}

static void hle_sceKernelFreePartitionMemory(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    g_mem_blocks.erase(uid);
    g_mem_sizes.erase(uid);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelGetBlockHeadAddr(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    auto it = g_mem_blocks.find(uid);
    if (it != g_mem_blocks.end()) {
        ctx->r[2] = static_cast<int32_t>(it->second);
    } else {
        std::fprintf(stderr,
            "[HLE] sceKernelGetBlockHeadAddr(uid=%d) NOT FOUND\n",
            uid);
        ctx->r[2] = 0;
    }
    (void)rdram;
}

// sceKernelGetMemoryBlockAddr(uid, void** ppAddr): unlike GetBlockHeadAddr it
// writes the address through a1 and returns 0 (the game's EFFECT_ALLOCATOR
// pool base came out as 0 before this, so no character effect pkg loaded).
static void hle_sceKernelGetMemoryBlockAddr(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t pp = static_cast<uint32_t>(ctx->r[5]);
    hle_sceKernelGetBlockHeadAddr(rdram, ctx);
    uint32_t addr = static_cast<uint32_t>(ctx->r[2]);
    if (pp && addr) {
        *reinterpret_cast<uint32_t*>(rdram + (pp & PSP_ADDR_MASK)) = addr;
        ctx->r[2] = 0;
    }
}

// sceKernelAllocMemoryBlock(name, type, size, params) / GetMemoryBlockAddr /
// FreeMemoryBlock: the 6.xx SDK block API (SysMemUserForUser NIDs 0xFE707FDF,
// 0xDB83A952, 0x50F61D8A — absent from the NID database, so the generated
// stub table carries the NID_0x fallback names). Same arena as partition
// memory. Dissidia 012 allocates its main buffers through this API.
static void hle_sceKernelAllocMemoryBlock(uint8_t* rdram, recomp_context* ctx) {
    uint32_t name_ptr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t size = static_cast<uint32_t>(ctx->r[6]);
    const char* name = name_ptr ? reinterpret_cast<const char*>(rdram + (name_ptr & PSP_ADDR_MASK)) : "";
    uint32_t aligned = (size + 0xFF) & ~0xFFU;
    uint32_t block_addr = kmem_alloc(aligned, name);
    if (block_addr == 0) {
        std::fprintf(stderr, "[HLE] sceKernelAllocMemoryBlock(\"%s\", %u) OUT OF MEMORY\n", name, size);
        ctx->r[2] = SCE_KERNEL_ERROR_NO_MEMORY;
        return;
    }
    int uid = psp_next_uid();
    g_mem_blocks[uid] = block_addr;
    g_mem_sizes[uid] = aligned;
    std::fprintf(stderr, "[HLE] sceKernelAllocMemoryBlock(\"%s\", type=%d, %u) -> uid=%d addr=0x%08X\n",
                 name, (int)ctx->r[5], size, uid, block_addr);
    if (g_partition_alloc_observer != nullptr) g_partition_alloc_observer(name, block_addr, aligned);
    ctx->r[2] = uid;
}

static void hle_sceKernelMaxFreeMemSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = static_cast<int32_t>(std::max(kmem_free_r1(), kmem_free_r2()));
    (void)rdram;
}

static void hle_sceKernelTotalFreeMemSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = static_cast<int32_t>(kmem_free_r1() + kmem_free_r2());
    (void)rdram;
}

static void hle_sceKernelDevkitVersion(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0x06060010;  // PSP FW 6.60
    (void)rdram;
}

static void hle_sceKernelPrintf(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t fmt_ptr = static_cast<uint32_t>(ctx->r[4]);
    if (fmt_ptr != 0) {
        const char* fmt = reinterpret_cast<const char*>(
            rdram + (fmt_ptr & PSP_ADDR_MASK));
        std::fprintf(stderr, "[GAME] %s", fmt);
    }
    ctx->r[2] = SCE_OK;
}

// Compiled SDK version (mirrors PPSSPP sceKernelMemory.cpp sdkVersion_):
// recorded by the sceKernelSetCompiledSdkVersion* family, read by
// sceKernelReferThreadStatus to pick the 104- vs 108-byte
// SceKernelThreadInfo protocol (gate: version > 0x02060010). 0 = never set.
static uint32_t g_compiled_sdk_version = 0;

uint32_t psp_kernel_compiled_sdk_version() {
    return g_compiled_sdk_version;
}

static void hle_sceKernelSetCompiledSdkVersion(
    uint8_t* rdram, recomp_context* ctx
) {
    g_compiled_sdk_version = static_cast<uint32_t>(ctx->r[4]);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelSetCompilerVersion(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Game-module seams (issue #47 Phase 5) ----
// The per-game module (games/<id>/runtime/, e.g. Patapon's allocator/CRT
// overrides) interacts with the bump heap ONLY through these functions —
// the heap statics stay private and the core carries no game knowledge.

void psp_kmem_set_partition_alloc_observer(PspPartitionAllocObserver fn) {
    g_partition_alloc_observer = fn;
}

uint32_t psp_kmem_bump_alloc(uint32_t size) {
    return kmem_alloc(size, "bump");
}

uint32_t psp_kmem_bump_alloc_named(uint32_t size, const char* name) {
    return kmem_alloc(size, name);
}

// Claims what is left of region 1 only (Patapon's dlmalloc-override arena);
// region 2, when configured, stays available to later allocations.
void psp_kmem_reserve_remaining(uint32_t* start, uint32_t* end) {
    if (start) *start = g_heap_pos;
    if (end) *end = g_heap_end;
    if (g_heap_pos < g_heap_end)
        g_kmem_log.push_back({"reserved-remaining", g_heap_pos, g_heap_end - g_heap_pos});
    g_heap_pos = g_heap_end;
}

bool psp_kmem_plan_fits(const uint32_t* sizes, int n) {
    uint32_t p1 = g_heap_pos, p2 = g_heap2_pos;
    for (int i = 0; i < n; i++) {
        const uint32_t a = (sizes[i] + 0xFFu) & ~0xFFu;
        if (p1 + a <= g_heap_end) p1 += a;
        else if (g_heap2_end != 0 && p2 + a <= g_heap2_end) p2 += a;
        else return false;
    }
    return true;
}

uint32_t psp_kmem_free_total() { return kmem_free_r1() + kmem_free_r2(); }
uint32_t psp_kmem_host_top() { return g_heap2_end != 0 ? g_heap2_end : g_heap_end; }

void psp_kmem_log_layout(const char* tag) {
    std::fprintf(stderr, "[MEM] host heap layout (%s):\n", tag ? tag : "");
    std::fprintf(stderr, "[MEM]   region 1 0x%08X-0x%08X (%u KiB), used %u KiB, free %u KiB\n",
                 k_heap_start, g_heap_end, (g_heap_end - k_heap_start) >> 10,
                 (std::min(g_heap_pos, g_heap_end) - k_heap_start) >> 10, kmem_free_r1() >> 10);
    if (g_heap2_end != 0)
        std::fprintf(stderr, "[MEM]   region 2 0x%08X-0x%08X (%u KiB), used %u KiB, free %u KiB\n",
                     g_heap2_base, g_heap2_end, (g_heap2_end - g_heap2_base) >> 10,
                     (g_heap2_pos - g_heap2_base) >> 10, kmem_free_r2() >> 10);
    for (const auto& b : g_kmem_log)
        std::fprintf(stderr, "[MEM]   0x%08X-0x%08X %8u KiB  %s\n", b.addr, b.addr + b.size, b.size >> 10, b.name.c_str());
    for (size_t i = 0; i < g_kmem_log.size(); i++)
        for (size_t j = i + 1; j < g_kmem_log.size(); j++)
            if (g_kmem_log[i].addr < g_kmem_log[j].addr + g_kmem_log[j].size &&
                g_kmem_log[j].addr < g_kmem_log[i].addr + g_kmem_log[i].size)
                std::fprintf(stderr, "[MEM] OVERLAP: %s and %s\n", g_kmem_log[i].name.c_str(), g_kmem_log[j].name.c_str());
    std::fprintf(stderr, "[MEM]   stacks grow down from 0x%08X; host heap top 0x%08X\n",
                 PSP_USER_MEM_END - 0x1000U, psp_kmem_host_top());
}

// ---- Registration ----

void psp_hle_register_kernel_memory() {
    if (g_heap2_end != 0 || g_heap_end != 0x0B000000U || RECOMP_HOST_HEAP_START != 0) psp_kmem_log_layout("boot");
    psp_hle_register("sceKernelAllocPartitionMemory",
                      hle_sceKernelAllocPartitionMemory);
    psp_hle_register("sceKernelFreePartitionMemory",
                      hle_sceKernelFreePartitionMemory);
    psp_hle_register("sceKernelGetBlockHeadAddr",
                      hle_sceKernelGetBlockHeadAddr);
    psp_hle_register("sceKernelAllocMemoryBlock", hle_sceKernelAllocMemoryBlock);
    psp_hle_register("NID_0xFE707FDF", hle_sceKernelAllocMemoryBlock);
    psp_hle_register("sceKernelGetMemoryBlockAddr", hle_sceKernelGetMemoryBlockAddr);
    psp_hle_register("NID_0xDB83A952", hle_sceKernelGetMemoryBlockAddr);
    psp_hle_register("sceKernelFreeMemoryBlock", hle_sceKernelFreePartitionMemory);
    psp_hle_register("NID_0x50F61D8A", hle_sceKernelFreePartitionMemory);
    psp_hle_register("sceKernelMaxFreeMemSize",
                      hle_sceKernelMaxFreeMemSize);
    psp_hle_register("sceKernelTotalFreeMemSize",
                      hle_sceKernelTotalFreeMemSize);
    psp_hle_register("sceKernelDevkitVersion",
                      hle_sceKernelDevkitVersion);
    psp_hle_register("sceKernelPrintf",
                      hle_sceKernelPrintf);
    psp_hle_register("sceKernelSetCompiledSdkVersion370",
                      hle_sceKernelSetCompiledSdkVersion);
    psp_hle_register("sceKernelSetCompilerVersion",
                      hle_sceKernelSetCompilerVersion);
}
