// Host test for Dissidia 012's VFPU atan2 (FUN_089A457C, EBOOT).
//
// The body below is a 1:1 transcription of the recompiler output for that
// function (vfpu_* call sequence and goto structure unchanged), except that
// the delay slots of the two branch-likely VFPU branches (bvtl @089A45CC,
// bvfl @089A45F0) are executed on the taken path, as hardware does. Before
// the emitter fix those slots were dropped; kDropLikelyDelaySlots=true
// reproduces that legacy output and must produce inf (negative control).
//
// Build/run: runtime/tests/run_vfpu_atan2.sh (no cmake, no SDL/GL needed).
#include "psp_vfpu.h"
#include "recomp.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// Link stub: the runtime's NaN-trap helper lives outside the VFPU core.
void psp_dump_func_ring(const char*) {}

static float getv(recomp_context& c, int reg) { return c.vfpu[vfpu_single_index(reg)]; }
static void setv(recomp_context& c, int reg, float v) { c.vfpu[vfpu_single_index(reg)] = v; }

// Returns the value the game stores (S001) after running the function.
static float vfpu_atan2(float y, float x, bool dropLikelyDS) {
    recomp_context cc;
    recomp_context* ctx = &cc;
    uint8_t* rdram = nullptr;
    std::memset(&cc, 0, sizeof(cc));
    vfpu_init_context(ctx);
    setv(cc, 0x00, y);   // lv.s S000 <- a0 (y)
    setv(cc, 0x20, x);   // lv.s S010 <- a1 (x)

    vfpu_vcmp(ctx, rdram, 0x00, 0x00, 11, 2);
    vfpu_vzero(ctx, rdram, 0x40, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 4))) goto L_4608;
    vfpu_vcmp(ctx, rdram, 0x20, 0x40, 6, 1);
    vfpu_vcst(ctx, rdram, 0x01, 9, 1);
    vfpu_vcmov(ctx, rdram, 0x01, 0x40, 0, 1);
    vfpu_vcmp(ctx, rdram, 0x00, 0x00, 8, 1);
    vfpu_vcst(ctx, rdram, 0x40, 8, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 0))) goto L_46CC;
    vfpu_vrcp(ctx, rdram, 0x60, 0x20, 1);
    vfpu_vmul(ctx, rdram, 0x60, 0x00, 0x60, 1);
    vfpu_vcmp(ctx, rdram, 0x60, 0x60, 9, 1);
    vfpu_vmov(ctx, rdram, 0x01, 0x60, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 0))) goto L_46CC;
    vfpu_vcmp(ctx, rdram, 0x60, 0x60, 11, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 0))) {            // bvtl
        if (!dropLikelyDS) vfpu_vsgn(ctx, rdram, 0x01, 0x00, 1);  // delay slot 089A45D0
        goto L_469C;
    }
    vfpu_vmul(ctx, rdram, 0x21, 0x60, 0x60, 1);
    vfpu_vcmp(ctx, rdram, 0x21, 0x21, 11, 1);
    vfpu_vone(ctx, rdram, 0x60, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 0))) goto L_4668;
    vfpu_set_prefix(ctx, 0, 0x000001E4u);
    vfpu_vcmp(ctx, rdram, 0x01, 0x60, 2, 1);
    vfpu_vadd(ctx, rdram, 0x60, 0x21, 0x60, 1);
    if (!(ctx->vfpu_ctrl[3] & (1 << 0))) {           // bvfl
        if (!dropLikelyDS) vfpu_vrcp(ctx, rdram, 0x60, 0x60, 1);  // delay slot 089A45F4
        goto L_4684;
    }
    vfpu_vrsq(ctx, rdram, 0x60, 0x60, 1);
    vfpu_vmul(ctx, rdram, 0x01, 0x01, 0x60, 1);
    vfpu_vasin(ctx, rdram, 0x01, 0x01, 1);
    goto L_469C;
