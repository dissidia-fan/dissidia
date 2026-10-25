// VFPU op entry points called by the recompiled code.
//
// The semantics are PPSSPP's: every op builds the PSP opcode word from the decoded fields the
// emitter passes and runs PPSSPP's interpreter function on a view of the live recomp_context
// (vendored in vfpu_ppsspp/, see the README there). That brings along the hardware behaviour the
// interpreter encodes: prefix lifetime, last-lane prefix quirks, min/max ordering, the table-driven
// sin/cos/rcp/rsq/sqrt/exp2/log2/asin, the exact dot product, NaN/denormal fixups, vrnd, etc.
//
// tests/run_vfpu_semantics.sh checks these entry points against the interpreter, bit for bit.
#define PSP_VFPU_CTRL_FROM_PPSSPP 1  // MIPS.h below declares VFPU_CTRL_*
#include "psp_vfpu.h"
#include "recomp.h"

#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/InterpreterVFPU.h"
#include "Core/MemMap.h"

namespace {

inline MIPSState& bind(recomp_context* ctx, uint8_t* rdram) {
    static thread_local MIPSState st;
    st.r = reinterpret_cast<u32*>(ctx->r);
    st.v = ctx->vfpu;
    st.vfpuCtrl = ctx->vfpu_ctrl;
    Memory::g_rdram = rdram;
    currentMIPS = &st;
    return st;
}

inline uint32_t sz_bits(int n) {
    return (uint32_t)((((n - 1) & 1) << 7) | ((((n - 1) >> 1) & 1) << 15));
}

constexpr uint32_t kVfpu0 = 24u << 26;   // vadd/vsub/vdiv
constexpr uint32_t kVfpu1 = 25u << 26;   // vmul/vdot/vscl/vhdp/vcrs/vdet
constexpr uint32_t kVfpu2 = 27u << 26;   // vcmp/vmin/vmax/vscmp/vsge/vslt
constexpr uint32_t kVfpu4 = 52u << 26;   // unary, conversions, constants
constexpr uint32_t kVfpu5 = 60u << 26;   // matrix ops, vrot
constexpr uint32_t kVfpu6 = 55u << 26;   // vpfx*, viim, vfim

inline uint32_t vec3(uint32_t base, int n, int vd, int vs, int vt) {
    return base | sz_bits(n) | ((uint32_t)vt << 16) | ((uint32_t)vs << 8) | (uint32_t)vd;
}
inline uint32_t vec2(uint32_t base, int n, int vd, int vs) {
    return base | sz_bits(n) | ((uint32_t)vs << 8) | (uint32_t)vd;
}

}  // namespace

#define BIN_OP(NAME, FN, BASE)                                                                   \
    void vfpu_##NAME(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t vt,    \
                     uint8_t size) {                                                             \
        MIPSInt::FN(&bind(ctx, rdram), MIPSOpcode(vec3(BASE, size, vd, vs, vt)));                \
    }
#define UN_OP(NAME, FN, BASE)                                                                    \
    void vfpu_##NAME(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t size) {\
        MIPSInt::FN(&bind(ctx, rdram), MIPSOpcode(vec2(BASE, size, vd, vs)));                    \
    }
#define IMM_OP(NAME, FN, BASE)                                                                   \
    void vfpu_##NAME(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t imm5,  \
                     uint8_t size) {                                                             \
        MIPSInt::FN(&bind(ctx, rdram), MIPSOpcode(vec2(BASE, size, vd, vs) | ((uint32_t)imm5 << 16))); \
    }
#define INIT_OP(NAME, FN, BASE)                                                                  \
    void vfpu_##NAME(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t size) {            \
        MIPSInt::FN(&bind(ctx, rdram), MIPSOpcode(vec2(BASE, size, vd, 0)));                     \
    }

