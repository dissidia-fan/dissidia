// Adapter that exposes PPSSPP's VFPU interpreter (Core/MIPS/InterpreterVFPU.cpp, compiled unmodified
#include <cstdint>
#include <cmath>
#include <limits>
// from a local PPSSPP checkout) to the VFPU semantics host test through a PPSSPP-free C interface.
// Kept in its own translation unit because PPSSPP's headers and psp_vfpu.h/recomp.h define clashing names.
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/InterpreterVFPU.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/Config.h"
#include "Core/MemMap.h"
#include "ref_adapter.h"
#include <cstdarg>
#include <cstdio>
#include <string>

ShimConfig g_Config;
unsigned char Memory::g_mem[0x10000];
u8 voffset[128];
u8 fromvoffset[128];
MIPSState *currentMIPS = nullptr;
const float vfpuPrefixConstants[8] = { 0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f };

#ifndef M_E
#define M_E 2.71828182845904523536
#endif
const float cst_constants[32] = {
	0, std::numeric_limits<float>::max(), sqrtf(2.0f), sqrtf(0.5f), 2.0f / sqrtf((float)M_PI), 2.0f / (float)M_PI,
	1.0f / (float)M_PI, (float)M_PI / 4, (float)M_PI / 2, (float)M_PI, (float)M_E, (float)M_LOG2E,
	(float)M_LOG10E, (float)M_LN2, (float)M_LN10, 2 * (float)M_PI, (float)M_PI / 6, log10f(2.0f),
	logf(10.0f) / logf(2.0f), sqrtf(3.0f) / 2.0f,
};

std::string StringFromFormat(const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

MIPSState::MIPSState() {}
MIPSState::~MIPSState() {}

static MIPSState g_state;

void ref_init() {
	int i = 0;
	for (int m = 0; m < 8; m++)
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++)
				voffset[m * 4 + x * 32 + y] = i++;
	for (int j = 0; j < 128; j++)
		fromvoffset[voffset[j]] = j;
	currentMIPS = &g_state;
}

void ref_set_accurate_dot(int on) { g_Config.bAccurateVdot = on != 0; }

unsigned char *ref_mem() { return Memory::g_mem; }

struct Entry { const char *name; void (*fn)(MIPSState *, MIPSOpcode); };
#define E(n) { #n, MIPSInt::n }
static const Entry kTable[] = {
	E(Int_SV), E(Int_SVQ), E(Int_Mftv), E(Int_VecDo3), E(Int_Vcst), E(Int_VMatrixInit), E(Int_VVectorInit),
	E(Int_Vmmul), E(Int_Vmscl), E(Int_Vmmov), E(Int_VV2Op), E(Int_Vrot), E(Int_VDot), E(Int_VHdp),
	E(Int_Vavg), E(Int_Vfad), E(Int_Vocp), E(Int_Vsocp), E(Int_Vsgn), E(Int_Vtfm), E(Int_Viim), E(Int_VScl),
	E(Int_Vidt), E(Int_Vcmp), E(Int_Vminmax), E(Int_Vscmp), E(Int_Vcrs), E(Int_Vdet), E(Int_Vcmov),
	E(Int_CrossQuat), E(Int_VPFX), E(Int_Vflush), E(Int_Vbfy), E(Int_Vsrt1), E(Int_Vsrt2), E(Int_Vsrt3),
	E(Int_Vsrt4), E(Int_Vf2i), E(Int_Vi2f), E(Int_Vi2x), E(Int_Vx2i), E(Int_Vrnds), E(Int_VrndX),
	E(Int_ColorConv), E(Int_Vh2f), E(Int_Vf2h), E(Int_Vsge), E(Int_Vslt), E(Int_Vmfvc), E(Int_Vmtvc),
	E(Int_Vlgb), E(Int_Vwbn), E(Int_Vsbn), E(Int_Vsbz),
};
#undef E

int ref_fn_count() { return (int)(sizeof(kTable) / sizeof(kTable[0])); }
const char *ref_fn_name(int i) { return kTable[i].name; }
int ref_fn_find(const char *name) {
	for (int i = 0; i < ref_fn_count(); i++)
		if (std::string(kTable[i].name) == name) return i;
	return -1;
}

void ref_run(int fn, uint32_t op, RefState *st) {
	MIPSState *m = &g_state;
	std::memcpy(m->v, st->v, sizeof(st->v));
	std::memcpy(m->vfpuCtrl, st->ctrl, sizeof(st->ctrl));
	std::memcpy(m->r, st->r, sizeof(st->r));
	m->pc = 0x08800000;
	kTable[fn].fn(m, MIPSOpcode(op));
	std::memcpy(st->v, m->v, sizeof(st->v));
	std::memcpy(st->ctrl, m->vfpuCtrl, sizeof(st->ctrl));
	std::memcpy(st->r, m->r, sizeof(st->r));
}
