#include "psp_settings.h"
#include <atomic>
#include <cstdlib>
#include "psp_prof.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
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

// Fast-forward (debug socket FF <ms> [x]): while it lasts, the vblank wait moves the
// game's clock on to the next boundary instead of sleeping until it, so the game
// runs as fast as the host renders. The game's clock is the host clock plus this
// skew (vcount here, system time in psp_hle_kernel_thread.cpp); the skew only
// grows, so time carries on from where fast-forward left it and never goes back.
// A movie keeps its own pace: its picture follows its sound, which plays in real
// time (the game just draws each movie frame more often).
std::atomic<int64_t> g_clock_skew_us{0};
static std::atomic<int64_t> g_fast_forward_until_ms{0};   // steady-clock ms
static std::atomic<int> g_fast_forward_factor{0};         // 0: as fast as frames render; x: x times real time

// ms <= 0: back to real time now. factor x > 1 caps the speed at x times real
// time: a screen with an idle timer (a title that starts its attract movie) then
// still gives a driver real seconds to press before it runs out.
void psp_display_fast_forward(int64_t ms, int factor) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    g_fast_forward_factor.store(factor > 1 ? factor : 0);
    g_fast_forward_until_ms.store(ms > 0 ? now_ms + ms : 0);
}

static uint32_t vblank_count_now() {
    if (g_vclock_on) return static_cast<uint32_t>(g_vclock_us.load() / 16667);
    const auto game_now = std::chrono::steady_clock::now() + std::chrono::microseconds(g_clock_skew_us.load());
    return static_cast<uint32_t>((game_now - g_vblank_epoch) / k_vblank_period);
}

static void hle_sceDisplayWaitVblankStart(
    uint8_t* rdram, recomp_context* ctx
) {
    (void)rdram;
    // When this thread's last wait returned, on the host clock and on the game's (FF's speed cap)
    static thread_local std::chrono::steady_clock::time_point t_last_host, t_last_game;

    sched_yield_point();
    psp_adhoc_pump(rdram, ctx);   // ad-hoc handler/callback events
    psp_settings_guest_wait();   // the settings menu pauses the game here

    psp_prof::seq("vblank-begin");
    const auto now = std::chrono::steady_clock::now();
    const auto skew = std::chrono::microseconds(g_clock_skew_us.load());
    const auto next = g_vblank_epoch + ((now + skew - g_vblank_epoch) / k_vblank_period + 1) * k_vblank_period;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    if (now_ms < g_fast_forward_until_ms.load(std::memory_order_relaxed)) {
        // Capped at x: the game time from this thread's last wait to the boundary takes 1/x of it on the host
        // clock (a frame that already took that long: no wait; nothing is caught up after a slow one). Then the
        // skew moves the clock the rest of the way, rounded up: a clock left a few ns short of the boundary would
        // not count this vblank, and a frame loop waiting for it would spin. PSPRECOMP_VCLOCK advances below
        // anyway: skipping the sleep is enough there.
        if (const int x = g_fast_forward_factor.load(std::memory_order_relaxed); x > 1)
            std::this_thread::sleep_until(t_last_host + (next - t_last_game) / x);
        const auto rest = next - (std::chrono::steady_clock::now() + std::chrono::microseconds(g_clock_skew_us.load()));
        if (!g_vclock_on && rest > rest.zero())
            g_clock_skew_us.fetch_add(std::chrono::ceil<std::chrono::microseconds>(rest).count());
    } else {
        std::this_thread::sleep_until(next - skew);
    }
    psp_prof::seq("vblank-end");
    if (g_vclock_on) g_vclock_us.fetch_add(16667);
    g_vcount = vblank_count_now();
    t_last_host = std::chrono::steady_clock::now();
    t_last_game = t_last_host + std::chrono::microseconds(g_clock_skew_us.load());

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