// ---- binary
BIN_OP(vadd, Int_VecDo3, kVfpu0 | (0u << 23))
BIN_OP(vsub, Int_VecDo3, kVfpu0 | (1u << 23))
BIN_OP(vdiv, Int_VecDo3, kVfpu0 | (7u << 23))
BIN_OP(vmul, Int_VecDo3, kVfpu1 | (0u << 23))
BIN_OP(vdot, Int_VDot, kVfpu1 | (1u << 23))
BIN_OP(vscl, Int_VScl, kVfpu1 | (2u << 23))
BIN_OP(vhdp, Int_VHdp, kVfpu1 | (4u << 23))
BIN_OP(vcrs, Int_Vcrs, kVfpu1 | (5u << 23))
BIN_OP(vdet, Int_Vdet, kVfpu1 | (6u << 23))
BIN_OP(vmin, Int_Vminmax, kVfpu2 | (2u << 23))
BIN_OP(vmax, Int_Vminmax, kVfpu2 | (3u << 23))
BIN_OP(vscmp, Int_Vscmp, kVfpu2 | (5u << 23))
BIN_OP(vsge, Int_Vsge, kVfpu2 | (6u << 23))
BIN_OP(vslt, Int_Vslt, kVfpu2 | (7u << 23))

void vfpu_vcmp(recomp_context* ctx, uint8_t* rdram, uint8_t vs, uint8_t vt, uint8_t cond,
               uint8_t size) {
    uint32_t op = (kVfpu2 | sz_bits(size) | ((uint32_t)vt << 16) | ((uint32_t)vs << 8)) & ~0x7Fu;
    MIPSInt::Int_Vcmp(&bind(ctx, rdram), MIPSOpcode(op | (cond & 0xF)));
}

// cc = imm3 | (tf << 3), as the emitter packs it.
void vfpu_vcmov(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t cc,
                uint8_t size) {
    uint32_t op = (52u << 26) | (21u << 21) | sz_bits(size) | ((uint32_t)(cc & 7) << 16) |
                  ((uint32_t)((cc >> 3) & 1) << 19) | ((uint32_t)vs << 8) | vd;
    MIPSInt::Int_Vcmov(&bind(ctx, rdram), MIPSOpcode(op));
}

// ---- unary
#define VV2(NAME, SUB) UN_OP(NAME, Int_VV2Op, kVfpu4 | ((uint32_t)(SUB) << 16))
VV2(vmov, 0)
VV2(vabs, 1)
VV2(vneg, 2)
VV2(vsat0, 4)
VV2(vsat1, 5)
VV2(vrcp, 16)
VV2(vrsq, 17)
VV2(vsin, 18)
VV2(vcos, 19)
VV2(vexp2, 20)
VV2(vlog2, 21)
VV2(vsqrt, 22)
VV2(vasin, 23)
VV2(vnrcp, 24)
VV2(vnsin, 26)
VV2(vrexp2, 28)

#define VV3(NAME, FN, SUB) UN_OP(NAME, FN, kVfpu4 | (2u << 21) | ((uint32_t)(SUB) << 16))
VV3(vsrt1, Int_Vsrt1, 0)
VV3(vsrt2, Int_Vsrt2, 1)
VV3(vbfy1, Int_Vbfy, 2)
VV3(vbfy2, Int_Vbfy, 3)
VV3(vocp, Int_Vocp, 4)
VV3(vsocp, Int_Vsocp, 5)
VV3(vfad, Int_Vfad, 6)
VV3(vavg, Int_Vavg, 7)
VV3(vsrt3, Int_Vsrt3, 8)
VV3(vsrt4, Int_Vsrt4, 9)
VV3(vsgn, Int_Vsgn, 10)

// Format conversions: sub-op in bits 16-20 under major group 1.
#define VX(NAME, FN, SUB) UN_OP(NAME, FN, kVfpu4 | (1u << 21) | ((uint32_t)(SUB) << 16))
VX(vf2h, Int_Vf2h, 18)
VX(vh2f, Int_Vh2f, 19)
VX(vuc2i, Int_Vx2i, 24)
VX(vc2i, Int_Vx2i, 25)
VX(vus2i, Int_Vx2i, 26)
VX(vs2i, Int_Vx2i, 27)
VX(vi2uc, Int_Vi2x, 28)
VX(vi2c, Int_Vi2x, 29)
VX(vi2us, Int_Vi2x, 30)
VX(vi2s, Int_Vi2x, 31)

