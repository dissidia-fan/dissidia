#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include "hle/psp_hle_io.h"
#include "hle/psp_audio_sink.h"
#include "psp_memory.h"
#include "psp_runtime.h"
#include "psp_savedata_crypto.h"
#include "psp_savedata_dialog.h"
#include "psp_scheduler.h"
#include "psp_tex_pack.h"   // psp_png_decode_rgba (ICON0.PNG thumbnails)
#include "recomp.h"
#include "recomp_module.h"  // generated module facts (issue #47 Phase 2)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <strings.h>
#include <ctime>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// ================================================================
// Catch-all HLE stubs for remaining Patapon NIDs.
// Groups: cache, RTC, audio, ATRAC, module mgmt, UMD, network,
//         MPEG, SAS, HTTP, SSL, impose, PSMF, stdio, interrupt,
//         kernel_library, loadexec, sceUtility.
// ================================================================

// ---- Kernel Memory Bump Allocator ----
// Starts after BOOT_MODULE_ADDR + NATIVE_MODULE_SIZE, grows upward.
// Kernel memory range: 0x08000000 - 0x083FFFFF (4MB)
static uint32_t g_kernel_heap_pos =
    BOOT_MODULE_ADDR + NATIVE_MODULE_SIZE;
static constexpr uint32_t KERNEL_MEM_END = 0x08400000U;

uint32_t psp_alloc_kernel_memory(uint32_t size) {
    uint32_t aligned = (size + 0xFU) & ~0xFU;  // 16-byte align
    if (g_kernel_heap_pos + aligned > KERNEL_MEM_END) {
        return 0;  // OOM
    }
    uint32_t addr = g_kernel_heap_pos;
    g_kernel_heap_pos += aligned;
    return addr;
}

uint32_t psp_get_boot_module_gp() {
    return RECOMP_MODULE_GP;  // SceModuleInfo gp (generated fact, #47 P2)
}

// ---- Cache Operations (all no-ops) ----

static void hle_ret0(uint8_t*, recomp_context* ctx) { ctx->r[2] = 0; }

// sceSuspendForUser volatile memory: the 4 MB block at 0x08400000 that the
// OS lends to the game (sceKernelVolatileMemLock/TryLock(unk, &ptr, &size)).
static bool g_volatile_locked = false;
static void hle_sceKernelVolatileMemLock(uint8_t* rdram, recomp_context* ctx) {
    uint32_t pptr = (uint32_t)ctx->r[5], psize = (uint32_t)ctx->r[6];
    if (pptr) psp_mem_write<uint32_t>(rdram, pptr, 0x08400000u);
    if (psize) psp_mem_write<uint32_t>(rdram, psize, 0x00400000u);
    g_volatile_locked = true;
    ctx->r[2] = 0;
}
static void hle_sceKernelVolatileMemTryLock(uint8_t* rdram, recomp_context* ctx) {
    if (g_volatile_locked) { ctx->r[2] = (int32_t)0x800201B8; return; }  // SCE_KERNEL_ERROR_POWER_VMEM_IN_USE
    hle_sceKernelVolatileMemLock(rdram, ctx);
}
static void hle_sceKernelVolatileMemUnlock(uint8_t*, recomp_context* ctx) {
    g_volatile_locked = false;
    ctx->r[2] = 0;
}

