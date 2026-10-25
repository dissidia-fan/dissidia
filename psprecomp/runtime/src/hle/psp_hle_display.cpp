#include <atomic>
#include <cstdlib>
#include "psp_prof.h"
#include "hle/psp_hle.h"
#include "psp_render_queue.h"
#include "psp_scheduler.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdio>
#include <thread>
#include <chrono>

// ---- State ----
static uint32_t g_fb_addr = 0;
static uint32_t g_fb_stride = 512;
static uint32_t g_fb_format = 3;  // 8888
static uint32_t g_vcount = 0;

// ---- HLE Functions ----

static void hle_sceDisplaySetFrameBuf(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t fb_addr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t stride = static_cast<uint32_t>(ctx->r[5]);
    uint32_t format = static_cast<uint32_t>(ctx->r[6]);
    uint32_t sync_mode = static_cast<uint32_t>(ctx->r[7]);

    (void)sync_mode;

    g_fb_addr = fb_addr;
    g_fb_stride = stride;
    g_fb_format = format;

    if (g_hle_trace_enabled)   // was every frame, always
        std::fprintf(stderr,
            "[HLE] sceDisplaySetFrameBuf(0x%08X, %u, %u)\n",
            fb_addr, stride, format);

    // Post a FramePresent render request to the render queue
    RenderRequest req;
    req.type = RenderRequestType::FramePresent;
    req.rdram = rdram;
    req.fb_addr = fb_addr;
    req.fb_stride = stride;
    req.fb_format = format;
    render_queue_post(req);

    ctx->r[2] = SCE_OK;
}

// Simulated 60 Hz vblank clock: boundaries are fixed points on a free-running
// timeline (epoch + k * period), as on hardware. A wait returns at the NEXT
// boundary, so time the game already spent rendering counts toward the
// vblank instead of being added to a fixed full-period sleep (which made every
// frame cost render_time + N * 16.7 ms and ran the game in slow motion).
static const std::chrono::steady_clock::time_point g_vblank_epoch =
    std::chrono::steady_clock::now();
static constexpr std::chrono::microseconds k_vblank_period(16667);

// PSPRECOMP_VCLOCK=1: virtual clock advancing exactly one period per vblank
// wait return; all time HLE (system time, vcount) derives from it so the game
// sees a steady 60 Hz even when rendering is slow.
std::atomic<uint64_t> g_vclock_us{0};
bool g_vclock_on = std::getenv("PSPRECOMP_VCLOCK") != nullptr;

static uint32_t vblank_count_now() {
    if (g_vclock_on) return static_cast<uint32_t>(g_vclock_us.load() / 16667);
    return static_cast<uint32_t>(
        (std::chrono::steady_clock::now() - g_vblank_epoch) / k_vblank_period);
}

static void hle_sceDisplayWaitVblankStart(
    uint8_t* rdram, recomp_context* ctx
) {
    (void)rdram;

    sched_yield_point();

    psp_prof::seq("vblank-begin");
    const auto now = std::chrono::steady_clock::now();
    const auto n = (now - g_vblank_epoch) / k_vblank_period;
    std::this_thread::sleep_until(g_vblank_epoch + (n + 1) * k_vblank_period);
    psp_prof::seq("vblank-end");
    if (g_vclock_on) g_vclock_us.fetch_add(16667);
    g_vcount = vblank_count_now();

    ctx->r[2] = SCE_OK;
}

static void hle_sceDisplayWaitVblankStartCB(
    uint8_t* rdram, recomp_context* ctx
) {
    hle_sceDisplayWaitVblankStart(rdram, ctx);
}

static void hle_sceDisplayWaitVblank(
    uint8_t* rdram, recomp_context* ctx
) {
    hle_sceDisplayWaitVblankStart(rdram, ctx);
}

static void hle_sceDisplayGetFrameBuf(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t fb_addr_ptr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t stride_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t format_ptr = static_cast<uint32_t>(ctx->r[6]);

    if (fb_addr_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, fb_addr_ptr, g_fb_addr);
    }
    if (stride_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, stride_ptr, g_fb_stride);
    }
    if (format_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, format_ptr, g_fb_format);
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceDisplayGetVcount(
    uint8_t* rdram, recomp_context* ctx
) {
    g_vcount = vblank_count_now();
    ctx->r[2] = static_cast<int32_t>(g_vcount);
    (void)rdram;
}

static void hle_sceDisplaySetMode(
    uint8_t* rdram, recomp_context* ctx
) {
    std::fprintf(stderr, "[HLE] sceDisplaySetMode(%d, %d, %d)\n",
        (int)ctx->r[4], (int)ctx->r[5], (int)ctx->r[6]);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceDisplayGetFramePerSec(
    uint8_t* rdram, recomp_context* ctx
) {
    // Return 59.94 as float in v0 (PSP returns float via register)
    // Actually PSP returns a float, but the ABI puts it in $f0.
    // For simplicity, store the float representation.
    union { float f; int32_t i; } u;
    u.f = 59.94f;
    ctx->r[2] = u.i;
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_display() {
    psp_hle_register("sceDisplaySetFrameBuf",
                      hle_sceDisplaySetFrameBuf);
    psp_hle_register("sceDisplayWaitVblankStart",
                      hle_sceDisplayWaitVblankStart);
    psp_hle_register("sceDisplayWaitVblankStartCB",
                      hle_sceDisplayWaitVblankStartCB);
    psp_hle_register("sceDisplayWaitVblank",
                      hle_sceDisplayWaitVblank);
    psp_hle_register("sceDisplayGetFrameBuf",
                      hle_sceDisplayGetFrameBuf);
    psp_hle_register("sceDisplayGetVcount",
                      hle_sceDisplayGetVcount);
    psp_hle_register("sceDisplaySetMode",
                      hle_sceDisplaySetMode);
    psp_hle_register("sceDisplayGetFramePerSec",
                      hle_sceDisplayGetFramePerSec);
}
