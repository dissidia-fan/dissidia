#include "hle/psp_hle.h"
#include "psp_runtime.h"
#include "psp_scheduler.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdlib>

// ---- Right stick (host pad, or debug-socket `A2` injection) ----
static void read_right_raw(int* rx, int* ry) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint32_t v = (now_ms < g_injected_ranalog_deadline_ms.load(std::memory_order_relaxed))
        ? g_injected_ranalog.load(std::memory_order_relaxed)
        : g_host_ranalog.load(std::memory_order_relaxed);
    *rx = static_cast<int>(v & 0xFF) - 128;
    *ry = static_cast<int>((v >> 8) & 0xFF) - 128;
}

// Radial dead zone (PSPRECOMP_RSTICK_DEADZONE, default 0.20 of full travel),
// rescaled so the output starts at 0 at the edge of the dead zone.
bool psp_ctrl_right_stick(float* x, float* y) {
    static const float dz = [] {
        const char* e = std::getenv("PSPRECOMP_RSTICK_DEADZONE");
        float d = e ? static_cast<float>(std::atof(e)) : 0.20f;
        return d < 0.f ? 0.f : d > 0.9f ? 0.9f : d;
    }();
    int rx, ry; read_right_raw(&rx, &ry);
    float fx = rx / 127.f, fy = -ry / 127.f;   // y: up positive
    float m = std::sqrt(fx * fx + fy * fy);
    if (m <= dz) { *x = *y = 0.f; return false; }
    float k = (m > 1.f ? 1.f : m);
    k = (k - dz) / (1.f - dz) / m;
    *x = fx * k; *y = fy * k;
    return true;
}

// Right stick -> D-pad bits: ON by default (a pad is present or injected);
// PSPRECOMP_RSTICK_DPAD=0 turns it off. Per-axis threshold 55% with a release
// at 40% (hysteresis), independent of the camera dead zone.
static uint32_t right_stick_dpad_bits() {
    static const bool on = [] {
        const char* e = std::getenv("PSPRECOMP_RSTICK_DPAD");
        return !(e && e[0] == '0');
    }();
    static uint32_t held = 0;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!on || now_ms < g_rstick_consumed_until_ms.load(std::memory_order_relaxed)) { held = 0; return 0; }
    int rx, ry; read_right_raw(&rx, &ry);
    auto upd = [&](int v, uint32_t lo, uint32_t hi) {
        int thr_on = 70, thr_off = 51;   // of 127
        uint32_t cur = held & (lo | hi);
        int th = cur ? thr_off : thr_on;
        held &= ~(lo | hi);
        if (v < -th) held |= lo; else if (v > th) held |= hi;
    };
    upd(rx, 0x0080, 0x0020);   // LEFT / RIGHT
    upd(ry, 0x0010, 0x0040);   // UP / DOWN (SDL y down == PSP y down)
    return held;
}

// ---- SceCtrlData layout (PSP SDK) ----
// offset 0: uint32_t timestamp
// offset 4: uint32_t buttons
// offset 8: uint8_t  analog_x  (128 = center)
// offset 9: uint8_t  analog_y  (128 = center)
// offset 10-15: reserved
// Total: 16 bytes per sample

static void write_ctrl_data(
    uint8_t* rdram, uint32_t ptr
) {
    // Timestamp: microseconds since boot
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<
        std::chrono::microseconds>(
            now.time_since_epoch()).count();
    psp_mem_write<uint32_t>(rdram, ptr,
        static_cast<uint32_t>(us & 0xFFFFFFFF));

    // Buttons: host keyboard mask OR debug-socket injected mask
    // (injected mask only while its deadline has not expired).
    uint32_t buttons =
        g_host_buttons.load(std::memory_order_relaxed);
    auto now_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    if (now_ms < g_injected_buttons_deadline_ms.load(
            std::memory_order_relaxed)) {
        buttons |= g_injected_buttons.load(
            std::memory_order_relaxed);
    }
    buttons |= right_stick_dpad_bits();
    psp_mem_write<uint32_t>(rdram, ptr + 4, buttons);

    // Analog stick: host pad/keyboard stick (128 = centre)
    uint32_t analog = g_host_analog.load(std::memory_order_relaxed);
    if (now_ms < g_injected_analog_deadline_ms.load(
            std::memory_order_relaxed)) {
        analog = g_injected_analog.load(std::memory_order_relaxed);
    }
    psp_mem_write<uint8_t>(rdram, ptr + 8,
        static_cast<uint8_t>(analog & 0xFF));
    psp_mem_write<uint8_t>(rdram, ptr + 9,
        static_cast<uint8_t>((analog >> 8) & 0xFF));

    // Reserved bytes: zero
    for (uint32_t i = 10; i < 16; i++) {
        psp_mem_write<uint8_t>(rdram, ptr + i, 0);
    }
}

// ---- HLE Functions ----

static void hle_sceCtrlPeekBufferPositive(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t data_ptr = static_cast<uint32_t>(ctx->r[4]);
    int32_t count = ctx->r[5];

    if (std::getenv("PSPRECOMP_CTRL_LOG")) {   // distinct callers of the pad poll (input-hunt aid)
        static uint32_t seen[64]; static int ns = 0;
        uint32_t ra = static_cast<uint32_t>(ctx->r[31]);
        bool known = false;
        for (int i = 0; i < ns; i++) known |= seen[i] == ra;
        if (!known && ns < 64) {
            seen[ns++] = ra;
            std::fprintf(stderr, "[CTRL] sceCtrlPeek/Read buf=0x%08X count=%d ra=0x%08X\n", data_ptr, count, ra);
        }
    }
    if (data_ptr != 0 && count > 0) {
        // Write count samples (16 bytes each)
        for (int32_t i = 0; i < count; i++) {
            write_ctrl_data(rdram, data_ptr + i * 16);
        }
    }

    ctx->r[2] = count;
}

static void hle_sceCtrlReadBufferPositive(
    uint8_t* rdram, recomp_context* ctx
) {
    sched_yield_point();
    hle_sceCtrlPeekBufferPositive(rdram, ctx);
}

static void hle_sceCtrlSetSamplingCycle(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceCtrlSetSamplingMode(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceCtrlSetIdleCancelThreshold(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_ctrl() {
    psp_hle_register("sceCtrlPeekBufferPositive",
                      hle_sceCtrlPeekBufferPositive);
    psp_hle_register("sceCtrlReadBufferPositive",
                      hle_sceCtrlReadBufferPositive);
    psp_hle_register("sceCtrlSetSamplingCycle",
                      hle_sceCtrlSetSamplingCycle);
    psp_hle_register("sceCtrlSetSamplingMode",
                      hle_sceCtrlSetSamplingMode);
    psp_hle_register("sceCtrlSetIdleCancelThreshold",
                      hle_sceCtrlSetIdleCancelThreshold);
}