static void hle_sceKernelDcacheWritebackAll(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelDcacheWritebackInvalidateAll(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelDcacheWritebackRange(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- RTC ----

static void hle_sceRtcGetAccumulativeTime(
    uint8_t* rdram, recomp_context* ctx
) {
    // Return current microsecond timestamp as 64-bit in v0:v1
    auto now = std::chrono::steady_clock::now();
    uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count());
    ctx->r[2] = static_cast<int32_t>(us & 0xFFFFFFFF);
    ctx->r[3] = static_cast<int32_t>((us >> 32) & 0xFFFFFFFF);
    (void)rdram;
}

static void hle_sceRtcGetCurrentClockLocalTime(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t time_ptr = static_cast<uint32_t>(ctx->r[4]);

    if (time_ptr != 0) {
        // ScePspDateTime: year(u16), month(u16), day(u16),
        //   hour(u16), minute(u16), second(u16), microsecond(u32)
        time_t now = std::time(nullptr);
        struct tm* local = std::localtime(&now);
        if (local) {
            psp_mem_write<uint16_t>(rdram, time_ptr,
                static_cast<uint16_t>(local->tm_year + 1900));
            psp_mem_write<uint16_t>(rdram, time_ptr + 2,
                static_cast<uint16_t>(local->tm_mon + 1));
            psp_mem_write<uint16_t>(rdram, time_ptr + 4,
                static_cast<uint16_t>(local->tm_mday));
            psp_mem_write<uint16_t>(rdram, time_ptr + 6,
                static_cast<uint16_t>(local->tm_hour));
            psp_mem_write<uint16_t>(rdram, time_ptr + 8,
                static_cast<uint16_t>(local->tm_min));
            psp_mem_write<uint16_t>(rdram, time_ptr + 10,
                static_cast<uint16_t>(local->tm_sec));
            psp_mem_write<uint32_t>(rdram, time_ptr + 12, 0);
        }
    }

    ctx->r[2] = SCE_OK;
}

// ---- sceRtc tick API ----
// Ticks are microseconds since 0001-01-01 (PSP epoch). Offset from the Unix
// epoch per PPSSPP: 62135596800 s.
static constexpr uint64_t k_rtc_unix_offset_us = 62135596800ULL * 1000000ULL;

static uint64_t rtc_now_ticks() {
    using namespace std::chrono;
    auto us = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    return static_cast<uint64_t>(us) + k_rtc_unix_offset_us;
}

static void hle_sceRtcGetTick(uint8_t* rdram, recomp_context* ctx) {
    uint32_t tick_ptr = static_cast<uint32_t>(ctx->r[4]);
    if (tick_ptr) psp_mem_write<uint64_t>(rdram, tick_ptr, rtc_now_ticks());
    ctx->r[2] = SCE_OK;
}
static void hle_sceRtcGetTickResolution(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = 1000000;
}
// sceRtcSetTick(ScePspDateTime* dt, const u64* tick): tick -> date fields
static void hle_sceRtcSetTick(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dt_ptr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t tick_ptr = static_cast<uint32_t>(ctx->r[5]);
    if (dt_ptr && tick_ptr) {
        uint64_t t = psp_mem_read<uint64_t>(rdram, tick_ptr);
        time_t unix_s = static_cast<time_t>((t - k_rtc_unix_offset_us) / 1000000ULL);
        struct tm* g = std::gmtime(&unix_s);
        if (g) {
            psp_mem_write<uint16_t>(rdram, dt_ptr, static_cast<uint16_t>(g->tm_year + 1900));
            psp_mem_write<uint16_t>(rdram, dt_ptr + 2, static_cast<uint16_t>(g->tm_mon + 1));
            psp_mem_write<uint16_t>(rdram, dt_ptr + 4, static_cast<uint16_t>(g->tm_mday));
            psp_mem_write<uint16_t>(rdram, dt_ptr + 6, static_cast<uint16_t>(g->tm_hour));
            psp_mem_write<uint16_t>(rdram, dt_ptr + 8, static_cast<uint16_t>(g->tm_min));
            psp_mem_write<uint16_t>(rdram, dt_ptr + 10, static_cast<uint16_t>(g->tm_sec));
            psp_mem_write<uint32_t>(rdram, dt_ptr + 12, static_cast<uint32_t>(t % 1000000ULL));
        }
    }
    ctx->r[2] = SCE_OK;
}
// sceRtcTickAddSeconds / AddDays (u64* dst, const u64* src, s64 n)
static void rtc_tick_add(uint8_t* rdram, recomp_context* ctx, uint64_t unit_us) {
    uint32_t dst = static_cast<uint32_t>(ctx->r[4]);
    uint32_t src = static_cast<uint32_t>(ctx->r[5]);
    int64_t n = (static_cast<int64_t>(static_cast<uint32_t>(ctx->r[7])) << 32) | static_cast<uint32_t>(ctx->r[6]);
    if (dst && src) {
        uint64_t t = psp_mem_read<uint64_t>(rdram, src);
        psp_mem_write<uint64_t>(rdram, dst, t + static_cast<uint64_t>(n) * unit_us);
    }
    ctx->r[2] = SCE_OK;
}
static void hle_sceRtcTickAddSeconds(uint8_t* rdram, recomp_context* ctx) { rtc_tick_add(rdram, ctx, 1000000ULL); }
static void hle_sceRtcTickAddDays(uint8_t* rdram, recomp_context* ctx) { rtc_tick_add(rdram, ctx, 86400ULL * 1000000ULL); }
// sceRtcGetDayOfWeek(year, month, day) -> 0=Sunday
static void hle_sceRtcGetDayOfWeek(uint8_t*, recomp_context* ctx) {
    int y = ctx->r[4], m = ctx->r[5], d = ctx->r[6];
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    ctx->r[2] = (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

// ---- Kernel_Library memcpy/memset and sceDmacMemcpy ----
// Log the first copies that target VRAM (0x04000000..0x04200000, any mirror).
static void log_vram_copy(const char* who, uint32_t dst, uint32_t src, uint32_t n) {
    static int logged = 0;
    const uint32_t d = dst & 0x0FFFFFFFu;
    if (d >= 0x04000000u && d < 0x04800000u && logged < 40) {
        logged++;
        std::fprintf(stderr, "[VRAM_COPY] %s dst=0x%08X src=0x%08X n=0x%X\n", who, dst, src, n);
    }
}
static void hle_sceKernelMemcpy(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dst = static_cast<uint32_t>(ctx->r[4]), src = static_cast<uint32_t>(ctx->r[5]), n = static_cast<uint32_t>(ctx->r[6]);
    log_vram_copy("sceKernelMemcpy", dst, src, n);
    if (n) std::memmove(rdram + (dst & PSP_ADDR_MASK), rdram + (src & PSP_ADDR_MASK), n);
    ctx->r[2] = static_cast<int32_t>(dst);
}
static void hle_sceKernelMemset(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dst = static_cast<uint32_t>(ctx->r[4]); int c = ctx->r[5]; uint32_t n = static_cast<uint32_t>(ctx->r[6]);
    if (n) std::memset(rdram + (dst & PSP_ADDR_MASK), c, n);
    ctx->r[2] = static_cast<int32_t>(dst);
}
static void hle_sceDmacMemcpy(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dst = static_cast<uint32_t>(ctx->r[4]), src = static_cast<uint32_t>(ctx->r[5]), n = static_cast<uint32_t>(ctx->r[6]);
    log_vram_copy("sceDmacMemcpy", dst, src, n);
    if (n) std::memmove(rdram + (dst & PSP_ADDR_MASK), rdram + (src & PSP_ADDR_MASK), n);
    ctx->r[2] = SCE_OK;
}

static void hle_sceKernelLibcTime(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t out_ptr = static_cast<uint32_t>(ctx->r[4]);
    int32_t t = static_cast<int32_t>(std::time(nullptr));

    if (out_ptr != 0) {
        psp_mem_write<int32_t>(rdram, out_ptr, t);
    }

    ctx->r[2] = t;
}

static void hle_sceKernelLibcClock(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = static_cast<int32_t>(std::clock());
    (void)rdram;
}

static void hle_sceKernelLibcGettimeofday(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t tv_ptr = static_cast<uint32_t>(ctx->r[4]);

    if (tv_ptr != 0) {
        auto now = std::chrono::system_clock::now();
        auto secs = std::chrono::duration_cast<
            std::chrono::seconds>(
                now.time_since_epoch()).count();
        auto usecs = std::chrono::duration_cast<
            std::chrono::microseconds>(
                now.time_since_epoch()).count() % 1000000;
        // struct timeval: tv_sec(u32), tv_usec(u32)
        psp_mem_write<uint32_t>(rdram, tv_ptr,
            static_cast<uint32_t>(secs));
        psp_mem_write<uint32_t>(rdram, tv_ptr + 4,
            static_cast<uint32_t>(usecs));
    }

    ctx->r[2] = SCE_OK;
}

// ---- Audio (timing only; no rendering yet) ----
// Issue #29: the *Blocking output calls must pace the caller at the
// hardware playback rate (44.1kHz). Returning instantly made the PCM
// loop spin at kHz, starving everything else.

// PSPRECOMP_AUDIO_TRACE=1: log every sceAudio channel-config call.
static bool audio_trace() {
    static const bool on = std::getenv("PSPRECOMP_AUDIO_TRACE") != nullptr;
    return on;
}

static constexpr int AUDIO_OUTPUT_SAMPLE_RATE = 44100;
static constexpr int AUDIO_CHANNEL_COUNT = 8;
// Valid PSP sample counts: 17..4111 (sceAudioOutput2Reserve contract).
static constexpr int AUDIO_MIN_SAMPLES = 17;
static constexpr int AUDIO_MAX_SAMPLES = 4111;

// Sample count from sceAudioOutput2Reserve (default one 1024 grain
// if Reserve was never seen).
static int g_output2_samples = 1024;
// Per-channel sample counts from sceAudioChReserve.
static int g_channel_samples[AUDIO_CHANNEL_COUNT] = {
    1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
};
// Per-channel format from sceAudioChReserve: 0 = stereo, 0x10 = mono.
static int g_channel_mono[AUDIO_CHANNEL_COUNT] = {0};
// sceAudioChReserve(-1, ...) allocates the first free channel; Release frees it.
static bool g_channel_reserved[AUDIO_CHANNEL_COUNT] = {false};

/// Hand `frames` frames of guest PCM at `buf` to the host audio sink.
static bool audio_submit(uint8_t* rdram, int source, uint32_t buf, int frames,
                         bool mono, int volL, int volR) {
    if (!buf || frames <= 0) return false;
    uint64_t bytes = static_cast<uint64_t>(frames) * (mono ? 2 : 4);
    uint32_t off = buf & PSP_ADDR_MASK;
    if (off < 0x10000u || off + bytes > PSP_MEM_SIZE) return false;
    psp_audio_push(source, reinterpret_cast<const int16_t*>(rdram + off),
                   frames, mono ? 1 : 2, volL, volR);
    return true;
}

/// Block the calling thread for the playback duration of `samples`
/// samples at 44.1kHz. Mirrors the hle_sceKernelDelayThread blocking
/// idiom (psp_hle_kernel_thread.cpp): dispatch pending IO callbacks,
/// yield, then sleep.
static void audio_block_for_samples(
    uint8_t* rdram, recomp_context* ctx, int source, int64_t samples, bool queued
) {
    psp_kernel_check_callbacks(rdram, ctx);
    sched_yield_point();
    // Paced against the sink's real drain (or an absolute timeline), not a
    // fixed post-work sleep that made the producer slower than real time.
    psp_audio_pace(source, static_cast<int>(samples), queued);
}

static void hle_sceAudioOutputBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    // Preserve existing return convention (a1) before callbacks can
    // clobber argument registers.
    int32_t ret = ctx->r[5];
    int samples = (channel < AUDIO_CHANNEL_COUNT)
        ? g_channel_samples[channel] : 1024;
    bool queued = false;
    if (channel < AUDIO_CHANNEL_COUNT) {
        int vol = ctx->r[5];
        queued = audio_submit(rdram, PSP_AUDIO_SRC_CHANNEL0 + channel,
                     static_cast<uint32_t>(ctx->r[6]), samples,
                     g_channel_mono[channel] != 0, vol, vol);
    }
    audio_block_for_samples(rdram, ctx, PSP_AUDIO_SRC_CHANNEL0 + (channel < AUDIO_CHANNEL_COUNT ? channel : 0), samples, queued);
    ctx->r[2] = ret;
}

static void hle_sceAudioOutputPannedBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    // Preserve existing return convention (a3) before callbacks can
    // clobber argument registers.
    int32_t ret = ctx->r[7];
    int samples = (channel < AUDIO_CHANNEL_COUNT)
        ? g_channel_samples[channel] : 1024;
    if (audio_trace()) {
        static int n = 0;
        if (n++ < 12) std::fprintf(stderr, "[AUDIOCFG] OutputPannedBlocking(ch=%u volL=%d volR=%d buf=%#x) samples=%d\n",
                                   channel, (int)ctx->r[5], (int)ctx->r[6], (unsigned)ctx->r[7], samples);
    }
    bool queued = false;
    if (channel < AUDIO_CHANNEL_COUNT) {
        queued = audio_submit(rdram, PSP_AUDIO_SRC_CHANNEL0 + channel,
                     static_cast<uint32_t>(ctx->r[7]), samples,
                     g_channel_mono[channel] != 0, ctx->r[5], ctx->r[6]);
    }
    audio_block_for_samples(rdram, ctx, PSP_AUDIO_SRC_CHANNEL0 + (channel < AUDIO_CHANNEL_COUNT ? channel : 0), samples, queued);
    ctx->r[2] = ret;
}

static void hle_sceAudioChReserve(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t channel = ctx->r[4];
    int32_t samples = ctx->r[5];
    if (audio_trace())
        std::fprintf(stderr, "[AUDIOCFG] sceAudioChReserve(ch=%d samples=%d fmt=%#x)\n",
                     channel, samples, (unsigned)ctx->r[6]);
    if (channel < 0 || channel >= AUDIO_CHANNEL_COUNT) {
        // Auto-allocate the first free channel. (Mapping every -1 to channel
        // 0 made the game's four reserved channels share one slot.)
        channel = -1;
        for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++)
            if (!g_channel_reserved[i]) { channel = i; break; }
        if (channel < 0) {
            ctx->r[2] = static_cast<int32_t>(0x80260005u);  // no channel available
            (void)rdram;
            return;
        }
    }
    g_channel_reserved[channel] = true;
    if (samples >= AUDIO_MIN_SAMPLES && samples <= AUDIO_MAX_SAMPLES) {
        g_channel_samples[channel] = samples;
    }
    g_channel_mono[channel] = (ctx->r[6] == 0x10) ? 1 : 0;
    ctx->r[2] = channel;
    (void)rdram;
}

static void hle_sceAudioChRelease(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t channel = ctx->r[4];
    if (channel >= 0 && channel < AUDIO_CHANNEL_COUNT) g_channel_reserved[channel] = false;
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutput2Reserve(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t samples = ctx->r[4] & 0x7FFFFFFF;
    if (samples >= AUDIO_MIN_SAMPLES && samples <= AUDIO_MAX_SAMPLES) {
        g_output2_samples = samples;
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutput2OutputBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0=vol, a1=buf. buf==0 is the pre-Release drain: sleep one grain
    // and report success, same as a normal grain.
    bool queued = audio_submit(rdram, PSP_AUDIO_SRC_OUTPUT2, static_cast<uint32_t>(ctx->r[5]),
                 g_output2_samples, false, ctx->r[4], ctx->r[4]);
    audio_block_for_samples(rdram, ctx, PSP_AUDIO_SRC_OUTPUT2, g_output2_samples, queued);
    ctx->r[2] = SCE_OK;
}

static void hle_sceAudioOutput2Release(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// Non-blocking output: queue the channel's grain and return at once
// (sceAudioOutputPanned(ch, volL, volR, buf); sceAudioOutput(ch, vol, buf)).
static void audio_output_nonblocking(
    uint8_t* rdram, recomp_context* ctx, int volL, int volR, uint32_t buf
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    if (channel < AUDIO_CHANNEL_COUNT)
        audio_submit(rdram, PSP_AUDIO_SRC_CHANNEL0 + channel, buf, g_channel_samples[channel],
                     g_channel_mono[channel] != 0, volL, volR);
    ctx->r[2] = SCE_OK;
}

static void hle_sceAudioOutputPanned(
    uint8_t* rdram, recomp_context* ctx
) {
    audio_output_nonblocking(rdram, ctx, ctx->r[5], ctx->r[6], static_cast<uint32_t>(ctx->r[7]));
}

static void hle_sceAudioOutput(
    uint8_t* rdram, recomp_context* ctx
) {
    audio_output_nonblocking(rdram, ctx, ctx->r[5], ctx->r[5], static_cast<uint32_t>(ctx->r[6]));
}

static void hle_sceAudioChangeChannelConfig(
    uint8_t* rdram, recomp_context* ctx
) {
    if (audio_trace())
        std::fprintf(stderr, "[AUDIOCFG] sceAudioChangeChannelConfig(ch=%d fmt=%#x)\n",
                     (int)ctx->r[4], (unsigned)ctx->r[5]);
    {
        // sceAudioChangeChannelConfig(ch, format): 0 stereo, 0x10 mono.
        uint32_t ch = static_cast<uint32_t>(ctx->r[4]);
        if (ch < AUDIO_CHANNEL_COUNT) g_channel_mono[ch] = (ctx->r[5] == 0x10) ? 1 : 0;
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioGetChannelRestLength(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t ch = static_cast<uint32_t>(ctx->r[4]);
    ctx->r[2] = ch < AUDIO_CHANNEL_COUNT
        ? static_cast<int32_t>(psp_audio_queued_frames(PSP_AUDIO_SRC_CHANNEL0 + ch)) : 0;
    (void)rdram;
}

static void hle_sceAudioChangeChannelVolume(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioSetChannelDataLen(
    uint8_t* rdram, recomp_context* ctx
) {
    if (audio_trace())
        std::fprintf(stderr, "[AUDIOCFG] sceAudioSetChannelDataLen(ch=%d samples=%d)\n",
                     (int)ctx->r[4], (int)ctx->r[5]);
    {
        // sceAudioSetChannelDataLen(ch, samples): the game switches a reserved
        // channel to its real grain (Dissidia's movie audio: 2048 after
        // reserving 256). Ignoring this played 1/8 of each decoded frame.
        uint32_t ch = static_cast<uint32_t>(ctx->r[4]);
        int32_t n = ctx->r[5];
        if (ch < AUDIO_CHANNEL_COUNT && n >= AUDIO_MIN_SAMPLES && n <= AUDIO_MAX_SAMPLES)
            g_channel_samples[ch] = n;
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- ATRAC3/ATRAC3+ moved to psp_hle_atrac.cpp ----

// ---- Module Management ----

struct ModuleInfo {
    int uid;
    uint32_t native_module_addr;  // Address in rdram of NativeModule
    uint32_t entry_addr;          // 0xFFFFFFFF for fake/PRX modules
    char name[28];
};

static std::vector<ModuleInfo> g_modules;
static int g_module_next_uid = 0x1000;

/// Initialize boot module in tracking list.
/// Called from psp_hle_register_utility().
static void init_boot_module_tracking() {
    ModuleInfo boot_mod{};
    boot_mod.uid = BOOT_MODULE_UID;
    boot_mod.native_module_addr = BOOT_MODULE_ADDR;
    boot_mod.entry_addr = RECOMP_MODULE_ENTRY;
    std::strncpy(boot_mod.name, RECOMP_MODULE_NAME, sizeof(boot_mod.name) - 1);
    g_modules.push_back(boot_mod);
}

/// Check if a path refers to BOOT.BIN.
/// Primary: case-insensitive last-component match.
/// Fallback: scan entire path for "BOOT.BIN" substring.
/// Handles "0sdisc0:0s/PSP_GAME/SYSDIR/BOOT.BIN" corruption
/// from LWL/LWR string-copy artifacts.
static bool path_is_boot_bin(const char* path) {
    // Primary: check last path component
    const char* slash = std::strrchr(path, '/');
    const char* filename = slash ? slash + 1 : path;
    const char* target = "BOOT.BIN";
    bool primary_match = true;
    for (int i = 0; target[i]; i++) {
        char c = filename[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != target[i]) { primary_match = false; break; }
    }
    if (primary_match && filename[8] == '\0') return true;

    // Fallback: check if "BOOT.BIN" appears anywhere in path
    for (const char* p = path; *p; p++) {
        if ((*p == 'B' || *p == 'b') &&
            strncasecmp(p, "BOOT.BIN", 8) == 0) {
            return true;
        }
    }
    return false;
}

static void hle_sceKernelLoadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t path_ptr = static_cast<uint32_t>(ctx->r[4]);
    const char* path = reinterpret_cast<const char*>(
        rdram + (path_ptr & PSP_ADDR_MASK));

    // Special case: BOOT.BIN self-load.
    // PPSSPP handles this via __KernelLoadExec (full restart).
    // Static recompiler: return existing boot module UID since
    // the game binary is already loaded and running.
    if (path_is_boot_bin(path)) {
        std::fprintf(stderr,
            "[HLE] sceKernelLoadModule(\"%s\") "
            "-> returning boot module uid=%d (self-load)\n",
            path, BOOT_MODULE_UID);
        ctx->r[2] = BOOT_MODULE_UID;
        return;
    }

    int uid = g_module_next_uid++;

    // Allocate a fake NativeModule in kernel memory
    uint32_t mod_addr =
        psp_alloc_kernel_memory(NATIVE_MODULE_SIZE);
    if (mod_addr != 0) {
        std::memset(
            rdram + (mod_addr & PSP_ADDR_MASK),
            0, NATIVE_MODULE_SIZE);
        // entry_addr = 0xFFFFFFFF (fake module, no entry)
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x64, 0xFFFFFFFFU);
        // status = 4 (MODULE_STATUS_STARTING)
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x24, 4);
        // modid
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x2C,
            static_cast<uint32_t>(uid));
        // Extract filename from path (last '/' component)
        const char* filename = path;
        const char* slash = std::strrchr(path, '/');
        if (slash) {
            filename = slash + 1;
        }
        std::strncpy(
            reinterpret_cast<char*>(
                rdram + ((mod_addr + 0x08) & PSP_ADDR_MASK)),
            filename, 27);
    }

    // Track module
    ModuleInfo info{};
    info.uid = uid;
    info.native_module_addr = mod_addr;
    info.entry_addr = 0xFFFFFFFFU;
    const char* fname = path;
    const char* sl = std::strrchr(path, '/');
    if (sl) { fname = sl + 1; }
    std::strncpy(info.name, fname, sizeof(info.name) - 1);
    g_modules.push_back(info);

    std::fprintf(stderr,
        "[HLE] sceKernelLoadModule(\"%s\") -> uid=%d (fake)\n",
        path, uid);
    ctx->r[2] = uid;
}

static void hle_sceKernelStartModule(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    const char* action = "ok";

    // Look up module in tracking list
    for (auto& mod : g_modules) {
        if (mod.uid == uid) {
            // Boot module special case: it is already loaded and
            // running. The game calls LoadModule("BOOT.BIN") which
            // returns BOOT_MODULE_UID, then StartModule on it.
            // PPSSPP returns 0 for already-started modules.
            if (uid == BOOT_MODULE_UID) {
                std::fprintf(stderr,
                    "[HLE] sceKernelStartModule(uid=%d) "
                    "-> already started (boot module), "
                    "returning 0\n", uid);
                ctx->r[2] = SCE_OK;
                return;
            }

            if (mod.entry_addr == 0xFFFFFFFFU) {
                // Fake module: set status to STARTED, skip thread
                if (mod.native_module_addr != 0) {
                    psp_mem_write<uint32_t>(
                        rdram,
                        mod.native_module_addr + 0x24, 5);
                }
                action = "skipped (fake)";
            }
            break;
        }
    }

    std::fprintf(stderr,
        "[HLE] sceKernelStartModule(uid=%d) -> %s\n",
        uid, action);
    ctx->r[2] = SCE_OK;
}

static void hle_sceKernelStopModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelUnloadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelStopUnloadSelfModuleWithStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelGetModuleIdByAddress(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    // If this ever gains a real UNKNOWN_MODULE branch: PPSSPP matches by memory
    // block (RECOMP_SEG0_VADDR..+MEMSZ), NOT the text range — data/.bss addresses
    // must still resolve to the module UID (sceKernelModule.cpp:2388).
    constexpr uint32_t TEXT_START = RECOMP_MODULE_TEXT_START;
    constexpr uint32_t TEXT_END = TEXT_START + RECOMP_MODULE_TEXT_SIZE;
    if (addr >= TEXT_START && addr < TEXT_END) {
        ctx->r[2] = BOOT_MODULE_UID;
    } else {
        // Fallback: only one real module exists
        ctx->r[2] = BOOT_MODULE_UID;
    }
    (void)rdram;
}

static void hle_sceKernelGetModuleId(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = BOOT_MODULE_UID;
    (void)rdram;
}

static void hle_sceKernelQueryModuleInfo(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    uint32_t info_addr = static_cast<uint32_t>(ctx->r[5]);

    if (info_addr == 0) {
        std::fprintf(stderr,
            "[HLE] sceKernelQueryModuleInfo(uid=%d) "
            "-> EINVAL (null infoAddr)\n", uid);
        ctx->r[2] = SCE_ERROR_ERRNO_EINVAL;
        return;
    }

    // Find the module in tracking list
    uint32_t native_addr = 0;
    bool found = false;
    for (const auto& mod : g_modules) {
        if (mod.uid == uid) {
            native_addr = mod.native_module_addr;
            found = true;
            break;
        }
    }

    if (!found) {
        std::fprintf(stderr,
            "[HLE] sceKernelQueryModuleInfo(uid=%d) "
            "-> NOT_FOUND_MODULE\n", uid);
        ctx->r[2] = SCE_KERNEL_ERROR_NOT_FOUND_MODULE;
        return;
    }

    // Read fields from NativeModule in rdram and write
    // to SceKernelModuleInfo output struct at info_addr.
    //
    // NativeModule (source)        SceKernelModuleInfo (dest)
    // +0x7C nsegment            -> +0x0004 nsegment (u32)
    // +0x80 segaddr[0..3]       -> +0x0008 segmentaddr[0..3]
    // +0x90 segsize[0..3]       -> +0x0018 segmentsize[0..3]
    // +0x50 entry_addr          -> +0x0028 entry_addr (u32)
    // +0x68 gp_value            -> +0x002C gp_value (u32)
    // +0x6C text_addr           -> +0x0030 text_addr (u32)
    // +0x70 text_size           -> +0x0034 text_size (u32)
    //                              +0x0038 data_size = 0
    //                              +0x003C bss_size = 0
    // +0x04 attribute           -> +0x0040 attribute (u16)
    // +0x06 version             -> +0x0042 version (u8[2])
    // +0x08 name                -> +0x0044 name (28 bytes)

    // nsegment
    uint32_t nseg = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x7C);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x04, nseg);

    // segmentaddr[0..3]
    for (int i = 0; i < 4; i++) {
        uint32_t sa = psp_mem_read<uint32_t>(
            rdram, native_addr + 0x80 + i * 4);
        psp_mem_write<uint32_t>(
            rdram, info_addr + 0x08 + i * 4, sa);
    }

    // segmentsize[0..3]
    for (int i = 0; i < 4; i++) {
        uint32_t ss = psp_mem_read<uint32_t>(
            rdram, native_addr + 0x90 + i * 4);
        psp_mem_write<uint32_t>(
            rdram, info_addr + 0x18 + i * 4, ss);
    }

    // entry_addr (from module_start_func at +0x50)
    uint32_t entry = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x50);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x28, entry);

    // gp_value
    uint32_t gp = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x68);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x2C, gp);

    // text_addr
    uint32_t ta = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x6C);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x30, ta);

    // text_size
    uint32_t ts = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x70);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x34, ts);

    // data_size = 0, bss_size = 0 (not tracked in NativeModule)
    psp_mem_write<uint32_t>(rdram, info_addr + 0x38, 0);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x3C, 0);

    // attribute
    uint16_t attr = psp_mem_read<uint16_t>(
        rdram, native_addr + 0x04);
    psp_mem_write<uint16_t>(rdram, info_addr + 0x40, attr);

    // version[2]
    uint8_t v0 = psp_mem_read<uint8_t>(
        rdram, native_addr + 0x06);
    uint8_t v1 = psp_mem_read<uint8_t>(
        rdram, native_addr + 0x07);
    psp_mem_write<uint8_t>(rdram, info_addr + 0x42, v0);
    psp_mem_write<uint8_t>(rdram, info_addr + 0x43, v1);

    // name (28 bytes)
    std::memcpy(
        rdram + ((info_addr + 0x44) & PSP_ADDR_MASK),
        rdram + ((native_addr + 0x08) & PSP_ADDR_MASK),
        28);

    std::fprintf(stderr,
        "[HLE] sceKernelQueryModuleInfo(uid=%d) -> OK\n",
        uid);
    ctx->r[2] = SCE_OK;
}

