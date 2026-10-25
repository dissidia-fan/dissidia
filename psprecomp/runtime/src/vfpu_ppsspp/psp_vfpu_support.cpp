// Definitions the vendored PPSSPP VFPU code expects from the rest of PPSSPP (Core/MIPS/MIPS.cpp etc.).
// Constants and the register permutation are copied from there.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>

#include "Core/Config.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Common/StringUtils.h"

VfpuConfig g_Config;

namespace Memory {
thread_local unsigned char *g_rdram = nullptr;
}

thread_local MIPSState *currentMIPS = nullptr;

u8 voffset[128];
u8 fromvoffset[128];

const float vfpuPrefixConstants[8] = { 0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f };

#ifndef M_E
#define M_E 2.71828182845904523536
#endif
#ifndef M_LOG2E
#define M_LOG2E 1.44269504088896340736
#endif
#ifndef M_LOG10E
#define M_LOG10E 0.434294481903251827651
#endif
#ifndef M_LN2
#define M_LN2 0.693147180559945309417
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const float cst_constants[32] = {
	0,
	std::numeric_limits<float>::max(),  // all these are verified on real PSP
	sqrtf(2.0f),
	sqrtf(0.5f),
	2.0f / sqrtf((float)M_PI),
	2.0f / (float)M_PI,
	1.0f / (float)M_PI,
	(float)M_PI / 4,
	(float)M_PI / 2,
	(float)M_PI,
	(float)M_E,
	(float)M_LOG2E,
	(float)M_LOG10E,
	(float)M_LN2,
	(float)M_LN10,
	2 * (float)M_PI,
	(float)M_PI / 6,
	log10f(2.0f),
	logf(10.0f) / logf(2.0f),
	sqrtf(3.0f) / 2.0f,
};

namespace {
struct VoffsetInit {
	VoffsetInit() {
		int i = 0;
		for (int m = 0; m < 8; m++)
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++)
					voffset[m * 4 + x * 32 + y] = i++;
		for (int j = 0; j < 128; j++)
			fromvoffset[voffset[j]] = j;
		// Dynamic init of these tables must precede the first VFPU op; the runtime's first op runs
		// long after static initialisation.
	}
} s_voffsetInit;
}  // namespace

std::string StringFromFormat(const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}
