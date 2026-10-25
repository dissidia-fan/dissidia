#include "hle/psp_hle.h"
#include "recomp.h"

// ---- HLE Functions ----
// Patapon only imports one scePower NID: 0xEBD177D6.
// sceKernelPowerTick is under sceSuspendForUser, registered
// by psp_hle_register_utility().

// NID 0xEBD177D6 is missing from data/niddb, so the generated import table
// (issue #40) carries the walker's canonical fallback name "NID_0xEBD177D6"
// — handlers for unresolved NIDs register under exactly that name.
// Identification: PPSSPP Core/HLE/scePower.cpp maps 0xEBD177D6 to
// scePowerSetClockFrequency (exported as "scePower_EBD177D6", the 3.71+
// alias). Returning SCE_OK without touching clocks is the correct no-op.
static void hle_NID_0xEBD177D6(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// scePowerGetCpuClockFrequencyInt / scePowerGetBusClockFrequencyInt:
// report the PSP's stock clocks (222 MHz CPU, 111 MHz bus). A 0 here would
// poison any game-side timing arithmetic derived from the clock.
static void hle_scePowerGetCpuClockFrequencyInt(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = 222;
}
static void hle_scePowerGetBusClockFrequencyInt(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = 111;
}
// scePowerRegisterCallback / Unregister: no power events are ever raised
// (no battery, no suspend), so registering is a successful no-op.
static void hle_scePowerRegisterCallback(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = SCE_OK;
}
static void hle_scePowerUnregisterCallback(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = SCE_OK;
}
// scePowerCheckWlanCoexistenceClock: 0 = no WLAN clock restriction.
static void hle_scePowerCheckWlanCoexistenceClock(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = 0;
}

// ---- Registration ----

void psp_hle_register_power() {
    psp_hle_register("NID_0xEBD177D6",
                      hle_NID_0xEBD177D6);
    psp_hle_register("NID_0x469989AD", hle_NID_0xEBD177D6);  // scePowerSetClockFrequency (6.30 alias)
    psp_hle_register("scePowerGetCpuClockFrequencyInt", hle_scePowerGetCpuClockFrequencyInt);
    psp_hle_register("scePowerGetBusClockFrequencyInt", hle_scePowerGetBusClockFrequencyInt);
    psp_hle_register("scePowerRegisterCallback", hle_scePowerRegisterCallback);
    psp_hle_register("scePowerUnregisterCallback", hle_scePowerUnregisterCallback);
    psp_hle_register("scePowerCheckWlanCoexistenceClock", hle_scePowerCheckWlanCoexistenceClock);
}