// ---- UMD (Universal Media Disc) ----

static void hle_sceUmdCheckMedium(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // Disc is present
    (void)rdram;
}

static void hle_sceUmdGetDriveStat(
    uint8_t* rdram, recomp_context* ctx
) {
    // PSP_UMD_PRESENT | PSP_UMD_READY | PSP_UMD_READABLE = 0x02 | 0x10 | 0x20 = 0x32
    // Bit values from PPSSPP sceUmd.h:
    //   0x01 = NOT_PRESENT, 0x02 = PRESENT, 0x04 = CHANGED,
    //   0x08 = NOT_READY,   0x10 = READY,   0x20 = READABLE
    ctx->r[2] = 0x02 | 0x10 | 0x20;
    (void)rdram;
}

static void hle_sceUmdActivate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdWaitDriveStat(
    uint8_t* rdram, recomp_context* ctx
) {
    sched_yield_point();
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdWaitDriveStatCB(
    uint8_t* rdram, recomp_context* ctx
) {
    psp_kernel_check_callbacks(rdram, ctx);
    sched_yield_point();
    ctx->r[2] = SCE_OK;
}

static void hle_sceUmdGetErrorStat(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // No error
    (void)rdram;
}

static void hle_sceUmdRegisterUMDCallBack(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdUnRegisterUMDCallBack(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Networking (all init/term no-ops) ----

static void hle_sceNetInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetInetInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetInetTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetApctlInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetApctlTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetResolverInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetResolverTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- HTTP/SSL ----

static void hle_sceHttpInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsLoadDefaultCert(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpSaveSystemCookie(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpLoadSystemCookie(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceSslInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceSslEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- MPEG: see psp_hle_mpeg.cpp ----

// ---- PSMF (PlayStation Media Framework) ----

static void hle_scePsmfSetPsmf(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfGetVideoInfo(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfSpecifyStream(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfGetNumberOfSpecificStreams(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // No streams
    (void)rdram;
}

static void hle_scePsmfGetCurrentStreamType(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfGetPsmfVersion(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // Version 1
    (void)rdram;
}

static void hle_scePsmfGetNumberOfStreams(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;
    (void)rdram;
}

// ---- SAS (Software Audio Synthesis) ----
// Moved to psp_hle_sas.cpp (issue #29: minimal voice state machine).

// ---- Stdio ----

static void hle_sceKernelStdin(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // fd 0
    (void)rdram;
}

static void hle_sceKernelStdout(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // fd 1
    (void)rdram;
}

static void hle_sceKernelStderr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 2;  // fd 2
    (void)rdram;
}

// ---- Interrupt Manager ----

static void hle_sceKernelRegisterSubIntrHandler(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelReleaseSubIntrHandler(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelEnableSubIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Kernel_Library ----

static void hle_sceKernelCpuSuspendIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    // Return previous interrupt state (0 = enabled)
    ctx->r[2] = 0;
    (void)rdram;
}

static void hle_sceKernelCpuResumeIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- LoadExec ----

static void hle_sceKernelExitGame(
    uint8_t* rdram, recomp_context* ctx
) {
    std::fprintf(stderr, "[HLE] sceKernelExitGame called\n");
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelRegisterExitCallback(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Impose ----

static void hle_sceImposeGetLanguageMode(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0 = lang_ptr, a1 = button_ptr
    uint32_t lang_ptr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t button_ptr = static_cast<uint32_t>(ctx->r[5]);

    if (lang_ptr != 0) {
        psp_mem_write<int32_t>(rdram, lang_ptr, 1);  // English
    }
    if (button_ptr != 0) {
        psp_mem_write<int32_t>(rdram, button_ptr, 1);  // Cross=confirm
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceImposeSetLanguageMode(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceImposeSetUMDPopup(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Suspend ----

static void hle_sceKernelPowerTick(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- sceUtility ----

static void hle_sceUtilityLoadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityUnloadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityGetSystemParamInt(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t param_id = ctx->r[4];
    uint32_t value_ptr = static_cast<uint32_t>(ctx->r[5]);

    int32_t value = 0;
    switch (param_id) {
        case 1: value = 1; break;  // Language = English
        case 5: value = 0; break;  // Date format = YYYYMMDD
        case 6: value = 0; break;  // Time format = 24h
        case 7: value = 0; break;  // Timezone offset
        case 8: value = 1; break;  // Daylight saving
        case 9: value = 1; break;  // Nickname (not impl'd via int)
        default: break;
    }

    if (value_ptr != 0) {
        psp_mem_write<int32_t>(rdram, value_ptr, value);
    }

    ctx->r[2] = SCE_OK;
}

// ---- Utility dialog status protocol (savedata, msg dialog) ----
// PSP utility dialogs share a status machine (PPSSPP
// Core/Dialog/PSPDialog.h DialogStatus):
//   NONE(0) -> InitStart -> INITIALIZE(1) -> RUNNING(2) -> FINISHED(3)
//   -> ShutdownStart -> SHUTDOWN(4) -> NONE(0)
// GetStatus auto-advances INITIALIZE->RUNNING and SHUTDOWN->NONE
// (PPSSPP PSPDialog::GetStatus with UseAutoStatus). We have no dialog
// UI, so the "dialog" completes instantly: the RUNNING poll performs
// the work, writes pspUtilityDialogCommon.result (offset +28: u32
// size, s32 language, s32 buttonSwap, s32 graphics/access/font/sound
// Thread, s32 result, s32 reserved[4] = 48 bytes), and advances to
// FINISHED for the next poll.
static constexpr int32_t PSP_UTILITY_STATUS_NONE = 0;
static constexpr int32_t PSP_UTILITY_STATUS_INITIALIZE = 1;
static constexpr int32_t PSP_UTILITY_STATUS_RUNNING = 2;
static constexpr int32_t PSP_UTILITY_STATUS_FINISHED = 3;
static constexpr int32_t PSP_UTILITY_STATUS_SHUTDOWN = 4;

// PPSSPP Core/HLE/ErrorCodes.h
static constexpr uint32_t SCE_ERROR_UTILITY_INVALID_STATUS = 0x80110001U;
static constexpr uint32_t SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA =
    0x80110307U;
static constexpr uint32_t SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA =
    0x80110327U;
static constexpr uint32_t SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA =
    0x80110347U;
static constexpr uint32_t SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA =
    0x801103C7U;

// pspUtilityDialogCommon.result offset within the param struct.
static constexpr uint32_t UTILITY_COMMON_RESULT_OFFSET = 28;
// SceUtilitySavedataParam.mode immediately follows the 48-byte common
// header (PPSSPP Core/Dialog/SavedataParam.h).
static constexpr uint32_t SAVEDATA_MODE_OFFSET = 48;

struct UtilityDialogState {
    int32_t status = PSP_UTILITY_STATUS_NONE;
    uint32_t param_addr = 0;
    // Set by a complete() callback that needs more RUNNING polls (an
    // interactive dialog still waiting for the user).
    bool pending = false;
};

static UtilityDialogState g_savedata_dialog;
static UtilityDialogState g_msg_dialog;

static void utility_dialog_init_start(
    UtilityDialogState& dlg, recomp_context* ctx
) {
    if (dlg.status != PSP_UTILITY_STATUS_NONE) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    dlg.param_addr = static_cast<uint32_t>(ctx->r[4]);
    dlg.status = PSP_UTILITY_STATUS_INITIALIZE;
    ctx->r[2] = SCE_OK;
}

// Returns the pre-advance status; performs `complete` (writes the
// result into the param struct) when leaving RUNNING.
template <typename CompleteFn>
static void utility_dialog_get_status(
    UtilityDialogState& dlg, uint8_t* rdram, recomp_context* ctx,
    CompleteFn complete
) {
    int32_t ret = dlg.status;
    switch (dlg.status) {
        case PSP_UTILITY_STATUS_INITIALIZE:
            dlg.status = PSP_UTILITY_STATUS_RUNNING;
            break;
        case PSP_UTILITY_STATUS_RUNNING:
            dlg.pending = false;
            complete(dlg, rdram);
            if (!dlg.pending) dlg.status = PSP_UTILITY_STATUS_FINISHED;
            break;
        case PSP_UTILITY_STATUS_SHUTDOWN:
            dlg.status = PSP_UTILITY_STATUS_NONE;
            dlg.param_addr = 0;
            break;
        default:
            break;
    }
    ctx->r[2] = ret;
}

static void utility_dialog_shutdown_start(
    UtilityDialogState& dlg, recomp_context* ctx
) {
    if (dlg.status != PSP_UTILITY_STATUS_FINISHED) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    dlg.status = PSP_UTILITY_STATUS_SHUTDOWN;
    ctx->r[2] = SCE_OK;
}

static void utility_dialog_update(
    UtilityDialogState& dlg, recomp_context* ctx
) {
    if (dlg.status != PSP_UTILITY_STATUS_RUNNING) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    ctx->r[2] = SCE_OK;
}

// ---- Savedata write path (modes AUTOSAVE=1, SAVE=3, LISTSAVE=5) ----
// SceUtilitySavedataParam (PPSSPP SavedataParam.h): +0x30 mode,
// +0x3C char gameName[13], +0x4C char saveName[20], +0x64 char
// fileName[13], +0x74 dataBuf, +0x78 bufSize, +0x7C dataSize, +0x80
// sfoParam{ char title[0x80], savedataTitle[0x80], detail[0x400], u8
// parentalLevel, pad[3] } (0x504), +0x584 icon0 {buf,bufSize,size,unk}.
static constexpr uint32_t SD_GAMENAME = 0x3C, SD_SAVENAME = 0x4C,
    SD_FILENAME = 0x64, SD_DATABUF = 0x74, SD_DATASIZE = 0x7C,
    SD_SFO = 0x80, SD_ICON0 = 0x584;

static std::string sd_cstr(uint8_t* rdram, uint32_t addr, size_t max) {
    std::string r;
    for (size_t i = 0; i < max; ++i) {
        char c = static_cast<char>(psp_mem_read<uint8_t>(rdram, addr + i));
        if (!c) break;
        r.push_back(c);
    }
    return r;
}

static bool sd_write_file(const std::string& path, const void* d, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = n == 0 || fwrite(d, 1, n, f) == n;
    fclose(f);
    return ok;
}

static void sd_put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = (x >> (8 * i)) & 0xFF;
}

// ---- PARAM.SFO model ----
// Parsed/written as a sorted key -> value map so an existing (PPSSPP or
// PSP-written) file keeps every entry we do not touch. Value formats:
// 0x0004 binary, 0x0204 UTF-8 string (NUL-terminated), 0x0404 u32.
struct SfoVal {
    uint16_t fmt = 0x0204;
    uint32_t cap = 0;
    std::vector<uint8_t> data;   // used bytes
};
using SfoMap = std::map<std::string, SfoVal>;

static bool sfo_parse(const std::vector<uint8_t>& v, SfoMap& out) {
    auto rd32 = [&](size_t o) -> uint32_t {
        return o + 4 <= v.size() ? v[o] | v[o+1] << 8 | v[o+2] << 16 |
                                   uint32_t(v[o+3]) << 24 : 0; };
    if (v.size() < 20 || v[1] != 'P' || v[2] != 'S' || v[3] != 'F') return false;
    uint32_t kt = rd32(8), dt = rd32(12), n = rd32(16);
    if (n > 256) return false;
    for (uint32_t i = 0; i < n && 20 + 16 * (i + 1) <= v.size(); ++i) {
        size_t e = 20 + 16 * i;
        uint32_t ko = v[e] | v[e + 1] << 8, len = rd32(e + 4),
                 cap = rd32(e + 8), off = rd32(e + 12);
        if (size_t(kt) + ko >= v.size() || size_t(dt) + off > v.size()) continue;
        std::string key(reinterpret_cast<const char*>(&v[kt + ko]),
            strnlen(reinterpret_cast<const char*>(&v[kt + ko]), v.size() - (kt + ko)));
        SfoVal val;
        val.fmt = uint16_t(v[e + 2] | v[e + 3] << 8);
        val.cap = cap;
        size_t avail = std::min<size_t>(len, v.size() - (size_t(dt) + off));
        val.data.assign(v.begin() + dt + off, v.begin() + dt + off + avail);
        out[key] = std::move(val);
    }
    return true;
}
static void sfo_set_str(SfoMap& m, const std::string& k, const std::string& s,
                        uint32_t cap) {
    SfoVal v;
    v.fmt = 0x0204; v.cap = cap;
    size_t n = std::min<size_t>(s.size(), cap ? cap - 1 : 0);
    v.data.assign(s.begin(), s.begin() + n);
    v.data.push_back(0);
    m[k] = std::move(v);
}
static void sfo_set_int(SfoMap& m, const std::string& k, uint32_t x) {
    SfoVal v;
    v.fmt = 0x0404; v.cap = 4;
    v.data = {uint8_t(x), uint8_t(x >> 8), uint8_t(x >> 16), uint8_t(x >> 24)};
    m[k] = std::move(v);
}
static void sfo_set_bin(SfoMap& m, const std::string& k,
                        const std::vector<uint8_t>& d, uint32_t cap) {
    SfoVal v;
    v.fmt = 0x0004; v.cap = cap;
    v.data = d;
    v.data.resize(cap, 0);
    m[k] = std::move(v);
}
static std::string sfo_get_str(const SfoMap& m, const char* k) {
    auto it = m.find(k);
    if (it == m.end()) return "";
    const auto& d = it->second.data;
    return std::string(reinterpret_cast<const char*>(d.data()),
                       strnlen(reinterpret_cast<const char*>(d.data()), d.size()));
}
// Serialises to a PSF whose size is a multiple of 16 (zero padded), which
// is what the SAVEDATA_PARAMS hash is computed over. *params_off receives
// the file offset of the SAVEDATA_PARAMS value (or SIZE_MAX).
static std::vector<uint8_t> sfo_write(const SfoMap& m, size_t* params_off) {
    size_t n = m.size(), keytab = 20 + 16 * n, keysz = 0;
    for (auto& kv : m) keysz += kv.first.size() + 1;
    size_t datatab = (keytab + keysz + 3) & ~size_t(3);
    size_t total = datatab;
    for (auto& kv : m) total += (kv.second.cap + 3) & ~3u;
    total = (total + 15) & ~size_t(15);
    std::vector<uint8_t> v(total, 0);
    v[1] = 'P'; v[2] = 'S'; v[3] = 'F'; v[4] = 1; v[5] = 1;
    sd_put32(v, 8, uint32_t(keytab)); sd_put32(v, 12, uint32_t(datatab));
    sd_put32(v, 16, uint32_t(n));
    size_t koff = 0, doff = 0, i = 0;
    if (params_off) *params_off = SIZE_MAX;
    for (auto& kv : m) {
        const SfoVal& e = kv.second;
        size_t ent = 20 + 16 * i++;
        v[ent] = koff & 0xFF; v[ent + 1] = (koff >> 8) & 0xFF;
        v[ent + 2] = e.fmt & 0xFF; v[ent + 3] = e.fmt >> 8;
        sd_put32(v, ent + 4, uint32_t(e.data.size()));
        sd_put32(v, ent + 8, e.cap);
        sd_put32(v, ent + 12, uint32_t(doff));
        memcpy(&v[keytab + koff], kv.first.c_str(), kv.first.size() + 1);
        memcpy(&v[datatab + doff], e.data.data(),
               std::min<size_t>(e.data.size(), e.cap));
        if (kv.first == "SAVEDATA_PARAMS" && params_off) *params_off = datatab + doff;
        koff += kv.first.size() + 1;
        doff += (e.cap + 3) & ~3u;
    }
    return v;
}

// ---- Savedata read side + dispatcher ----
// Layout (PPSSPP SavedataParam.h / pspsdk SceUtilitySavedataParam):
//   +0x30 mode, +0x34 bind, +0x38 overwriteMode, +0x3C gameName[13]
//   (+3 pad), +0x4C saveName[20], +0x60 saveNameList ptr, +0x64
//   fileName[13] (+3 pad), +0x74 dataBuf, +0x78 dataBufSize, +0x7C
//   dataSize, +0x80 sfoParam (0x504), +0x584/594/5A4/5B4 icon0/icon1/
//   pic1/snd0 {buf,bufSize,size,unk}, +0x5C4 newData, +0x5C8 focus,
//   +0x5CC abortStatus, +0x5D0 msFree, +0x5D4 msData, +0x5D8
//   utilityData, +0x5DC key[16], +0x5EC secureVersion, +0x5F0
//   multiStatus, +0x5F4 idList, +0x5F8 fileList, +0x5FC sizeInfo.
static constexpr uint32_t SD_KEY = 0x5DC, SD_SECUREVER = 0x5EC,
    SD_BIND = 0x34, SD_FOCUS = 0x5C8;
static constexpr uint32_t SD_ERR_LOAD_PARAM = 0x80110308U,
    SD_ERR_SAVE_PARAM = 0x80110388U;
static constexpr uint32_t SD_SAVENAMELIST = 0x60, SD_DATABUFSIZE = 0x78,
    SD_ICON_BASE = 0x584, SD_MSFREE = 0x5D0, SD_MSDATA = 0x5D4,
    SD_UTILDATA = 0x5D8, SD_IDLIST = 0x5F4, SD_FILELIST = 0x5F8;
static constexpr uint32_t SD_CLUSTER = 32768;
static constexpr uint32_t SD_ERR_RW_FILE_NOT_FOUND = 0x80110322U;
static constexpr uint32_t SD_ERR_DELETE_NO_DATA = 0x80110347U;
static const char* const SD_SYS_FILES[4] = {
    "ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3"};

static bool sd_ram_ok(uint32_t a, uint32_t n) {
    return a >= 0x08000000u && a < 0x0C000000u &&
           n <= 0x0C000000u - a;
}

static bool sd_is_dir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool sd_read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        out.insert(out.end(), tmp, tmp + n);
    fclose(f);
    return true;
}

static std::vector<std::string> sd_list_dir(const std::string& path) {
    std::vector<std::string> v;
    DIR* d = opendir(path.c_str());
    if (!d) return v;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        v.push_back(e->d_name);
    }
    closedir(d);
    std::sort(v.begin(), v.end());
    return v;
}

static bool sd_glob(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') {
        for (;; ++s) {
            if (sd_glob(p + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*s) return false;
    if (*p == '?' || *p == *s) return sd_glob(p + 1, s + 1);
    return false;
}

static void sd_put_str(uint8_t* rdram, uint32_t addr, const std::string& s,
                       size_t cap) {
    for (size_t i = 0; i < cap; ++i)
        psp_mem_write<uint8_t>(rdram, addr + i,
            i < s.size() && i + 1 < cap ? uint8_t(s[i]) : 0);
}

static std::string sd_space_text(uint64_t kb) {
    char b[16];
    if (kb < 1024) snprintf(b, sizeof b, "%llu KB", (unsigned long long)kb);
    else if (kb < 1024 * 1024)
        snprintf(b, sizeof b, "%llu MB", (unsigned long long)(kb / 1024));
    else snprintf(b, sizeof b, "%llu GB",
                  (unsigned long long)(kb / 1024 / 1024));
    return b;
}

static void sd_write_dt(uint8_t* rdram, uint32_t a, time_t t) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    psp_mem_write<uint16_t>(rdram, a + 0, uint16_t(tmv.tm_year + 1900));
    psp_mem_write<uint16_t>(rdram, a + 2, uint16_t(tmv.tm_mon + 1));
    psp_mem_write<uint16_t>(rdram, a + 4, uint16_t(tmv.tm_mday));
    psp_mem_write<uint16_t>(rdram, a + 6, uint16_t(tmv.tm_hour));
    psp_mem_write<uint16_t>(rdram, a + 8, uint16_t(tmv.tm_min));
    psp_mem_write<uint16_t>(rdram, a + 10, uint16_t(tmv.tm_sec));
    psp_mem_write<uint32_t>(rdram, a + 12, 0);
}

static uint32_t sd_clusters(uint64_t bytes) {
    return uint32_t((bytes + SD_CLUSTER - 1) / SD_CLUSTER);
}

// SceUtilitySavedataUsedDataInfo (28 bytes): s32 usedClusters,
// s32 usedSpaceKB, char usedSpaceStr[8], s32 usedSpace32KB,
// char usedSpace32Str[8].
static void sd_put_used(uint8_t* rdram, uint32_t a, uint32_t clusters) {
    uint32_t kb = clusters * (SD_CLUSTER / 1024);
    psp_mem_write<uint32_t>(rdram, a + 0, clusters);
    psp_mem_write<uint32_t>(rdram, a + 4, kb);
    sd_put_str(rdram, a + 8, sd_space_text(kb), 8);
    psp_mem_write<uint32_t>(rdram, a + 16, kb);
    sd_put_str(rdram, a + 20, sd_space_text(kb), 8);
}

static std::string sd_base() { return psp_path_to_host("ms0:/PSP/SAVEDATA"); }

// mkdir -p (a fresh memory-stick folder has no PSP/SAVEDATA yet).
static void sd_mkpath(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
            ::mkdir(path.substr(0, i).c_str(), 0755);
}

// ---- crypto helpers ----
static bool sd_has_key(uint8_t* rdram, uint32_t p) {
    for (int i = 0; i < 16; ++i)
        if (psp_mem_read<uint8_t>(rdram, p + SD_KEY + i)) return true;
    return false;
}
static void sd_get_key(uint8_t* rdram, uint32_t p, uint8_t out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = psp_mem_read<uint8_t>(rdram, p + SD_KEY + i);
}
// SceUtilitySavedataParam only carries secureVersion/key from the 1536-byte
// revision on; DetermineCryptMode treats smaller structs as mode 1.
static int sd_crypt_mode(uint8_t* rdram, uint32_t p) {
    uint32_t csize = psp_mem_read<uint32_t>(rdram, p);
    uint32_t secver = csize >= 1536 ? psp_mem_read<uint32_t>(rdram, p + SD_SECUREVER) : 0;
    return psp_sdcrypto::determine_crypt_mode(csize, secver,
                                              psp_kernel_compiled_sdk_version());
}
// secureVersion 0/2/3 need a key (PPSSPP SavedataParam::MissingRequiredKey).
static bool sd_missing_required_key(uint8_t* rdram, uint32_t p) {
    uint32_t csize = psp_mem_read<uint32_t>(rdram, p);
    if (csize < 1536) return false;
    return psp_mem_read<uint32_t>(rdram, p + SD_SECUREVER) != 1 &&
           !sd_has_key(rdram, p);
}

// SAVEDATA_FILE_LIST: 99 entries of {char name[13]; u8 hash[16]; u8 pad[3]}.
static constexpr size_t SD_FILELIST_ENTRY = 32, SD_FILELIST_COUNT = 99,
    SD_FILELIST_SIZE = SD_FILELIST_ENTRY * SD_FILELIST_COUNT;

static bool sd_read_sfo_map(const std::string& dir, SfoMap& m) {
    std::vector<uint8_t> v;
    return sd_read_file(dir + "/PARAM.SFO", v) && sfo_parse(v, m);
}

// Directory name = gameName + saveName; for LIST modes (4/5/6) the dialog
// stores its choice in g_sd_chosen_save (interactive) or, headless, we take
// the only existing save, else the first non-empty saveNameList entry,
// else saveName.
static std::string g_sd_chosen_save;

static std::vector<std::string> sd_savenamelist(uint8_t* rdram, uint32_t p) {
    std::vector<std::string> out;
    uint32_t list = psp_mem_read<uint32_t>(rdram, p + SD_SAVENAMELIST);
    if (!list || !sd_ram_ok(list, 20)) return out;
    for (uint32_t i = 0; i < 99 && sd_ram_ok(list + i * 20, 20); ++i) {
        std::string n = sd_cstr(rdram, list + i * 20, 20);
        if (n.empty()) break;
        out.push_back(n);
    }
    return out;
}

static std::string sd_resolve_dirname(uint8_t* rdram, uint32_t p,
                                      uint32_t mode) {
    std::string game = sd_cstr(rdram, p + SD_GAMENAME, 13);
    std::string save = sd_cstr(rdram, p + SD_SAVENAME, 20);
    if (mode == 4 || mode == 5 || mode == 6) {
        if (!g_sd_chosen_save.empty()) {
            save = g_sd_chosen_save;
        } else {
            auto names = sd_savenamelist(rdram, p);
            std::vector<std::string> existing;
            if (names.empty()) {
                std::string pat = game + "*";
                for (auto& n : sd_list_dir(sd_base()))
                    if (sd_glob(pat.c_str(), n.c_str()) &&
                        sd_is_dir(sd_base() + "/" + n))
                        existing.push_back(n.substr(std::min(n.size(), game.size())));
            } else {
                for (auto& n : names)
                    if (sd_is_dir(sd_base() + "/" + game + n)) existing.push_back(n);
            }
            if (existing.size() == 1) save = existing[0];
            else if (!names.empty()) save = names[0];
        }
    }
    return game + save;
}

static void sd_load_sfo(uint8_t* rdram, uint32_t p, const std::string& dir) {
    SfoMap m;
    if (!sd_read_sfo_map(dir, m)) return;
    uint32_t sfo = p + SD_SFO;
    sd_put_str(rdram, sfo, sfo_get_str(m, "TITLE"), 0x80);
    sd_put_str(rdram, sfo + 0x80, sfo_get_str(m, "SAVEDATA_TITLE"), 0x80);
    sd_put_str(rdram, sfo + 0x100, sfo_get_str(m, "SAVEDATA_DETAIL"), 0x400);
    auto it = m.find("PARENTAL_LEVEL");
    if (it != m.end() && it->second.data.size() >= 1)
        psp_mem_write<uint8_t>(rdram, sfo + 0x500, it->second.data[0]);
}

// secure = LOAD/AUTOLOAD/LISTLOAD/READDATASECURE (data file is encrypted
// when PARAM.SFO's SAVEDATA_PARAMS says so); plain reads pass through.
static int32_t savedata_do_load(uint8_t* rdram, uint32_t p,
                                const std::string& dir, bool with_extras,
                                bool secure) {
    std::string file = sd_cstr(rdram, p + SD_FILENAME, 13);
    uint32_t csize = psp_mem_read<uint32_t>(rdram, p);
    if (csize >= 1536 && psp_mem_read<uint32_t>(rdram, p + SD_SECUREVER) > 3)
        return int32_t(SD_ERR_LOAD_PARAM);
    if (secure && sd_missing_required_key(rdram, p))
        return int32_t(SD_ERR_LOAD_PARAM);
    std::vector<uint8_t> d;
    if (file.empty() || file.find('/') != std::string::npos ||
        !sd_read_file(dir + "/" + file, d))
        return int32_t(SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA);

    SfoMap sfo;
    bool have_sfo = sd_read_sfo_map(dir, sfo);
    int prev_mode = 0;
    uint8_t expected[16];
    bool have_expected = false;
    if (have_sfo) {
        auto it = sfo.find("SAVEDATA_PARAMS");
        if (it != sfo.end() && !it->second.data.empty())
            prev_mode = psp_sdcrypto::crypt_mode_from_params_flag(it->second.data[0]);
        auto fl = sfo.find("SAVEDATA_FILE_LIST");
        if (fl != sfo.end()) {
            const auto& b = fl->second.data;
            for (size_t o = 0; o + SD_FILELIST_ENTRY <= b.size() &&
                               o / SD_FILELIST_ENTRY < SD_FILELIST_COUNT; o += SD_FILELIST_ENTRY) {
                if (strncmp(reinterpret_cast<const char*>(&b[o]), file.c_str(), 13) == 0) {
                    memcpy(expected, &b[o + 13], 16);
                    have_expected = true;
                    break;
                }
            }
        }
    }
    if (secure && prev_mode != 0) {
        int detected = sd_crypt_mode(rdram, p);
        if (detected > 1 && !sd_has_key(rdram, p))
            return int32_t(SD_ERR_LOAD_PARAM);
        uint8_t key[16];
        sd_get_key(rdram, p, key);
        std::vector<uint8_t> plain;
        bool hash_ok = false;
        int r = psp_sdcrypto::decrypt_file(prev_mode, detected, d.data(), d.size(),
                                           key, have_expected ? expected : nullptr,
                                           plain, &hash_ok);
        if (r == 0) {
            static bool logged = false;
            if (!logged || !hash_ok) {
                fprintf(stderr, "[HLE] sceUtilitySavedata: decrypted %s (%zu -> %zu bytes, mode %d%s, hash %s)\n",
                        file.c_str(), d.size(), plain.size(), prev_mode,
                        prev_mode != detected ? "!=detected" : "",
                        hash_ok ? "ok" : (have_expected ? "MISMATCH" : "n/a"));
                logged = true;
            }
            d.swap(plain);
        } else {
            fprintf(stderr, "[HLE] sceUtilitySavedata: %s flagged encrypted but decrypt failed (%d); reading as plain\n",
                    file.c_str(), r);
        }
    }
    uint32_t buf = psp_mem_read<uint32_t>(rdram, p + SD_DATABUF);
    uint32_t cap = psp_mem_read<uint32_t>(rdram, p + SD_DATABUFSIZE);
    uint32_t n = uint32_t(std::min<size_t>(d.size(), cap));
    if (buf && sd_ram_ok(buf, n))
        for (uint32_t i = 0; i < n; ++i)
            psp_mem_write<uint8_t>(rdram, buf + i, d[i]);
    else n = 0;
    psp_mem_write<uint32_t>(rdram, p + SD_DATASIZE, n);
    if (with_extras) {
        sd_load_sfo(rdram, p, dir);
        psp_mem_write<uint32_t>(rdram, p + SD_BIND, 1021);  // PSP always reports this
        for (int k = 0; k < 4; ++k) {
            uint32_t a = p + SD_ICON_BASE + 16 * k;
            uint32_t ib = psp_mem_read<uint32_t>(rdram, a);
            uint32_t ic = psp_mem_read<uint32_t>(rdram, a + 4);
            std::vector<uint8_t> x;
            if (!ib || !sd_read_file(dir + "/" + SD_SYS_FILES[k], x)) continue;
            uint32_t m = uint32_t(std::min<size_t>(x.size(), ic));
            if (!sd_ram_ok(ib, m)) continue;
            for (uint32_t i = 0; i < m; ++i)
                psp_mem_write<uint8_t>(rdram, ib + i, x[i]);
            psp_mem_write<uint32_t>(rdram, a + 8, m);
        }
    }
    return 0;
}

static void sd_remove_dir(const std::string& dir) {
    for (auto& f : sd_list_dir(dir)) ::unlink((dir + "/" + f).c_str());
    ::rmdir(dir.c_str());
}

static uint64_t sd_dir_clusters(const std::string& dir) {
    uint64_t c = 0;
    for (auto& f : sd_list_dir(dir)) {
        struct stat st;
        if (::stat((dir + "/" + f).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            c += sd_clusters(uint64_t(st.st_size));
    }
    return c;
}

static int32_t savedata_do_sizes(uint8_t* rdram, uint32_t p,
                                 const std::string& dirname) {
    int32_t result = 0;
    uint32_t msfree = psp_mem_read<uint32_t>(rdram, p + SD_MSFREE);
    if (msfree && sd_ram_ok(msfree, 20)) {
        uint64_t free_clusters = (1ull << 30) / SD_CLUSTER;  // fallback 1GB
        struct statvfs sv;
        std::string root = psp_path_to_host("ms0:/");
        if (!root.empty() && statvfs(root.c_str(), &sv) == 0)
            free_clusters = uint64_t(sv.f_bavail) * sv.f_frsize / SD_CLUSTER;
        free_clusters = std::min<uint64_t>(free_clusters, 0x1FFFFF);
        uint32_t kb = uint32_t(free_clusters * (SD_CLUSTER / 1024));
        psp_mem_write<uint32_t>(rdram, msfree + 0, SD_CLUSTER);
        psp_mem_write<uint32_t>(rdram, msfree + 4, uint32_t(free_clusters));
        psp_mem_write<uint32_t>(rdram, msfree + 8, kb);
        sd_put_str(rdram, msfree + 12, sd_space_text(kb), 8);
    }
    uint32_t msdata = psp_mem_read<uint32_t>(rdram, p + SD_MSDATA);
    if (msdata && sd_ram_ok(msdata, 0x40)) {
        std::string g = sd_cstr(rdram, msdata, 13);
        std::string s = sd_cstr(rdram, msdata + 16, 20);
        std::string name = (g.empty() && s.empty()) ? dirname : g + s;
        std::string dir = sd_base() + "/" + name;
        if (!name.empty() && sd_is_dir(dir)) {
            sd_put_str(rdram, msdata, g, 13);
            sd_put_str(rdram, msdata + 16, s, 20);
            sd_put_used(rdram, msdata + 36, uint32_t(sd_dir_clusters(dir)));
        } else {
            result = int32_t(SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA);
        }
    } else if (!sd_is_dir(sd_base() + "/" + dirname)) {
        result = int32_t(SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA);
    }
    uint32_t ud = psp_mem_read<uint32_t>(rdram, p + SD_UTILDATA);
    if (ud && sd_ram_ok(ud, 28)) {
        // Space the request would need: data file + PARAM.SFO (0x1330)
        // + icon/pic/snd buffers, each rounded to clusters, + dir entry.
        uint64_t c = 1;
        c += sd_clusters(psp_mem_read<uint32_t>(rdram, p + SD_DATASIZE));
        c += sd_clusters(0x1330);
        for (int k = 0; k < 4; ++k)
            c += sd_clusters(psp_mem_read<uint32_t>(
                rdram, p + SD_ICON_BASE + 16 * k + 8));
        sd_put_used(rdram, ud, uint32_t(c));
    }
    return result;
}

static int32_t savedata_do_list(uint8_t* rdram, uint32_t p) {
    uint32_t il = psp_mem_read<uint32_t>(rdram, p + SD_IDLIST);
    if (!il || !sd_ram_ok(il, 12)) return 0;
    std::string game = sd_cstr(rdram, p + SD_GAMENAME, 13);
    std::string save = sd_cstr(rdram, p + SD_SAVENAME, 20);
    if (save.empty() || save == "<>") save = "*";
    std::string pat = game + save;
    uint32_t maxc = psp_mem_read<uint32_t>(rdram, il);
    uint32_t entries = psp_mem_read<uint32_t>(rdram, il + 8);
    std::string base = sd_base();
    uint32_t n = 0;
    for (auto& name : sd_list_dir(base)) {
        if (n >= maxc) break;
        if (!sd_glob(pat.c_str(), name.c_str())) continue;
        std::string dir = base + "/" + name;
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        uint32_t e = entries + n * 72;
        if (!sd_ram_ok(e, 72)) break;
        // SceUtilitySavedataIdListEntry: u32 st_mode, 3x ScePspDateTime
        // (ctime, atime, mtime), char name[20].
        psp_mem_write<uint32_t>(rdram, e, 0x11FF);
        sd_write_dt(rdram, e + 4, st.st_ctime);
        sd_write_dt(rdram, e + 20, st.st_atime);
        sd_write_dt(rdram, e + 36, st.st_mtime);
        sd_put_str(rdram, e + 52, name.substr(std::min(name.size(), game.size())), 20);
        ++n;
    }
    psp_mem_write<uint32_t>(rdram, il + 4, n);
    return 0;
}

// SceUtilitySavedataFileListInfo: u32 maxSecure, maxNormal, maxSystem,
// resultSecure, resultNormal, resultSystem, secureAddr, normalAddr,
// systemAddr. Entry (80 bytes): u32 st_mode, u32 st_attr, u64 st_size,
// 3x ScePspDateTime, char name[16].
static int32_t savedata_do_files(uint8_t* rdram, uint32_t p,
                                 const std::string& dir) {
    if (!sd_is_dir(dir)) return int32_t(SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA);
    uint32_t fl = psp_mem_read<uint32_t>(rdram, p + SD_FILELIST);
    if (!fl || !sd_ram_ok(fl, 36)) return 0;
    uint32_t maxn = psp_mem_read<uint32_t>(rdram, fl + 4);
    uint32_t maxs = psp_mem_read<uint32_t>(rdram, fl + 8);
    uint32_t naddr = psp_mem_read<uint32_t>(rdram, fl + 28);
    uint32_t saddr = psp_mem_read<uint32_t>(rdram, fl + 32);
    uint32_t nn = 0, ns = 0;
    for (auto& f : sd_list_dir(dir)) {
        struct stat st;
        if (::stat((dir + "/" + f).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        bool sys = f == "PARAM.SFO";
        for (auto s : SD_SYS_FILES) sys = sys || f == s;
        uint32_t& cnt = sys ? ns : nn;
        uint32_t lim = sys ? maxs : maxn;
        uint32_t e = (sys ? saddr : naddr) + cnt * 80;
        if (cnt >= lim || !sd_ram_ok(e, 80)) continue;
        psp_mem_write<uint32_t>(rdram, e, 0x21FF);
        psp_mem_write<uint32_t>(rdram, e + 4, 0x20);
        psp_mem_write<uint32_t>(rdram, e + 8, uint32_t(st.st_size));
        psp_mem_write<uint32_t>(rdram, e + 12, 0);
        sd_write_dt(rdram, e + 16, st.st_ctime);
        sd_write_dt(rdram, e + 32, st.st_atime);
        sd_write_dt(rdram, e + 48, st.st_mtime);
        sd_put_str(rdram, e + 64, f, 16);
        ++cnt;
    }
    psp_mem_write<uint32_t>(rdram, fl + 16, nn);
    psp_mem_write<uint32_t>(rdram, fl + 20, ns);
    return 0;
}

// secure = SAVE/AUTOSAVE/LISTSAVE/MAKEDATASECURE/WRITEDATASECURE: the data
// file is encrypted and PARAM.SFO signed (PPSSPP's default). sub_write =
// WRITEDATA(SECURE): update one file in an existing save.
static int32_t savedata_do_save(uint8_t* rdram, uint32_t p,
                                const std::string& dirname, bool secure,
                                bool sub_write) {
    std::string file = sd_cstr(rdram, p + SD_FILENAME, 13);
    std::string base = sd_base();
    if (base.empty() || dirname.empty()) return 0;
    std::string dir = base + "/" + dirname;
    sd_mkpath(base);
    ::mkdir(dir.c_str(), 0755);

    uint32_t buf = psp_mem_read<uint32_t>(rdram, p + SD_DATABUF);
    uint32_t sz = psp_mem_read<uint32_t>(rdram, p + SD_DATASIZE);
    bool have_data = !file.empty() && file.find('/') == std::string::npos &&
                     sd_ram_ok(buf, sz) && sz > 0 && sz < 0x1000000u;
    std::vector<uint8_t> data;
    if (have_data) {
        data.resize(sz);
        for (uint32_t i = 0; i < sz; ++i)
            data[i] = psp_mem_read<uint8_t>(rdram, buf + i);
    }

    // Encrypt the data file (PPSSPP EncryptData path).
    int crypt_mode = sd_crypt_mode(rdram, p);
    std::vector<uint8_t> file_bytes = data;
    uint8_t file_hash[16] = {0};
    bool encrypted = false;
    if (have_data && secure) {
        if (crypt_mode > 1 && !sd_has_key(rdram, p))
            return int32_t(SD_ERR_SAVE_PARAM);
        uint8_t key[16];
        sd_get_key(rdram, p, key);
        std::vector<uint8_t> enc;
        if (psp_sdcrypto::encrypt_file(crypt_mode, data.data(), data.size(),
                                       crypt_mode > 1 ? key : nullptr, enc,
                                       file_hash) == 0) {
            file_bytes.swap(enc);
            encrypted = true;
        } else {
            fprintf(stderr, "[HLE] sceUtilitySavedata: save encryption failed; writing plain\n");
            memset(file_hash, 0, sizeof file_hash);
        }
    }

    // PARAM.SFO: start from the existing file, update fields (PPSSPP only
    // overwrites the title block when the game supplied a title).
    SfoMap sfo;
    bool had_sfo = sd_read_sfo_map(dir, sfo);
    bool was_crypted = false;
    if (had_sfo) {
        auto it = sfo.find("SAVEDATA_PARAMS");
        was_crypted = it != sfo.end() && !it->second.data.empty() &&
                      it->second.data[0] != 0;
    }
    std::string title = sd_cstr(rdram, p + SD_SFO, 0x80);
    if (!title.empty() || !sfo.count("TITLE")) {
        sfo_set_str(sfo, "TITLE", title, 0x80);
        sfo_set_str(sfo, "SAVEDATA_TITLE", sd_cstr(rdram, p + SD_SFO + 0x80, 0x80), 0x80);
        sfo_set_str(sfo, "SAVEDATA_DETAIL", sd_cstr(rdram, p + SD_SFO + 0x100, 0x400), 0x400);
        sfo_set_int(sfo, "PARENTAL_LEVEL", psp_mem_read<uint8_t>(rdram, p + SD_SFO + 0x500));
        sfo_set_str(sfo, "CATEGORY", "MS", 4);
        sfo_set_str(sfo, "SAVEDATA_DIRECTORY", dirname, 0x40);
    }
    // File list (name + MAC per secure file).
    std::vector<uint8_t> fl(SD_FILELIST_SIZE, 0);
    if (sfo.count("SAVEDATA_FILE_LIST")) {
        const auto& old = sfo["SAVEDATA_FILE_LIST"].data;
        memcpy(fl.data(), old.data(), std::min(old.size(), fl.size()));
    }
    if (secure && have_data) {
        for (size_t i = 0; i < SD_FILELIST_COUNT; ++i) {
            uint8_t* e = &fl[i * SD_FILELIST_ENTRY];
            if (e[0] != '\0' && strncmp(reinterpret_cast<char*>(e), file.c_str(), 13) != 0)
                continue;
            memset(e, 0, 13);
            memcpy(e, file.c_str(), std::min<size_t>(file.size(), 12));
            memcpy(e + 13, file_hash, 16);
            break;
        }
    }
    sfo_set_bin(sfo, "SAVEDATA_FILE_LIST", fl, uint32_t(SD_FILELIST_SIZE));
    sfo_set_bin(sfo, "SAVEDATA_PARAMS", std::vector<uint8_t>(128, 0), 128);
    size_t params_off = SIZE_MAX;
    std::vector<uint8_t> sfo_bytes = sfo_write(sfo, &params_off);
    if ((encrypted || (sub_write && was_crypted)) && params_off != SIZE_MAX) {
        int r = psp_sdcrypto::update_sfo_hash(sfo_bytes, params_off, crypt_mode);
        if (r < 0)
            fprintf(stderr, "[HLE] sceUtilitySavedata: PARAM.SFO signing failed (%d)\n", r);
    }
    sd_write_file(dir + "/PARAM.SFO", sfo_bytes.data(), sfo_bytes.size());

    if (have_data)
        sd_write_file(dir + "/" + file, file_bytes.data(), file_bytes.size());
    for (int k = 0; k < 4; ++k) {
        uint32_t a = p + SD_ICON_BASE + 16 * k;
        uint32_t ibuf = psp_mem_read<uint32_t>(rdram, a);
        uint32_t isz = psp_mem_read<uint32_t>(rdram, a + 8);
        if (sd_ram_ok(ibuf, isz) && isz > 0 && isz < 0x100000u) {
            std::vector<uint8_t> d(isz);
            for (uint32_t i = 0; i < isz; ++i)
                d[i] = psp_mem_read<uint8_t>(rdram, ibuf + i);
            sd_write_file(dir + "/" + SD_SYS_FILES[k], d.data(), d.size());
        }
    }
    fprintf(stderr, "[HLE] sceUtilitySavedata: saved %s/%s (%u bytes%s)\n",
            dirname.c_str(), file.c_str(), sz,
            encrypted ? psp_sdcrypto::crypt_mode_has_key(crypt_mode) ? ", encrypted+key" : ", encrypted" : "");
    return 0;
}

static int32_t savedata_dispatch(uint8_t* rdram, uint32_t p, uint32_t mode) {
    static bool logged[32];
    std::string dirname = sd_resolve_dirname(rdram, p, mode);
    std::string dir = sd_base() + "/" + dirname;
    std::string file = sd_cstr(rdram, p + SD_FILENAME, 13);
    if (mode < 32 && !logged[mode]) {
        logged[mode] = true;
        fprintf(stderr, "[HLE] sceUtilitySavedata mode=%u dir='%s' -> %s\n",
                mode, dirname.c_str(), dir.c_str());
    }
    switch (mode) {
        case 0: case 2: case 4:  // AUTOLOAD / LOAD / LISTLOAD
            return savedata_do_load(rdram, p, dir, true, true);
        case 1: case 3: case 5:  // AUTOSAVE / SAVE / LISTSAVE (secure)
        case 13:                 // MAKEDATASECURE
            return savedata_do_save(rdram, p, dirname, true, false);
        case 14:                 // MAKEDATA (not encrypted)
            return savedata_do_save(rdram, p, dirname, false, false);
        case 6: case 7: case 9: case 10:  // *DELETE
            if (!sd_is_dir(dir)) return int32_t(SD_ERR_DELETE_NO_DATA);
            sd_remove_dir(dir);
            return 0;
        case 8:
            return savedata_do_sizes(rdram, p, dirname);
        case 11:
            return savedata_do_list(rdram, p);
        case 12:
            return savedata_do_files(rdram, p, dir);
        case 15: case 16: {  // READDATA(SECURE)
            if (!sd_is_dir(dir))
                return int32_t(SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA);
            if (savedata_do_load(rdram, p, dir, false, mode == 16) != 0)
                return int32_t(SD_ERR_RW_FILE_NOT_FOUND);
            return 0;
        }
        case 17: case 18: {  // WRITEDATA / WRITEDATASECURE
            if (!sd_is_dir(dir))
                return int32_t(SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA);
            return savedata_do_save(rdram, p, dirname, mode == 18, true);
        }
        case 19: case 20: case 21: {  // ERASE(SECURE) / DELETEDATA
            if (!sd_is_dir(dir))
                return int32_t(mode == 21 ? SD_ERR_DELETE_NO_DATA
                    : SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA);
            if (file.empty() || file.find('/') != std::string::npos ||
                ::unlink((dir + "/" + file).c_str()) != 0)
                return int32_t(mode == 21 ? SD_ERR_DELETE_NO_DATA
                                          : SD_ERR_RW_FILE_NOT_FOUND);
            return 0;
        }
        default:  // GETSIZE (22) and unknown: struct layout not modelled
            return 0;
    }
}

// ---- Interactive LISTLOAD / LISTSAVE dialog ----
// Reads the pad like sceCtrl does (host keys + debug-socket injection).
static uint32_t sd_pad_buttons() {
    uint32_t b = g_host_buttons.load(std::memory_order_relaxed);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms < g_injected_buttons_deadline_ms.load(std::memory_order_relaxed))
        b |= g_injected_buttons.load(std::memory_order_relaxed);
    return b;
}

static bool sd_auto_mode() {
    const char* e = std::getenv("PSPRECOMP_SAVEDATA_AUTO");
    return e && *e && std::strcmp(e, "0") != 0;
}

struct SavedataUi {
    int stage = 0;               // 0 = not started, 1 = dialog running
    psp_sddialog::ListDialog dlg;
    // What was last published: re-render only when the dialog state or the
    // output scale changes (the canvas is up to 1920x1088).
    uint32_t pub_rev = 0;
    int pub_scale = 0;
    psp_sddialog::Canvas canvas;
};
static SavedataUi g_sd_ui;

// Rows for the list: LISTLOAD = existing saves; LISTSAVE = every slot of
// saveNameList (empty ones are "New Save"), else existing + one new slot.
static std::vector<psp_sddialog::SaveEntry> sd_collect_entries(
    uint8_t* rdram, uint32_t p, bool load) {
    std::string game = sd_cstr(rdram, p + SD_GAMENAME, 13);
    std::string base = sd_base();
    auto names = sd_savenamelist(rdram, p);
    if (names.empty()) {
        std::string pat = game + "*";
        for (auto& n : sd_list_dir(base))
            if (sd_glob(pat.c_str(), n.c_str()) && sd_is_dir(base + "/" + n))
                names.push_back(n.substr(std::min(n.size(), game.size())));
        if (!load) {
            std::string cur = sd_cstr(rdram, p + SD_SAVENAME, 20);
            if (cur.empty() || cur == "<>") cur = "0000";
            if (std::find(names.begin(), names.end(), cur) == names.end())
                names.push_back(cur);
        }
    }
    std::vector<psp_sddialog::SaveEntry> out;
    for (auto& slot : names) {
        psp_sddialog::SaveEntry e;
        e.slot = slot;
        e.dirname = game + slot;
        std::string dir = base + "/" + e.dirname;
        e.exists = sd_is_dir(dir);
        if (e.exists) {
            SfoMap m;
            if (sd_read_sfo_map(dir, m)) {
                e.title = sfo_get_str(m, "TITLE");
                e.savetitle = sfo_get_str(m, "SAVEDATA_TITLE");
                e.detail = sfo_get_str(m, "SAVEDATA_DETAIL");
            }
            struct stat st;
            if (::stat((dir + "/PARAM.SFO").c_str(), &st) == 0 ||
                ::stat(dir.c_str(), &st) == 0)
                e.mtime = st.st_mtime;
            // Total size of the save's files (the PSP list shows it in KB).
            for (auto& fn : sd_list_dir(dir)) {
                struct stat fs;
                if (::stat((dir + "/" + fn).c_str(), &fs) == 0 && S_ISREG(fs.st_mode))
                    e.size_bytes += uint64_t(fs.st_size);
            }
            // ICON0.PNG thumbnail (144x80 on the PSP; any plain PNG is accepted).
            std::vector<uint8_t> png;
            if (sd_read_file(dir + "/ICON0.PNG", png) && !png.empty()) {
                std::vector<uint8_t> px;
                int iw = 0, ih = 0;
                if (psp_png_decode_rgba(png.data(), png.size(), px, iw, ih, nullptr) &&
                    iw > 0 && ih > 0 && iw <= 512 && ih <= 512) {
                    e.icon_w = iw;
                    e.icon_h = ih;
                    e.icon_rgba = std::move(px);
                }
            }
        } else if (load) {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

// One step of the dialog; returns true when the utility call is finished
// (g_sd_chosen_save / *cancelled set). Called from Update and GetStatus.
static bool sd_ui_step(uint8_t* rdram, uint32_t p, uint32_t mode,
                       bool* cancelled, bool* no_data) {
    using namespace psp_sddialog;
    *cancelled = false;
    *no_data = false;
    bool load = mode == 4;
    if (g_sd_ui.stage == 0) {
        auto entries = sd_collect_entries(rdram, p, load);
        if (entries.empty()) {
            *no_data = true;   // nothing to pick: PSP shows "no data"
            return true;
        }
        bool confirm_cross = psp_mem_read<int32_t>(rdram, p + 8) == 1;  // common.buttonSwap
        if (const char* e = std::getenv("PSPRECOMP_SAVEDATA_CONFIRM"))
            confirm_cross = std::strcmp(e, "circle") != 0;
        int initial = 0;
        std::string cur = sd_cstr(rdram, p + SD_SAVENAME, 20);
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].slot == cur) initial = int(i);
        g_sd_ui.dlg.begin(load ? Kind::Load : Kind::Save, std::move(entries),
                          confirm_cross, initial);
        g_sd_ui.stage = 1;
        g_sd_ui.pub_scale = 0;
        fprintf(stderr, "[HLE] sceUtilitySavedata: list dialog opened (%zu rows, %s confirms)\n",
                g_sd_ui.dlg.entries().size(), confirm_cross ? "CROSS" : "CIRCLE");
    }
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    Result r = g_sd_ui.dlg.update(sd_pad_buttons(), now_ms);
    if (r == Result::Running) {
        const int scale = output_scale();
        if (scale != g_sd_ui.pub_scale || g_sd_ui.dlg.revision() != g_sd_ui.pub_rev) {
            g_sd_ui.canvas = Canvas(scale);
            g_sd_ui.dlg.render(g_sd_ui.canvas);
            // blitted over the frame by present_dialog_overlay() in psp_ge_draw.cpp
            overlay_publish(g_sd_ui.canvas);
            g_sd_ui.pub_scale = scale;
            g_sd_ui.pub_rev = g_sd_ui.dlg.revision();
        }
        return false;
    }
    overlay_clear();
    g_sd_ui.stage = 0;
    if (r == Result::Cancelled) {
        *cancelled = true;
        return true;
    }
    const SaveEntry* e = g_sd_ui.dlg.selected_entry();
    g_sd_chosen_save = e ? e->slot : "";
    // PSP copies the chosen name back into the request.
    sd_put_str(rdram, p + SD_SAVENAME, g_sd_chosen_save, 20);
    return true;
}

static void savedata_complete(UtilityDialogState& dlg, uint8_t* rdram) {
    if (dlg.param_addr == 0) {
        return;
    }
    uint32_t mode = psp_mem_read<uint32_t>(
        rdram, dlg.param_addr + SAVEDATA_MODE_OFFSET);
    int32_t result;
    if ((mode == 4 || mode == 5) && !sd_auto_mode()) {
        bool cancelled = false, no_data = false;
        if (!sd_ui_step(rdram, dlg.param_addr, mode, &cancelled, &no_data)) {
            dlg.pending = true;     // keep reporting RUNNING until the user decides
            return;
        }
        if (cancelled) {
            result = 1;             // SCE_UTILITY_DIALOG_RESULT_CANCEL
            fprintf(stderr, "[HLE] sceUtilitySavedata: list dialog cancelled\n");
        } else if (no_data) {
            result = int32_t(SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA);
        } else {
            result = savedata_dispatch(rdram, dlg.param_addr, mode);
        }
        g_sd_chosen_save.clear();
    } else {
        result = savedata_dispatch(rdram, dlg.param_addr, mode);
    }
    psp_mem_write<int32_t>(
        rdram, dlg.param_addr + UTILITY_COMMON_RESULT_OFFSET, result);
    fprintf(stderr,
            "[HLE] sceUtilitySavedata complete: mode=%u result=0x%08X\n",
            mode, static_cast<uint32_t>(result));
}

static void msgdialog_complete(UtilityDialogState& dlg, uint8_t* rdram) {
    if (dlg.param_addr == 0) {
        return;
    }
    psp_mem_write<int32_t>(
        rdram, dlg.param_addr + UTILITY_COMMON_RESULT_OFFSET, 0);
}

static void hle_sceUtilitySavedataInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_init_start(g_savedata_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilitySavedataGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_get_status(g_savedata_dialog, rdram, ctx,
                              savedata_complete);
}

static void hle_sceUtilitySavedataShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_shutdown_start(g_savedata_dialog, ctx);
    psp_sddialog::overlay_clear();
    g_sd_ui.stage = 0;
    (void)rdram;
}

static void hle_sceUtilitySavedataUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    // The list dialog is driven from GetStatus (the game polls it every
    // frame while RUNNING); Update only validates the status.
    utility_dialog_update(g_savedata_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityMsgDialogInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_init_start(g_msg_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityMsgDialogGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_get_status(g_msg_dialog, rdram, ctx,
                              msgdialog_complete);
}

static void hle_sceUtilityMsgDialogShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_shutdown_start(g_msg_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityMsgDialogUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_update(g_msg_dialog, ctx);
    (void)rdram;
}

// ---- Headless auto-OSK (sceUtilityOsk*) ----
// SceUtilityOskParams (pspsdk psputility_osk.h): 48-byte common header,
// +0x30 s32 datacount, +0x34 SceUtilityOskData* data, +0x38 s32 state,
// +0x3C s32 unk. SceUtilityOskData: +0x00 unk, +0x04 unk, +0x08 language,
// +0x0C unk, +0x10 inputtype, +0x14 lines, +0x18 unk, +0x1C u16* desc,
// +0x20 u16* intext, +0x24 s32 outtextlength, +0x28 u16* outtext,
// +0x2C s32 result, +0x30 s32 outtextlimit.
// There is no keyboard UI: after InitStart the dialog reports INITIALIZE,
// then RUNNING for OSK_RUNNING_TICKS Update/GetStatus polls (so the game's
// own dialog handling runs), then completes with PSPRECOMP_OSK_TEXT, else
// the initial text, else "PLAYER". Status machine = the shared one above.
static constexpr uint32_t OSK_DATACOUNT_OFFSET = 0x30;
static constexpr uint32_t OSK_DATA_OFFSET = 0x34;
static constexpr uint32_t OSKDATA_INTEXT_OFFSET = 0x20;
static constexpr uint32_t OSKDATA_OUTLEN_OFFSET = 0x24;
static constexpr uint32_t OSKDATA_OUTTEXT_OFFSET = 0x28;
static constexpr uint32_t OSKDATA_RESULT_OFFSET = 0x2C;
static constexpr uint32_t OSKDATA_OUTLIMIT_OFFSET = 0x30;
// pspsdk PspUtilityOskResult
static constexpr int32_t OSK_RESULT_UNCHANGED = 0;
static constexpr int32_t OSK_RESULT_CANCELLED = 1;
static constexpr int32_t OSK_RESULT_CHANGED = 2;
static constexpr int OSK_RUNNING_TICKS = 4;

static UtilityDialogState g_osk_dialog;
static int g_osk_ticks = 0;

static bool osk_ptr_ok(uint32_t a) {
    return a >= 0x08000000u && a < 0x0C000000u;
}

static void osk_complete(UtilityDialogState& dlg, uint8_t* rdram) {
    if (dlg.param_addr == 0) {
        return;
    }
    uint32_t params = dlg.param_addr;
    uint32_t count = psp_mem_read<uint32_t>(rdram, params + OSK_DATACOUNT_OFFSET);
    uint32_t data = psp_mem_read<uint32_t>(rdram, params + OSK_DATA_OFFSET);
    std::string text;
    int32_t result = OSK_RESULT_CHANGED;
    if (count >= 1 && osk_ptr_ok(data)) {
        uint32_t intext = psp_mem_read<uint32_t>(rdram, data + OSKDATA_INTEXT_OFFSET);
        uint32_t outtext = psp_mem_read<uint32_t>(rdram, data + OSKDATA_OUTTEXT_OFFSET);
        int32_t limit = psp_mem_read<int32_t>(rdram, data + OSKDATA_OUTLIMIT_OFFSET);
        std::u16string in16;
        if (osk_ptr_ok(intext)) {
            for (int i = 0; i < 512; ++i) {
                uint16_t c = psp_mem_read<uint16_t>(rdram, intext + i * 2);
                if (c == 0) break;
                in16.push_back(static_cast<char16_t>(c));
            }
        }
        std::u16string out16;
        const char* env = std::getenv("PSPRECOMP_OSK_TEXT");
        if (env && *env) {
            for (const char* p = env; *p; ++p)
                out16.push_back(static_cast<char16_t>(
                    static_cast<unsigned char>(*p)));  // Latin-1/ASCII
        } else if (!in16.empty()) {
            out16 = in16;
        } else {
            out16 = u"PLAYER";
        }
        // outtextlimit counts u16 units; keep room for the terminator.
        size_t max_chars = limit > 1 ? static_cast<size_t>(limit) - 1 : 0;
        if (limit <= 0) max_chars = 31;
        if (out16.size() > max_chars) out16.resize(max_chars);
        result = (out16 == in16) ? OSK_RESULT_UNCHANGED : OSK_RESULT_CHANGED;
        if (osk_ptr_ok(outtext)) {
            for (size_t i = 0; i < out16.size(); ++i) {
                psp_mem_write<uint16_t>(rdram, outtext + i * 2, out16[i]);
                text.push_back(out16[i] < 0x80 ? static_cast<char>(out16[i]) : '?');
            }
            psp_mem_write<uint16_t>(rdram, outtext + out16.size() * 2, 0);
        }
        psp_mem_write<int32_t>(rdram, data + OSKDATA_OUTLEN_OFFSET,
                               static_cast<int32_t>(out16.size()));
        psp_mem_write<int32_t>(rdram, data + OSKDATA_RESULT_OFFSET, result);
    }
    psp_mem_write<int32_t>(
        rdram, params + UTILITY_COMMON_RESULT_OFFSET, 0);
    fprintf(stderr, "[HLE] sceUtilityOsk: returning \"%s\"\n", text.c_str());
}

static void hle_sceUtilityOskInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_init_start(g_osk_dialog, ctx);
    g_osk_ticks = 0;
    (void)rdram;
}

static void hle_sceUtilityOskGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    // Hold RUNNING (without completing) until enough polls happened.
    if (g_osk_dialog.status == PSP_UTILITY_STATUS_RUNNING &&
        g_osk_ticks < OSK_RUNNING_TICKS) {
        ++g_osk_ticks;
        ctx->r[2] = PSP_UTILITY_STATUS_RUNNING;
        return;
    }
    utility_dialog_get_status(g_osk_dialog, rdram, ctx, osk_complete);
}

static void hle_sceUtilityOskShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_shutdown_start(g_osk_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityOskUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    if (g_osk_dialog.status == PSP_UTILITY_STATUS_RUNNING) {
        ++g_osk_ticks;
    }
    utility_dialog_update(g_osk_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = PSP_UTILITY_STATUS_NONE;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- MsgPipe (Kernel message pipes) ----
// PSP MsgPipes are byte streams with no message boundaries
// (PPSSPP Core/HLE/sceKernelMsgPipe.cpp semantics). Patapon's
// sound-stream manager runs a command bus over one: the main
// thread Sends 84-byte commands, the sound thread TryReceives
// 8-byte headers. Producer and consumer are different OS threads,
// so the map and FIFOs are mutex-guarded.

struct PspMsgPipe {
    std::deque<uint8_t> fifo;
    uint32_t buf_size = 0;
};

static std::mutex g_msgpipe_mtx;
static std::unordered_map<int, PspMsgPipe> g_msgpipes;

// waitMode values (PPSSPP SCE_KERNEL_MPW_*):
// FULL(0) = all-or-nothing, ASAP(1) = partial transfer ok
static constexpr uint32_t PSP_MPW_ASAP = 1;

static void hle_sceKernelCreateMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0=name, a1=part, a2=attr, a3=bufSize, t0=opt
    uint32_t buf_size = static_cast<uint32_t>(ctx->r[7]);
    int uid = psp_next_uid();
    {
        std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
        g_msgpipes[uid].buf_size = buf_size;
    }
    ctx->r[2] = uid;
    (void)rdram;
}

static void hle_sceKernelDeleteMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = static_cast<int>(ctx->r[4]);
    std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
    g_msgpipes.erase(uid);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// Blocking send (NID 0x876DBFAD). Copies the message into the
// FIFO; if the pipe is full, bounded-polls for space (the game
// sends 84 bytes into a 1024-byte pipe, so this never blocks in
// practice).
static void hle_sceKernelSendMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = static_cast<int>(ctx->r[4]);
    uint32_t msg_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t size = static_cast<uint32_t>(ctx->r[6]);
    uint32_t result_ptr = static_cast<uint32_t>(ctx->r[8]);
    // r[7]=waitMode, r[9]=timeout*: send completes immediately
    // unless the pipe is full.

    sched_yield_point();

    for (int spins = 0; ; ++spins) {
        {
            std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
            auto it = g_msgpipes.find(uid);
            if (it == g_msgpipes.end()) {
                ctx->r[2] = SCE_KERNEL_ERROR_UNKNOWN_MPPID;
                return;
            }
            PspMsgPipe& mp = it->second;
            if (size > mp.buf_size) {
                ctx->r[2] = SCE_KERNEL_ERROR_ILLEGAL_SIZE;
                return;
            }
            if (mp.fifo.size() + size <= mp.buf_size) {
                for (uint32_t i = 0; i < size; ++i) {
                    mp.fifo.push_back(psp_mem_read<uint8_t>(
                        rdram, msg_ptr + i));
                }
                if (result_ptr != 0) {
                    psp_mem_write<uint32_t>(rdram, result_ptr,
                                            size);
                }
                ctx->r[2] = SCE_OK;
                return;
            }
        }
        // Pipe full: bounded poll (~5s) for the consumer to drain.
        if (g_should_exit.load() || spins >= 5000) {
            ctx->r[2] = SCE_KERNEL_ERROR_MPP_FULL;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Non-blocking receive (NID 0xDF52098F).
// waitMode FULL(0): all-or-nothing — if fewer than `size` bytes
// are buffered, return MPP_EMPTY without consuming anything.
// waitMode ASAP(1): pop min(available, size) bytes.
static void hle_sceKernelTryReceiveMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = static_cast<int>(ctx->r[4]);
    uint32_t buf_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t size = static_cast<uint32_t>(ctx->r[6]);
    uint32_t wait_mode = static_cast<uint32_t>(ctx->r[7]);
    uint32_t result_ptr = static_cast<uint32_t>(ctx->r[8]);

    std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
    auto it = g_msgpipes.find(uid);
    if (it == g_msgpipes.end()) {
        ctx->r[2] = SCE_KERNEL_ERROR_UNKNOWN_MPPID;
        return;
    }
    PspMsgPipe& mp = it->second;
    uint32_t avail = static_cast<uint32_t>(mp.fifo.size());

    uint32_t count;
    if (wait_mode == PSP_MPW_ASAP) {
        count = std::min(avail, size);
        if (count == 0 && size != 0) {
            ctx->r[2] = SCE_KERNEL_ERROR_MPP_EMPTY;
            return;
        }
    } else {  // FULL(0)
        if (avail < size) {
            ctx->r[2] = SCE_KERNEL_ERROR_MPP_EMPTY;
            return;
        }
        count = size;
    }

    for (uint32_t i = 0; i < count; ++i) {
        psp_mem_write<uint8_t>(rdram, buf_ptr + i, mp.fifo.front());
        mp.fifo.pop_front();
    }
    if (result_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, result_ptr, count);
    }
    ctx->r[2] = SCE_OK;
}

// sceLibFont lives in psp_hle_font.cpp (FreeType-backed).

// ---- Registration ----

void psp_hle_register_utility() {
    // Initialize boot module tracking
    init_boot_module_tracking();

    // Cache operations
    psp_hle_register("sceKernelDcacheWritebackAll",
                      hle_sceKernelDcacheWritebackAll);
    // Cache maintenance is a no-op on a recompiled binary (no caches to
    // keep coherent); the instruction-cache variants matter to overlay
    // loaders (Dissidia) only as a signal, which the overlay resolver
    // doesn't need. sceKernelGetGPI: no debug GPI switches set.
    psp_hle_register("sceKernelIcacheInvalidateRange", hle_ret0);
    psp_hle_register("sceKernelIcacheInvalidateAll", hle_ret0);
    psp_hle_register("sceKernelDcacheInvalidateRange", hle_ret0);
    psp_hle_register("sceKernelDcacheWritebackInvalidateRange", hle_ret0);
    psp_hle_register("sceKernelGetGPI", hle_ret0);
    psp_hle_register("sceKernelVolatileMemLock", hle_sceKernelVolatileMemLock);
    psp_hle_register("sceKernelVolatileMemTryLock", hle_sceKernelVolatileMemTryLock);
    psp_hle_register("sceKernelVolatileMemUnlock", hle_sceKernelVolatileMemUnlock);
    psp_hle_register("sceKernelSetCompiledSdkVersion603_605", hle_ret0);
    psp_hle_register("sceKernelDcacheWritebackInvalidateAll",
                      hle_sceKernelDcacheWritebackInvalidateAll);
    psp_hle_register("sceKernelDcacheWritebackRange",
                      hle_sceKernelDcacheWritebackRange);

    // RTC
    psp_hle_register("sceRtcGetAccumulativeTime",
                      hle_sceRtcGetAccumulativeTime);
    psp_hle_register("sceNpDrmSetLicenseeKey", hle_ret0);  // SCE_OK no-op
    psp_hle_register("sceRtcGetTick", hle_sceRtcGetTick);
    psp_hle_register("sceRtcGetTickResolution", hle_sceRtcGetTickResolution);
    psp_hle_register("sceRtcSetTick", hle_sceRtcSetTick);
    psp_hle_register("sceRtcTickAddSeconds", hle_sceRtcTickAddSeconds);
    psp_hle_register("sceRtcTickAddDays", hle_sceRtcTickAddDays);
    psp_hle_register("sceRtcGetDayOfWeek", hle_sceRtcGetDayOfWeek);
    psp_hle_register("sceKernelMemcpy", hle_sceKernelMemcpy);
    psp_hle_register("sceKernelMemset", hle_sceKernelMemset);
    psp_hle_register("sceDmacMemcpy", hle_sceDmacMemcpy);
    psp_hle_register("sceRtcGetCurrentClockLocalTime",
                      hle_sceRtcGetCurrentClockLocalTime);
    psp_hle_register("sceKernelLibcTime",
                      hle_sceKernelLibcTime);
    psp_hle_register("sceKernelLibcClock",
                      hle_sceKernelLibcClock);
    psp_hle_register("sceKernelLibcGettimeofday",
                      hle_sceKernelLibcGettimeofday);

    // Audio
    psp_hle_register("sceAudioOutputBlocking",
                      hle_sceAudioOutputBlocking);
    psp_hle_register("sceAudioOutputPannedBlocking",
                      hle_sceAudioOutputPannedBlocking);
    psp_hle_register("sceAudioChReserve",
                      hle_sceAudioChReserve);
    psp_hle_register("sceAudioChRelease",
                      hle_sceAudioChRelease);
    psp_hle_register("sceAudioOutput2Reserve",
                      hle_sceAudioOutput2Reserve);
    psp_hle_register("sceAudioOutput2OutputBlocking",
                      hle_sceAudioOutput2OutputBlocking);
    psp_hle_register("sceAudioOutput2Release",
                      hle_sceAudioOutput2Release);
    psp_hle_register("sceAudioOutputPanned",
                      hle_sceAudioOutputPanned);
    psp_hle_register("sceAudioOutput", hle_sceAudioOutput);
    psp_hle_register("sceAudioChangeChannelConfig",
                      hle_sceAudioChangeChannelConfig);
    psp_hle_register("sceAudioGetChannelRestLength",
                      hle_sceAudioGetChannelRestLength);
    psp_hle_register("sceAudioChangeChannelVolume",
                      hle_sceAudioChangeChannelVolume);
    psp_hle_register("sceAudioSetChannelDataLen",
                      hle_sceAudioSetChannelDataLen);

    // ATRAC3plus

    // Module management
    psp_hle_register("sceKernelLoadModule",
                      hle_sceKernelLoadModule);
    psp_hle_register("sceKernelStartModule",
                      hle_sceKernelStartModule);
    psp_hle_register("sceKernelStopModule",
                      hle_sceKernelStopModule);
    psp_hle_register("sceKernelUnloadModule",
                      hle_sceKernelUnloadModule);
    psp_hle_register(
        "sceKernelStopUnloadSelfModuleWithStatus",
        hle_sceKernelStopUnloadSelfModuleWithStatus);
    psp_hle_register("sceKernelGetModuleIdByAddress",
                      hle_sceKernelGetModuleIdByAddress);
    psp_hle_register("sceKernelGetModuleId",
                      hle_sceKernelGetModuleId);
    psp_hle_register("sceKernelQueryModuleInfo",
                      hle_sceKernelQueryModuleInfo);

    // UMD
    psp_hle_register("sceUmdCheckMedium",
                      hle_sceUmdCheckMedium);
    psp_hle_register("sceUmdGetDriveStat",
                      hle_sceUmdGetDriveStat);
    psp_hle_register("sceUmdActivate",
                      hle_sceUmdActivate);
    psp_hle_register("sceUmdWaitDriveStat",
                      hle_sceUmdWaitDriveStat);
    psp_hle_register("sceUmdWaitDriveStatCB",
                      hle_sceUmdWaitDriveStatCB);
    psp_hle_register("sceUmdGetErrorStat",
                      hle_sceUmdGetErrorStat);
    psp_hle_register("sceUmdRegisterUMDCallBack",
                      hle_sceUmdRegisterUMDCallBack);
    psp_hle_register("sceUmdUnRegisterUMDCallBack",
                      hle_sceUmdUnRegisterUMDCallBack);

    // Networking
    psp_hle_register("sceNetInit", hle_sceNetInit);
    psp_hle_register("sceNetTerm", hle_sceNetTerm);
    psp_hle_register("sceNetInetInit", hle_sceNetInetInit);
    psp_hle_register("sceNetInetTerm", hle_sceNetInetTerm);
    psp_hle_register("sceNetApctlInit", hle_sceNetApctlInit);
    psp_hle_register("sceNetApctlTerm", hle_sceNetApctlTerm);
    psp_hle_register("sceNetResolverInit",
                      hle_sceNetResolverInit);
    psp_hle_register("sceNetResolverTerm",
                      hle_sceNetResolverTerm);

    // HTTP/SSL
    psp_hle_register("sceHttpInit", hle_sceHttpInit);
    psp_hle_register("sceHttpEnd", hle_sceHttpEnd);
    psp_hle_register("sceHttpsInit", hle_sceHttpsInit);
    psp_hle_register("sceHttpsEnd", hle_sceHttpsEnd);
    psp_hle_register("sceHttpsLoadDefaultCert",
                      hle_sceHttpsLoadDefaultCert);
    psp_hle_register("sceHttpSaveSystemCookie",
                      hle_sceHttpSaveSystemCookie);
    psp_hle_register("sceHttpLoadSystemCookie",
                      hle_sceHttpLoadSystemCookie);
    psp_hle_register("sceSslInit", hle_sceSslInit);
    psp_hle_register("sceSslEnd", hle_sceSslEnd);

    // MPEG

    // PSMF
    psp_hle_register("scePsmfSetPsmf",
                      hle_scePsmfSetPsmf);
    psp_hle_register("scePsmfGetVideoInfo",
                      hle_scePsmfGetVideoInfo);
    psp_hle_register("scePsmfSpecifyStream",
                      hle_scePsmfSpecifyStream);
    psp_hle_register("scePsmfGetNumberOfSpecificStreams",
                      hle_scePsmfGetNumberOfSpecificStreams);
    psp_hle_register("scePsmfGetCurrentStreamType",
                      hle_scePsmfGetCurrentStreamType);
    psp_hle_register("scePsmfGetPsmfVersion",
                      hle_scePsmfGetPsmfVersion);
    psp_hle_register("scePsmfGetNumberOfStreams",
                      hle_scePsmfGetNumberOfStreams);

    // SAS (Software Audio Synthesis) -- see psp_hle_sas.cpp
    // (registered by psp_hle_register_sas()).

    // Stdio
    psp_hle_register("sceKernelStdin", hle_sceKernelStdin);
    psp_hle_register("sceKernelStdout", hle_sceKernelStdout);
    psp_hle_register("sceKernelStderr", hle_sceKernelStderr);

    // Interrupt manager
    psp_hle_register("sceKernelRegisterSubIntrHandler",
                      hle_sceKernelRegisterSubIntrHandler);
    psp_hle_register("sceKernelReleaseSubIntrHandler",
                      hle_sceKernelReleaseSubIntrHandler);
    psp_hle_register("sceKernelEnableSubIntr",
                      hle_sceKernelEnableSubIntr);

    // Kernel_Library
    psp_hle_register("sceKernelCpuSuspendIntr",
                      hle_sceKernelCpuSuspendIntr);
    psp_hle_register("sceKernelCpuResumeIntr",
                      hle_sceKernelCpuResumeIntr);

    // LoadExec
    psp_hle_register("sceKernelExitGame",
                      hle_sceKernelExitGame);
    psp_hle_register("sceKernelRegisterExitCallback",
                      hle_sceKernelRegisterExitCallback);

    // Impose
    psp_hle_register("sceImposeGetLanguageMode",
                      hle_sceImposeGetLanguageMode);
    psp_hle_register("sceImposeSetLanguageMode",
                      hle_sceImposeSetLanguageMode);
    psp_hle_register("sceImposeSetUMDPopup",
                      hle_sceImposeSetUMDPopup);

    // Suspend
    psp_hle_register("sceKernelPowerTick",
                      hle_sceKernelPowerTick);

    // sceUtility
    psp_hle_register("sceUtilityLoadModule",
                      hle_sceUtilityLoadModule);
    psp_hle_register("sceUtilityUnloadModule",
                      hle_sceUtilityUnloadModule);
    psp_hle_register("sceUtilityGetSystemParamInt",
                      hle_sceUtilityGetSystemParamInt);
    psp_hle_register("sceUtilitySavedataInitStart",
                      hle_sceUtilitySavedataInitStart);
    psp_hle_register("sceUtilitySavedataGetStatus",
                      hle_sceUtilitySavedataGetStatus);
    psp_hle_register("sceUtilitySavedataShutdownStart",
                      hle_sceUtilitySavedataShutdownStart);
    psp_hle_register("sceUtilitySavedataUpdate",
                      hle_sceUtilitySavedataUpdate);
    psp_hle_register("sceUtilityMsgDialogInitStart",
                      hle_sceUtilityMsgDialogInitStart);
    psp_hle_register("sceUtilityMsgDialogGetStatus",
                      hle_sceUtilityMsgDialogGetStatus);
    psp_hle_register("sceUtilityMsgDialogShutdownStart",
                      hle_sceUtilityMsgDialogShutdownStart);
    psp_hle_register("sceUtilityMsgDialogUpdate",
                      hle_sceUtilityMsgDialogUpdate);
    psp_hle_register("sceUtilityOskInitStart",
                      hle_sceUtilityOskInitStart);
    psp_hle_register("sceUtilityOskGetStatus",
                      hle_sceUtilityOskGetStatus);
    psp_hle_register("sceUtilityOskShutdownStart",
                      hle_sceUtilityOskShutdownStart);
    psp_hle_register("sceUtilityOskUpdate",
                      hle_sceUtilityOskUpdate);
    psp_hle_register("sceUtilityHtmlViewerInitStart",
                      hle_sceUtilityHtmlViewerInitStart);
    psp_hle_register("sceUtilityHtmlViewerGetStatus",
                      hle_sceUtilityHtmlViewerGetStatus);
    psp_hle_register("sceUtilityHtmlViewerShutdownStart",
                      hle_sceUtilityHtmlViewerShutdownStart);
    psp_hle_register("sceUtilityHtmlViewerUpdate",
                      hle_sceUtilityHtmlViewerUpdate);

    // MsgPipe
    psp_hle_register("sceKernelCreateMsgPipe",
                      hle_sceKernelCreateMsgPipe);
    psp_hle_register("sceKernelDeleteMsgPipe",
                      hle_sceKernelDeleteMsgPipe);
    psp_hle_register("sceKernelSendMsgPipe",
                      hle_sceKernelSendMsgPipe);
    psp_hle_register("sceKernelTryReceiveMsgPipe",
                      hle_sceKernelTryReceiveMsgPipe);
}