IMM_OP(vf2in, Int_Vf2i, kVfpu4 | (16u << 21))
IMM_OP(vf2iz, Int_Vf2i, kVfpu4 | (17u << 21))
IMM_OP(vf2iu, Int_Vf2i, kVfpu4 | (18u << 21))
IMM_OP(vf2id, Int_Vf2i, kVfpu4 | (19u << 21))
IMM_OP(vi2f, Int_Vi2f, kVfpu4 | (20u << 21))

void vfpu_vcst(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t imm5, uint8_t size) {
    MIPSInt::Int_Vcst(&bind(ctx, rdram),
                      MIPSOpcode(kVfpu4 | (3u << 21) | ((uint32_t)imm5 << 16) | sz_bits(size) | vd));
}

void vfpu_vwbn(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t imm8,
               uint8_t size) {
    MIPSInt::Int_Vwbn(&bind(ctx, rdram),
                      MIPSOpcode(vec2(kVfpu4 | (0x18u << 21), size, vd, vs) | ((uint32_t)imm8 << 16)));
}

void vfpu_vrot(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint16_t imm5,
               uint8_t size) {
    MIPSInt::Int_Vrot(&bind(ctx, rdram),
                      MIPSOpcode(vec2(kVfpu5 | (3u << 21), size, vd, vs) | ((uint32_t)(imm5 & 0x1F) << 16)));
}

// ---- init ops
INIT_OP(vidt, Int_Vidt, kVfpu4 | (3u << 16))
INIT_OP(vzero, Int_VVectorInit, kVfpu4 | (6u << 16))
INIT_OP(vone, Int_VVectorInit, kVfpu4 | (7u << 16))
INIT_OP(vmidt, Int_VMatrixInit, kVfpu5 | (7u << 23) | (3u << 16))
INIT_OP(vmzero, Int_VMatrixInit, kVfpu5 | (7u << 23) | (6u << 16))
INIT_OP(vmone, Int_VMatrixInit, kVfpu5 | (7u << 23) | (7u << 16))

UN_OP(vmmov, Int_Vmmov, kVfpu5 | (3u << 23))
BIN_OP(vmmul, Int_Vmmul, kVfpu5 | (0u << 23))
BIN_OP(vmscl, Int_Vmscl, kVfpu5 | (4u << 23))

// vtfmN: (ins = N-1) N x N matrix times an N-vector; vhtfmN: (ins = N) matrix, vector gets an implicit 1.
#define TFM(NAME, INS, N)                                                                        \
    void vfpu_##NAME(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t vt) {  \
        MIPSInt::Int_Vtfm(&bind(ctx, rdram), MIPSOpcode(vec3(kVfpu5 | ((uint32_t)(INS) << 23), N, vd, vs, vt))); \
    }
TFM(vtfm2, 1, 2)
TFM(vtfm3, 2, 3)
TFM(vtfm4, 3, 4)
TFM(vhtfm2, 2, 2)
TFM(vhtfm3, 3, 3)

// There is no 4-wide homogeneous form on the PSP (the encoding does not exist); the emitter only
// reaches this for undecodable words, so it behaves as vtfm4.
void vfpu_vhtfm4(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t vt) {
    vfpu_vtfm4(ctx, rdram, vd, vs, vt);
}

void vfpu_vcrsp(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t vt) {
    MIPSInt::Int_CrossQuat(&bind(ctx, rdram), MIPSOpcode(vec3(kVfpu5 | (5u << 23), 3, vd, vs, vt)));
}
void vfpu_vqmul(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t vs, uint8_t vt) {
    MIPSInt::Int_CrossQuat(&bind(ctx, rdram), MIPSOpcode(vec3(kVfpu5 | (5u << 23), 4, vd, vs, vt)));
}

// ---- random
void vfpu_vrnds(recomp_context* ctx, uint8_t* rdram, uint8_t vs) {
    MIPSInt::Int_Vrnds(&bind(ctx, rdram), MIPSOpcode(kVfpu4 | (1u << 21) | ((uint32_t)vs << 8)));
}
#define RND(NAME, SUB)                                                                           \
    void vfpu_##NAME(recomp_context* ctx, uint8_t* rdram, uint8_t vd, uint8_t size) {            \
        MIPSInt::Int_VrndX(&bind(ctx, rdram),                                                    \
                           MIPSOpcode(kVfpu4 | (1u << 21) | ((uint32_t)(SUB) << 16) | sz_bits(size) | vd)); \
    }