L_4608:
    vfpu_vcmp(ctx, rdram, 0x00, 0x00, 9, 2);
    vfpu_vfim(ctx, 0x01, 0xBC00u);
    vfpu_vrsq(ctx, rdram, 0x01, 0x01, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 4))) goto L_46CC;
    vfpu_vcmp(ctx, rdram, 0x00, 0x00, 10, 2);
    if ((ctx->vfpu_ctrl[3] & (1 << 5))) goto L_4654;
    vfpu_vi2f(ctx, rdram, 0x00, 0x00, 0, 1);
    vfpu_set_prefix(ctx, 1, 0x000010E7u);
    vfpu_vadd(ctx, rdram, 0x00, 0x00, 0x00, 1);
    vfpu_vsgn(ctx, rdram, 0x00, 0x00, 1);
    vfpu_vcst(ctx, rdram, 0x40, 8, 1);
    vfpu_vone(ctx, rdram, 0x01, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 0))) goto L_46C0;
    vfpu_vzero(ctx, rdram, 0x60, 1);
    vfpu_vcmp(ctx, rdram, 0x20, 0x60, 6, 1);
    vfpu_vcmov(ctx, rdram, 0x01, 0x60, 0, 1);
    vfpu_vcst(ctx, rdram, 0x40, 9, 1);
    goto L_46C0;
L_4654:
    vfpu_vsgn(ctx, rdram, 0x00, 0x00, 2);
    vfpu_set_prefix(ctx, 0, 0x000010E6u);
    vfpu_vsub(ctx, rdram, 0x01, 0x01, 0x20, 1);
    vfpu_vcst(ctx, rdram, 0x40, 7, 1);
    goto L_46C0;
L_4668:
    vfpu_vsgn(ctx, rdram, 0x00, 0x00, 1);
    vfpu_vcst(ctx, rdram, 0x40, 8, 1);
    vfpu_vone(ctx, rdram, 0x01, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 0))) goto L_46C0;
    vfpu_vsgn(ctx, rdram, 0x21, 0x20, 1);
    vfpu_vsub(ctx, rdram, 0x01, 0x01, 0x21, 1);
    goto L_46C0;
L_4684:
    vfpu_vmul(ctx, rdram, 0x60, 0x21, 0x60, 1);
    vfpu_set_prefix(ctx, 2, 0x00000E01u);
    vfpu_vocp(ctx, rdram, 0x60, 0x60, 1);
    vfpu_vsqrt(ctx, rdram, 0x60, 0x60, 1);
    vfpu_vasin(ctx, rdram, 0x60, 0x60, 1);
    vfpu_vocp(ctx, rdram, 0x01, 0x60, 1);
L_469C:
    vfpu_set_prefix(ctx, 2, 0x00000C04u);
    vfpu_set_prefix(ctx, 0, 0x000200E4u);
    vfpu_vsgn(ctx, rdram, 0x00, 0x00, 2);
    vfpu_vcmp(ctx, rdram, 0x20, 0x20, 8, 1);
    if ((ctx->vfpu_ctrl[3] & (1 << 0))) goto L_46C0;
    vfpu_set_prefix(ctx, 0, 0x000010E6u);
    vfpu_set_prefix(ctx, 1, 0x000001E4u);
    vfpu_vsub(ctx, rdram, 0x01, 0x01, 0x01, 1);
L_46C0:
    vfpu_set_prefix(ctx, 0, 0x000001E4u);
    vfpu_vmul(ctx, rdram, 0x01, 0x01, 0x00, 1);
    vfpu_vmul(ctx, rdram, 0x01, 0x01, 0x40, 1);
L_46CC:
    return getv(cc, 0x01);
}

int main() {
    static const float cases[][2] = {
        {1, 0}, {-1, 0}, {0, 0}, {1, 1}, {0, 1}, {3, -4},
        {-3, -4}, {0, -1}, {-1, -1}, {2, 0.5f}, {-2, 0.5f}, {0.5f, 2}, {5, 0},
        {1e-3f, 1}, {1000, 1e-3f},
    };
    int fail = 0;
    for (auto& c : cases) {
        float y = c[0], x = c[1];
        float got = vfpu_atan2(y, x, false);
        float want = std::atan2(y, x);
        // (0,0): hardware/PSP convention is whatever the game code yields, but it must be finite.
        bool ok = std::isfinite(got) && (y == 0 && x == 0 ? true : std::fabs(got - want) < 1e-3f);
        std::printf("atan2(y=%g, x=%g) = %+.6f  (atan2f %+.6f)  %s\n", y, x, got, want, ok ? "ok" : "FAIL");
        if (!ok) fail++;
    }
    // Negative control: legacy emitter output (likely-branch delay slots dropped) yields inf.
    float legacy = vfpu_atan2(1, 0, true);
    std::printf("legacy (delay slots dropped) atan2(1,0) = %g  (expected non-finite)\n", legacy);
    if (std::isfinite(legacy)) { std::printf("FAIL: negative control did not reproduce the bug\n"); fail++; }
    std::printf("%s\n", fail ? "FAILED" : "ALL PASSED");
    return fail ? 1 : 0;
}