RND(vrndi, 1)
RND(vrndf1, 2)
RND(vrndf2, 3)

// ---- immediates, prefixes, control moves
void vfpu_viim(recomp_context* ctx, uint8_t vt, uint16_t imm) {
    uint8_t dummy;
    MIPSInt::Int_Viim(&bind(ctx, &dummy),
                      MIPSOpcode(kVfpu6 | (6u << 23) | ((uint32_t)(vt & 0x7F) << 16) | imm));
}
void vfpu_vfim(recomp_context* ctx, uint8_t vt, uint16_t imm) {
    uint8_t dummy;
    MIPSInt::Int_Viim(&bind(ctx, &dummy),
                      MIPSOpcode(kVfpu6 | (7u << 23) | ((uint32_t)(vt & 0x7F) << 16) | imm));
}

void vfpu_mfv(recomp_context* ctx, int rt_idx, uint8_t vd) {
    uint8_t dummy;
    MIPSInt::Int_Mftv(&bind(ctx, &dummy),
                      MIPSOpcode((18u << 26) | (3u << 21) | ((uint32_t)rt_idx << 16) | (vd & 0x7F)));
}
void vfpu_mtv(recomp_context* ctx, int rt_idx, uint8_t vd) {
    uint8_t dummy;
    MIPSInt::Int_Mftv(&bind(ctx, &dummy),
                      MIPSOpcode((18u << 26) | (7u << 21) | ((uint32_t)rt_idx << 16) | (vd & 0x7F)));
}
void vfpu_mfvc(recomp_context* ctx, int rt_idx, int imm) {
    uint8_t dummy;
    MIPSInt::Int_Mftv(&bind(ctx, &dummy),
                      MIPSOpcode((18u << 26) | (3u << 21) | ((uint32_t)rt_idx << 16) | (uint32_t)(128 + (imm & 15))));
}
void vfpu_mtvc(recomp_context* ctx, int rt_idx, int imm) {
    uint8_t dummy;
    MIPSInt::Int_Mftv(&bind(ctx, &dummy),
                      MIPSOpcode((18u << 26) | (7u << 21) | ((uint32_t)rt_idx << 16) | (uint32_t)(128 + (imm & 15))));
}

// ---- memory
// The rs register holds the base address; offset is the sign-extended immediate.
#define MEM_OP(NAME, FN, MAJOR, LOW, SHIFT_TO_VT)                                                \
    void vfpu_##NAME(recomp_context* ctx, uint8_t* rdram, uint8_t vt, uint8_t rs, int16_t offset) { \
        uint32_t op = ((uint32_t)(MAJOR) << 26) | ((uint32_t)rs << 21) | ((uint32_t)(vt & 0x1F) << 16) | \
                      ((uint32_t)(uint16_t)offset & 0xFFFCu) | (LOW) | SHIFT_TO_VT;              \
        MIPSInt::FN(&bind(ctx, rdram), MIPSOpcode(op));                                          \
    }
MEM_OP(lv_s, Int_SV, 50, 0, ((uint32_t)(vt >> 5) & 3))
MEM_OP(sv_s, Int_SV, 58, 0, ((uint32_t)(vt >> 5) & 3))
MEM_OP(lv_q, Int_SVQ, 54, 0, ((uint32_t)(vt >> 5) & 1))
MEM_OP(sv_q, Int_SVQ, 62, 0, ((uint32_t)(vt >> 5) & 1))
MEM_OP(lvl_q, Int_SVQ, 53, 0, ((uint32_t)(vt >> 5) & 1))
MEM_OP(lvr_q, Int_SVQ, 53, 2, ((uint32_t)(vt >> 5) & 1))
MEM_OP(svl_q, Int_SVQ, 61, 0, ((uint32_t)(vt >> 5) & 1))
MEM_OP(svr_q, Int_SVQ, 61, 2, ((uint32_t)(vt >> 5) & 1))
